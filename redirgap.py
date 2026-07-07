#!/usr/bin/env python
# Aplica a MESMA logica de apiset_redirect do kernel a cada import do explorer, agrupa
# por DLL implementadora REAL e mostra o que falta em cada uma. Espelha loader.c.
import struct, sys, os
EXE = r'C:\Windows\explorer.exe'
BUILD = r'E:\1 - OS FOCED APLICATION\build'
def redirect(dll):
    d = dll.lower()
    if d.startswith('api-ms-win-crt-'):          return 'ucrtbase.dll'
    if d.startswith('api-ms-win-core-com'):       return 'combase.dll'
    if d.startswith('api-ms-win-core-winrt'):     return dll
    if d.startswith('api-ms-win-core-registry'):  return 'advapi32.dll'
    if d.startswith('api-ms-win-core-'):          return 'kernel32.dll'
    if d.startswith('api-ms-win-rtcore-ntuser-'): return 'user32.dll'
    if d.startswith('api-ms-win-ntuser-'):        return 'user32.dll'
    if d.startswith('api-ms-win-security-'):      return 'advapi32.dll'
    if d.startswith('api-ms-win-eventing-'):      return 'advapi32.dll'
    return dll
def parse(path):
    d=open(path,'rb').read(); e=struct.unpack_from('<I',d,0x3C)[0]; coff=e+4
    nsec=struct.unpack_from('<H',d,coff+2)[0]; optsz=struct.unpack_from('<H',d,coff+16)[0]
    opt=coff+20; magic=struct.unpack_from('<H',d,opt)[0]; dd=opt+(112 if magic==0x20b else 96)
    imp=struct.unpack_from('<I',d,dd+8)[0]; sec=opt+optsz
    secs=[(struct.unpack_from('<I',d,sec+40*i+12)[0],struct.unpack_from('<I',d,sec+40*i+16)[0],struct.unpack_from('<I',d,sec+40*i+20)[0]) for i in range(nsec)]
    def o2(r):
        for va,rsz,raw in secs:
            if va<=r<va+rsz: return raw+(r-va)
    def cs(o): return d[o:d.index(b'\0',o)].decode('latin1')
    res={}; i=o2(imp)
    while True:
        nrva=struct.unpack_from('<I',d,i+12)[0]
        if nrva==0: break
        dll=cs(o2(nrva)); oft=struct.unpack_from('<I',d,i)[0]; ft=struct.unpack_from('<I',d,i+16)[0]
        tn=o2(oft or ft)
        while True:
            v=struct.unpack_from('<Q',d,tn)[0]
            if v==0: break
            if not (v&(1<<63)): res.setdefault(redirect(dll),set()).add(cs(o2(v&0x7fffffff)+2))
            tn+=8
        i+=20
    return res
def exports(path):
    if not os.path.exists(path): return set()
    d=open(path,'rb').read(); e=struct.unpack_from('<I',d,0x3C)[0]; coff=e+4
    nsec=struct.unpack_from('<H',d,coff+2)[0]; optsz=struct.unpack_from('<H',d,coff+16)[0]
    opt=coff+20; magic=struct.unpack_from('<H',d,opt)[0]; dd=opt+(112 if magic==0x20b else 96)
    exp=struct.unpack_from('<I',d,dd+0)[0]; sec=opt+optsz
    secs=[(struct.unpack_from('<I',d,sec+40*i+12)[0],struct.unpack_from('<I',d,sec+40*i+16)[0],struct.unpack_from('<I',d,sec+40*i+20)[0]) for i in range(nsec)]
    def o2(r):
        for va,rsz,raw in secs:
            if va<=r<va+rsz: return raw+(r-va)
    if not exp: return set()
    eo=o2(exp); nn=struct.unpack_from('<I',d,eo+24)[0]; no=o2(struct.unpack_from('<I',d,eo+32)[0]); s=set()
    for i in range(nn):
        off=o2(struct.unpack_from('<I',d,no+4*i)[0]); s.add(d[off:d.index(b'\0',off)].decode('latin1'))
    return s
res=parse(EXE)
only=sys.argv[1].lower() if len(sys.argv)>1 else None
for dll in sorted(res, key=lambda k:-len(res[k])):
    if only and only not in dll.lower(): continue
    exp=exports(os.path.join(BUILD, dll))
    missing=sorted(res[dll]-exp)
    tag='DIRETA/sem-impl' if dll.startswith('api-ms') else ''
    print("== %s: precisa %d, falta %d %s ==" % (dll, len(res[dll]), len(missing), tag))
    if only:
        for m in missing: print(m)
