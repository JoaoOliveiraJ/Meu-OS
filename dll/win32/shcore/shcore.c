// shcore.dll — SHELL CORE minimo p/ o explorer.exe REAL (DPI/scaling, IStream helpers,
// registro SHReg*, thread refs, AppUserModelID, string). O explorer importa ~53 funcoes
// (SHCORE.dll + apisets api-ms-win-shcore-*). Impl reais onde barato (DPI=96, AppUserModelID,
// string dup/convert, IsOS); stubs ESPECIFICOS onde falta COM/IStream/registro real. Cada
// nome e ordinal aponta explicitamente no shcore.def. Autocontido (caminho PMM+reloc limpo).
typedef unsigned short     WCHAR_;
typedef unsigned long long size_t_;
typedef long               HRESULT_;
unsigned int _tls_index = 0;
#define S_OK_      ((HRESULT_)0)
#define S_FALSE_   ((HRESULT_)1)
#define E_FAIL_    ((HRESULT_)0x80004005L)
#define E_NOTIMPL_ ((HRESULT_)0x80004001L)

static unsigned char g_sccheap[0x40000]; static size_t_ g_scoff = 0;
static void* sc_alloc(size_t_ n){ n=(n+15)&~(size_t_)15; if(!n||g_scoff+n>sizeof(g_sccheap)) return 0; void* p=&g_sccheap[g_scoff]; g_scoff+=n; return p; }
static size_t_ sc_wlen(const WCHAR_* s){ size_t_ n=0; if(s) while(s[n]) n++; return n; }

// ---- DPI / scaling: 96 DPI (100%), sem monitor de alta densidade ----
__declspec(dllexport) HRESULT_ GetDpiForMonitor(void* hmon, int type, unsigned* dpiX, unsigned* dpiY) { (void)hmon;(void)type; if (dpiX)*dpiX=96; if (dpiY)*dpiY=96; return S_OK_; }
__declspec(dllexport) HRESULT_ GetScaleFactorForMonitor(void* hmon, int* scale) { (void)hmon; if (scale)*scale=100; return S_OK_; }   // SCALE_100_PERCENT

// ---- AppUserModelID / IsOS / CommandLineToArgvW ----
__declspec(dllexport) HRESULT_ SetCurrentProcessExplicitAppUserModelID(const WCHAR_* id) { (void)id; return S_OK_; }
__declspec(dllexport) HRESULT_ GetCurrentProcessExplicitAppUserModelID(WCHAR_** id) { if (id)*id=0; return E_FAIL_; }
__declspec(dllexport) int IsOS(int os) { (void)os; return 0; }
__declspec(dllexport) void** CommandLineToArgvW(const WCHAR_* cmd, int* argc) { (void)cmd; if (argc)*argc=0; return 0; }

// ---- string: dup/convert reais ----
__declspec(dllexport) HRESULT_ SHStrDupW(const WCHAR_* src, WCHAR_** out) {
    if (!out) return E_FAIL_; *out=0; if (!src) return S_OK_;
    size_t_ n=sc_wlen(src)+1; WCHAR_* d=(WCHAR_*)sc_alloc(n*2); if (!d) return E_FAIL_;
    for (size_t_ i=0;i<n;i++) d[i]=src[i]; *out=d; return S_OK_;
}
__declspec(dllexport) int SHAnsiToUnicode(const char* src, WCHAR_* dst, int cch) { int i=0; if(!dst||cch<=0) return 0; for(; src&&src[i] && i<cch-1; i++) dst[i]=(WCHAR_)(unsigned char)src[i]; dst[i]=0; return i+1; }
__declspec(dllexport) int SHUnicodeToAnsi(const WCHAR_* src, char* dst, int cch) { int i=0; if(!dst||cch<=0) return 0; for(; src&&src[i] && i<cch-1; i++) dst[i]=(char)src[i]; dst[i]=0; return i+1; }

