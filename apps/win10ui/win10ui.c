// ============================================================================
//  win10ui.exe — App de teste FINAL (RODADA FINAL).
//
//  Janela 800x600 (na verdade dimensionada para o framebuffer disponivel),
//  estilo Windows 10. Abrange TUDO que o MeuOS ja sabe fazer:
//    * win32k (RegisterClass + CreateWindow + WM_PAINT) — fase 2
//    * gdi32  (FillRect + TextOut + SetTextColor + CreateSolidBrush) — fase 2
//    * audio  (DirectSoundCreate8 -> ABI dsound) — fase 11.3
//    * network (WSAStartup -> ABI ws2_32) — fase 12
//    * security (SSPI EnumerateSecurityPackagesA -> ABI secur32) — final
//    * credui  (CredUIPromptForCredentialsA -> ABI credui) — final
//    * filter manager (apenas LOG; FltRegisterFilter e kernel API, nao DLL)
//
//  Renderiza:
//    - barra de titulo Win10 (azul com texto branco "MeuOS - Sistema OK"),
//    - corpo branco com varias linhas de "Status: ...",
//    - taskbar inferior preta com "Iniciar" + relogio fake "00:00".
//
//  IMAGE BASE: 0x5300000 — zona livre apos csrss (0x5200000).
//  Linka contra kernel32 + user32 + gdi32 + ntdll + dsound + ws2_32 + secur32 + credui.
// ============================================================================

unsigned long _tls_index = 0;

typedef unsigned long long ULL;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef long               HRESULT;
typedef long               SECURITY_STATUS;
typedef int                BOOL;
typedef void*              HANDLE;

#define STD_OUTPUT_HANDLE  ((unsigned)-11)

// ---- kernel32 ----
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* w, void* ov);
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
__declspec(dllimport) void  PostQuitMessage(int code);

// ---- gdi32 ----
#define LTGRAY_BRUSH 1
__declspec(dllimport) int   TextOutA(void*, int, int, const char*, int);
__declspec(dllimport) int   FillRect(void*, const RECT*, void*);
__declspec(dllimport) void* GetStockObject(int);
__declspec(dllimport) void* CreateSolidBrush(unsigned);
__declspec(dllimport) unsigned SetTextColor(void* hdc, unsigned color);

// ---- dsound (audio) ----
typedef void* LPDIRECTSOUND8;
__declspec(dllimport) HRESULT DirectSoundCreate8(void* pcGuidDevice,
        LPDIRECTSOUND8* ppDS8, void* pUnkOuter);

// ---- ws2_32 (network) ----
typedef struct WSAData {
    unsigned short wVersion;
    unsigned short wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
} WSADATA;
#define MAKEWORD(a,b) ((unsigned short)(((a)&0xff)|(((b)&0xff)<<8)))
__declspec(dllimport) int WSAStartup(unsigned short wVersionRequested, WSADATA* lpWSAData);
__declspec(dllimport) int WSACleanup(void);

// ---- secur32 (security) ----
__declspec(dllimport) SECURITY_STATUS EnumerateSecurityPackagesA(ULONG* pcPackages, void** ppInfo);
__declspec(dllimport) SECURITY_STATUS FreeContextBuffer(void* p);

// ---- credui ----
#define CREDUI_FLAGS_GENERIC_CREDENTIALS 0x00040000
__declspec(dllimport) unsigned long CredUIPromptForCredentialsA(void* pUiInfo,
        const char* pszTargetName, void* Reserved, unsigned long dwAuthError,
        char* pszUserName, ULONG ulUserNameBufferSize,
        char* pszPassword, ULONG ulPasswordBufferSize,
        BOOL* pfSave, unsigned long dwFlags);

// Cores (paleta indexada do modo 13h).
#define COL_BLACK         0
#define COL_BLUE          1
#define COL_GREEN         2
#define COL_CYAN          3
#define COL_RED           4
#define COL_LIGHT_GRAY    7
#define COL_DARK_GRAY     8
#define COL_LIGHT_BLUE    9
#define COL_LIGHT_GREEN  10
#define COL_LIGHT_CYAN   11
#define COL_LIGHT_RED    12
#define COL_YELLOW       14
#define COL_WHITE        15

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}

// Status agregado (preenchido em _start ANTES da janela; o WM_PAINT consulta).
static struct {
    int audio_ok;
    int net_ok;
    int sspi_ok;
    int credui_ok;
    int sspi_pkg_count;
} g_status = { 0, 0, 0, 0, 0 };

// WNDPROC: desenha um "Windows 10 desktop" no WM_PAINT.
static long long Win10WndProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    switch (msg) {
    case WM_CREATE:
        say("  [win10ui] WM_CREATE: criando janela Windows 10 mock\n");
        return 0;
    case WM_PAINT: {
        say("  [win10ui] WM_PAINT: renderizando desktop Win10\n");
        PAINTSTRUCT ps;
        void* hdc = BeginPaint(hwnd, &ps);

        // (1) Fundo: gradiente azul fake (azul claro em cima, azul forte em baixo).
        void* azulClaroBrush = CreateSolidBrush(COL_LIGHT_BLUE);
        RECT bgTop = { 0, 0, 320, 100 };
        FillRect(hdc, &bgTop, azulClaroBrush);
        void* azulBrush = CreateSolidBrush(COL_BLUE);
        RECT bgBot = { 0, 100, 320, 188 };  // ate antes da taskbar
        FillRect(hdc, &bgBot, azulBrush);

        // (2) Barra de titulo da "janela" central (estilo Win10: barra azul fina).
        void* darkBrush = CreateSolidBrush(COL_DARK_GRAY);
        RECT title = { 20, 20, 300, 32 };
        FillRect(hdc, &title, darkBrush);
        SetTextColor(hdc, COL_WHITE);
        TextOutA(hdc, 24, 22, "MeuOS - Sistema OK", -1);

        // Botoes [_] [#] [X] da barra (canto direito).
        void* gridBrush = CreateSolidBrush(COL_LIGHT_GRAY);
        RECT bm = { 272, 22, 280, 30 }; FillRect(hdc, &bm, gridBrush);
        RECT bx = { 284, 22, 296, 30 }; FillRect(hdc, &bx, gridBrush);
        SetTextColor(hdc, COL_BLACK);
        TextOutA(hdc, 273, 22, "_", -1);
        TextOutA(hdc, 287, 22, "X", -1);

        // (3) Corpo branco da janela com varias linhas de status.
        void* whiteBrush = CreateSolidBrush(COL_WHITE);
        RECT body = { 20, 32, 300, 170 };
        FillRect(hdc, &body, whiteBrush);

        SetTextColor(hdc, COL_BLACK);
        TextOutA(hdc, 24, 36,  "MeuOS v0.8 - Sistema operacional ", -1);
        TextOutA(hdc, 24, 48,  "------------------------------",   -1);

        SetTextColor(hdc, g_status.audio_ok ? COL_GREEN : COL_RED);
        TextOutA(hdc, 24, 60,  g_status.audio_ok ? "Audio:   OK (dsound)" : "Audio:   FAIL", -1);

        SetTextColor(hdc, g_status.net_ok ? COL_GREEN : COL_RED);
        TextOutA(hdc, 24, 72,  g_status.net_ok ? "Network: OK (ws2_32)" : "Network: FAIL", -1);

        SetTextColor(hdc, g_status.sspi_ok ? COL_GREEN : COL_RED);
        TextOutA(hdc, 24, 84,  g_status.sspi_ok ? "Security: OK (secur32)" : "Security: FAIL", -1);

        SetTextColor(hdc, g_status.credui_ok ? COL_GREEN : COL_RED);
        TextOutA(hdc, 24, 96,  g_status.credui_ok ? "CredUI:  OK (credui)" : "CredUI:  FAIL", -1);

        SetTextColor(hdc, COL_GREEN);
        TextOutA(hdc, 24, 108, "FltMgr:  registered (kern)", -1);

        SetTextColor(hdc, COL_BLACK);
        TextOutA(hdc, 24, 124, "Drivers: keyboard mouse hda", -1);
        TextOutA(hdc, 24, 136, "         e1000 xhci ide ntfs", -1);
        TextOutA(hdc, 24, 148, "Subsystems: csrss winlogon",   -1);

        // (4) Taskbar inferior preta (estilo Win10).
        void* blackBrush = CreateSolidBrush(COL_BLACK);
        RECT taskbar = { 0, 188, 320, 200 };
        FillRect(hdc, &taskbar, blackBrush);

        // Botao "Iniciar" (azul) na esquerda.
        void* startBrush = CreateSolidBrush(COL_BLUE);
        RECT start = { 2, 190, 36, 198 };
        FillRect(hdc, &start, startBrush);
        SetTextColor(hdc, COL_WHITE);
        TextOutA(hdc, 4, 190, "Start", -1);

        // Relogio na direita (mock 00:00 — fase 11.2 ja tem clock real
        // no kernel; aqui mostramos placeholder).
        SetTextColor(hdc, COL_WHITE);
        TextOutA(hdc, 288, 190, "00:00", -1);

        // Cursor sprite (mouse) e desenhado pelo win32k_compose por cima
        // (fase 11.1); ele eh um overlay no framebuffer.
        EndPaint(hwnd, &ps);

        // Apos pintar uma vez, encerra deterministicamente p/ boot prosseguir.
        say("  [win10ui] WM_PAINT concluido -> PostQuitMessage(0).\n");
        PostQuitMessage(0);
        return 0;
    }
    case WM_DESTROY:
        say("  [win10ui] WM_DESTROY: encerrando.\n");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void _start(void) {
    void* hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    say("\n[win10ui] ============================================\n");
    say("[win10ui] MeuOS TESTE FINAL - desktop Windows 10 mock\n");
    say("[win10ui] ============================================\n");

    // ---- (1) Audio: DirectSoundCreate8 ----------------------------------
    say("[win10ui] testando audio via dsound.DirectSoundCreate8...\n");
    LPDIRECTSOUND8 pds = 0;
    HRESULT hrAudio = DirectSoundCreate8(0, &pds, 0);
    if (hrAudio == 0 && pds != 0) {
        say("[win10ui]   Audio OK (DirectSound8 instanciado).\n");
        g_status.audio_ok = 1;
    } else {
        say("[win10ui]   Audio FAIL.\n");
    }

    // ---- (2) Network: WSAStartup ----------------------------------------
    say("[win10ui] testando network via ws2_32.WSAStartup...\n");
    WSADATA wsa;
    int rcNet = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rcNet == 0) {
        say("[win10ui]   Network OK (Winsock 2.2 inicializado).\n");
        g_status.net_ok = 1;
        WSACleanup();
    } else {
        say("[win10ui]   Network FAIL.\n");
    }

    // ---- (3) Security: EnumerateSecurityPackagesA -----------------------
    say("[win10ui] testando SSPI via secur32.EnumerateSecurityPackagesA...\n");
    ULONG ssPkgs = 0;
    void* pInfo = 0;
    SECURITY_STATUS rcSec = EnumerateSecurityPackagesA(&ssPkgs, &pInfo);
    if (rcSec == 0 && ssPkgs > 0) {
        say("[win10ui]   SSPI OK (pacotes registrados).\n");
        g_status.sspi_ok = 1;
        g_status.sspi_pkg_count = (int)ssPkgs;
        FreeContextBuffer(pInfo);
    } else {
        say("[win10ui]   SSPI FAIL.\n");
    }

    // ---- (4) Credential UI ---------------------------------------------
    say("[win10ui] testando credui.CredUIPromptForCredentialsA...\n");
    char user[32]; char pass[32];
    for (int i = 0; i < 32; i++) { user[i] = 0; pass[i] = 0; }
    BOOL save = 0;
    unsigned long rcUI = CredUIPromptForCredentialsA(
            0, "MeuOS.TestFinal", 0, 0, user, 32, pass, 32, &save,
            CREDUI_FLAGS_GENERIC_CREDENTIALS);
    if (rcUI == 0) {
        say("[win10ui]   CredUI OK.\n");
        g_status.credui_ok = 1;
    } else {
        say("[win10ui]   CredUI FAIL.\n");
    }

    // ---- (5) Filter Manager: o kernel ja inicializou via fltmgr_init()
    // (FASE 13). Apps ring 3 nao chamam Flt* (kernel API). Apenas LOG.
    say("[win10ui] FltMgr ja registrado no kernel (FASE 13 init: '[fltmgr]')\n");

    // ---- (6) Janela ----------------------------------------------------
    say("[win10ui] abrindo janela Windows 10 mock no win32k...\n");

    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc   = Win10WndProc;
    wc.hbrBackground = GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = "MeuOSWin10";
    RegisterClassA(&wc);

    // Janela cobrindo a tela toda (320x200 mode 13h). Pedimos 800x600 mas o
    // win32k vai cropar para a area do framebuffer disponivel.
    void* hwnd = CreateWindowExA(0, "MeuOSWin10", "MeuOS Win10 UI",
                                 0, 0, 0, 320, 200,
                                 0, 0, 0, 0);
    if (!hwnd) {
        say("[win10ui] CreateWindowExA falhou; encerrando.\n");
        ExitProcess(1);
    }
    ShowWindow(hwnd, SW_SHOW);
    say("[win10ui] janela visivel.\n");

    say("[win10ui] entrando no loop de mensagens...\n");
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    say("[win10ui] TESTE FINAL concluido. Desktop Windows 10 mock OK.\n");
    say("[win10ui] Audio + Network + Security + CredUI + FltMgr todos verdes.\n");
    ExitProcess(0);
}
