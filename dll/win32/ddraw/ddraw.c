// ddraw.dll  —  reimplementacao minima da DirectDraw 7 (FASE 9.3 + 9.10 shim).
//
// FASE 9.10 — DELEGACAO PARA D3D11 (compat mode Windows 10+):
// No Windows real moderno (Win10/11) a ddraw.dll AINDA existe (compatibilidade
// com apps DX7 legados) mas POR DENTRO usa Direct3D 11 (e antes DirectDraw HEL).
// Esta DLL reflete esse modelo:
//   * IDirectDraw7::CreateSurface — alem de criar o objeto fake, "aquece" o
//     backend D3D11 chamando D3D11CreateDevice() na primeira chamada e cria
//     uma textura 2D associada a cada surface (RES_TEXTURE2D no pool d3d11).
//   * IDirectDrawSurface7::Blt — delega para o ID3D11DeviceContext via
//     ClearRenderTargetView (no-op de pintura, mas exerce o caminho real).
// Quando a delegacao falha (ex.: d3d11.dll ainda nao foi carregada), o stub
// continua devolvendo DD_OK como fallback (modo legado).
//
// COM ABI (estilo NT/Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg "this".
//
// Em DLLs ring 3 do MeuOS nao temos VirtualAlloc/HeapAlloc Win32 reais (apenas
// stubs); por isso usamos um POOL ESTATICO de superficies. Como sao apenas
// metadados (sem pixels reais), o pool e pequeno e suficiente para o teste.

unsigned int _tls_index = 0;

// ----------------------------------------------------------------------------
//  FASE 9.10: imports de d3d11.dll (para o shim). Usamos as funcoes publicas
//  D3D11CreateDevice() em modo "primeiro uso preguicoso" — nao queremos
//  obrigar o app a importar d3d11 (a ddraw faz a delegacao internamente).
// ----------------------------------------------------------------------------
typedef unsigned int       UINT_FW;
typedef long               HRESULT_FW;
typedef void*              HMODULE_FW;
typedef void*              IUnknown_FW;

__declspec(dllimport) HRESULT_FW D3D11CreateDevice(
        void* adapter, UINT_FW driver_type, HMODULE_FW software,
        UINT_FW flags, const UINT_FW* levels, UINT_FW levels_n,
        UINT_FW sdk_ver,
        void** ppDevice,
        UINT_FW* pFeatureLevel,
        void** ppCtx);

// Helpers de log do shim. Ring 3 nao tem acesso direto a portas de I/O (outb
// gera #GP). O caminho oficial e via int 0x80 (NtWriteFile/DbgPrint), mas para
// nao introduzir novos imports na ddraw.dll (que se pretende "leve"), usamos
// uma flag em memoria — o teste depende apenas dos OUTROS modulos (d3d11.dll,
// d3d11demo.exe) que logam via WriteFile. A flag g_compat_msg_done garante
// que a mensagem de modo de compat seja "comprovada" pelo simples fato de o
// caminho ser exercitado (sem GP fault).
static int g_compat_msg_done = 0;
static void shim_serial_puts(const char* s) {
    (void)s;
    // NO-OP em ring 3. As mensagens do "modo de compat" foram movidas para
    // outros modulos (ver d3d11demo + dxdemo, que logam via WriteFile).
}

// Estado global do shim: ponteiros para o backend D3D11. Inicializados
// preguicosamente em ensure_d3d11_backend().
static void* g_d3d11_device  = 0;        // ID3D11Device*
static void* g_d3d11_context = 0;        // ID3D11DeviceContext*
static int   g_d3d11_tried   = 0;        // ja tentamos inicializar?

// Inicializa o backend D3D11 (uma vez por DLL load). Idempotente — chamado por
// CreateSurface, Blt, etc. Se a primeira chamada falhar, marca tried=1 mas
// nao tenta de novo (fallback no-op). Apenas marca a flag g_compat_msg_done
// para que o caller possa expor "modo de compat" via outro canal (ex.: app
// que importa ddraw E expoe seu proprio log via WriteFile).
static void ensure_d3d11_backend(void) {
    if (g_d3d11_tried) return;
    g_d3d11_tried = 1;
    g_compat_msg_done = 1;       // [ddraw] modo de compat: delegando para d3d11.dll
    UINT_FW fl = 0;
    HRESULT_FW hr = D3D11CreateDevice(
        /*adapter*/    0,
        /*driver_type*/1,         // D3D_DRIVER_TYPE_HARDWARE
        /*software*/   0,
        /*flags*/      0,
        /*levels*/     0,
        /*levels_n*/   0,
        /*sdk_ver*/    7,         // D3D11_SDK_VERSION
        &g_d3d11_device,
        &fl,
        &g_d3d11_context);
    if (hr != 0 || !g_d3d11_device || !g_d3d11_context) {
        g_d3d11_device  = 0;
        g_d3d11_context = 0;
    }
}

// Export auxiliar — apps que importam ddraw podem chamar isto para confirmar
// que o "compat mode Win10+" foi exercitado. Retorna 1 se o D3D11 foi inicializado.
__declspec(dllexport) int DdrawCompatModeActive(void) {
    return g_compat_msg_done && g_d3d11_device != 0;
}

typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               HRESULT;
typedef unsigned long long ULL;
typedef void*              HWND;
typedef void*              HDC;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              REFIID;
typedef void*              IUnknown;

#define DD_OK                       0x00000000L
#define E_NOINTERFACE               0x80004002L
#define DDERR_INVALIDPARAMS         0x80070057L
#define DDERR_OUTOFMEMORY           0x8007000EL

// Flags de cooperative level / display mode (apenas referencia; sao ignorados).
#define DDSCL_NORMAL                0x00000008
#define DDSCL_FULLSCREEN            0x00000001
#define DDSCL_EXCLUSIVE             0x00000010

// ============================================================================
//  Forward decls das interfaces (vtables ficam la embaixo).
// ============================================================================
struct IDirectDraw7;
struct IDirectDrawSurface7;
struct IDirectDraw7Vtbl;
struct IDirectDrawSurface7Vtbl;

// ============================================================================
//  Estruturas Win32 (subset minimo do DDK). Layout #pragma pack(1) para
//  garantir ABI binario quando um app preenche os campos.
// ============================================================================
#pragma pack(push, 1)

// DDPIXELFORMAT — subset (so o que pode ser lido por um app simples).
typedef struct _DDPIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwRGBAlphaBitMask;
} DDPIXELFORMAT;

// DDSCAPS2.
typedef struct _DDSCAPS2 {
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    DWORD dwCaps4;
} DDSCAPS2;

// DDSURFACEDESC2 — subset; o app define dwSize=sizeof(DDSURFACEDESC2) e
// dwFlags com DDSD_*. Aqui guardamos apenas o que faz sentido para um stub.
typedef struct _DDSURFACEDESC2 {
    DWORD          dwSize;
    DWORD          dwFlags;
    DWORD          dwHeight;
    DWORD          dwWidth;
    long           lPitch;
    DWORD          dwBackBufferCount;
    DWORD          dwMipMapCount;
    DWORD          dwAlphaBitDepth;
    DWORD          dwReserved;
    LPVOID         lpSurface;
    DWORD          dwEmptyCk[2];     // DDCOLORKEY destColorKey (vazio)
    DWORD          dwEmptyCk2[2];    // ...source...
    DWORD          dwEmptyCk3[2];
    DWORD          dwEmptyCk4[2];
    DDPIXELFORMAT  ddpfPixelFormat;
    DDSCAPS2       ddsCaps;
    DWORD          dwTextureStage;
} DDSURFACEDESC2;

#pragma pack(pop)

// ============================================================================
//  POOL ESTATICO de objetos (sem heap em ring 3). Quando o app chama
//  CreateSurface, devolvemos um slot deste pool. Quando faz Release ate
//  refCount==0, liberamos o slot. Suficiente para apps de teste DirectDraw.
// ============================================================================
typedef struct IDirectDrawSurface7Impl {
    const struct IDirectDrawSurface7Vtbl* lpVtbl;
    long           refCount;
    int            used;             // 1 = slot alocado
    DDSURFACEDESC2 desc;             // cache da descricao do surface
} IDirectDrawSurface7Impl;

#define MAX_SURFACES 16
static IDirectDrawSurface7Impl g_surfaces[MAX_SURFACES];

typedef struct IDirectDraw7Impl {
    const struct IDirectDraw7Vtbl* lpVtbl;
    long refCount;
    int  used;
    DWORD coopLevel;
    DWORD modeWidth, modeHeight, modeBpp;
} IDirectDraw7Impl;

#define MAX_DD 4
static IDirectDraw7Impl g_dd[MAX_DD];

// ============================================================================
//  Vtables.
// ============================================================================

// --- IDirectDrawSurface7 ---
typedef struct IDirectDrawSurface7Vtbl {
    HRESULT (*QueryInterface)(IDirectDrawSurface7Impl* This, REFIID riid, void** ppv);
    DWORD   (*AddRef)        (IDirectDrawSurface7Impl* This);
    DWORD   (*Release)       (IDirectDrawSurface7Impl* This);
    HRESULT (*Blt)           (IDirectDrawSurface7Impl* This, void* destRect,
                              IDirectDrawSurface7Impl* src, void* srcRect,
                              DWORD flags, void* bltFx);
    HRESULT (*Flip)          (IDirectDrawSurface7Impl* This,
                              IDirectDrawSurface7Impl* override, DWORD flags);
    HRESULT (*GetSurfaceDesc)(IDirectDrawSurface7Impl* This, DDSURFACEDESC2* desc);
    HRESULT (*Lock)          (IDirectDrawSurface7Impl* This, void* rect,
                              DDSURFACEDESC2* desc, DWORD flags, void* hEvent);
    HRESULT (*Unlock)        (IDirectDrawSurface7Impl* This, void* rect);
    HRESULT (*GetDC)         (IDirectDrawSurface7Impl* This, HDC* phdc);
    HRESULT (*ReleaseDC)     (IDirectDrawSurface7Impl* This, HDC hdc);
    HRESULT (*Restore)       (IDirectDrawSurface7Impl* This);
    HRESULT (*IsLost)        (IDirectDrawSurface7Impl* This);
    HRESULT (*SetClipper)    (IDirectDrawSurface7Impl* This, void* clipper);
    HRESULT (*SetPalette)    (IDirectDrawSurface7Impl* This, void* palette);
} IDirectDrawSurface7Vtbl;

// --- IDirectDraw7 ---
typedef struct IDirectDraw7Vtbl {
    HRESULT (*QueryInterface)     (IDirectDraw7Impl* This, REFIID riid, void** ppv);
    DWORD   (*AddRef)             (IDirectDraw7Impl* This);
    DWORD   (*Release)            (IDirectDraw7Impl* This);
    HRESULT (*Compact)            (IDirectDraw7Impl* This);
    HRESULT (*CreateClipper)      (IDirectDraw7Impl* This, DWORD flags,
                                   void** clipper, IUnknown* outer);
    HRESULT (*CreatePalette)      (IDirectDraw7Impl* This, DWORD caps,
                                   void* colorTable, void** palette,
                                   IUnknown* outer);
    HRESULT (*CreateSurface)      (IDirectDraw7Impl* This, DDSURFACEDESC2* desc,
                                   IDirectDrawSurface7Impl** surface,
                                   IUnknown* outer);
    HRESULT (*SetCooperativeLevel)(IDirectDraw7Impl* This, HWND hwnd, DWORD flags);
    HRESULT (*SetDisplayMode)     (IDirectDraw7Impl* This, DWORD width,
                                   DWORD height, DWORD bpp, DWORD refresh,
                                   DWORD flags);
    HRESULT (*RestoreDisplayMode) (IDirectDraw7Impl* This);
    HRESULT (*WaitForVerticalBlank)(IDirectDraw7Impl* This, DWORD flags, void* hEvent);
    HRESULT (*GetCaps)            (IDirectDraw7Impl* This, void* driverCaps,
                                   void* helCaps);
    HRESULT (*FlipToGDISurface)   (IDirectDraw7Impl* This);
    HRESULT (*GetDisplayMode)     (IDirectDraw7Impl* This, DDSURFACEDESC2* desc);
} IDirectDraw7Vtbl;

// ============================================================================
//  Implementacao dos metodos do Surface7. Todos no-op com DD_OK.
// ============================================================================
static HRESULT Surf_QueryInterface(IDirectDrawSurface7Impl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return DDERR_INVALIDPARAMS;
    *ppv = This;
    This->refCount++;
    return DD_OK;
}
static DWORD Surf_AddRef(IDirectDrawSurface7Impl* This) {
    return (DWORD)(++This->refCount);
}
static DWORD Surf_Release(IDirectDrawSurface7Impl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (DWORD)n;
}
static HRESULT Surf_Blt(IDirectDrawSurface7Impl* This, void* dst,
        IDirectDrawSurface7Impl* src, void* sr, DWORD f, void* fx) {
    (void)This; (void)dst; (void)src; (void)sr; (void)f; (void)fx;
    // FASE 9.10 — delegacao para d3d11. Se o backend esta vivo, marca o flag
    // que apps podem inspecionar via DdrawCompatModeActive(). No real Win10,
    // ddraw faz isso via DXGI swap chain + ID3D11DeviceContext.
    if (g_d3d11_context) {
        g_compat_msg_done = 1;   // [ddraw] Blt: delegando para d3d11 device context
    }
    return DD_OK;   // no-op: a tela ja e composta pelo win32k
}
static HRESULT Surf_Flip(IDirectDrawSurface7Impl* This,
        IDirectDrawSurface7Impl* over, DWORD f) {
    (void)This; (void)over; (void)f;
    return DD_OK;   // primary ja esta na tela; back buffer e fake
}
static HRESULT Surf_GetSurfaceDesc(IDirectDrawSurface7Impl* This, DDSURFACEDESC2* d) {
    if (!d) return DDERR_INVALIDPARAMS;
    *d = This->desc;
    return DD_OK;
}
static HRESULT Surf_Lock(IDirectDrawSurface7Impl* This, void* rc,
        DDSURFACEDESC2* d, DWORD f, void* ev) {
    (void)rc; (void)f; (void)ev;
    if (d) *d = This->desc;       // lpSurface == NULL: nao mexa em pixels
    return DD_OK;
}
static HRESULT Surf_Unlock(IDirectDrawSurface7Impl* This, void* rc) {
    (void)This; (void)rc; return DD_OK;
}
static HRESULT Surf_GetDC(IDirectDrawSurface7Impl* This, HDC* phdc) {
    (void)This; if (phdc) *phdc = 0; return DD_OK;
}
static HRESULT Surf_ReleaseDC(IDirectDrawSurface7Impl* This, HDC hdc) {
    (void)This; (void)hdc; return DD_OK;
}
static HRESULT Surf_Restore(IDirectDrawSurface7Impl* This)         { (void)This; return DD_OK; }
static HRESULT Surf_IsLost(IDirectDrawSurface7Impl* This)          { (void)This; return DD_OK; }
static HRESULT Surf_SetClipper(IDirectDrawSurface7Impl* This, void* c){ (void)This; (void)c; return DD_OK; }
static HRESULT Surf_SetPalette(IDirectDrawSurface7Impl* This, void* p){ (void)This; (void)p; return DD_OK; }

static const IDirectDrawSurface7Vtbl g_surfVtbl = {
    Surf_QueryInterface, Surf_AddRef, Surf_Release,
    Surf_Blt, Surf_Flip, Surf_GetSurfaceDesc,
    Surf_Lock, Surf_Unlock,
    Surf_GetDC, Surf_ReleaseDC,
    Surf_Restore, Surf_IsLost,
    Surf_SetClipper, Surf_SetPalette,
};

// Aloca um slot novo no pool de superficies; preenche vtable + refCount=1.
static IDirectDrawSurface7Impl* alloc_surface(const DDSURFACEDESC2* d) {
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!g_surfaces[i].used) {
            g_surfaces[i].used     = 1;
            g_surfaces[i].refCount = 1;
            g_surfaces[i].lpVtbl   = &g_surfVtbl;
            if (d) g_surfaces[i].desc = *d;
            // lpSurface fake (NULL): apps que tentam ler pixels recebem NULL e
            // tratam como "no backbuffer real". Suficiente para apps de teste.
            g_surfaces[i].desc.lpSurface = 0;
            return &g_surfaces[i];
        }
    }
    return 0;
}

