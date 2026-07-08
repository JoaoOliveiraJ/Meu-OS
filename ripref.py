#!/usr/bin/env python
# ripref.py <rva...> : brute-force por LEA/MOV reg,[rip+disp] (e call/jmp [rip+disp]) que
# apontam p/ cada RVA dado, varrendo TODAS as secoes executaveis do explorer real. Acha
# referencias que a desmontagem linear PERDE por desalinhamento. Recriado do historico da
# sessao 4 (foi como se achou o registrar da "Worker Window"). Base VA 0x140000000.
import struct, sys, capstone, re
EXE = r'C:\Windows\explorer.exe'
d = open(EXE, 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e + 4
nsec = struct.unpack_from('<H', d, coff + 2)[0]; optsz = struct.unpack_from('<H', d, coff + 16)[0]
opt = coff + 20; magic = struct.unpack_from('<H', d, opt)[0]
imagebase = struct.unpack_from('<Q', d, opt + 24)[0]
sec = opt + optsz
secs = [(struct.unpack_from('<I', d, sec + 40 * i + 12)[0],
         struct.unpack_from('<I', d, sec + 40 * i + 16)[0],
         struct.unpack_from('<I', d, sec + 40 * i + 20)[0],
         struct.unpack_from('<I', d, sec + 40 * i + 36)[0]) for i in range(nsec)]
wants = set(int(a, 16) for a in sys.argv[1:])
if not wants:
    print('uso: ripref.py <rva_hex> [rva_hex...]'); sys.exit(1)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
found = 0
for va, rsz, raw, ch in secs:
    if not (ch & 0x20000000): continue     # so secoes executaveis
    code = d[raw:raw + rsz]
    # desmonta a partir de VARIOS offsets iniciais p/ pegar refs em codigo desalinhado
    for start_off in range(0, 16):
        for ins in md.disasm(code[start_off:], va + start_off):
            if 'rip' not in ins.op_str: continue
            m = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ins.op_str)
            if not m: continue
            disp = int(m.group(2), 16) * (-1 if m.group(1) == '-' else 1)
            tgt = ins.address + ins.size + disp
            if tgt in wants:
                print('  0x%08x: %-7s %-24s -> RVA 0x%x' % (imagebase + ins.address, ins.mnemonic, ins.op_str, tgt))
                found += 1
print('(%d refs)' % found)
