#!/usr/bin/env python
# disrange.py <rva_start_hex> <count> : disassemble a range of the real explorer by RVA,
# naming CALL [IAT] imports and flagging virtual calls (mov rax,[reg+disp]; call [rip+CFG]).
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
def cstr(off): return d[off:d.index(b'\0', off)].decode('latin1')
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
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
start = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 80
so = o2(start)
code = d[so:so+count*8]
last_vmethod = None
for ins in md.disasm(code, start):
    tag = ''
    # track a preceding "mov rax,[reg+disp]" for slot inference
    if ins.mnemonic in ('call', 'jmp') and 'rip' in ins.op_str:
        m = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ins.op_str)
        if m:
            disp = int(m.group(2), 16) * (-1 if m.group(1) == '-' else 1)
            slot = ins.address + ins.size + disp
            if slot in iat: tag = '  -> %s!%s' % iat[slot]
            elif last_vmethod is not None: tag = '  -> VIRT slot %d (vtbl+0x%x)' % (last_vmethod//8, last_vmethod)
    mm = re.match(r'rax, qword ptr \[r\w+ \+ (0x[0-9a-f]+)\]', ins.op_str)
    if ins.mnemonic == 'mov' and mm:
        last_vmethod = int(mm.group(1), 16)
    elif ins.mnemonic == 'mov' and ins.op_str == 'rax, qword ptr [rcx]':
        last_vmethod = 0
    elif ins.mnemonic not in ('nop',):
        if not (ins.mnemonic == 'mov' and 'rax' in ins.op_str): last_vmethod = last_vmethod
    print("  0x%08x: %-20s %s %s%s" % (imagebase+ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, tag))
