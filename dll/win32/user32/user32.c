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

// ---- TRACE de diagnostico (ring 3 -> serial via sys_write, syscall #1) ----
// Ativado so p/ investigar o fluxo do explorer real; deixe 0 nos commits.
#define U32_TRACE 0
#if U32_TRACE
static void u32_trace_s(const char* s){ long long r; __asm__ volatile("int $0x80":"=a"(r):"a"(1LL),"D"(s):"memory"); }
static void u32_trace_h(const char* label, unsigned long long v){
    char b[48]; int i=0; const char* hx="0123456789abcdef";
    while(label[i] && i<28){ b[i]=label[i]; i++; }
    b[i++]=' '; b[i++]='0'; b[i++]='x';
    for(int s=60;s>=0;s-=4) b[i++]=hx[(v>>s)&0xf];
    b[i++]='\n'; b[i]=0; u32_trace_s(b);
}
#else
static void u32_trace_s(const char* s){ (void)s; }
static void u32_trace_h(const char* label, unsigned long long v){ (void)label; (void)v; }
#endif

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
#define WM_CREATE   0x0001
#define WM_DESTROY  0x0002
#define WM_PAINT    0x000F
#define WM_QUIT     0x0012
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_KEYDOWN  0x0100
#define WM_CHAR     0x0102

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

// Forward: converte um nome de classe/janela wide (UTF-16) para ASCII agrupado
// (a definicao real fica adiante, junto das W-variants).
static const char* u32_wpool(const void* ws);

// Resolve uma referencia de classe que pode ser um ATOM (valor < 0x10000, como o que
// RegisterClass* devolve) OU um ponteiro de string. Nossos atoms = 0xC000 + slot; aqui
// resolvemos o atom de volta ao NOME ASCII registrado. Sem isto, criar uma janela por
// ATOM (ex.: o explorer real chama CreateWindowInBand(atom, ...) com o atom devolvido por
// RegisterClassExW) derefaria um inteiro pequeno como string -> page fault em RING-3.
#define U32_ATOM_BASE 0xC000u
static const char* class_ref_to_name_w(const void* c) {
    unsigned long long v = (unsigned long long)c;
    if (!v) return 0;
    if (v < 0x10000ULL) {                                    // ATOM (MAKEINTATOM)
        if (v >= U32_ATOM_BASE && v < U32_ATOM_BASE + MAX_CLASSES && g_classes[v - U32_ATOM_BASE].used)
            return g_classes[v - U32_ATOM_BASE].name;
        return 0;                                            // atom desconhecido (classe de sistema)
    }
    return u32_wpool(c);                                     // string wide -> ascii agrupada
}
static const char* class_ref_to_name_a(const char* c) {
    unsigned long long v = (unsigned long long)c;
    if (!v) return 0;
    if (v < 0x10000ULL) {                                    // ATOM
        if (v >= U32_ATOM_BASE && v < U32_ATOM_BASE + MAX_CLASSES && g_classes[v - U32_ATOM_BASE].used)
            return g_classes[v - U32_ATOM_BASE].name;
        return 0;
    }
    return c;                                                // string ascii direta
}

// ---- armazenamento REAL de Window Long Ptr (GWLP_USERDATA etc.) por (hwnd, indice) ----
// O wndproc real da Worker Window do explorer (RVA 0x72e00) guarda o ponteiro do seu objeto
// C++ em GWLP_USERDATA (-21) no WM_NCCREATE (via SetWindowLongPtrW) e o recupera em CADA
// mensagem (GetWindowLongPtrW) p/ despachar ao metodo do objeto. Sem isto (era no-op -> 0) o
// wndproc caia SEMPRE em DefWindowProc. Tabela simples por (hwnd, indice).
#define U32_MAX_WLP 128
static struct { void* hwnd; int idx; long long val; } g_wlp[U32_MAX_WLP];
static long long wlp_get(void* hwnd, int idx) {
    for (int i = 0; i < U32_MAX_WLP; i++) if (g_wlp[i].hwnd == hwnd && g_wlp[i].idx == idx) return g_wlp[i].val;
    return 0;
}
static long long wlp_set(void* hwnd, int idx, long long v) {
    for (int i = 0; i < U32_MAX_WLP; i++) if (g_wlp[i].hwnd == hwnd && g_wlp[i].idx == idx) { long long o = g_wlp[i].val; g_wlp[i].val = v; return o; }
    for (int i = 0; i < U32_MAX_WLP; i++) if (!g_wlp[i].hwnd) { g_wlp[i].hwnd = hwnd; g_wlp[i].idx = idx; g_wlp[i].val = v; return 0; }
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
    return (unsigned short)(U32_ATOM_BASE + slot);   // ATOM unico por classe (resolvivel em CreateWindow*)
}

// CREATESTRUCTW (layout Win32 x64) — o wndproc real le lpCreateParams no offset 0 (o
// ponteiro do objeto C++ passado como lParam do CreateWindow). Preenchemos o suficiente.
typedef struct { void* lpCreateParams; void* hInstance; void* hMenu; void* hwndParent;
                 int cy, cx, y, x; unsigned style; const void* lpszName, *lpszClass; unsigned dwExStyle; } CREATESTRUCTW;

// Implementacao comum: cria a janela com bgColor + flags (FASE 6). O kernel preenche os
// campos novos; lembra o (hwnd -> wndproc) para o despacho local. `lpParam` = o lParam do
// CreateWindow* (o explorer passa o ponteiro do objeto da Worker Window aqui).
static void* create_window_impl(const char* className, const char* windowName,
        unsigned style, int x, int y, int w, int h, void* parent,
        unsigned bgColor, unsigned flags, void* lpParam) {
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
    u32_trace_h("u32:create hwnd", (unsigned long long)hwnd);
    u32_trace_h("u32:create proc", (unsigned long long)(void*)proc);
    // Como no Windows real: envia WM_NCCREATE ao wndproc INLINE, ANTES de CreateWindow
    // retornar. O wndproc do explorer associa aqui seu objeto C++ via SetWindowLongPtrW
    // (GWLP_USERDATA) — o que passa a fazer o despacho (GetWindowLongPtrW) achar o objeto
    // e chamar o handler real (antes caia em DefWindowProc). O retorno e' ignorado (nao
    // falhamos a criacao) p/ nao regredir apps simples. WM_CREATE segue pela fila do kernel.
    if (hwnd && proc) {
        CREATESTRUCTW cs; unsigned char* z = (unsigned char*)&cs;
        for (unsigned i = 0; i < sizeof(cs); i++) z[i] = 0;
        cs.lpCreateParams = lpParam; cs.style = style; cs.x = x; cs.y = y; cs.cx = w; cs.cy = h;
        cs.hwndParent = parent; cs.lpszName = windowName; cs.lpszClass = className;
        proc(hwnd, WM_NCCREATE, 0, (ULL)(unsigned long long)(unsigned char*)&cs);
    }
    return hwnd;
}

// CreateWindowExA: janela classica (area cliente cinza, com chrome). Compat. Fase 2.
__declspec(dllexport) void* CreateWindowExA(unsigned exStyle, const char* className,
        const char* windowName, unsigned style, int x, int y, int w, int h,
        void* parent, void* menu, void* inst, void* param) {
    (void)exStyle; (void)menu; (void)inst;
    return create_window_impl(class_ref_to_name_a(className), windowName, style, x, y, w, h, parent,
                              WND_BG_DEFAULT, 0, param);
}

// FASE 6 — CreateDesktopWindowA: janela de FUNDO (papel de parede). Cobre a tela,
// sem chrome, cor de fundo 'bgColor'. Fica no fundo do z-order; nao toma o foco.
__declspec(dllexport) void* CreateDesktopWindowA(const char* className,
        const char* title, unsigned bgColor, WNDPROC unusedProc) {
    (void)unusedProc;
    return create_window_impl(className, title, 0, 0, 0, 0, 0, 0,
                              bgColor, WNDF_DESKTOP, 0);
}

// FASE 6 — CreateConsoleWindowA: janela de CONSOLE (cmd). Area cliente escura
// (bgColor), com chrome e barra de titulo; recebe foco e teclado normalmente.
__declspec(dllexport) void* CreateConsoleWindowA(const char* className,
        const char* title, int x, int y, int w, int h, unsigned bgColor) {
    return create_window_impl(className, title, 0, x, y, w, h, 0,
                              bgColor, WNDF_CONSOLE, 0);
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
    u32_trace_h("u32:DestroyWindow ra", (unsigned long long)__builtin_return_address(0));
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
    if (msg == WM_NCCREATE) return 1;   // real DefWindowProc: TRUE p/ prosseguir a criacao
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
// CreateWindowInBand: como CreateWindowExW + um 'band' (faixa de z-order, Win8+). O explorer
// REAL cria a sua "Worker Window" por aqui (import privado api-ms-win-rtcore-ntuser-private!
// CreateWindowInBand), passando o ATOM devolvido por RegisterClassExW como className. Antes
// era um stub -> 0, e o explorer, vendo hwnd==NULL, DESISTIA (wWinMain retornava 0). Agora
// cria de verdade (mesmo caminho do CreateWindowExW), resolvendo o ATOM -> nome de classe.
__declspec(dllexport) void* CreateWindowInBand(unsigned ex, const void* cls, const void* name, unsigned style, int x, int y, int w, int h, void* par, void* menu, void* inst, void* pm, unsigned band){ (void)ex;(void)menu;(void)inst;(void)band; return create_window_impl(class_ref_to_name_w(cls), u32_wpool(name), style, x, y, w, h, par, WND_BG_DEFAULT, 0, pm); }
// CreateWindowInBandEx: variante com typeFlags extra (mesma criacao; band+flags ignorados).
__declspec(dllexport) void* CreateWindowInBandEx(unsigned ex, const void* cls, const void* name, unsigned style, int x, int y, int w, int h, void* par, void* menu, void* inst, void* pm, unsigned band, unsigned typeFlags){ (void)ex;(void)menu;(void)inst;(void)band;(void)typeFlags; return create_window_impl(class_ref_to_name_w(cls), u32_wpool(name), style, x, y, w, h, par, WND_BG_DEFAULT, 0, pm); }

// ---- ordinais PRIVADOS/noname do user32 que o explorer real importa POR ORDINAL ----
// Exportados nos ordinais da MS via user32.def (aditivo). Sem NOME publico na user32 real;
// implementados como stubs cujo retorno foi escolhido a partir do USO no explorer (RE das
// call-sites) para NAO provocar um bail. Nao levam __declspec: o .def os exporta.
//   #2522(hwnd): chamado logo apos criar a "Worker Window" (CreateWindowInBand); o explorer
//                IGNORA o retorno -> stub inofensivo. Era o slot=0 que crashava (rip=0).
//   #2005(p):    retorno IGNORADO em todas as call-sites (o arg e' relido depois) -> passthrough.
//   #2521:       GetProcessUIContextInformation-like -> FALSE (processo sem UI-context de
//                app-container; caso normal do explorer classico).
//   #2573/#2574/#2611: fora do caminho atual; retornam 0 (revisar se/quando alcancados).
void* User32_ord2005(void* a){ return a; }
int   User32_ord2521_GetProcessUIContextInformation(void* proc, void* out){ (void)proc; (void)out; return 0; }
int   User32_ord2522(void* hwnd){ (void)hwnd; return 1; }
int   User32_ord2573(void* a){ (void)a; return 0; }
int   User32_ord2574(void* hwnd){ (void)hwnd; return 0; }
int   User32_ord2611(void* a){ (void)a; return 0; }

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

// ============================================================================
// Frente C (explorer real) — W-VARIANTS de criacao/gerencia de janela. O explorer importa
// 72 funcoes de janela (RegisterClassExW/CreateWindowExW/loop de msg/geometria/props/timers/
// hooks) do apiset ext-ms-win-rtcore-ntuser-window-ext (delay), que redireciona p/ ca. Sem
// as W-variants, elas bindavam ao LdrpNullStub (devolve 0) e a criacao da Shell_TrayWnd
// FALHAVA EM SILENCIO -> o explorer abortava a init da taskbar. Aqui as W delegam ao MESMO
// win32k (Nt*) das A-variants, convertendo UTF-16->ascii. Sem catch-all: cada uma nomeada.
// ============================================================================
// pool persistente p/ nomes de classe/janela convertidos de wide (o kernel guarda o ponteiro).
static char g_wpool[16384]; static int g_wpool_off = 0;
static const char* u32_wpool(const void* ws) {
    if (!ws) return 0;
    const u16_* w = (const u16_*)ws;
    if ((unsigned long long)ws < 0x10000ULL) return (const char*)ws;   // ATOM (MAKEINTATOM) -> passa cru
    char* out = &g_wpool[g_wpool_off]; int n = 0;
    while (w[n] && g_wpool_off + n < (int)sizeof(g_wpool) - 1) { out[n] = (char)w[n]; n++; }
    out[n] = 0; g_wpool_off += n + 1; return out;
}
static int u32_weq(const u16_* a, const u16_* b) {   // compara wide-strings
    if (!a || !b) return 0; while (*a && *b) { if (*a != *b) return 0; a++; b++; } return *a == *b;
}

// WNDCLASSW / WNDCLASSEXW (layout Win32). So os campos que usamos: wndproc + className.
typedef struct { unsigned style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; void* hInstance,*hIcon,*hCursor,*hbrBackground; const u16_* lpszMenuName,*lpszClassName; } WNDCLASSW;
typedef struct { unsigned cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; void* hInstance,*hIcon,*hCursor,*hbrBackground; const u16_* lpszMenuName,*lpszClassName; void* hIconSm; } WNDCLASSEXW;

static unsigned short u32_register_class(const u16_* clsName, WNDPROC proc) {
    const char* nm = u32_wpool(clsName);
    if (!nm) return 0;
    int slot = -1;
    for (int i = 0; i < MAX_CLASSES; i++) if (g_classes[i].used && seq(g_classes[i].name, nm)) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < MAX_CLASSES; i++) if (!g_classes[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    g_classes[slot].used = 1; g_classes[slot].name = nm; g_classes[slot].proc = proc;
    NtUserRegisterClass(nm, (void*)proc);
    return (unsigned short)(U32_ATOM_BASE + slot);   // ATOM unico por classe (resolvivel em CreateWindow*)
}
__declspec(dllexport) unsigned short RegisterClassW(const WNDCLASSW* wc) { return wc ? u32_register_class(wc->lpszClassName, wc->lpfnWndProc) : 0; }
__declspec(dllexport) unsigned short RegisterClassExW(const WNDCLASSEXW* wc) { return wc ? u32_register_class(wc->lpszClassName, wc->lpfnWndProc) : 0; }

__declspec(dllexport) void* CreateWindowExW(unsigned exStyle, const u16_* className, const u16_* windowName,
        unsigned style, int x, int y, int w, int h, void* parent, void* menu, void* inst, void* param) {
    (void)exStyle; (void)menu; (void)inst;
    return create_window_impl(class_ref_to_name_w(className), u32_wpool(windowName), style, x, y, w, h, parent, WND_BG_DEFAULT, 0, param);
}
__declspec(dllexport) long long DefWindowProcW(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    if (msg == WM_NCCREATE) return 1;   // real DefWindowProc: TRUE p/ prosseguir a criacao
    if (msg == WM_DESTROY) { NtUserPostQuitMessage(0); return 0; }
    (void)hwnd; (void)wParam; (void)lParam; return 0;
}

// ---- loop de mensagens (W) — delega ao mesmo win32k das A-variants ----
__declspec(dllexport) int GetMessageW(MSG* msg, void* hwnd, unsigned mn, unsigned mx) { (void)hwnd;(void)mn;(void)mx; return (int)NtUserGetMessage(msg); }
__declspec(dllexport) int PeekMessageW(MSG* msg, void* hwnd, unsigned mn, unsigned mx, unsigned rm) {
    (void)hwnd;(void)mn;(void)mx;(void)rm; if (msg) { msg->hwnd=0; msg->message=0; msg->wParam=0; msg->lParam=0; } return 0;  // sem msg pendente
}
__declspec(dllexport) long long DispatchMessageW(const MSG* msg) {
    if (!msg) return 0;
    NtUserDispatchMessage((void*)msg);
    WNDPROC proc = wndproc_of(msg->hwnd);
    if (!proc) return DefWindowProcW(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    return proc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}
__declspec(dllexport) int PostMessageW(void* hwnd, unsigned msg, ULL w, ULL l) { return (int)NtUserPostMessage(hwnd, msg, w, l); }
__declspec(dllexport) long long SendMessageW(void* hwnd, unsigned msg, ULL w, ULL l) {
    u32_trace_h("u32:SendMsgW msg", (unsigned long long)msg);
    u32_trace_h("u32:SendMsgW ra", (unsigned long long)__builtin_return_address(0));
    WNDPROC proc = wndproc_of(hwnd);   // SendMessage e' sincrono: chama o wndproc direto (ring 3)
    if (proc) return proc(hwnd, msg, w, l);
    return DefWindowProcW(hwnd, msg, w, l);
}
__declspec(dllexport) long long SendMessageTimeoutW(void* hwnd, unsigned msg, ULL w, ULL l, unsigned fl, unsigned to, ULL* res) { (void)fl;(void)to; long long r=SendMessageW(hwnd,msg,w,l); if(res)*res=(ULL)r; return 1; }
__declspec(dllexport) int SendNotifyMessageW(void* hwnd, unsigned msg, ULL w, ULL l) { return (int)PostMessageW(hwnd,msg,w,l); }

// ---- geometria / posicao (tela fixa 1024x768) ----
__declspec(dllexport) int GetClientRect(void* hwnd, RECT* r) { (void)hwnd; if(!r) return 0; r->left=0; r->top=0; r->right=1024; r->bottom=768; return 1; }
__declspec(dllexport) int GetWindowRect(void* hwnd, RECT* r) { (void)hwnd; if(!r) return 0; r->left=0; r->top=0; r->right=1024; r->bottom=768; return 1; }
__declspec(dllexport) void* GetDesktopWindow(void) { return (void*)1; }   // hwnd do desktop (nao-nulo, referencia)
__declspec(dllexport) int SetWindowPos(void* hwnd, void* after, int x, int y, int w, int h, unsigned fl) { (void)hwnd;(void)after;(void)x;(void)y;(void)w;(void)h;(void)fl; return 1; }
__declspec(dllexport) int ScreenToClient(void* hwnd, POINT* p) { (void)hwnd; (void)p; return 1; }   // identidade (janela na origem)
__declspec(dllexport) int ClientToScreen(void* hwnd, POINT* p) { (void)hwnd; (void)p; return 1; }
__declspec(dllexport) int MapWindowPoints(void* from, void* to, POINT* pts, unsigned n) { (void)from;(void)to;(void)pts;(void)n; return 0; }
__declspec(dllexport) int GetWindowInfo(void* hwnd, void* pwi) { (void)hwnd; if(pwi){ unsigned char* b=(unsigned char*)pwi; for(int i=0;i<60;i++) b[i]=0; } return 1; }

// ---- atributos de janela (GWL_*) ----
// Window Long Ptr REAIS (armazenamento por hwnd/indice em g_wlp) — GWLP_USERDATA e cia.
__declspec(dllexport) long long GetWindowLongPtrW(void* hwnd, int idx) { return wlp_get(hwnd, idx); }
__declspec(dllexport) long long SetWindowLongPtrW(void* hwnd, int idx, long long v) { return wlp_set(hwnd, idx, v); }
__declspec(dllexport) long long GetWindowLongPtrA(void* hwnd, int idx) { return wlp_get(hwnd, idx); }
__declspec(dllexport) long long SetWindowLongPtrA(void* hwnd, int idx, long long v) { return wlp_set(hwnd, idx, v); }
__declspec(dllexport) long GetWindowLongW(void* hwnd, int idx) { return (long)wlp_get(hwnd, idx); }
__declspec(dllexport) long SetWindowLongW(void* hwnd, int idx, long v) { return (long)wlp_set(hwnd, idx, (long long)v); }
__declspec(dllexport) long GetWindowLongA(void* hwnd, int idx) { return (long)wlp_get(hwnd, idx); }
__declspec(dllexport) long SetWindowLongA(void* hwnd, int idx, long v) { return (long)wlp_set(hwnd, idx, (long long)v); }

// ---- props (janela -> dado). O explorer guarda ponteiros nas janelas. Tabela por (hwnd,nome). ----
#define U32_MAX_PROPS 128
static struct { void* hwnd; const u16_* name; void* val; } g_props[U32_MAX_PROPS];
static int prop_find(void* hwnd, const u16_* name) {
    for (int i = 0; i < U32_MAX_PROPS; i++) if (g_props[i].hwnd == hwnd && g_props[i].name) {
        if (name && (unsigned long long)name < 0x10000ULL) { if (g_props[i].name == name) return i; }   // ATOM
        else if (u32_weq(g_props[i].name, name)) return i;
    }
    return -1;
}
__declspec(dllexport) int SetPropW(void* hwnd, const u16_* name, void* val) {
    int i = prop_find(hwnd, name);
    if (i < 0) for (int k = 0; k < U32_MAX_PROPS; k++) if (!g_props[k].hwnd) { i = k; break; }
    if (i < 0) return 0;
    g_props[i].hwnd = hwnd; g_props[i].name = name; g_props[i].val = val; return 1;
}
__declspec(dllexport) void* GetPropW(void* hwnd, const u16_* name) { int i = prop_find(hwnd, name); return i >= 0 ? g_props[i].val : 0; }
__declspec(dllexport) void* RemovePropW(void* hwnd, const u16_* name) { int i = prop_find(hwnd, name); if (i < 0) return 0; void* v = g_props[i].val; g_props[i].hwnd = 0; g_props[i].name = 0; g_props[i].val = 0; return v; }

// ---- timers / hooks ----
__declspec(dllexport) unsigned long long SetTimer(void* hwnd, unsigned long long id, unsigned el, void* proc) { (void)hwnd;(void)el;(void)proc; return id ? id : 1; }
__declspec(dllexport) unsigned long long SetCoalescableTimer(void* hwnd, unsigned long long id, unsigned el, void* proc, unsigned tol) { (void)tol; return SetTimer(hwnd,id,el,proc); }
__declspec(dllexport) int KillTimer(void* hwnd, unsigned long long id) { (void)hwnd;(void)id; return 1; }
__declspec(dllexport) void* SetWindowsHookExW(int id, void* fn, void* mod, unsigned tid) { (void)id;(void)fn;(void)mod;(void)tid; return u32_h(); }
__declspec(dllexport) int UnhookWindowsHookEx(void* h) { (void)h; return 1; }
__declspec(dllexport) long long CallNextHookEx(void* h, int code, ULL w, ULL l) { (void)h;(void)code;(void)w;(void)l; return 0; }

// ---- consultas de janela ----
__declspec(dllexport) int IsWindow(void* hwnd) { return hwnd ? 1 : 0; }
__declspec(dllexport) int IsWindowVisible(void* hwnd) { return hwnd ? 1 : 0; }
__declspec(dllexport) int IsWindowEnabled(void* hwnd) { return hwnd ? 1 : 0; }
__declspec(dllexport) void* GetParent(void* hwnd) { (void)hwnd; return 0; }
__declspec(dllexport) void* GetAncestor(void* hwnd, unsigned fl) { (void)fl; return hwnd; }
__declspec(dllexport) void* GetForegroundWindow(void) { return 0; }
__declspec(dllexport) int SetForegroundWindow(void* hwnd) { (void)hwnd; return 1; }
__declspec(dllexport) void* GetFocus(void) { return 0; }
__declspec(dllexport) void* GetWindow(void* hwnd, unsigned cmd) { (void)hwnd;(void)cmd; return 0; }
__declspec(dllexport) int EnumWindows(void* proc, ULL lparam) { (void)proc;(void)lparam; return 1; }
__declspec(dllexport) int EnumChildWindows(void* hwnd, void* proc, ULL lparam) { (void)hwnd;(void)proc;(void)lparam; return 1; }
__declspec(dllexport) int EnumThreadWindows(unsigned tid, void* proc, ULL lparam) { (void)tid;(void)proc;(void)lparam; return 1; }
__declspec(dllexport) int GetClassInfoExW(void* inst, const u16_* cls, void* wc) { (void)inst;(void)cls;(void)wc; return 0; }   // classe nao registrada -> caller registra
__declspec(dllexport) int GetClassInfoW(void* inst, const u16_* cls, void* wc) { (void)inst;(void)cls;(void)wc; return 0; }
__declspec(dllexport) int GetClassNameW(void* hwnd, u16_* buf, int cch) { (void)hwnd; if(buf&&cch>0) buf[0]=0; return 0; }
__declspec(dllexport) unsigned RegisterWindowMessageW(const u16_* s) { (void)s; static unsigned m = 0xC000; return m++; }
__declspec(dllexport) int SetWindowTextW(void* hwnd, const u16_* s) { (void)hwnd;(void)s; return 1; }
__declspec(dllexport) int GetWindowTextW(void* hwnd, u16_* buf, int cch) { (void)hwnd; if(buf&&cch>0) buf[0]=0; return 0; }
__declspec(dllexport) unsigned GetWindowThreadProcessId(void* hwnd, unsigned* pid) { (void)hwnd; if(pid)*pid=1; return 1; }
__declspec(dllexport) int GetWindowTextLengthW(void* hwnd) { (void)hwnd; return 0; }
__declspec(dllexport) void* WindowFromPoint(POINT p) { (void)p.x;(void)p.y; return 0; }
__declspec(dllexport) void* ChildWindowFromPoint(void* hwnd, POINT p) { (void)hwnd;(void)p.x;(void)p.y; return 0; }
__declspec(dllexport) int GetGUIThreadInfo(unsigned tid, void* pgui) { (void)tid;(void)pgui; return 0; }
// DeferWindowPos (batch de SetWindowPos): devolve o handle recebido (encadeia sem efeito).
__declspec(dllexport) void* BeginDeferWindowPos(int n) { (void)n; return u32_h(); }
__declspec(dllexport) void* DeferWindowPos(void* h, void* hwnd, void* after, int x,int y,int w,int hh, unsigned fl) { (void)hwnd;(void)after;(void)x;(void)y;(void)w;(void)hh;(void)fl; return h; }
__declspec(dllexport) int EndDeferWindowPos(void* h) { (void)h; return 1; }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