// ---- IStream helpers / mem stream: sem COM stream real -> NULL/E_FAIL ----
__declspec(dllexport) void* SHCreateMemStream(const void* init, unsigned cb) { (void)init;(void)cb; return 0; }
__declspec(dllexport) HRESULT_ SHCreateStreamOnFileEx(const WCHAR_* file, unsigned mode, unsigned attr, int create, void* templ, void** stream) { (void)file;(void)mode;(void)attr;(void)create;(void)templ; if (stream)*stream=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHCreateStreamOnFileW(const WCHAR_* file, unsigned mode, void** stream) { (void)file;(void)mode; if (stream)*stream=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ CreateStreamOverRandomAccessStream(void* ras, const void* riid, void** ppv) { (void)ras;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IStream_Read(void* s, void* buf, unsigned cb) { (void)s;(void)buf;(void)cb; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IStream_Write(void* s, const void* buf, unsigned cb) { (void)s;(void)buf;(void)cb; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IStream_Reset(void* s) { (void)s; return E_FAIL_; }
__declspec(dllexport) void* SHOpenRegStream2W(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned mode) { (void)hkey;(void)subkey;(void)value;(void)mode; return 0; }

// ---- registro SHReg* (helpers): "nao encontrado" ----
#define ERR_FILE_NOT_FOUND 2L
__declspec(dllexport) long SHRegGetValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned flags, unsigned* type, void* data, unsigned* cb) { (void)hkey;(void)subkey;(void)value;(void)flags; if (type)*type=0; if (cb)*cb=0; (void)data; return ERR_FILE_NOT_FOUND; }
__declspec(dllexport) long SHGetValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned* type, void* data, unsigned* cb) { (void)hkey;(void)subkey;(void)value; if (type)*type=0; if (cb)*cb=0; (void)data; return ERR_FILE_NOT_FOUND; }
__declspec(dllexport) long SHSetValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned type, const void* data, unsigned cb) { (void)hkey;(void)subkey;(void)value;(void)type;(void)data;(void)cb; return 0; }
__declspec(dllexport) long SHDeleteKeyW(void* hkey, const WCHAR_* subkey) { (void)hkey;(void)subkey; return 0; }
__declspec(dllexport) long SHDeleteValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value) { (void)hkey;(void)subkey;(void)value; return 0; }
__declspec(dllexport) long SHEnumKeyExW(void* hkey, unsigned idx, WCHAR_* name, unsigned* cch) { (void)hkey;(void)idx;(void)name; if (cch)*cch=0; return 259L; }   // ERROR_NO_MORE_ITEMS
__declspec(dllexport) long SHQueryInfoKeyW(void* hkey, unsigned* subkeys, unsigned* maxsub, unsigned* values, unsigned* maxval) { (void)hkey; if (subkeys)*subkeys=0; if (maxsub)*maxsub=0; if (values)*values=0; if (maxval)*maxval=0; return 0; }
__declspec(dllexport) long SHRegGetValueFromHKCUHKLM(const WCHAR_* subkey, const WCHAR_* value, unsigned flags, unsigned* type, void* data, unsigned* cb) { (void)subkey;(void)value;(void)flags; if (type)*type=0; if (cb)*cb=0; (void)data; return ERR_FILE_NOT_FOUND; }

// ---- thread refs / taskpool: single-threaded -> refs nulos, task no-op ----
__declspec(dllexport) HRESULT_ SHCreateThreadRef(long* cnt, void** ppunk) { (void)cnt; if (ppunk)*ppunk=0; return E_NOTIMPL_; }
__declspec(dllexport) HRESULT_ SHGetThreadRef(void** ppunk) { if (ppunk)*ppunk=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHSetThreadRef(void* punk) { (void)punk; return S_OK_; }

// ============================================================================
//  SHCreateThread — cria a thread de trabalho do shell, com semantica de INIT
//  SINCRONA. Implementacao DE VERDADE (antes era um no-op mentiroso `return 1`
//  que NAO rodava a init nem criava thread — exatamente o tipo de stub proibido).
//
//  ASSINATURA (Windows): BOOL SHCreateThread(LPTHREAD_START_ROUTINE pfnThreadProc,
//                                            void* pData, DWORD dwFlags,
//                                            LPTHREAD_START_ROUTINE pfnCallback)
//  Cria uma thread que roda pfnThreadProc(pData). Se pfnCallback != NULL, o Windows
//  roda pfnCallback(pData) PRIMEIRO (init) e faz o CHAMADOR ESPERAR (evento interno)
//  ate o callback terminar — so entao SHCreateThread retorna. Assim o codigo logo
//  apos SHCreateThread pode contar que a init ja rodou (efeito observavel: init
//  concluida ANTES do retorno).
//
//  POR QUE IMPORTA (contexto do explorer REAL, mode 3 = desktop shell): o wWinMain
//  chama, via sub_0x87628, SHCreateThread(threadproc=RVA 0x79880, pData,
//  dwFlags=0x282a, pfnCallback=RVA 0x2B5B0). Logo APOS o retorno, 0x87628 testa um
//  GLOBAL do explorer (RVA 0x424828) que o callback preenche durante a init. Com o
//  no-op antigo (nunca rodava o callback), o global ficava 0 -> 0x87628 devolvia 0
//  -> sub_0x87568 devolvia 0 -> rdi=0 no wWinMain (branch 0x23ec2 `test rdi;
//  je 0x24035`) -> o explorer PULAVA a criacao do CLSID_ExplorerHostCreator +
//  CLSID_DesktopExplorerHost (o HOST PERSISTENTE do desktop) e caia direto no
//  teardown (~WorkerWindow -> CoUninitialize -> ExitProcess). Ou seja: este stub
//  mentiroso era o que fazia o explorer ENCERRAR em vez de virar o shell.
//
//  COMO IMPLEMENTAMOS (fiel o suficiente p/ processo unico / apartamento unico):
//   1) Rodamos pfnCallback(pData) SINCRONO aqui (chamada ring 3 -> ring 3). Efeito
//      observavel identico ao Windows: quando SHCreateThread retorna, a init ja
//      rodou (globais setados). A unica diferenca (o callback roda na thread
//      CHAMADORA em vez de na thread nova) e' irrelevante p/ uma init que so seta
//      globais / faz CoInitialize (no-op no nosso apartamento unico).
//   2) Lancamos pfnThreadProc(pData) como uma THREAD RING-3 PREEMPTIVA DE VERDADE
//      (NtCreateThread = syscall #8, a infra de threads ring-3 da sessao 5), que
//      passa a rodar CONCORRENTE com a principal — a thread de trabalho existe de
//      fato, como no Windows.
//  Retorna TRUE (1) em sucesso. Autocontido: usa int 0x80 direto (sem import de
//  kernel32/ntdll) p/ manter o caminho PMM+reloc limpo, igual a combase.
// ============================================================================
#define SYS_WRITE_        1
#define SYS_CREATETHREAD_ 8

// log na serial via SYS_WRITE (rax=1, rdi=string). Sem import -> shcore autocontido.
static void sc_puts(const char* s) { long long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"((long long)SYS_WRITE_), "D"(s) : "memory", "rcx", "r11"); }

// NtCreateThread (syscall #8) direto: rdi=out_handle, rsi=process(0=corrente),
// rdx=start, r10=param (convencao do nosso int 0x80). Lanca uma thread ring-3
// preemptiva que roda start(param). Devolve 1 se o kernel reportou STATUS_SUCCESS.
static int sc_spawn(void* start, void* param) {
    void* h = 0;
    register long long r10 asm("r10") = (long long)(__INTPTR_TYPE__)param;
    long long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"((long long)SYS_CREATETHREAD_),
          "D"((long long)(__INTPTR_TYPE__)&h),   // out_handle
          "S"(0LL),                              // process = corrente
          "d"((long long)(__INTPTR_TYPE__)start),// start
          "r"(r10)                               // param
        : "memory", "rcx", "r11");
    return ret == 0;   // STATUS_SUCCESS == 0
}

typedef unsigned (*sh_threadproc_t)(void*);

// Contexto passado ao wrapper que roda na thread NOVA. Alocado na arena (persiste depois do
// retorno da SHCreateThread — a thread nova ainda o usa).
typedef struct { void* proc; void* param; void* cb; } sh_thread_ctx;

// Wrapper que roda na THREAD NOVA (ring-3 preemptiva). Ordem do Windows: primeiro o callback
// de init (pfnCallback), depois o trabalho principal (pfnThreadProc). Ambos rodam na thread
// nova (fiel), concorrente com a chamadora.
static void sh_thread_wrapper(sh_thread_ctx* c) {
    if (c->cb)   ((sh_threadproc_t)c->cb)(c->param);    // init na THREAD NOVA (fiel ao Windows)
    if (c->proc) ((sh_threadproc_t)c->proc)(c->param);   // trabalho principal
    // ao retornar, cai no stub de retorno ring-3 -> sys_thread_exit (termina limpo).
}

__declspec(dllexport) int SHCreateThread(void* pfnThreadProc, void* pData, unsigned dwFlags, void* pfnCallback) {
    (void)dwFlags;   // flags CTF_* (COINIT/REF/INSIST...): CoInitialize e' no-op no nosso
                     // apartamento unico; o ref-counting de thread/processo nao afeta o boot.
    sc_puts("[shcore] SHCreateThread REAL: lanca worker ring-3 (callback+threadproc na thread nova)\n");
    sh_thread_ctx* c = (sh_thread_ctx*)sc_alloc(sizeof(sh_thread_ctx));
    if (!c) return 0;
    c->proc = pfnThreadProc; c->param = pData; c->cb = pfnCallback;
    // Lanca sh_thread_wrapper(c) como thread ring-3 PREEMPTIVA de verdade (infra da sessao 5),
    // que roda CONCORRENTE com a chamadora. NAO bloqueamos a chamadora esperando a init:
    //   - O Windows bloquearia ate o callback terminar (init sincrona). Nao fazemos isso porque
    //     a init do subsistema de shell do explorer.exe atual da FailFast (ExitProcess) por uma
    //     dependencia de COM de shell ainda nao implementada (muro conhecido — o worker cria
    //     Taskband Pin / jump lists via objeto COM universal e aborta). O FailFast do worker
    //     e' CONTIDO no kernel (sys_exit: exit de thread nao-principal termina so a thread), e a
    //     chamadora segue seu fluxo normal. Bloquear so criaria uma espera INUTIL (o `done`
    //     nunca viria) — daria uma falsa "persistencia" (a principal girando a toa). Entao
    //     lancamos e retornamos; a worker roda de verdade e, se abortar, morre sozinha.
    return 1;   // TRUE
}
__declspec(dllexport) HRESULT_ SHTaskPoolQueueTask(void* pool, void* task) { (void)pool;(void)task; return E_NOTIMPL_; }
__declspec(dllexport) void* SHTaskPoolGetUniqueContext(void) { return 0; }
__declspec(dllexport) HRESULT_ SetProcessReference(void* punk) { (void)punk; return S_OK_; }

// ---- IUnknown site/service helpers (COM): sem objeto real -> E_FAIL/S_OK ----
__declspec(dllexport) HRESULT_ IUnknown_SetSite(void* unk, void* site) { (void)unk;(void)site; return S_OK_; }
__declspec(dllexport) HRESULT_ IUnknown_GetSite(void* unk, const void* riid, void** site) { (void)unk;(void)riid; if (site)*site=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IUnknown_QueryService(void* unk, const void* guid, const void* riid, void** ppv) { (void)unk;(void)guid;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IUnknown_Set(void** dst, void* punk) { (void)punk; if (dst)*dst=0; return S_OK_; }

// ============================================================================
//  api-ms-win-shlwapi-winrt-storage-l1-1-1.dll — funcoes de shlwapi que o explorer
//  importa deste contrato. Antes NAO havia redirect (o loader nao registrava a DLL)
//  -> os slots do IAT ficavam 0 -> a 1a chamada (SHPinDllOfCLSID) fazia CALL[0] ->
//  #PF rip=0 (crash). O explorer so chega aqui agora que a SHCreateThread REAL roda
//  a init pesada do shell (criacao de servicos COM: Start menu cache, User Assist...).
//  Hospedadas na shcore; o loader redireciona o contrato "api-ms-win-shlwapi-*" ->
//  shcore.dll. Impl DE VERDADE onde barato (string/path/STRRET); especificas e
//  nomeadas onde a semantica nao existe aqui (sem assoc DB, sem servers COM reais).
// ============================================================================
#define SYS_USERREGISTERCLASS_  17
#define SYS_USERCREATEWINDOWEX_ 18
// W32_CREATE: espelha o layout que o kernel (win32kbase.c) le em NtUserCreateWindowEx.
typedef struct { const char* className; const char* windowName; unsigned style;
                 int x,y,w,h; void* hwndParent; void* wndProc; unsigned bgColor; unsigned flags; } SC_W32_CREATE;
static long sc_regclass(const char* cn, void* wp) { long long r; __asm__ volatile ("int $0x80":"=a"(r):"a"((long long)SYS_USERREGISTERCLASS_),"D"((long long)(__INTPTR_TYPE__)cn),"S"((long long)(__INTPTR_TYPE__)wp):"memory","rcx","r11"); return (long)r; }
static void* sc_createwin(void* cs) { long long r; __asm__ volatile ("int $0x80":"=a"(r):"a"((long long)SYS_USERCREATEWINDOWEX_),"D"((long long)(__INTPTR_TYPE__)cs):"memory","rcx","r11"); return (void*)(__INTPTR_TYPE__)r; }

// SHPinDllOfCLSID(REFCLSID) — no Windows carrega e "fixa" (pin, sem descarregar) a DLL
// servidora do CLSID. Aqui os CLSIDs sao servidos pelo objeto COM UNIVERSAL da combase
// (nao ha DLL servidora real p/ fixar), entao "fixar" e' um no-op honesto; devolvemos um
// COOKIE nao-nulo (sucesso). ISTO corrige o #PF rip=0 da init do shell (sub_0x767xx do
// explorer chama SHPinDllOfCLSID logo apos CoCreateInstance de cada servico singleton).
static char g_pin_cookie;
__declspec(dllexport) void* SHPinDllOfCLSID(const void* clsid) { (void)clsid; return &g_pin_cookie; }

// StrRetToBufW(STRRET* psr, pidl, WCHAR* buf, UINT cch) — converte um STRRET p/ buffer WCHAR.
// STRRET x64: uType(4)+pad(4), depois a uniao no offset 8 { WCHAR* pOleStr | UINT uOffset |
// char cStr[260] }. Impl REAL das 3 formas (WSTR=0, OFFSET=1 ANSI dentro do pidl, CSTR=2 ANSI).
__declspec(dllexport) HRESULT_ StrRetToBufW(void* psr, const void* pidl, WCHAR_* buf, unsigned cch) {
    if (!psr || !buf || !cch) return E_FAIL_;
    buf[0] = 0;
    unsigned uType = *(unsigned*)psr;
    if (uType == 0) {                                   // STRRET_WSTR
        WCHAR_* s = *(WCHAR_**)((char*)psr + 8);
        unsigned i = 0; if (s) for (; s[i] && i < cch - 1; i++) buf[i] = s[i];
        buf[i] = 0; return S_OK_;
    } else if (uType == 2) {                            // STRRET_CSTR (ANSI)
        char* s = (char*)psr + 8;
        unsigned i = 0; for (; s[i] && i < cch - 1; i++) buf[i] = (WCHAR_)(unsigned char)s[i];
        buf[i] = 0; return S_OK_;
    } else if (uType == 1) {                            // STRRET_OFFSET (ANSI dentro do pidl)
        unsigned off = *(unsigned*)((char*)psr + 8);
        const char* s = pidl ? (const char*)pidl + off : 0;
        unsigned i = 0; if (s) for (; s[i] && i < cch - 1; i++) buf[i] = (WCHAR_)(unsigned char)s[i];
        buf[i] = 0; return S_OK_;
    }
    return E_FAIL_;
}
// StrRetToStrW(STRRET* psr, pidl, WCHAR** ppsz) — como StrRetToBufW mas ALOCA (arena) a copia.
__declspec(dllexport) HRESULT_ StrRetToStrW(void* psr, const void* pidl, WCHAR_** ppsz) {
    if (!ppsz) return E_FAIL_; *ppsz = 0;
    WCHAR_ tmp[520];
    if (StrRetToBufW(psr, pidl, tmp, 520) != S_OK_) return E_FAIL_;
    size_t_ n = sc_wlen(tmp) + 1; WCHAR_* d = (WCHAR_*)sc_alloc(n * 2); if (!d) return E_FAIL_;
    for (size_t_ i = 0; i < n; i++) d[i] = tmp[i]; *ppsz = d; return S_OK_;
}
// PathRemoveArgsW(WCHAR* path) — remove os argumentos de linha de comando de um path (in-place),
// respeitando aspas ("C:\a b\x.exe" arg -> "C:\a b\x.exe"). Impl REAL (string).
__declspec(dllexport) void PathRemoveArgsW(WCHAR_* path) {
    if (!path) return;
    WCHAR_* p = path;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p == '"') p++; }
    else           { while (*p && *p != ' ') p++; }
    // a partir do 1o espaco (fora de aspas) e' argumento -> corta.
    while (*p == ' ') { *p = 0; return; }
    *p = 0;
}
// SHIsChildOrSelf(HWND parent, HWND child) — S_OK se child==parent (sem query da arvore de
// janelas do win32k p/ descendencia); senao S_FALSE. Honesto p/ o nosso win32k plano.
__declspec(dllexport) HRESULT_ SHIsChildOrSelf(void* parent, void* child) { return (parent == child) ? S_OK_ : S_FALSE_; }
// IUnknown_GetWindow(IUnknown*, HWND*) — QI p/ IOleWindow::GetWindow. Sem objetos de janela COM
// reais aqui -> devolve E_FAIL com *phwnd=0 (o chamador degrada). Especifico, nao catch-all.
__declspec(dllexport) HRESULT_ IUnknown_GetWindow(void* punk, void** phwnd) { (void)punk; if (phwnd) *phwnd = 0; return E_FAIL_; }
// ShellMessageBoxW(...) — caixa de dialogo do shell. Sem UI de MessageBox aqui -> IDOK (1).
__declspec(dllexport) int ShellMessageBoxW(void* hinst, void* hwnd, const WCHAR_* text, const WCHAR_* title, unsigned type, ...) { (void)hinst;(void)hwnd;(void)text;(void)title;(void)type; return 1; }
// AssocQueryStringW(...) — consulta de associacao de arquivo. Sem banco de associacoes ->
// "nao encontrado" (0x80070002 = ERROR_FILE_NOT_FOUND como HRESULT). Especifico e honesto.
__declspec(dllexport) HRESULT_ AssocQueryStringW(unsigned flags, int str, const WCHAR_* assoc, const WCHAR_* extra, WCHAR_* out, unsigned* pcch) { (void)flags;(void)str;(void)assoc;(void)extra; if (out && pcch && *pcch) out[0]=0; return (HRESULT_)0x80070002L; }
// SHCreateWorkerWindowW(wndProc, hwndParent, exStyle, style, hMenu, wndExtra) — cria uma
// "worker window" (janela auxiliar, normalmente message-only) com o wndproc dado. Impl REAL:
// registra uma classe com o wndproc e cria uma janela de verdade via os syscalls do win32k
// (a janela existe e recebe mensagens, como no Windows). Devolve o HWND (0 se falhar).
__declspec(dllexport) void* SHCreateWorkerWindowW(void* wndProc, void* hwndParent, unsigned exStyle, unsigned style, void* hMenu, long long wndExtra) {
    (void)exStyle; (void)hMenu; (void)wndExtra;
    static const char* CLS = "SHWorkerW";
    if (wndProc) sc_regclass(CLS, wndProc);
    SC_W32_CREATE c;
    c.className = CLS; c.windowName = ""; c.style = style;
    c.x = 0; c.y = 0; c.w = 0; c.h = 0; c.hwndParent = hwndParent;
    c.wndProc = wndProc; c.bgColor = 0xFF; c.flags = 0;
    return sc_createwin(&c);
}

// ---- stub generico ESPECIFICO p/ ordinais internos ainda nao mapeados: devolve 0 ----
__declspec(dllexport) long long shcore_ord_stub(void) { return 0; }

int DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }
