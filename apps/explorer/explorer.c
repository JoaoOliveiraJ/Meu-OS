// explorer.c  —  Shell ring-3 do MeuOS (inicio do explorer.exe de verdade).
//
// Diferente das apps demo (guiapp/desktop), o explorer.exe NAO sai: ele roda um
// message loop PERSISTENTE. Por isso sua janela permanece VIVA em s_wins (nao e'
// reaped), e o desktop deixa de ser uma "foto congelada" — passa a ter uma janela
// real que sobrevive a recomposicao e responde ao mouse/teclado (clique da foco).
//
// Este e o 1o incremento do explorer.exe ring-3 (o shell que, no Windows, possui a
// taskbar + menu Iniciar via user32/gdi32 — NAO o kernel). Incrementos seguintes:
//   - janela SEM chrome (borderless) ocupando a barra inteira no rodape;
//   - botoes de app (enumerando janelas top-level via uma syscall EnumWindows);
//   - menu Iniciar como janela propria;
//   - remover o shell desenhado pelo kernel (win32kshell.c).
//
// Caminho de cada chamada (igual Windows): ring3 explorer -> user32/gdi32 -> ntdll
// (int 0x80) -> win32k (kernel) -> framebuffer. O WNDPROC roda AQUI, em ring 3.

unsigned long _tls_index = 0;
typedef unsigned long long ULL;

// ---- console (log do que o shell faz, alem dos logs do kernel) ----
#define STD_OUTPUT_HANDLE ((unsigned)-11)
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) void  ExitProcess(unsigned code);

// ---- user32 / gdi32 ----
#define WM_CREATE  0x0001
#define WM_DESTROY 0x0002
#define WM_PAINT   0x000F
#define WM_LBUTTONDOWN 0x0201
#define SW_SHOW    5

typedef struct { int x, y; } POINT;
typedef struct { void* hwnd; unsigned message; ULL wParam; ULL lParam; unsigned time; POINT pt; } MSG;
typedef long long (*WNDPROC)(void*, unsigned, ULL, ULL);
typedef struct {
    unsigned style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra;
    void* hInstance; void* hIcon; void* hCursor; void* hbrBackground;
    const char* lpszMenuName; const char* lpszClassName;
} WNDCLASSA;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { void* hdc; int fErase; int rc[4]; int fRestore; int fIncUpdate; char rgb[32]; } PAINTSTRUCT;

__declspec(dllimport) unsigned short RegisterClassA(const WNDCLASSA*);
__declspec(dllimport) void* CreateWindowExA(unsigned, const char*, const char*, unsigned,
        int, int, int, int, void*, void*, void*, void*);
__declspec(dllimport) int   ShowWindow(void*, int);
__declspec(dllimport) int   GetMessageA(MSG*, void*, unsigned, unsigned);
__declspec(dllimport) int   TranslateMessage(const MSG*);
__declspec(dllimport) long long DispatchMessageA(const MSG*);
__declspec(dllimport) long long DefWindowProcA(void*, unsigned, ULL, ULL);
__declspec(dllimport) void* BeginPaint(void*, PAINTSTRUCT*);
__declspec(dllimport) int   EndPaint(void*, const PAINTSTRUCT*);
__declspec(dllimport) int   TextOutA(void*, int, int, const char*, int);
__declspec(dllimport) int   FillRect(void*, const RECT*, void*);
__declspec(dllimport) void* CreateSolidBrush(unsigned);

// Cores (indices da paleta de 16 do compositor).
#define COL_DKGRAY 8
#define COL_WHITE  15
#define COL_BLUE   1

static unsigned slen(const char* s){ unsigned n=0; while(s[n]) n++; return n; }
static void say(const char* s){ unsigned w=0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0); }

// WNDPROC da "taskbar" do explorer. NAO trata WM_DESTROY com PostQuitMessage:
// o shell e' persistente. Pinta a barra no WM_PAINT; loga o clique.
static long long ExplorerProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    switch (msg) {
    case WM_CREATE:
        say("  [explorer] WM_CREATE (shell ring-3).\n");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        void* hdc = BeginPaint(hwnd, &ps);
        // Maquete da taskbar na area cliente: fundo escuro + Iniciar + relogio.
        void* dk = CreateSolidBrush(COL_DKGRAY);
        RECT bar = { 0, 0, 640, 40 };
        FillRect(hdc, &bar, dk);
        void* bl = CreateSolidBrush(COL_BLUE);
        RECT start = { 6, 8, 78, 32 };
        FillRect(hdc, &start, bl);
        TextOutA(hdc, 14, 14, "Iniciar", -1);
        TextOutA(hdc, 110, 14, "explorer.exe", -1);
        TextOutA(hdc, 560, 14, "00:00", -1);
        TextOutA(hdc, 10, 56, "shell ring-3 PERSISTENTE: janela VIVA,", -1);
        TextOutA(hdc, 10, 72, "sobrevive a recompose; clique chega aqui.", -1);
        EndPaint(hwnd, &ps);
        say("  [explorer] WM_PAINT: maquete da taskbar pintada.\n");
        return 0;
    }
    case WM_LBUTTONDOWN:
        say("  [explorer] WM_LBUTTONDOWN: clique recebido na taskbar.\n");
        return 0;
    case WM_DESTROY:
        // Shell persistente: NAO chamamos DefWindowProc (que postaria WM_QUIT).
        say("  [explorer] WM_DESTROY ignorado (shell persistente).\n");
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void _start(void) {
    say("\n  [explorer] shell ring-3 iniciando (message loop PERSISTENTE)...\n");

    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc   = ExplorerProc;
    wc.lpszClassName = "Shell_TrayWnd";          // mesmo nome de classe do Windows
    RegisterClassA(&wc);

    // Janela do shell (flutuante por ora, p/ nao colidir com a taskbar do kernel
    // — que sai num proximo incremento). Depois: borderless ocupando o rodape.
    void* hwnd = CreateWindowExA(0, "Shell_TrayWnd", "explorer",
                                 0, 180, 150, 640, 150, 0, 0, 0, 0);
    if (!hwnd) { say("  [explorer] CreateWindowEx FALHOU.\n"); ExitProcess(1); }
    ShowWindow(hwnd, SW_SHOW);

    say("  [explorer] no loop de mensagens (nao sai; janela permanece viva).\n");
    MSG msg;
    // Persistente: enquanto houver janela, GetMessage bloqueia em sti;hlt e so
    // retorna com mouse/teclado reais. Nunca postamos WM_QUIT -> nao sai.
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    // So chega aqui se o kernel mandar WM_QUIT (ex.: ultima janela fechada).
    say("  [explorer] saiu do loop (WM_QUIT). Encerrando shell.\n");
    ExitProcess(0);
}
