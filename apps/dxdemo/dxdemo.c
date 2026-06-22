// dxdemo\dxdemo.c  —  Aplicativo de demonstracao da FASE 9.4 (Direct3D 7 stack completo).
//
// Exercita TODO o stack GPU ring 3 do MeuOS:
//
//   1) GDI 32 (kernel32 + user32 + gdi32): RegisterClassA + CreateWindowExA +
//      loop GetMessage/DispatchMessage. WM_PAINT desenha com FillRect (2 cores)
//      + TextOut. Mesmo caminho do guiapp.exe (Fase 2/6).
//
//   2) DirectDraw 7 (ddraw.dll, Fase 9.3): DirectDrawCreate7 -> IDirectDraw7.
//      Chama SetCooperativeLevel + SetDisplayMode + CreateSurface (primary).
//      O ddraw stub devolve DD_OK em quase tudo; o win32k continua dono do FB.
//
//   3) Direct3D 7 (d3d.dll, Fase 9.4): Direct3DCreate7 -> IDirect3D7. Chama
//      CreateDevice -> IDirect3DDevice7. Exercita BeginScene/Clear/SetViewport/
//      SetRenderState/DrawPrimitive/EndScene/Present (no caminho stub, todos
//      devolvem D3D_OK; nao renderizam triangulos reais — o win32k ainda e
//      quem desenha pixels via GDI).
//
// Caminho completo de cada chamada:
//   ring3 (gpuapp) -> user32/gdi32/ddraw/d3d -> ntdll -> int 0x80 -> win32k
//   (kernel) -> framebuffer. Cada etapa loga na serial; em headless, os logs
//   provam a logica; um screendump (run.ps1 -Screendump) mostra os pixels.
//
// Ao terminar o WM_PAINT, chama PostQuitMessage(0) para o GetMessage devolver
// WM_QUIT — assim o app encerra de forma DETERMINISTICA mesmo sem o auto-inject
// do win32k (que vai para a PRIMEIRA janela simples, ja consumida pelo guiapp).
//
// ImageBase 0x400000 (4 MiB): faixa livre entre o kernel (0x100000-0x1D5000) e
// o USTACK (0x600000-0x700000). NAO usa 0x4200000 (a zona do PMM_BASE em
// src/ntos/mm/pmm.c colide com frames gerenciados — qualquer base >= 0x4000000
// corrompe o estado do PMM). Slot pd[2] do PD: livre, sem nenhum modulo.

unsigned long _tls_index = 0;

typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               HRESULT;
typedef unsigned long long ULL;
typedef void*              HWND;
typedef void*              HDC;
typedef void*              REFIID;
typedef void*              REFCLSID;
typedef void*              IUnknown;
typedef float              D3DVALUE;

// ---- console (logs do app, alem dos logs do kernel) ----
#define STD_OUTPUT_HANDLE ((unsigned)-11)
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) void  ExitProcess(unsigned code);

// ---- user32: janela + loop de mensagens ----
#define WM_CREATE    0x0001
#define WM_DESTROY   0x0002
#define WM_PAINT     0x000F
#define WM_KEYDOWN   0x0100
#define WM_CHAR      0x0102
#define SW_SHOW      5

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
__declspec(dllimport) void  PostQuitMessage(int);
__declspec(dllimport) void* GetDC(void*);

// ---- gdi32 ----
#define LTGRAY_BRUSH 1
__declspec(dllimport) int   TextOutA(void*, int, int, const char*, int);
__declspec(dllimport) int   FillRect(void*, const RECT*, void*);
__declspec(dllimport) void* GetStockObject(int);
__declspec(dllimport) void* CreateSolidBrush(unsigned);

// Cores (indices da paleta mode 13h; o win32k traduz para a paleta GPU LFB).
#define COL_RED      4
#define COL_YELLOW   14
#define COL_WHITE    15

// ---- ddraw 7 (FASE 9.3) ----
//
// IDirectDraw7 / IDirectDrawSurface7 expostos como ponteiro opaco; chamamos os
// metodos via Vtbl COM-ABI (ABI Microsoft: 1o arg = this).
#define DD_OK                  0x00000000L
#define DDSCL_NORMAL           0x00000008
#define DDSCL_FULLSCREEN       0x00000001
#define DDSCL_EXCLUSIVE        0x00000010
#define DDSD_CAPS              0x00000001
#define DDSD_HEIGHT            0x00000002
#define DDSD_WIDTH             0x00000004
#define DDSCAPS_PRIMARYSURFACE 0x00000200

