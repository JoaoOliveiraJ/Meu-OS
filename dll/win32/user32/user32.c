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

// ============================================================================
// Frente C (explorer real) — user32 fase GUI: RECT/char reais; metricas/DPI/monitor com
// defaults sensatos (tela 1024x768, 96 DPI, 1 monitor); loaders devolvem handle nao-nulo;
// menus/input/hooks/composicao = stubs ESPECIFICOS nomeados (win32k proprio ainda nao
// cobre esses caminhos). O explorer importa 140 daqui alem das 24 basicas. Sem catch-all.
// ============================================================================
typedef unsigned short u16_;
static long long g_u32h = 0x9000;
static void* u32_h(void){ g_u32h += 8; return (void*)g_u32h; }

// ---- RECT/POINT: geometria REAL ----
__declspec(dllexport) int SetRect(RECT* r, int l, int t, int rr, int b){ if(!r) return 0; r->left=l;r->top=t;r->right=rr;r->bottom=b; return 1; }
__declspec(dllexport) int SetRectEmpty(RECT* r){ if(!r) return 0; r->left=r->top=r->right=r->bottom=0; return 1; }
__declspec(dllexport) int CopyRect(RECT* d, const RECT* s){ if(!d||!s) return 0; *d=*s; return 1; }
__declspec(dllexport) int IsRectEmpty(const RECT* r){ return (!r||r->left>=r->right||r->top>=r->bottom)?1:0; }
__declspec(dllexport) int EqualRect(const RECT* a, const RECT* b){ return (a&&b&&a->left==b->left&&a->top==b->top&&a->right==b->right&&a->bottom==b->bottom)?1:0; }
__declspec(dllexport) int InflateRect(RECT* r, int dx, int dy){ if(!r) return 0; r->left-=dx;r->top-=dy;r->right+=dx;r->bottom+=dy; return 1; }
__declspec(dllexport) int OffsetRect(RECT* r, int dx, int dy){ if(!r) return 0; r->left+=dx;r->top+=dy;r->right+=dx;r->bottom+=dy; return 1; }
__declspec(dllexport) int PtInRect(const RECT* r, POINT p){ return (r&&p.x>=r->left&&p.x<r->right&&p.y>=r->top&&p.y<r->bottom)?1:0; }
__declspec(dllexport) int IntersectRect(RECT* d, const RECT* a, const RECT* b){ if(!d||!a||!b) return 0; int l=a->left>b->left?a->left:b->left,t=a->top>b->top?a->top:b->top,rr=a->right<b->right?a->right:b->right,bo=a->bottom<b->bottom?a->bottom:b->bottom; if(l<rr&&t<bo){d->left=l;d->top=t;d->right=rr;d->bottom=bo;return 1;} d->left=d->top=d->right=d->bottom=0; return 0; }
__declspec(dllexport) int UnionRect(RECT* d, const RECT* a, const RECT* b){ if(!d||!a||!b) return 0; int ae=IsRectEmpty(a),be=IsRectEmpty(b); if(ae&&be){SetRectEmpty(d);return 0;} if(ae){*d=*b;return 1;} if(be){*d=*a;return 1;} d->left=a->left<b->left?a->left:b->left; d->top=a->top<b->top?a->top:b->top; d->right=a->right>b->right?a->right:b->right; d->bottom=a->bottom>b->bottom?a->bottom:b->bottom; return 1; }
__declspec(dllexport) int SubtractRect(RECT* d, const RECT* a, const RECT* b){ (void)b; if(!d||!a) return 0; *d=*a; return 1; }
__declspec(dllexport) int AdjustWindowRect(RECT* r, unsigned style, int menu){ (void)style; if(!r) return 0; r->left-=1; r->top-=(menu?43:23); r->right+=1; r->bottom+=1; return 1; }
__declspec(dllexport) int AdjustWindowRectEx(RECT* r, unsigned style, int menu, unsigned ex){ (void)ex; return AdjustWindowRect(r,style,menu); }

