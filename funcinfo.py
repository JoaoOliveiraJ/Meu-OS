#!/usr/bin/env python
# funcinfo.py <pe> <func_rva> : decodifica o FuncInfo (C++ EH da MS) da funcao que cobre
# <func_rva> — lista os TRY BLOCKS (faixa de state) e os TIPOS de catch (nome mangled;
# "..." = catch-all). Usado p/ saber se um `throw` DEVERIA ser capturado por aquele frame
# (validar a maquinaria de C++ EH da ntdll).
import struct, sys
PE = sys.argv[1]; RVA = int(sys.argv[2], 16)
d = open(PE,'rb').read()
e = struct.unpack_from('<I', d, 0x3C)[0]; coff=e+4
nsec = struct.unpack_from('<H', d, coff+2)[0]; optsz = struct.unpack_from('<H', d, coff+16)[0]
opt=coff+20; magic=struct.unpack_from('<H', d, opt)[0]; dd=opt+(112 if magic==0x20b else 96)
imagebase=struct.unpack_from('<Q', d, opt+24)[0]
exc_rva=struct.unpack_from('<I', d, dd+3*8)[0]; exc_sz=struct.unpack_from('<I', d, dd+3*8+4)[0]
sec=opt+optsz
secs=[(struct.unpack_from('<I',d,sec+40*i+12)[0], struct.unpack_from('<I',d,sec+40*i+16)[0], struct.unpack_from('<I',d,sec+40*i+20)[0]) for i in range(nsec)]
def o2(rva):
    for va,rsz,raw in secs:
        if va<=rva<va+rsz: return raw+(rva-va)
    return None
def i32(rva): return struct.unpack_from('<i', d, o2(rva))[0]
def u32(rva): return struct.unpack_from('<I', d, o2(rva))[0]
def cstr(rva):
    o=o2(rva); return d[o:d.index(b'\0',o)].decode('latin1')
# acha a RUNTIME_FUNCTION que cobre RVA
po=o2(exc_rva); n=exc_sz//12; rf=None
for i in range(n):
    b,en,u=struct.unpack_from('<III',d,po+12*i)
    if b<=RVA<en: rf=(b,en,u); break
if not rf: print("sem RUNTIME_FUNCTION p/ 0x%x"%RVA); sys.exit(0)
b,en,u=rf
print("func [0x%x,0x%x) unwind=0x%x"%(b,en,u))
uo=o2(u); flags=d[uo]>>3; cnt=d[uo+2]
# segue CHAININFO ate achar o info com EHANDLER
while True:
    if flags & 1:  # EHANDLER
        ncodes=(cnt+1)&~1; ho=uo+4+ncodes*2
        handler=struct.unpack_from('<I',d,ho)[0]; fi_rva=struct.unpack_from('<I',d,ho+4)[0]
        break
    if flags & 4:  # CHAININFO -> RUNTIME_FUNCTION do pai
        ncodes=(cnt+1)&~1; cho=uo+4+ncodes*2
        pb,pe2,pu=struct.unpack_from('<III',d,cho)
        uo=o2(pu); flags=d[uo]>>3; cnt=d[uo+2]; continue
    print("funcao SEM EHANDLER (so cleanup/nenhum)"); sys.exit(0)
print("handler=0x%x FuncInfo rva=0x%x"%(handler, fi_rva))
# FuncInfo x64
fo=o2(fi_rva)
mn=struct.unpack_from('<I',d,fo)[0]; maxState=struct.unpack_from('<i',d,fo+4)[0]
dispTry=struct.unpack_from('<i',d,fo+16)[0]; nTry=struct.unpack_from('<I',d,fo+12)[0]
print("  magic=0x%x maxState=%d nTryBlocks=%d dispTryBlockMap=0x%x"%(mn,maxState,nTry,dispTry))
if not dispTry or not nTry: print("  (sem try blocks)"); sys.exit(0)
to=o2(dispTry)
for t in range(nTry):
    tl,th,ch,nc,dh=struct.unpack_from('<iiiiI',d,to+20*t)
    print("  try[%d]: state[%d..%d] catchHigh=%d nCatches=%d handlers@0x%x"%(t,tl,th,ch,nc,dh))
    ho2=o2(dh)
    for c in range(nc):
        adj,dispType,dispCatchObj,dispHandler,dispFrame=struct.unpack_from('<IiiiI',d,ho2+20*c)
        name = "..." if dispType==0 else cstr(dispType+16)
        print("      catch: type=%s adj=0x%x handler_rva=0x%x"%(name,adj,dispHandler))