#pragma pack(push, 1)
typedef struct _DDPIXELFORMAT {
    DWORD dwSize, dwFlags, dwFourCC, dwRGBBitCount;
    DWORD dwRBitMask, dwGBitMask, dwBBitMask, dwRGBAlphaBitMask;
} DDPIXELFORMAT;
typedef struct _DDSCAPS2 { DWORD dwCaps, dwCaps2, dwCaps3, dwCaps4; } DDSCAPS2;
typedef struct _DDSURFACEDESC2 {
    DWORD dwSize, dwFlags, dwHeight, dwWidth;
    long  lPitch;
    DWORD dwBackBufferCount, dwMipMapCount, dwAlphaBitDepth, dwReserved;
    void* lpSurface;
    DWORD dwEmptyCk[2], dwEmptyCk2[2], dwEmptyCk3[2], dwEmptyCk4[2];
    DDPIXELFORMAT ddpfPixelFormat;
    DDSCAPS2      ddsCaps;
    DWORD         dwTextureStage;
} DDSURFACEDESC2;
#pragma pack(pop)

// Forward decls dos objetos.
typedef struct IDirectDraw7Impl IDirectDraw7;
typedef struct IDirectDrawSurface7Impl IDirectDrawSurface7;

// Vtable do IDirectDrawSurface7 (ordem ESPELHA dll/win32/ddraw/ddraw.c).
typedef struct IDirectDrawSurface7Vtbl {
    HRESULT (*QueryInterface)(IDirectDrawSurface7*, REFIID, void**);
    DWORD   (*AddRef)        (IDirectDrawSurface7*);
    DWORD   (*Release)       (IDirectDrawSurface7*);
    HRESULT (*Blt)           (IDirectDrawSurface7*, void*, IDirectDrawSurface7*,
                              void*, DWORD, void*);
    HRESULT (*Flip)          (IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);
    HRESULT (*GetSurfaceDesc)(IDirectDrawSurface7*, DDSURFACEDESC2*);
    HRESULT (*Lock)          (IDirectDrawSurface7*, void*, DDSURFACEDESC2*, DWORD, void*);
    HRESULT (*Unlock)        (IDirectDrawSurface7*, void*);
    HRESULT (*GetDC)         (IDirectDrawSurface7*, HDC*);
    HRESULT (*ReleaseDC)     (IDirectDrawSurface7*, HDC);
    HRESULT (*Restore)       (IDirectDrawSurface7*);
    HRESULT (*IsLost)        (IDirectDrawSurface7*);
    HRESULT (*SetClipper)    (IDirectDrawSurface7*, void*);
    HRESULT (*SetPalette)    (IDirectDrawSurface7*, void*);
} IDirectDrawSurface7Vtbl;
struct IDirectDrawSurface7Impl { const IDirectDrawSurface7Vtbl* lpVtbl; };

// Vtable do IDirectDraw7 (ordem ESPELHA dll/win32/ddraw/ddraw.c).
typedef struct IDirectDraw7Vtbl {
    HRESULT (*QueryInterface)     (IDirectDraw7*, REFIID, void**);
    DWORD   (*AddRef)             (IDirectDraw7*);
    DWORD   (*Release)            (IDirectDraw7*);
    HRESULT (*Compact)            (IDirectDraw7*);
    HRESULT (*CreateClipper)      (IDirectDraw7*, DWORD, void**, IUnknown*);
    HRESULT (*CreatePalette)      (IDirectDraw7*, DWORD, void*, void**, IUnknown*);
    HRESULT (*CreateSurface)      (IDirectDraw7*, DDSURFACEDESC2*, IDirectDrawSurface7**, IUnknown*);
    HRESULT (*SetCooperativeLevel)(IDirectDraw7*, HWND, DWORD);
    HRESULT (*SetDisplayMode)     (IDirectDraw7*, DWORD, DWORD, DWORD, DWORD, DWORD);
    HRESULT (*RestoreDisplayMode) (IDirectDraw7*);
    HRESULT (*WaitForVerticalBlank)(IDirectDraw7*, DWORD, void*);
    HRESULT (*GetCaps)            (IDirectDraw7*, void*, void*);
    HRESULT (*FlipToGDISurface)   (IDirectDraw7*);
    HRESULT (*GetDisplayMode)     (IDirectDraw7*, DDSURFACEDESC2*);
} IDirectDraw7Vtbl;
struct IDirectDraw7Impl { const IDirectDraw7Vtbl* lpVtbl; };

__declspec(dllimport) HRESULT DirectDrawCreateEx(void* guid, void** dd, REFIID refiid, IUnknown* outer);

// ---- d3d 7 (FASE 9.4) ----
#define D3D_OK        0x00000000L
#define D3D_SDK_VERSION 7

#pragma pack(push, 1)
typedef struct _D3DMATRIX {
    D3DVALUE _11, _12, _13, _14;
    D3DVALUE _21, _22, _23, _24;
    D3DVALUE _31, _32, _33, _34;
    D3DVALUE _41, _42, _43, _44;
} D3DMATRIX;
typedef struct _D3DVIEWPORT7 {
    DWORD    dwX, dwY, dwWidth, dwHeight;
    D3DVALUE dvMinZ, dvMaxZ;
} D3DVIEWPORT7;
typedef struct _D3DRECT { long x1, y1, x2, y2; } D3DRECT;
typedef struct _D3DDEVICEDESC7 {
    DWORD dwDevCaps;
    DWORD dwMinTextureWidth, dwMinTextureHeight;
    DWORD dwMaxTextureWidth, dwMaxTextureHeight;
    DWORD dwMaxActiveLights;
    DWORD deviceGUID[4];
    char  reserved[64];
} D3DDEVICEDESC7;
#pragma pack(pop)

// Estados do D3D (subset minimo p/ exercitar SetRenderState).
#define D3DRS_LIGHTING       137
#define D3DRS_ZENABLE        7
#define D3DRS_CULLMODE       22
#define D3DTS_WORLD          1
#define D3DTS_VIEW           2
#define D3DTS_PROJECTION     3

// Forward decls dos objetos.
typedef struct IDirect3D7Impl       IDirect3D7;
typedef struct IDirect3DDevice7Impl IDirect3DDevice7;

// Vtable do IDirect3DDevice7 (ordem ESPELHA dll/win32/d3d9/d3d9.c).
typedef struct IDirect3DDevice7Vtbl {
    HRESULT (*QueryInterface)(IDirect3DDevice7*, REFIID, void**);
    DWORD   (*AddRef)        (IDirect3DDevice7*);
    DWORD   (*Release)       (IDirect3DDevice7*);
    HRESULT (*GetCaps)       (IDirect3DDevice7*, D3DDEVICEDESC7*);
    HRESULT (*EnumTextureFormats)(IDirect3DDevice7*, void*, void*);
    HRESULT (*BeginScene)    (IDirect3DDevice7*);
    HRESULT (*EndScene)      (IDirect3DDevice7*);
    HRESULT (*GetDirect3D)   (IDirect3DDevice7*, IDirect3D7**);
    HRESULT (*SetRenderTarget)(IDirect3DDevice7*, IDirectDrawSurface7*, DWORD);
    HRESULT (*GetRenderTarget)(IDirect3DDevice7*, IDirectDrawSurface7**);
    HRESULT (*Clear)         (IDirect3DDevice7*, DWORD, D3DRECT*, DWORD, DWORD, D3DVALUE, DWORD);
    HRESULT (*SetTransform)  (IDirect3DDevice7*, DWORD, D3DMATRIX*);
    HRESULT (*GetTransform)  (IDirect3DDevice7*, DWORD, D3DMATRIX*);
    HRESULT (*SetViewport)   (IDirect3DDevice7*, D3DVIEWPORT7*);
    HRESULT (*GetViewport)   (IDirect3DDevice7*, D3DVIEWPORT7*);
    HRESULT (*SetMaterial)   (IDirect3DDevice7*, void*);
    HRESULT (*SetLight)      (IDirect3DDevice7*, DWORD, void*);
    HRESULT (*LightEnable)   (IDirect3DDevice7*, DWORD, BOOL);
    HRESULT (*SetRenderState)(IDirect3DDevice7*, DWORD, DWORD);
    HRESULT (*GetRenderState)(IDirect3DDevice7*, DWORD, DWORD*);
    HRESULT (*BeginStateBlock)(IDirect3DDevice7*);
    HRESULT (*EndStateBlock) (IDirect3DDevice7*, DWORD*);
    HRESULT (*PreLoad)       (IDirect3DDevice7*, void*);
    HRESULT (*DrawPrimitive) (IDirect3DDevice7*, DWORD, DWORD, void*, DWORD, DWORD);
    HRESULT (*DrawIndexedPrimitive)(IDirect3DDevice7*, DWORD, DWORD, void*, DWORD,
                                    unsigned short*, DWORD, DWORD);
    HRESULT (*SetClipStatus) (IDirect3DDevice7*, void*);
    HRESULT (*GetClipStatus) (IDirect3DDevice7*, void*);
    HRESULT (*DrawPrimitiveStrided)(IDirect3DDevice7*, DWORD, DWORD, void*, DWORD, DWORD);
    HRESULT (*ComputeSphereVisibility)(IDirect3DDevice7*, void*, void*, DWORD, DWORD, DWORD*);
    HRESULT (*GetTexture)    (IDirect3DDevice7*, DWORD, void**);
    HRESULT (*SetTexture)    (IDirect3DDevice7*, DWORD, void*);
    HRESULT (*GetTextureStageState)(IDirect3DDevice7*, DWORD, DWORD, DWORD*);
    HRESULT (*SetTextureStageState)(IDirect3DDevice7*, DWORD, DWORD, DWORD);
    HRESULT (*ValidateDevice)(IDirect3DDevice7*, DWORD*);
    HRESULT (*ApplyStateBlock)(IDirect3DDevice7*, DWORD);
    HRESULT (*CaptureStateBlock)(IDirect3DDevice7*, DWORD);
    HRESULT (*DeleteStateBlock)(IDirect3DDevice7*, DWORD);
    HRESULT (*CreateStateBlock)(IDirect3DDevice7*, DWORD, DWORD*);
    HRESULT (*Load)          (IDirect3DDevice7*, void*, void*, void*, void*, DWORD);
    HRESULT (*GetInfo)       (IDirect3DDevice7*, DWORD, void*, DWORD);
} IDirect3DDevice7Vtbl;
struct IDirect3DDevice7Impl { const IDirect3DDevice7Vtbl* lpVtbl; };

