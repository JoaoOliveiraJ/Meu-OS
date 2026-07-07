// combase.dll — COM base p/ rodar o explorer.exe REAL. Cobre as 35 funcoes de
// api-ms-win-core-com* que o explorer importa. Implementacoes DE VERDADE onde a
// semantica e' bem definida e segura em processo unico / apartamento unico:
//   - alocador de tarefa (CoTaskMem*) + IMalloc real sobre ele;
//   - helpers de GUID/string (CoCreateGuid/StringFromGUID2/CLSIDFromString/...);
//   - IStream em memoria (CreateStreamOnHGlobal) real (Read/Write/Seek/Stat/...);
//   - marshaling de IDENTIDADE (mesmo processo: marshal->stream->unmarshal devolve o
//     MESMO ponteiro; correto quando ha' de fato um so apartamento);
//   - objeto COM UNIVERSAL p/ CoCreateInstance: QueryInterface devolve a si mesmo e os
//     metodos devolvem E_NOTIMPL — o explorer recebe um ponteiro NAO-NULO, le a vtable,
//     chama um metodo (recebe E_NOTIMPL) e DEGRADA em vez de derefar NULL.
//   - stubs ESPECIFICOS e nomeados (S_OK/no-op) p/ seguranca/cancelamento/registro de
//     class object, que sao genuinamente no-op num processo unico sem DCOM.
// Sem stub generico catch-all. Autocontido (sem ntdll) -> caminho PMM+reloc limpo.

typedef unsigned long long size_t_;
typedef long               HRESULT_;
typedef unsigned long      ULONG_;
typedef unsigned short     WCHAR_;
unsigned int _tls_index = 0;

#define S_OK_            ((HRESULT_)0)
#define S_FALSE_         ((HRESULT_)1)
#define E_NOTIMPL_       ((HRESULT_)0x80004001L)
#define E_NOINTERFACE_   ((HRESULT_)0x80004002L)
#define E_POINTER_       ((HRESULT_)0x80004003L)
#define E_FAIL_          ((HRESULT_)0x80004005L)
#define E_INVALIDARG_    ((HRESULT_)0x80070057L)
#define E_OUTOFMEMORY_   ((HRESULT_)0x8007000EL)
#define CO_E_CLASSSTRING ((HRESULT_)0x800401F3L)

typedef struct { unsigned int Data1; unsigned short Data2; unsigned short Data3; unsigned char Data4[8]; } GUID_;

// ---- utilitarios de bytes (nostdlib: sem memcpy/memset) ----
static void bzero_(void* p, size_t_ n) { unsigned char* d = (unsigned char*)p; for (size_t_ i = 0; i < n; i++) d[i] = 0; }
static void bcopy_(void* dst, const void* src, size_t_ n) { unsigned char* d = (unsigned char*)dst; const unsigned char* s = (const unsigned char*)src; for (size_t_ i = 0; i < n; i++) d[i] = s[i]; }

// ---- arena bump (task memory + objetos + buffers de stream). Sem free real. ----
static unsigned char g_comheap[0x100000];   // 1 MiB — cabe no mapeamento de usuario da DLL
static size_t_ g_comoff = 0;
static void* arena_alloc(size_t_ cb) {
    cb = (cb + 15) & ~(size_t_)15;
    if (!cb || g_comoff + cb > sizeof(g_comheap)) return 0;
    void* p = &g_comheap[g_comoff]; g_comoff += cb; return p;
}

__declspec(dllexport) void* CoTaskMemAlloc(size_t_ cb) { return arena_alloc(cb); }
__declspec(dllexport) void  CoTaskMemFree(void* p) { (void)p; }                                  // bump: nao libera
__declspec(dllexport) void* CoTaskMemRealloc(void* p, size_t_ cb) { (void)p; return arena_alloc(cb); }

// ---- invocacao de metodos IUnknown via vtable (p/ marshaling de identidade) ----
static HRESULT_ unk_qi(void* p, const GUID_* iid, void** ppv) {
    void*** o = (void***)p; typedef HRESULT_ (*qi_t)(void*, const GUID_*, void**);
    return ((qi_t)(o[0][0]))(p, iid, ppv);
}

