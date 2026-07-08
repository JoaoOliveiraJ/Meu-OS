// shell32.dll — SHELL base MINIMO p/ rodar o explorer.exe REAL. O explorer importa ~90
// funcoes (SHELL32.dll direta + apisets api-ms-win-shell-namespace/changenotify/
// dataobject/shdirectory), muitas por ORDINAL. Implementamos DE VERDADE a familia IL*
// (manipulacao de ITEMIDLIST — bem definida e segura) e deixamos stubs ESPECIFICOS e
// nomeados para o resto (ShellExecute, Shell_NotifyIcon, SHParseDisplayName, ...), que
// dependem do namespace COM / IShellFolder ainda ausente. NUNCA um catch-all: cada nome
// e ordinal aponta explicitamente no shell32.def. Autocontido (alocador proprio) p/
// pegar o caminho limpo PMM+reloc do loader (sem imports circulares).
typedef unsigned short     USHORT_;
typedef unsigned long long size_t_;
typedef long               HRESULT_;
unsigned int _tls_index = 0;

#define S_OK_        ((HRESULT_)0)
#define E_FAIL_      ((HRESULT_)0x80004005L)
#define E_NOTIMPL_   ((HRESULT_)0x80004001L)
#define E_INVALIDARG ((HRESULT_)0x80070057L)

// arena propria p/ os ITEMIDLIST clonados (SHAlloc/CoTaskMem-like). Bump, sem free.
static unsigned char g_shheap[0x80000];
static size_t_ g_shoff = 0;
static void* sh_alloc(size_t_ n) { n = (n + 15) & ~(size_t_)15; if (!n || g_shoff + n > sizeof(g_shheap)) return 0; void* p = &g_shheap[g_shoff]; g_shoff += n; return p; }

// ---- ITEMIDLIST: sequencia de SHITEMID {USHORT cb; BYTE abID[cb-2]}, terminada por cb=0.
typedef struct { USHORT_ cb; unsigned char abID[1]; } SHITEMID_;
typedef SHITEMID_ ITEMIDLIST_;
static USHORT_ il_cb(const void* p) { return p ? *(const USHORT_*)p : 0; }
static const unsigned char* il_next(const unsigned char* p) { USHORT_ cb = il_cb(p); return cb ? p + cb : 0; }

