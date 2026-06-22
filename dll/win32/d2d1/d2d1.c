// d2d1.dll  —  reimplementacao minima do Direct2D (FASE 9.9).
//
// Direct2D e a API 2D moderna, hardware-accelerated do Windows 7+. Acima de
// DXGI e Direct3D 10/11 (no Windows real), ela substitui GDI/GDI+ para
// renderizacao 2D de alta qualidade com anti-aliasing por pixel-shader,
// transformacoes afins (matriz 3x2) e composicao alfa. Apps modernos como
// Edge, Office e Explorer usam Direct2D para desenhar UI.
//
// Pipeline real: app -> d2d1.dll -> d3d10/11.dll -> dxgi.dll -> dxgkrnl.sys ->
// driver da placa. No MeuOS o backend grafico real e o BasicDisplay (KMD)
// expondo um framebuffer linear. Aqui no ring 3 nao temos rasterizador
// programavel; entao este stub se limita ao ABI COM (vtable + refcount)
// de ID2D1Factory + ID2D1RenderTarget + ID2D1Brush + ID2D1Bitmap +
// ID2D1Geometry — tudo retornando S_OK e handles fake.
//
// COM ABI (estilo Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg
// "this". Em ABI ms_abi (x86_64-windows-gnu) os parametros entram em
// RCX,RDX,R8,R9 (essa e a ABI que o zig cc gera com -target windows-gnu).
//
// Em DLLs ring 3 do MeuOS nao temos VirtualAlloc/HeapAlloc Win32 reais
// (apenas stubs); por isso usamos POOLS ESTATICOS de objetos. Pools
// dimensionados para um app D2D simples: 4 factories, 4 render targets,
// 16 brushes, 16 bitmaps, 16 geometrias.
//
// IMAGE BASE: 0x4700000 — sobreposicao com PMM_BASE (0x4000000). Para evitar
// colisao usamos --dynamicbase no build (.reloc), entao o loader pode realocar
// para qualquer endereco virtual livre. Mesmo mecanismo de d3d11/d3d12.

unsigned int _tls_index = 0;

// ============================================================================
//  Tipos Windows minimos.
// ============================================================================
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef int                INT;
typedef long               HRESULT;
typedef unsigned long long ULL;
typedef unsigned long long UINT64;
typedef long long          INT64;
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef void*              HWND;
typedef void*              HDC;
typedef void*              HANDLE;
typedef void*              HMODULE;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              REFIID;
typedef void*              REFGUID;
typedef void*              IUnknown;
typedef unsigned char      BYTE;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef unsigned long long ULONG64;
typedef float              FLOAT;
typedef unsigned long long SIZE_T;

#define S_OK                         0x00000000L
#define S_FALSE                      0x00000001L
#define E_NOTIMPL                    0x80004001L
#define E_NOINTERFACE                0x80004002L
#define E_POINTER                    0x80004003L
#define E_FAIL                       0x80004005L
#define E_INVALIDARG                 0x80070057L
#define E_OUTOFMEMORY                0x8007000EL

// D2D1_FACTORY_TYPE — single-threaded e o default; multi-threaded usa lock interno.
#define D2D1_FACTORY_TYPE_SINGLE_THREADED  0
#define D2D1_FACTORY_TYPE_MULTI_THREADED   1

// D2D1_RENDER_TARGET_TYPE.
#define D2D1_RENDER_TARGET_TYPE_DEFAULT    0
#define D2D1_RENDER_TARGET_TYPE_SOFTWARE   1
#define D2D1_RENDER_TARGET_TYPE_HARDWARE   2

// D2D1_PRESENT_OPTIONS.
#define D2D1_PRESENT_OPTIONS_NONE                  0x0
#define D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS       0x1
#define D2D1_PRESENT_OPTIONS_IMMEDIATELY           0x2

// DXGI_FORMAT (subset).
#define DXGI_FORMAT_UNKNOWN              0
#define DXGI_FORMAT_R8G8B8A8_UNORM       28
#define DXGI_FORMAT_B8G8R8A8_UNORM       87

// D2D1_ALPHA_MODE.
#define D2D1_ALPHA_MODE_UNKNOWN          0
#define D2D1_ALPHA_MODE_PREMULTIPLIED    1
#define D2D1_ALPHA_MODE_STRAIGHT         2
#define D2D1_ALPHA_MODE_IGNORE           3

// D2D1_DRAW_TEXT_OPTIONS (subset).
#define D2D1_DRAW_TEXT_OPTIONS_NONE      0
#define D2D1_DRAW_TEXT_OPTIONS_NO_SNAP   1
#define D2D1_DRAW_TEXT_OPTIONS_CLIP      2

// D2D1_DEBUG_LEVEL.
#define D2D1_DEBUG_LEVEL_NONE            0
#define D2D1_DEBUG_LEVEL_ERROR           1
#define D2D1_DEBUG_LEVEL_WARNING         2
#define D2D1_DEBUG_LEVEL_INFORMATION     3

// ============================================================================
//  Estruturas D2D1 publicas (subset relevante).
// ============================================================================
#pragma pack(push, 8)

// D2D1_POINT_2F (par de floats).
typedef struct D2D1_POINT_2F { FLOAT x, y; } D2D1_POINT_2F;
// D2D1_POINT_2U (par de uints — coord de pixel).
typedef struct D2D1_POINT_2U { UINT  x, y; } D2D1_POINT_2U;
// D2D1_SIZE_F.
typedef struct D2D1_SIZE_F   { FLOAT width, height; } D2D1_SIZE_F;
// D2D1_SIZE_U.
typedef struct D2D1_SIZE_U   { UINT  width, height; } D2D1_SIZE_U;
// D2D1_RECT_F (retangulo float).
typedef struct D2D1_RECT_F   { FLOAT left, top, right, bottom; } D2D1_RECT_F;
// D2D1_RECT_U (retangulo uint — em pixels).
typedef struct D2D1_RECT_U   { UINT  left, top, right, bottom; } D2D1_RECT_U;

// D2D1_COLOR_F (RGBA float [0..1]).
typedef struct D2D1_COLOR_F  { FLOAT r, g, b, a; } D2D1_COLOR_F;

// D2D1_MATRIX_3X2_F (transformacao afim 2D — 6 floats em layout row-major).
typedef struct D2D1_MATRIX_3X2_F {
    FLOAT _11, _12;
    FLOAT _21, _22;
    FLOAT _31, _32;
} D2D1_MATRIX_3X2_F;

// D2D1_PIXEL_FORMAT — formato + alpha mode.
typedef struct D2D1_PIXEL_FORMAT {
    UINT format;       // DXGI_FORMAT
    UINT alphaMode;    // D2D1_ALPHA_MODE
} D2D1_PIXEL_FORMAT;

// D2D1_RENDER_TARGET_PROPERTIES.
typedef struct D2D1_RENDER_TARGET_PROPERTIES {
    UINT             type;        // D2D1_RENDER_TARGET_TYPE
    D2D1_PIXEL_FORMAT pixelFormat;
    FLOAT            dpiX;
    FLOAT            dpiY;
    UINT             usage;
    UINT             minLevel;
} D2D1_RENDER_TARGET_PROPERTIES;

// D2D1_HWND_RENDER_TARGET_PROPERTIES.
typedef struct D2D1_HWND_RENDER_TARGET_PROPERTIES {
    HWND          hwnd;
    D2D1_SIZE_U   pixelSize;
    UINT          presentOptions;     // D2D1_PRESENT_OPTIONS
} D2D1_HWND_RENDER_TARGET_PROPERTIES;

// D2D1_BRUSH_PROPERTIES (opacity + transform).
typedef struct D2D1_BRUSH_PROPERTIES {
    FLOAT             opacity;
    D2D1_MATRIX_3X2_F transform;
} D2D1_BRUSH_PROPERTIES;