// ====================================================================
//  Objeto COM UNIVERSAL — QI devolve a si mesmo p/ qualquer IID; metodos
//  nao-IUnknown devolvem E_NOTIMPL. Vtable de 64 slots (cobre interfaces
//  grandes: IShellFolder, IServiceProvider, etc.). O explorer chama e degrada.
// ====================================================================
static HRESULT_ univ_QI(void* this_, const GUID_* riid, void** ppv) { (void)riid; if (!ppv) return E_POINTER_; *ppv = this_; return S_OK_; }
static ULONG_   univ_AddRef(void* this_)  { (void)this_; return 2; }
static ULONG_   univ_Release(void* this_) { (void)this_; return 1; }
static HRESULT_ ret_notimpl(void* this_)  { (void)this_; return E_NOTIMPL_; }   // slots 3..63
static void*    g_univ_vtbl[64];
static struct { void** lpVtbl; } g_univ_obj = { 0 };
static int      g_univ_ready = 0;
static void univ_init(void) {
    if (g_univ_ready) return;
    g_univ_vtbl[0] = (void*)univ_QI;
    g_univ_vtbl[1] = (void*)univ_AddRef;
    g_univ_vtbl[2] = (void*)univ_Release;
    for (int i = 3; i < 64; i++) g_univ_vtbl[i] = (void*)ret_notimpl;
    g_univ_obj.lpVtbl = g_univ_vtbl;
    g_univ_ready = 1;
}
static void* universal_object(void) { univ_init(); return &g_univ_obj; }

// ====================================================================
//  IMalloc real — sobre o alocador de tarefa.
// ====================================================================
static HRESULT_ imalloc_QI(void* this_, const GUID_* riid, void** ppv) { (void)riid; if (!ppv) return E_POINTER_; *ppv = this_; return S_OK_; }
static ULONG_   imalloc_AddRef(void* this_)  { (void)this_; return 2; }
static ULONG_   imalloc_Release(void* this_) { (void)this_; return 1; }
static void*    imalloc_Alloc(void* this_, size_t_ cb) { (void)this_; return arena_alloc(cb); }
static void*    imalloc_Realloc(void* this_, void* pv, size_t_ cb) { (void)this_; (void)pv; return arena_alloc(cb); }
static void     imalloc_Free(void* this_, void* pv) { (void)this_; (void)pv; }
static size_t_  imalloc_GetSize(void* this_, void* pv) { (void)this_; (void)pv; return (size_t_)-1; }
static int      imalloc_DidAlloc(void* this_, void* pv) { (void)this_; (void)pv; return -1; }   // nao sei
static void     imalloc_HeapMinimize(void* this_) { (void)this_; }
static void*    g_imalloc_vtbl[9];
static struct { void** lpVtbl; } g_imalloc = { 0 };
static int      g_imalloc_ready = 0;
static void imalloc_init(void) {
    if (g_imalloc_ready) return;
    g_imalloc_vtbl[0] = (void*)imalloc_QI;      g_imalloc_vtbl[1] = (void*)imalloc_AddRef;
    g_imalloc_vtbl[2] = (void*)imalloc_Release; g_imalloc_vtbl[3] = (void*)imalloc_Alloc;
    g_imalloc_vtbl[4] = (void*)imalloc_Realloc; g_imalloc_vtbl[5] = (void*)imalloc_Free;
    g_imalloc_vtbl[6] = (void*)imalloc_GetSize; g_imalloc_vtbl[7] = (void*)imalloc_DidAlloc;
    g_imalloc_vtbl[8] = (void*)imalloc_HeapMinimize;
    g_imalloc.lpVtbl = g_imalloc_vtbl; g_imalloc_ready = 1;
}

// ====================================================================
//  IStream em memoria (CreateStreamOnHGlobal). Buffer crescente na arena.
// ====================================================================
typedef struct { void** lpVtbl; unsigned char* buf; size_t_ cap, size, pos; long ref; } memstream_t;
static void* g_stream_vtbl[14];
static int   g_stream_ready = 0;
static int stream_ensure(memstream_t* s, size_t_ need) {
    if (need <= s->cap) return 1;
    size_t_ ncap = s->cap ? s->cap : 256;
    while (ncap < need) ncap *= 2;
    unsigned char* nb = (unsigned char*)arena_alloc(ncap);
    if (!nb) return 0;
    if (s->buf && s->size) bcopy_(nb, s->buf, s->size);
    s->buf = nb; s->cap = ncap; return 1;
}
static HRESULT_ stm_QI(void* this_, const GUID_* riid, void** ppv) { (void)riid; if (!ppv) return E_POINTER_; *ppv = this_; return S_OK_; }
static ULONG_   stm_AddRef(void* this_)  { memstream_t* s = (memstream_t*)this_; return (ULONG_)(++s->ref); }
static ULONG_   stm_Release(void* this_) { memstream_t* s = (memstream_t*)this_; long r = --s->ref; return (ULONG_)(r < 0 ? 0 : r); }
static HRESULT_ stm_Read(void* this_, void* pv, ULONG_ cb, ULONG_* pcbRead) {
    memstream_t* s = (memstream_t*)this_;
    size_t_ avail = (s->pos < s->size) ? (s->size - s->pos) : 0;
    size_t_ n = (cb < avail) ? cb : avail;
    if (n) bcopy_(pv, s->buf + s->pos, n);
    s->pos += n; if (pcbRead) *pcbRead = (ULONG_)n;
    return (n == cb) ? S_OK_ : S_FALSE_;
}
static HRESULT_ stm_Write(void* this_, const void* pv, ULONG_ cb, ULONG_* pcbWritten) {
    memstream_t* s = (memstream_t*)this_;
    if (cb) { if (!stream_ensure(s, s->pos + cb)) { if (pcbWritten) *pcbWritten = 0; return E_OUTOFMEMORY_; }
              bcopy_(s->buf + s->pos, pv, cb); s->pos += cb; if (s->pos > s->size) s->size = s->pos; }
    if (pcbWritten) *pcbWritten = cb; return S_OK_;
}
static HRESULT_ stm_Seek(void* this_, long long move, ULONG_ origin, unsigned long long* pnew) {
    memstream_t* s = (memstream_t*)this_;
    long long base = (origin == 1) ? (long long)s->pos : (origin == 2) ? (long long)s->size : 0;   // 0=SET 1=CUR 2=END
    long long np = base + move; if (np < 0) return E_INVALIDARG_;
    s->pos = (size_t_)np; if (pnew) *pnew = (unsigned long long)np; return S_OK_;
}
static HRESULT_ stm_SetSize(void* this_, unsigned long long newsize) {
    memstream_t* s = (memstream_t*)this_;
    if (!stream_ensure(s, (size_t_)newsize)) return E_OUTOFMEMORY_;
    s->size = (size_t_)newsize; return S_OK_;
}
static HRESULT_ stm_CopyTo(void* this_, void* pstm, unsigned long long cb, unsigned long long* pread, unsigned long long* pwritten) {
    memstream_t* s = (memstream_t*)this_;
    size_t_ avail = (s->pos < s->size) ? (s->size - s->pos) : 0;
    size_t_ n = ((size_t_)cb < avail) ? (size_t_)cb : avail;
    ULONG_ wrote = 0;
    if (n && pstm) { typedef HRESULT_ (*wr_t)(void*, const void*, ULONG_, ULONG_*);
        void*** o = (void***)pstm; ((wr_t)(o[0][4]))(pstm, s->buf + s->pos, (ULONG_)n, &wrote); }
    s->pos += n;
    if (pread) *pread = n; if (pwritten) *pwritten = wrote; return S_OK_;
}
static HRESULT_ stm_Commit(void* this_, ULONG_ f) { (void)this_; (void)f; return S_OK_; }
static HRESULT_ stm_Revert(void* this_) { (void)this_; return S_OK_; }
static HRESULT_ stm_LockRegion(void* this_, unsigned long long off, unsigned long long cb, ULONG_ t) { (void)this_;(void)off;(void)cb;(void)t; return E_NOTIMPL_; }
static HRESULT_ stm_UnlockRegion(void* this_, unsigned long long off, unsigned long long cb, ULONG_ t) { (void)this_;(void)off;(void)cb;(void)t; return E_NOTIMPL_; }
// STATSTG (x64, 80 bytes): pwcsName(8) type(4)+pad cbSize(8) mtime(8) ctime(8) atime(8)
//   grfMode(4) grfLocksSupported(4) clsid(16) grfStateBits(4) reserved(4).
static HRESULT_ stm_Stat(void* this_, void* pstatstg, ULONG_ flag) {
    memstream_t* s = (memstream_t*)this_; (void)flag;
    if (!pstatstg) return E_POINTER_;
    unsigned char* st = (unsigned char*)pstatstg; bzero_(st, 80);
    *(ULONG_*)(st + 8) = 2;                              // type = STGTY_STREAM
    *(unsigned long long*)(st + 16) = (unsigned long long)s->size;   // cbSize
    return S_OK_;
}
static memstream_t* stream_new(void);
static HRESULT_ stm_Clone(void* this_, void** ppstm) {
    memstream_t* s = (memstream_t*)this_;
    if (!ppstm) return E_POINTER_;
    memstream_t* c = stream_new(); if (!c) return E_OUTOFMEMORY_;
    if (s->size) { if (!stream_ensure(c, s->size)) return E_OUTOFMEMORY_; bcopy_(c->buf, s->buf, s->size); c->size = s->size; }
    c->pos = s->pos; *ppstm = c; return S_OK_;
}
static void stream_init_vtbl(void) {
    if (g_stream_ready) return;
    g_stream_vtbl[0]=(void*)stm_QI;        g_stream_vtbl[1]=(void*)stm_AddRef;    g_stream_vtbl[2]=(void*)stm_Release;
    g_stream_vtbl[3]=(void*)stm_Read;      g_stream_vtbl[4]=(void*)stm_Write;     g_stream_vtbl[5]=(void*)stm_Seek;
    g_stream_vtbl[6]=(void*)stm_SetSize;   g_stream_vtbl[7]=(void*)stm_CopyTo;    g_stream_vtbl[8]=(void*)stm_Commit;
    g_stream_vtbl[9]=(void*)stm_Revert;    g_stream_vtbl[10]=(void*)stm_LockRegion; g_stream_vtbl[11]=(void*)stm_UnlockRegion;
    g_stream_vtbl[12]=(void*)stm_Stat;     g_stream_vtbl[13]=(void*)stm_Clone;
    g_stream_ready = 1;
}
static memstream_t* stream_new(void) {
    stream_init_vtbl();
    memstream_t* s = (memstream_t*)arena_alloc(sizeof(memstream_t));
    if (!s) return 0;
    s->lpVtbl = g_stream_vtbl; s->buf = 0; s->cap = 0; s->size = 0; s->pos = 0; s->ref = 1;
    return s;
}
__declspec(dllexport) HRESULT_ CreateStreamOnHGlobal(void* hGlobal, int fDeleteOnRelease, void** ppstm) {
    (void)hGlobal; (void)fDeleteOnRelease;
    if (!ppstm) return E_POINTER_;
    memstream_t* s = stream_new(); if (!s) return E_OUTOFMEMORY_;
    *ppstm = s; return S_OK_;
}

// ====================================================================
//  Init de apartamento / seguranca — no-op honesto (processo unico, sem DCOM).
// ====================================================================
__declspec(dllexport) HRESULT_ CoInitialize(void* rsv) { (void)rsv; return S_OK_; }
__declspec(dllexport) HRESULT_ CoInitializeEx(void* rsv, unsigned f) { (void)rsv; (void)f; return S_OK_; }
__declspec(dllexport) void     CoUninitialize(void) { }
__declspec(dllexport) HRESULT_ CoInitializeSecurity(void* sd, long cAuth, void* asAuth, void* rsv1, unsigned lvl, unsigned imp, void* authList, unsigned caps, void* rsv3) { (void)sd;(void)cAuth;(void)asAuth;(void)rsv1;(void)lvl;(void)imp;(void)authList;(void)caps;(void)rsv3; return S_OK_; }
__declspec(dllexport) HRESULT_ CoGetApartmentType(int* pAptType, int* pAptQualifier) { if (pAptType) *pAptType = 0; if (pAptQualifier) *pAptQualifier = 0; return S_OK_; }   // APTTYPE_STA
__declspec(dllexport) HRESULT_ CoIncrementMTAUsage(void** pCookie) { if (pCookie) *pCookie = (void*)(size_t_)1; return S_OK_; }
__declspec(dllexport) void     CoFreeUnusedLibraries(void) { }
__declspec(dllexport) HRESULT_ CoDisableCallCancellation(void* rsv) { (void)rsv; return S_OK_; }
__declspec(dllexport) HRESULT_ CoEnableCallCancellation(void* rsv) { (void)rsv; return S_OK_; }
__declspec(dllexport) HRESULT_ CoCancelCall(unsigned long tid, unsigned long ms) { (void)tid; (void)ms; return S_OK_; }
__declspec(dllexport) HRESULT_ CoSetProxyBlanket(void* proxy, unsigned dwAuthnSvc, unsigned dwAuthzSvc, void* srv, unsigned lvl, unsigned imp, void* auth, unsigned caps) { (void)proxy;(void)dwAuthnSvc;(void)dwAuthzSvc;(void)srv;(void)lvl;(void)imp;(void)auth;(void)caps; return S_OK_; }
__declspec(dllexport) HRESULT_ CoGetCallContext(const GUID_* riid, void** ppv) { (void)riid; if (ppv) *ppv = 0; return E_NOINTERFACE_; }
__declspec(dllexport) HRESULT_ CoGetObjectContext(const GUID_* riid, void** ppv) { (void)riid; if (ppv) *ppv = 0; return E_NOTIMPL_; }

// ---- registro de class object (out-of-proc) — no-op c/ cookie valido ----
static unsigned long g_class_cookie = 0x100;
__declspec(dllexport) HRESULT_ CoRegisterClassObject(const GUID_* rclsid, void* pUnk, unsigned ctx, unsigned flags, unsigned long* lpdw) { (void)rclsid;(void)pUnk;(void)ctx;(void)flags; if (lpdw) *lpdw = ++g_class_cookie; return S_OK_; }
__declspec(dllexport) HRESULT_ CoRevokeClassObject(unsigned long dw) { (void)dw; return S_OK_; }
__declspec(dllexport) HRESULT_ CoRegisterMessageFilter(void* pNew, void** ppOld) { (void)pNew; if (ppOld) *ppOld = 0; return S_OK_; }
__declspec(dllexport) HRESULT_ CoWaitForMultipleHandles(unsigned flags, unsigned long ms, unsigned long cH, void* pH, unsigned long* lpIdx) { (void)flags;(void)ms;(void)cH;(void)pH; if (lpIdx) *lpIdx = 0; return S_OK_; }   // handle 0 "sinalizado"

// ====================================================================
//  IMalloc / CoCreateInstance.
// ====================================================================
__declspec(dllexport) HRESULT_ CoGetMalloc(unsigned ctx, void** ppMalloc) { (void)ctx; if (!ppMalloc) return E_POINTER_; imalloc_init(); *ppMalloc = &g_imalloc; return S_OK_; }
__declspec(dllexport) HRESULT_ CoCreateInstance(const GUID_* clsid, void* outer, unsigned ctx, const GUID_* iid, void** ppv) {
    (void)clsid; (void)outer; (void)ctx; (void)iid;
    if (!ppv) return E_POINTER_;
    *ppv = universal_object();     // ponteiro NAO-NULO c/ vtable; metodos -> E_NOTIMPL (degrada)
    return S_OK_;
}

// ====================================================================
//  Marshaling de IDENTIDADE (mesmo processo/apartamento): grava o ponteiro da
//  interface no stream; ao desmarshalar, devolve o MESMO ponteiro (via QI).
// ====================================================================
__declspec(dllexport) HRESULT_ CoMarshalInterThreadInterfaceInStream(const GUID_* riid, void* pUnk, void** ppStm) {
    (void)riid; if (!ppStm) return E_POINTER_;
    memstream_t* s = stream_new(); if (!s) return E_OUTOFMEMORY_;
    ULONG_ wrote = 0; stm_Write(s, &pUnk, (ULONG_)sizeof(void*), &wrote);
    s->pos = 0; *ppStm = s; return S_OK_;
}
__declspec(dllexport) HRESULT_ CoGetInterfaceAndReleaseStream(void* pStm, const GUID_* riid, void** ppv) {
    if (!ppv) return E_POINTER_; *ppv = 0;
    if (!pStm) return E_INVALIDARG_;
    void* pUnk = 0; ULONG_ rd = 0;
    typedef HRESULT_ (*rd_t)(void*, void*, ULONG_, ULONG_*); typedef ULONG_ (*rel_t)(void*);
    void*** o = (void***)pStm;
    ((rd_t)(o[0][3]))(pStm, &pUnk, (ULONG_)sizeof(void*), &rd);   // Read
    ((rel_t)(o[0][2]))(pStm);                                     // Release stream
    if (rd < sizeof(void*) || !pUnk) return E_FAIL_;
    return unk_qi(pUnk, riid, ppv);
}
__declspec(dllexport) HRESULT_ CoReleaseMarshalData(void* pStm) { (void)pStm; return S_OK_; }
__declspec(dllexport) HRESULT_ CoGetStdMarshalEx(void* pUnkOuter, unsigned smexflags, void** ppUnkInner) { (void)pUnkOuter; (void)smexflags; if (!ppUnkInner) return E_POINTER_; *ppUnkInner = universal_object(); return S_OK_; }
__declspec(dllexport) HRESULT_ CoCreateFreeThreadedMarshaler(void* pUnkOuter, void** ppunkMarshal) { (void)pUnkOuter; if (!ppunkMarshal) return E_POINTER_; *ppunkMarshal = universal_object(); return S_OK_; }
__declspec(dllexport) HRESULT_ RoGetAgileReference(unsigned opts, const GUID_* riid, void* pUnk, void** ppAgileReference) { (void)opts;(void)riid;(void)pUnk; if (ppAgileReference) *ppAgileReference = 0; return E_NOTIMPL_; }

// ====================================================================
//  GUID <-> string.
// ====================================================================
static WCHAR_ hexU(unsigned v) { v &= 0xF; return (WCHAR_)(v < 10 ? ('0' + v) : ('A' + v - 10)); }
static int    hexV(WCHAR_ c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }

__declspec(dllexport) int StringFromGUID2(const GUID_* g, WCHAR_* out, int cch) {
    if (!g || !out || cch < 39) return 0;
    int i = 0; out[i++] = '{';
    for (int s = 28; s >= 0; s -= 4) out[i++] = hexU(g->Data1 >> s);
    out[i++] = '-';
    for (int s = 12; s >= 0; s -= 4) out[i++] = hexU((unsigned)g->Data2 >> s);
    out[i++] = '-';
    for (int s = 12; s >= 0; s -= 4) out[i++] = hexU((unsigned)g->Data3 >> s);
    out[i++] = '-';
    out[i++] = hexU(g->Data4[0] >> 4); out[i++] = hexU(g->Data4[0]);
    out[i++] = hexU(g->Data4[1] >> 4); out[i++] = hexU(g->Data4[1]);
    out[i++] = '-';
    for (int k = 2; k < 8; k++) { out[i++] = hexU(g->Data4[k] >> 4); out[i++] = hexU(g->Data4[k]); }
    out[i++] = '}'; out[i++] = 0;
    return i;   // 39 (inclui o terminador)
}
static HRESULT_ guid_from_str(const WCHAR_* s, GUID_* g) {
    if (!g) return E_INVALIDARG_;
    bzero_(g, sizeof(*g));
    if (!s) return S_OK_;                          // NULL -> GUID_NULL (igual ao Windows)
    const WCHAR_* p = s; if (*p == '{') p++;
    unsigned long d1 = 0; for (int k = 0; k < 8; k++) { int h = hexV(*p++); if (h < 0) return CO_E_CLASSSTRING; d1 = (d1 << 4) | (unsigned)h; }
    if (*p++ != '-') return CO_E_CLASSSTRING;
    unsigned d2 = 0; for (int k = 0; k < 4; k++) { int h = hexV(*p++); if (h < 0) return CO_E_CLASSSTRING; d2 = (d2 << 4) | (unsigned)h; }
    if (*p++ != '-') return CO_E_CLASSSTRING;
    unsigned d3 = 0; for (int k = 0; k < 4; k++) { int h = hexV(*p++); if (h < 0) return CO_E_CLASSSTRING; d3 = (d3 << 4) | (unsigned)h; }
    if (*p++ != '-') return CO_E_CLASSSTRING;
    unsigned char d4[8];
    for (int k = 0; k < 2; k++) { int hi = hexV(*p++), lo = hexV(*p++); if (hi < 0 || lo < 0) return CO_E_CLASSSTRING; d4[k] = (unsigned char)((hi << 4) | lo); }
    if (*p++ != '-') return CO_E_CLASSSTRING;
    for (int k = 2; k < 8; k++) { int hi = hexV(*p++), lo = hexV(*p++); if (hi < 0 || lo < 0) return CO_E_CLASSSTRING; d4[k] = (unsigned char)((hi << 4) | lo); }
    g->Data1 = (unsigned int)d1; g->Data2 = (unsigned short)d2; g->Data3 = (unsigned short)d3;
    for (int k = 0; k < 8; k++) g->Data4[k] = d4[k];
    return S_OK_;
}
__declspec(dllexport) HRESULT_ CLSIDFromString(const WCHAR_* s, GUID_* g) { return guid_from_str(s, g); }
__declspec(dllexport) HRESULT_ IIDFromString(const WCHAR_* s, GUID_* g) { return guid_from_str(s, g); }
static HRESULT_ str_from_guid(const GUID_* g, WCHAR_** out) {
    if (!out) return E_INVALIDARG_;
    WCHAR_* s = (WCHAR_*)arena_alloc(39 * sizeof(WCHAR_)); if (!s) { *out = 0; return E_OUTOFMEMORY_; }
    if (!StringFromGUID2(g, s, 39)) { *out = 0; return E_FAIL_; }
    *out = s; return S_OK_;
}
__declspec(dllexport) HRESULT_ StringFromCLSID(const GUID_* g, WCHAR_** out) { return str_from_guid(g, out); }
__declspec(dllexport) HRESULT_ StringFromIID(const GUID_* g, WCHAR_** out)   { return str_from_guid(g, out); }

// CoCreateGuid: GUID v4 pseudo-aleatorio (rdtsc + contador). Suficiente p/ identificadores.
static unsigned long long g_guid_ctr = 0x123456789ABCDEFULL;
static unsigned long long rdtsc_(void) { unsigned hi, lo; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
__declspec(dllexport) HRESULT_ CoCreateGuid(GUID_* pguid) {
    if (!pguid) return E_INVALIDARG_;
    unsigned long long a = rdtsc_() ^ (g_guid_ctr * 0x9E3779B97F4A7C15ULL);
    g_guid_ctr += 0xD1B54A32D192ED03ULL;
    unsigned long long b = rdtsc_() + g_guid_ctr;
    a ^= a >> 33; a *= 0xFF51AFD7ED558CCDULL; a ^= a >> 33;
    b ^= b >> 29; b *= 0xC4CEB9FE1A85EC53ULL; b ^= b >> 32;
    pguid->Data1 = (unsigned int)a;
    pguid->Data2 = (unsigned short)(a >> 32);
    pguid->Data3 = (unsigned short)(((a >> 48) & 0x0FFF) | 0x4000);         // versao 4
    pguid->Data4[0] = (unsigned char)(((b) & 0x3F) | 0x80);                 // variante RFC 4122
    for (int k = 1; k < 8; k++) pguid->Data4[k] = (unsigned char)(b >> (8 * k));
    return S_OK_;
}

// PropVariantClear: zera o PROPVARIANT (24 bytes no x64) -> VT_EMPTY. As alocacoes
// contidas nao sao liberadas (arena bump), mas o valor fica limpo (sem crash/uso indevido).
__declspec(dllexport) HRESULT_ PropVariantClear(void* pvar) { if (pvar) bzero_(pvar, 24); return S_OK_; }

int DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }
