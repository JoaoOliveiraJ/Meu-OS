#!/usr/bin/env python
# Lista os nomes exportados por um PE (arg1 = caminho da DLL).
import struct, sys
d = open(sys.argv[1], 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e+4
nsec = struct.unpack_from('<H', d, coff+2)[0]; optsz = struct.unpack_from('<H', d, coff+16)[0]
opt = coff+20; magic = struct.unpack_from('<H', d, opt)[0]
dd = opt+(112 if magic == 0x20b else 96)
exp_rva = struct.unpack_from('<I', d, dd+0)[0]; sec = opt+optsz
secs = [(struct.unpack_from('<I', d, sec+40*i+12)[0], struct.unpack_from('<I', d, sec+40*i+16)[0], struct.unpack_from('<I', d, sec+40*i+20)[0]) for i in range(nsec)]
def o2(rva):
    for va, rsz, raw in secs:
        if va <= rva < va+rsz: return raw+(rva-va)
    return None
if not exp_rva:
    print("(sem export directory)"); sys.exit(0)
eo = o2(exp_rva)
nnames = struct.unpack_from('<I', d, eo+24)[0]
names_rva = struct.unpack_from('<I', d, eo+32)[0]
no = o2(names_rva)
out = []
for i in range(nnames):
    nrva = struct.unpack_from('<I', d, no+4*i)[0]
    off = o2(nrva)
    out.append(d[off:d.index(b'\0', off)].decode('latin1'))
for n in out: print(n)
print("total:", len(out))
