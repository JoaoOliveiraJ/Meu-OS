#!/usr/bin/env python
# strxref.py <substr> : acha a string (ASCII e UTF-16LE) no explorer real e desmonta
# ~24 instrucoes em torno de cada LEA rip-relative que aponta p/ ela (o call-site que
# usa a string, ex.: LoadLibraryW("comctl32.dll")). Nomeia CALL [IAT] pelo import.
import struct, sys, re, capstone
EXE = r'C:\Windows\explorer.exe'
d = open(EXE, 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e+4
nsec = struct.unpack_from('<H', d, coff+2)[0]; optsz = struct.unpack_from('<H', d, coff+16)[0]
opt = coff+20; magic = struct.unpack_from('<H', d, opt)[0]
dd = opt+(112 if magic == 0x20b else 96)
imagebase = struct.unpack_from('<Q', d, opt+24)[0]
imp = struct.unpack_from('<I', d, dd+8)[0]; sec = opt+optsz
secs = [(struct.unpack_from('<I', d, sec+40*i+12)[0], struct.unpack_from('<I', d, sec+40*i+16)[0], struct.unpack_from('<I', d, sec+40*i+20)[0]) for i in range(nsec)]
def o2(rva):
    for va, rsz, raw in secs:
        if va <= rva < va+rsz: return raw+(rva-va)
    return None
def r2o(rva): return o2(rva)
def cstr(off): return d[off:d.index(b'\0', off)].decode('latin1')
# IAT slot -> name
iat = {}
i = o2(imp)
while True:
    oft = struct.unpack_from('<I', d, i)[0]; nrva = struct.unpack_from('<I', d, i+12)[0]; ft = struct.unpack_from('<I', d, i+16)[0]
    if nrva == 0: break
    dll = cstr(o2(nrva)); tn = o2(oft or ft); slot = ft
    while True:
        v = struct.unpack_from('<Q', d, tn)[0]
        if v == 0: break
        if not (v & (1 << 63)): iat[slot] = (dll, cstr(o2(v & 0x7fffffff)+2))
        tn += 8; slot += 8
    i += 20

sub = sys.argv[1]
# build target RVAs: find ASCII and UTF-16LE occurrences in all sections
targets = []
asc = sub.encode('latin1')
utf16 = sub.encode('utf-16-le')
for va, rsz, raw in secs:
    blob = d[raw:raw+rsz]
    for pat, kind in ((utf16, 'W'), (asc, 'A')):
        start = 0
        while True:
            j = blob.find(pat, start)
            if j < 0: break
            targets.append((va + j, kind))
            start = j + 1
print("string '%s': %d ocorrencia(s)" % (sub, len(targets)))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
# scan .text for LEA reg,[rip+disp] whose target hits a string RVA
tset = {rva for rva, k in targets}
for va, rsz, raw in secs:
    # only executable-ish: scan all; cheap enough
    code = d[raw:raw+rsz]
    for ins in md.disasm(code, va):
        if ins.mnemonic == 'lea' and 'rip' in ins.op_str:
            m = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ins.op_str)
            if not m: continue
            disp = int(m.group(2), 16) * (-1 if m.group(1) == '-' else 1)
            tgt = ins.address + ins.size + disp
            if tgt in tset:
                # disassemble a window around this lea
                start = ins.address - 8
                so = o2(start)
                if so is None: continue
                print("\n==== LEA @ rva 0x%x (va 0x%x) -> string rva 0x%x ====" % (ins.address, imagebase+ins.address, tgt))
                win = d[so:so+140]
                for x in md.disasm(win, start):
                    tag = ''
                    if x.mnemonic in ('call','jmp') and 'rip' in x.op_str:
                        mm = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', x.op_str)
                        if mm:
                            dsp = int(mm.group(2),16)*(-1 if mm.group(1)=='-' else 1)
                            slot = x.address + x.size + dsp
                            if slot in iat: tag = '  -> %s!%s' % iat[slot]
                    mark = ' <== lea' if x.address == ins.address else ''
                    print("  0x%08x: %-22s %s%s%s" % (imagebase+x.address, x.mnemonic, x.op_str, tag, mark))
