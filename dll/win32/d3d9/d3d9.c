// d3d.dll  —  reimplementacao minima do Direct3D 7/8/9 (FASE 9.4).
//
// Igual ao Windows: a d3d.dll vive em RING 3, acoplada a ddraw.dll. No MeuOS
// nao temos rasterizador real (o win32k ja e dono do framebuffer pos-Fase 9.2),
// entao os stubs aqui apenas criam objetos COM falsos (IDirect3D7 e
// IDirect3DDevice7) e devolvem D3D_OK em quase tudo. Suficiente para um app
// Direct3D 7 carregar, enumerar devices, criar device, BeginScene/EndScene/
// Clear/Present e exercitar todo o ABI COM sem falhar.
//
// COM ABI (estilo NT/Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg "this".
//
// Em DLLs ring 3 do MeuOS nao temos VirtualAlloc/HeapAlloc Win32 reais (apenas
// stubs); por isso usamos POOLS ESTATICOS de objetos. Como sao apenas metadados
// (sem rendering real), os pools sao pequenos e suficientes para o teste.

unsigned int _tls_index = 0;

typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               HRESULT;
typedef unsigned long long ULL;
typedef void*              HWND;
typedef void*              HDC;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              REFIID;
typedef void*              REFCLSID;
typedef void*              IUnknown;
typedef float              D3DVALUE;

#define D3D_OK                      0x00000000L
#define E_NOINTERFACE               0x80004002L
#define D3DERR_INVALIDPARAMS        0x80070057L
#define D3DERR_OUTOFMEMORY          0x8007000EL
#define D3DERR_INVALIDDEVICE        0x8876086CL

// ============================================================================
//  Forward decls das interfaces.
// ============================================================================
struct IDirect3D7;
struct IDirect3DDevice7;
struct IDirect3D7Vtbl;
struct IDirect3DDevice7Vtbl;
struct IDirectDrawSurface7Impl;  // vem de ddraw.dll; aqui so referenciamos.

// ============================================================================
//  Estruturas D3D 7 (subset minimo do DDK).
// ============================================================================
#pragma pack(push, 1)

// D3DMATRIX — matriz 4x4 de floats. Padrao Direct3D row-major.
typedef struct _D3DMATRIX {
    D3DVALUE _11, _12, _13, _14;
    D3DVALUE _21, _22, _23, _24;
    D3DVALUE _31, _32, _33, _34;
    D3DVALUE _41, _42, _43, _44;
} D3DMATRIX;

// D3DVIEWPORT7 — descricao do viewport (rect 2D + range Z).
typedef struct _D3DVIEWPORT7 {
    DWORD    dwX;
    DWORD    dwY;
    DWORD    dwWidth;
    DWORD    dwHeight;
    D3DVALUE dvMinZ;
    D3DVALUE dvMaxZ;
} D3DVIEWPORT7;

// D3DRECT — retangulo para Clear.
typedef struct _D3DRECT {
    long x1, y1, x2, y2;
} D3DRECT;

// D3DDEVICEDESC7 — descricao basica do device (so o suficiente para EnumDevices).
typedef struct _D3DDEVICEDESC7 {
    DWORD    dwDevCaps;
    DWORD    dwMinTextureWidth;
    DWORD    dwMinTextureHeight;
    DWORD    dwMaxTextureWidth;
    DWORD    dwMaxTextureHeight;
    DWORD    dwMaxActiveLights;
    DWORD    deviceGUID[4];
    char     reserved[64];
} D3DDEVICEDESC7;

#pragma pack(pop)

// ============================================================================
//  POOL ESTATICO de objetos (sem heap em ring 3).
// ============================================================================
typedef struct IDirect3DDevice7Impl {
    const struct IDirect3DDevice7Vtbl* lpVtbl;
    long refCount;
    int  used;
    D3DMATRIX   transforms[8];   // WORLD, VIEW, PROJECTION, ...
    D3DVIEWPORT7 viewport;
    DWORD       renderStates[256];
} IDirect3DDevice7Impl;

#define MAX_DEVICES 4
static IDirect3DDevice7Impl g_devices[MAX_DEVICES];

typedef struct IDirect3D7Impl {
    const struct IDirect3D7Vtbl* lpVtbl;
    long refCount;
    int  used;
} IDirect3D7Impl;

#define MAX_D3D 4
static IDirect3D7Impl g_d3d[MAX_D3D];

// ============================================================================
//  Vtables.
// ============================================================================

// --- IDirect3DDevice7 ---
typedef struct IDirect3DDevice7Vtbl {
    HRESULT (*QueryInterface)(IDirect3DDevice7Impl* This, REFIID riid, void** ppv);
    DWORD   (*AddRef)        (IDirect3DDevice7Impl* This);
    DWORD   (*Release)       (IDirect3DDevice7Impl* This);

    HRESULT (*GetCaps)       (IDirect3DDevice7Impl* This, D3DDEVICEDESC7* desc);
    HRESULT (*EnumTextureFormats)(IDirect3DDevice7Impl* This, void* cb, void* ctx);
    HRESULT (*BeginScene)    (IDirect3DDevice7Impl* This);
    HRESULT (*EndScene)      (IDirect3DDevice7Impl* This);
    HRESULT (*GetDirect3D)   (IDirect3DDevice7Impl* This, IDirect3D7Impl** d3d);
    HRESULT (*SetRenderTarget)(IDirect3DDevice7Impl* This,
                              struct IDirectDrawSurface7Impl* surf, DWORD flags);
    HRESULT (*GetRenderTarget)(IDirect3DDevice7Impl* This,
                              struct IDirectDrawSurface7Impl** surf);
    HRESULT (*Clear)         (IDirect3DDevice7Impl* This, DWORD count,
                              D3DRECT* rects, DWORD flags, DWORD color,
                              D3DVALUE z, DWORD stencil);
    HRESULT (*SetTransform)  (IDirect3DDevice7Impl* This, DWORD state,
                              D3DMATRIX* mat);
    HRESULT (*GetTransform)  (IDirect3DDevice7Impl* This, DWORD state,
                              D3DMATRIX* mat);
    HRESULT (*SetViewport)   (IDirect3DDevice7Impl* This, D3DVIEWPORT7* vp);
    HRESULT (*GetViewport)   (IDirect3DDevice7Impl* This, D3DVIEWPORT7* vp);
    HRESULT (*SetMaterial)   (IDirect3DDevice7Impl* This, void* mat);
    HRESULT (*SetLight)      (IDirect3DDevice7Impl* This, DWORD idx, void* light);
    HRESULT (*LightEnable)   (IDirect3DDevice7Impl* This, DWORD idx, BOOL enable);
    HRESULT (*SetRenderState)(IDirect3DDevice7Impl* This, DWORD state, DWORD val);
    HRESULT (*GetRenderState)(IDirect3DDevice7Impl* This, DWORD state, DWORD* val);
    HRESULT (*BeginStateBlock)(IDirect3DDevice7Impl* This);
    HRESULT (*EndStateBlock) (IDirect3DDevice7Impl* This, DWORD* token);
    HRESULT (*PreLoad)       (IDirect3DDevice7Impl* This, void* tex);
    HRESULT (*DrawPrimitive) (IDirect3DDevice7Impl* This, DWORD type,
                              DWORD fvf, void* verts, DWORD count, DWORD flags);
    HRESULT (*DrawIndexedPrimitive)(IDirect3DDevice7Impl* This, DWORD type,
                              DWORD fvf, void* verts, DWORD vcount,
                              unsigned short* indices, DWORD icount, DWORD flags);
    HRESULT (*SetClipStatus) (IDirect3DDevice7Impl* This, void* status);
    HRESULT (*GetClipStatus) (IDirect3DDevice7Impl* This, void* status);
    HRESULT (*DrawPrimitiveStrided)(IDirect3DDevice7Impl* This, DWORD type,
                              DWORD fvf, void* data, DWORD count, DWORD flags);
    HRESULT (*ComputeSphereVisibility)(IDirect3DDevice7Impl* This, void* centers,
                              void* radii, DWORD count, DWORD flags, DWORD* ret);
    HRESULT (*GetTexture)    (IDirect3DDevice7Impl* This, DWORD stage, void** tex);
    HRESULT (*SetTexture)    (IDirect3DDevice7Impl* This, DWORD stage, void* tex);
    HRESULT (*GetTextureStageState)(IDirect3DDevice7Impl* This, DWORD stage,
                              DWORD state, DWORD* val);
    HRESULT (*SetTextureStageState)(IDirect3DDevice7Impl* This, DWORD stage,
                              DWORD state, DWORD val);
    HRESULT (*ValidateDevice)(IDirect3DDevice7Impl* This, DWORD* passes);
    HRESULT (*ApplyStateBlock)(IDirect3DDevice7Impl* This, DWORD token);
    HRESULT (*CaptureStateBlock)(IDirect3DDevice7Impl* This, DWORD token);
    HRESULT (*DeleteStateBlock)(IDirect3DDevice7Impl* This, DWORD token);
    HRESULT (*CreateStateBlock)(IDirect3DDevice7Impl* This, DWORD type, DWORD* tok);
    HRESULT (*Load)          (IDirect3DDevice7Impl* This, void* dst, void* dstP,
                              void* src, void* srcR, DWORD flags);
    HRESULT (*GetInfo)       (IDirect3DDevice7Impl* This, DWORD id, void* data, DWORD sz);
} IDirect3DDevice7Vtbl;

