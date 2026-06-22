// d3d11demo\d3d11demo.c  —  Aplicativo demo do stack WDDM 2.x (FASE 9.10).
//
// Exercita o caminho completo Windows 10+ de uma app Direct3D 11:
//   1) CreateWindowExA (HWND via user32/win32k)
//   2) CreateDXGIFactory -> IDXGIFactory
//   3) IDXGIFactory::EnumAdapters(0, ...) -> IDXGIAdapter (Vendor=0x1234, Device=0x1111)
//   4) D3D11CreateDeviceAndSwapChain (passa o HWND)
//   5) ID3D11DeviceContext::ClearRenderTargetView (R=0.2 G=0.4 B=0.7 A=1.0)
//   6) IDXGISwapChain::Present(1, 0)
//   7) Loop GetMessage/Dispatch; em WM_PAINT refaz Clear+Present.
//
// Caminho completo: ring3 (d3d11demo) -> kernel32/user32/dxgi/d3d11 -> ntdll ->
// int 0x80 -> kernel/win32k -> framebuffer. Cada etapa loga na serial; o
// caminho do DXGI+D3D11 e exercitado mesmo sem rasterizador real (os stubs
// devolvem S_OK, mas o ABI COM e completo).
//
// ImageBase 0x420000: faixa baixa LIVRE entre o kernel (0x100000-0x1D5000),
// o USTACK (0x600000-0x700000) e o dxdemo.exe (0x400000). Distancia segura
// — dxdemo + d3d11demo nao colidem mesmo se ambos sao carregados.

unsigned long _tls_index = 0;

typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               HRESULT;
typedef unsigned long long ULL;
typedef unsigned long long UINT64;
typedef long long          INT64;
typedef unsigned short     WCHAR;
typedef void*              HWND;
typedef void*              HDC;
typedef void*              HMODULE;
typedef void*              REFIID;
typedef void*              IUnknown;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef float              FLOAT;

// ---- console (logs do app, alem dos logs do kernel) ----
#define STD_OUTPUT_HANDLE ((unsigned)-11)
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) void  ExitProcess(unsigned code);

// ---- user32 ----
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

// ---- gdi32 ----
#define LTGRAY_BRUSH 1
__declspec(dllimport) void* GetStockObject(int);

// ============================================================================
//  Tipos DXGI minimos (subset).
// ============================================================================
#pragma pack(push, 8)
typedef struct DXGI_ADAPTER_DESC {
    WCHAR  Description[128];
    UINT   VendorId;
    UINT   DeviceId;
    UINT   SubSysId;
    UINT   Revision;
    UINT64 DedicatedVideoMemory;
    UINT64 DedicatedSystemMemory;
    UINT64 SharedSystemMemory;
    struct { UINT LowPart; LONG HighPart; } AdapterLuid;
} DXGI_ADAPTER_DESC;

typedef struct DXGI_RATIONAL { UINT Numerator; UINT Denominator; } DXGI_RATIONAL;
typedef struct DXGI_MODE_DESC {
    UINT          Width;
    UINT          Height;
    DXGI_RATIONAL RefreshRate;
    UINT          Format;
    UINT          ScanlineOrdering;
    UINT          Scaling;
} DXGI_MODE_DESC;
typedef struct DXGI_SAMPLE_DESC { UINT Count; UINT Quality; } DXGI_SAMPLE_DESC;
typedef struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE_DESC   BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    DWORD            BufferUsage;
    UINT             BufferCount;
    HWND             OutputWindow;
    BOOL             Windowed;
    UINT             SwapEffect;
    UINT             Flags;
} DXGI_SWAP_CHAIN_DESC;
#pragma pack(pop)

#define DXGI_FORMAT_B8G8R8A8_UNORM       87
#define DXGI_USAGE_RENDER_TARGET_OUTPUT  0x00000020L
#define DXGI_SWAP_EFFECT_DISCARD         0

// Forward decls dos objetos DXGI.
typedef struct IDXGIFactoryImpl   IDXGIFactory;
typedef struct IDXGIAdapterImpl   IDXGIAdapter;
typedef struct IDXGIOutputImpl    IDXGIOutput;
typedef struct IDXGISwapChainImpl IDXGISwapChain;

// Vtables DXGI (espelham dll/win32/dxgi/dxgi.c).
typedef struct IDXGIAdapterVtbl {
    HRESULT (*QueryInterface)(IDXGIAdapter*, REFIID, void**);
    ULONG   (*AddRef)        (IDXGIAdapter*);
    ULONG   (*Release)       (IDXGIAdapter*);
    HRESULT (*SetPrivateData)(IDXGIAdapter*, void*, UINT, const void*);
    HRESULT (*SetPrivateDataInterface)(IDXGIAdapter*, void*, const IUnknown*);
    HRESULT (*GetPrivateData)(IDXGIAdapter*, void*, UINT*, void*);
    HRESULT (*GetParent)     (IDXGIAdapter*, REFIID, void**);
    HRESULT (*EnumOutputs)   (IDXGIAdapter*, UINT, IDXGIOutput**);
    HRESULT (*GetDesc)       (IDXGIAdapter*, DXGI_ADAPTER_DESC*);
    HRESULT (*CheckInterfaceSupport)(IDXGIAdapter*, void*, UINT64*);
    HRESULT (*GetDesc1)      (IDXGIAdapter*, void*);
} IDXGIAdapterVtbl;
struct IDXGIAdapterImpl { const IDXGIAdapterVtbl* lpVtbl; };

typedef struct IDXGIFactoryVtbl {
    HRESULT (*QueryInterface)(IDXGIFactory*, REFIID, void**);
    ULONG   (*AddRef)        (IDXGIFactory*);
    ULONG   (*Release)       (IDXGIFactory*);
    HRESULT (*SetPrivateData)(IDXGIFactory*, void*, UINT, const void*);
    HRESULT (*SetPrivateDataInterface)(IDXGIFactory*, void*, const IUnknown*);
    HRESULT (*GetPrivateData)(IDXGIFactory*, void*, UINT*, void*);
    HRESULT (*GetParent)     (IDXGIFactory*, REFIID, void**);
    HRESULT (*EnumAdapters)  (IDXGIFactory*, UINT, IDXGIAdapter**);
    HRESULT (*MakeWindowAssociation)(IDXGIFactory*, HWND, UINT);
    HRESULT (*GetWindowAssociation) (IDXGIFactory*, HWND*);
    HRESULT (*CreateSwapChain)     (IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                    IDXGISwapChain**);
    HRESULT (*CreateSoftwareAdapter)(IDXGIFactory*, void*, IDXGIAdapter**);
    HRESULT (*EnumAdapters1)(IDXGIFactory*, UINT, IDXGIAdapter**);
    BOOL    (*IsCurrent)    (IDXGIFactory*);
} IDXGIFactoryVtbl;
struct IDXGIFactoryImpl { const IDXGIFactoryVtbl* lpVtbl; };