// D2D1_BITMAP_PROPERTIES.
typedef struct D2D1_BITMAP_PROPERTIES {
    D2D1_PIXEL_FORMAT pixelFormat;
    FLOAT             dpiX;
    FLOAT             dpiY;
} D2D1_BITMAP_PROPERTIES;

// D2D1_ELLIPSE.
typedef struct D2D1_ELLIPSE {
    D2D1_POINT_2F point;
    FLOAT         radiusX;
    FLOAT         radiusY;
} D2D1_ELLIPSE;

// D2D1_ROUNDED_RECT.
typedef struct D2D1_ROUNDED_RECT {
    D2D1_RECT_F rect;
    FLOAT       radiusX;
    FLOAT       radiusY;
} D2D1_ROUNDED_RECT;

#pragma pack(pop)

// ============================================================================
//  Forward decls.
// ============================================================================
struct ID2D1FactoryImpl;
struct ID2D1RenderTargetImpl;
struct ID2D1BrushImpl;
struct ID2D1BitmapImpl;
struct ID2D1GeometryImpl;

struct ID2D1FactoryVtbl;
struct ID2D1RenderTargetVtbl;
struct ID2D1BrushVtbl;
struct ID2D1BitmapVtbl;
struct ID2D1GeometryVtbl;

// ============================================================================
//  POOLS ESTATICOS. Sem heap em ring 3 — todas as instancias vivem aqui.
// ============================================================================
#define MAX_FACTORIES        4
#define MAX_RENDER_TARGETS   4
#define MAX_BRUSHES         16
#define MAX_BITMAPS         16
#define MAX_GEOMETRIES      16

// Tag para diferenciar tipos de geometria (Rect/Rounded/Ellipse/Path).
#define D2D_GEO_RECT       1
#define D2D_GEO_ROUNDED    2
#define D2D_GEO_ELLIPSE    3
#define D2D_GEO_PATH       4

// Tag para diferenciar tipos de brush (Solid/Linear/Radial/Bitmap).
#define D2D_BR_SOLID       1
#define D2D_BR_LINEAR      2
#define D2D_BR_RADIAL      3
#define D2D_BR_BITMAP      4

typedef struct ID2D1FactoryImpl {
    const struct ID2D1FactoryVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT factoryType;        // SINGLE/MULTI threaded
} ID2D1FactoryImpl;

typedef struct ID2D1BrushImpl {
    const struct ID2D1BrushVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT tag;                // D2D_BR_*
    D2D1_COLOR_F color;      // para SOLID
    FLOAT opacity;
    D2D1_MATRIX_3X2_F transform;
} ID2D1BrushImpl;

typedef struct ID2D1BitmapImpl {
    const struct ID2D1BitmapVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    D2D1_SIZE_U pixelSize;
    D2D1_PIXEL_FORMAT pixelFormat;
    FLOAT dpiX, dpiY;
} ID2D1BitmapImpl;

typedef struct ID2D1GeometryImpl {
    const struct ID2D1GeometryVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT tag;                // D2D_GEO_*
    D2D1_RECT_F  rect;
    D2D1_ROUNDED_RECT rounded;
    D2D1_ELLIPSE ellipse;
} ID2D1GeometryImpl;

typedef struct ID2D1RenderTargetImpl {
    const struct ID2D1RenderTargetVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    HWND hwnd;                  // janela alvo (NULL se DXGI/bitmap/etc.)
    D2D1_SIZE_U pixelSize;
    D2D1_MATRIX_3X2_F transform;
    INT  drawing;               // 1 entre BeginDraw/EndDraw
} ID2D1RenderTargetImpl;

static ID2D1FactoryImpl       g_factories     [MAX_FACTORIES];
static ID2D1RenderTargetImpl  g_render_targets[MAX_RENDER_TARGETS];
static ID2D1BrushImpl         g_brushes       [MAX_BRUSHES];
static ID2D1BitmapImpl        g_bitmaps       [MAX_BITMAPS];
static ID2D1GeometryImpl      g_geometries    [MAX_GEOMETRIES];

// ----------------------------------------------------------------------------
//  Utilitarios.
// ----------------------------------------------------------------------------
static void mem_zero(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  Vtable: ID2D1Brush (base de SolidColor/Linear/Radial/Bitmap brushes).
// ============================================================================
typedef struct ID2D1BrushVtbl {
    // --- IUnknown ---
    HRESULT (*QueryInterface)(ID2D1BrushImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID2D1BrushImpl* This);
    ULONG   (*Release)       (ID2D1BrushImpl* This);
    // --- ID2D1Resource ---
    void    (*GetFactory)    (ID2D1BrushImpl* This, void** factory);
    // --- ID2D1Brush ---
    void    (*SetOpacity)    (ID2D1BrushImpl* This, FLOAT opacity);
    void    (*SetTransform)  (ID2D1BrushImpl* This, const D2D1_MATRIX_3X2_F* m);
    FLOAT   (*GetOpacity)    (ID2D1BrushImpl* This);
    void    (*GetTransform)  (ID2D1BrushImpl* This, D2D1_MATRIX_3X2_F* m);
    // --- ID2D1SolidColorBrush proprio ---
    void    (*SetColor)      (ID2D1BrushImpl* This, const D2D1_COLOR_F* color);
    void    (*GetColor)      (ID2D1BrushImpl* This, D2D1_COLOR_F* color);
} ID2D1BrushVtbl;

static HRESULT Br_QueryInterface(ID2D1BrushImpl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Br_AddRef (ID2D1BrushImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Br_Release(ID2D1BrushImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static void Br_GetFactory(ID2D1BrushImpl* T, void** f) { (void)T; if (f) *f = &g_factories[0]; }
static void Br_SetOpacity (ID2D1BrushImpl* T, FLOAT op) { T->opacity = op; }
static void Br_SetTransform(ID2D1BrushImpl* T, const D2D1_MATRIX_3X2_F* m) {
    if (m) T->transform = *m;
}
static FLOAT Br_GetOpacity(ID2D1BrushImpl* T) { return T->opacity; }
static void Br_GetTransform(ID2D1BrushImpl* T, D2D1_MATRIX_3X2_F* m) {
    if (m) *m = T->transform;
}
static void Br_SetColor(ID2D1BrushImpl* T, const D2D1_COLOR_F* c) { if (c) T->color = *c; }
static void Br_GetColor(ID2D1BrushImpl* T, D2D1_COLOR_F* c)       { if (c) *c = T->color; }

static const ID2D1BrushVtbl g_brushVtbl = {
    Br_QueryInterface, Br_AddRef, Br_Release,
    Br_GetFactory,
    Br_SetOpacity, Br_SetTransform, Br_GetOpacity, Br_GetTransform,
    Br_SetColor, Br_GetColor,
};

static ID2D1BrushImpl* alloc_brush(UINT tag, const D2D1_COLOR_F* color,
                                    const D2D1_BRUSH_PROPERTIES* props) {
    for (int i = 0; i < MAX_BRUSHES; i++) {
        if (!g_brushes[i].used) {
            mem_zero(&g_brushes[i], sizeof(g_brushes[i]));
            g_brushes[i].used     = 1;
            g_brushes[i].refCount = 1;
            g_brushes[i].lpVtbl   = &g_brushVtbl;
            g_brushes[i].tag      = tag;
            if (color) g_brushes[i].color = *color;
            g_brushes[i].opacity = props ? props->opacity : 1.0f;
            if (props) {
                g_brushes[i].transform = props->transform;
            } else {
                // identidade
                g_brushes[i].transform._11 = 1.0f;
                g_brushes[i].transform._22 = 1.0f;
            }
            return &g_brushes[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: ID2D1Bitmap.
// ============================================================================
typedef struct ID2D1BitmapVtbl {
    HRESULT (*QueryInterface)(ID2D1BitmapImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID2D1BitmapImpl* This);
    ULONG   (*Release)       (ID2D1BitmapImpl* This);
    void    (*GetFactory)    (ID2D1BitmapImpl* This, void** factory);
    // --- ID2D1Image (sem metodos proprios alem de IUnknown+GetFactory) ---
    // --- ID2D1Bitmap proprio ---
    void    (*GetSize)       (ID2D1BitmapImpl* This, D2D1_SIZE_F* size);
    void    (*GetPixelSize)  (ID2D1BitmapImpl* This, D2D1_SIZE_U* size);
    void    (*GetPixelFormat)(ID2D1BitmapImpl* This, D2D1_PIXEL_FORMAT* fmt);
    void    (*GetDpi)        (ID2D1BitmapImpl* This, FLOAT* dpiX, FLOAT* dpiY);
    HRESULT (*CopyFromBitmap)(ID2D1BitmapImpl* This, const D2D1_POINT_2U* dst,
                              ID2D1BitmapImpl* src, const D2D1_RECT_U* srcRect);
    HRESULT (*CopyFromRenderTarget)(ID2D1BitmapImpl* This, const D2D1_POINT_2U* dst,
                                    void* renderTarget, const D2D1_RECT_U* srcRect);
    HRESULT (*CopyFromMemory)(ID2D1BitmapImpl* This, const D2D1_RECT_U* dstRect,
                              LPCVOID srcData, UINT pitch);
} ID2D1BitmapVtbl;

static HRESULT Bm_QueryInterface(ID2D1BitmapImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Bm_AddRef (ID2D1BitmapImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Bm_Release(ID2D1BitmapImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static void Bm_GetFactory(ID2D1BitmapImpl* T, void** f) { (void)T; if (f) *f = &g_factories[0]; }
static void Bm_GetSize(ID2D1BitmapImpl* T, D2D1_SIZE_F* s) {
    if (!s) return;
    s->width  = (FLOAT)T->pixelSize.width;
    s->height = (FLOAT)T->pixelSize.height;
}
static void Bm_GetPixelSize(ID2D1BitmapImpl* T, D2D1_SIZE_U* s)   { if (s) *s = T->pixelSize; }
static void Bm_GetPixelFormat(ID2D1BitmapImpl* T, D2D1_PIXEL_FORMAT* f){ if (f) *f = T->pixelFormat; }
static void Bm_GetDpi(ID2D1BitmapImpl* T, FLOAT* x, FLOAT* y) {
    if (x) *x = T->dpiX; if (y) *y = T->dpiY;
}
static HRESULT Bm_CopyFromBitmap(ID2D1BitmapImpl* T, const D2D1_POINT_2U* d,
                                 ID2D1BitmapImpl* s, const D2D1_RECT_U* r) {
    (void)T; (void)d; (void)s; (void)r; return S_OK;
}
static HRESULT Bm_CopyFromRenderTarget(ID2D1BitmapImpl* T, const D2D1_POINT_2U* d,
                                       void* rt, const D2D1_RECT_U* r) {
    (void)T; (void)d; (void)rt; (void)r; return S_OK;
}
static HRESULT Bm_CopyFromMemory(ID2D1BitmapImpl* T, const D2D1_RECT_U* r,
                                 LPCVOID data, UINT pitch) {
    (void)T; (void)r; (void)data; (void)pitch; return S_OK;
}

static const ID2D1BitmapVtbl g_bitmapVtbl = {
    Bm_QueryInterface, Bm_AddRef, Bm_Release,
    Bm_GetFactory,
    Bm_GetSize, Bm_GetPixelSize, Bm_GetPixelFormat, Bm_GetDpi,
    Bm_CopyFromBitmap, Bm_CopyFromRenderTarget, Bm_CopyFromMemory,
};

static ID2D1BitmapImpl* alloc_bitmap(D2D1_SIZE_U size, const D2D1_BITMAP_PROPERTIES* p) {
    for (int i = 0; i < MAX_BITMAPS; i++) {
        if (!g_bitmaps[i].used) {
            mem_zero(&g_bitmaps[i], sizeof(g_bitmaps[i]));
            g_bitmaps[i].used      = 1;
            g_bitmaps[i].refCount  = 1;
            g_bitmaps[i].lpVtbl    = &g_bitmapVtbl;
            g_bitmaps[i].pixelSize = size;
            if (p) {
                g_bitmaps[i].pixelFormat = p->pixelFormat;
                g_bitmaps[i].dpiX = p->dpiX;
                g_bitmaps[i].dpiY = p->dpiY;
            } else {
                g_bitmaps[i].pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
                g_bitmaps[i].pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                g_bitmaps[i].dpiX = 96.0f;
                g_bitmaps[i].dpiY = 96.0f;
            }
            return &g_bitmaps[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: ID2D1Geometry (Rect/Rounded/Ellipse/Path).
// ============================================================================
typedef struct ID2D1GeometryVtbl {
    HRESULT (*QueryInterface)(ID2D1GeometryImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID2D1GeometryImpl* This);
    ULONG   (*Release)       (ID2D1GeometryImpl* This);
    void    (*GetFactory)    (ID2D1GeometryImpl* This, void** factory);
    // --- ID2D1Geometry proprio (subset) ---
    HRESULT (*GetBounds)     (ID2D1GeometryImpl* This, const D2D1_MATRIX_3X2_F* worldTransform,
                              D2D1_RECT_F* bounds);
    HRESULT (*FillContainsPoint)(ID2D1GeometryImpl* This, D2D1_POINT_2F pt,
                                  const D2D1_MATRIX_3X2_F* m, FLOAT tol, BOOL* contains);
    HRESULT (*StrokeContainsPoint)(ID2D1GeometryImpl* This, D2D1_POINT_2F pt,
                                    FLOAT strokeWidth, void* style,
                                    const D2D1_MATRIX_3X2_F* m, FLOAT tol, BOOL* contains);
} ID2D1GeometryVtbl;

static HRESULT Geo_QueryInterface(ID2D1GeometryImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Geo_AddRef (ID2D1GeometryImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Geo_Release(ID2D1GeometryImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static void Geo_GetFactory(ID2D1GeometryImpl* T, void** f) { (void)T; if (f) *f = &g_factories[0]; }
static HRESULT Geo_GetBounds(ID2D1GeometryImpl* T, const D2D1_MATRIX_3X2_F* m, D2D1_RECT_F* b) {
    (void)m;
    if (!b) return E_POINTER;
    if (T->tag == D2D_GEO_RECT) { *b = T->rect; return S_OK; }
    if (T->tag == D2D_GEO_ROUNDED) { *b = T->rounded.rect; return S_OK; }
    if (T->tag == D2D_GEO_ELLIPSE) {
        b->left   = T->ellipse.point.x - T->ellipse.radiusX;
        b->top    = T->ellipse.point.y - T->ellipse.radiusY;
        b->right  = T->ellipse.point.x + T->ellipse.radiusX;
        b->bottom = T->ellipse.point.y + T->ellipse.radiusY;
        return S_OK;
    }
    b->left = b->top = b->right = b->bottom = 0;
    return S_OK;
}
static HRESULT Geo_FillContainsPoint(ID2D1GeometryImpl* T, D2D1_POINT_2F pt,
                                      const D2D1_MATRIX_3X2_F* m, FLOAT tol, BOOL* contains) {
    (void)T; (void)pt; (void)m; (void)tol;
    if (contains) *contains = 0;
    return S_OK;
}
static HRESULT Geo_StrokeContainsPoint(ID2D1GeometryImpl* T, D2D1_POINT_2F pt,
                                        FLOAT sw, void* style,
                                        const D2D1_MATRIX_3X2_F* m, FLOAT tol, BOOL* contains) {
    (void)T; (void)pt; (void)sw; (void)style; (void)m; (void)tol;
    if (contains) *contains = 0;
    return S_OK;
}

static const ID2D1GeometryVtbl g_geometryVtbl = {
    Geo_QueryInterface, Geo_AddRef, Geo_Release,
    Geo_GetFactory,
    Geo_GetBounds, Geo_FillContainsPoint, Geo_StrokeContainsPoint,
};

static ID2D1GeometryImpl* alloc_geometry(UINT tag) {
    for (int i = 0; i < MAX_GEOMETRIES; i++) {
        if (!g_geometries[i].used) {
            mem_zero(&g_geometries[i], sizeof(g_geometries[i]));
            g_geometries[i].used     = 1;
            g_geometries[i].refCount = 1;
            g_geometries[i].lpVtbl   = &g_geometryVtbl;
            g_geometries[i].tag      = tag;
            return &g_geometries[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: ID2D1RenderTarget. E o objeto onde quase tudo de desenho acontece:
//  BeginDraw, Clear, FillRectangle, DrawText, CreateSolidColorBrush, EndDraw.
// ============================================================================
typedef struct ID2D1RenderTargetVtbl {
    // --- IUnknown ---
    HRESULT (*QueryInterface)(ID2D1RenderTargetImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID2D1RenderTargetImpl* This);
    ULONG   (*Release)       (ID2D1RenderTargetImpl* This);
    // --- ID2D1Resource ---
    void    (*GetFactory)    (ID2D1RenderTargetImpl* This, void** factory);
    // --- ID2D1RenderTarget proprio ---
    HRESULT (*CreateBitmap)  (ID2D1RenderTargetImpl* This, D2D1_SIZE_U size,
                              LPCVOID srcData, UINT pitch,
                              const D2D1_BITMAP_PROPERTIES* props,
                              ID2D1BitmapImpl** bitmap);
    HRESULT (*CreateBitmapFromWicBitmap)(ID2D1RenderTargetImpl* This, void* wic,
                                          const D2D1_BITMAP_PROPERTIES* props,
                                          ID2D1BitmapImpl** bitmap);
    HRESULT (*CreateSharedBitmap)(ID2D1RenderTargetImpl* This, REFIID riid,
                                   void* data, const D2D1_BITMAP_PROPERTIES* props,
                                   ID2D1BitmapImpl** bitmap);
    HRESULT (*CreateBitmapBrush)(ID2D1RenderTargetImpl* This, ID2D1BitmapImpl* bm,
                                  void* bmBrushProps, const D2D1_BRUSH_PROPERTIES* props,
                                  ID2D1BrushImpl** brush);
    HRESULT (*CreateSolidColorBrush)(ID2D1RenderTargetImpl* This,
                                      const D2D1_COLOR_F* color,
                                      const D2D1_BRUSH_PROPERTIES* props,
                                      ID2D1BrushImpl** brush);
    HRESULT (*CreateGradientStopCollection)(ID2D1RenderTargetImpl* This,
                                             const void* stops, UINT count,
                                             UINT gamma, UINT extend,
                                             void** out);
    HRESULT (*CreateLinearGradientBrush)(ID2D1RenderTargetImpl* This,
                                          const void* lgProps,
                                          const D2D1_BRUSH_PROPERTIES* props,
                                          void* stops, ID2D1BrushImpl** brush);
    HRESULT (*CreateRadialGradientBrush)(ID2D1RenderTargetImpl* This,
                                          const void* rgProps,
                                          const D2D1_BRUSH_PROPERTIES* props,
                                          void* stops, ID2D1BrushImpl** brush);
    HRESULT (*CreateCompatibleRenderTarget)(ID2D1RenderTargetImpl* This,
                                             const D2D1_SIZE_F* size,
                                             const D2D1_SIZE_U* pixelSize,
                                             const D2D1_PIXEL_FORMAT* fmt,
                                             UINT options,
                                             ID2D1RenderTargetImpl** out);
    HRESULT (*CreateLayer)   (ID2D1RenderTargetImpl* This, const D2D1_SIZE_F* size, void** out);
    HRESULT (*CreateMesh)    (ID2D1RenderTargetImpl* This, void** out);
    void    (*DrawLine)      (ID2D1RenderTargetImpl* This, D2D1_POINT_2F p0,
                              D2D1_POINT_2F p1, ID2D1BrushImpl* brush,
                              FLOAT strokeWidth, void* strokeStyle);
    void    (*DrawRectangle) (ID2D1RenderTargetImpl* This, const D2D1_RECT_F* rect,
                              ID2D1BrushImpl* brush, FLOAT strokeWidth, void* strokeStyle);
    void    (*FillRectangle) (ID2D1RenderTargetImpl* This, const D2D1_RECT_F* rect,
                              ID2D1BrushImpl* brush);
    void    (*DrawRoundedRectangle)(ID2D1RenderTargetImpl* This,
                                     const D2D1_ROUNDED_RECT* rr,
                                     ID2D1BrushImpl* brush, FLOAT sw, void* style);
    void    (*FillRoundedRectangle)(ID2D1RenderTargetImpl* This,
                                     const D2D1_ROUNDED_RECT* rr,
                                     ID2D1BrushImpl* brush);
    void    (*DrawEllipse)   (ID2D1RenderTargetImpl* This, const D2D1_ELLIPSE* e,
                              ID2D1BrushImpl* brush, FLOAT sw, void* style);
    void    (*FillEllipse)   (ID2D1RenderTargetImpl* This, const D2D1_ELLIPSE* e,
                              ID2D1BrushImpl* brush);
    void    (*DrawGeometry)  (ID2D1RenderTargetImpl* This, ID2D1GeometryImpl* g,
                              ID2D1BrushImpl* brush, FLOAT sw, void* style);
    void    (*FillGeometry)  (ID2D1RenderTargetImpl* This, ID2D1GeometryImpl* g,
                              ID2D1BrushImpl* brush, ID2D1BrushImpl* opacityBrush);
    void    (*FillMesh)      (ID2D1RenderTargetImpl* This, void* mesh, ID2D1BrushImpl* brush);
    void    (*FillOpacityMask)(ID2D1RenderTargetImpl* This, ID2D1BitmapImpl* mask,
                                ID2D1BrushImpl* brush, UINT content,
                                const D2D1_RECT_F* dst, const D2D1_RECT_F* src);
    void    (*DrawBitmap)    (ID2D1RenderTargetImpl* This, ID2D1BitmapImpl* bm,
                              const D2D1_RECT_F* dst, FLOAT opacity, UINT mode,
                              const D2D1_RECT_F* src);
    void    (*DrawText)      (ID2D1RenderTargetImpl* This, const WCHAR* text,
                              UINT len, void* textFormat, const D2D1_RECT_F* layout,
                              ID2D1BrushImpl* brush, UINT opts, UINT measuring);
    void    (*DrawTextLayout)(ID2D1RenderTargetImpl* This, D2D1_POINT_2F origin,
                              void* textLayout, ID2D1BrushImpl* brush, UINT opts);
    void    (*DrawGlyphRun)  (ID2D1RenderTargetImpl* This, D2D1_POINT_2F origin,
                              const void* glyphRun, ID2D1BrushImpl* brush, UINT mode);
    void    (*SetTransform)  (ID2D1RenderTargetImpl* This, const D2D1_MATRIX_3X2_F* m);
    void    (*GetTransform)  (ID2D1RenderTargetImpl* This, D2D1_MATRIX_3X2_F* m);
    void    (*SetAntialiasMode)(ID2D1RenderTargetImpl* This, UINT mode);
    UINT    (*GetAntialiasMode)(ID2D1RenderTargetImpl* This);
    void    (*SetTextAntialiasMode)(ID2D1RenderTargetImpl* This, UINT mode);
    UINT    (*GetTextAntialiasMode)(ID2D1RenderTargetImpl* This);
    void    (*SetTextRenderingParams)(ID2D1RenderTargetImpl* This, void* params);
    void    (*GetTextRenderingParams)(ID2D1RenderTargetImpl* This, void** params);
    void    (*SetTags)       (ID2D1RenderTargetImpl* This, UINT64 t1, UINT64 t2);
    void    (*GetTags)       (ID2D1RenderTargetImpl* This, UINT64* t1, UINT64* t2);
    void    (*PushLayer)     (ID2D1RenderTargetImpl* This, const void* params, void* layer);
    void    (*PopLayer)      (ID2D1RenderTargetImpl* This);
    HRESULT (*Flush)         (ID2D1RenderTargetImpl* This, UINT64* tag1, UINT64* tag2);
    void    (*SaveDrawingState)(ID2D1RenderTargetImpl* This, void* block);
    void    (*RestoreDrawingState)(ID2D1RenderTargetImpl* This, void* block);
    void    (*PushAxisAlignedClip)(ID2D1RenderTargetImpl* This, const D2D1_RECT_F* clip, UINT mode);
    void    (*PopAxisAlignedClip)(ID2D1RenderTargetImpl* This);
    void    (*Clear)         (ID2D1RenderTargetImpl* This, const D2D1_COLOR_F* color);
    void    (*BeginDraw)     (ID2D1RenderTargetImpl* This);
    HRESULT (*EndDraw)       (ID2D1RenderTargetImpl* This, UINT64* tag1, UINT64* tag2);
    D2D1_PIXEL_FORMAT (*GetPixelFormat)(ID2D1RenderTargetImpl* This);
    void    (*SetDpi)        (ID2D1RenderTargetImpl* This, FLOAT dpiX, FLOAT dpiY);
    void    (*GetDpi)        (ID2D1RenderTargetImpl* This, FLOAT* dpiX, FLOAT* dpiY);
    D2D1_SIZE_F (*GetSize)   (ID2D1RenderTargetImpl* This);
    D2D1_SIZE_U (*GetPixelSize)(ID2D1RenderTargetImpl* This);
    UINT    (*GetMaximumBitmapSize)(ID2D1RenderTargetImpl* This);
    BOOL    (*IsSupported)   (ID2D1RenderTargetImpl* This, const D2D1_RENDER_TARGET_PROPERTIES* p);
} ID2D1RenderTargetVtbl;

static HRESULT Rt_QueryInterface(ID2D1RenderTargetImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Rt_AddRef (ID2D1RenderTargetImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Rt_Release(ID2D1RenderTargetImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static void Rt_GetFactory(ID2D1RenderTargetImpl* T, void** f) { (void)T; if (f) *f = &g_factories[0]; }

static HRESULT Rt_CreateBitmap(ID2D1RenderTargetImpl* T, D2D1_SIZE_U size,
                                LPCVOID src, UINT pitch,
                                const D2D1_BITMAP_PROPERTIES* p,
                                ID2D1BitmapImpl** bm) {
    (void)T; (void)src; (void)pitch;
    if (!bm) return E_POINTER;
    ID2D1BitmapImpl* b = alloc_bitmap(size, p);
    if (!b) { *bm = 0; return E_OUTOFMEMORY; }
    *bm = b;
    return S_OK;
}
static HRESULT Rt_CreateBitmapFromWicBitmap(ID2D1RenderTargetImpl* T, void* wic,
                                             const D2D1_BITMAP_PROPERTIES* p,
                                             ID2D1BitmapImpl** bm) {
    (void)T; (void)wic;
    if (!bm) return E_POINTER;
    D2D1_SIZE_U size = { 64, 64 };
    ID2D1BitmapImpl* b = alloc_bitmap(size, p);
    if (!b) { *bm = 0; return E_OUTOFMEMORY; }
    *bm = b;
    return S_OK;
}
static HRESULT Rt_CreateSharedBitmap(ID2D1RenderTargetImpl* T, REFIID r, void* data,
                                      const D2D1_BITMAP_PROPERTIES* p, ID2D1BitmapImpl** bm) {
    (void)T; (void)r; (void)data;
    if (!bm) return E_POINTER;
    D2D1_SIZE_U size = { 64, 64 };
    ID2D1BitmapImpl* b = alloc_bitmap(size, p);
    if (!b) { *bm = 0; return E_OUTOFMEMORY; }
    *bm = b;
    return S_OK;
}
static HRESULT Rt_CreateBitmapBrush(ID2D1RenderTargetImpl* T, ID2D1BitmapImpl* bm,
                                     void* bp, const D2D1_BRUSH_PROPERTIES* p,
                                     ID2D1BrushImpl** br) {
    (void)T; (void)bm; (void)bp;
    if (!br) return E_POINTER;
    ID2D1BrushImpl* b = alloc_brush(D2D_BR_BITMAP, 0, p);
    if (!b) { *br = 0; return E_OUTOFMEMORY; }
    *br = b;
    return S_OK;
}
static HRESULT Rt_CreateSolidColorBrush(ID2D1RenderTargetImpl* T,
                                         const D2D1_COLOR_F* color,
                                         const D2D1_BRUSH_PROPERTIES* p,
                                         ID2D1BrushImpl** br) {
    (void)T;
    if (!br) return E_POINTER;
    ID2D1BrushImpl* b = alloc_brush(D2D_BR_SOLID, color, p);
    if (!b) { *br = 0; return E_OUTOFMEMORY; }
    *br = b;
    return S_OK;
}
static HRESULT Rt_CreateGradientStopCollection(ID2D1RenderTargetImpl* T,
                                                const void* stops, UINT count,
                                                UINT gamma, UINT ext, void** out) {
    (void)T; (void)stops; (void)count; (void)gamma; (void)ext;
    if (out) *out = (void*)0x1; // handle fake
    return S_OK;
}
static HRESULT Rt_CreateLinearGradientBrush(ID2D1RenderTargetImpl* T, const void* lg,
                                             const D2D1_BRUSH_PROPERTIES* p,
                                             void* stops, ID2D1BrushImpl** br) {
    (void)T; (void)lg; (void)stops;
    if (!br) return E_POINTER;
    ID2D1BrushImpl* b = alloc_brush(D2D_BR_LINEAR, 0, p);
    if (!b) { *br = 0; return E_OUTOFMEMORY; }
    *br = b;
    return S_OK;
}
static HRESULT Rt_CreateRadialGradientBrush(ID2D1RenderTargetImpl* T, const void* rg,
                                             const D2D1_BRUSH_PROPERTIES* p,
                                             void* stops, ID2D1BrushImpl** br) {
    (void)T; (void)rg; (void)stops;
    if (!br) return E_POINTER;
    ID2D1BrushImpl* b = alloc_brush(D2D_BR_RADIAL, 0, p);
    if (!b) { *br = 0; return E_OUTOFMEMORY; }
    *br = b;
    return S_OK;
}
static HRESULT Rt_CreateCompatibleRenderTarget(ID2D1RenderTargetImpl* T,
                                                const D2D1_SIZE_F* sz,
                                                const D2D1_SIZE_U* psz,
                                                const D2D1_PIXEL_FORMAT* fmt,
                                                UINT opts, ID2D1RenderTargetImpl** out) {
    (void)T; (void)sz; (void)psz; (void)fmt; (void)opts;
    if (!out) return E_POINTER;
    for (int i = 0; i < MAX_RENDER_TARGETS; i++) {
        if (!g_render_targets[i].used) {
            mem_zero(&g_render_targets[i], sizeof(g_render_targets[i]));
            g_render_targets[i].used     = 1;
            g_render_targets[i].refCount = 1;
            g_render_targets[i].lpVtbl   = T->lpVtbl;       // mesma vtable
            g_render_targets[i].pixelSize = psz ? *psz : (D2D1_SIZE_U){ 1024, 768 };
            g_render_targets[i].transform._11 = 1.0f;
            g_render_targets[i].transform._22 = 1.0f;
            *out = &g_render_targets[i];
            return S_OK;
        }
    }
    *out = 0;
    return E_OUTOFMEMORY;
}
static HRESULT Rt_CreateLayer(ID2D1RenderTargetImpl* T, const D2D1_SIZE_F* sz, void** out) {
    (void)T; (void)sz;
    if (out) *out = (void*)0x1;
    return S_OK;
}
static HRESULT Rt_CreateMesh(ID2D1RenderTargetImpl* T, void** out) {
    (void)T;
    if (out) *out = (void*)0x1;
    return S_OK;
}

// Os "draw" / "fill" sao no-ops aqui — sem rasterizador. Apps que chamam
// estes metodos rodam sem crash; o conteudo visivel continua via win32k/FB.
static void Rt_DrawLine(ID2D1RenderTargetImpl* T, D2D1_POINT_2F p0, D2D1_POINT_2F p1,
                        ID2D1BrushImpl* b, FLOAT sw, void* st) {
    (void)T; (void)p0; (void)p1; (void)b; (void)sw; (void)st;
}
static void Rt_DrawRectangle(ID2D1RenderTargetImpl* T, const D2D1_RECT_F* r,
                              ID2D1BrushImpl* b, FLOAT sw, void* st) {
    (void)T; (void)r; (void)b; (void)sw; (void)st;
}
static void Rt_FillRectangle(ID2D1RenderTargetImpl* T, const D2D1_RECT_F* r,
                              ID2D1BrushImpl* b) {
    (void)T; (void)r; (void)b;
}
static void Rt_DrawRoundedRectangle(ID2D1RenderTargetImpl* T, const D2D1_ROUNDED_RECT* rr,
                                     ID2D1BrushImpl* b, FLOAT sw, void* st) {
    (void)T; (void)rr; (void)b; (void)sw; (void)st;
}
static void Rt_FillRoundedRectangle(ID2D1RenderTargetImpl* T, const D2D1_ROUNDED_RECT* rr,
                                     ID2D1BrushImpl* b) {
    (void)T; (void)rr; (void)b;
}
static void Rt_DrawEllipse(ID2D1RenderTargetImpl* T, const D2D1_ELLIPSE* e,
                            ID2D1BrushImpl* b, FLOAT sw, void* st) {
    (void)T; (void)e; (void)b; (void)sw; (void)st;
}
static void Rt_FillEllipse(ID2D1RenderTargetImpl* T, const D2D1_ELLIPSE* e,
                            ID2D1BrushImpl* b) {
    (void)T; (void)e; (void)b;
}
static void Rt_DrawGeometry(ID2D1RenderTargetImpl* T, ID2D1GeometryImpl* g,
                             ID2D1BrushImpl* b, FLOAT sw, void* st) {
    (void)T; (void)g; (void)b; (void)sw; (void)st;
}
static void Rt_FillGeometry(ID2D1RenderTargetImpl* T, ID2D1GeometryImpl* g,
                             ID2D1BrushImpl* b, ID2D1BrushImpl* ob) {
    (void)T; (void)g; (void)b; (void)ob;
}
static void Rt_FillMesh(ID2D1RenderTargetImpl* T, void* m, ID2D1BrushImpl* b) {
    (void)T; (void)m; (void)b;
}
static void Rt_FillOpacityMask(ID2D1RenderTargetImpl* T, ID2D1BitmapImpl* m,
                                ID2D1BrushImpl* b, UINT c,
                                const D2D1_RECT_F* d, const D2D1_RECT_F* s) {
    (void)T; (void)m; (void)b; (void)c; (void)d; (void)s;
}
static void Rt_DrawBitmap(ID2D1RenderTargetImpl* T, ID2D1BitmapImpl* bm,
                           const D2D1_RECT_F* d, FLOAT op, UINT mode,
                           const D2D1_RECT_F* s) {
    (void)T; (void)bm; (void)d; (void)op; (void)mode; (void)s;
}
static void Rt_DrawText(ID2D1RenderTargetImpl* T, const WCHAR* text, UINT len,
                         void* tf, const D2D1_RECT_F* layout, ID2D1BrushImpl* b,
                         UINT opts, UINT meas) {
    (void)T; (void)text; (void)len; (void)tf; (void)layout; (void)b; (void)opts; (void)meas;
}
static void Rt_DrawTextLayout(ID2D1RenderTargetImpl* T, D2D1_POINT_2F origin,
                               void* tl, ID2D1BrushImpl* b, UINT opts) {
    (void)T; (void)origin; (void)tl; (void)b; (void)opts;
}
static void Rt_DrawGlyphRun(ID2D1RenderTargetImpl* T, D2D1_POINT_2F origin,
                             const void* gr, ID2D1BrushImpl* b, UINT mode) {
    (void)T; (void)origin; (void)gr; (void)b; (void)mode;
}

static void Rt_SetTransform(ID2D1RenderTargetImpl* T, const D2D1_MATRIX_3X2_F* m) {
    if (m) T->transform = *m;
}
static void Rt_GetTransform(ID2D1RenderTargetImpl* T, D2D1_MATRIX_3X2_F* m) {
    if (m) *m = T->transform;
}
static void Rt_SetAntialiasMode(ID2D1RenderTargetImpl* T, UINT m) { (void)T; (void)m; }
static UINT Rt_GetAntialiasMode(ID2D1RenderTargetImpl* T)         { (void)T; return 0; }
static void Rt_SetTextAntialiasMode(ID2D1RenderTargetImpl* T, UINT m) { (void)T; (void)m; }
static UINT Rt_GetTextAntialiasMode(ID2D1RenderTargetImpl* T)     { (void)T; return 0; }
static void Rt_SetTextRenderingParams(ID2D1RenderTargetImpl* T, void* p) { (void)T; (void)p; }
static void Rt_GetTextRenderingParams(ID2D1RenderTargetImpl* T, void** p){ (void)T; if (p) *p = 0; }
static void Rt_SetTags(ID2D1RenderTargetImpl* T, UINT64 t1, UINT64 t2) { (void)T; (void)t1; (void)t2; }
static void Rt_GetTags(ID2D1RenderTargetImpl* T, UINT64* t1, UINT64* t2) {
    (void)T; if (t1) *t1 = 0; if (t2) *t2 = 0;
}
static void Rt_PushLayer(ID2D1RenderTargetImpl* T, const void* p, void* l) { (void)T; (void)p; (void)l; }
static void Rt_PopLayer(ID2D1RenderTargetImpl* T)                         { (void)T; }
static HRESULT Rt_Flush(ID2D1RenderTargetImpl* T, UINT64* t1, UINT64* t2) {
    (void)T; if (t1) *t1 = 0; if (t2) *t2 = 0; return S_OK;
}
static void Rt_SaveDrawingState(ID2D1RenderTargetImpl* T, void* b) { (void)T; (void)b; }
static void Rt_RestoreDrawingState(ID2D1RenderTargetImpl* T, void* b) { (void)T; (void)b; }
static void Rt_PushAxisAlignedClip(ID2D1RenderTargetImpl* T, const D2D1_RECT_F* c, UINT m) {
    (void)T; (void)c; (void)m;
}
static void Rt_PopAxisAlignedClip(ID2D1RenderTargetImpl* T) { (void)T; }
static void Rt_Clear(ID2D1RenderTargetImpl* T, const D2D1_COLOR_F* c) {
    (void)T; (void)c;
}
static void Rt_BeginDraw(ID2D1RenderTargetImpl* T) { T->drawing = 1; }
static HRESULT Rt_EndDraw(ID2D1RenderTargetImpl* T, UINT64* t1, UINT64* t2) {
    T->drawing = 0;
    if (t1) *t1 = 0; if (t2) *t2 = 0;
    return S_OK;
}
static D2D1_PIXEL_FORMAT Rt_GetPixelFormat(ID2D1RenderTargetImpl* T) {
    (void)T;
    D2D1_PIXEL_FORMAT f = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
    return f;
}
static void Rt_SetDpi(ID2D1RenderTargetImpl* T, FLOAT dx, FLOAT dy) { (void)T; (void)dx; (void)dy; }
static void Rt_GetDpi(ID2D1RenderTargetImpl* T, FLOAT* dx, FLOAT* dy) {
    (void)T; if (dx) *dx = 96.0f; if (dy) *dy = 96.0f;
}
static D2D1_SIZE_F Rt_GetSize(ID2D1RenderTargetImpl* T) {
    D2D1_SIZE_F s = { (FLOAT)T->pixelSize.width, (FLOAT)T->pixelSize.height };
    return s;
}
static D2D1_SIZE_U Rt_GetPixelSize(ID2D1RenderTargetImpl* T) { return T->pixelSize; }
static UINT Rt_GetMaximumBitmapSize(ID2D1RenderTargetImpl* T) { (void)T; return 16384; }
static BOOL Rt_IsSupported(ID2D1RenderTargetImpl* T, const D2D1_RENDER_TARGET_PROPERTIES* p) {
    (void)T; (void)p; return 1;
}

static const ID2D1RenderTargetVtbl g_rtVtbl = {
    Rt_QueryInterface, Rt_AddRef, Rt_Release,
    Rt_GetFactory,
    Rt_CreateBitmap, Rt_CreateBitmapFromWicBitmap, Rt_CreateSharedBitmap,
    Rt_CreateBitmapBrush, Rt_CreateSolidColorBrush,
    Rt_CreateGradientStopCollection,
    Rt_CreateLinearGradientBrush, Rt_CreateRadialGradientBrush,
    Rt_CreateCompatibleRenderTarget,
    Rt_CreateLayer, Rt_CreateMesh,
    Rt_DrawLine, Rt_DrawRectangle, Rt_FillRectangle,
    Rt_DrawRoundedRectangle, Rt_FillRoundedRectangle,
    Rt_DrawEllipse, Rt_FillEllipse,
    Rt_DrawGeometry, Rt_FillGeometry,
    Rt_FillMesh, Rt_FillOpacityMask,
    Rt_DrawBitmap,
    Rt_DrawText, Rt_DrawTextLayout, Rt_DrawGlyphRun,
    Rt_SetTransform, Rt_GetTransform,
    Rt_SetAntialiasMode, Rt_GetAntialiasMode,
    Rt_SetTextAntialiasMode, Rt_GetTextAntialiasMode,
    Rt_SetTextRenderingParams, Rt_GetTextRenderingParams,
    Rt_SetTags, Rt_GetTags,
    Rt_PushLayer, Rt_PopLayer,
    Rt_Flush, Rt_SaveDrawingState, Rt_RestoreDrawingState,
    Rt_PushAxisAlignedClip, Rt_PopAxisAlignedClip,
    Rt_Clear, Rt_BeginDraw, Rt_EndDraw,
    Rt_GetPixelFormat, Rt_SetDpi, Rt_GetDpi,
    Rt_GetSize, Rt_GetPixelSize, Rt_GetMaximumBitmapSize, Rt_IsSupported,
};

static ID2D1RenderTargetImpl* alloc_rt(HWND hwnd, D2D1_SIZE_U size) {
    for (int i = 0; i < MAX_RENDER_TARGETS; i++) {
        if (!g_render_targets[i].used) {
            mem_zero(&g_render_targets[i], sizeof(g_render_targets[i]));
            g_render_targets[i].used     = 1;
            g_render_targets[i].refCount = 1;
            g_render_targets[i].lpVtbl   = &g_rtVtbl;
            g_render_targets[i].hwnd     = hwnd;
            g_render_targets[i].pixelSize = size;
            g_render_targets[i].transform._11 = 1.0f;
            g_render_targets[i].transform._22 = 1.0f;
            return &g_render_targets[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: ID2D1Factory. Cria render targets e objetos derivados de fabrica
//  (geometrias retangulo/ellipse/rounded/path).
// ============================================================================
typedef struct ID2D1FactoryVtbl {
    HRESULT (*QueryInterface)(ID2D1FactoryImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID2D1FactoryImpl* This);
    ULONG   (*Release)       (ID2D1FactoryImpl* This);
    HRESULT (*ReloadSystemMetrics)(ID2D1FactoryImpl* This);
    void    (*GetDesktopDpi) (ID2D1FactoryImpl* This, FLOAT* dpiX, FLOAT* dpiY);
    HRESULT (*CreateRectangleGeometry)(ID2D1FactoryImpl* This, const D2D1_RECT_F* rect,
                                         ID2D1GeometryImpl** out);
    HRESULT (*CreateRoundedRectangleGeometry)(ID2D1FactoryImpl* This,
                                                const D2D1_ROUNDED_RECT* rr,
                                                ID2D1GeometryImpl** out);
    HRESULT (*CreateEllipseGeometry)(ID2D1FactoryImpl* This, const D2D1_ELLIPSE* e,
                                       ID2D1GeometryImpl** out);
    HRESULT (*CreateGeometryGroup)(ID2D1FactoryImpl* This, UINT fill,
                                     ID2D1GeometryImpl** geoms, UINT count,
                                     ID2D1GeometryImpl** out);
    HRESULT (*CreateTransformedGeometry)(ID2D1FactoryImpl* This,
                                          ID2D1GeometryImpl* base,
                                          const D2D1_MATRIX_3X2_F* m,
                                          ID2D1GeometryImpl** out);
    HRESULT (*CreatePathGeometry)(ID2D1FactoryImpl* This, ID2D1GeometryImpl** out);
    HRESULT (*CreateStrokeStyle)(ID2D1FactoryImpl* This, const void* props,
                                  const FLOAT* dashes, UINT count, void** out);
    HRESULT (*CreateDrawingStateBlock)(ID2D1FactoryImpl* This,
                                         const void* desc, void* params, void** out);
    HRESULT (*CreateWicBitmapRenderTarget)(ID2D1FactoryImpl* This, void* wic,
                                             const D2D1_RENDER_TARGET_PROPERTIES* props,
                                             ID2D1RenderTargetImpl** out);
    HRESULT (*CreateHwndRenderTarget)(ID2D1FactoryImpl* This,
                                        const D2D1_RENDER_TARGET_PROPERTIES* rtProps,
                                        const D2D1_HWND_RENDER_TARGET_PROPERTIES* hwndProps,
                                        ID2D1RenderTargetImpl** out);
    HRESULT (*CreateDxgiSurfaceRenderTarget)(ID2D1FactoryImpl* This, void* surface,
                                                const D2D1_RENDER_TARGET_PROPERTIES* props,
                                                ID2D1RenderTargetImpl** out);
    HRESULT (*CreateDCRenderTarget)(ID2D1FactoryImpl* This,
                                      const D2D1_RENDER_TARGET_PROPERTIES* props,
                                      ID2D1RenderTargetImpl** out);
} ID2D1FactoryVtbl;

static HRESULT Fa_QueryInterface(ID2D1FactoryImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Fa_AddRef (ID2D1FactoryImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Fa_Release(ID2D1FactoryImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Fa_ReloadSystemMetrics(ID2D1FactoryImpl* T) { (void)T; return S_OK; }
static void Fa_GetDesktopDpi(ID2D1FactoryImpl* T, FLOAT* dx, FLOAT* dy) {
    (void)T; if (dx) *dx = 96.0f; if (dy) *dy = 96.0f;
}
static HRESULT Fa_CreateRectangleGeometry(ID2D1FactoryImpl* T, const D2D1_RECT_F* r,
                                           ID2D1GeometryImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    ID2D1GeometryImpl* g = alloc_geometry(D2D_GEO_RECT);
    if (!g) { *out = 0; return E_OUTOFMEMORY; }
    if (r) g->rect = *r;
    *out = g;
    return S_OK;
}
static HRESULT Fa_CreateRoundedRectangleGeometry(ID2D1FactoryImpl* T,
                                                  const D2D1_ROUNDED_RECT* rr,
                                                  ID2D1GeometryImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    ID2D1GeometryImpl* g = alloc_geometry(D2D_GEO_ROUNDED);
    if (!g) { *out = 0; return E_OUTOFMEMORY; }
    if (rr) g->rounded = *rr;
    *out = g;
    return S_OK;
}
static HRESULT Fa_CreateEllipseGeometry(ID2D1FactoryImpl* T, const D2D1_ELLIPSE* e,
                                         ID2D1GeometryImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    ID2D1GeometryImpl* g = alloc_geometry(D2D_GEO_ELLIPSE);
    if (!g) { *out = 0; return E_OUTOFMEMORY; }
    if (e) g->ellipse = *e;
    *out = g;
    return S_OK;
}
static HRESULT Fa_CreateGeometryGroup(ID2D1FactoryImpl* T, UINT fill,
                                       ID2D1GeometryImpl** g, UINT count,
                                       ID2D1GeometryImpl** out) {
    (void)T; (void)fill; (void)g; (void)count;
    if (!out) return E_POINTER;
    ID2D1GeometryImpl* gg = alloc_geometry(D2D_GEO_PATH);
    if (!gg) { *out = 0; return E_OUTOFMEMORY; }
    *out = gg;
    return S_OK;
}
static HRESULT Fa_CreateTransformedGeometry(ID2D1FactoryImpl* T, ID2D1GeometryImpl* base,
                                             const D2D1_MATRIX_3X2_F* m,
                                             ID2D1GeometryImpl** out) {
    (void)T; (void)base; (void)m;
    if (!out) return E_POINTER;
    ID2D1GeometryImpl* g = alloc_geometry(D2D_GEO_PATH);
    if (!g) { *out = 0; return E_OUTOFMEMORY; }
    *out = g;
    return S_OK;
}
static HRESULT Fa_CreatePathGeometry(ID2D1FactoryImpl* T, ID2D1GeometryImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    ID2D1GeometryImpl* g = alloc_geometry(D2D_GEO_PATH);
    if (!g) { *out = 0; return E_OUTOFMEMORY; }
    *out = g;
    return S_OK;
}
static HRESULT Fa_CreateStrokeStyle(ID2D1FactoryImpl* T, const void* p, const FLOAT* d,
                                     UINT c, void** out) {
    (void)T; (void)p; (void)d; (void)c;
    if (out) *out = (void*)0x1;
    return S_OK;
}
static HRESULT Fa_CreateDrawingStateBlock(ID2D1FactoryImpl* T, const void* d, void* p, void** out) {
    (void)T; (void)d; (void)p;
    if (out) *out = (void*)0x1;
    return S_OK;
}
static HRESULT Fa_CreateWicBitmapRenderTarget(ID2D1FactoryImpl* T, void* wic,
                                                const D2D1_RENDER_TARGET_PROPERTIES* p,
                                                ID2D1RenderTargetImpl** out) {
    (void)T; (void)wic; (void)p;
    if (!out) return E_POINTER;
    D2D1_SIZE_U size = { 1024, 768 };
    ID2D1RenderTargetImpl* rt = alloc_rt(0, size);
    if (!rt) { *out = 0; return E_OUTOFMEMORY; }
    *out = rt;
    return S_OK;
}
static HRESULT Fa_CreateHwndRenderTarget(ID2D1FactoryImpl* T,
                                          const D2D1_RENDER_TARGET_PROPERTIES* rtp,
                                          const D2D1_HWND_RENDER_TARGET_PROPERTIES* hp,
                                          ID2D1RenderTargetImpl** out) {
    (void)T; (void)rtp;
    if (!out) return E_POINTER;
    HWND hwnd = hp ? hp->hwnd : 0;
    D2D1_SIZE_U size = hp ? hp->pixelSize : (D2D1_SIZE_U){ 1024, 768 };
    if (size.width  == 0) size.width  = 1024;
    if (size.height == 0) size.height = 768;
    ID2D1RenderTargetImpl* rt = alloc_rt(hwnd, size);
    if (!rt) { *out = 0; return E_OUTOFMEMORY; }
    *out = rt;
    return S_OK;
}
static HRESULT Fa_CreateDxgiSurfaceRenderTarget(ID2D1FactoryImpl* T, void* surf,
                                                  const D2D1_RENDER_TARGET_PROPERTIES* p,
                                                  ID2D1RenderTargetImpl** out) {
    (void)T; (void)surf; (void)p;
    if (!out) return E_POINTER;
    D2D1_SIZE_U size = { 1024, 768 };
    ID2D1RenderTargetImpl* rt = alloc_rt(0, size);
    if (!rt) { *out = 0; return E_OUTOFMEMORY; }
    *out = rt;
    return S_OK;
}
static HRESULT Fa_CreateDCRenderTarget(ID2D1FactoryImpl* T,
                                        const D2D1_RENDER_TARGET_PROPERTIES* p,
                                        ID2D1RenderTargetImpl** out) {
    (void)T; (void)p;
    if (!out) return E_POINTER;
    D2D1_SIZE_U size = { 1024, 768 };
    ID2D1RenderTargetImpl* rt = alloc_rt(0, size);
    if (!rt) { *out = 0; return E_OUTOFMEMORY; }
    *out = rt;
    return S_OK;
}

static const ID2D1FactoryVtbl g_factoryVtbl = {
    Fa_QueryInterface, Fa_AddRef, Fa_Release,
    Fa_ReloadSystemMetrics, Fa_GetDesktopDpi,
    Fa_CreateRectangleGeometry, Fa_CreateRoundedRectangleGeometry,
    Fa_CreateEllipseGeometry, Fa_CreateGeometryGroup,
    Fa_CreateTransformedGeometry, Fa_CreatePathGeometry,
    Fa_CreateStrokeStyle, Fa_CreateDrawingStateBlock,
    Fa_CreateWicBitmapRenderTarget, Fa_CreateHwndRenderTarget,
    Fa_CreateDxgiSurfaceRenderTarget, Fa_CreateDCRenderTarget,
};

static ID2D1FactoryImpl* alloc_factory(UINT type) {
    for (int i = 0; i < MAX_FACTORIES; i++) {
        if (!g_factories[i].used) {
            mem_zero(&g_factories[i], sizeof(g_factories[i]));
            g_factories[i].used        = 1;
            g_factories[i].refCount    = 1;
            g_factories[i].lpVtbl      = &g_factoryVtbl;
            g_factories[i].factoryType = type;
            return &g_factories[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry point exportado — assinatura BATE com a d2d1.dll real do Windows.
//  D2D1CreateFactory(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**)
// ============================================================================

// D2D1_FACTORY_OPTIONS — opcional (debug level).
typedef struct D2D1_FACTORY_OPTIONS { UINT debugLevel; } D2D1_FACTORY_OPTIONS;

__declspec(dllexport) HRESULT D2D1CreateFactory(UINT factoryType, REFIID riid,
                                                 const D2D1_FACTORY_OPTIONS* options,
                                                 void** ppFactory) {
    (void)riid; (void)options;
    if (!ppFactory) return E_POINTER;
    ID2D1FactoryImpl* f = alloc_factory(factoryType);
    if (!f) { *ppFactory = 0; return E_OUTOFMEMORY; }
    *ppFactory = f;
    return S_OK;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