// --- IDirect3D7 ---
typedef HRESULT (*LPD3DENUMDEVICESCALLBACK7)(char* desc, char* name,
                                              D3DDEVICEDESC7* devDesc, void* ctx);

typedef struct IDirect3D7Vtbl {
    HRESULT (*QueryInterface)(IDirect3D7Impl* This, REFIID riid, void** ppv);
    DWORD   (*AddRef)        (IDirect3D7Impl* This);
    DWORD   (*Release)       (IDirect3D7Impl* This);

    HRESULT (*EnumDevices)   (IDirect3D7Impl* This, LPD3DENUMDEVICESCALLBACK7 cb,
                              void* ctx);
    HRESULT (*CreateDevice)  (IDirect3D7Impl* This, REFCLSID clsid,
                              struct IDirectDrawSurface7Impl* target,
                              IDirect3DDevice7Impl** dev);
    HRESULT (*CreateVertexBuffer)(IDirect3D7Impl* This, void* desc,
                              void** vb, DWORD flags);
    HRESULT (*EnumZBufferFormats)(IDirect3D7Impl* This, REFCLSID dev,
                              void* cb, void* ctx);
    HRESULT (*EvictManagedTextures)(IDirect3D7Impl* This);
} IDirect3D7Vtbl;

// ============================================================================
//  Implementacao dos metodos do IDirect3DDevice7. Quase todos no-op com D3D_OK.
// ============================================================================
static HRESULT Dev_QueryInterface(IDirect3DDevice7Impl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return D3DERR_INVALIDPARAMS;
    *ppv = This; This->refCount++;
    return D3D_OK;
}
static DWORD Dev_AddRef(IDirect3DDevice7Impl* This) { return (DWORD)(++This->refCount); }
static DWORD Dev_Release(IDirect3DDevice7Impl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (DWORD)n;
}
static HRESULT Dev_GetCaps(IDirect3DDevice7Impl* This, D3DDEVICEDESC7* d) {
    (void)This;
    if (!d) return D3DERR_INVALIDPARAMS;
    // Caps minimas: Reference Rasterizer "MeuOS RGB", 1..2048 textures, 8 lights.
    d->dwDevCaps          = 0x00010000;  // D3DDEVCAPS_FLOATTLVERTEX (fake)
    d->dwMinTextureWidth  = 1;
    d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth  = 2048;
    d->dwMaxTextureHeight = 2048;
    d->dwMaxActiveLights  = 8;
    return D3D_OK;
}
static HRESULT Dev_EnumTextureFormats(IDirect3DDevice7Impl* This, void* cb, void* ctx) {
    (void)This; (void)cb; (void)ctx; return D3D_OK;
}
static HRESULT Dev_BeginScene(IDirect3DDevice7Impl* This) { (void)This; return D3D_OK; }
static HRESULT Dev_EndScene  (IDirect3DDevice7Impl* This) { (void)This; return D3D_OK; }

static HRESULT Dev_GetDirect3D(IDirect3DDevice7Impl* This, IDirect3D7Impl** d3d) {
    (void)This;
    if (!d3d) return D3DERR_INVALIDPARAMS;
    // Procura o primeiro IDirect3D7 vivo no pool.
    for (int i = 0; i < MAX_D3D; i++) {
        if (g_d3d[i].used) { *d3d = &g_d3d[i]; g_d3d[i].refCount++; return D3D_OK; }
    }
    *d3d = 0; return D3DERR_INVALIDDEVICE;
}
static HRESULT Dev_SetRenderTarget(IDirect3DDevice7Impl* This,
        struct IDirectDrawSurface7Impl* s, DWORD f) {
    (void)This; (void)s; (void)f; return D3D_OK;
}
static HRESULT Dev_GetRenderTarget(IDirect3DDevice7Impl* This,
        struct IDirectDrawSurface7Impl** s) {
    (void)This; if (s) *s = 0; return D3D_OK;
}
// Clear: no-op — o framebuffer pertence ao win32k.
static HRESULT Dev_Clear(IDirect3DDevice7Impl* This, DWORD c, D3DRECT* r,
        DWORD f, DWORD col, D3DVALUE z, DWORD st) {
    (void)This; (void)c; (void)r; (void)f; (void)col; (void)z; (void)st;
    return D3D_OK;
}
static HRESULT Dev_SetTransform(IDirect3DDevice7Impl* This, DWORD state, D3DMATRIX* m) {
    if (!m) return D3DERR_INVALIDPARAMS;
    if (state < 8) This->transforms[state] = *m;     // cache simples
    return D3D_OK;
}
static HRESULT Dev_GetTransform(IDirect3DDevice7Impl* This, DWORD state, D3DMATRIX* m) {
    if (!m) return D3DERR_INVALIDPARAMS;
    if (state < 8) *m = This->transforms[state];
    return D3D_OK;
}
static HRESULT Dev_SetViewport(IDirect3DDevice7Impl* This, D3DVIEWPORT7* v) {
    if (!v) return D3DERR_INVALIDPARAMS;
    This->viewport = *v; return D3D_OK;
}
static HRESULT Dev_GetViewport(IDirect3DDevice7Impl* This, D3DVIEWPORT7* v) {
    if (!v) return D3DERR_INVALIDPARAMS;
    *v = This->viewport; return D3D_OK;
}
static HRESULT Dev_SetMaterial(IDirect3DDevice7Impl* This, void* m) {
    (void)This; (void)m; return D3D_OK;
}
static HRESULT Dev_SetLight(IDirect3DDevice7Impl* This, DWORD i, void* l) {
    (void)This; (void)i; (void)l; return D3D_OK;
}
static HRESULT Dev_LightEnable(IDirect3DDevice7Impl* This, DWORD i, BOOL e) {
    (void)This; (void)i; (void)e; return D3D_OK;
}
static HRESULT Dev_SetRenderState(IDirect3DDevice7Impl* This, DWORD st, DWORD v) {
    if (st < 256) This->renderStates[st] = v;
    return D3D_OK;
}
static HRESULT Dev_GetRenderState(IDirect3DDevice7Impl* This, DWORD st, DWORD* v) {
    if (!v) return D3DERR_INVALIDPARAMS;
    *v = (st < 256) ? This->renderStates[st] : 0;
    return D3D_OK;
}
static HRESULT Dev_BeginStateBlock(IDirect3DDevice7Impl* This) { (void)This; return D3D_OK; }
static HRESULT Dev_EndStateBlock(IDirect3DDevice7Impl* This, DWORD* tok) {
    (void)This; if (tok) *tok = 1; return D3D_OK;
}
static HRESULT Dev_PreLoad(IDirect3DDevice7Impl* This, void* t) {
    (void)This; (void)t; return D3D_OK;
}
// DrawPrimitive / DrawIndexedPrimitive: nao renderizam, devolvem D3D_OK.
static HRESULT Dev_DrawPrimitive(IDirect3DDevice7Impl* This, DWORD t, DWORD f,
        void* v, DWORD c, DWORD fl) {
    (void)This; (void)t; (void)f; (void)v; (void)c; (void)fl; return D3D_OK;
}
static HRESULT Dev_DrawIndexedPrimitive(IDirect3DDevice7Impl* This, DWORD t,
        DWORD f, void* v, DWORD vc, unsigned short* idx, DWORD ic, DWORD fl) {
    (void)This; (void)t; (void)f; (void)v; (void)vc; (void)idx; (void)ic; (void)fl;
    return D3D_OK;
}
static HRESULT Dev_SetClipStatus(IDirect3DDevice7Impl* This, void* s) {
    (void)This; (void)s; return D3D_OK;
}
static HRESULT Dev_GetClipStatus(IDirect3DDevice7Impl* This, void* s) {
    (void)This; (void)s; return D3D_OK;
}
static HRESULT Dev_DrawPrimitiveStrided(IDirect3DDevice7Impl* This, DWORD t,
        DWORD f, void* d, DWORD c, DWORD fl) {
    (void)This; (void)t; (void)f; (void)d; (void)c; (void)fl; return D3D_OK;
}
static HRESULT Dev_ComputeSphereVisibility(IDirect3DDevice7Impl* This, void* ce,
        void* r, DWORD c, DWORD f, DWORD* ret) {
    (void)This; (void)ce; (void)r; (void)c; (void)f;
    if (ret) *ret = 0;                // "tudo visivel"
    return D3D_OK;
}
static HRESULT Dev_GetTexture(IDirect3DDevice7Impl* This, DWORD s, void** t) {
    (void)This; (void)s; if (t) *t = 0; return D3D_OK;
}
static HRESULT Dev_SetTexture(IDirect3DDevice7Impl* This, DWORD s, void* t) {
    (void)This; (void)s; (void)t; return D3D_OK;
}
static HRESULT Dev_GetTextureStageState(IDirect3DDevice7Impl* This, DWORD st,
        DWORD state, DWORD* v) {
    (void)This; (void)st; (void)state; if (v) *v = 0; return D3D_OK;
}
static HRESULT Dev_SetTextureStageState(IDirect3DDevice7Impl* This, DWORD st,
        DWORD state, DWORD v) {
    (void)This; (void)st; (void)state; (void)v; return D3D_OK;
}
static HRESULT Dev_ValidateDevice(IDirect3DDevice7Impl* This, DWORD* p) {
    (void)This; if (p) *p = 1; return D3D_OK;
}
static HRESULT Dev_ApplyStateBlock(IDirect3DDevice7Impl* This, DWORD t) {
    (void)This; (void)t; return D3D_OK;
}
static HRESULT Dev_CaptureStateBlock(IDirect3DDevice7Impl* This, DWORD t) {
    (void)This; (void)t; return D3D_OK;
}
static HRESULT Dev_DeleteStateBlock(IDirect3DDevice7Impl* This, DWORD t) {
    (void)This; (void)t; return D3D_OK;
}
static HRESULT Dev_CreateStateBlock(IDirect3DDevice7Impl* This, DWORD t, DWORD* tok) {
    (void)This; (void)t; if (tok) *tok = 1; return D3D_OK;
}
static HRESULT Dev_Load(IDirect3DDevice7Impl* This, void* d, void* dp, void* s,
        void* sr, DWORD f) {
    (void)This; (void)d; (void)dp; (void)s; (void)sr; (void)f; return D3D_OK;
}
static HRESULT Dev_GetInfo(IDirect3DDevice7Impl* This, DWORD id, void* d, DWORD sz) {
    (void)This; (void)id; (void)d; (void)sz; return D3D_OK;
}