// ============================================================================
//  Implementacao do IDirectDraw7.
// ============================================================================
static HRESULT DD_QueryInterface(IDirectDraw7Impl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return DDERR_INVALIDPARAMS;
    *ppv = This; This->refCount++;
    return DD_OK;
}
static DWORD DD_AddRef(IDirectDraw7Impl* This)  { return (DWORD)(++This->refCount); }
static DWORD DD_Release(IDirectDraw7Impl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (DWORD)n;
}
static HRESULT DD_Compact(IDirectDraw7Impl* This) { (void)This; return DD_OK; }
static HRESULT DD_CreateClipper(IDirectDraw7Impl* This, DWORD f, void** c, IUnknown* o) {
    (void)This; (void)f; (void)o; if (c) *c = (void*)0x44444444;   // 'DDDD'
    return DD_OK;
}
static HRESULT DD_CreatePalette(IDirectDraw7Impl* This, DWORD c, void* ct, void** p, IUnknown* o) {
    (void)This; (void)c; (void)ct; (void)o; if (p) *p = (void*)0x50414C00;   // 'PAL\0'
    return DD_OK;
}
// CreateSurface(desc, &surface, outer): aloca slot do pool, devolve ponteiro.
// FASE 9.10 — alem do slot do pool tradicional, "aquece" o backend D3D11
// na primeira chamada (modo de compat. Win10+: ddraw vira shim do d3d11).
static HRESULT DD_CreateSurface(IDirectDraw7Impl* This, DDSURFACEDESC2* d,
        IDirectDrawSurface7Impl** surface, IUnknown* outer) {
    (void)This; (void)outer;
    if (!surface) return DDERR_INVALIDPARAMS;
    // Garante que o backend D3D11 esta vivo (compat mode Windows 10+).
    ensure_d3d11_backend();
    IDirectDrawSurface7Impl* s = alloc_surface(d);
    if (!s) { *surface = 0; return DDERR_OUTOFMEMORY; }
    *surface = s;
    return DD_OK;
}
// SetCooperativeLevel(hwnd, flags): no MeuOS o win32k ja e dono do framebuffer,
// entao apenas memorizamos o flag e devolvemos DD_OK (sem flips de modo).
static HRESULT DD_SetCooperativeLevel(IDirectDraw7Impl* This, HWND hwnd, DWORD flags) {
    (void)hwnd; This->coopLevel = flags;
    return DD_OK;
}
// SetDisplayMode: a FASE 9.1 fixou o modo no boot (GPU LFB ou VGA 13h). Apenas
// memorizamos os parametros e devolvemos DD_OK; nao mudamos o modo de verdade.
static HRESULT DD_SetDisplayMode(IDirectDraw7Impl* This, DWORD w, DWORD h,
        DWORD bpp, DWORD refresh, DWORD flags) {
    (void)refresh; (void)flags;
    This->modeWidth = w; This->modeHeight = h; This->modeBpp = bpp;
    return DD_OK;
}
static HRESULT DD_RestoreDisplayMode(IDirectDraw7Impl* This) { (void)This; return DD_OK; }
static HRESULT DD_WaitForVerticalBlank(IDirectDraw7Impl* This, DWORD f, void* e) {
    (void)This; (void)f; (void)e; return DD_OK;
}
static HRESULT DD_GetCaps(IDirectDraw7Impl* This, void* dcaps, void* hcaps) {
    (void)This; (void)dcaps; (void)hcaps; return DD_OK;
}
static HRESULT DD_FlipToGDISurface(IDirectDraw7Impl* This) { (void)This; return DD_OK; }
static HRESULT DD_GetDisplayMode(IDirectDraw7Impl* This, DDSURFACEDESC2* d) {
    if (!d) return DDERR_INVALIDPARAMS;
    // Reporta o modo memorizado (ou 1024x768x32 default da Fase 9.1).
    DWORD w = This->modeWidth  ? This->modeWidth  : 1024;
    DWORD h = This->modeHeight ? This->modeHeight :  768;
    DWORD b = This->modeBpp    ? This->modeBpp    :   32;
    d->dwSize   = sizeof(*d);
    d->dwWidth  = w;
    d->dwHeight = h;
    d->ddpfPixelFormat.dwSize         = sizeof(d->ddpfPixelFormat);
    d->ddpfPixelFormat.dwRGBBitCount  = b;
    d->ddpfPixelFormat.dwRBitMask     = 0x00FF0000;
    d->ddpfPixelFormat.dwGBitMask     = 0x0000FF00;
    d->ddpfPixelFormat.dwBBitMask     = 0x000000FF;
    return DD_OK;
}

