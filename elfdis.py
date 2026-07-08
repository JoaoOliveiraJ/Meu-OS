#!/usr/bin/env python
# elfdis.py <symbol_substr> : acha um simbolo no build\kernel.elf (ELF64) e desmonta
# ~120 bytes a partir dele. Usado p/ inspecionar o codigo GERADO de rotinas asm inline
# (ex.: enter_ring3_arg) — a fonte da verdade quando a alocacao de registrador do GCC
# importa. So-leitura; nao altera nada.
import struct, sys, capstone
elf = open(r'build/kernel.elf', 'rb').read()
assert elf[:4] == b'\x7fELF'
is64 = elf[4] == 2
e_shoff = struct.unpack_from('<Q', elf, 0x28)[0]
e_shentsize = struct.unpack_from('<H', elf, 0x3a)[0]
e_shnum = struct.unpack_from('<H', elf, 0x3c)[0]
e_shstrndx = struct.unpack_from('<H', elf, 0x3e)[0]
secs = []
for i in range(e_shnum):
    b = e_shoff + i * e_shentsize
    name, typ, flags, addr, off, size, link, info, align, entsz = struct.unpack_from('<IIQQQQIIQQ', elf, b)
    secs.append(dict(name=name, typ=typ, addr=addr, off=off, size=size, link=link, entsz=entsz))
shstr = secs[e_shstrndx]
def secname(s):
    o = shstr['off'] + s['name']
    return elf[o:elf.index(b'\0', o)].decode()
# acha .symtab + .strtab
symtab = next((s for s in secs if s['typ'] == 2), None)   # SHT_SYMTAB
assert symtab, 'sem symtab (kernel.elf sem simbolos?)'
strtab = secs[symtab['link']]
want = sys.argv[1]
n = symtab['size'] // symtab['entsz']
hits = []
for i in range(n):
    b = symtab['off'] + i * symtab['entsz']
    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', elf, b)
    o = strtab['off'] + st_name
    nm = elf[o:elf.index(b'\0', o)].decode()
    if want in nm and st_value:
        hits.append((nm, st_value, st_size, st_shndx))
if not hits:
    print('nenhum simbolo casa', want); sys.exit(1)
# mapeia VA->offset de arquivo pela secao que contem
def va2off(va):
    for s in secs:
        if s['addr'] and s['addr'] <= va < s['addr'] + s['size'] and s['typ'] == 1:  # PROGBITS
            return s['off'] + (va - s['addr'])
    return None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for nm, va, size, shndx in hits:
    off = va2off(va)
    if off is None:
        print('%s @ 0x%x (sem offset de arquivo)' % (nm, va)); continue
    ln = size if size else 160
    code = elf[off:off + ln]
    print('=== %s @ 0x%x (size=%d) ===' % (nm, va, size))
    for ins in md.disasm(code, va):
        print('  0x%08x: %-24s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