static const IDirect3DDevice7Vtbl g_devVtbl = {
    Dev_QueryInterface, Dev_AddRef, Dev_Release,
    Dev_GetCaps, Dev_EnumTextureFormats,
    Dev_BeginScene, Dev_EndScene,
    Dev_GetDirect3D, Dev_SetRenderTarget, Dev_GetRenderTarget,
    Dev_Clear,
    Dev_SetTransform, Dev_GetTransform,
    Dev_SetViewport, Dev_GetViewport,
    Dev_SetMaterial, Dev_SetLight, Dev_LightEnable,
    Dev_SetRenderState, Dev_GetRenderState,
    Dev_BeginStateBlock, Dev_EndStateBlock,
    Dev_PreLoad,
    Dev_DrawPrimitive, Dev_DrawIndexedPrimitive,
    Dev_SetClipStatus, Dev_GetClipStatus,
    Dev_DrawPrimitiveStrided, Dev_ComputeSphereVisibility,
    Dev_GetTexture, Dev_SetTexture,
    Dev_GetTextureStageState, Dev_SetTextureStageState,
    Dev_ValidateDevice,
    Dev_ApplyStateBlock, Dev_CaptureStateBlock, Dev_DeleteStateBlock,
    Dev_CreateStateBlock,
    Dev_Load, Dev_GetInfo,
};

