// user32.dll  —  reimplementacao. API de janelas (RegisterClassA,
// CreateWindowExA, GetMessageA, DispatchMessageA, DefWindowProcA,
// TranslateMessage, ShowWindow, PostQuitMessage) + MessageBoxA.
//
// Igual ao Windows: o user32 vive em RING 3 e encaminha tudo para o win32k
// (lado kernel) via ntdll (int 0x80). O ponto-chave: DispatchMessage CHAMA O
// WNDPROC AQUI, em ring 3 — o kernel so entrega a MSG; o callback do app nunca
// roda no kernel (o win32k guarda apenas metadados da janela).
unsigned int _tls_index = 0;

typedef unsigned long long ULL;

// ---- imports do ntdll (a unica camada que faz syscall) ----
__declspec(dllimport) long  NtUserMessageBox(const char* text, const char* caption);
__declspec(dllimport) long  NtUserRegisterClass(const char* className, void* wndProc);
__declspec(dllimport) void* NtUserCreateWindowEx(void* createStruct);
__declspec(dllimport) long  NtUserDestroyWindow(void* hwnd);
__declspec(dllimport) long  NtUserShowWindow(void* hwnd, int cmdShow);
__declspec(dllimport) long  NtUserGetMessage(void* msg);
__declspec(dllimport) long  NtUserDispatchMessage(void* msg);
__declspec(dllimport) long  NtUserPostMessage(void* hwnd, unsigned msg, ULL w, ULL l);
__declspec(dllimport) long  NtUserPostQuitMessage(int exitCode);
__declspec(dllimport) void* NtUserGetDC(void* hwnd);
__declspec(dllimport) long  NtUserInvalidate(void* hwnd);
// FillRect, no Windows real, vive em user32.dll (e uma funcao USER, nao GDI).
__declspec(dllimport) long  NtGdiFillRect(void* hdc, int x, int y, int w, int h, void* hbrush);
// FASE 6: foco + injecao de tecla numa janela especifica (demo headless).
__declspec(dllimport) long  NtUserSetFocus(void* hwnd);
__declspec(dllimport) long  NtUserPostKey(void* hwnd, int ascii, int scancode);
// FASE 11: cursor do mouse.
__declspec(dllimport) long  NtUserGetCursorPos(void* out_point);
__declspec(dllimport) long  NtUserSetCursorPos(int x, int y);

// ---- mensagens (subset) ----
#define WM_CREATE  0x0001
#define WM_DESTROY 0x0002
#define WM_PAINT   0x000F
#define WM_QUIT    0x0012
#define WM_KEYDOWN 0x0100
#define WM_CHAR    0x0102

// ---- tipos (layout identico ao win32k.h do kernel) ----
typedef struct { int x, y; } POINT;
typedef struct {
    void*    hwnd;
    unsigned message;
    ULL      wParam;
    ULL      lParam;
    unsigned time;
    POINT    pt;
} MSG;
typedef struct { int left, top, right, bottom; } RECT;

// WNDCLASS simplificada (so o que usamos): lpfnWndProc + lpszClassName.
typedef long long (*WNDPROC)(void* hwnd, unsigned msg, ULL wParam, ULL lParam);
__declspec(dllexport) long long DefWindowProcA(void* hwnd, unsigned msg, ULL wParam, ULL lParam);
typedef struct {
    unsigned    style;
    WNDPROC     lpfnWndProc;
    int         cbClsExtra;
    int         cbWndExtra;
    void*       hInstance;
    void*       hIcon;
    void*       hCursor;
    void*       hbrBackground;
    const char* lpszMenuName;
    const char* lpszClassName;
} WNDCLASSA;

// Estrutura passada ao kernel em CreateWindowEx (espelha W32_CREATE do win32k.h).
// FASE 6: ganhou bgColor + flags no fim (layout compativel).
#define WNDF_DESKTOP   0x0001
#define WNDF_CONSOLE   0x0002
#define WND_BG_DEFAULT 0xFF
typedef struct {
    const char* className;
    const char* windowName;
    unsigned    style;
    int         x, y, w, h;
    void*       hwndParent;
    void*       wndProc;
    unsigned    bgColor;
    unsigned    flags;
} W32_CREATE;

// ---- tabela classe -> wndproc (vive aqui, em ring 3) ----
#define MAX_CLASSES 16
static struct { int used; const char* name; WNDPROC proc; } g_classes[MAX_CLASSES];

static int seq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static WNDPROC find_wndproc(const char* className) {
    for (int i = 0; i < MAX_CLASSES; i++)
        if (g_classes[i].used && seq(g_classes[i].name, className)) return g_classes[i].proc;
    return 0;
}