__declspec(dllexport) unsigned il_get_size(const void* pidl) {   // ILGetSize
    if (!pidl) return 0;
    const unsigned char* p = (const unsigned char*)pidl; unsigned total = 0;
    while (il_cb(p)) { total += il_cb(p); p += il_cb(p); }
    return total + 2;   // + terminador (SHITEMID cb=0)
}
__declspec(dllexport) void* il_find_last(const void* pidl) {     // ILFindLastID
    if (!pidl) return 0;
    const unsigned char* p = (const unsigned char*)pidl; const unsigned char* last = p;
    while (il_cb(p)) { last = p; p += il_cb(p); }
    return (void*)last;
}
__declspec(dllexport) void* il_clone(const void* pidl) {         // ILClone
    if (!pidl) return 0;
    unsigned n = il_get_size(pidl); void* d = sh_alloc(n);
    if (d) { const unsigned char* s = (const unsigned char*)pidl; unsigned char* o = (unsigned char*)d; for (unsigned i=0;i<n;i++) o[i]=s[i]; }
    return d;
}
__declspec(dllexport) void* il_clone_first(const void* pidl) {   // ILCloneFirst
    if (!pidl) return 0;
    USHORT_ cb = il_cb(pidl); unsigned n = cb + 2; void* d = sh_alloc(n);
    if (d) { unsigned char* o = (unsigned char*)d; const unsigned char* s = (const unsigned char*)pidl; for (USHORT_ i=0;i<cb;i++) o[i]=s[i]; o[cb]=0; o[cb+1]=0; }
    return d;
}
__declspec(dllexport) void* il_combine(const void* a, const void* b) {   // ILCombine
    if (!a) return il_clone(b); if (!b) return il_clone(a);
    unsigned na = il_get_size(a) - 2, nb = il_get_size(b);
    void* d = sh_alloc(na + nb); if (!d) return 0;
    unsigned char* o = (unsigned char*)d; const unsigned char* sa=(const unsigned char*)a; const unsigned char* sb=(const unsigned char*)b;
    unsigned i=0; for (; i<na; i++) o[i]=sa[i]; for (unsigned j=0;j<nb;j++) o[i+j]=sb[j];
    return d;
}
__declspec(dllexport) int il_remove_last(void* pidl) {           // ILRemoveLastID
    if (!pidl) return 0; void* last = il_find_last(pidl);
    if (last == pidl) { *(USHORT_*)pidl = 0; return il_cb(pidl)!=0; }
    *(USHORT_*)last = 0; return 1;
}
__declspec(dllexport) int il_is_equal(const void* a, const void* b) {    // ILIsEqual
    if (a == b) return 1; if (!a || !b) return 0;
    unsigned na = il_get_size(a); if (na != il_get_size(b)) return 0;
    const unsigned char* sa=(const unsigned char*)a; const unsigned char* sb=(const unsigned char*)b;
    for (unsigned i=0;i<na;i++) if (sa[i]!=sb[i]) return 0; return 1;
}
__declspec(dllexport) int il_is_parent(const void* parent, const void* child, int immediate) {   // ILIsParent
    (void)immediate; if (!parent || !child) return 0;
    const unsigned char* p=(const unsigned char*)parent; const unsigned char* c=(const unsigned char*)child;
    while (il_cb(p)) { USHORT_ cb=il_cb(p); if (il_cb(c)!=cb) return 0; for (USHORT_ i=0;i<cb;i++) if (p[i]!=c[i]) return 0; p+=cb; c+=cb; }
    return 1;
}
__declspec(dllexport) void il_free(void* pidl) { (void)pidl; }   // ILFree (bump: no-op)

