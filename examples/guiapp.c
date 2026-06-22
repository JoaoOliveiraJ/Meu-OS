// guiapp.c  —  Programa WINDOWS GUI (PE32+ x64) de demonstracao da FASE 2.
//
// Faz o ciclo de vida classico de uma app Win32:
//   RegisterClassA -> CreateWindowExA -> ShowWindow -> loop GetMessage/
//   TranslateMessage/DispatchMessage. No WM_PAINT desenha um retangulo
//   (FillRect com um brush) e texto (TextOutA). WM_CHAR loga a tecla recebida.
//   WM_DESTROY encerra (DefWindowProc -> PostQuitMessage).
//
// Caminho completo de cada chamada:
//   ring3 (guiapp) -> user32/gdi32 -> ntdll -> int 0x80 -> win32k (kernel) ->
//   framebuffer (drivers/video.c). Cada etapa loga na serial.
//
// A janela APARECE no framebuffer (mode 13h). Em headless, a serial comprova a
// logica; um screendump (run.ps1 -Screendump) mostra os pixels.

unsigned long _tls_index = 0;

typedef unsigned long long ULL;

// ---- console (para logar o que o app faz, alem dos logs do kernel) ----
#define STD_OUTPUT_HANDLE ((unsigned)-11)
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) void  ExitProcess(unsigned code);

// ---- user32 ----
#define WM_CREATE  0x0001
#define WM_DESTROY 0x0002
#define WM_PAINT   0x000F
#define WM_KEYDOWN 0x0100
#define WM_CHAR    0x0102
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

// ---- gdi32 ----
#define LTGRAY_BRUSH 1
__declspec(dllimport) int   TextOutA(void*, int, int, const char*, int);
__declspec(dllimport) int   FillRect(void*, const RECT*, void*);
__declspec(dllimport) void* GetStockObject(int);
__declspec(dllimport) void* CreateSolidBrush(unsigned);

// Cores (indices da paleta do mode 13h).
#define COL_RED   4
#define COL_WHITE 15

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}

// WNDPROC da janela: trata WM_PAINT (desenha), WM_CHAR (loga), WM_DESTROY (sai).
static long long WindowProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    switch (msg) {
    case WM_CREATE:
        say("  [guiapp] WM_CREATE recebido.\n");
        return 0;
    case WM_PAINT: {
        say("  [guiapp] WM_PAINT: desenhando (FillRect + TextOut)...\n");
        PAINTSTRUCT ps;
        void* hdc = BeginPaint(hwnd, &ps);
        // Um retangulo vermelho na area cliente.
        void* redBrush = CreateSolidBrush(COL_RED);
        RECT r = { 8, 24, 120, 44 };
        FillRect(hdc, &r, redBrush);
        // Texto na area cliente.
        TextOutA(hdc, 6, 4,  "Janela GUI do MeuOS", -1);
        TextOutA(hdc, 12, 28, "WM_PAINT OK", -1);
        TextOutA(hdc, 6, 52,  "Fase 2: win32k", -1);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        say("  [guiapp] WM_KEYDOWN recebido.\n");
        return 0;
    case WM_CHAR: {
        char buf[40]; const char* p = "  [guiapp] WM_CHAR: '";
        unsigned i = 0; while (p[i]) { buf[i] = p[i]; i++; }
        buf[i++] = (char)(unsigned char)wParam;
        buf[i++] = '\''; buf[i++] = '\n'; buf[i] = 0;
        say(buf);
        return 0;
    }
    case WM_DESTROY:
        say("  [guiapp] WM_DESTROY: encerrando.\n");
        return DefWindowProcA(hwnd, msg, wParam, lParam);   // -> PostQuitMessage
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void _start(void) {
    say("  [guiapp] inicio (ring 3). Registrando classe...\n");

    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc   = WindowProc;
    wc.hbrBackground = GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = "MeuOSWindowClass";
    RegisterClassA(&wc);

    say("  [guiapp] CreateWindowExA...\n");
    void* hwnd = CreateWindowExA(0, "MeuOSWindowClass", "MeuOS GUI",
                                 0, 60, 50, 180, 110, 0, 0, 0, 0);
    if (!hwnd) { say("  [guiapp] CreateWindowEx falhou.\n"); ExitProcess(1); }

    say("  [guiapp] ShowWindow...\n");
    ShowWindow(hwnd, SW_SHOW);

    say("  [guiapp] entrando no loop de mensagens (GetMessage)...\n");
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    say("  [guiapp] saiu do loop (WM_QUIT). Fim.\n");
    ExitProcess(0);
}