// Vtable do IDirect3D7 (ordem ESPELHA dll/win32/d3d9/d3d9.c).
typedef struct IDirect3D7Vtbl {
    HRESULT (*QueryInterface)(IDirect3D7*, REFIID, void**);
    DWORD   (*AddRef)        (IDirect3D7*);
    DWORD   (*Release)       (IDirect3D7*);
    HRESULT (*EnumDevices)   (IDirect3D7*, void* cb, void* ctx);
    HRESULT (*CreateDevice)  (IDirect3D7*, REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**);
    HRESULT (*CreateVertexBuffer)(IDirect3D7*, void*, void**, DWORD);
    HRESULT (*EnumZBufferFormats)(IDirect3D7*, REFCLSID, void*, void*);
    HRESULT (*EvictManagedTextures)(IDirect3D7*);
} IDirect3D7Vtbl;
struct IDirect3D7Impl { const IDirect3D7Vtbl* lpVtbl; };

__declspec(dllimport) HRESULT Direct3DCreate7(DWORD sdkVer, IDirect3D7** d3d);

// ============================================================================
//  Helpers de log.
// ============================================================================
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}
static void say_hex(const char* prefix, ULL v) {
    static const char hex[] = "0123456789ABCDEF";
    // 128 bytes cobrem com folga: prefix (~64) + "0x" + 16 digitos + "\n\0".
    // (Buf de 64 bytes estourava com prefixos longos -> vector 6 / UD2 no
    // smashing da stack canary, antes do retorno.)
    char buf[160]; unsigned i = 0;
    while (prefix[i] && i < 128) { buf[i] = prefix[i]; i++; }
    buf[i++] = '0'; buf[i++] = 'x';
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) {
        unsigned d = (unsigned)((v >> s) & 0xF);
        if (d || started || s == 0) { buf[i++] = hex[d]; started = 1; }
    }
    buf[i++] = '\n'; buf[i] = 0;
    say(buf);
}

// ============================================================================
//  Estado global. Criamos os objetos D3D apos a janela ficar visivel (no inicio
//  do _start, antes do loop). Eles vivem em pools estaticos das DLLs ddraw/d3d.
// ============================================================================
static IDirectDraw7*        g_dd      = 0;
static IDirectDrawSurface7* g_primary = 0;
static IDirect3D7*          g_d3d     = 0;
static IDirect3DDevice7*    g_device  = 0;
static int                  g_painted = 0;   // s/ a 1a pintura, posta WM_QUIT

// ============================================================================
//  WindowProc — trata WM_PAINT (desenha + exercita D3D) e WM_DESTROY (sai).
// ============================================================================
static long long WindowProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    switch (msg) {
    case WM_CREATE:
        say("  [gpuapp] WM_CREATE recebido.\n");
        return 0;

    case WM_PAINT: {
        say("  [gpuapp] WM_PAINT: desenhando via GDI (FillRect + TextOut)...\n");
        PAINTSTRUCT ps;
        void* hdc = BeginPaint(hwnd, &ps);

        // Retangulo vermelho na area cliente.
        void* redBrush = CreateSolidBrush(COL_RED);
        RECT r1 = { 8, 24, 100, 44 };
        FillRect(hdc, &r1, redBrush);

        // Retangulo amarelo do lado.
        void* yelBrush = CreateSolidBrush(COL_YELLOW);
        RECT r2 = { 108, 24, 200, 44 };
        FillRect(hdc, &r2, yelBrush);

        TextOutA(hdc, 6, 4,  "MeuOS GPU: GDI + DirectDraw + D3D", -1);
        TextOutA(hdc, 12, 52, "FASE 9.4 ok", -1);
        EndPaint(hwnd, &ps);

        // Exercita o ciclo D3D no WM_PAINT (apos a janela estar visivel).
        if (g_device) {
            say("  [gpuapp] D3D: BeginScene\n");
            g_device->lpVtbl->BeginScene(g_device);

            // Clear: nao pinta (stub), mas exercita o caminho COM-ABI.
            say("  [gpuapp] D3D: Clear (azul fundo, stub)\n");
            g_device->lpVtbl->Clear(g_device, 0, 0, 0, 0xFF0033AA, 1.0f, 0);

            // SetViewport simulando uma janela 800x600.
            D3DVIEWPORT7 vp = { 0, 0, 800, 600, 0.0f, 1.0f };
            g_device->lpVtbl->SetViewport(g_device, &vp);

            // SetRenderState: desliga lighting/zbuffer (stub aceita tudo).
            g_device->lpVtbl->SetRenderState(g_device, D3DRS_LIGHTING, 0);
            g_device->lpVtbl->SetRenderState(g_device, D3DRS_ZENABLE,  0);
            g_device->lpVtbl->SetRenderState(g_device, D3DRS_CULLMODE, 1);

            // DrawPrimitive: stub no-op, mas comprova a vtable.
            say("  [gpuapp] D3D: DrawPrimitive (triangulo fake)\n");
            g_device->lpVtbl->DrawPrimitive(g_device, 4 /*TRIANGLELIST*/,
                                            0x44 /*FVF*/, (void*)0x1, 3, 0);

            say("  [gpuapp] D3D: EndScene\n");
            g_device->lpVtbl->EndScene(g_device);
        }

        say("  [gpuapp] WM_PAINT rendered\n");

        // Encerra de forma deterministica logo apos a 1a pintura (o auto-inject
        // do win32k foi para o guiapp; aqui pedimos WM_QUIT manualmente).
        if (!g_painted) {
            g_painted = 1;
            say("  [gpuapp] PostQuitMessage(0): encerrando demo.\n");
            PostQuitMessage(0);
        }
        return 0;
    }

    case WM_KEYDOWN:
        say("  [gpuapp] WM_KEYDOWN -> PostQuitMessage(0)\n");
        PostQuitMessage(0);
        return 0;

    case WM_CHAR:
        say("  [gpuapp] WM_CHAR recebido.\n");
        return 0;

    case WM_DESTROY:
        say("  [gpuapp] WM_DESTROY: encerrando.\n");
        return DefWindowProcA(hwnd, msg, wParam, lParam);   // -> PostQuitMessage
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  Entry point. Sequencia exigida pela FASE 9.4:
//    1) RegisterClass + CreateWindowEx + ShowWindow.
//    2) DirectDrawCreateEx + SetCooperativeLevel + SetDisplayMode + CreateSurface.
//    3) Direct3DCreate7 + CreateDevice.
//    4) Loop GetMessage/DispatchMessage (WM_PAINT desenha + exercita D3D).
//    5) Release final dos objetos COM (refCount->0).
// ============================================================================
void _start(void) {
    say("  [gpuapp] inicio (ring 3). Registrando classe...\n");

    // -------- 1) GDI 32: classe + janela --------
    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc   = WindowProc;
    wc.hbrBackground = GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = "MeuOSGpuWindowClass";
    RegisterClassA(&wc);

    say("  [gpuapp] CreateWindowExA 800x600...\n");
    void* hwnd = CreateWindowExA(0, "MeuOSGpuWindowClass", "MeuOS GPU App",
                                 0, 40, 40, 240, 100, 0, 0, 0, 0);
    if (!hwnd) { say("  [gpuapp] CreateWindowEx falhou.\n"); ExitProcess(1); }

    // -------- 2) DirectDraw 7 --------
    say("  [gpuapp] DirectDrawCreateEx -> IDirectDraw7...\n");
    HRESULT hr = DirectDrawCreateEx(0, (void**)&g_dd, 0, 0);
    if (hr != DD_OK || !g_dd) {
        say("  [gpuapp] DirectDrawCreateEx FALHOU\n"); ExitProcess(2);
    }
    say_hex("  [gpuapp] DirectDrawCreate7 OK; IDirectDraw7=", (ULL)(unsigned long long)(void*)g_dd);

    say("  [gpuapp] IDirectDraw7::SetCooperativeLevel(NORMAL)...\n");
    g_dd->lpVtbl->SetCooperativeLevel(g_dd, hwnd, DDSCL_NORMAL);

    say("  [gpuapp] IDirectDraw7::SetDisplayMode(800,600,32,60,0)...\n");
    g_dd->lpVtbl->SetDisplayMode(g_dd, 800, 600, 32, 60, 0);

    // Cria a primary surface (no MeuOS o win32k ja e dono do FB; o stub apenas
    // devolve um handle COM falso, mas exercita o caminho CreateSurface).
    DDSURFACEDESC2 sd;
    for (unsigned i = 0; i < sizeof(sd); i++) ((char*)&sd)[i] = 0;
    sd.dwSize  = sizeof(sd);
    sd.dwFlags = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    say("  [gpuapp] IDirectDraw7::CreateSurface (primary)...\n");
    hr = g_dd->lpVtbl->CreateSurface(g_dd, &sd, &g_primary, 0);
    if (hr != DD_OK || !g_primary) {
        say("  [gpuapp] CreateSurface FALHOU\n"); ExitProcess(3);
    }
    say_hex("  [gpuapp] CreateSurface primary OK; IDirectDrawSurface7=",
            (ULL)(unsigned long long)(void*)g_primary);

    // -------- 3) Direct3D 7 --------
    say("  [gpuapp] Direct3DCreate7...\n");
    hr = Direct3DCreate7(D3D_SDK_VERSION, &g_d3d);
    if (hr != D3D_OK || !g_d3d) {
        say("  [gpuapp] Direct3DCreate7 FALHOU\n"); ExitProcess(4);
    }
    say_hex("  [gpuapp] Direct3DCreate7 OK; IDirect3D7=", (ULL)(unsigned long long)(void*)g_d3d);

    say("  [gpuapp] IDirect3D7::CreateDevice (HAL)...\n");
    hr = g_d3d->lpVtbl->CreateDevice(g_d3d, 0, g_primary, &g_device);
    if (hr != D3D_OK || !g_device) {
        say("  [gpuapp] CreateDevice FALHOU\n"); ExitProcess(5);
    }
    say_hex("  [gpuapp] CreateDevice OK; IDirect3DDevice7=",
            (ULL)(unsigned long long)(void*)g_device);

    // GetCaps comprova GetCaps + estrutura D3DDEVICEDESC7 (1..2048 textures).
    D3DDEVICEDESC7 caps;
    for (unsigned i = 0; i < sizeof(caps); i++) ((char*)&caps)[i] = 0;
    if (g_device->lpVtbl->GetCaps(g_device, &caps) == D3D_OK) {
        say_hex("  [gpuapp] D3D caps.dwMaxTextureWidth=", (ULL)caps.dwMaxTextureWidth);
        say_hex("  [gpuapp] D3D caps.dwMaxActiveLights=", (ULL)caps.dwMaxActiveLights);
    }

    // -------- 4) Loop de mensagens --------
    say("  [gpuapp] ShowWindow + GetMessage loop...\n");
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // -------- 5) Release dos objetos COM (refCount -> 0) --------
    say("  [gpuapp] Release dos objetos COM...\n");
    if (g_device)  g_device->lpVtbl->Release(g_device);
    if (g_d3d)     g_d3d->lpVtbl->Release(g_d3d);
    if (g_primary) g_primary->lpVtbl->Release(g_primary);
    if (g_dd)      g_dd->lpVtbl->Release(g_dd);

    say("  [gpuapp] fim do demo GPU. ExitProcess(0).\n");
    ExitProcess(0);
}
