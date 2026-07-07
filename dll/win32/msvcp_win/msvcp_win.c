// msvcp_win.dll — STL do C++ (MSVC) MINIMA p/ rodar o explorer.exe REAL. O explorer
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
size_t_ g_locid_0 = 0;
size_t_ g_locid_1 = 0;
size_t_ g_locid_2 = 0;

int DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }
