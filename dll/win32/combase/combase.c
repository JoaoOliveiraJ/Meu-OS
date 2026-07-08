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

// ---- Log de bring-up SEMPRE ATIVO (baixo ruido — dispara pontualmente na escada do host
// de persistencia). SYS_WRITE (rax=1, rdi=string) via int 0x80. Combase segue autocontida. ----
static void cb_log(const char* s) { unsigned long long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(1ULL), "D"(s) : "memory","rcx","r11"); }
static void cb_loghex(unsigned long long v) { char b[19]; b[0]='0'; b[1]='x'; const char* h="0123456789ABCDEF"; for (int i=0;i<16;i++) b[2+i]=h[(v>>((15-i)*4))&0xF]; b[18]=0; cb_log(b); }
// Syscalls do win32k p/ o LOOP DE MENSAGENS do DesktopExplorerHost (persistencia do explorer).
#define CB_SYS_GETMESSAGE_      21
#define CB_SYS_DISPATCHMESSAGE_ 22
static long cb_getmessage(void* msg) { long long r; __asm__ volatile ("int $0x80":"=a"(r):"a"((long long)CB_SYS_GETMESSAGE_),"D"((long long)(__INTPTR_TYPE__)msg):"memory","rcx","r11"); return (long)r; }
static void cb_dispatchmessage(void* msg) { long long r; __asm__ volatile ("int $0x80":"=a"(r):"a"((long long)CB_SYS_DISPATCHMESSAGE_),"D"((long long)(__INTPTR_TYPE__)msg):"memory","rcx","r11"); }

// ---- DIAGNOSTICO de bring-up (TEMPORARIO): escreve na serial via SYS_WRITE (rax=1,
// rdi=string) por int 0x80. Sem import -> a combase segue autocontida. Loga qual objeto
// COM/WinRT o explorer pede, p/ descobrir a decisao de saida. Remover depois.
#define COMBASE_DBG 0   // 1 = loga CLSID/IID/classe-WinRT + faz getters do obj universal
                        // devolverem objetos (SCAFFOLD de RE do MRT: empurra o explorer
                        // ALEM da init de recursos p/ mapear os proximos muros). Ver
                        // PROMPT-PROXIMA-SESSAO.md (sequencia slot 7->6->8/9 + GetUserNameExW).
#if COMBASE_DBG
static void dbg_puts(const char* s) { unsigned long long ret; __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(1ULL), "D"(s) : "memory", "rcx", "r11"); }
static const char g_hx[] = "0123456789ABCDEF";
static void dbg_guid(const char* label, const GUID_* g) {
    char b[96]; int i = 0; for (const char* p = label; *p; p++) b[i++] = *p;
    if (g) { b[i++]='{';
        for (int s=28;s>=0;s-=4) b[i++]=g_hx[(g->Data1>>s)&0xF]; b[i++]='-';
        for (int s=12;s>=0;s-=4) b[i++]=g_hx[((unsigned)g->Data2>>s)&0xF]; b[i++]='-';
        for (int s=12;s>=0;s-=4) b[i++]=g_hx[((unsigned)g->Data3>>s)&0xF]; b[i++]='-';
        b[i++]=g_hx[g->Data4[0]>>4]; b[i++]=g_hx[g->Data4[0]&0xF];
        b[i++]=g_hx[g->Data4[1]>>4]; b[i++]=g_hx[g->Data4[1]&0xF]; b[i++]='-';
        for (int k=2;k<8;k++){ b[i++]=g_hx[g->Data4[k]>>4]; b[i++]=g_hx[g->Data4[k]&0xF]; }
        b[i++]='}';
    } else { b[i++]='N'; b[i++]='U'; b[i++]='L'; b[i++]='L'; }
    b[i++]='\n'; b[i++]=0; dbg_puts(b);
}
static void dbg_wstr(const char* label, const unsigned short* w, unsigned wlen) {
    char b[160]; int i = 0; for (const char* p = label; *p; p++) b[i++] = *p;
    if (w) { unsigned n = wlen < 100 ? wlen : 100; for (unsigned k = 0; k < n; k++) { unsigned short c = w[k]; b[i++] = (c >= 32 && c < 127) ? (char)c : '.'; } }
    else { b[i++]='N'; b[i++]='U'; b[i++]='L'; b[i++]='L'; }
    b[i++]='\n'; b[i++]=0; dbg_puts(b);
}
static void dbg_slot2(int n, int pos) { char b[32]; int i=0; const char* p="[cb] univ slot "; while(*p)b[i++]=*p++; if(n>=10)b[i++]=(char)('0'+n/10); b[i++]=(char)('0'+n%10); const char* q=" out@a"; while(*q)b[i++]=*q++; b[i++]=(char)('0'+pos); b[i++]='\n'; b[i++]=0; dbg_puts(b); }
// Loga o RETURN ADDRESS de quem chamou (o codigo do explorer logo APOS a chamada do metodo).
// Serve p/ pinpointar o site do THROW_IF_FAILED: quando o slot 3 devolve E_NOTIMPL, esse RA e'
// exatamente a instrucao do explorer que testa o HRESULT e (se falho) chama o wil throw helper.
static void dbg_ra(const char* label, void* ra) {
    char b[64]; int i=0; for (const char* p=label; *p; p++) b[i++]=*p;
    b[i++]='0'; b[i++]='x'; unsigned long long v=(unsigned long long)ra;
    for (int s=60; s>=0; s-=4) b[i++]=g_hx[(v>>s)&0xF];
    b[i++]='\n'; b[i++]=0; dbg_puts(b);
}
// Qual thread (do explorer) esta chamando: a pilha da PRINCIPAL vive em [0x600000,0x700000);
// as threads ring-3 do CreateThread/SHCreateThread ganham pilha na faixa do PMM (>= 0x4000000).
// Isto revela se a init pesada do shell roda na thread PRINCIPAL ou numa WORKER.
static void dbg_thread(void) { unsigned long long rsp; __asm__ volatile("mov %%rsp,%0":"=r"(rsp)); dbg_puts((rsp>=0x600000ULL && rsp<0x700000ULL) ? "[cb] <thread=MAIN>\n" : "[cb] <thread=WORKER>\n"); }
#else
#define dbg_guid(a,b) ((void)0)
#define dbg_wstr(a,b,c) ((void)0)
#endif

// ---- Preenchimento de out-param dos getters do objeto universal (comportamento REAL,
// NAO diagnostico). Um getter COM/WinRT devolve a interface por um out-param T** que fica
// em arg2 (RDX, getter comum) OU num arg POSTERIOR — ex.: um metodo de mapa MRT tem forma
// M(this, iface, 0, 0, &out), com out em arg5 (stack). Se so preenchessemos RDX, o out
// real ficaria NULO e o explorer derefaria NULL adiante (era o muro do MRT). Aqui achamos
// o out-param REAL: o 1o dos args a2..a5 que aponta p/ um qword ZERADO, alinhado, em faixa
// de usuario (a convencao COM zera o out ANTES da chamada; um input nunca e' um ponteiro
// -p/-zero-alinhado valido). Escrevemos ali o proprio objeto universal, cujos metodos por
// sua vez degradam igual. E' um preenchedor ESPECIFICO do objeto universal (nao um
// catch-all de DLL): da' ao explorer uma interface WinRT valida em vez de NULL.
static void* universal_object(void);   // fwd (definido na secao do objeto universal)
static int cb_out_ok(void* p) { unsigned long long v=(unsigned long long)p; return v>=0x10000ULL && v<0x10000000ULL && ((v&7ULL)==0ULL); }
static long univ_fill(void* self, void** a2, void** a3, void** a4, void** a5) {
    (void)self;
    void** cands[4] = { a2, a3, a4, a5 };
    for (int i = 0; i < 4; i++) { void** p = cands[i]; if (cb_out_ok(p) && *p == 0) { *p = universal_object(); return 0; } }
    return 0;
}
#if COMBASE_DBG
// Versoes que LOGAM o slot chamado e delegam ao MESMO univ_fill do caminho real.
#define USLI(n) static long usl##n(void* t){(void)t; dbg_slot2(n,0); dbg_ra("[cb]   slot"#n" RA=", __builtin_return_address(0)); return (long)0x80004001L;}
USLI(3)USLI(4)USLI(5)
#define USL(n) static long usl##n(void* s,void**a2,void**a3,void**a4,void**a5){ long r=univ_fill(s,a2,a3,a4,a5); dbg_slot2(n,0); return r; }
USL(6)USL(7)USL(8)USL(9)USL(10)USL(11)USL(12)USL(13)USL(14)USL(15)USL(16)USL(17)USL(18)USL(19)USL(20)USL(21)USL(22)USL(23)USL(24)
#endif

// ---- arena bump (task memory + objetos + buffers de stream + HSTRING). Sem free real. ----
static unsigned char g_comheap[0x200000];   // 2 MiB — WinRT cria muitas HSTRING na init
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
static HRESULT_ ret_notimpl(void* this_)  { (void)this_; return E_NOTIMPL_; }   // IInspectable 3..5
static void*    g_univ_vtbl[64];
static struct { void** lpVtbl; } g_univ_obj = { 0 };
static int      g_univ_ready = 0;
static void univ_init(void) {
    if (g_univ_ready) return;
    g_univ_vtbl[0] = (void*)univ_QI;
    g_univ_vtbl[1] = (void*)univ_AddRef;
    g_univ_vtbl[2] = (void*)univ_Release;
    // IInspectable (3-5: GetIids/GetRuntimeClassName/GetTrustLevel): E_NOTIMPL (seguro —
    // NAO preenchemos: os outs sao um count/HSTRING/TrustLevel, nao uma interface).
    for (int i = 3; i < 6; i++)  g_univ_vtbl[i] = (void*)ret_notimpl;
    // Metodos de interface (6+): getters — preenchem o out-param REAL e devolvem S_OK, de
    // modo que o explorer recebe uma interface WinRT valida (e nao NULL) em cada slot.
    for (int i = 6; i < 64; i++) g_univ_vtbl[i] = (void*)univ_fill;
#if COMBASE_DBG
    g_univ_vtbl[3]=(void*)usl3; g_univ_vtbl[4]=(void*)usl4; g_univ_vtbl[5]=(void*)usl5; g_univ_vtbl[6]=(void*)usl6;
    g_univ_vtbl[7]=(void*)usl7; g_univ_vtbl[8]=(void*)usl8; g_univ_vtbl[9]=(void*)usl9; g_univ_vtbl[10]=(void*)usl10;
    g_univ_vtbl[11]=(void*)usl11; g_univ_vtbl[12]=(void*)usl12; g_univ_vtbl[13]=(void*)usl13; g_univ_vtbl[14]=(void*)usl14;
    g_univ_vtbl[15]=(void*)usl15; g_univ_vtbl[16]=(void*)usl16; g_univ_vtbl[17]=(void*)usl17; g_univ_vtbl[18]=(void*)usl18; g_univ_vtbl[19]=(void*)usl19;
    g_univ_vtbl[20]=(void*)usl20; g_univ_vtbl[21]=(void*)usl21; g_univ_vtbl[22]=(void*)usl22; g_univ_vtbl[23]=(void*)usl23; g_univ_vtbl[24]=(void*)usl24;
#endif
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
//  Objetos COM ESPECIFICOS do shell (por CLSID) — o jeito CORRETO (NAO catch-all)
//  de fazer a init do explorer CONCLUIR. Ao contrario do objeto UNIVERSAL, cada
//  CLSID aqui tem vtable/semantica proprias e out-params VALIDOS.
//
//  CLSID_TaskbandPin {90AA3A4E-1CBA-4233-B8BB-535773D48449} implementa IPinnedList3
//  {0DD79AE2-...}: a LISTA de itens FIXADOS na barra de tarefas. Num perfil NOVO ela e'
//  VAZIA. O worker do explorer (RVA 0x1A500, na cadeia do callback 0x2C5BC) faz:
//     CoCreateInstance(TaskbandPin, IPinnedList3) -> pl->slot3(&enum)   [THROW_IF_FAILED]
//     pl->QueryInterface(IID_IPinnedList {60274FA2}, &pl2)              [THROW_IF_FAILED]
//     loop: enum->slot3(1, &ctrl, 0)  (pega proximo item)              [THROW_IF_FAILED]
//  Antes, slot3 do objeto UNIVERSAL devolvia E_NOTIMPL -> THROW_IF_FAILED -> throw ->
//  o callback NAO concluia (GLOBAL_C [rip+0x3ad16c] ficava 0 -> rdi=0 -> teardown).
//  Agora: slot3 devolve S_OK + um ENUMERADOR; enum->slot3 devolve S_FALSE (sem itens),
//  e o loop de reconciliacao termina a lista VAZIA sem lancar. Contrato observado
//  (0x1A500): apos enum->slot3, `cmp ebx,1; jne 0x1a90d` — ebx==1 (S_FALSE) cai no
//  caminho "lista terminada / 0 itens" (0x1a6a4); ebx==0 (S_OK) tentaria PROCESSAR um
//  item inexistente. Por isso o enum devolve S_FALSE. O byte "hasValue" (pctrl+8) e'
//  zerado (o caller checa `cmp [rsp+0xc0],0` p/ decidir se libera o item anterior).
// ====================================================================
static int guid_eq(const GUID_* a, const GUID_* b) {
    const unsigned char* x = (const unsigned char*)a; const unsigned char* y = (const unsigned char*)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static const GUID_ CLSID_TaskbandPin = { 0x90AA3A4E, 0x1CBA, 0x4233, { 0xB8,0xBB,0x53,0x57,0x73,0xD4,0x84,0x49 } };

// ---- Enumerador VAZIO da lista fixada (o out-param de IPinnedList3::slot3). ----
static long pe_QI(void* self, const GUID_* riid, void** ppv) { (void)riid; if (!ppv) return E_POINTER_; *ppv = self; return S_OK_; }
static ULONG_ pe_AddRef(void* s) { (void)s; return 2; }
static ULONG_ pe_Release(void* s) { (void)s; return 1; }
// enum->slot3(this, count, pctrl, flags): SEM itens. Zera o "hasValue" (byte em pctrl+8) e
// devolve S_FALSE(1) -> a reconciliacao em 0x1A500 conclui a lista VAZIA sem lancar.
static long pe_next(void* self, unsigned count, void* pctrl, void* flags) {
    (void)self; (void)count; (void)flags;
    if (pctrl) *((volatile unsigned char*)pctrl + 8) = 0;
    return S_FALSE_;
}
static void* g_pe_vtbl[64];
static struct { void** lpVtbl; } g_pe_obj = { 0 };
static int g_pe_ready = 0;
static void* pinnedenum_object(void) {
    if (!g_pe_ready) {
        g_pe_vtbl[0] = (void*)pe_QI; g_pe_vtbl[1] = (void*)pe_AddRef; g_pe_vtbl[2] = (void*)pe_Release;
        g_pe_vtbl[3] = (void*)pe_next;
        for (int i = 4; i < 64; i++) g_pe_vtbl[i] = (void*)univ_fill;   // demais getters -> out valido + S_OK
        g_pe_obj.lpVtbl = g_pe_vtbl; g_pe_ready = 1;
    }
    return &g_pe_obj;
}
// ---- IPinnedList3 (CLSID_TaskbandPin): lista de fixados (VAZIA num perfil novo). ----
static long pl_QI(void* self, const GUID_* riid, void** ppv) { (void)riid; if (!ppv) return E_POINTER_; *ppv = self; return S_OK_; }
static ULONG_ pl_AddRef(void* s) { (void)s; return 2; }
static ULONG_ pl_Release(void* s) { (void)s; return 1; }
// IPinnedList3::slot3(this, out): devolve o enumerador VAZIO. S_OK (nao lanca).
static long pl_slot3(void* self, void** out) { (void)self; if (out) *out = pinnedenum_object(); return S_OK_; }
static void* g_pl_vtbl[64];
static struct { void** lpVtbl; } g_pl_obj = { 0 };
static int g_pl_ready = 0;
static void* pinnedlist_object(void) {
    if (!g_pl_ready) {
        g_pl_vtbl[0] = (void*)pl_QI; g_pl_vtbl[1] = (void*)pl_AddRef; g_pl_vtbl[2] = (void*)pl_Release;
        g_pl_vtbl[3] = (void*)pl_slot3;
        for (int i = 4; i < 64; i++) g_pl_vtbl[i] = (void*)univ_fill;   // slots 11/15/16/18/19 etc.
        g_pl_obj.lpVtbl = g_pl_vtbl; g_pl_ready = 1;
    }
    return &g_pl_obj;
}
// ====================================================================
//  ESCADA DE PERSISTENCIA do wWinMain (mode 3), quando rdi != 0 (0x23ec2):
//    CoCreateInstance(CLSID_ExplorerHostCreator {AB0B37EC}) -> obj em [rbp+0x2b0]
//    obj->slot3 (vtbl+0x18, "Create")({682159D9}, ...) -> DesktopExplorerHost em [rsp+0x58]
//    host->slot5 (vtbl+0x28)  [init]   e   host->slot10 (vtbl+0x50)  [UM roda o loop de msg]
//  Implementamos os DOIS como objetos COM ESPECIFICOS (por CLSID), NAO pelo universal:
//    - DesktopExplorerHost::slot5  -> S_OK (init no-op)
//    - DesktopExplorerHost::slot10 -> LOOP DE MENSAGENS BLOQUEANTE (GetMessage/DispatchMessage
//      via syscall do win32k) ate WM_QUIT. Enquanto roda, o wWinMain NAO retorna: o explorer
//      REAL PERSISTE (mesmo mecanismo de fila do win32k que o desktop caseiro ja usa p/ viver).
// ====================================================================
static long deh_slot5(void* self)  { (void)self; return S_OK_; }   // init do host: no-op OK
static long deh_slot10(void* self) {                                // "Run": loop de mensagens
    (void)self;
    cb_log("[cb] DesktopExplorerHost::slot10 -> LOOP DE MENSAGENS (o explorer REAL PERSISTE)\n");
    unsigned char msg[64];                                          // W32_MSG (hwnd/msg/w/lParam/time/pt) < 64
    for (;;) {
        long r = cb_getmessage(msg);
        if (r <= 0) break;                                          // 0 = WM_QUIT, <0 = sem janelas
        cb_dispatchmessage(msg);                                    // o wndproc real roda no user32 (ring 3)
    }
    cb_log("[cb] DesktopExplorerHost::slot10 -> loop terminou (WM_QUIT); wWinMain vai encerrar\n");
    return S_OK_;
}
static void* g_deh_vtbl[64];
static struct { void** lpVtbl; } g_deh_obj = { 0 };
static int g_deh_ready = 0;
static void* desktopexplorerhost_object(void) {
    if (!g_deh_ready) {
        g_deh_vtbl[0] = (void*)pl_QI; g_deh_vtbl[1] = (void*)pl_AddRef; g_deh_vtbl[2] = (void*)pl_Release;
        for (int i = 3; i < 64; i++) g_deh_vtbl[i] = (void*)univ_fill;
        g_deh_vtbl[5] = (void*)deh_slot5;    // vtbl+0x28
        g_deh_vtbl[10] = (void*)deh_slot10;  // vtbl+0x50 (loop de msg)
        g_deh_obj.lpVtbl = g_deh_vtbl; g_deh_ready = 1;
    }
    return &g_deh_obj;
}
// ExplorerHostCreator::Create (slot3): cria o DesktopExplorerHost e o devolve por out-param.
// O out-param real fica em [rsp+0x58] do wWinMain — passado a Create num dos args. Varremos
// a2..a6 (rdx/r8/r9 + 2 args de pilha) por um ponteiro-p/-qword-zerado-alinhado em faixa de
// usuario (mesma heuristica do univ_fill) e escrevemos o host ali. Logamos os args p/ mapear.
static long ehc_create(void* self, void* a2, void* a3, void* a4, void* a5, void* a6) {
    (void)self;
    // NOTA (sessao 8): observado em runtime que a chamada Create do wWinMain (0x23f56) so
    // seta rcx(this)+rdx(=CLSID_DesktopExplorerHost); r8/r9/args de pilha sao LIXO (leftover),
    // e o retorno (rax) e' IGNORADO. Ou seja, o host em [rsp+0x58] NAO vem dos args nem do
    // retorno de Create — vem de uma construcao interna do explorer (sub 0x2d2fc + delegates
    // com rcx=rdi/ord#200) ainda por mapear. Portanto NAO tocamos em nenhum arg (escrever num
    // arg-lixo corromperia um ponteiro real). Preenchemos SO um out-param genuinamente ZERADO
    // e alinhado, se existir (contrato COM padrao) — hoje nao ocorre, mas fica correto/pronto.
    void** cands[5] = { (void**)a2, (void**)a3, (void**)a4, (void**)a5, (void**)a6 };
    for (int i = 0; i < 5; i++) {
        if (cb_out_ok(cands[i]) && *cands[i] == 0) {
            *cands[i] = desktopexplorerhost_object();
            cb_log("[cb]   ExplorerHostCreator::Create -> host no out-param zerado\n");
            return S_OK_;
        }
    }
    return S_OK_;
}
static void* g_ehc_vtbl[64];
static struct { void** lpVtbl; } g_ehc_obj = { 0 };
static int g_ehc_ready = 0;
static void* explorerhostcreator_object(void) {
    if (!g_ehc_ready) {
        g_ehc_vtbl[0] = (void*)pl_QI; g_ehc_vtbl[1] = (void*)pl_AddRef; g_ehc_vtbl[2] = (void*)pl_Release;
        for (int i = 3; i < 64; i++) g_ehc_vtbl[i] = (void*)univ_fill;
        g_ehc_vtbl[3] = (void*)ehc_create;   // vtbl+0x18 (Create)
        g_ehc_obj.lpVtbl = g_ehc_vtbl; g_ehc_ready = 1;
    }
    return &g_ehc_obj;
}
static const GUID_ CLSID_ExplorerHostCreator = { 0xAB0B37EC, 0x56F6, 0x4A0E, { 0xA8,0xFD,0x7A,0x8B,0xF7,0xC2,0xDA,0x96 } };

// Fabrica por-CLSID: devolve um objeto ESPECIFICO se o CLSID for conhecido, senao 0
// (o chamador cai no objeto universal). Aditivo: novos CLSIDs entram aqui.
static void* specific_object_for(const GUID_* clsid) {
    if (!clsid) return 0;
    if (guid_eq(clsid, &CLSID_TaskbandPin))         return pinnedlist_object();
    if (guid_eq(clsid, &CLSID_ExplorerHostCreator)) return explorerhostcreator_object();
    return 0;
}

// ====================================================================
//  IMalloc / CoCreateInstance.
// ====================================================================
__declspec(dllexport) HRESULT_ CoGetMalloc(unsigned ctx, void** ppMalloc) { (void)ctx; if (!ppMalloc) return E_POINTER_; imalloc_init(); *ppMalloc = &g_imalloc; return S_OK_; }
__declspec(dllexport) HRESULT_ CoCreateInstance(const GUID_* clsid, void* outer, unsigned ctx, const GUID_* iid, void** ppv) {
    (void)outer; (void)ctx;
    if (!ppv) return E_POINTER_;
#if COMBASE_DBG
    dbg_thread();
#endif
    dbg_guid("[cb] CoCreateInstance clsid=", clsid); dbg_guid("[cb]            iid=", iid);   // no-op se COMBASE_DBG=0
    (void)iid;
    void* specific = specific_object_for(clsid);   // objeto REAL por CLSID (ex.: TaskbandPin)
    *ppv = specific ? specific : universal_object();
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

// ====================================================================
//  WinRT (Win8+: a combase absorveu o runtime WinRT). HSTRING = string
//  imutavel ref-contada; HSTRING NULL == string vazia (convencao WinRT).
// ====================================================================
#define HSTRING_REFERENCE_FLAG 1
typedef struct { unsigned int flags; unsigned int length; const WCHAR_* buf; long ref; } hstring_t;
static const WCHAR_ g_wempty[1] = { 0 };

__declspec(dllexport) HRESULT_ WindowsCreateString(const WCHAR_* src, unsigned int len, void** out) {
    if (!out) return E_POINTER_;
    *out = 0;
    if (len == 0) return S_OK_;                         // HSTRING vazia == NULL
    if (!src) return E_POINTER_;
    hstring_t* h = (hstring_t*)arena_alloc(sizeof(hstring_t));
    WCHAR_* copy = (WCHAR_*)arena_alloc((size_t_)(len + 1) * sizeof(WCHAR_));
    if (!h || !copy) return E_OUTOFMEMORY_;
    for (unsigned int i = 0; i < len; i++) copy[i] = src[i];
    copy[len] = 0;
    h->flags = 0; h->length = len; h->buf = copy; h->ref = 1;
    *out = h; return S_OK_;
}
// fast-pass: usa o HSTRING_HEADER (24 bytes) do chamador como hstring_t; NAO copia.
__declspec(dllexport) HRESULT_ WindowsCreateStringReference(const WCHAR_* src, unsigned int len, void* header, void** out) {
    if (!out || !header) return E_POINTER_;
    *out = 0;
    if (len == 0) return S_OK_;
    if (!src) return E_POINTER_;
    hstring_t* h = (hstring_t*)header;
    h->flags = HSTRING_REFERENCE_FLAG; h->length = len; h->buf = src; h->ref = 1;
    *out = h; return S_OK_;
}
__declspec(dllexport) HRESULT_ WindowsDeleteString(void* hs) {
    if (!hs) return S_OK_;
    hstring_t* h = (hstring_t*)hs;
    if (h->flags & HSTRING_REFERENCE_FLAG) return S_OK_;   // fast-pass: o chamador e' dono
    if (h->ref > 0) --h->ref;                              // bump: nao libera de fato
    return S_OK_;
}
__declspec(dllexport) const WCHAR_* WindowsGetStringRawBuffer(void* hs, unsigned int* len) {
    if (!hs) { if (len) *len = 0; return g_wempty; }
    hstring_t* h = (hstring_t*)hs;
    if (len) *len = h->length;
    return h->buf ? h->buf : g_wempty;
}
__declspec(dllexport) unsigned int WindowsGetStringLen(void* hs) { return hs ? ((hstring_t*)hs)->length : 0; }
__declspec(dllexport) HRESULT_ WindowsDuplicateString(void* hs, void** out) {
    if (!out) return E_POINTER_;
    *out = 0;
    if (!hs) return S_OK_;                                 // dup de vazia == vazia (NULL)
    hstring_t* h = (hstring_t*)hs;
    if (!(h->flags & HSTRING_REFERENCE_FLAG)) { ++h->ref; *out = h; return S_OK_; }   // normal: AddRef
    return WindowsCreateString(h->buf, h->length, out);    // fast-pass: copia real (buffer do chamador pode nao sobreviver)
}
__declspec(dllexport) HRESULT_ WindowsCompareStringOrdinal(void* h1, void* h2, int* result) {
    if (!result) return E_POINTER_;
    unsigned int l1 = 0, l2 = 0;
    const WCHAR_* b1 = WindowsGetStringRawBuffer(h1, &l1);
    const WCHAR_* b2 = WindowsGetStringRawBuffer(h2, &l2);
    unsigned int n = (l1 < l2) ? l1 : l2;
    for (unsigned int i = 0; i < n; i++) if (b1[i] != b2[i]) { *result = (b1[i] < b2[i]) ? -1 : 1; return S_OK_; }
    *result = (l1 == l2) ? 0 : (l1 < l2 ? -1 : 1);
    return S_OK_;
}
__declspec(dllexport) HRESULT_ WindowsSubstringWithSpecifiedLength(void* hs, unsigned int start, unsigned int len, void** out) {
    if (!out) return E_POINTER_;
    *out = 0;
    unsigned int L = 0; const WCHAR_* b = WindowsGetStringRawBuffer(hs, &L);
    if (start > L || len > L - start) return E_INVALIDARG_;
    if (len == 0) return S_OK_;
    return WindowsCreateString(b + start, len, out);
}
// buffer mutavel: Preallocate -> (chamador escreve) -> Promote finaliza em HSTRING.
__declspec(dllexport) HRESULT_ WindowsPreallocateStringBuffer(unsigned int len, WCHAR_** charBuf, void** bufferHandle) {
    if (!charBuf || !bufferHandle) return E_POINTER_;
    *charBuf = 0; *bufferHandle = 0;
    hstring_t* h = (hstring_t*)arena_alloc(sizeof(hstring_t));
    WCHAR_* buf = (WCHAR_*)arena_alloc((size_t_)(len + 1) * sizeof(WCHAR_));
    if (!h || !buf) return E_OUTOFMEMORY_;
    for (unsigned int i = 0; i <= len; i++) buf[i] = 0;
    h->flags = 0; h->length = len; h->buf = buf; h->ref = 1;
    *charBuf = buf; *bufferHandle = h; return S_OK_;
}
__declspec(dllexport) HRESULT_ WindowsPromoteStringBuffer(void* bufferHandle, void** out) {
    if (!out) return E_POINTER_;
    *out = 0;
    if (!bufferHandle) return S_OK_;
    hstring_t* h = (hstring_t*)bufferHandle;
    WCHAR_* buf = (WCHAR_*)h->buf;
    if (buf) buf[h->length] = 0;                           // garante terminacao
    if (h->length == 0) return S_OK_;                      // vazia -> NULL
    *out = h; return S_OK_;
}
__declspec(dllexport) HRESULT_ WindowsDeleteStringBuffer(void* bufferHandle) { (void)bufferHandle; return S_OK_; }   // bump: descarta

// ---- WinRT runtime (Ro*). Ativacao: devolve o objeto universal (mesma politica do
// CoCreateInstance). Agora que os getters (univ_fill) preenchem o out-param REAL, a
// factory/instancia respondem a fabrica MRT (get_MainResourceMap, etc.) com interfaces
// validas em vez de NULL — o explorer roda a init COMPLETA de recursos e SEGUE ate o
// wWinMain. (Antes devolviamos REGDB_E_CLASSNOTREG porque o objeto universal com metodos
// E_NOTIMPL travava o MRT; isso mudou com o univ_fill.) ----
__declspec(dllexport) HRESULT_ RoInitialize(unsigned initType) { (void)initType; return S_OK_; }
__declspec(dllexport) void     RoUninitialize(void) { }
__declspec(dllexport) HRESULT_ RoActivateInstance(void* activatableClassId, void** instance) {
    unsigned int L=0; const WCHAR_* nm=WindowsGetStringRawBuffer(activatableClassId,&L); dbg_wstr("[cb] RoActivateInstance class=", nm, L); (void)nm; (void)L;
    if (!instance) return E_POINTER_;
    *instance = universal_object(); return S_OK_;
}
__declspec(dllexport) HRESULT_ RoGetActivationFactory(void* activatableClassId, const GUID_* iid, void** factory) {
    unsigned int L=0; const WCHAR_* nm=WindowsGetStringRawBuffer(activatableClassId,&L); dbg_wstr("[cb] RoGetActivationFactory class=", nm, L); dbg_guid("[cb]            factory-iid=", iid); (void)nm; (void)L; (void)iid;
    if (!factory) return E_POINTER_;
    *factory = universal_object(); return S_OK_;
}

// ---- WinRT error info (mecanismo de erro rico) — no-op honesto aqui ----
__declspec(dllexport) int      RoOriginateError(HRESULT_ error, void* message) { (void)error; (void)message; return 1; }
__declspec(dllexport) int      RoOriginateLanguageException(HRESULT_ error, void* message, void* langException) { (void)error; (void)message; (void)langException; return 1; }
__declspec(dllexport) void     RoTransformError(HRESULT_ oldError, HRESULT_ newError, void* message) { (void)oldError; (void)newError; (void)message; }
__declspec(dllexport) HRESULT_ GetRestrictedErrorInfo(void** ppRestrictedErrorInfo) { if (ppRestrictedErrorInfo) *ppRestrictedErrorInfo = 0; return S_OK_; }   // sem info -> *pp=NULL
__declspec(dllexport) HRESULT_ SetRestrictedErrorInfo(void* pRestrictedErrorInfo) { (void)pRestrictedErrorInfo; return S_OK_; }
__declspec(dllexport) HRESULT_ RoGetMatchingRestrictedErrorInfo(HRESULT_ hr, void** ppRestrictedErrorInfo) { (void)hr; if (ppRestrictedErrorInfo) *ppRestrictedErrorInfo = 0; return S_FALSE_; }
__declspec(dllexport) void     RoFailFastWithErrorContext(HRESULT_ error) { (void)error; }   // no-op: prefere continuar a matar o processo

int DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }
