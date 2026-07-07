#!/usr/bin/env python
# disat.py <hexaddr> [hexaddr...] : para cada endereco absoluto, se cair na imagem do
# explorer real (base lida do serial.log), desmonta ~16 instrucoes ANTES e o alvo, p/
# achar o CALL [IAT] que desviou. Tambem nomeia o import se o site for CALL [rip+disp].
import struct, sys, re, capstone
EXE = r'C:\Windows\explorer.exe'
LOG = r'E:\1 - OS FOCED APLICATION\build\serial.log'
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
log = open(LOG, 'r', errors='ignore').read()
mb = re.findall(r'img=explorerreal\.exe base=0x([0-9A-Fa-f]+)', log)
base = int(mb[-1], 16) if mb else 0x04319000
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
addrs = [int(a, 16) for a in sys.argv[1:]]
if not addrs:
    mc = re.findall(r'stack\[rsp\.\.\]=\s*((?:0x[0-9A-Fa-f]+\s*)+)', log)
    if mc: addrs = [int(x, 16) for x in mc[-1].split()]
    print("(sem args) usando stack candidates do log:", [hex(a) for a in addrs])
for a in addrs:
    rva = a - base
    off = o2(rva)
    print("\n==== addr=0x%x base=0x%x rva=0x%x %s ====" % (a, base, rva, "(fora da imagem)" if off is None else ""))
    if off is None: continue
    start = rva - 48
    so = o2(start)
    code = d[so:so+96]
    for ins in md.disasm(code, start):
        tag = ''
        if ins.mnemonic in ('call', 'jmp') and 'rip' in ins.op_str:
            m = re.search(r'\[rip \+ (0x[0-9a-f]+)\]', ins.op_str) or re.search(r'\[rip \- (0x[0-9a-f]+)\]', ins.op_str)
            if m:
                disp = int(m.group(1), 16) * (-1 if '-' in ins.op_str else 1)
                slot = ins.address + ins.size + disp
                if slot in iat: tag = '  -> %s!%s' % iat[slot]
        mark = ' <== target' if ins.address == rva else ''
        print("  0x%08x: %-16s %s %s%s%s" % (base+ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, tag, mark))
