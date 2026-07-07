#!/usr/bin/env python
# rdstr.py <rva_hex> [utf16|ascii] : dump the string at an RVA of the real explorer.
import struct, sys
EXE = r'C:\Windows\explorer.exe'
d = open(EXE, 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e+4
nsec = struct.unpack_from('<H', d, coff+2)[0]; optsz = struct.unpack_from('<H', d, coff+16)[0]
opt = coff+20; sec = opt+optsz
secs = [(struct.unpack_from('<I', d, sec+40*i+12)[0], struct.unpack_from('<I', d, sec+40*i+16)[0], struct.unpack_from('<I', d, sec+40*i+20)[0]) for i in range(nsec)]
def o2(rva):
    for va, rsz, raw in secs:
        if va <= rva < va+rsz: return raw+(rva-va)
    return None
for a in sys.argv[1:]:
    rva = int(a, 16); off = o2(rva)
    if off is None: print(hex(rva), "(no section)"); continue
    # try UTF-16
    w = ''
    i = off
    while d[i] or d[i+1]:
        w += chr(d[i] | (d[i+1] << 8)); i += 2
        if len(w) > 120: break
    a8 = d[off:d.index(b'\0', off)].decode('latin1', 'replace')[:120]
    print("rva 0x%x  ASCII=%r  UTF16=%r" % (rva, a8, w))