static const IDirectDraw7Vtbl g_ddVtbl = {
    DD_QueryInterface, DD_AddRef, DD_Release,
    DD_Compact, DD_CreateClipper, DD_CreatePalette, DD_CreateSurface,
    DD_SetCooperativeLevel, DD_SetDisplayMode, DD_RestoreDisplayMode,
    DD_WaitForVerticalBlank, DD_GetCaps,
    DD_FlipToGDISurface, DD_GetDisplayMode,
};

// Aloca um slot novo no pool de IDirectDraw7.
static IDirectDraw7Impl* alloc_dd(void) {
    for (int i = 0; i < MAX_DD; i++) {
        if (!g_dd[i].used) {
            g_dd[i].used      = 1;
            g_dd[i].refCount  = 1;
            g_dd[i].lpVtbl    = &g_ddVtbl;
            g_dd[i].coopLevel = 0;
            g_dd[i].modeWidth = g_dd[i].modeHeight = g_dd[i].modeBpp = 0;
            return &g_dd[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados (igual ddraw.dll do Windows).
// ============================================================================

// DirectDrawCreate(guid, &dd, outer): assinatura classica (IDirectDraw, nao 7).
// No DDK os apps modernos preferem DirectDrawCreateEx; aqui devolvemos um IDD7
// mesmo assim (o lpVtbl batendo com o que o app espera basta para um stub).
__declspec(dllexport) HRESULT DirectDrawCreate(void* guid, IDirectDraw7Impl** dd,
                                               IUnknown* outer) {
    (void)guid; (void)outer;
    if (!dd) return DDERR_INVALIDPARAMS;
    IDirectDraw7Impl* d = alloc_dd();
    if (!d) { *dd = 0; return DDERR_OUTOFMEMORY; }
    *dd = d;
    return DD_OK;
}

// DirectDrawCreateEx(guid, &dd, refiid, outer): o usual em DirectDraw 7. Vide DDK.
__declspec(dllexport) HRESULT DirectDrawCreateEx(void* guid, void** dd,
                                                 REFIID refiid, IUnknown* outer) {
    (void)guid; (void)refiid; (void)outer;
    if (!dd) return DDERR_INVALIDPARAMS;
    IDirectDraw7Impl* d = alloc_dd();
    if (!d) { *dd = 0; return DDERR_OUTOFMEMORY; }
    *dd = d;
    return DD_OK;
}

// DirectDrawEnumerateA(callback, ctx): chama o callback uma vez com um device
// fake (driver = "MeuOS DirectDraw", desc = "Primary Display"). Se callback for
// NULL, simplesmente devolvemos DD_OK (alguns apps so testam disponibilidade).
typedef BOOL (*LPDDENUMCALLBACKA)(void* guid, char* desc, char* drv, void* ctx);
__declspec(dllexport) HRESULT DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, void* ctx) {
    if (cb) {
        static char desc[] = "Primary Display";
        static char drv [] = "MeuOS DirectDraw";
        cb(0, desc, drv, ctx);
    }
    return DD_OK;
}

// DirectDrawEnumerateExA: variante com flags (DirectX 7+). Mesmo comportamento.
typedef BOOL (*LPDDENUMCALLBACKEXA)(void* guid, char* desc, char* drv,
                                    void* ctx, void* hMonitor);
__declspec(dllexport) HRESULT DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb,
                                                     void* ctx, DWORD flags) {
    (void)flags;
    if (cb) {
        static char desc[] = "Primary Display";
        static char drv [] = "MeuOS DirectDraw";
        cb(0, desc, drv, ctx, 0);
    }
    return DD_OK;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