// Cada HWND lembra a sua classe (para DispatchMessage achar o wndproc). Como o
// HWND e um id pequeno (1..N) atribuido pelo kernel, indexamos por ele.
#define MAX_WINDOWS 16
static struct { void* hwnd; WNDPROC proc; } g_windows[MAX_WINDOWS];
static int g_nwin;

static WNDPROC wndproc_of(void* hwnd) {
    for (int i = 0; i < g_nwin; i++) if (g_windows[i].hwnd == hwnd) return g_windows[i].proc;
    return 0;
}

// ============================================================================
//  API
// ============================================================================
__declspec(dllexport) int MessageBoxA(void* hWnd, const char* text,
                                      const char* caption, unsigned type) {
    (void)hWnd; (void)type;
    return (int)NtUserMessageBox(text, caption);
}

// RegisterClassA: guarda a classe (nome + wndproc) localmente e avisa o kernel.
__declspec(dllexport) unsigned short RegisterClassA(const WNDCLASSA* wc) {
    if (!wc || !wc->lpszClassName) return 0;
    int slot = -1;
    for (int i = 0; i < MAX_CLASSES; i++)
        if (g_classes[i].used && seq(g_classes[i].name, wc->lpszClassName)) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < MAX_CLASSES; i++) if (!g_classes[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    g_classes[slot].used = 1;
    g_classes[slot].name = wc->lpszClassName;
    g_classes[slot].proc = wc->lpfnWndProc;
    NtUserRegisterClass(wc->lpszClassName, (void*)wc->lpfnWndProc);
    return 1;   // ATOM (simplificado)
}

// Implementacao comum: cria a janela com bgColor + flags (FASE 6). O kernel
// preenche os campos novos; lembra o (hwnd -> wndproc) para o despacho local.
static void* create_window_impl(const char* className, const char* windowName,
        unsigned style, int x, int y, int w, int h, void* parent,
        unsigned bgColor, unsigned flags) {
    WNDPROC proc = find_wndproc(className);
    W32_CREATE c;
    c.className = className; c.windowName = windowName; c.style = style;
    c.x = x; c.y = y; c.w = w; c.h = h; c.hwndParent = parent;
    c.wndProc = (void*)proc;
    c.bgColor = bgColor; c.flags = flags;

    void* hwnd = NtUserCreateWindowEx(&c);
    if (hwnd && g_nwin < MAX_WINDOWS) {
        g_windows[g_nwin].hwnd = hwnd; g_windows[g_nwin].proc = proc; g_nwin++;
    }
    return hwnd;
}

// CreateWindowExA: janela classica (area cliente cinza, com chrome). Compat. Fase 2.
__declspec(dllexport) void* CreateWindowExA(unsigned exStyle, const char* className,
        const char* windowName, unsigned style, int x, int y, int w, int h,
        void* parent, void* menu, void* inst, void* param) {
    (void)exStyle; (void)menu; (void)inst; (void)param;
    return create_window_impl(className, windowName, style, x, y, w, h, parent,
                              WND_BG_DEFAULT, 0);
}

// FASE 6 — CreateDesktopWindowA: janela de FUNDO (papel de parede). Cobre a tela,
// sem chrome, cor de fundo 'bgColor'. Fica no fundo do z-order; nao toma o foco.
__declspec(dllexport) void* CreateDesktopWindowA(const char* className,
        const char* title, unsigned bgColor, WNDPROC unusedProc) {
    (void)unusedProc;
    return create_window_impl(className, title, 0, 0, 0, 0, 0, 0,
                              bgColor, WNDF_DESKTOP);
}

// FASE 6 — CreateConsoleWindowA: janela de CONSOLE (cmd). Area cliente escura
// (bgColor), com chrome e barra de titulo; recebe foco e teclado normalmente.
__declspec(dllexport) void* CreateConsoleWindowA(const char* className,
        const char* title, int x, int y, int w, int h, unsigned bgColor) {
    return create_window_impl(className, title, 0, x, y, w, h, 0,
                              bgColor, WNDF_CONSOLE);
}

__declspec(dllexport) int ShowWindow(void* hwnd, int cmdShow) {
    return (int)NtUserShowWindow(hwnd, cmdShow);
}

// UpdateWindow: forca o repaint da area invalida (marca invalida -> WM_PAINT no
// proximo GetMessage). No MeuOS a janela ja pinta ao ShowWindow; aqui reforcamos.
__declspec(dllexport) int UpdateWindow(void* hwnd) {
    NtUserInvalidate(hwnd);
    return 1;
}

__declspec(dllexport) int DestroyWindow(void* hwnd) {
    return (int)NtUserDestroyWindow(hwnd);
}

// GetMessageA(&msg, hwnd, min, max): >0 normal, 0 = WM_QUIT, -1 = erro.
__declspec(dllexport) int GetMessageA(MSG* msg, void* hwnd, unsigned min, unsigned max) {
    (void)hwnd; (void)min; (void)max;
    return (int)NtUserGetMessage(msg);
}

// PeekMessageA: nao bloqueante — aqui delegamos a GetMessage (simplificado).
__declspec(dllexport) int TranslateMessage(const MSG* msg) {
    // WM_KEYDOWN -> WM_CHAR ja e feito no kernel (win32k_on_key); aqui e no-op
    // como no Windows quando ja ha caractere. Retorna 1 se traduziu algo.
    return (msg && msg->message == WM_KEYDOWN) ? 1 : 0;
}

// DispatchMessageA: CHAMA o WNDPROC da janela (em ring 3). Avisa o kernel
// (log/roteamento) e invoca o callback do app. ESTE e o coracao do laco.
__declspec(dllexport) long long DispatchMessageA(const MSG* msg) {
    if (!msg) return 0;
    NtUserDispatchMessage((void*)msg);            // log do lado kernel
    WNDPROC proc = wndproc_of(msg->hwnd);
    if (!proc) return DefWindowProcA(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    return proc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

// DefWindowProcA: tratamento padrao. WM_DESTROY -> PostQuitMessage.
__declspec(dllexport) long long DefWindowProcA(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    (void)hwnd; (void)wParam; (void)lParam;
    if (msg == WM_DESTROY) { NtUserPostQuitMessage(0); return 0; }
    return 0;
}

__declspec(dllexport) int PostMessageA(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    return (int)NtUserPostMessage(hwnd, msg, wParam, lParam);
}

__declspec(dllexport) void PostQuitMessage(int exitCode) {
    NtUserPostQuitMessage(exitCode);
}

__declspec(dllexport) int InvalidateRect(void* hwnd, void* rect, int erase) {
    (void)rect; (void)erase;
    return (int)NtUserInvalidate(hwnd);
}

// FASE 6 — SetFocus(hwnd): da o foco a janela (clique simulado / Alt+Tab). O
// teclado passa a ir para ela; devolve o hwnd (simplificado).
__declspec(dllexport) void* SetFocus(void* hwnd) {
    NtUserSetFocus(hwnd);
    return hwnd;
}

// FASE 6 — PostKeyToWindow(hwnd, ascii): injeta uma tecla (WM_KEYDOWN+WM_CHAR)
// numa janela especifica (estilo SendInput direcionado). Usado pelo modo demo
// para alimentar varias janelas de cmd de forma deterministica em headless.
__declspec(dllexport) int PostKeyToWindow(void* hwnd, int ascii) {
    return (int)NtUserPostKey(hwnd, ascii, 0);
}

// FASE 11 — GetCursorPos(POINT*): le a posicao atual do cursor. POINT tem
// layout {int x, int y} (mesmo do win32). Devolve 1 se preencheu, 0 senao.
__declspec(dllexport) int GetCursorPos(POINT* pt) {
    if (!pt) return 0;
    return (int)NtUserGetCursorPos(pt);
}

// FASE 11 — SetCursorPos(x, y): move o cursor para (x,y). Clamp aplicado no
// kernel (win32k_set_cursor) ao tamanho da tela.
__declspec(dllexport) int SetCursorPos(int x, int y) {
    return (int)NtUserSetCursorPos(x, y);
}

// FillRect(hdc, const RECT*, hbrush): preenche o retangulo com o brush. No
// Windows real e uma funcao do user32 (apesar de "GDI"); encaminha p/ o win32k.
__declspec(dllexport) int FillRect(void* hdc, const RECT* rc, void* hbrush) {
    if (!rc) return 0;
    return (int)NtGdiFillRect(hdc, rc->left, rc->top,
                              rc->right - rc->left, rc->bottom - rc->top, hbrush);
}

__declspec(dllexport) void* GetDC(void* hwnd) {
    return NtUserGetDC(hwnd);
}
__declspec(dllexport) int ReleaseDC(void* hwnd, void* hdc) {
    (void)hwnd; (void)hdc; return 1;   // HDC e simples; nada a liberar por ora
}

// BeginPaint/EndPaint: simplificado. PAINTSTRUCT guarda o HDC no offset 0.
typedef struct { void* hdc; int fErase; int rc[4]; int fRestore; int fIncUpdate; char rgb[32]; } PAINTSTRUCT;
__declspec(dllexport) void* BeginPaint(void* hwnd, PAINTSTRUCT* ps) {
    void* hdc = NtUserGetDC(hwnd);
    if (ps) { ps->hdc = hdc; ps->fErase = 0; }
    return hdc;
}
__declspec(dllexport) int EndPaint(void* hwnd, const PAINTSTRUCT* ps) {
    (void)hwnd; (void)ps; return 1;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
