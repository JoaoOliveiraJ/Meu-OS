#!/usr/bin/env python
# whocalls.py <import_name_substr> : acha o(s) slot(s) da IAT de um import NOMEADO
# e TODAS as call-sites `call/jmp [rip+disp]` que apontam p/ esse slot no .text do
# explorer real. Imprime o RVA da call-site (base 0x140000000) p/ jogar no disrange.py.
# Recriado do historico da sessao 4 (era muito util p/ mapear um import ao chamador).
import struct, sys, re, capstone
EXE = r'C:\Windows\explorer.exe'
d = open(EXE, 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e + 4
nsec = struct.unpack_from('<H', d, coff + 2)[0]; optsz = struct.unpack_from('<H', d, coff + 16)[0]
opt = coff + 20; magic = struct.unpack_from('<H', d, opt)[0]
dd = opt + (112 if magic == 0x20b else 96)
imagebase = struct.unpack_from('<Q', d, opt + 24)[0]
imp = struct.unpack_from('<I', d, dd + 8)[0]; sec = opt + optsz
secs = [(struct.unpack_from('<I', d, sec + 40 * i + 12)[0],
         struct.unpack_from('<I', d, sec + 40 * i + 16)[0],
         struct.unpack_from('<I', d, sec + 40 * i + 20)[0],
         struct.unpack_from('<I', d, sec + 40 * i + 36)[0],
         d[sec + 40 * i:sec + 40 * i + 8].split(b'\0')[0].decode('latin1')) for i in range(nsec)]
def o2(rva):
    for va, rsz, raw, ch, nm in secs:
        if va <= rva < va + rsz: return raw + (rva - va)
    return None
def cstr(off): return d[off:d.index(b'\0', off)].decode('latin1')
# monta IAT: slot_rva -> (dll, func)
iat = {}
i = o2(imp)
while True:
    oft = struct.unpack_from('<I', d, i)[0]; nrva = struct.unpack_from('<I', d, i + 12)[0]; ft = struct.unpack_from('<I', d, i + 16)[0]
    if nrva == 0: break
    dll = cstr(o2(nrva)); tn = o2(oft or ft); slot = ft
    while True:
        v = struct.unpack_from('<Q', d, tn)[0]
        if v == 0: break
        if not (v & (1 << 63)): iat[slot] = (dll, cstr(o2(v & 0x7fffffff) + 2))
        else:                   iat[slot] = (dll, 'ord#%d' % (v & 0xffff))
        tn += 8; slot += 8
    i += 20
want = sys.argv[1].lower()
# slots que casam com o filtro (por func OU dll)
targets = {s: v for s, v in iat.items() if want in v[1].lower() or want in v[0].lower()}
if not targets:
    print('nenhum import casa "%s"' % sys.argv[1]); sys.exit(0)
for s, v in sorted(targets.items()):
    print('SLOT rva=0x%x va=0x%x -> %s!%s' % (s, imagebase + s, v[0], v[1]))
print('--- call-sites ---')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = False
# varre so as secoes executaveis (IMAGE_SCN_MEM_EXECUTE = 0x20000000)
for va, rsz, raw, ch, nm in secs:
    if not (ch & 0x20000000): continue
    code = d[raw:raw + rsz]
    for ins in md.disasm(code, va):
        if ins.mnemonic in ('call', 'jmp') and 'rip' in ins.op_str:
            m = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ins.op_str)
            if not m: continue
            disp = int(m.group(2), 16) * (-1 if m.group(1) == '-' else 1)
            slot = ins.address + ins.size + disp
            if slot in targets:
                print('  0x%08x: %-6s -> %s!%s' % (imagebase + ins.address, ins.mnemonic, targets[slot][0], targets[slot][1]))
