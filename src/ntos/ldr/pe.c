#include <stdint.h>
#include "ldr/pe.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

typedef struct __attribute__((packed)) { uint32_t rva, size; } datadir_t;

static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ---------------------------------------------------------------------------
// pe_info: le os campos do cabecalho PE de forma INDEPENDENTE da bitness.
// PE32 (magic 0x10B) e PE32+ (magic 0x20B) tem layouts diferentes no optional
// header (ImageBase 4 vs 8 bytes; BaseOfData so existe no PE32; o array de
// data directories comeca em 96 no PE32 e 112 no PE32+). Centralizamos isso
// aqui para que pe_map/pe_bind_imports/pe_relocate sirvam aos dois formatos.
// ---------------------------------------------------------------------------
int pe_parse(const void* image, pe_info_t* pi) {
    const uint8_t* f = (const uint8_t*)image;
    for (int i = 0; i < (int)sizeof(*pi); i++) ((uint8_t*)pi)[i] = 0;
    if (f[0] != 'M' || f[1] != 'Z') return 0;
    uint32_t e = *(const uint32_t*)(f + 0x3C);
    const uint8_t* nt = f + e;
    if (!(nt[0] == 'P' && nt[1] == 'E' && nt[2] == 0 && nt[3] == 0)) return 0;

    const uint8_t* coff = nt + 4;
    pi->machine = *(const uint16_t*)(coff + 0);
    pi->nsec    = *(const uint16_t*)(coff + 2);
    uint16_t optsz = *(const uint16_t*)(coff + 16);
    const uint8_t* opt = coff + 20;
    pi->magic = *(const uint16_t*)(opt + 0);

    pi->entry_rva = *(const uint32_t*)(opt + 16);

    if (pi->magic == 0x20B) {                 // PE32+ (64-bit)
        pi->is64        = 1;
        pi->preferred   = *(const uint64_t*)(opt + 24);
        pi->size_image  = *(const uint32_t*)(opt + 56);
        pi->size_hdrs   = *(const uint32_t*)(opt + 60);
        pi->ndirs       = *(const uint32_t*)(opt + 108);
        pi->dir_off     = 112;
    } else if (pi->magic == 0x10B) {          // PE32 (32-bit)
        pi->is64        = 0;
        pi->preferred   = *(const uint32_t*)(opt + 28);
        pi->size_image  = *(const uint32_t*)(opt + 56);
        pi->size_hdrs   = *(const uint32_t*)(opt + 60);
        pi->ndirs       = *(const uint32_t*)(opt + 92);
        pi->dir_off     = 96;
    } else {
        return 0;                              // magic desconhecido
    }

    pi->sec_off = (uint32_t)((opt + optsz) - f);  // offset das section headers no arquivo
    return 1;
}

int pe_subsystem(const void* image) {
    pe_info_t pi;
    if (!pe_parse(image, &pi)) return -1;
    // Subsystem fica no offset 68 do optional header nos DOIS formatos: no PE32 a
    // ImageBase tem 4 bytes mas o campo BaseOfData (so existe no PE32) adiciona
    // outros 4, alinhando os campos seguintes ao mesmo offset do PE32+.
    const uint8_t* f = (const uint8_t*)image;
    uint32_t e = *(const uint32_t*)(f + 0x3C);
    return *(const uint16_t*)(f + e + 4 + 20 + 68);
}

// Acessa um data directory (rva/size) de uma imagem ja mapeada na base 'b'.
static datadir_t get_dir(uint8_t* b, uint32_t idx) {
    datadir_t empty = { 0, 0 };
    uint32_t e = *(uint32_t*)(b + 0x3C);
    uint8_t* opt = b + e + 4 + 20;
    uint16_t magic = *(uint16_t*)(opt + 0);
    uint32_t ndirs, diroff;
    if (magic == 0x20B) { ndirs = *(uint32_t*)(opt + 108); diroff = 112; }
    else                { ndirs = *(uint32_t*)(opt + 92);  diroff = 96;  }
    if (idx >= ndirs) return empty;
    datadir_t* dirs = (datadir_t*)(opt + diroff);
    return dirs[idx];
}

// Mapeia headers + secoes na base dada (default = ImageBase preferido).
// Funciona para PE32 e PE32+. *entry_out recebe o entry point ja na base usada.
void* pe_map_at(const void* image, uint64_t base, void** entry_out) {
    if (entry_out) *entry_out = 0;
    pe_info_t pi;
    if (!pe_parse(image, &pi)) { kputs("[PE] imagem invalida\n"); return 0; }

    const uint8_t* f = (const uint8_t*)image;
    uint8_t* img = (uint8_t*)(uintptr_t)base;
    for (uint32_t i = 0; i < pi.size_image; i++) img[i] = 0;
    for (uint32_t i = 0; i < pi.size_hdrs;  i++) img[i] = f[i];

    const uint8_t* sh = f + pi.sec_off;
    for (uint16_t s = 0; s < pi.nsec; s++, sh += 40) {
        uint32_t va = *(const uint32_t*)(sh + 12);
        uint32_t rs = *(const uint32_t*)(sh + 16);
        uint32_t rp = *(const uint32_t*)(sh + 20);
        for (uint32_t i = 0; i < rs; i++) img[va + i] = f[rp + i];
    }

    if (entry_out) *entry_out = (void*)(uintptr_t)(img + pi.entry_rva);
    return img;
}

// Compatibilidade: mapeia no ImageBase preferido (caminho 64-bit existente).
void* pe_map(const void* image, void** entry_out) {
    pe_info_t pi;
    if (!pe_parse(image, &pi)) { kputs("[PE] imagem invalida\n"); return 0; }
    return pe_map_at(image, pi.preferred, entry_out);
}

// ---------------------------------------------------------------------------
// Relocacoes (base relocations, .reloc). Aplica quando a base efetiva difere
// da preferida. delta = base_usada - preferida. Trata:
//   IMAGE_REL_BASED_ABSOLUTE (0): padding, ignora.
//   IMAGE_REL_BASED_HIGHLOW  (3): patch de 32 bits (PE32).
//   IMAGE_REL_BASED_DIR64   (10): patch de 64 bits (PE32+).
// Retorna o numero de fixups aplicados.
uint32_t pe_relocate(void* base, uint64_t preferred) {
    uint8_t* b = (uint8_t*)base;
    int64_t delta = (int64_t)((uint64_t)(uintptr_t)b - preferred);
    if (delta == 0) return 0;                 // carregado no lugar preferido

    datadir_t reloc = get_dir(b, 5);          // IMAGE_DIRECTORY_ENTRY_BASERELOC
    if (!reloc.rva || !reloc.size) return 0;

    uint8_t* p   = b + reloc.rva;
    uint8_t* end = p + reloc.size;
    uint32_t applied = 0;
    while (p < end) {
        uint32_t page_rva  = *(uint32_t*)(p + 0);
        uint32_t block_sz  = *(uint32_t*)(p + 4);
        if (block_sz < 8) break;
        uint32_t nentries = (block_sz - 8) / 2;
        uint16_t* ents = (uint16_t*)(p + 8);
        for (uint32_t i = 0; i < nentries; i++) {
            uint16_t type   = ents[i] >> 12;
            uint16_t offset = ents[i] & 0x0FFF;
            uint8_t* target = b + page_rva + offset;
            if (type == 3) {                  // HIGHLOW (32-bit)
                *(uint32_t*)target = (uint32_t)(*(uint32_t*)target + (int32_t)delta);
                applied++;
            } else if (type == 10) {          // DIR64 (64-bit)
                *(uint64_t*)target = (uint64_t)(*(uint64_t*)target + delta);
                applied++;
            }
            // type 0 (ABSOLUTE) e outros: ignorados.
        }
        p += block_sz;
    }
    return applied;
}

// ---------------------------------------------------------------------------
// Imports (IAT). Os thunks tem 8 bytes no PE32+ e 4 bytes no PE32; o bit de
// "import by ordinal" e o 63 (PE32+) ou o 31 (PE32). O hint/name aponta para
// uma estrutura {hint:2}{name}. O resolver devolve o endereco da funcao.
// ---------------------------------------------------------------------------
// Formata "#<decimal>" (import por ORDINAL) num buffer >= 8 bytes.
static void fmt_ord(char* buf, uint32_t ord) {
    buf[0] = '#'; char tmp[8]; int t = 0;
    if (ord == 0) tmp[t++] = '0';
    while (ord) { tmp[t++] = (char)('0' + ord % 10); ord /= 10; }
    int q = 1; while (t > 0) buf[q++] = tmp[--t]; buf[q] = 0;
}

void pe_bind_imports(void* base, pe_resolver_t resolve) {
    uint8_t* b = (uint8_t*)base;
    int is64 = (*(uint16_t*)(b + *(uint32_t*)(b + 0x3C) + 4 + 20) == 0x20B);

    datadir_t imp_dir = get_dir(b, 1);        // IMAGE_DIRECTORY_ENTRY_IMPORT
    if (!imp_dir.rva) return;

    uint8_t* imp = b + imp_dir.rva;
    for (;; imp += 20) {
        uint32_t oft  = *(uint32_t*)(imp + 0);
        uint32_t name = *(uint32_t*)(imp + 12);
        uint32_t ft   = *(uint32_t*)(imp + 16);
        if (oft == 0 && name == 0 && ft == 0) break;

        const char* dll = (const char*)(b + name);
        uint32_t thunk_rva = oft ? oft : ft;

        if (is64) {
            uint64_t* ilt = (uint64_t*)(b + thunk_rva);
            uint64_t* iat = (uint64_t*)(b + ft);
            for (uint32_t k = 0; ilt[k]; k++) {
                uint64_t t = ilt[k];
                void* fn = 0;
                char ordbuf[8];
                const char* fname;
                if (t & (1ULL << 63)) {                 // import por ORDINAL
                    fmt_ord(ordbuf, (uint32_t)(t & 0xFFFF));
                    fname = ordbuf;
                } else {
                    fname = (const char*)(b + (uint32_t)t + 2);
                }
                fn = resolve(dll, fname);
                if (!fn) {
                    kputs("[ldr] import nao resolvido: "); kputs(dll);
                    kputs("!"); kputs(fname); kputc('\n');
                }
                iat[k] = (uint64_t)(uintptr_t)fn;
            }
        } else {
            uint32_t* ilt = (uint32_t*)(b + thunk_rva);
            uint32_t* iat = (uint32_t*)(b + ft);
            for (uint32_t k = 0; ilt[k]; k++) {
                uint32_t t = ilt[k];
                void* fn = 0;
                char ordbuf[8];
                const char* fname;
                if (t & (1UL << 31)) {                  // import por ORDINAL
                    fmt_ord(ordbuf, (uint32_t)(t & 0xFFFF));
                    fname = ordbuf;
                } else {
                    fname = (const char*)(b + (t & 0x7FFFFFFF) + 2);
                }
                fn = resolve(dll, fname);
                if (!fn) {
                    kputs("[ldr] import nao resolvido: "); kputs(dll);
                    kputs("!"); kputs(fname); kputc('\n');
                }
                // O ponteiro de 32 bits precisa caber na faixa baixa (identidade);
                // as DLLs/kernel vivem abaixo de 4 GiB, entao o truncamento e seguro.
                iat[k] = (uint32_t)(uintptr_t)fn;
            }
        }
    }

    // Delay-load imports (data dir 13, secao .didat). Sem isto, o thunk de delay-load
    // chama ResolveDelayLoadedAPI (stub que devolveria 0) -> slot=0 -> CALL[0] -> rip=0.
    // Resolvemos EAGERLY com o mesmo resolver (nome + ORDINAL); so sobrescrevemos o slot
    // quando resolve (senao mantem o thunk original). So descritores RVA-based (bit 0).
    if (is64) {
        datadir_t del = get_dir(b, 13);
        if (del.rva) {
            // Fallback p/ imports de delay-load NAO resolvidos (DLL opcional ausente):
            // aponta o slot p/ o no-op ntdll!LdrpNullStub (devolve 0) em vez de deixar o
            // thunk -> ResolveDelayLoadedAPI -> 0 -> rip=0. Sem SEH, e' a degradacao segura.
            void* nullstub = resolve("ntdll.dll", "LdrpNullStub");
            uint8_t* dp = b + del.rva;
            for (;; dp += 32) {
                uint32_t attrs   = *(uint32_t*)(dp + 0);
                uint32_t nameRva = *(uint32_t*)(dp + 4);
                uint32_t iatRva  = *(uint32_t*)(dp + 12);
                uint32_t intRva  = *(uint32_t*)(dp + 16);
                if (nameRva == 0 && iatRva == 0) break;
                if (!(attrs & 1) || !intRva || !iatRva) continue;   // so RVA-based
                const char* dll = (const char*)(b + nameRva);
                uint64_t* iat = (uint64_t*)(b + iatRva);
                uint64_t* intt = (uint64_t*)(b + intRva);
                for (uint32_t k = 0; intt[k]; k++) {
                    uint64_t t = intt[k];
                    char ordbuf[8];
                    const char* fname;
                    if (t & (1ULL << 63)) { fmt_ord(ordbuf, (uint32_t)(t & 0xFFFF)); fname = ordbuf; }
                    else                  { fname = (const char*)(b + (uint32_t)t + 2); }
                    void* fn = resolve(dll, fname);
                    if (!fn) fn = nullstub;                     // opcional ausente -> no-op (nao crasha)
                    if (fn) iat[k] = (uint64_t)(uintptr_t)fn;
                }
            }
        }
    }
}

void* pe_get_export(void* base, const char* name) {
    uint8_t* b = (uint8_t*)base;
    datadir_t exp_dir = get_dir(b, 0);        // IMAGE_DIRECTORY_ENTRY_EXPORT
    if (!exp_dir.rva) return 0;

    uint8_t* ex = b + exp_dir.rva;
    uint32_t nnames = *(uint32_t*)(ex + 0x18);
    uint32_t* funcs = (uint32_t*)(b + *(uint32_t*)(ex + 0x1C));
    uint32_t* names = (uint32_t*)(b + *(uint32_t*)(ex + 0x20));
    uint16_t* ords  = (uint16_t*)(b + *(uint32_t*)(ex + 0x24));

    for (uint32_t i = 0; i < nnames; i++) {
        if (streq((const char*)(b + names[i]), name))
            return (void*)(uintptr_t)(b + funcs[ords[i]]);
    }
    return 0;
}

// Resolve um export por ORDINAL (imports #N — ex.: shell32 via api-ms-win-shell-*).
// AddressOfFunctions e' indexado por (ordinal - Base). RVA 0 = slot vazio.
void* pe_get_export_by_ordinal(void* base, uint32_t ordinal) {
    uint8_t* b = (uint8_t*)base;
    datadir_t exp_dir = get_dir(b, 0);        // IMAGE_DIRECTORY_ENTRY_EXPORT
    if (!exp_dir.rva) return 0;
    uint8_t* ex = b + exp_dir.rva;
    uint32_t ord_base = *(uint32_t*)(ex + 0x10);   // Base
    uint32_t nfuncs   = *(uint32_t*)(ex + 0x14);   // NumberOfFunctions
    uint32_t* funcs   = (uint32_t*)(b + *(uint32_t*)(ex + 0x1C));
    if (ordinal < ord_base) return 0;
    uint32_t idx = ordinal - ord_base;
    if (idx >= nfuncs) return 0;
    uint32_t rva = funcs[idx];
    if (!rva) return 0;
    return (void*)(uintptr_t)(b + rva);
}