// ---- namespace COM (SHParseDisplayName/SHBind*/SHCreateItem*): sem IShellFolder real
//      ainda -> E_FAIL e zera o out-param (o caller degrada em vez de usar lixo). ----
__declspec(dllexport) HRESULT_ SHParseDisplayName(const void* name, void* bc, void** pidl, unsigned sfgao, unsigned* attr) { (void)name;(void)bc;(void)sfgao; if (pidl)*pidl=0; if (attr)*attr=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHCreateItemFromParsingName(const void* name, void* bc, const void* riid, void** ppv) { (void)name;(void)bc;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHCreateItemFromIDList(const void* pidl, const void* riid, void** ppv) { (void)pidl;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHCreateItemInKnownFolder(const void* kf, unsigned flags, const void* name, const void* riid, void** ppv) { (void)kf;(void)flags;(void)name;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHGetIDListFromObject(void* unk, void** pidl) { (void)unk; if (pidl)*pidl=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHBindToParent(const void* pidl, const void* riid, void** ppv, const void** last) { (void)pidl;(void)riid; if (ppv)*ppv=0; if (last)*last=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHBindToObject(void* sf, const void* pidl, void* bc, const void* riid, void** ppv) { (void)sf;(void)pidl;(void)bc;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHBindToFolderIDListParent(void* sf, const void* pidl, const void* riid, void** ppv, const void** last) { (void)sf;(void)pidl;(void)riid; if (ppv)*ppv=0; if (last)*last=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHGetNameFromIDList(const void* pidl, int form, void** name) { (void)pidl;(void)form; if (name)*name=0; return E_FAIL_; }
__declspec(dllexport) void SHChangeNotify(long event, unsigned flags, const void* a, const void* b) { (void)event;(void)flags;(void)a;(void)b; }
__declspec(dllexport) unsigned long SHChangeNotifyRegisterThread(unsigned status) { (void)status; return 0; }
__declspec(dllexport) HRESULT_ SHCreateDataObject(const void* folder, unsigned cidl, const void* apidl, void* inner, const void* riid, void** ppv) { (void)folder;(void)cidl;(void)apidl;(void)inner;(void)riid; if (ppv)*ppv=0; return E_NOTIMPL_; }

// ---- shell classico (ShellExecute/NotifyIcon/AppBar/icones/arquivo): stubs nomeados ----
__declspec(dllexport) void* ShellExecuteW(void* hwnd, const void* verb, const void* file, const void* params, const void* dir, int show) { (void)hwnd;(void)verb;(void)file;(void)params;(void)dir;(void)show; return (void*)(long long)33; }   // > 32 = "sucesso"
__declspec(dllexport) int ShellExecuteExW(void* info) { (void)info; return 1; }
__declspec(dllexport) unsigned DragQueryFileW(void* hdrop, unsigned i, void* buf, unsigned cch) { (void)hdrop;(void)buf;(void)cch; return (i == 0xFFFFFFFFu) ? 0 : 0; }   // 0 arquivos
__declspec(dllexport) int Shell_NotifyIconW(unsigned msg, void* data) { (void)msg;(void)data; return 1; }
__declspec(dllexport) HRESULT_ Shell_NotifyIconGetRect(const void* id, void* rect) { (void)id;(void)rect; return E_FAIL_; }
__declspec(dllexport) unsigned long long SHAppBarMessage(unsigned msg, void* data) { (void)msg;(void)data; return 0; }
__declspec(dllexport) HRESULT_ SHGetStockIconInfo(int id, unsigned flags, void* info) { (void)id;(void)flags;(void)info; return E_FAIL_; }
__declspec(dllexport) unsigned ExtractIconExW(const void* file, int idx, void** large, void** small_, unsigned n) { (void)file;(void)idx;(void)n; if (large)*large=0; if (small_)*small_=0; return 0; }
__declspec(dllexport) void* DuplicateIcon(void* inst, void* icon) { (void)inst;(void)icon; return 0; }
__declspec(dllexport) int Shell_GetCachedImageIndexW(const void* path, int idx, unsigned flags) { (void)path;(void)idx;(void)flags; return -1; }
__declspec(dllexport) int SHFileOperationW(void* op) { (void)op; return 1; }   // != 0 = nao executou
__declspec(dllexport) int SHGetPathFromIDListW(const void* pidl, void* path) { (void)pidl;(void)path; return 0; }
__declspec(dllexport) void SHAddToRecentDocs(unsigned flags, const void* p) { (void)flags;(void)p; }
__declspec(dllexport) HRESULT_ SHUpdateRecycleBinIcon(void) { return S_OK_; }
__declspec(dllexport) HRESULT_ SHQueryUserNotificationState(int* state) { if (state)*state=1; return S_OK_; }   // QUNS_NOT_PRESENT
__declspec(dllexport) HRESULT_ SHGetPropertyStoreForWindow(void* hwnd, const void* riid, void** ppv) { (void)hwnd;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHGetLocalizedName(const void* path, void* module, unsigned cch, int* resid) { (void)path;(void)module;(void)cch; if (resid)*resid=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHEvaluateSystemCommandTemplate(const void* cmd, void** app, void** args, void** dir) { (void)cmd; if (app)*app=0; if (args)*args=0; if (dir)*dir=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHEnableServiceObject(const void* clsid, int enable) { (void)clsid;(void)enable; return S_OK_; }

// ---- api-ms-win-storage-exports-internal-l1-1-0.dll (o explorer importa daqui) ----
// SHGetFolderPathEx/SHGetKnownFolderIDList: sem backing de known-folders/FS ainda ->
// falham honestamente (buffer/pidl zerados; o explorer degrada). Get/SetThreadFlags: par
// de flags de thread do shell (mask,value); single-thread aqui, uma variavel serve.
static unsigned g_shell_thread_flags = 0;
__declspec(dllexport) HRESULT_ SHGetFolderPathEx(const void* rfid, unsigned flags, void* token, unsigned short* path, unsigned cch) { (void)rfid;(void)flags;(void)token; if (path && cch) path[0]=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHGetKnownFolderIDList(const void* rfid, unsigned flags, void* token, void** ppidl) { (void)rfid;(void)flags;(void)token; if (ppidl)*ppidl=0; return E_FAIL_; }
__declspec(dllexport) unsigned GetThreadFlags(void) { return g_shell_thread_flags; }
__declspec(dllexport) void SetThreadFlags(unsigned mask, unsigned value) { g_shell_thread_flags = (g_shell_thread_flags & ~mask) | (value & mask); }

// ---- stub generico ESPECIFICO p/ os ordinais internos ainda nao mapeados: devolve 0
//      (S_OK/NULL/FALSE — o caminho benigno). Cada ordinal aponta aqui pelo .def. ----
__declspec(dllexport) long long shell_ord_stub(void) { return 0; }

// ============================================================================
//  shell32 ordinal #200 (real: RVA 0xa8380). No Windows aloca ~0x2c0 bytes, constroi
//  um objeto de CONTEXTO DO HOST do shell (le dimensoes de tela via GetSystemMetrics —
//  SM_C*SCREEN/SM_CYCAPTION...) e devolve o PONTEIRO. O wWinMain do explorer (0x87568 ->
//  0x875cd) usa esse retorno como rbx->rdi: se != 0 (e GLOBAL_C setado + GLOBAL_B==0), o
//  wWinMain ENTRA na escada de PERSISTENCIA (0x23ec2: test rdi -> CoCreateInstance
//  ExplorerHostCreator/DesktopExplorerHost + loop de msg). Antes: shell_ord_stub -> 0 ->
//  rdi=0 -> teardown (~WorkerWindow -> ExitProcess), o explorer NAO persistia.
//
//  Aqui devolvemos um objeto NAO-NULO, ZERADO (0x2c0), com uma vtable de FALLBACK no
//  offset 0 (QI->self, AddRef/Release->1, demais metodos->S_OK). Assim o explorer, ao usar
//  rdi, tolera tanto LEITURA DE CAMPO (le 0) quanto CHAMADA VIRTUAL (recebe S_OK) sem #PF.
//  Objeto singleton (o explorer pede um host so). Aditivo; nao toca em nenhum outro export.
// ============================================================================
static long          s200_QI(void* self, const void* riid, void** ppv) { (void)riid; if (ppv) *ppv = self; return 0; }
static unsigned long s200_ref(void* self) { (void)self; return 1; }
static long          s200_ok(void* self) { (void)self; return 0; }   // S_OK
static void*         g_s200_vtbl[64];
static unsigned char g_s200_obj[0x2c0 + 16];
static int           g_s200_ready = 0;
static void s_puts(const char* s) { long long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(1LL), "D"(s) : "memory", "rcx", "r11"); }
__declspec(dllexport) void* shell_ord200(void* arg) {
    (void)arg;
    s_puts("[shell32] ord#200 CHAMADA -> devolve host NAO-NULO (rdi != 0 -> persistencia)\n");
    if (!g_s200_ready) {
        g_s200_vtbl[0] = (void*)s200_QI; g_s200_vtbl[1] = (void*)s200_ref; g_s200_vtbl[2] = (void*)s200_ref;
        for (int i = 3; i < 64; i++) g_s200_vtbl[i] = (void*)s200_ok;
        *(void***)g_s200_obj = g_s200_vtbl;   // vtable no offset 0 (caso o explorer faca call virtual)
        g_s200_ready = 1;
    }
    return g_s200_obj;   // ponteiro NAO-NULO -> rdi != 0 -> caminho de persistencia
}

int DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }
