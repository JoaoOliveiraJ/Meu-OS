#!/usr/bin/env python
# pdata.py <pe> <rva> : acha o RUNTIME_FUNCTION (.pdata) que cobre <rva>, decodifica o
# UNWIND_INFO (.xdata) e reporta se a funcao tem EHANDLER (try/catch C++) ou UHANDLER
# (cleanup/unwind). Essencial p/ saber se um `throw` do explorer SERA capturado (existe
# um __CxxFrameHandler3/4 na cadeia) — ou se cai em std::terminate de qualquer jeito.
import struct, sys
PE = sys.argv[1]; RVA = int(sys.argv[2], 16)
d = open(PE, 'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff = e+4
nsec = struct.unpack_from('<H', d, coff+2)[0]; optsz = struct.unpack_from('<H', d, coff+16)[0]
opt = coff+20; magic = struct.unpack_from('<H', d, opt)[0]
dd = opt+(112 if magic == 0x20b else 96)
imagebase = struct.unpack_from('<Q', d, opt+24)[0]
exc_rva = struct.unpack_from('<I', d, dd+3*8)[0]; exc_sz = struct.unpack_from('<I', d, dd+3*8+4)[0]  # dir 3 = exception (.pdata)
sec = opt+optsz
secs = [(struct.unpack_from('<I', d, sec+40*i+12)[0], struct.unpack_from('<I', d, sec+40*i+16)[0], struct.unpack_from('<I', d, sec+40*i+20)[0]) for i in range(nsec)]
def o2(rva):
    for va, rsz, raw in secs:
        if va <= rva < va+rsz: return raw+(rva-va)
    return None
# RUNTIME_FUNCTION = { u32 begin; u32 end; u32 unwindInfo }  (12 bytes)
po = o2(exc_rva); n = exc_sz // 12
found = None
for i in range(n):
    b, en, u = struct.unpack_from('<III', d, po + 12*i)
    if b <= RVA < en: found = (b, en, u); break
if not found:
    print("RVA 0x%x: sem RUNTIME_FUNCTION (funcao folha, sem unwind)" % RVA); sys.exit(0)
b, en, u = found
print("func [0x%x, 0x%x)  unwindInfo=0x%x" % (b, en, u))
uo = o2(u)
ver_flags = d[uo]; ver = ver_flags & 7; flags = ver_flags >> 3
szprolog = d[uo+1]; cntcodes = d[uo+2]; frame = d[uo+3]
print("  UNWIND_INFO ver=%d flags=0x%x (EHANDLER=%d UHANDLER=%d CHAININFO=%d) prolog=0x%x codes=%d"
      % (ver, flags, flags&1, (flags>>1)&1, (flags>>2)&1, szprolog, cntcodes))
# apos os unwind codes (2 bytes cada, arredondado p/ par) vem, se EHANDLER/UHANDLER,
# o RVA do handler (u32) + os dados do handler (p/ __CxxFrameHandler3 = RVA do FuncInfo).
if flags & 3:
    ncodes = (cntcodes + 1) & ~1
    ho = uo + 4 + ncodes*2
    handler_rva = struct.unpack_from('<I', d, ho)[0]
    hdata_rva = ho - o2(0) if False else None
    # o campo apos o handler e' o "handler data" (p/ C++ EH = RVA do FuncInfo)
    fi_rva = struct.unpack_from('<I', d, ho+4)[0]
    print("  handler RVA=0x%x  handlerData(FuncInfo RVA)=0x%x" % (handler_rva, fi_rva))
    print("  -> handler VA=0x%x" % (imagebase + handler_rva))
