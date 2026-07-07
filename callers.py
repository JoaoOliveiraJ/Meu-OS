#!/usr/bin/env python
# callers.py <target_rva_hex> : find all E8 rel32 CALLs whose target == target_rva.
import struct, sys, capstone
EXE = r'C:\Windows\explorer.exe'
d = open(EXE, 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e+4
nsec = struct.unpack_from('<H', d, coff+2)[0]; optsz = struct.unpack_from('<H', d, coff+16)[0]
opt = coff+20; magic = struct.unpack_from('<H', d, opt)[0]
imagebase = struct.unpack_from('<Q', d, opt+24)[0]
sec = opt+optsz
secs = [(struct.unpack_from('<I', d, sec+40*i+12)[0], struct.unpack_from('<I', d, sec+40*i+16)[0], struct.unpack_from('<I', d, sec+40*i+20)[0], struct.unpack_from('<I', d, sec+40*i+36)[0]) for i in range(nsec)]
def o2(rva):
    for va, rsz, raw, ch in secs:
        if va <= rva < va+rsz: return raw+(rva-va)
    return None
target = int(sys.argv[1], 16)
# scan executable sections for E8 rel32
hits = []
for va, rsz, raw, ch in secs:
    if not (ch & 0x20000000): continue  # IMAGE_SCN_MEM_EXECUTE
    blob = d[raw:raw+rsz]
    i = 0
    n = len(blob)
    while i < n-5:
        if blob[i] == 0xE8:
            rel = struct.unpack_from('<i', blob, i+1)[0]
            tgt = va + i + 5 + rel
            if tgt == target:
                hits.append(va+i)
        i += 1
print("target rva 0x%x: %d caller CALL site(s)" % (target, len(hits)))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for h in hits:
    print("  call @ rva 0x%x (va 0x%x)" % (h, imagebase+h))