typedef struct IDXGISwapChainVtbl {
    HRESULT (*QueryInterface)(IDXGISwapChain*, REFIID, void**);
    ULONG   (*AddRef)        (IDXGISwapChain*);
    ULONG   (*Release)       (IDXGISwapChain*);
    HRESULT (*SetPrivateData)(IDXGISwapChain*, void*, UINT, const void*);
    HRESULT (*SetPrivateDataInterface)(IDXGISwapChain*, void*, const IUnknown*);
    HRESULT (*GetPrivateData)(IDXGISwapChain*, void*, UINT*, void*);
    HRESULT (*GetParent)     (IDXGISwapChain*, REFIID, void**);
    HRESULT (*GetDevice)     (IDXGISwapChain*, REFIID, void**);
    HRESULT (*Present)       (IDXGISwapChain*, UINT, UINT);
    HRESULT (*GetBuffer)     (IDXGISwapChain*, UINT, REFIID, void**);
    HRESULT (*SetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
    HRESULT (*GetFullscreenState)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
    HRESULT (*GetDesc)       (IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
    HRESULT (*ResizeBuffers) (IDXGISwapChain*, UINT, UINT, UINT, UINT, UINT);
    HRESULT (*ResizeTarget)  (IDXGISwapChain*, const DXGI_MODE_DESC*);
    HRESULT (*GetContainingOutput)(IDXGISwapChain*, IDXGIOutput**);
    HRESULT (*GetFrameStatistics)(IDXGISwapChain*, void*);
    HRESULT (*GetLastPresentCount)(IDXGISwapChain*, UINT*);
} IDXGISwapChainVtbl;
struct IDXGISwapChainImpl { const IDXGISwapChainVtbl* lpVtbl; };

__declspec(dllimport) HRESULT CreateDXGIFactory(REFIID riid, void** ppFactory);

// ============================================================================
//  Tipos D3D11 minimos.
// ============================================================================
typedef struct ID3D11DeviceImpl        ID3D11Device;
typedef struct ID3D11DeviceContextImpl ID3D11DeviceContext;
typedef struct ID3D11ResourceImpl      ID3D11Resource;       // RTV via mesma vtbl

// Vtables D3D11 (espelham dll/win32/d3d11/d3d11.c — ordem dos metodos).
typedef struct ID3D11DeviceContextVtbl {
    HRESULT (*QueryInterface)(ID3D11DeviceContext*, REFIID, void**);
    ULONG   (*AddRef)        (ID3D11DeviceContext*);
    ULONG   (*Release)       (ID3D11DeviceContext*);
    void    (*GetDevice)     (ID3D11DeviceContext*, void**);
    HRESULT (*GetPrivateData)(ID3D11DeviceContext*, void*, UINT*, void*);
    HRESULT (*SetPrivateData)(ID3D11DeviceContext*, void*, UINT, const void*);
    HRESULT (*SetPrivateDataInterface)(ID3D11DeviceContext*, void*, const IUnknown*);
    void    (*VSSetConstantBuffers)(ID3D11DeviceContext*, UINT, UINT, void**);
    void    (*PSSetShaderResources)(ID3D11DeviceContext*, UINT, UINT, void**);
    void    (*PSSetShader)         (ID3D11DeviceContext*, ID3D11Resource*, void**, UINT);
    void    (*PSSetSamplers)       (ID3D11DeviceContext*, UINT, UINT, void**);
    void    (*VSSetShader)         (ID3D11DeviceContext*, ID3D11Resource*, void**, UINT);
    void    (*DrawIndexed)         (ID3D11DeviceContext*, UINT, UINT, int);
    void    (*Draw)                (ID3D11DeviceContext*, UINT, UINT);
    HRESULT (*Map)                 (ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, void*);
    void    (*Unmap)               (ID3D11DeviceContext*, ID3D11Resource*, UINT);
    void    (*PSSetConstantBuffers)(ID3D11DeviceContext*, UINT, UINT, void**);
    void    (*IASetInputLayout)    (ID3D11DeviceContext*, ID3D11Resource*);
    void    (*IASetVertexBuffers)  (ID3D11DeviceContext*, UINT, UINT, void**, const UINT*, const UINT*);
    void    (*IASetIndexBuffer)    (ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT);
    void    (*DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, int, UINT);
    void    (*DrawInstanced)       (ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
    void    (*IASetPrimitiveTopology)(ID3D11DeviceContext*, UINT);
    void    (*RSSetState)          (ID3D11DeviceContext*, ID3D11Resource*);
    void    (*RSSetViewports)      (ID3D11DeviceContext*, UINT, const void*);
    void    (*RSSetScissorRects)   (ID3D11DeviceContext*, UINT, const void*);
    void    (*OMSetRenderTargets)  (ID3D11DeviceContext*, UINT, ID3D11Resource**, ID3D11Resource*);
    void    (*OMSetBlendState)     (ID3D11DeviceContext*, ID3D11Resource*, const FLOAT*, UINT);
    void    (*OMSetDepthStencilState)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
    void    (*ClearRenderTargetView)(ID3D11DeviceContext*, ID3D11Resource*, const FLOAT[4]);
    void    (*ClearDepthStencilView)(ID3D11DeviceContext*, ID3D11Resource*, UINT, FLOAT, unsigned char);
    HRESULT (*FinishCommandList)   (ID3D11DeviceContext*, BOOL, void**);
} ID3D11DeviceContextVtbl;
struct ID3D11DeviceContextImpl { const ID3D11DeviceContextVtbl* lpVtbl; };

// (ID3D11Device vtbl nao precisa estar completa; chamamos apenas Release.)
typedef struct ID3D11DeviceVtbl {
    HRESULT (*QueryInterface)(ID3D11Device*, REFIID, void**);
    ULONG   (*AddRef)        (ID3D11Device*);
    ULONG   (*Release)       (ID3D11Device*);
} ID3D11DeviceVtbl;
struct ID3D11DeviceImpl { const ID3D11DeviceVtbl* lpVtbl; };

__declspec(dllimport) HRESULT D3D11CreateDeviceAndSwapChain(
        void* adapter, UINT driver_type, HMODULE software,
        UINT flags, const UINT* levels, UINT levels_n,
        UINT sdk_ver,
        const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
        IDXGISwapChain** ppSwapChain,
        ID3D11Device** ppDevice,
        UINT* pFeatureLevel,
        ID3D11DeviceContext** ppCtx);

#define D3D_DRIVER_TYPE_HARDWARE  1
#define D3D_FEATURE_LEVEL_11_0    0xb000
#define D3D11_SDK_VERSION         7
#define S_OK                      0x00000000L

// ============================================================================
//  Helpers de log.
// ============================================================================
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}
static void say_hex(const char* prefix, ULL v) {
    static const char hex[] = "0123456789ABCDEF";
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
//  Estado global do app.
// ============================================================================
static IDXGIFactory*       g_factory   = 0;
static IDXGIAdapter*       g_adapter   = 0;
static IDXGISwapChain*     g_swap      = 0;
static ID3D11Device*       g_device    = 0;
static ID3D11DeviceContext* g_context  = 0;
static int                 g_frame_n   = 0;
static int                 g_painted   = 0;

// Faz um Clear+Present coerente (R=0.2 G=0.4 B=0.7 A=1.0 — azul-acinzentado).
static void do_clear_and_present(void) {
    if (!g_context || !g_swap) return;
    g_frame_n++;
    const FLOAT color[4] = { 0.2f, 0.4f, 0.7f, 1.0f };
    g_context->lpVtbl->ClearRenderTargetView(g_context, 0, color);
    say_hex("  [d3d11demo] ClearRenderTargetView + Present (frame ", (ULL)g_frame_n);
    g_swap->lpVtbl->Present(g_swap, 1, 0);
}

// ============================================================================
//  WindowProc.
// ============================================================================
static long long WindowProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    switch (msg) {
    case WM_CREATE:
        say("  [d3d11demo] WM_CREATE.\n");
        return 0;

    case WM_PAINT: {
        say("  [d3d11demo] WM_PAINT: render via D3D11.\n");
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        do_clear_and_present();
        EndPaint(hwnd, &ps);

        if (!g_painted) {
            g_painted = 1;
            say("  [d3d11demo] 1a pintura concluida -> PostQuitMessage(0).\n");
            PostQuitMessage(0);
        }
        return 0;
    }

    case WM_KEYDOWN:
        say("  [d3d11demo] WM_KEYDOWN -> PostQuitMessage(0)\n");
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        say("  [d3d11demo] WM_DESTROY.\n");
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  Entry point.
// ============================================================================
void _start(void) {
    say("  [d3d11demo] inicio (ring 3). Stack WDDM 2.x exerce.\n");

    // -------- 1) Janela 1024x768 --------
    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc   = WindowProc;
    wc.hbrBackground = GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = "MeuOSD3D11Window";
    RegisterClassA(&wc);

    say("  [d3d11demo] CreateWindowExA 1024x768...\n");
    void* hwnd = CreateWindowExA(0, "MeuOSD3D11Window", "MeuOS D3D11 Demo",
                                 0, 0, 0, 1024, 768, 0, 0, 0, 0);
    if (!hwnd) { say("  [d3d11demo] CreateWindowEx falhou.\n"); ExitProcess(1); }

    // -------- 2) CreateDXGIFactory --------
    say("  [d3d11demo] CreateDXGIFactory...\n");
    HRESULT hr = CreateDXGIFactory(0, (void**)&g_factory);
    if (hr != S_OK || !g_factory) {
        say("  [d3d11demo] CreateDXGIFactory FALHOU.\n"); ExitProcess(2);
    }
    say("[d3d11demo] CreateDXGIFactory -> S_OK\n");

    // -------- 3) EnumAdapters(0, ...) --------
    say("  [d3d11demo] EnumAdapters(0, ...)...\n");
    hr = g_factory->lpVtbl->EnumAdapters(g_factory, 0, &g_adapter);
    if (hr != S_OK || !g_adapter) {
        say("  [d3d11demo] EnumAdapters FALHOU.\n"); ExitProcess(3);
    }
    // Verifica VendorId/DeviceId
    DXGI_ADAPTER_DESC desc;
    for (unsigned i = 0; i < sizeof(desc); i++) ((char*)&desc)[i] = 0;
    if (g_adapter->lpVtbl->GetDesc(g_adapter, &desc) == S_OK) {
        say_hex("[d3d11demo] EnumAdapter 0: Vendor=", (ULL)desc.VendorId);
        say_hex("[d3d11demo] EnumAdapter 0: Device=", (ULL)desc.DeviceId);
    }

    // -------- 4) D3D11CreateDeviceAndSwapChain --------
    DXGI_SWAP_CHAIN_DESC scd;
    for (unsigned i = 0; i < sizeof(scd); i++) ((char*)&scd)[i] = 0;
    scd.BufferDesc.Width   = 1024;
    scd.BufferDesc.Height  = 768;
    scd.BufferDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.SampleDesc.Count   = 1;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount        = 2;          // double buffer
    scd.OutputWindow       = hwnd;
    scd.Windowed           = 1;
    scd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags              = 0;

    say("  [d3d11demo] D3D11CreateDeviceAndSwapChain...\n");
    UINT feature_levels[1] = { D3D_FEATURE_LEVEL_11_0 };
    UINT chosen = 0;
    hr = D3D11CreateDeviceAndSwapChain(
            (void*)g_adapter,            // adapter (mesmo do EnumAdapters)
            D3D_DRIVER_TYPE_HARDWARE,
            0,                            // software
            0,                            // flags
            feature_levels, 1,
            D3D11_SDK_VERSION,
            &scd,
            &g_swap,
            &g_device,
            &chosen,
            &g_context);
    if (hr != S_OK || !g_device || !g_context || !g_swap) {
        say("  [d3d11demo] D3D11CreateDeviceAndSwapChain FALHOU.\n"); ExitProcess(4);
    }
    say("[d3d11demo] D3D11CreateDeviceAndSwapChain -> S_OK\n");
    say_hex("  [d3d11demo] FeatureLevel=", (ULL)chosen);

    // -------- 5,6) ClearRenderTargetView + Present (frame 1) --------
    do_clear_and_present();

    // -------- 7) ShowWindow + GetMessage loop --------
    say("  [d3d11demo] ShowWindow + GetMessage loop...\n");
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // -------- Release final dos objetos COM (refCount -> 0) --------
    say("  [d3d11demo] Release dos objetos COM...\n");
    if (g_context) g_context->lpVtbl->Release(g_context);
    if (g_device)  g_device->lpVtbl->Release(g_device);
    if (g_swap)    g_swap->lpVtbl->Release(g_swap);
    if (g_adapter) g_adapter->lpVtbl->Release(g_adapter);
    if (g_factory) g_factory->lpVtbl->Release(g_factory);

    say("  [d3d11demo] fim do demo D3D11. ExitProcess(0).\n");
    ExitProcess(0);
}
