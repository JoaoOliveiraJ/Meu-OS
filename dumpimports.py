#!/usr/bin/env python
# Dump da import table do explorer real, filtrando por DLL (arg1). Sem arg: resumo por DLL.
import struct, sys
EXE = r'C:\Windows\explorer.exe'
d = open(EXE, 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e+4
nsec = struct.unpack_from('<H', d, coff+2)[0]; optsz = struct.unpack_from('<H', d, coff+16)[0]
opt = coff+20; magic = struct.unpack_from('<H', d, opt)[0]
dd = opt+(112 if magic == 0x20b else 96)
imp = struct.unpack_from('<I', d, dd+8)[0]; sec = opt+optsz
secs = [(struct.unpack_from('<I', d, sec+40*i+12)[0], struct.unpack_from('<I', d, sec+40*i+16)[0], struct.unpack_from('<I', d, sec+40*i+20)[0]) for i in range(nsec)]
def o2(rva):
    for va, rsz, raw in secs:
        if va <= rva < va+rsz: return raw+(rva-va)
    return None
def cstr(off):
    return d[off:d.index(b'\0', off)].decode('latin1')
tbl = {}
i = o2(imp)
while True:
    oft = struct.unpack_from('<I', d, i)[0]; nrva = struct.unpack_from('<I', d, i+12)[0]; ft = struct.unpack_from('<I', d, i+16)[0]
    if nrva == 0: break
    dll = cstr(o2(nrva))
    tn = o2(oft or ft)
    fns = []
    while True:
        v = struct.unpack_from('<Q', d, tn)[0]
        if v == 0: break
        if v & (1 << 63):
            fns.append('#%d' % (v & 0xffff))       # ordinal
        else:
            fns.append(cstr(o2(v & 0x7fffffff)+2))
        tn += 8
    tbl[dll] = fns
    i += 20
filt = sys.argv[1].lower() if len(sys.argv) > 1 else None
if filt:
    for dll, fns in tbl.items():
        if filt in dll.lower():
            print("== %s (%d) ==" % (dll, len(fns)))
            for f in sorted(fns): print(f)
else:
    for dll in sorted(tbl, key=lambda k: -len(tbl[k])):
        print("%4d  %s" % (len(tbl[dll]), dll))