// ---- char ----
__declspec(dllexport) u16_* CharLowerW(u16_* s){ if(((unsigned long long)s)>>16){ for(u16_* p=s;*p;p++) if(*p>='A'&&*p<='Z')*p+=32; return s; } u16_ c=(u16_)(unsigned long long)s; if(c>='A'&&c<='Z')c+=32; return (u16_*)(unsigned long long)c; }
__declspec(dllexport) int IsCharAlphaNumericW(u16_ c){ return ((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z'))?1:0; }

// ---- metricas / cores / DPI ----
__declspec(dllexport) int GetSystemMetrics(int i){ switch(i){ case 0:return 1024; case 1:return 768; case 4:return 23; case 5: case 6:return 1; case 11: case 12: case 13: case 14:return 32; case 15:return 20; case 16:return 1024; case 17:return 745; case 43:return 3; case 45: case 46:return 2; case 49: case 50:return 16; default:return 0; } }
__declspec(dllexport) int GetSystemMetricsForDpi(int i, unsigned dpi){ (void)dpi; return GetSystemMetrics(i); }
__declspec(dllexport) unsigned GetSysColor(int i){ (void)i; return 0x00C0C0C0u; }
__declspec(dllexport) void* GetSysColorBrush(int i){ (void)i; return u32_h(); }
__declspec(dllexport) int SystemParametersInfoW(unsigned a, unsigned u, void* pv, unsigned w){ (void)a;(void)u;(void)pv;(void)w; return 1; }
__declspec(dllexport) unsigned GetDpiForWindow(void* hwnd){ (void)hwnd; return 96; }
__declspec(dllexport) unsigned GetDpiForSystem(void){ return 96; }
__declspec(dllexport) int IsProcessDPIAware(void){ return 1; }
__declspec(dllexport) void* SetThreadDpiAwarenessContext(void* c){ (void)c; return (void*)(long long)-4; }
__declspec(dllexport) void* GetWindowDpiAwarenessContext(void* hwnd){ (void)hwnd; return (void*)(long long)-4; }
__declspec(dllexport) int AreDpiAwarenessContextsEqual(void* a, void* b){ return a==b; }
__declspec(dllexport) unsigned GetDoubleClickTime(void){ return 500; }
__declspec(dllexport) unsigned GetCaretBlinkTime(void){ return 530; }

// ---- loaders (icone/cursor/imagem/menu): handle nao-nulo ----
__declspec(dllexport) void* LoadIconW(void* inst, const void* n){ (void)inst;(void)n; return u32_h(); }
__declspec(dllexport) void* LoadCursorW(void* inst, const void* n){ (void)inst;(void)n; return u32_h(); }
__declspec(dllexport) void* LoadImageW(void* inst, const void* n, unsigned ty, int cx, int cy, unsigned fl){ (void)inst;(void)n;(void)ty;(void)cx;(void)cy;(void)fl; return u32_h(); }
__declspec(dllexport) void* LoadMenuW(void* inst, const void* n){ (void)inst;(void)n; return u32_h(); }
__declspec(dllexport) void* LoadAcceleratorsW(void* inst, const void* n){ (void)inst;(void)n; return u32_h(); }
__declspec(dllexport) void* CopyIcon(void* ic){ (void)ic; return u32_h(); }
__declspec(dllexport) void* CopyImage(void* h, unsigned ty, int cx, int cy, unsigned fl){ (void)ty;(void)cx;(void)cy;(void)fl; return h; }
__declspec(dllexport) int DestroyIcon(void* ic){ (void)ic; return 1; }
__declspec(dllexport) void* CreateIconIndirect(void* info){ (void)info; return u32_h(); }
__declspec(dllexport) int DrawIconEx(void* hdc, int x, int y, void* ic, int cx, int cy, unsigned st, void* br, unsigned fl){ (void)hdc;(void)x;(void)y;(void)ic;(void)cx;(void)cy;(void)st;(void)br;(void)fl; return 1; }
__declspec(dllexport) int GetIconInfo(void* ic, void* info){ (void)ic; if(info){ unsigned char* b=(unsigned char*)info; for(int i=0;i<32;i++) b[i]=0; } return 1; }
__declspec(dllexport) int GetIconInfoExW(void* ic, void* info){ (void)ic;(void)info; return 0; }
__declspec(dllexport) void* SetCursor(void* c){ (void)c; return 0; }

// ---- menus ----
__declspec(dllexport) void* CreatePopupMenu(void){ return u32_h(); }
__declspec(dllexport) int DestroyMenu(void* m){ (void)m; return 1; }
__declspec(dllexport) int DeleteMenu(void* m, unsigned p, unsigned f){ (void)m;(void)p;(void)f; return 1; }
__declspec(dllexport) int InsertMenuW(void* m, unsigned p, unsigned f, unsigned long long id, const void* it){ (void)m;(void)p;(void)f;(void)id;(void)it; return 1; }
__declspec(dllexport) int ModifyMenuW(void* m, unsigned p, unsigned f, unsigned long long id, const void* it){ (void)m;(void)p;(void)f;(void)id;(void)it; return 1; }
__declspec(dllexport) int RemoveMenu(void* m, unsigned p, unsigned f){ (void)m;(void)p;(void)f; return 1; }
__declspec(dllexport) int EnableMenuItem(void* m, unsigned id, unsigned en){ (void)m;(void)id;(void)en; return 0; }
__declspec(dllexport) int CheckMenuItem(void* m, unsigned id, unsigned ck){ (void)m;(void)id;(void)ck; return 0; }
__declspec(dllexport) int GetMenuItemCount(void* m){ (void)m; return 0; }
__declspec(dllexport) unsigned GetMenuState(void* m, unsigned id, unsigned f){ (void)m;(void)id;(void)f; return 0xFFFFFFFFu; }
__declspec(dllexport) void* GetSubMenu(void* m, int p){ (void)m;(void)p; return 0; }
__declspec(dllexport) void* GetSystemMenu(void* hwnd, int rev){ (void)hwnd;(void)rev; return u32_h(); }
__declspec(dllexport) int TrackPopupMenuEx(void* m, unsigned f, int x, int y, void* hwnd, void* pr){ (void)m;(void)f;(void)x;(void)y;(void)hwnd;(void)pr; return 0; }
__declspec(dllexport) int GetMenuItemInfoW(void* m, unsigned it, int bp, void* info){ (void)m;(void)it;(void)bp;(void)info; return 0; }
__declspec(dllexport) int SetMenuItemInfoW(void* m, unsigned it, int bp, void* info){ (void)m;(void)it;(void)bp;(void)info; return 1; }
__declspec(dllexport) int GetMenuInfo(void* m, void* info){ (void)m;(void)info; return 1; }
__declspec(dllexport) int SetMenuInfo(void* m, void* info){ (void)m;(void)info; return 1; }
__declspec(dllexport) int GetMenuStringW(void* m, unsigned id, void* buf, int cch, unsigned f){ (void)m;(void)id;(void)f; if(buf&&cch>0)*(u16_*)buf=0; return 0; }
__declspec(dllexport) unsigned GetMenuDefaultItem(void* m, unsigned bp, unsigned f){ (void)m;(void)bp;(void)f; return 0xFFFFFFFFu; }
__declspec(dllexport) int SetMenuDefaultItem(void* m, unsigned it, unsigned bp){ (void)m;(void)it;(void)bp; return 1; }

// ---- monitores / display ----
__declspec(dllexport) void* MonitorFromWindow(void* hwnd, unsigned f){ (void)hwnd;(void)f; return (void*)(long long)0x10001; }
__declspec(dllexport) void* MonitorFromPoint(POINT p, unsigned f){ (void)p;(void)f; return (void*)(long long)0x10001; }
__declspec(dllexport) void* MonitorFromRect(const RECT* r, unsigned f){ (void)r;(void)f; return (void*)(long long)0x10001; }
__declspec(dllexport) int GetMonitorInfoW(void* mon, void* info){ (void)mon; if(!info) return 0; unsigned* p=(unsigned*)info; p[1]=0;p[2]=0;p[3]=1024;p[4]=768; p[5]=0;p[6]=0;p[7]=1024;p[8]=744; p[9]=1; return 1; }
__declspec(dllexport) int EnumDisplayMonitors(void* hdc, const RECT* clip, void* cb, long long data){ (void)hdc;(void)clip; if(cb){ RECT r; r.left=0;r.top=0;r.right=1024;r.bottom=768; ((int(*)(void*,void*,RECT*,long long))cb)((void*)(long long)0x10001,0,&r,data); } return 1; }
__declspec(dllexport) int EnumDisplayDevicesW(const void* dev, unsigned n, void* info, unsigned f){ (void)dev;(void)n;(void)info;(void)f; return 0; }
__declspec(dllexport) long GetDisplayConfigBufferSizes(unsigned f, unsigned* np, unsigned* nm){ (void)f; if(np)*np=0; if(nm)*nm=0; return 0; }
__declspec(dllexport) long QueryDisplayConfig(unsigned f, unsigned* np, void* pi, unsigned* nm, void* mi, void* topo){ (void)f;(void)pi;(void)mi;(void)topo; if(np)*np=0; if(nm)*nm=0; return 0; }

// ---- input / pointer ----
__declspec(dllexport) int EnableMouseInPointer(int en){ (void)en; return 1; }
__declspec(dllexport) int GetPointerInfo(unsigned id, void* info){ (void)id;(void)info; return 0; }
__declspec(dllexport) int GetPointerType(unsigned id, unsigned* ty){ (void)id; if(ty)*ty=1; return 1; }
__declspec(dllexport) int GetPointerDevices(unsigned* c, void* dv){ (void)dv; if(c)*c=0; return 1; }
__declspec(dllexport) short GetAsyncKeyState(int vk){ (void)vk; return 0; }
__declspec(dllexport) short GetKeyState(int vk){ (void)vk; return 0; }
__declspec(dllexport) unsigned MapVirtualKeyExW(unsigned code, unsigned ty, void* hkl){ (void)ty;(void)hkl; return code; }
__declspec(dllexport) int GetLastInputInfo(void* info){ if(info) ((unsigned*)info)[1]=0; return 1; }
__declspec(dllexport) int TrackMouseEvent(void* ev){ (void)ev; return 1; }
__declspec(dllexport) unsigned InjectMouseInput(const void* in, unsigned n){ (void)in;(void)n; return 0; }
__declspec(dllexport) unsigned InjectKeyboardInput(const void* in, unsigned n){ (void)in;(void)n; return 0; }
__declspec(dllexport) int RegisterHotKey(void* hwnd, int id, unsigned mod, unsigned vk){ (void)hwnd;(void)id;(void)mod;(void)vk; return 1; }
__declspec(dllexport) int UnregisterHotKey(void* hwnd, int id){ (void)hwnd;(void)id; return 1; }
__declspec(dllexport) int GetPhysicalCursorPos(POINT* p){ if(p){p->x=0;p->y=0;} return 1; }
__declspec(dllexport) int GetCursorInfo(void* info){ (void)info; return 0; }
__declspec(dllexport) void* SetCapture(void* hwnd){ (void)hwnd; return 0; }
__declspec(dllexport) void* GetCapture(void){ return 0; }
__declspec(dllexport) int ReleaseCapture(void){ return 1; }
__declspec(dllexport) long SetGestureConfig(void* hwnd, unsigned rs, unsigned ci, void* cfg, unsigned sz){ (void)hwnd;(void)rs;(void)ci;(void)cfg;(void)sz; return 1; }

// ---- gerencia de janela ----
__declspec(dllexport) int BringWindowToTop(void* hwnd){ (void)hwnd; return 1; }
__declspec(dllexport) int ShowWindowAsync(void* hwnd, int cmd){ (void)hwnd;(void)cmd; return 1; }
__declspec(dllexport) void SwitchToThisWindow(void* hwnd, int alt){ (void)hwnd;(void)alt; }
__declspec(dllexport) int IsIconic(void* hwnd){ (void)hwnd; return 0; }
__declspec(dllexport) int IsHungAppWindow(void* hwnd){ (void)hwnd; return 0; }
__declspec(dllexport) int IsWindowUnicode(void* hwnd){ (void)hwnd; return 1; }
__declspec(dllexport) int IsTopLevelWindow(void* hwnd){ (void)hwnd; return 1; }
__declspec(dllexport) int InternalGetWindowText(void* hwnd, void* buf, int cch){ (void)hwnd; if(buf&&cch>0)*(u16_*)buf=0; return 0; }
__declspec(dllexport) unsigned long long GetClassLongPtrW(void* hwnd, int i){ (void)hwnd;(void)i; return 0; }
__declspec(dllexport) unsigned GetClassLongW(void* hwnd, int i){ (void)hwnd;(void)i; return 0; }
__declspec(dllexport) unsigned short GetClassWord(void* hwnd, int i){ (void)hwnd;(void)i; return 0; }
__declspec(dllexport) void* GetLastActivePopup(void* hwnd){ return hwnd; }
__declspec(dllexport) void* GetWindowProcessHandle(void* hwnd){ (void)hwnd; return 0; }
__declspec(dllexport) int EndTask(void* hwnd, int shut, int force){ (void)hwnd;(void)shut;(void)force; return 1; }
__declspec(dllexport) int CascadeWindows(void* par, unsigned how, const RECT* r, unsigned n, const void* kids){ (void)par;(void)how;(void)r;(void)n;(void)kids; return 0; }
__declspec(dllexport) int TileWindows(void* par, unsigned how, const RECT* r, unsigned n, const void* kids){ (void)par;(void)how;(void)r;(void)n;(void)kids; return 0; }
__declspec(dllexport) void* GhostWindowFromHungWindow(void* hwnd){ (void)hwnd; return 0; }
__declspec(dllexport) void* HungWindowFromGhostWindow(void* hwnd){ (void)hwnd; return 0; }
__declspec(dllexport) int GetWindowBand(void* hwnd, unsigned* band){ (void)hwnd; if(band)*band=0; return 1; }
__declspec(dllexport) void* CreateWindowInBand(unsigned ex, const void* cls, const void* name, unsigned style, int x, int y, int w, int h, void* par, void* menu, void* inst, void* pm, unsigned band){ (void)ex;(void)cls;(void)name;(void)style;(void)x;(void)y;(void)w;(void)h;(void)par;(void)menu;(void)inst;(void)pm;(void)band; return 0; }

// ---- texto ----
__declspec(dllexport) int DrawTextW(void* hdc, const void* txt, int cch, RECT* r, unsigned fmt){ (void)hdc;(void)txt;(void)cch; if((fmt&0x400u)&&r) r->bottom=r->top+16; return 16; }
__declspec(dllexport) int DrawTextExW(void* hdc, void* txt, int cch, RECT* r, unsigned fmt, void* pr){ (void)pr; return DrawTextW(hdc,txt,cch,r,fmt); }

// ---- loop de mensagens / dialogo / hooks / composicao ----
__declspec(dllexport) unsigned MsgWaitForMultipleObjects(unsigned n, const void* h, int all, unsigned ms, unsigned mask){ (void)n;(void)h;(void)all;(void)ms;(void)mask; return 0x102u; }
__declspec(dllexport) unsigned MsgWaitForMultipleObjectsEx(unsigned n, const void* h, unsigned ms, unsigned mask, unsigned fl){ (void)n;(void)h;(void)ms;(void)mask;(void)fl; return 0x102u; }
__declspec(dllexport) int PostThreadMessageW(unsigned tid, unsigned msg, unsigned long long wp, long long lp){ (void)tid;(void)msg;(void)wp;(void)lp; return 1; }
__declspec(dllexport) int ReplyMessage(long long res){ (void)res; return 1; }
__declspec(dllexport) long long SendDlgItemMessageW(void* dlg, int id, unsigned msg, unsigned long long wp, long long lp){ (void)dlg;(void)id;(void)msg;(void)wp;(void)lp; return 0; }
__declspec(dllexport) unsigned RegisterClipboardFormatW(const void* n){ (void)n; return 0xC000u; }
__declspec(dllexport) int ChangeWindowMessageFilterEx(void* hwnd, unsigned msg, unsigned act, void* st){ (void)hwnd;(void)msg;(void)act;(void)st; return 1; }
__declspec(dllexport) int GetCurrentInputMessageSource(void* src){ (void)src; return 1; }
__declspec(dllexport) int EndDialog(void* dlg, long long res){ (void)dlg;(void)res; return 1; }
__declspec(dllexport) int TranslateAcceleratorW(void* hwnd, void* acc, void* msg){ (void)hwnd;(void)acc;(void)msg; return 0; }
__declspec(dllexport) void* SetWinEventHook(unsigned mn, unsigned mx, void* mod, void* cb, unsigned pid, unsigned tid, unsigned fl){ (void)mn;(void)mx;(void)mod;(void)cb;(void)pid;(void)tid;(void)fl; return u32_h(); }
__declspec(dllexport) int UnhookWinEvent(void* hk){ (void)hk; return 1; }
__declspec(dllexport) void NotifyWinEvent(unsigned ev, void* hwnd, long obj, long child){ (void)ev;(void)hwnd;(void)obj;(void)child; }
__declspec(dllexport) int GetScrollInfo(void* hwnd, int bar, void* info){ (void)hwnd;(void)bar;(void)info; return 0; }
__declspec(dllexport) int SetScrollInfo(void* hwnd, int bar, const void* info, int rd){ (void)hwnd;(void)bar;(void)info;(void)rd; return 0; }
__declspec(dllexport) int SetScrollPos(void* hwnd, int bar, int pos, int rd){ (void)hwnd;(void)bar;(void)pos;(void)rd; return 0; }
__declspec(dllexport) int GetLayeredWindowAttributes(void* hwnd, unsigned* key, unsigned char* al, unsigned* fl){ (void)hwnd; if(key)*key=0; if(al)*al=255; if(fl)*fl=0; return 1; }
__declspec(dllexport) int SetLayeredWindowAttributes(void* hwnd, unsigned key, unsigned char al, unsigned fl){ (void)hwnd;(void)key;(void)al;(void)fl; return 1; }
__declspec(dllexport) int UpdateLayeredWindow(void* hwnd, void* dd, POINT* pd, void* sz, void* sd, POINT* ps, unsigned key, void* bl, unsigned fl){ (void)hwnd;(void)dd;(void)pd;(void)sz;(void)sd;(void)ps;(void)key;(void)bl;(void)fl; return 1; }
__declspec(dllexport) int GetWindowCompositionAttribute(void* hwnd, void* d){ (void)hwnd;(void)d; return 0; }
__declspec(dllexport) int SetWindowCompositionAttribute(void* hwnd, void* d){ (void)hwnd;(void)d; return 1; }
__declspec(dllexport) long SetWindowFeedbackSetting(void* hwnd, int fb, unsigned fl, unsigned sz, const void* cfg){ (void)hwnd;(void)fb;(void)fl;(void)sz;(void)cfg; return 1; }
__declspec(dllexport) int CalculatePopupWindowPosition(const POINT* an, const void* sz, unsigned fl, RECT* ex, RECT* out){ (void)an;(void)sz;(void)fl;(void)ex; if(out){out->left=0;out->top=0;out->right=100;out->bottom=100;} return 1; }

// ---- misc de sistema ----
__declspec(dllexport) int ExitWindowsEx(unsigned fl, unsigned reason){ (void)fl;(void)reason; return 1; }
__declspec(dllexport) int LockWorkStation(void){ return 1; }
__declspec(dllexport) void* RegisterPowerSettingNotification(void* h, const void* g, unsigned fl){ (void)h;(void)g;(void)fl; return u32_h(); }
__declspec(dllexport) int UnregisterPowerSettingNotification(void* h){ (void)h; return 1; }
__declspec(dllexport) int UnregisterClassW(const void* cls, void* inst){ (void)cls;(void)inst; return 1; }
__declspec(dllexport) int UnregisterClassA(const void* cls, void* inst){ (void)cls;(void)inst; return 1; }
__declspec(dllexport) unsigned GetGuiResources(void* proc, unsigned fl){ (void)proc;(void)fl; return 0; }
// FindWindow*: NULL = janela nao encontrada. O explorer usa p/ detectar outra instancia
// do shell (Shell_TrayWnd/Progman) — NULL = "sou o primeiro", que e' o que queremos.
__declspec(dllexport) void* FindWindowW(const void* cls, const void* name){ (void)cls;(void)name; return 0; }
__declspec(dllexport) void* FindWindowExW(void* parent, void* after, const void* cls, const void* name){ (void)parent;(void)after;(void)cls;(void)name; return 0; }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
