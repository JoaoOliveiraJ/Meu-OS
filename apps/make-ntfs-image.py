#!/usr/bin/env python3
# =============================================================================
#  make-ntfs-image.py  -  Gera uma imagem de disco RAW com UMA particao NTFS
#  minima, SEM precisar de privilegio de administrador (constroi os bytes na mao).
#
#  Por que isto existe: criar NTFS "de verdade" no Windows exige diskpart/New-VHD
#  + Format-Volume (admin). Quando nao da para elevar, este builder escreve
#  diretamente:
#    - MBR (setor 0) com 1 particao tipo 0x07 (NTFS) comecando em LBA 2048.
#    - Boot sector NTFS (VBR) com a assinatura OEM "NTFS    " no offset 3, um BPB
#      coerente (bytes/setor, setores/cluster, $MFT LCN) e a magic 0xAA55.
#    - Uma $MFT minima com os registros de metadados padrao ($MFT, $MFTMirr,
#      ...,. root '.') + um arquivo \hello.txt (conteudo RESIDENTE conhecido) e
#      um diretorio \dir1, indexados na raiz via $INDEX_ROOT.
#
#  O objetivo desta FASE e validar o BOOT SECTOR (assinatura NTFS) lido pelo
#  kernel via IDE PIO; a $MFT minima ja deixa o terreno pronto para a leitura
#  NTFS (montar BPB+MFT, ler $DATA residente de \hello.txt) das fases seguintes.
#
#  Uso:  python make-ntfs-image.py <saida.img> [tamanho_MiB]
#        (default: 64 MiB).  O conteudo conhecido de \hello.txt esta em HELLO_TXT.
# =============================================================================
import sys
import struct

SECTOR = 512
PART_START_LBA = 2048          # 1 MiB de alinhamento (padrao das ferramentas Windows)
BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 8        # cluster de 4 KiB (padrao NTFS p/ volumes pequenos)
CLUSTER = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER  # 4096
MFT_RECORD_SIZE = 1024         # tamanho de um registro da MFT (padrao NTFS)

# Conteudo CONHECIDO do arquivo de teste (o kernel valida lendo exatamente isto).
HELLO_TXT = b"Hello from MeuOS NTFS! Este arquivo foi lido do disco via IDE PIO.\r\n"
DIR1_FILE_TXT = b"Arquivo dentro de dir1.\r\n"

# ---- numeros de registro fixos da MFT (layout padrao do NTFS) ---------------
MFT_REC_MFT      = 0
MFT_REC_MFTMIRR  = 1
MFT_REC_LOGFILE  = 2
MFT_REC_VOLUME   = 3
MFT_REC_ATTRDEF  = 4
MFT_REC_ROOT     = 5    # diretorio raiz '.'
MFT_REC_BITMAP   = 6
MFT_REC_BOOT     = 7
MFT_REC_BADCLUS  = 8
MFT_REC_SECURE   = 9
MFT_REC_UPCASE   = 10
MFT_REC_EXTEND   = 11
MFT_REC_RESERVED_END = 24   # 0..23 reservados; arquivos do usuario a partir daqui
MFT_REC_HELLO    = 24       # \hello.txt
MFT_REC_DIR1     = 25       # \dir1
MFT_REC_DIR1FILE = 26       # \dir1\file.txt

# Tipos de atributo NTFS.
ATTR_STANDARD_INFORMATION = 0x10
ATTR_FILE_NAME            = 0x30
ATTR_DATA                 = 0x80
ATTR_INDEX_ROOT           = 0x90
ATTR_INDEX_ALLOCATION     = 0xA0
ATTR_BITMAP               = 0xB0

FILE_ATTR_READONLY   = 0x0001
FILE_ATTR_ARCHIVE    = 0x0020
FILE_ATTR_DIRECTORY  = 0x10000000   # flag de diretorio nos atributos NTFS

# Namespace do $FILE_NAME.
NS_WIN32_DOS = 3

# Tempo NTFS (100ns desde 1601). Valor fixo arbitrario (nao precisa ser real).
NT_TIME = 0x01D6000000000000


def fname_attr_bytes(parent_ref, name, flags, real_size, alloc_size):
    """Conteudo de um atributo $FILE_NAME (sem o cabecalho de atributo)."""
    name_utf16 = name.encode('utf-16-le')
    namelen = len(name)
    body = struct.pack('<Q', parent_ref)          # referencia do diretorio pai (6 bytes seq incl.)
    body += struct.pack('<Q', NT_TIME)            # creation
    body += struct.pack('<Q', NT_TIME)            # altered
    body += struct.pack('<Q', NT_TIME)            # mft changed
    body += struct.pack('<Q', NT_TIME)            # read
    body += struct.pack('<Q', alloc_size)         # allocated size
    body += struct.pack('<Q', real_size)          # real size
    body += struct.pack('<I', flags)              # file attributes
    body += struct.pack('<I', 0)                  # EA/reparse
    body += struct.pack('<B', namelen)            # comprimento do nome (chars)
    body += struct.pack('<B', NS_WIN32_DOS)       # namespace
    body += name_utf16
    return body


def std_info_bytes(flags):
    """Conteudo de um atributo $STANDARD_INFORMATION (versao curta, 48 bytes)."""
    return (struct.pack('<Q', NT_TIME) * 4 +      # created/altered/mftchg/read
            struct.pack('<I', flags) +            # dos permissions
            struct.pack('<I', 0) +                # max versions
            struct.pack('<I', 0) +                # version
            struct.pack('<I', 0))                 # class id


def make_resident_attr(attr_type, content, name=b'', flags=0, attr_id=0):
    """Monta um atributo RESIDENTE completo (cabecalho + conteudo)."""
    name_len = len(name) // 2  # name em UTF-16LE
    # cabecalho residente tem 24 bytes; o conteudo comeca alinhado em 8.
    content_off = 24 + (len(name))
    content_off = (content_off + 7) & ~7
    hdr = struct.pack('<I', attr_type)
    rec_len = content_off + len(content)
    rec_len = (rec_len + 7) & ~7
    hdr += struct.pack('<I', rec_len)             # comprimento total do atributo
    hdr += struct.pack('<B', 0)                   # non-resident flag = 0
    hdr += struct.pack('<B', name_len)            # name length (chars)
    hdr += struct.pack('<H', 24 if name else 0)   # name offset
    hdr += struct.pack('<H', flags)               # flags
    hdr += struct.pack('<H', attr_id)             # attribute id
    hdr += struct.pack('<I', len(content))        # content length
    hdr += struct.pack('<H', content_off)         # content offset
    hdr += struct.pack('<B', 0)                    # indexed flag
    hdr += struct.pack('<B', 0)                    # padding
    if name:
        hdr += name
    # padding ate content_off
    hdr += b'\x00' * (content_off - len(hdr))
    out = hdr + content
    out += b'\x00' * (rec_len - len(out))
    return out


def make_index_root_empty(indexed_attr_type, collation):
    """$INDEX_ROOT 'vazio' (so com o no final). Indexa $FILE_NAME (0x30)."""
    # Index root header (16 bytes) + index node header (16 bytes) + entrada final.
    end_entry = struct.pack('<Q', 0)              # MFT ref = 0 (entrada final)
    end_entry += struct.pack('<H', 16)            # length da entrada
    end_entry += struct.pack('<H', 0)             # key length
    end_entry += struct.pack('<I', 0x02)          # flags: ultimo no (END)
    node = struct.pack('<I', 16)                  # offset p/ 1a entrada (rel ao node hdr)
    node += struct.pack('<I', 16 + len(end_entry))# total usado
    node += struct.pack('<I', 16 + len(end_entry))# total alocado
    node += struct.pack('<I', 0)                  # flags: 0 = no folha (sem child)
    node += end_entry
    root = struct.pack('<I', indexed_attr_type)   # tipo do atributo indexado (0x30)
    root += struct.pack('<I', collation)          # collation rule
    root += struct.pack('<I', CLUSTER)            # bytes por index record
    root += struct.pack('<B', 1)                  # clusters por index record
    root += b'\x00' * 3                           # padding
    root += node
    return root


def mft_record(record_no, attrs, is_dir=False, in_use=True, base_ref=0,
               seq=1, link_count=1):
    """Monta um registro completo da MFT (1024 bytes) com fixups (USA)."""
    rec = bytearray(MFT_RECORD_SIZE)
    # --- cabecalho FILE ---
    struct.pack_into('<4s', rec, 0, b'FILE')
    usa_off = 0x30
    usa_count = (MFT_RECORD_SIZE // SECTOR) + 1   # 1 USN + 1 por setor
    struct.pack_into('<H', rec, 0x04, usa_off)    # offset do Update Sequence Array
    struct.pack_into('<H', rec, 0x06, usa_count)  # count
    struct.pack_into('<Q', rec, 0x08, 0)          # $LogFile sequence number
    struct.pack_into('<H', rec, 0x10, seq)        # sequence number
    struct.pack_into('<H', rec, 0x12, link_count) # hard link count
    attrs_off = 0x38 + usa_count * 2
    attrs_off = (attrs_off + 7) & ~7
    struct.pack_into('<H', rec, 0x14, attrs_off)  # offset do 1o atributo
    flags = 0x01 if in_use else 0x00              # 0x01 = em uso
    if is_dir:
        flags |= 0x02                             # 0x02 = diretorio
    struct.pack_into('<H', rec, 0x16, flags)
    # bytes used / allocated preenchidos no fim.
    struct.pack_into('<Q', rec, 0x20, base_ref)   # base file record ref
    struct.pack_into('<H', rec, 0x28, record_no + 1)  # next attribute id
    struct.pack_into('<I', rec, 0x2C, record_no)  # numero deste registro (MFT record number)

    # --- atributos ---
    off = attrs_off
    for a in attrs:
        rec[off:off + len(a)] = a
        off += len(a)
    # marcador de fim (0xFFFFFFFF)
    struct.pack_into('<I', rec, off, 0xFFFFFFFF)
    off += 8
    used = (off + 7) & ~7
    struct.pack_into('<I', rec, 0x18, used)               # bytes used
    struct.pack_into('<I', rec, 0x1C, MFT_RECORD_SIZE)    # bytes allocated

    # --- Update Sequence Array (USA/fixups) ---
    # USN em usa_off; depois 1 valor por setor. Grava o USN nos 2 ultimos bytes
    # de cada setor e guarda os bytes originais no array (protocolo NTFS).
    usn = 0x0001
    struct.pack_into('<H', rec, usa_off, usn)
    for i in range(usa_count - 1):
        sec_end = (i + 1) * SECTOR - 2
        orig = bytes(rec[sec_end:sec_end + 2])
        rec[usa_off + 2 + i * 2:usa_off + 2 + i * 2 + 2] = orig
        struct.pack_into('<H', rec, sec_end, usn)
    return bytes(rec)


def file_ref(rec_no, seq=1):
    return (seq << 48) | rec_no


def build_mft():
    """Constroi a tabela de registros da MFT (lista de bytes de 1024)."""
    records = {}

    def add_basic(no, name, parent, content, is_dir=False, flags=FILE_ATTR_ARCHIVE):
        attrs = [make_resident_attr(ATTR_STANDARD_INFORMATION, std_info_bytes(flags))]
        if name is not None:
            real = len(content) if content is not None else 0
            alloc = (real + 7) & ~7
            attrs.append(make_resident_attr(
                ATTR_FILE_NAME,
                fname_attr_bytes(file_ref(parent), name, flags, real, alloc),
                flags=0))
        if is_dir:
            attrs.append(make_resident_attr(ATTR_INDEX_ROOT,
                         make_index_root_empty(ATTR_FILE_NAME, 0x01),
                         name='$I30'.encode('utf-16-le')))
        else:
            attrs.append(make_resident_attr(ATTR_DATA, content if content else b''))
        records[no] = mft_record(no, attrs, is_dir=is_dir)

    # Registros de metadados (conteudo simbolico; o que importa e existirem).
    add_basic(MFT_REC_MFT, '$MFT', MFT_REC_ROOT, b'', is_dir=False)
    add_basic(MFT_REC_MFTMIRR, '$MFTMirr', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_LOGFILE, '$LogFile', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_VOLUME, '$Volume', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_ATTRDEF, '$AttrDef', MFT_REC_ROOT, b'')

    # Diretorio raiz '.' (record 5): $INDEX_ROOT lista hello.txt e dir1.
    root_index = build_root_index([
        (MFT_REC_HELLO, 'hello.txt', FILE_ATTR_ARCHIVE, len(HELLO_TXT)),
        (MFT_REC_DIR1, 'dir1', FILE_ATTR_DIRECTORY, 0),
    ])
    root_attrs = [
        make_resident_attr(ATTR_STANDARD_INFORMATION,
                           std_info_bytes(FILE_ATTR_DIRECTORY)),
        make_resident_attr(ATTR_FILE_NAME,
                           fname_attr_bytes(file_ref(MFT_REC_ROOT), '.',
                                            FILE_ATTR_DIRECTORY, 0, 0)),
        make_resident_attr(ATTR_INDEX_ROOT, root_index,
                           name='$I30'.encode('utf-16-le')),
    ]
    records[MFT_REC_ROOT] = mft_record(MFT_REC_ROOT, root_attrs, is_dir=True,
                                       link_count=1)

    add_basic(MFT_REC_BITMAP, '$Bitmap', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_BOOT, '$Boot', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_BADCLUS, '$BadClus', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_SECURE, '$Secure', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_UPCASE, '$UpCase', MFT_REC_ROOT, b'')
    add_basic(MFT_REC_EXTEND, '$Extend', MFT_REC_ROOT, b'', is_dir=True)

    # \hello.txt (record 24): $DATA residente com o conteudo CONHECIDO.
    records[MFT_REC_HELLO] = mft_record(MFT_REC_HELLO, [
        make_resident_attr(ATTR_STANDARD_INFORMATION,
                           std_info_bytes(FILE_ATTR_ARCHIVE)),
        make_resident_attr(ATTR_FILE_NAME,
                           fname_attr_bytes(file_ref(MFT_REC_ROOT), 'hello.txt',
                                            FILE_ATTR_ARCHIVE, len(HELLO_TXT),
                                            (len(HELLO_TXT) + 7) & ~7)),
        make_resident_attr(ATTR_DATA, HELLO_TXT),
    ], seq=1)

    # \dir1 (record 25): diretorio com $INDEX_ROOT listando file.txt.
    dir1_index = build_root_index([
        (MFT_REC_DIR1FILE, 'file.txt', FILE_ATTR_ARCHIVE, len(DIR1_FILE_TXT)),
    ])
    records[MFT_REC_DIR1] = mft_record(MFT_REC_DIR1, [
        make_resident_attr(ATTR_STANDARD_INFORMATION,
                           std_info_bytes(FILE_ATTR_DIRECTORY)),
        make_resident_attr(ATTR_FILE_NAME,
                           fname_attr_bytes(file_ref(MFT_REC_ROOT), 'dir1',
                                            FILE_ATTR_DIRECTORY, 0, 0)),
        make_resident_attr(ATTR_INDEX_ROOT, dir1_index,
                           name='$I30'.encode('utf-16-le')),
    ], is_dir=True)

    # \dir1\file.txt (record 26): $DATA residente.
    records[MFT_REC_DIR1FILE] = mft_record(MFT_REC_DIR1FILE, [
        make_resident_attr(ATTR_STANDARD_INFORMATION,
                           std_info_bytes(FILE_ATTR_ARCHIVE)),
        make_resident_attr(ATTR_FILE_NAME,
                           fname_attr_bytes(file_ref(MFT_REC_DIR1), 'file.txt',
                                            FILE_ATTR_ARCHIVE, len(DIR1_FILE_TXT),
                                            (len(DIR1_FILE_TXT) + 7) & ~7)),
        make_resident_attr(ATTR_DATA, DIR1_FILE_TXT),
    ])

    # Monta a MFT como um array continuo (registros 0..26; buracos = zeros).
    max_rec = max(records.keys())
    mft = bytearray()
    for i in range(max_rec + 1):
        if i in records:
            mft += records[i]
        else:
            # registro 'nao em uso' valido (FILE header sem flag de uso).
            mft += mft_record(i, [], in_use=False)
    return bytes(mft)


def build_root_index(entries):
    """$INDEX_ROOT com entradas de $FILE_NAME (lista de (ref, nome, flags, size))."""
    index_entries = b''
    for rec_no, name, flags, size in entries:
        # A chave do indice e um $FILE_NAME cujo parent e a raiz:
        key = fname_attr_bytes(file_ref(MFT_REC_ROOT), name, flags, size,
                               (size + 7) & ~7)
        entry_len = 16 + len(key)
        entry_len = (entry_len + 7) & ~7
        e = struct.pack('<Q', file_ref(rec_no))   # MFT ref do filho
        e += struct.pack('<H', entry_len)         # length
        e += struct.pack('<H', len(key))          # key length
        e += struct.pack('<I', 0x00)              # flags
        e += key
        e += b'\x00' * (entry_len - len(e))
        index_entries += e
    # entrada final (END)
    end = struct.pack('<Q', 0) + struct.pack('<H', 16) + struct.pack('<H', 0) + \
          struct.pack('<I', 0x02)
    index_entries += end

    node = struct.pack('<I', 16)
    node += struct.pack('<I', 16 + len(index_entries))
    node += struct.pack('<I', 16 + len(index_entries))
    node += struct.pack('<I', 0)
    node += index_entries

    root = struct.pack('<I', ATTR_FILE_NAME)
    root += struct.pack('<I', 0x01)               # collation: filename
    root += struct.pack('<I', CLUSTER)
    root += struct.pack('<B', 1)
    root += b'\x00' * 3
    root += node
    return root


def build_boot_sector(total_sectors, mft_lcn, mftmirr_lcn):
    """Boot sector (VBR) NTFS de 512 bytes com BPB + assinatura."""
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x52\x90'             # jump
    bs[3:11] = b'NTFS    '                # OEM ID (a assinatura que o kernel valida)
    struct.pack_into('<H', bs, 0x0B, BYTES_PER_SECTOR)
    bs[0x0D] = SECTORS_PER_CLUSTER
    # reserved sectors / FATs / root entries / etc = 0 no NTFS.
    bs[0x15] = 0xF8                       # media descriptor (disco fixo)
    struct.pack_into('<H', bs, 0x18, 63)             # sectors per track
    struct.pack_into('<H', bs, 0x1A, 255)            # heads
    struct.pack_into('<I', bs, 0x1C, PART_START_LBA) # hidden sectors (offset da particao)
    # total sectors (32-bit) = 0 no NTFS; usa o campo de 64 bits abaixo.
    struct.pack_into('<I', bs, 0x24, 0x00800080)     # drive number + flags (estilo NTFS)
    struct.pack_into('<Q', bs, 0x28, total_sectors)  # total sectors (64-bit)
    struct.pack_into('<Q', bs, 0x30, mft_lcn)        # $MFT LCN
    struct.pack_into('<Q', bs, 0x38, mftmirr_lcn)    # $MFTMirr LCN
    # clusters per MFT record: valor negativo = 2^|v| bytes. -10 => 1024 bytes.
    struct.pack_into('<b', bs, 0x40, -10)
    struct.pack_into('<b', bs, 0x44, 1)              # clusters per index buffer
    struct.pack_into('<Q', bs, 0x48, 0x1234567890ABCDEF)  # volume serial
    bs[510] = 0x55
    bs[511] = 0xAA
    return bytes(bs)


def build_mbr(part_start, part_sectors):
    """MBR (setor 0) com 1 particao NTFS (type 0x07)."""
    mbr = bytearray(SECTOR)
    # Entrada de particao 0 no offset 0x1BE.
    e = 0x1BE
    mbr[e + 0] = 0x80                     # bootable
    # CHS de inicio (irrelevante p/ LBA; valores tipicos).
    mbr[e + 1] = 0x20
    mbr[e + 2] = 0x21
    mbr[e + 3] = 0x00
    mbr[e + 4] = 0x07                     # type 0x07 = NTFS/exFAT
    mbr[e + 5] = 0xFE                     # CHS de fim (max)
    mbr[e + 6] = 0xFF
    mbr[e + 7] = 0xFF
    struct.pack_into('<I', mbr, e + 8, part_start)      # LBA inicial
    struct.pack_into('<I', mbr, e + 12, part_sectors)   # numero de setores
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def main():
    if len(sys.argv) < 2:
        print("uso: python make-ntfs-image.py <saida.img> [tamanho_MiB]")
        sys.exit(2)
    out_path = sys.argv[1]
    size_mib = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    total_img_sectors = size_mib * 1024 * 1024 // SECTOR
    part_sectors = total_img_sectors - PART_START_LBA

    # Layout do volume NTFS (em clusters de 4 KiB), dentro da particao:
    #   cluster 0      : boot sector (VBR) + BPB
    #   $MFT em LCN 4  : tabela de registros (escolha simples e baixa)
    mft_lcn = 4
    mftmirr_lcn = 8
    part_clusters = part_sectors // SECTORS_PER_CLUSTER

    img = bytearray(total_img_sectors * SECTOR)

    # 1) MBR.
    img[0:SECTOR] = build_mbr(PART_START_LBA, part_sectors)

    # 2) Boot sector NTFS no inicio da particao.
    vbr = build_boot_sector(part_sectors, mft_lcn, mftmirr_lcn)
    part_off = PART_START_LBA * SECTOR
    img[part_off:part_off + SECTOR] = vbr
    # Copia de backup do VBR no ULTIMO setor da particao (padrao NTFS).
    backup_off = part_off + (part_sectors - 1) * SECTOR
    img[backup_off:backup_off + SECTOR] = vbr

    # 3) $MFT no LCN escolhido.
    mft = build_mft()
    mft_off = part_off + mft_lcn * CLUSTER
    img[mft_off:mft_off + len(mft)] = mft
    # $MFTMirr: copia dos 4 primeiros registros.
    mftmirr_off = part_off + mftmirr_lcn * CLUSTER
    img[mftmirr_off:mftmirr_off + 4 * MFT_RECORD_SIZE] = mft[:4 * MFT_RECORD_SIZE]

    with open(out_path, 'wb') as f:
        f.write(img)

    print("[make-ntfs] imagem RAW gerada: %s" % out_path)
    print("[make-ntfs]   tamanho: %d MiB (%d setores de %d bytes)" %
          (size_mib, total_img_sectors, SECTOR))
    print("[make-ntfs]   particao NTFS (type 0x07): LBA %d, %d setores, %d clusters" %
          (PART_START_LBA, part_sectors, part_clusters))
    print("[make-ntfs]   boot sector OEM='NTFS    ' @offset 3, magic 0xAA55 @510")
    print("[make-ntfs]   $MFT em LCN %d (offset particao 0x%X); bytes/cluster=%d" %
          (mft_lcn, mft_lcn * CLUSTER, CLUSTER))
    print("[make-ntfs]   arquivos: \\hello.txt (%d bytes, residente), \\dir1\\file.txt (%d bytes)" %
          (len(HELLO_TXT), len(DIR1_FILE_TXT)))
    print("[make-ntfs]   conteudo conhecido de \\hello.txt: %r" % HELLO_TXT)


if __name__ == '__main__':
    main()