// Aloca um slot novo no pool de devices.
static IDirect3DDevice7Impl* alloc_device(void) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            // Zera tudo (transforms identidade serao "zero matrix" — apps que
            // usam D3DTS_WORLD ainda assim chamam SetTransform; isso e apenas
            // cache. Renderizacao real nao acontece).
            for (unsigned k = 0; k < sizeof(g_devices[i]); k++) {
                ((unsigned char*)&g_devices[i])[k] = 0;
            }
            g_devices[i].used      = 1;
            g_devices[i].refCount  = 1;
            g_devices[i].lpVtbl    = &g_devVtbl;
            // Viewport default 1024x768 (Fase 9.1).
            g_devices[i].viewport.dwX      = 0;
            g_devices[i].viewport.dwY      = 0;
            g_devices[i].viewport.dwWidth  = 1024;
            g_devices[i].viewport.dwHeight = 768;
            g_devices[i].viewport.dvMinZ   = 0.0f;
            g_devices[i].viewport.dvMaxZ   = 1.0f;
            return &g_devices[i];
        }
    }
    return 0;
}

// ============================================================================
//  Implementacao do IDirect3D7.
// ============================================================================
static HRESULT D3D_QueryInterface(IDirect3D7Impl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return D3DERR_INVALIDPARAMS;
    *ppv = This; This->refCount++;
    return D3D_OK;
}
static DWORD D3D_AddRef(IDirect3D7Impl* This)  { return (DWORD)(++This->refCount); }
static DWORD D3D_Release(IDirect3D7Impl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (DWORD)n;
}
// EnumDevices(cb, ctx): chama callback uma vez com o nosso "Reference Rasterizer".
static HRESULT D3D_EnumDevices(IDirect3D7Impl* This, LPD3DENUMDEVICESCALLBACK7 cb,
        void* ctx) {
    (void)This;
    if (cb) {
        static char name[] = "MeuOS Reference Rasterizer";
        static char desc[] = "Software rasterizer (stub)";
        D3DDEVICEDESC7 dd = {0};
        dd.dwDevCaps          = 0x00010000;
        dd.dwMinTextureWidth  = 1;
        dd.dwMinTextureHeight = 1;
        dd.dwMaxTextureWidth  = 2048;
        dd.dwMaxTextureHeight = 2048;
        dd.dwMaxActiveLights  = 8;
        cb(desc, name, &dd, ctx);
    }
    return D3D_OK;
}
// CreateDevice(clsid, target, &dev): aloca slot do pool e devolve.
static HRESULT D3D_CreateDevice(IDirect3D7Impl* This, REFCLSID clsid,
        struct IDirectDrawSurface7Impl* target, IDirect3DDevice7Impl** dev) {
    (void)This; (void)clsid; (void)target;
    if (!dev) return D3DERR_INVALIDPARAMS;
    IDirect3DDevice7Impl* d = alloc_device();
    if (!d) { *dev = 0; return D3DERR_OUTOFMEMORY; }
    *dev = d;
    return D3D_OK;
}
static HRESULT D3D_CreateVertexBuffer(IDirect3D7Impl* This, void* desc,
        void** vb, DWORD flags) {
    (void)This; (void)desc; (void)flags;
    if (vb) *vb = (void*)0x56455254;     // 'VERT' fake
    return D3D_OK;
}
static HRESULT D3D_EnumZBufferFormats(IDirect3D7Impl* This, REFCLSID dev,
        void* cb, void* ctx) {
    (void)This; (void)dev; (void)cb; (void)ctx; return D3D_OK;
}
static HRESULT D3D_EvictManagedTextures(IDirect3D7Impl* This) {
    (void)This; return D3D_OK;
}

static const IDirect3D7Vtbl g_d3dVtbl = {
    D3D_QueryInterface, D3D_AddRef, D3D_Release,
    D3D_EnumDevices, D3D_CreateDevice,
    D3D_CreateVertexBuffer, D3D_EnumZBufferFormats,
    D3D_EvictManagedTextures,
};

// Aloca um slot novo no pool de IDirect3D7.
static IDirect3D7Impl* alloc_d3d(void) {
    for (int i = 0; i < MAX_D3D; i++) {
        if (!g_d3d[i].used) {
            g_d3d[i].used     = 1;
            g_d3d[i].refCount = 1;
            g_d3d[i].lpVtbl   = &g_d3dVtbl;
            return &g_d3d[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados (igual d3d8.dll/d3d9.dll do Windows).
//  Mantemos Direct3DCreate7 (a entry point real de DirectX 7 vive em ddraw.dll
//  via QueryInterface; ainda assim oferecemos a Create7 nominal para apps que
//  preferirem chamar diretamente).
// ============================================================================

// Direct3DCreate7(sdkVer, &d3d): cria o objeto IDirect3D7.
__declspec(dllexport) HRESULT Direct3DCreate7(DWORD sdkVer, IDirect3D7Impl** d3d) {
    (void)sdkVer;
    if (!d3d) return D3DERR_INVALIDPARAMS;
    IDirect3D7Impl* p = alloc_d3d();
    if (!p) { *d3d = 0; return D3DERR_OUTOFMEMORY; }
    *d3d = p;
    return D3D_OK;
}

// Direct3DCreate8(sdkVer) -> IDirect3D8*  (a 8 da DLL d3d8.dll do Windows).
// Aqui o stub devolve o mesmo objeto IDirect3D7 (apps DirectX 8 que so chamam
// QueryInterface/AddRef/Release ainda funcionam; rendering real nao existe).
// Assinatura classica: retorna ponteiro, nao HRESULT.
__declspec(dllexport) void* Direct3DCreate8(DWORD sdkVer) {
    (void)sdkVer;
    return (void*)alloc_d3d();
}

// Direct3DCreate9(sdkVer) -> IDirect3D9*  (a 9 da DLL d3d9.dll do Windows).
// Mesmo comportamento da Create8 — stub, sem rendering.
__declspec(dllexport) void* Direct3DCreate9(DWORD sdkVer) {
    (void)sdkVer;
    return (void*)alloc_d3d();
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
