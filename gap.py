#!/usr/bin/env python
# gap.py <import-dll-substr> <target.dll> : funcoes que o explorer importa da DLL
# (match exato-por-substring no nome do import) e que o target NAO exporta.
import struct, sys
EXE = r'C:\Windows\explorer.exe'
def parse_imports(path, filt):
    d = open(path, 'rb').read()
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
    def cstr(off): return d[off:d.index(b'\0', off)].decode('latin1')
    out = {}
    i = o2(imp)
    while True:
        nrva = struct.unpack_from('<I', d, i+12)[0]
        if nrva == 0: break
        dll = cstr(o2(nrva)); oft = struct.unpack_from('<I', d, i)[0]; ft = struct.unpack_from('<I', d, i+16)[0]
        if filt.lower() in dll.lower():
            tn = o2(oft or ft); fns = []
            while True:
                v = struct.unpack_from('<Q', d, tn)[0]
                if v == 0: break
                if not (v & (1 << 63)): fns.append(cstr(o2(v & 0x7fffffff)+2))
                tn += 8
            out[dll] = fns
        i += 20
    return out
def exports(path):
    d = open(path, 'rb').read()
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
    if not exp_rva: return set()
    eo = o2(exp_rva); nnames = struct.unpack_from('<I', d, eo+24)[0]; no = o2(struct.unpack_from('<I', d, eo+32)[0])
    s = set()
    for i in range(nnames):
        off = o2(struct.unpack_from('<I', d, no+4*i)[0]); s.add(d[off:d.index(b'\0', off)].decode('latin1'))
    return s
filt = sys.argv[1]; target = sys.argv[2]
imps = parse_imports(EXE, filt); exp = exports(target)
allneed = set()
for dll, fns in imps.items():
    print("# import: %s (%d)" % (dll, len(fns)))
    allneed |= set(fns)
missing = sorted(allneed - exp)
print("=== FALTAM %d de %d (target=%s tem %d exports) ===" % (len(missing), len(allneed), target, len(exp)))
for m in missing: print(m)
