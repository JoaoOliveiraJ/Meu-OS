#include <stdint.h>
#include "filesystems/ntfs/ntfs.h"
#include "hal/disk.h"
#include "mm/heap.h"

// Serial = canal de log (kputc -> VGA texto + COM1).
extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern void* memcpy(void* dst, const void* s, unsigned long n);
extern void* memmove(void* dst, const void* s, unsigned long n);
extern void* memset(void* dst, int v, unsigned long n);

// ============================================================================
//  Tipos de atributo NTFS (no header de cada atributo do registro MFT).
// ============================================================================
#define ATTR_STANDARD_INFORMATION 0x10
#define ATTR_ATTRIBUTE_LIST       0x20
#define ATTR_FILE_NAME            0x30
#define ATTR_OBJECT_ID            0x40
#define ATTR_DATA                 0x80
#define ATTR_INDEX_ROOT           0x90
#define ATTR_INDEX_ALLOCATION     0xA0
#define ATTR_BITMAP               0xB0
#define ATTR_END                  0xFFFFFFFF

// Flags do registro MFT (offset 0x16).
#define MFT_FLAG_IN_USE     0x0001
#define MFT_FLAG_DIRECTORY  0x0002

// Flags do $FILE_NAME file attributes.
#define FILE_ATTR_DIRECTORY 0x10000000

// Flags de entrada de indice (no $INDEX_ROOT/$INDEX_ALLOCATION).
#define INDEX_ENTRY_NODE    0x01   // a entrada aponta para um sub-no (VCN)
#define INDEX_ENTRY_END     0x02   // ultima entrada do no

// ============================================================================
//  Helpers de leitura little-endian (os campos NTFS sao LE; em x86 o acesso
//  poderia ser direto, mas lemos byte-a-byte para evitar problemas de
//  alinhamento ao apontar para o meio de um buffer).
// ============================================================================
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
// Inteiro com sinal de 'n' bytes (1..8) little-endian — usado nos data runs.
static int64_t rd_signed(const uint8_t* p, int n) {
    int64_t v = 0;
    for (int i = 0; i < n; i++) v |= (int64_t)p[i] << (i * 8);
    if (n > 0 && n < 8 && (p[n - 1] & 0x80)) {   // estende o sinal
        v |= ~(int64_t)0 << (n * 8);
    }
    return v;
}

// ============================================================================
//  Estado do volume + um buffer de cluster reutilizavel.
// ============================================================================
static NTFS_VOLUME s_vol;

int ntfs_mounted(void) { return s_vol.mounted; }

