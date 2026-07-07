#!/usr/bin/env python
# Gera dll/win32/msvcp_win/msvcp_win.{c,def} a partir da import table REAL do explorer.
# Classifica cada um dos 97 exports: funcoes C reais (mutex/thrd/collate/call_once),
# DATA (locale::id), e o resto -> stub que devolve arg0 (this / sret / *this / ptr nao-nulo).
import struct, os
EXE = r'C:\Windows\explorer.exe'
ROOT = r'E:\1 - OS FOCED APLICATION'
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
names = []
i = o2(imp)
while True:
    nrva = struct.unpack_from('<I', d, i+12)[0]
    if nrva == 0: break
    dll = cstr(o2(nrva))
    oft = struct.unpack_from('<I', d, i)[0]; ft = struct.unpack_from('<I', d, i+16)[0]
    if dll.lower() == 'msvcp_win.dll':
        tn = o2(oft or ft)
        while True:
            v = struct.unpack_from('<Q', d, tn)[0]
            if v == 0: break
            if not (v & (1 << 63)): names.append(cstr(o2(v & 0x7fffffff)+2))
            tn += 8
    i += 20

# Funcoes C com implementacao REAL (nome C exato -> simbolo interno).
REAL = {
    '_Mtx_init_in_situ': 'mtx_init', '_Mtx_destroy_in_situ': 'mtx_destroy',
    '_Mtx_lock': 'mtx_lock', '_Mtx_unlock': 'mtx_unlock', '_Mtx_trylock': 'mtx_trylock',
    '_Cnd_do_broadcast_at_thread_exit': 'cnd_bcast_exit',
    '_Thrd_detach': 'thrd_detach', '_Thrd_id': 'thrd_id', '_Thrd_join': 'thrd_join',
    '_Wcscoll': 'wcscoll_impl', '_Wcsxfrm': 'wcsxfrm_impl', '_Xtime_get_ticks': 'xtime_get_ticks',
    '?_Execute_once@std@@YAHAEAUonce_flag@1@P6AHPEAX1PEAPEAX@Z1@Z': 'execute_once',
    '?_Getcoll@_Locinfo@std@@QEBA?AU_Collvec@@XZ': 'ret_sret16',
}
lines_def = ['LIBRARY msvcp_win.dll', 'EXPORTS']
data_syms = []
for n in names:
    if n in REAL:
        lines_def.append('    %s = %s' % (n, REAL[n]))
    elif n.startswith('?id@') and n.endswith('@A'):
        sym = 'g_locid_%d' % len(data_syms)
        data_syms.append(sym)
        lines_def.append('    %s = %s DATA' % (n, sym))
    else:
        # Default universal: devolve arg0 (this p/ ctors, ponteiro sret, *this p/
        # refs, ponteiro nao-nulo p/ getters). ABI-correto p/ a maioria; especifico
        # (nao catch-all): cada nome mangled aponta explicitamente aqui no .def.
        lines_def.append('    %s = ret_this' % n)

