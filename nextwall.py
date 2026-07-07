#!/usr/bin/env python
# Le build/serial.log, pega o caller logado pelo diagnostico de bring-up e desmonta
# o explorer real p/ nomear a funcao (import nao resolvido) que faltou.
import struct, re, sys
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
iat = {}
i = o2(imp)
while True:
    oft = struct.unpack_from('<I', d, i)[0]; nrva = struct.unpack_from('<I', d, i+12)[0]; ft = struct.unpack_from('<I', d, i+16)[0]
    if nrva == 0: break
    dll = d[o2(nrva):d.index(b'\0', o2(nrva))].decode('latin1')
    tn = o2(oft or ft); slot = ft
    while True:
        v = struct.unpack_from('<Q', d, tn)[0]
        if v == 0: break
        if not (v & (1 << 63)):
            no = o2(v & 0x7fffffff)+2
            iat[slot] = (dll, d[no:d.index(b'\0', no)].decode('latin1'))
        tn += 8; slot += 8
    i += 20
def nm(t):
    x = iat.get(t); return ("%s!%s" % x) if x else None

log = open(LOG, 'r', errors='ignore').read()
mb = re.findall(r'img=explorerreal\.exe base=0x([0-9A-Fa-f]+)', log)
base = int(mb[-1], 16) if mb else 0x04319000
mc = re.findall(r'caller\[rsp\]=0x([0-9A-Fa-f]+)', log)
if not mc:
    print("Sem caller de bring-up no log (explorer nao crashou por import nulo?).")
    sys.exit(0)
caller = int(mc[-1], 16); rva = caller-base
print("caller=0x%x base=0x%x -> RVA 0x%x" % (caller, base, rva))
off = o2(rva)
def scan(frva, lim=96):
    fo = o2(frva)
    for k in range(lim):
        if d[fo+k] == 0xFF and d[fo+k+1] in (0x15, 0x25):
            disp = struct.unpack_from('<i', d, fo+k+2)[0]; t = frva+k+6+disp
            n = nm(t)
            if n: return n, ('call' if d[fo+k+1] == 0x15 else 'jmp')
    return None, None
# instrucao que termina no rva:
if d[off-6] == 0xFF and d[off-5] == 0x15:                 # CALL [IAT] direto
    disp = struct.unpack_from('<i', d, off-4)[0]
    print("  FALTA:", nm(rva+disp) or "?? 0x%x" % (rva+disp))
elif d[off-5] == 0xE8:                                    # CALL rel32 -> func -> scan
    rel = struct.unpack_from('<i', d, off-4)[0]; fn = rva+rel
    n, kind = scan(fn)
    print("  (via func 0x%x %s) FALTA:" % (fn, kind), n or ("?? dump "+d[o2(fn):o2(fn)+32].hex()))
else:
    print("  padrao inesperado; bytes antes:", d[off-8:off].hex())