// Le 'count' setores consecutivos do volume (LBA relativo a particao) para buf.
// Retorna 0 em sucesso. Cada setor passa pela HAL (que ja loga o setor lido).
static int ntfs_read_sectors(uint64_t vol_sector, uint32_t count, void* buf) {
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t lba = s_vol.part_lba + (uint32_t)(vol_sector + i);
        if (HalReadSector(lba, out + (uint64_t)i * s_vol.bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

// Le 'count' clusters a partir do LCN 'lcn' para buf.
static int ntfs_read_clusters(uint64_t lcn, uint32_t count, void* buf) {
    uint64_t first_sector = lcn * s_vol.sectors_per_cluster;
    uint32_t nsec = count * s_vol.sectors_per_cluster;
    return ntfs_read_sectors(first_sector, nsec, buf);
}

// ============================================================================
//  Fixups (Update Sequence Array / USA): NTFS grava o "Update Sequence Number"
//  (USN) nos 2 ultimos bytes de cada setor do registro e guarda os bytes
//  originais no array. Ao ler, conferimos o USN e RESTAURAMOS os bytes. Sem
//  isso, os ultimos 2 bytes de cada setor estariam corrompidos.
//  'buf' tem 'size' bytes; usa_off/usa_count vem do header (FILE ou INDX).
// ============================================================================
static int ntfs_apply_fixups(uint8_t* buf, uint32_t size,
                             uint16_t usa_off, uint16_t usa_count) {
    if (usa_count == 0) return 0;
    if ((uint32_t)usa_off + (uint32_t)usa_count * 2 > size) return -1;
    uint16_t usn = rd16(buf + usa_off);
    uint32_t sectors = usa_count - 1;   // 1 entrada de USN + 1 por setor
    for (uint32_t i = 0; i < sectors; i++) {
        uint32_t sec_end = (i + 1) * s_vol.bytes_per_sector - 2;
        if (sec_end + 2 > size) return -1;
        // Confere que o setor termina com o USN esperado (integridade).
        if (rd16(buf + sec_end) != usn) {
            // Nao aborta a leitura (algumas imagens sinteticas variam), mas loga.
            kputs("[ntfs] aviso: USN do setor "); kput_dec(i);
            kputs(" nao confere (fixup).\n");
        }
        const uint8_t* orig = buf + usa_off + 2 + i * 2;
        buf[sec_end]     = orig[0];
        buf[sec_end + 1] = orig[1];
    }
    return 0;
}

// ============================================================================
//  Le um registro MFT pelo numero. O $DATA do $MFT na imagem de teste e
//  contiguo a partir de mft_lcn (caso comum), entao localizamos o registro por
//  offset linear: byte = record_no * mft_record_size dentro do $DATA do $MFT.
//  buf deve ter pelo menos mft_record_size bytes. Aplica os fixups.
//  Retorna 0 em sucesso.
// ============================================================================
static int ntfs_read_mft_record(uint64_t record_no, uint8_t* buf) {
    uint64_t byte_off = record_no * s_vol.mft_record_size;
    uint64_t abs_byte = s_vol.mft_data_lcn * s_vol.bytes_per_cluster + byte_off;
    uint64_t sector   = abs_byte / s_vol.bytes_per_sector;
    uint32_t nsec     = s_vol.mft_record_size / s_vol.bytes_per_sector;
    if (nsec == 0) nsec = 1;

    if (ntfs_read_sectors(sector, nsec, buf) != 0) {
        kputs("[ntfs] erro lendo o registro MFT #"); kput_dec(record_no); kputc('\n');
        return -1;
    }
    // Valida a assinatura "FILE".
    if (!(buf[0] == 'F' && buf[1] == 'I' && buf[2] == 'L' && buf[3] == 'E')) {
        kputs("[ntfs] registro MFT #"); kput_dec(record_no);
        kputs(" sem assinatura FILE (");
        for (int i = 0; i < 4; i++) { char c = (char)buf[i]; kputc((c >= 0x20 && c < 0x7F) ? c : '.'); }
        kputs(").\n");
        return -1;
    }
    uint16_t usa_off   = rd16(buf + 0x04);
    uint16_t usa_count = rd16(buf + 0x06);
    ntfs_apply_fixups(buf, s_vol.mft_record_size, usa_off, usa_count);
    return 0;
}

// ============================================================================
//  Iteracao de atributos de um registro MFT. Devolve o ponteiro para o n-esimo
//  atributo do 'type' pedido (instancia 'which', 0 = primeiro), ou 0.
//  Se 'name_i30' != 0, casa apenas atributos cujo nome seja "$I30" (indices).
// ============================================================================
static const uint8_t* ntfs_find_attr(const uint8_t* rec, uint32_t type,
                                      int which, int name_i30) {
    uint16_t first = rd16(rec + 0x14);   // offset do 1o atributo
    uint32_t used  = rd32(rec + 0x18);   // bytes em uso no registro
    uint32_t off   = first;
    int seen = 0;
    while (off + 8 <= s_vol.mft_record_size && off + 8 <= used + 0x18 + 8) {
        const uint8_t* a = rec + off;
        uint32_t atype = rd32(a);
        if (atype == ATTR_END) break;
        uint32_t alen = rd32(a + 4);
        if (alen < 8 || off + alen > s_vol.mft_record_size) break;  // protecao

        if (atype == type) {
            int match = 1;
            if (name_i30) {
                uint8_t nlen = a[9];            // name length (chars)
                uint16_t noff = rd16(a + 10);   // name offset
                // "$I30" = '$','I','3','0' em UTF-16LE = 4 chars.
                if (nlen != 4 || off + noff + 8 > s_vol.mft_record_size) match = 0;
                else {
                    const uint8_t* nm = a + noff;
                    if (!(nm[0] == '$' && nm[2] == 'I' && nm[4] == '3' && nm[6] == '0'))
                        match = 0;
                }
            }
            if (match) {
                if (seen == which) return a;
                seen++;
            }
        }
        off += alen;
    }
    return 0;
}

// Conteudo de um atributo RESIDENTE: devolve ponteiro + tamanho.
// (Header residente: +0x10 content length (4), +0x14 content offset (2).)
static const uint8_t* attr_resident_data(const uint8_t* a, uint32_t* out_len) {
    uint32_t clen = rd32(a + 0x10);
    uint16_t coff = rd16(a + 0x14);
    if (out_len) *out_len = clen;
    return a + coff;
}

// Verdadeiro se o atributo e nao-residente (flag no offset 8).
static int attr_is_nonresident(const uint8_t* a) { return a[8] != 0; }

// ============================================================================
//  Data runs (atributo NAO-RESIDENTE): a lista de runs comeca no offset dado
//  por "mapping pairs offset" (+0x20 do header nao-residente). Cada run:
//    1 byte header: nibble baixo = tamanho do campo "length" (em bytes),
//                   nibble alto  = tamanho do campo "offset"  (em bytes);
//    'length' bytes: numero de clusters do run (unsigned);
//    'offset' bytes: delta de LCN em relacao ao run anterior (SIGNED).
//  header 0 = fim. Um run com offset_size==0 e "sparse" (buraco de zeros).
//
//  ntfs_run_for_vcn: dado um VCN (cluster logico dentro do atributo), encontra
//  o LCN fisico (ou -1 se sparse/fora). Tambem reporta quantos clusters
//  contiguos restam a partir desse VCN no mesmo run.
// ============================================================================
static int ntfs_run_for_vcn(const uint8_t* a, uint64_t vcn,
                            uint64_t* out_lcn, uint64_t* out_run_clusters,
                            int* out_sparse) {
    uint16_t runs_off = rd16(a + 0x20);    // mapping pairs offset
    uint32_t alen     = rd32(a + 4);
    const uint8_t* p  = a + runs_off;
    const uint8_t* end = a + alen;
    uint64_t cur_vcn = 0;
    int64_t  lcn = 0;                       // LCN absoluto acumulado

    while (p < end) {
        uint8_t hdr = *p++;
        if (hdr == 0) break;                // fim da lista
        int len_sz = hdr & 0x0F;
        int off_sz = (hdr >> 4) & 0x0F;
        if (len_sz == 0 || p + len_sz + off_sz > end) break;

        uint64_t run_len = 0;
        for (int i = 0; i < len_sz; i++) run_len |= (uint64_t)p[i] << (i * 8);
        p += len_sz;

        int sparse = (off_sz == 0);
        if (!sparse) {
            lcn += rd_signed(p, off_sz);
            p += off_sz;
        }
        // O VCN pedido cai neste run?
        if (vcn >= cur_vcn && vcn < cur_vcn + run_len) {
            uint64_t within = vcn - cur_vcn;
            if (out_sparse) *out_sparse = sparse;
            if (out_lcn)    *out_lcn = sparse ? 0 : (uint64_t)(lcn + (int64_t)within);
            if (out_run_clusters) *out_run_clusters = run_len - within;
            return 1;
        }
        cur_vcn += run_len;
    }
    return 0;   // VCN nao mapeado
}

// Tamanho logico de um atributo nao-residente (real data size, +0x30).
static uint64_t attr_nonresident_size(const uint8_t* a) {
    return rd64(a + 0x30);
}

// Le 'len' bytes a partir de 'offset' de um atributo NAO-RESIDENTE (via data
// runs) para 'buf'. Retorna bytes lidos. Clusters sparse viram zeros.
static uint32_t ntfs_read_nonresident(const uint8_t* a, uint64_t offset,
                                      uint8_t* buf, uint32_t len) {
    uint64_t real = attr_nonresident_size(a);
    if (offset >= real) return 0;
    if (offset + len > real) len = (uint32_t)(real - offset);

    uint32_t cl = s_vol.bytes_per_cluster;
    uint8_t* cluster_buf = (uint8_t*)kmalloc(cl);
    if (!cluster_buf) return 0;

    uint32_t done = 0;
    while (done < len) {
        uint64_t abs = offset + done;
        uint64_t vcn = abs / cl;
        uint32_t in_cl = (uint32_t)(abs % cl);

        uint64_t lcn = 0, run_clusters = 0; int sparse = 0;
        if (!ntfs_run_for_vcn(a, vcn, &lcn, &run_clusters, &sparse)) break;

        uint32_t chunk = cl - in_cl;
        if (chunk > len - done) chunk = len - done;

        if (sparse) {
            memset(buf + done, 0, chunk);
        } else {
            if (ntfs_read_clusters(lcn, 1, cluster_buf) != 0) break;
            memcpy(buf + done, cluster_buf + in_cl, chunk);
        }
        done += chunk;
    }
    kfree(cluster_buf);
    return done;
}

// ============================================================================
//  $DATA de um registro MFT (residente OU nao-residente, sem nome).
//  Le 'len' bytes a partir de 'offset' para 'buf'. Retorna bytes lidos.
// ============================================================================
static uint32_t ntfs_read_data_attr(const uint8_t* rec, uint64_t offset,
                                    uint8_t* buf, uint32_t len) {
    const uint8_t* a = ntfs_find_attr(rec, ATTR_DATA, 0, 0);
    if (!a) {
        kputs("[ntfs] registro sem atributo $DATA.\n");
        return 0;
    }
    if (!attr_is_nonresident(a)) {
        uint32_t clen = 0;
        const uint8_t* data = attr_resident_data(a, &clen);
        kputs("[ntfs] $DATA residente, "); kput_dec(clen); kputs(" bytes.\n");
        if (offset >= clen) return 0;
        uint32_t avail = clen - (uint32_t)offset;
        uint32_t n = len < avail ? len : avail;
        // Garante que o conteudo cabe no registro (protecao).
        memcpy(buf, data + offset, n);
        return n;
    }
    uint64_t real = attr_nonresident_size(a);
    kputs("[ntfs] $DATA nao-residente (data runs), "); kput_dec(real);
    kputs(" bytes.\n");
    return ntfs_read_nonresident(a, offset, buf, len);
}

// ============================================================================
//  $FILE_NAME -> nome ASCII. Pega a melhor instancia (preferindo o namespace
//  Win32, mas qualquer uma serve). Devolve o tamanho real do $DATA reportado
//  no $FILE_NAME e a flag de diretorio. 'out_name' deve ter NTFS_MAX_NAME+1.
// ============================================================================
static void utf16_to_ascii(const uint8_t* src, int chars, char* out, int max) {
    int i = 0;
    for (; i < chars && i < max - 1; i++) {
        uint16_t c = rd16(src + i * 2);
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : (c == 0 ? 0 : '?');
    }
    out[i] = 0;
}

// Preenche NTFS_FILE_INFO a partir de um registro MFT ja lido (nome + dir + size).
static void ntfs_fill_info(const uint8_t* rec, uint64_t record_no, NTFS_FILE_INFO* out) {
    out->mft_record = record_no;
    out->name[0] = 0;
    uint16_t flags = rd16(rec + 0x16);
    out->is_dir = (flags & MFT_FLAG_DIRECTORY) ? 1 : 0;
    out->size = 0;

    // Nome: percorre as instancias de $FILE_NAME e escolhe a de maior namespace
    // (Win32=1/3 > DOS=2 > POSIX=0) ou simplesmente a 1a valida.
    int best_ns = -1;
    for (int which = 0; which < 8; which++) {
        const uint8_t* a = ntfs_find_attr(rec, ATTR_FILE_NAME, which, 0);
        if (!a) break;
        if (attr_is_nonresident(a)) continue;
        uint32_t clen = 0;
        const uint8_t* fn = attr_resident_data(a, &clen);
        if (clen < 0x42) continue;
        uint8_t namelen = fn[0x40];
        uint8_t ns      = fn[0x41];
        int score = (ns == 1 || ns == 3) ? 3 : (ns == 2 ? 2 : 1);
        if (score > best_ns) {
            best_ns = score;
            utf16_to_ascii(fn + 0x42, namelen, out->name, sizeof(out->name));
        }
    }

    // Tamanho: do $DATA (real). Residente -> content length; nao-residente -> +0x30.
    const uint8_t* da = ntfs_find_attr(rec, ATTR_DATA, 0, 0);
    if (da) {
        if (attr_is_nonresident(da)) out->size = attr_nonresident_size(da);
        else { uint32_t clen = 0; attr_resident_data(da, &clen); out->size = clen; }
    }
}

// ============================================================================
//  Diretorios: $INDEX_ROOT (residente) + $INDEX_ALLOCATION (nao-residente).
//
//  $INDEX_ROOT content:
//    +0x00 indexed attr type (4), +0x04 collation (4), +0x08 bytes/index rec (4),
//    +0x0C clusters/index rec (1), +0x0D..0x0F pad,
//    +0x10 INDEX NODE HEADER: +0x00 entries offset (rel ao node hdr),
//          +0x04 total used, +0x08 total alloc, +0x0C flags.
//
//  Cada INDEX ENTRY:
//    +0x00 MFT ref (8: 6 bytes record + 2 seq), +0x08 entry length (2),
//    +0x0A key length (2), +0x0C flags (2). Se nao for END, a key e um
//    $FILE_NAME (parent ref @0, ..., namelen @0x40, ns @0x41, nome @0x42).
//
//  Percorre as entradas e, para cada uma que nao seja END nem '.'/metadados,
//  chama o callback com o nome + ref + dir flag + size (do $FILE_NAME).
// ============================================================================

// Processa um bloco de entradas de indice (a partir de entries_ptr, ate end).
// Conta entradas reportadas (via *pcount) e chama cb. 'rec_buf' nao usado aqui.
static void ntfs_walk_index_entries(const uint8_t* entries_ptr, const uint8_t* end,
                                    ntfs_dir_cb cb, void* ctx, int* pcount) {
    const uint8_t* e = entries_ptr;
    while (e + 0x10 <= end) {
        uint64_t ref   = rd64(e) & 0x0000FFFFFFFFFFFFULL;   // 6 bytes do record
        uint16_t elen  = rd16(e + 0x08);
        uint16_t klen  = rd16(e + 0x0A);
        uint16_t eflags= rd16(e + 0x0C);
        if (elen < 0x10) break;                              // protecao
        if (eflags & INDEX_ENTRY_END) break;                 // ultima entrada

        if (klen >= 0x42 && e + 0x10 + klen <= end) {
            const uint8_t* fn = e + 0x10;                    // key = $FILE_NAME
            uint8_t namelen = fn[0x40];
            uint8_t ns      = fn[0x41];
            uint32_t fnflags= rd32(fn + 0x38);               // file attributes (FILE_NAME)
            uint64_t realsz = rd64(fn + 0x30);               // real size
            // Ignora a entrada do namespace DOS (8.3) duplicada e nomes vazios.
            if (ns != 2 && namelen > 0) {
                NTFS_FILE_INFO info;
                info.mft_record = ref;
                info.is_dir = (fnflags & FILE_ATTR_DIRECTORY) ? 1 : 0;
                info.size   = info.is_dir ? 0 : realsz;
                utf16_to_ascii(fn + 0x42, namelen, info.name, sizeof(info.name));
                // Pula '.' (entrada da propria raiz) e nomes de metadados ($...).
                if (!(info.name[0] == '.' && info.name[1] == 0)) {
                    if (cb) cb(*pcount, &info, ctx);
                    (*pcount)++;
                }
            }
        }
        e += elen;
    }
}

int ntfs_list_dir(uint64_t dir_record, ntfs_dir_cb cb, void* ctx) {
    if (!s_vol.mounted) return 0;
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return 0;
    if (ntfs_read_mft_record(dir_record, rec) != 0) { kfree(rec); return 0; }

    int count = 0;

    // 1) $INDEX_ROOT (sempre residente).
    const uint8_t* ir = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 1);
    if (!ir) ir = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 0);  // fallback sem checar nome
    if (ir && !attr_is_nonresident(ir)) {
        uint32_t clen = 0;
        const uint8_t* root = attr_resident_data(ir, &clen);
        if (clen >= 0x20) {
            const uint8_t* node = root + 0x10;               // index node header
            uint32_t entries_off = rd32(node + 0x00);
            uint32_t total_used  = rd32(node + 0x04);
            const uint8_t* entries = node + entries_off;
            const uint8_t* end     = node + total_used;
            if (end > root + clen) end = root + clen;
            kputs("[ntfs]   $INDEX_ROOT do registro #"); kput_dec(dir_record);
            kputs(": percorrendo entradas...\n");
            ntfs_walk_index_entries(entries, end, cb, ctx, &count);
        }
    }

    // 2) $INDEX_ALLOCATION (nao-residente): blocos INDX adicionais (dir grande).
    const uint8_t* ia = ntfs_find_attr(rec, ATTR_INDEX_ALLOCATION, 0, 1);
    if (!ia) ia = ntfs_find_attr(rec, ATTR_INDEX_ALLOCATION, 0, 0);
    if (ia && attr_is_nonresident(ia)) {
        uint64_t total = attr_nonresident_size(ia);
        uint32_t irs = s_vol.index_record_size ? s_vol.index_record_size : s_vol.bytes_per_cluster;
        uint8_t* blk = (uint8_t*)kmalloc(irs);
        if (blk && irs) {
            kputs("[ntfs]   $INDEX_ALLOCATION: "); kput_dec(total);
            kputs(" bytes em blocos INDX de "); kput_dec(irs); kputs(".\n");
            for (uint64_t pos = 0; pos + irs <= total; pos += irs) {
                if (ntfs_read_nonresident(ia, pos, blk, irs) != irs) break;
                if (!(blk[0] == 'I' && blk[1] == 'N' && blk[2] == 'D' && blk[3] == 'X'))
                    continue;   // bloco nao usado
                uint16_t usa_off   = rd16(blk + 0x04);
                uint16_t usa_count = rd16(blk + 0x06);
                ntfs_apply_fixups(blk, irs, usa_off, usa_count);
                // INDEX BLOCK HEADER: o node header comeca em +0x18.
                const uint8_t* node = blk + 0x18;
                uint32_t entries_off = rd32(node + 0x00);
                uint32_t total_used  = rd32(node + 0x04);
                const uint8_t* entries = node + entries_off;
                const uint8_t* end     = node + total_used;
                if (end > blk + irs) end = blk + irs;
                ntfs_walk_index_entries(entries, end, cb, ctx, &count);
            }
        }
        if (blk) kfree(blk);
    }

    kfree(rec);
    return count;
}

// ============================================================================
//  Resolucao de caminho: '\', '\hello.txt', '\dir1\file.txt'.
//  Comeca na raiz (registro 5) e, para cada componente, lista o diretorio
//  atual procurando o nome (case-insensitive) — descendo de diretorio em
//  diretorio ate o ultimo componente.
// ============================================================================
static char nlower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  nieq(const char* a, const char* b) {
    while (*a && *b) { if (nlower(*a) != nlower(*b)) return 0; a++; b++; }
    return *a == *b;
}

// Contexto para "procurar um nome" via callback de ntfs_list_dir.
typedef struct { const char* want; int found; NTFS_FILE_INFO hit; } find_ctx;
static void find_cb(int index, const NTFS_FILE_INFO* info, void* ctx) {
    (void)index;
    find_ctx* f = (find_ctx*)ctx;
    if (!f->found && nieq(info->name, f->want)) {
        f->found = 1;
        f->hit = *info;
    }
}

int ntfs_resolve_path(const char* path, NTFS_FILE_INFO* out) {
    if (!s_vol.mounted || !path || !out) return 0;

    // Comeca na raiz. Le o registro #5 e preenche o info a partir dele (nome/
    // dir/size autenticos); se falhar, assume um diretorio raiz minimo.
    uint64_t cur = NTFS_MFT_ROOT;
    NTFS_FILE_INFO cur_info;
    cur_info.mft_record = cur; cur_info.is_dir = 1; cur_info.size = 0;
    cur_info.name[0] = '\\'; cur_info.name[1] = 0;
    {
        uint8_t* root = (uint8_t*)kmalloc(s_vol.mft_record_size);
        if (root) {
            if (ntfs_read_mft_record(NTFS_MFT_ROOT, root) == 0) {
                ntfs_fill_info(root, NTFS_MFT_ROOT, &cur_info);
                cur_info.is_dir = 1;            // a raiz e sempre diretorio
                cur_info.name[0] = '\\'; cur_info.name[1] = 0;
            }
            kfree(root);
        }
    }

    const char* p = path;
    while (*p == '\\' || *p == '/') p++;     // pula a(s) barra(s) inicial(is)

    char comp[NTFS_MAX_NAME + 1];
    while (*p) {
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < NTFS_MAX_NAME) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '\\' || *p == '/') p++;
        if (n == 0) continue;

        // Procura 'comp' no diretorio 'cur'.
        find_ctx f; f.want = comp; f.found = 0;
        ntfs_list_dir(cur, find_cb, &f);
        if (!f.found) {
            kputs("[ntfs] componente nao encontrado: '"); kputs(comp); kputs("'\n");
            return 0;
        }
        cur = f.hit.mft_record;
        cur_info = f.hit;
    }

    *out = cur_info;
    return 1;
}

// ============================================================================
//  ntfs_read_file: le o $DATA de um registro MFT (le o registro, depois o $DATA).
// ============================================================================
uint32_t ntfs_read_file(uint64_t mft_record, uint64_t offset, void* buf, uint32_t len) {
    if (!s_vol.mounted || !buf || !len) return 0;
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return 0;
    if (ntfs_read_mft_record(mft_record, rec) != 0) { kfree(rec); return 0; }
    uint32_t n = ntfs_read_data_attr(rec, offset, (uint8_t*)buf, len);
    kfree(rec);
    return n;
}

// ----------------------------------------------------------------------------
//  ntfs_volume_info — resumo do volume montado (FASE 5: NtQueryVolumeInformation).
//  Tenta ler o rotulo do registro $Volume (#3) pelo atributo $VOLUME_NAME (0x60,
//  UTF-16LE residente); se ausente, usa "MEUOS". O free e uma ESTIMATIVA: nao
//  varremos o $Bitmap (caminho de alocacao), entao reportamos ~7/8 do total como
//  livre (o volume de teste tem so metadados + 2 arquivos pequenos). Honesto e
//  suficiente p/ o 'vol'/'dir' do cmd; o tamanho TOTAL e exato (do BPB).
// ----------------------------------------------------------------------------
#define ATTR_VOLUME_NAME 0x60

int ntfs_volume_info(NTFS_VOLUME_INFO* out) {
    if (!out || !s_vol.mounted) return 0;
    memset(out, 0, sizeof(*out));
    out->serial           = s_vol.serial;
    out->bytes_per_sector = s_vol.bytes_per_sector;
    out->bytes_per_cluster= s_vol.bytes_per_cluster;
    out->total_bytes      = s_vol.total_sectors * (uint64_t)s_vol.bytes_per_sector;
    // Estimativa de espaco livre (sem varrer o $Bitmap): ~7/8 do total.
    out->free_bytes       = out->total_bytes - (out->total_bytes >> 3);
    // fs name
    out->fs_name[0]='N'; out->fs_name[1]='T'; out->fs_name[2]='F'; out->fs_name[3]='S'; out->fs_name[4]=0;

    // Rotulo padrao; tenta sobrescrever lendo o $Volume (#3) -> $VOLUME_NAME.
    out->label[0]='M'; out->label[1]='E'; out->label[2]='U'; out->label[3]='O';
    out->label[4]='S'; out->label[5]=0;
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (rec) {
        if (ntfs_read_mft_record(3, rec) == 0) {
            const uint8_t* vn = ntfs_find_attr(rec, ATTR_VOLUME_NAME, 0, 0);
            uint32_t vlen = 0;
            if (vn && !attr_is_nonresident(vn)) {
                const uint8_t* content = attr_resident_data(vn, &vlen);
                if (content && vlen >= 2) {
                    int chars = (int)(vlen / 2);
                    if (chars > (int)sizeof(out->label) - 1) chars = (int)sizeof(out->label) - 1;
                    utf16_to_ascii(content, chars, out->label, (int)sizeof(out->label));
                }
            }
        }
        kfree(rec);
    }
    return 1;
}

// ============================================================================
//  ntfs_mount — le o boot sector da particao, valida e preenche o BPB.
// ============================================================================
int ntfs_mount(uint32_t part_lba) {
    memset(&s_vol, 0, sizeof(s_vol));
    s_vol.part_lba = part_lba;
    s_vol.bytes_per_sector = HAL_SECTOR_SIZE;   // assume 512 ate ler o BPB

    kputs("\n--- FASE 3: montando volume NTFS (leitura) ---\n");
    kputs("[ntfs] lendo boot sector da particao em LBA "); kput_dec(part_lba); kputs("...\n");

    static uint8_t boot[HAL_SECTOR_SIZE];
    if (HalReadSector(part_lba, boot) != 0) {
        kputs("[ntfs] falha ao ler o boot sector.\n");
        return 0;
    }
    if (!(boot[3] == 'N' && boot[4] == 'T' && boot[5] == 'F' && boot[6] == 'S')) {
        kputs("[ntfs] assinatura 'NTFS    ' ausente no offset 3 (nao e NTFS).\n");
        return 0;
    }

    // --- BPB ---
    s_vol.bytes_per_sector    = rd16(boot + 0x0B);
    s_vol.sectors_per_cluster = boot[0x0D];
    s_vol.total_sectors       = rd64(boot + 0x28);
    s_vol.mft_lcn             = rd64(boot + 0x30);
    s_vol.mftmirr_lcn         = rd64(boot + 0x38);
    s_vol.serial              = rd64(boot + 0x48);   // numero de serie do volume

    if (s_vol.bytes_per_sector == 0) s_vol.bytes_per_sector = HAL_SECTOR_SIZE;
    if (s_vol.sectors_per_cluster == 0) s_vol.sectors_per_cluster = 1;
    s_vol.bytes_per_cluster = (uint32_t)s_vol.bytes_per_sector * s_vol.sectors_per_cluster;
    s_vol.mft_data_lcn = s_vol.mft_lcn;

    // clusters per MFT record (offset 0x40, SIGNED): se >0, e em clusters; se
    // <0, e 2^|v| bytes. NTFS usa -10 => 1024 bytes por registro.
    int8_t clpmft = (int8_t)boot[0x40];
    if (clpmft > 0) s_vol.mft_record_size = (uint32_t)clpmft * s_vol.bytes_per_cluster;
    else            s_vol.mft_record_size = (uint32_t)1u << (uint32_t)(-clpmft);

    // clusters per index record (offset 0x44, SIGNED): mesmo esquema.
    int8_t clpidx = (int8_t)boot[0x44];
    if (clpidx > 0) s_vol.index_record_size = (uint32_t)clpidx * s_vol.bytes_per_cluster;
    else            s_vol.index_record_size = (uint32_t)1u << (uint32_t)(-clpidx);

    // Sanidade: registro MFT entre 256 B e 8 KiB; multiplo do setor.
    if (s_vol.mft_record_size < 256 || s_vol.mft_record_size > 8192) {
        kputs("[ntfs] tamanho de registro MFT invalido ("); kput_dec(s_vol.mft_record_size);
        kputs("); abortando a montagem.\n");
        return 0;
    }

    kputs("[ntfs] BPB: bytes/setor="); kput_dec(s_vol.bytes_per_sector);
    kputs(" setores/cluster="); kput_dec(s_vol.sectors_per_cluster);
    kputs(" bytes/cluster="); kput_dec(s_vol.bytes_per_cluster); kputc('\n');
    kputs("[ntfs] BPB: total_setores="); kput_dec(s_vol.total_sectors);
    kputs(" $MFT_LCN="); kput_dec(s_vol.mft_lcn);
    kputs(" $MFTMirr_LCN="); kput_dec(s_vol.mftmirr_lcn); kputc('\n');
    kputs("[ntfs] BPB: tam. registro MFT="); kput_dec(s_vol.mft_record_size);
    kputs(" bytes; tam. index record="); kput_dec(s_vol.index_record_size); kputs(" bytes\n");

    // --- Le o registro #0 ($MFT) para validar a montagem e logar. ---
    uint8_t* rec0 = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec0) { kputs("[ntfs] sem memoria para o registro MFT.\n"); return 0; }
    if (ntfs_read_mft_record(0, rec0) != 0) {
        kputs("[ntfs] nao consegui ler o registro #0 ($MFT). Montagem falhou.\n");
        kfree(rec0);
        return 0;
    }
    kputs("[ntfs] registro MFT #0 ($MFT) lido: assinatura 'FILE' OK, fixups aplicados.\n");

    // Se o $MFT tiver $DATA nao-residente, o 1o run da o LCN REAL do $MFT — mas
    // normalmente comeca exatamente em mft_lcn (ja usado). Confirmamos o run.
    const uint8_t* mft_data = ntfs_find_attr(rec0, ATTR_DATA, 0, 0);
    if (mft_data && attr_is_nonresident(mft_data)) {
        uint64_t lcn = 0, rc = 0; int sp = 0;
        if (ntfs_run_for_vcn(mft_data, 0, &lcn, &rc, &sp) && !sp) {
            kputs("[ntfs] $MFT $DATA nao-residente: 1o run LCN="); kput_dec(lcn);
            kputs(" ("); kput_dec(rc); kputs(" clusters).\n");
            s_vol.mft_data_lcn = lcn;   // usa o LCN do data run (autoridade)
        }
    } else {
        kputs("[ntfs] $MFT $DATA residente/ausente: usando $MFT_LCN do BPB.\n");
    }
    kfree(rec0);

    s_vol.mounted = 1;
    kputs("[ntfs] volume NTFS MONTADO com sucesso.\n");
    return 1;
}

// ============================================================================
// ============================================================================
//
//   FASE 4 — ESCRITA NTFS (subconjunto SEGURO: sem alocacao de clusters).
//
//   Implementa a escrita que NAO precisa alocar clusters novos nem tocar no
//   $Bitmap/$LogFile (o caminho perigoso). Tudo aqui reusa o que JA esta
//   alocado:
//     - sobrescrever/crescer/encurtar um $DATA RESIDENTE dentro do registro MFT;
//     - sobrescrever um $DATA NAO-RESIDENTE nos clusters ja existentes.
//   Cada operacao loga na serial (regra 4) e reaplica os fixups (USA) antes da
//   escrita do registro MFT (inverso exato da leitura).
//
// ============================================================================
// ============================================================================

int ntfs_readonly(void) { return 0; }   // a HAL tem HalWriteSector

// ----------------------------------------------------------------------------
//  Escreve 'count' setores consecutivos (LBA relativo a particao) a partir de
//  buf. Retorna 0 em sucesso. Cada setor passa pela HAL (que ja loga a escrita).
// ----------------------------------------------------------------------------
static int ntfs_write_sectors(uint64_t vol_sector, uint32_t count, const void* buf) {
    const uint8_t* in = (const uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t lba = s_vol.part_lba + (uint32_t)(vol_sector + i);
        if (HalWriteSector(lba, in + (uint64_t)i * s_vol.bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

// Escreve 'count' clusters a partir do LCN 'lcn' (clusters ja alocados!).
static int ntfs_write_clusters(uint64_t lcn, uint32_t count, const void* buf) {
    uint64_t first_sector = lcn * s_vol.sectors_per_cluster;
    uint32_t nsec = count * s_vol.sectors_per_cluster;
    return ntfs_write_sectors(first_sector, nsec, buf);
}

// ----------------------------------------------------------------------------
//  Fixups na direcao da ESCRITA (inverso de ntfs_apply_fixups): incrementa o
//  USN, grava-o no slot do USA e nos 2 ultimos bytes de CADA setor do registro,
//  salvando os bytes reais (que estavam ali) no array. Sem isto, ao reler o
//  registro o fixup restauraria bytes errados nos finais de setor.
//  buf tem 'size' bytes; usa_off/usa_count vem do header (FILE/INDX).
// ----------------------------------------------------------------------------
static int ntfs_apply_fixups_for_write(uint8_t* buf, uint32_t size,
                                       uint16_t usa_off, uint16_t usa_count) {
    if (usa_count == 0) return 0;
    if ((uint32_t)usa_off + (uint32_t)usa_count * 2 > size) return -1;
    // Novo USN = (USN atual + 1), evitando 0x0000 e 0xFFFF (valores reservados).
    uint16_t usn = (uint16_t)(rd16(buf + usa_off) + 1);
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    buf[usa_off]     = (uint8_t)(usn & 0xFF);
    buf[usa_off + 1] = (uint8_t)(usn >> 8);
    uint32_t sectors = usa_count - 1;
    for (uint32_t i = 0; i < sectors; i++) {
        uint32_t sec_end = (i + 1) * s_vol.bytes_per_sector - 2;
        if (sec_end + 2 > size) return -1;
        uint8_t* slot = buf + usa_off + 2 + i * 2;
        slot[0] = buf[sec_end];          // salva os 2 bytes reais do fim do setor
        slot[1] = buf[sec_end + 1];
        buf[sec_end]     = (uint8_t)(usn & 0xFF);   // grava o USN no fim do setor
        buf[sec_end + 1] = (uint8_t)(usn >> 8);
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  Reescreve um registro MFT no disco (inverso de ntfs_read_mft_record): valida
//  o header FILE, aplica os fixups de ESCRITA e grava os setores via HAL.
//  'buf' DEVE ter mft_record_size bytes (o registro completo, ja editado).
// ----------------------------------------------------------------------------
int ntfs_write_mft_record(uint64_t record_no, uint8_t* buf) {
    if (!s_vol.mounted) return -1;
    if (!(buf[0] == 'F' && buf[1] == 'I' && buf[2] == 'L' && buf[3] == 'E')) {
        kputs("[ntfs] write: registro #"); kput_dec(record_no);
        kputs(" sem header FILE; abortando.\n");
        return -1;
    }
    uint16_t usa_off   = rd16(buf + 0x04);
    uint16_t usa_count = rd16(buf + 0x06);
    if (ntfs_apply_fixups_for_write(buf, s_vol.mft_record_size, usa_off, usa_count) != 0) {
        kputs("[ntfs] write: USA invalido no registro #"); kput_dec(record_no); kputc('\n');
        return -1;
    }

    uint64_t byte_off = record_no * s_vol.mft_record_size;
    uint64_t abs_byte = s_vol.mft_data_lcn * s_vol.bytes_per_cluster + byte_off;
    uint64_t sector   = abs_byte / s_vol.bytes_per_sector;
    uint32_t nsec     = s_vol.mft_record_size / s_vol.bytes_per_sector;
    if (nsec == 0) nsec = 1;

    kputs("[ntfs] write: gravando registro MFT #"); kput_dec(record_no);
    kputs(" ("); kput_dec(nsec); kputs(" setores @ LBA particao "); kput_dec(sector);
    kputs(") com fixups reaplicados.\n");
    if (ntfs_write_sectors(sector, nsec, buf) != 0) {
        kputs("[ntfs] write: falha ao gravar os setores do registro MFT.\n");
        return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  Atualiza, no $INDEX_ROOT do diretorio 'parent_record', os campos de TAMANHO
//  (real size @0x30, allocated size @0x28) da entrada cujo $FILE_NAME case com
//  o registro 'child_record'. Mantem o NTFS consistente apos crescer/encurtar um
//  arquivo (o tamanho aparece tanto no $FILE_NAME do arquivo quanto na chave do
//  indice do pai). So mexe em $INDEX_ROOT residente (caminho seguro do volume de
//  teste). Retorna 0 se atualizou, -1 se nao achou/erro.
// ----------------------------------------------------------------------------
static int ntfs_index_update_size(uint64_t parent_record, uint64_t child_record,
                                  uint64_t new_real, uint64_t new_alloc) {
    if (parent_record == 0) return -1;
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_mft_record(parent_record, rec) != 0) { kfree(rec); return -1; }

    const uint8_t* ir = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 1);
    if (!ir) ir = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 0);
    if (!ir || attr_is_nonresident(ir)) { kfree(rec); return -1; }

    uint32_t clen = 0;
    const uint8_t* root_c = attr_resident_data(ir, &clen);
    // root_c aponta para dentro de 'rec'; vamos editar in-place (mutavel).
    uint8_t* root = (uint8_t*)root_c;
    if (clen < 0x20) { kfree(rec); return -1; }

    uint8_t* node = root + 0x10;
    uint32_t entries_off = rd32(node + 0x00);
    uint32_t total_used  = rd32(node + 0x04);
    uint8_t* e   = node + entries_off;
    uint8_t* end = node + total_used;
    if (end > root + clen) end = root + clen;

    int updated = -1;
    while (e + 0x10 <= end) {
        uint64_t ref  = rd64(e) & 0x0000FFFFFFFFFFFFULL;
        uint16_t elen = rd16(e + 0x08);
        uint16_t klen = rd16(e + 0x0A);
        uint16_t efl  = rd16(e + 0x0C);
        if (elen < 0x10) break;
        if (efl & INDEX_ENTRY_END) break;
        if (ref == child_record && klen >= 0x42) {
            uint8_t* fn = e + 0x10;            // chave = $FILE_NAME
            // allocated size @0x28, real size @0x30 (8 bytes cada, LE).
            for (int i = 0; i < 8; i++) fn[0x28 + i] = (uint8_t)((new_alloc >> (i * 8)) & 0xFF);
            for (int i = 0; i < 8; i++) fn[0x30 + i] = (uint8_t)((new_real  >> (i * 8)) & 0xFF);
            updated = 0;
            kputs("[ntfs] write: entrada de indice do pai #"); kput_dec(parent_record);
            kputs(" p/ o filho #"); kput_dec(child_record);
            kputs(" atualizada (real="); kput_dec(new_real); kputs(").\n");
            break;
        }
        e += elen;
    }

    if (updated == 0) {
        if (ntfs_write_mft_record(parent_record, rec) != 0) updated = -1;
    }
    kfree(rec);
    return updated;
}

// ----------------------------------------------------------------------------
//  Atualiza o tamanho (real/allocated) em TODAS as instancias de $FILE_NAME do
//  proprio registro do arquivo (edicao in-place no buffer 'rec' ja carregado).
// ----------------------------------------------------------------------------
static void ntfs_record_set_filename_size(uint8_t* rec, uint64_t new_real,
                                          uint64_t new_alloc) {
    for (int which = 0; which < 8; which++) {
        const uint8_t* a = ntfs_find_attr(rec, ATTR_FILE_NAME, which, 0);
        if (!a) break;
        if (attr_is_nonresident(a)) continue;
        uint32_t clen = 0;
        uint8_t* fn = (uint8_t*)attr_resident_data(a, &clen);
        if (clen < 0x42) continue;
        for (int i = 0; i < 8; i++) fn[0x28 + i] = (uint8_t)((new_alloc >> (i * 8)) & 0xFF);
        for (int i = 0; i < 8; i++) fn[0x30 + i] = (uint8_t)((new_real  >> (i * 8)) & 0xFF);
    }
}

// ----------------------------------------------------------------------------
//  Sobrescreve o $DATA de um registro MFT. Veja ntfs.h para a semantica.
//    - residente: copia 'len' bytes em offset; se set_eof e o novo fim couber no
//      registro, ajusta o tamanho (cresce/encurta) + $FILE_NAME + $INDEX do pai.
//    - nao-residente: grava nos clusters ja alocados (offset+len <= tamanho real).
// ----------------------------------------------------------------------------
uint32_t ntfs_write_file(uint64_t mft_record, uint64_t offset,
                         const void* buf, uint32_t len,
                         int set_eof, uint64_t parent_record) {
    if (!s_vol.mounted || !buf) return 0;

    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return 0;
    if (ntfs_read_mft_record(mft_record, rec) != 0) { kfree(rec); return 0; }

    const uint8_t* a_const = ntfs_find_attr(rec, ATTR_DATA, 0, 0);
    if (!a_const) {
        kputs("[ntfs] write: registro #"); kput_dec(mft_record);
        kputs(" sem $DATA; nada a escrever.\n");
        kfree(rec); return 0;
    }
    uint8_t* a = (uint8_t*)a_const;     // editavel (aponta p/ dentro de 'rec')

    // =================== $DATA NAO-RESIDENTE (overwrite in-place) ============
    if (attr_is_nonresident(a)) {
        uint64_t real = attr_nonresident_size(a);
        kputs("[ntfs] write: $DATA NAO-residente (data runs), tamanho real ");
        kput_dec(real); kputs(" bytes.\n");
        if (offset >= real) { kfree(rec); return 0; }
        if (offset + len > real) {
            kputs("[ntfs] write: truncando p/ nao crescer um $DATA nao-residente "
                  "(sem alocacao de clusters).\n");
            len = (uint32_t)(real - offset);
        }
        uint32_t cl = s_vol.bytes_per_cluster;
        uint8_t* cbuf = (uint8_t*)kmalloc(cl);
        if (!cbuf) { kfree(rec); return 0; }
        const uint8_t* in = (const uint8_t*)buf;
        uint32_t done = 0;
        while (done < len) {
            uint64_t abs = offset + done;
            uint64_t vcn = abs / cl;
            uint32_t in_cl = (uint32_t)(abs % cl);
            uint64_t lcn = 0, run_clusters = 0; int sparse = 0;
            if (!ntfs_run_for_vcn(a, vcn, &lcn, &run_clusters, &sparse) || sparse) {
                kputs("[ntfs] write: VCN nao mapeado/sparse; parando.\n");
                break;     // nao alocamos clusters novos
            }
            uint32_t chunk = cl - in_cl;
            if (chunk > len - done) chunk = len - done;
            // read-modify-write do cluster (preserva os bytes nao tocados).
            if (ntfs_read_clusters(lcn, 1, cbuf) != 0) break;
            memcpy(cbuf + in_cl, in + done, chunk);
            if (ntfs_write_clusters(lcn, 1, cbuf) != 0) break;
            done += chunk;
        }
        kfree(cbuf);
        kputs("[ntfs] write: "); kput_dec(done);
        kputs(" bytes gravados no $DATA nao-residente (clusters existentes).\n");
        kfree(rec);
        return done;
    }

    // =================== $DATA RESIDENTE =====================================
    //  IMPORTANTE: validamos TUDO antes de tocar nos bytes (validate-before-write)
    //  para nunca corromper o registro num caminho que depois aborta.
    uint32_t attr_len   = rd32(a + 0x04);
    uint32_t content_len= rd32(a + 0x10);
    uint16_t content_off= rd16(a + 0x14);
    uint32_t attr_off   = (uint32_t)(a - rec);

    // Tamanho-alvo do conteudo apos a escrita.
    uint64_t new_eof = offset + len;
    uint32_t new_content_len = content_len;
    if (set_eof)               new_content_len = (uint32_t)new_eof;  // grow/shrink p/ o EOF
    else if (new_eof > content_len) new_content_len = (uint32_t)new_eof;  // estende se passar do fim

    kputs("[ntfs] write: $DATA RESIDENTE atual="); kput_dec(content_len);
    kputs(" bytes; escrevendo "); kput_dec(len); kputs(" bytes @offset ");
    kput_dec(offset); kputs("; novo tamanho-alvo="); kput_dec(new_content_len); kputc('\n');

    int resizing = (new_content_len != content_len);

    // Para PODER mudar o tamanho com seguranca, o $DATA tem de ser o ULTIMO
    // atributo (END logo apos ele) — senao precisariamos deslocar atributos. E o
    // novo tamanho tem de caber no registro MFT (sem converter p/ nao-residente).
    int can_resize = 1;
    uint32_t max_attr    = s_vol.mft_record_size - attr_off - 8;     // espaco p/ o atributo + END
    uint32_t max_content = (max_attr > content_off) ? (max_attr - content_off) : 0;
    uint32_t old_next    = attr_off + attr_len;     // onde DEVERIA estar o END atual
    if (resizing) {
        if (new_content_len > max_content) {
            kputs("[ntfs] write: novo tamanho ("); kput_dec(new_content_len);
            kputs(") excede o espaco do registro MFT (max "); kput_dec(max_content);
            kputs("); converter p/ NAO-residente (alocar clusters) NAO e suportado (seguro).\n");
            can_resize = 0;
        }
        if (can_resize && !(old_next + 4 <= s_vol.mft_record_size && rd32(rec + old_next) == ATTR_END)) {
            kputs("[ntfs] write: $DATA nao e o ultimo atributo do registro; resize "
                  "exigiria deslocar atributos -> NAO suportado (seguro).\n");
            can_resize = 0;
        }
    }

    // Se nao podemos mudar o tamanho, fazemos a escrita possivel SEM crescer:
    // limitamos a regiao ao content_len atual (sobrescrita pura, sempre segura).
    if (resizing && !can_resize) {
        if (offset >= content_len) {
            kputs("[ntfs] write: offset apos o fim e sem resize possivel; nada a escrever.\n");
            kfree(rec); return 0;
        }
        if (offset + len > content_len) len = content_len - (uint32_t)offset;
        new_content_len = content_len;          // tamanho inalterado
        resizing = 0;
        kputs("[ntfs] write: prosseguindo como SOBRESCRITA (sem mudar tamanho), "
              "len ajustado p/ "); kput_dec(len); kputs(" bytes.\n");
    }

    // A partir daqui a operacao e segura. Aplica as mudancas no buffer.
    if (offset > content_len) {     // gap entre o fim antigo e o offset -> zera
        memset(a + content_off + content_len, 0, (uint32_t)offset - content_len);
    }
    memcpy(a + content_off + (uint32_t)offset, buf, len);

    if (resizing) {
        uint32_t new_attr_len = content_off + new_content_len;
        new_attr_len = (new_attr_len + 7) & ~7u;
        for (int i = 0; i < 4; i++) a[0x10 + i] = (uint8_t)((new_content_len >> (i * 8)) & 0xFF);
        for (int i = 0; i < 4; i++) a[0x04 + i] = (uint8_t)((new_attr_len    >> (i * 8)) & 0xFF);

        uint32_t new_next = attr_off + new_attr_len;     // novo lugar do END
        // Zera qualquer "lixo" entre o novo fim do conteudo e o novo END (se encolheu
        // ou por alinhamento), e tambem a regiao do antigo END se ela ficou alem.
        // Grava o marcador de fim 0xFFFFFFFF + length 0.
        for (int i = 0; i < 4; i++) rec[new_next + i] = 0xFF;
        rec[new_next + 4] = 0; rec[new_next + 5] = 0;
        rec[new_next + 6] = 0; rec[new_next + 7] = 0;
        // bytes used (@0x18) = fim do END alinhado a 8.
        uint32_t used = (new_next + 8 + 7) & ~7u;
        for (int i = 0; i < 4; i++) rec[0x18 + i] = (uint8_t)((used >> (i * 8)) & 0xFF);

        // Atualiza o tamanho no $FILE_NAME do registro + na entrada do $INDEX do pai.
        uint64_t new_alloc = (new_content_len + 7) & ~7u;
        ntfs_record_set_filename_size(rec, new_content_len, new_alloc);
        if (parent_record)
            ntfs_index_update_size(parent_record, mft_record, new_content_len, new_alloc);
    }

    if (ntfs_write_mft_record(mft_record, rec) != 0) { kfree(rec); return 0; }
    kputs("[ntfs] write: $DATA residente do registro #"); kput_dec(mft_record);
    kputs(" gravado ("); kput_dec(len); kputs(" bytes, tamanho final ");
    kput_dec(new_content_len); kputs(").\n");
    kfree(rec);
    return len;
}

// ============================================================================
//
//   CRIAR / EXCLUIR arquivo e diretorio (subconjunto SEGURO, sem alocar cluster).
//
// ============================================================================

// Escreve um inteiro LE de 'n' bytes em 'p'.
static void wr_le(uint8_t* p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
}

// ASCII -> UTF-16LE em 'out' (2*n bytes). Retorna o numero de chars.
static int ascii_to_utf16(const char* s, uint8_t* out, int max_chars) {
    int n = 0;
    for (; s[n] && n < max_chars; n++) { out[n * 2] = (uint8_t)s[n]; out[n * 2 + 1] = 0; }
    return n;
}

// Monta o CONTEUDO de um atributo $FILE_NAME (sem o header de atributo), igual ao
// make-ntfs-image.py: parent ref(8) + 4 timestamps(8 cada) + alloc(8) + real(8) +
// flags(4) + ea/reparse(4) + namelen(1) + namespace(1) + nome UTF-16LE.
// Retorna o tamanho total do conteudo.
#define NT_TIME_FIXED 0x01D6000000000000ULL
static uint32_t build_filename_content(uint64_t parent_ref, const char* name,
                                       uint32_t flags, uint64_t real, uint64_t alloc,
                                       uint8_t* out) {
    uint32_t o = 0;
    wr_le(out + o, parent_ref, 8); o += 8;
    for (int i = 0; i < 4; i++) { wr_le(out + o, NT_TIME_FIXED, 8); o += 8; }
    wr_le(out + o, alloc, 8); o += 8;
    wr_le(out + o, real,  8); o += 8;
    wr_le(out + o, flags, 4); o += 4;
    wr_le(out + o, 0,     4); o += 4;     // EA/reparse
    int namelen = 0; const char* t = name; while (*t) { namelen++; t++; }
    out[o++] = (uint8_t)namelen;
    out[o++] = 3;                          // namespace Win32&DOS (1/3 = Win32)
    o += (uint32_t)(ascii_to_utf16(name, out + o, namelen) * 2);
    return o;
}

// Monta um atributo RESIDENTE completo (header + conteudo) em 'out'; retorna o
// tamanho total (alinhado a 8). Espelha make_resident_attr do builder.
static uint32_t build_resident_attr(uint32_t type, const uint8_t* content,
                                    uint32_t clen, const char* name16_ascii,
                                    uint8_t* out) {
    uint8_t name_chars = 0;
    if (name16_ascii) { const char* t = name16_ascii; while (*t) { name_chars++; t++; } }
    uint16_t name_off = name_chars ? 24 : 0;
    uint32_t content_off = 24 + (uint32_t)name_chars * 2;
    content_off = (content_off + 7) & ~7u;
    uint32_t rec_len = content_off + clen;
    rec_len = (rec_len + 7) & ~7u;

    memset(out, 0, rec_len);
    wr_le(out + 0x00, type, 4);
    wr_le(out + 0x04, rec_len, 4);
    out[0x08] = 0;                 // non-resident = 0
    out[0x09] = name_chars;
    wr_le(out + 0x0A, name_off, 2);
    wr_le(out + 0x0C, 0, 2);       // flags
    wr_le(out + 0x0E, 0, 2);       // attribute id
    wr_le(out + 0x10, clen, 4);    // content length
    wr_le(out + 0x14, content_off, 2);
    out[0x16] = 0;                 // indexed flag
    out[0x17] = 0;                 // padding
    if (name_chars) ascii_to_utf16(name16_ascii, out + 24, name_chars);
    if (clen) memcpy(out + content_off, content, clen);
    return rec_len;
}

// $STANDARD_INFORMATION curto (48 bytes): 4 timestamps + flags + 12 bytes zero.
static uint32_t build_stdinfo(uint32_t flags, uint8_t* out) {
    uint32_t o = 0;
    for (int i = 0; i < 4; i++) { wr_le(out + o, NT_TIME_FIXED, 8); o += 8; }
    wr_le(out + o, flags, 4); o += 4;
    wr_le(out + o, 0, 4); o += 4;     // max versions
    wr_le(out + o, 0, 4); o += 4;     // version
    wr_le(out + o, 0, 4); o += 4;     // class id
    return o;     // 48
}

// $INDEX_ROOT "vazio" (so a entrada END), indexando $FILE_NAME. Retorna o tamanho.
static uint32_t build_empty_index_root(uint8_t* out) {
    uint32_t o = 0;
    // index root header (16 bytes)
    wr_le(out + o, ATTR_FILE_NAME, 4); o += 4;     // indexed attr type
    wr_le(out + o, 0x01, 4); o += 4;               // collation = filename
    wr_le(out + o, s_vol.index_record_size ? s_vol.index_record_size : 4096, 4); o += 4;
    out[o++] = 1; out[o++] = 0; out[o++] = 0; out[o++] = 0;   // clusters/index rec + pad
    // node header (16 bytes): entries_off=16, used=16+16(END), alloc=used, flags=0
    uint32_t node = o;
    wr_le(out + node + 0, 16, 4);
    wr_le(out + node + 4, 16 + 16, 4);
    wr_le(out + node + 8, 16 + 16, 4);
    wr_le(out + node + 12, 0, 4);
    o += 16;
    // END entry (16 bytes): ref=0, len=16, keylen=0, flags=END(2)
    wr_le(out + o, 0, 8); o += 8;
    wr_le(out + o, 16, 2); o += 2;
    wr_le(out + o, 0, 2);  o += 2;
    wr_le(out + o, 0x02, 4); o += 4;
    return o;   // 48
}

// ----------------------------------------------------------------------------
//  Localiza um numero de registro MFT LIVRE (sem a flag in-use). Varre a partir
//  de 'start' ate 'start+limit'. Retorna o numero, ou 0 se nao achar.
//  Para a imagem de teste, registros >= 27 estao zerados/nao-em-uso e ficam num
//  vao seguro do disco (apos os 27 registros da MFT, antes de outras estruturas).
// ----------------------------------------------------------------------------
static uint64_t ntfs_find_free_record(uint64_t start, uint64_t limit) {
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return 0;
    for (uint64_t n = start; n < start + limit; n++) {
        // Le os setores crus (sem exigir 'FILE': registros livres podem ser zeros).
        uint64_t byte_off = n * s_vol.mft_record_size;
        uint64_t abs_byte = s_vol.mft_data_lcn * s_vol.bytes_per_cluster + byte_off;
        uint64_t sector   = abs_byte / s_vol.bytes_per_sector;
        uint32_t nsec     = s_vol.mft_record_size / s_vol.bytes_per_sector;
        if (nsec == 0) nsec = 1;
        if (ntfs_read_sectors(sector, nsec, rec) != 0) continue;
        int is_file = (rec[0] == 'F' && rec[1] == 'I' && rec[2] == 'L' && rec[3] == 'E');
        int in_use = 0;
        if (is_file) {
            uint16_t flags = rd16(rec + 0x16);
            in_use = (flags & MFT_FLAG_IN_USE) ? 1 : 0;
        }
        if (!in_use) { kfree(rec); return n; }   // zeros OU FILE sem in-use
    }
    kfree(rec);
    return 0;
}

// ----------------------------------------------------------------------------
//  Insere uma entrada (child_ref/name/flags/size) no $INDEX_ROOT (residente) do
//  diretorio 'parent_record'. O $INDEX_ROOT deve ser o ULTIMO atributo (caminho
//  seguro: vale p/ os diretorios da imagem de teste). Cresce o atributo dentro
//  da folga do registro. Retorna 0 em sucesso.
// ----------------------------------------------------------------------------
static int ntfs_index_insert(uint64_t parent_record, uint64_t child_ref,
                             const char* name, int is_dir, uint64_t real_size) {
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_mft_record(parent_record, rec) != 0) { kfree(rec); return -1; }

    const uint8_t* ir_c = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 1);
    if (!ir_c) ir_c = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 0);
    if (!ir_c || attr_is_nonresident(ir_c)) { kfree(rec); return -1; }
    uint8_t* ir = (uint8_t*)ir_c;
    uint32_t attr_off = (uint32_t)(ir - rec);
    uint32_t attr_len = rd32(ir + 0x04);

    // Confirma que e o ultimo atributo (END logo apos) — caminho seguro p/ crescer.
    uint32_t next = attr_off + attr_len;
    if (!(next + 4 <= s_vol.mft_record_size && rd32(rec + next) == ATTR_END)) {
        kputs("[ntfs] create: $INDEX_ROOT do pai #"); kput_dec(parent_record);
        kputs(" nao e o ultimo atributo; insercao segura nao suportada.\n");
        kfree(rec); return -1;
    }

    uint16_t content_off = rd16(ir + 0x14);
    uint32_t content_len = rd32(ir + 0x10);
    uint8_t* root = ir + content_off;          // index root content
    uint8_t* node = root + 0x10;               // node header
    uint32_t entries_off = rd32(node + 0x00);
    uint32_t total_used  = rd32(node + 0x04);
    uint8_t* entries = node + entries_off;

    // Monta a chave $FILE_NAME do novo nome.
    uint8_t key[320];
    uint32_t flags = is_dir ? FILE_ATTR_DIRECTORY : 0x20 /*ARCHIVE*/;
    uint64_t alloc = is_dir ? 0 : ((real_size + 7) & ~7ull);
    uint64_t parent_ref = (1ull << 48) | parent_record;   // seq 1
    uint32_t keylen = build_filename_content(parent_ref, name, flags,
                                             is_dir ? 0 : real_size, alloc, key);
    uint32_t entry_len = 16 + keylen;
    entry_len = (entry_len + 7) & ~7u;

    // A nova entrada vai ANTES da entrada END (que e a ultima). Achamos o offset
    // do END dentro de 'entries' (a entrada com flag END).
    uint8_t* e = entries;
    uint8_t* end = node + total_used;
    uint8_t* end_entry = 0;
    while (e + 0x10 <= end) {
        uint16_t elen = rd16(e + 0x08);
        uint16_t efl  = rd16(e + 0x0C);
        if (elen < 0x10) break;
        if (efl & INDEX_ENTRY_END) { end_entry = e; break; }
        e += elen;
    }
    if (!end_entry) { kputs("[ntfs] create: $INDEX sem entrada END.\n"); kfree(rec); return -1; }
    uint16_t end_len = rd16(end_entry + 0x08);

    // Verifica folga no registro: precisamos de entry_len bytes a mais.
    uint32_t new_content_len = content_len + entry_len;
    uint32_t new_attr_len = (content_off + new_content_len + 7) & ~7u;
    if (attr_off + new_attr_len + 8 > s_vol.mft_record_size) {
        kputs("[ntfs] create: sem folga no $INDEX_ROOT do registro p/ a nova entrada.\n");
        kfree(rec); return -1;
    }

    // Desloca a entrada END para a frente, abrindo espaco p/ a nova entrada no
    // lugar onde o END estava.
    uint32_t end_ofs_in_root = (uint32_t)(end_entry - root);
    // move END (end_len bytes) p/ end_ofs + entry_len
    memmove(root + end_ofs_in_root + entry_len, root + end_ofs_in_root, end_len);
    // escreve a nova entrada onde o END estava
    uint8_t* ne = root + end_ofs_in_root;
    wr_le(ne + 0x00, child_ref, 8);
    wr_le(ne + 0x08, entry_len, 2);
    wr_le(ne + 0x0A, keylen, 2);
    wr_le(ne + 0x0C, 0x00, 4);            // flags (folha, nao-END)
    memcpy(ne + 0x10, key, keylen);
    // zera o padding da entrada (entre keylen e entry_len)
    if (entry_len > 16 + keylen) memset(ne + 0x10 + keylen, 0, entry_len - (16 + keylen));

    // Atualiza node header: total_used e total_alloc += entry_len.
    wr_le(node + 0x04, total_used + entry_len, 4);
    wr_le(node + 0x08, total_used + entry_len, 4);
    // Atualiza o atributo (content length + attr length).
    wr_le(ir + 0x10, new_content_len, 4);
    wr_le(ir + 0x04, new_attr_len, 4);
    // Reposiciona o END do registro + bytes used.
    uint32_t new_next = attr_off + new_attr_len;
    for (int i = 0; i < 4; i++) rec[new_next + i] = 0xFF;
    wr_le(rec + new_next + 4, 0, 4);
    uint32_t used = (new_next + 8 + 7) & ~7u;
    wr_le(rec + 0x18, used, 4);

    if (ntfs_write_mft_record(parent_record, rec) != 0) { kfree(rec); return -1; }
    kputs("[ntfs] create: entrada '"); kputs(name);
    kputs("' inserida no $INDEX_ROOT do pai #"); kput_dec(parent_record);
    kputs(" -> filho #"); kput_dec(child_ref & 0x0000FFFFFFFFFFFFull); kputs(".\n");
    kfree(rec);
    return 0;
}

// ----------------------------------------------------------------------------
//  Remove a entrada cujo ref == child_record do $INDEX_ROOT do pai (compacta as
//  entradas seguintes). $INDEX_ROOT residente e ultimo atributo. Retorna 0 ok.
// ----------------------------------------------------------------------------
static int ntfs_index_remove(uint64_t parent_record, uint64_t child_record) {
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_mft_record(parent_record, rec) != 0) { kfree(rec); return -1; }

    const uint8_t* ir_c = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 1);
    if (!ir_c) ir_c = ntfs_find_attr(rec, ATTR_INDEX_ROOT, 0, 0);
    if (!ir_c || attr_is_nonresident(ir_c)) { kfree(rec); return -1; }
    uint8_t* ir = (uint8_t*)ir_c;
    uint32_t attr_off = (uint32_t)(ir - rec);
    uint32_t attr_len = rd32(ir + 0x04);
    uint16_t content_off = rd16(ir + 0x14);
    uint32_t content_len = rd32(ir + 0x10);
    uint8_t* root = ir + content_off;
    uint8_t* node = root + 0x10;
    uint32_t entries_off = rd32(node + 0x00);
    uint32_t total_used  = rd32(node + 0x04);
    uint8_t* entries = node + entries_off;
    uint8_t* end = node + total_used;

    // Acha a entrada do filho.
    uint8_t* e = entries;
    uint8_t* hit = 0; uint16_t hit_len = 0;
    while (e + 0x10 <= end) {
        uint64_t ref  = rd64(e) & 0x0000FFFFFFFFFFFFull;
        uint16_t elen = rd16(e + 0x08);
        uint16_t efl  = rd16(e + 0x0C);
        if (elen < 0x10) break;
        if (efl & INDEX_ENTRY_END) break;
        if (ref == child_record) { hit = e; hit_len = elen; break; }
        e += elen;
    }
    if (!hit) {
        kputs("[ntfs] delete: filho #"); kput_dec(child_record);
        kputs(" nao esta no $INDEX do pai #"); kput_dec(parent_record); kputs(".\n");
        kfree(rec); return -1;
    }

    // Compacta: move tudo depois de 'hit' p/ cima (sobre 'hit'), incluindo o END.
    // total_used e relativo ao NODE HEADER (root + 0x10); o fim usado, relativo ao
    // root, e 0x10 + total_used. Movemos [tail_ofs .. node_used_end) sobre 'hit'.
    uint32_t hit_ofs = (uint32_t)(hit - root);
    uint32_t tail_ofs = hit_ofs + hit_len;
    uint32_t node_used_end = 0x10 + total_used;           // rel. ao root
    uint32_t move_len = node_used_end - tail_ofs;
    memmove(root + hit_ofs, root + tail_ofs, move_len);

    uint32_t new_total_used = total_used - hit_len;
    uint32_t new_content_len = content_len - hit_len;
    uint32_t new_attr_len = (content_off + new_content_len + 7) & ~7u;
    wr_le(node + 0x04, new_total_used, 4);
    wr_le(node + 0x08, new_total_used, 4);
    wr_le(ir + 0x10, new_content_len, 4);
    wr_le(ir + 0x04, new_attr_len, 4);
    // Se for o ultimo atributo, reposiciona END + bytes used (caminho da imagem).
    uint32_t old_next = attr_off + attr_len;
    if (old_next + 4 <= s_vol.mft_record_size && rd32(rec + old_next) == ATTR_END) {
        uint32_t new_next = attr_off + new_attr_len;
        for (int i = 0; i < 4; i++) rec[new_next + i] = 0xFF;
        wr_le(rec + new_next + 4, 0, 4);
        uint32_t used = (new_next + 8 + 7) & ~7u;
        wr_le(rec + 0x18, used, 4);
    }

    if (ntfs_write_mft_record(parent_record, rec) != 0) { kfree(rec); return -1; }
    kputs("[ntfs] delete: entrada do filho #"); kput_dec(child_record);
    kputs(" removida do $INDEX do pai #"); kput_dec(parent_record); kputs(".\n");
    kfree(rec);
    return 0;
}

// ----------------------------------------------------------------------------
//  ntfs_create_file — monta um registro MFT novo (FILE + $STD_INFO + $FILE_NAME +
//  $DATA/$INDEX_ROOT) num registro livre e o indexa no diretorio pai.
// ----------------------------------------------------------------------------
int ntfs_create_file(uint64_t parent_record, const char* name, int is_dir,
                     const void* content, uint32_t len, uint64_t* out_record) {
    if (!s_vol.mounted || !name) return 0;

    // 1) Acha um registro MFT livre (a partir de 27 na imagem de teste).
    uint64_t rno = ntfs_find_free_record(27, 64);
    if (rno == 0) { kputs("[ntfs] create: nenhum registro MFT livre encontrado.\n"); return 0; }
    kputs("[ntfs] create: usando registro MFT livre #"); kput_dec(rno);
    kputs(" p/ '"); kputs(name); kputs(is_dir ? "' (DIR)\n" : "' (FILE)\n");

    // 2) Monta o registro do zero.
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return 0;
    memset(rec, 0, s_vol.mft_record_size);
    uint32_t rs = s_vol.mft_record_size;
    uint16_t usa_off = 0x30;
    uint16_t usa_count = (uint16_t)(rs / s_vol.bytes_per_sector + 1);
    rec[0] = 'F'; rec[1] = 'I'; rec[2] = 'L'; rec[3] = 'E';
    wr_le(rec + 0x04, usa_off, 2);
    wr_le(rec + 0x06, usa_count, 2);
    wr_le(rec + 0x08, 0, 8);                 // $LogFile seq
    wr_le(rec + 0x10, 1, 2);                 // sequence number
    wr_le(rec + 0x12, 1, 2);                 // hard link count
    uint32_t attrs_off = (0x38 + (uint32_t)usa_count * 2 + 7) & ~7u;
    wr_le(rec + 0x14, attrs_off, 2);         // 1o atributo
    uint16_t flags = MFT_FLAG_IN_USE | (is_dir ? MFT_FLAG_DIRECTORY : 0);
    wr_le(rec + 0x16, flags, 2);
    wr_le(rec + 0x1C, rs, 4);                // bytes allocated
    wr_le(rec + 0x20, 0, 8);                 // base record ref
    wr_le(rec + 0x28, 4, 2);                 // next attr id (>= numero de atributos)
    wr_le(rec + 0x2C, (uint32_t)rno, 4);     // este record number
    // inicializa o USA (USN 1; bytes originais 0).
    wr_le(rec + usa_off, 1, 2);

    // 3) Atributos.
    uint32_t off = attrs_off;
    uint8_t tmp[640];
    uint32_t flags_si = is_dir ? FILE_ATTR_DIRECTORY : 0x20;
    // $STANDARD_INFORMATION
    {
        uint8_t cont[48]; uint32_t cl = build_stdinfo(flags_si, cont);
        uint32_t al = build_resident_attr(ATTR_STANDARD_INFORMATION, cont, cl, 0, tmp);
        memcpy(rec + off, tmp, al); off += al;
    }
    // $FILE_NAME (parent ref = pai, seq 1)
    {
        uint64_t parent_ref = (1ull << 48) | parent_record;
        uint8_t cont[320];
        uint64_t alloc = is_dir ? 0 : ((len + 7) & ~7u);
        uint32_t cl = build_filename_content(parent_ref, name, flags_si,
                                             is_dir ? 0 : len, alloc, cont);
        uint32_t al = build_resident_attr(ATTR_FILE_NAME, cont, cl, 0, tmp);
        memcpy(rec + off, tmp, al); off += al;
    }
    // $DATA (arquivo) ou $INDEX_ROOT (diretorio)
    if (is_dir) {
        uint8_t cont[64]; uint32_t cl = build_empty_index_root(cont);
        uint32_t al = build_resident_attr(ATTR_INDEX_ROOT, cont, cl, "$I30", tmp);
        memcpy(rec + off, tmp, al); off += al;
    } else {
        uint32_t al = build_resident_attr(ATTR_DATA, (const uint8_t*)content, len, 0, tmp);
        memcpy(rec + off, tmp, al); off += al;
    }
    // marcador de fim + bytes used
    wr_le(rec + off, ATTR_END, 4);
    wr_le(rec + off + 4, 0, 4);
    uint32_t used = (off + 8 + 7) & ~7u;
    wr_le(rec + 0x18, used, 4);

    // 4) Grava o novo registro (fixups reaplicados na escrita).
    if (ntfs_write_mft_record(rno, rec) != 0) { kfree(rec); return 0; }
    kfree(rec);

    // 5) Indexa no diretorio pai.
    uint64_t child_ref = (1ull << 48) | rno;     // seq 1
    if (ntfs_index_insert(parent_record, child_ref, name, is_dir, is_dir ? 0 : len) != 0) {
        kputs("[ntfs] create: registro gravado, mas a indexacao no pai falhou.\n");
        // Reverte: marca o registro como nao-em-uso (evita orfao visivel por scan).
        uint8_t* r2 = (uint8_t*)kmalloc(s_vol.mft_record_size);
        if (r2 && ntfs_read_mft_record(rno, r2) == 0) {
            uint16_t f = rd16(r2 + 0x16); wr_le(r2 + 0x16, f & ~MFT_FLAG_IN_USE, 2);
            ntfs_write_mft_record(rno, r2);
        }
        if (r2) kfree(r2);
        return 0;
    }

    if (out_record) *out_record = rno;
    kputs("[ntfs] create: '"); kputs(name); kputs("' criado como MFT #");
    kput_dec(rno); kputs(" e indexado no pai #"); kput_dec(parent_record); kputs(".\n");
    return 1;
}

// ----------------------------------------------------------------------------
//  ntfs_delete_file — marca o registro como nao-em-uso e remove do $INDEX do pai.
// ----------------------------------------------------------------------------
int ntfs_delete_file(uint64_t parent_record, uint64_t child_record) {
    if (!s_vol.mounted) return 0;

    // 1) Remove a entrada do $INDEX do pai (torna invisivel a listagem).
    int idx = ntfs_index_remove(parent_record, child_record);

    // 2) Marca o registro do filho como nao-em-uso (limpa o bit IN_USE).
    uint8_t* rec = (uint8_t*)kmalloc(s_vol.mft_record_size);
    if (!rec) return 0;
    if (ntfs_read_mft_record(child_record, rec) != 0) { kfree(rec); return 0; }
    uint16_t flags = rd16(rec + 0x16);
    wr_le(rec + 0x16, flags & ~MFT_FLAG_IN_USE, 2);
    int w = ntfs_write_mft_record(child_record, rec);
    kfree(rec);
    if (w == 0)
        kputs("[ntfs] delete: registro MFT #") , kput_dec(child_record),
        kputs(" marcado como NAO-em-uso.\n");

    return (idx == 0 && w == 0) ? 1 : 0;
}
