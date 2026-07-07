// comctl32.dll — Common Controls v6. O explorer carrega em runtime (LoadLibraryW) e faz
// GetProcAddress dos pontos de entrada que usa na taskbar. Implementacoes ESPECIFICAS e
// nomeadas (sem catch-all): as que tem semantica segura de verdade (versao, subclassing,
// TrackMouseEvent) fazem a coisa certa; as que dependeriam do motor de controles (que ainda
// nao temos — as classes de controle nao estao registradas) devolvem o resultado que faz o
// explorer DEGRADAR em vez de derefar NULL. Autocontido (int 0x80 p/ log, sem ntdll).
//
// comctl32 real tem 421 exports (muitos por ordinal: ImageList_*, DPA_*/DSA_*). Aqui cobrimos
// os NOMEADOS que o shell tipicamente resolve por GetProcAddress; os pedidos que faltarem
// aparecem no log gated de GetProcAddress (sys_getprocaddress) e viram o proximo alvo.

typedef unsigned long long ULL_;
typedef long               LRESULT_;
typedef long               HRESULT_;
typedef int                BOOL_;
typedef unsigned long      DWORD_;
typedef void*              HANDLE_;
unsigned int _tls_index = 0;

#define S_OK_     ((HRESULT_)0)
#define E_FAIL_   ((HRESULT_)0x80004005L)
#define TRUE_     1
#define FALSE_    0

#define CC_DBG 0
#if CC_DBG
static void dbg_puts(const char* s) { ULL_ ret; __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(1ULL), "D"(s) : "memory", "rcx", "r11"); }
#define CCLOG(s) dbg_puts("[cc] " s "\n")
#else
#define CCLOG(s) ((void)0)
#endif

// ---- inicializacao de controles comuns ----
// InitCommonControlsEx(const INITCOMMONCONTROLSEX*) -> BOOL. Diz "controles prontos". Ainda
// nao registramos as classes de controle reais; devolver TRUE e' o que o chamador espera e
// deixa a taskbar prosseguir. A criacao de um controle real (CreateWindowEx de classe de
// controle) sera o proximo muro, surgindo honestamente adiante.
__declspec(dllexport) BOOL_ InitCommonControlsEx(const void* picce) { (void)picce; CCLOG("InitCommonControlsEx->TRUE"); return TRUE_; }
__declspec(dllexport) void  InitCommonControls(void) { CCLOG("InitCommonControls"); }

// ---- versao ----
typedef struct { DWORD_ cbSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformID; } DLLVERSIONINFO_;
__declspec(dllexport) HRESULT_ DllGetVersion(DLLVERSIONINFO_* pdvi) {
    if (!pdvi) return E_FAIL_;
    pdvi->dwMajorVersion = 6;      // comctl32 v6 (visual styles era)
    pdvi->dwMinorVersion = 10;
    pdvi->dwBuildNumber  = 19041;
    pdvi->dwPlatformID   = 1;      // DLLVER_PLATFORM_WINDOWS
    return S_OK_;
}

// ---- subclassing (SetWindowSubclass & cia.) — usado pesado pelo shell/DirectUI ----
// Sem tabela de subclasses real: aceitamos o registro (TRUE) e DefSubclassProc devolve 0
// (o subclass no-op nao altera o processamento). GetWindowSubclass "nao encontrado".
__declspec(dllexport) BOOL_ SetWindowSubclass(HANDLE_ hwnd, void* proc, ULL_ id, ULL_ ref) { (void)hwnd;(void)proc;(void)id;(void)ref; return TRUE_; }
__declspec(dllexport) BOOL_ RemoveWindowSubclass(HANDLE_ hwnd, void* proc, ULL_ id) { (void)hwnd;(void)proc;(void)id; return TRUE_; }
__declspec(dllexport) BOOL_ GetWindowSubclass(HANDLE_ hwnd, void* proc, ULL_ id, ULL_* pref) { (void)hwnd;(void)proc;(void)id; if(pref)*pref=0; return FALSE_; }
__declspec(dllexport) LRESULT_ DefSubclassProc(HANDLE_ hwnd, unsigned msg, ULL_ wp, ULL_ lp) { (void)hwnd;(void)msg;(void)wp;(void)lp; return 0; }

// ---- icones com escala/DPI — sem repositorio de icones -> falha graciosa (out=NULL) ----
__declspec(dllexport) HRESULT_ LoadIconMetric(HANDLE_ hinst, const void* name, int metric, HANDLE_* phico) {
    (void)hinst;(void)name;(void)metric; if(phico)*phico=0; return E_FAIL_;
}
__declspec(dllexport) HRESULT_ LoadIconWithScaleDown(HANDLE_ hinst, const void* name, int cx, int cy, HANDLE_* phico) {
    (void)hinst;(void)name;(void)cx;(void)cy; if(phico)*phico=0; return E_FAIL_;
}

// ---- notificacao de mouse (hover/leave) — aceita o registro ----
__declspec(dllexport) BOOL_ _TrackMouseEvent(void* ptme) { (void)ptme; return TRUE_; }

__declspec(dllexport) int DllMain(void* h, unsigned r, void* v) { (void)h;(void)r;(void)v; return 1; }