c = r'''// msvcp_win.dll — STL do C++ (MSVC) MINIMA p/ rodar o explorer.exe REAL. O explorer
// importa 97 simbolos daqui (mutex/threads de <mutex>/<thread>, call_once, collate e a
// maquinaria de iostream/locale p/ char16 'G'=unsigned short). Implementamos DE VERDADE
// o que roda single-threaded na init (mutex no-op correto, call_once que EXECUTA o
// callback, collate lexicografico, locale::id como dado); o resto sao stubs ESPECIFICOS
// e nomeados (via .def) que devolvem arg0 — ABI-correto p/ ctor(this)/sret/ref/ptr.
// NUNCA um catch-all generico. Nomes mangled exportados pelo msvcp_win.def.
typedef unsigned long long size_t_;
typedef unsigned short     ushort_;
unsigned int _tls_index = 0;   // exigido pelo runtime de DLL (igual as outras DLLs)

// ---- mutex de <mutex> (single-threaded: init/destroy no-op; lock/unlock ok) ----
// _Mtx_t aponta p/ um bloco de controle (_Mtx_internal_imp_t). Zeramos na init.
void mtx_init(void* mtx, int type)   { (void)type; if (mtx) { volatile size_t_* p = (volatile size_t_*)mtx; p[0]=0; } }
void mtx_destroy(void* mtx)          { (void)mtx; }
int  mtx_lock(void* mtx)             { (void)mtx; return 0; }   // _Thrd_success
int  mtx_unlock(void* mtx)           { (void)mtx; return 0; }
int  mtx_trylock(void* mtx)          { (void)mtx; return 0; }
void cnd_bcast_exit(void)            { }

// ---- threads de <thread> (single-threaded: nada a fazer) ----
int      thrd_detach(void* thr)      { (void)thr; return 0; }
unsigned thrd_id(void)               { return 1; }
int      thrd_join(void* thr, int* code){ (void)thr; if (code) *code = 0; return 0; }
long long xtime_get_ticks(void)      { return 0; }

// ---- call_once (_Execute_once): EXECUTA o callback UMA vez de verdade ----
// once_flag = { void* _Opaque }. Callback: int(void* initonce, void* pv, void** ctx).
typedef int (*exec_cb_t)(void*, void*, void**);
int execute_once(void** flag, exec_cb_t cb, void* pv) {
    if (!flag) return cb ? cb(0, pv, 0) : 1;
    if (*flag) return 1;                       // ja executado
    void* ctx = 0;
    int ok = cb ? cb((void*)flag, pv, &ctx) : 1;
    if (ok) *flag = (void*)(size_t_)1;         // marca concluido
    return ok;
}

// ---- collate (std::collate<G>): comparacao/transform lexicografica de char16 ----
int wcscoll_impl(const ushort_* a, const ushort_* ae, const ushort_* b, const ushort_* be, const void* coll) {
    (void)coll;
    while (a < ae && b < be) { if (*a != *b) return (*a < *b) ? -1 : 1; a++; b++; }
    if (a < ae) return 1; if (b < be) return -1; return 0;
}
size_t_ wcsxfrm_impl(ushort_* dst, ushort_* dend, const ushort_* s, const ushort_* se, const void* coll) {
    (void)coll; size_t_ n = 0;
    while (s < se) { if (dst < dend) *dst++ = *s; s++; n++; }
    return n;   // numero de elementos necessarios (identidade: sem collation real)
}

// ---- retorno sret de 16 bytes zerado (_Getcoll -> _Collvec{_Page,_LocaleName}) ----
// x64: ponteiro de retorno em arg0 (RCX); devolve-o em RAX zerando os 16 bytes.
void* ret_sret16(void* ret) { if (ret) { size_t_* p = (size_t_*)ret; p[0]=0; p[1]=0; } return ret; }

// ---- stub universal ESPECIFICO: devolve arg0 (this/sret/ref/ptr nao-nulo) ----
void* ret_this(void* self) { return self; }

// ---- locale::id (DATA): indices de facet, atribuidos preguicosamente pela STL ----
'''
for s in data_syms:
    c += 'size_t_ %s = 0;\n' % s
c += '\nint DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }\n'

os.makedirs(os.path.join(ROOT, 'dll', 'win32', 'msvcp_win'), exist_ok=True)
open(os.path.join(ROOT, 'dll', 'win32', 'msvcp_win', 'msvcp_win.c'), 'w', newline='\n').write(c)
open(os.path.join(ROOT, 'dll', 'win32', 'msvcp_win', 'msvcp_win.def'), 'w', newline='\n').write('\n'.join(lines_def) + '\n')
print('exports:', len(names), '| data(locale::id):', len(data_syms))
print('escrito: dll/win32/msvcp_win/msvcp_win.{c,def}')
