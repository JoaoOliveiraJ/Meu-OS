// d3d11.dll  —  reimplementacao minima do Direct3D 11 (FASE 9.8).
//
// Acima do DXGI 1.x (FASE 9.7+), o D3D11 e a API grafica PRIMARIA do Windows 8
// em diante. No real, d3d11.dll vive em RING 3, fala com d3d11_<vendor>.dll
// (driver UMD) e este por sua vez ataca dxgkrnl.sys via syscalls. Aqui no
// MeuOS nao temos rasterizador real (BasicDisplay e KMD-only para 2D); este
// stub se limita ao ABI COM completo de ID3D11Device + ID3D11DeviceContext +
// recursos (buffers/textures/views/shaders/states). TUDO retorna S_OK ou
// E_OUTOFMEMORY quando o pool acaba.
//
// COM ABI (estilo NT/Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg
// "this". Em ABI ms_abi (x86_64-windows-gnu) os parametros entram em
// RCX,RDX,R8,R9 (e essa a ABI que o zig cc gera com -target windows-gnu).
//
// Em DLLs ring 3 do MeuOS nao temos VirtualAlloc/HeapAlloc Win32 reais;
// por isso usamos POOLS ESTATICOS de objetos. Como sao apenas metadados,
// os pools sao pequenos: 4 devices, 16 contexts, 64 resources.
//
// IMAGE BASE: 0x4500000 — sobreposicao com PMM_BASE (0x4000000). Para
// evitar colisao usamos --dynamicbase no build (.reloc), entao o loader
// pode realocar para qualquer endereco virtual livre. Este e o mesmo
// mecanismo usado por hello32.exe e drivers .sys.

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

#define S_OK                         0x00000000L
#define S_FALSE                      0x00000001L
#define E_NOTIMPL                    0x80004001L
#define E_NOINTERFACE                0x80004002L
#define E_POINTER                    0x80004003L
#define E_FAIL                       0x80004005L
#define E_INVALIDARG                 0x80070057L
#define E_OUTOFMEMORY                0x8007000EL

// D3D_FEATURE_LEVEL — qual a "feature level" o app esta pedindo. Os mais
// usados: 9_1..9_3 (DX9-class), 10_0/10_1 (DX10), 11_0/11_1 (DX11).
#define D3D_FEATURE_LEVEL_9_1            0x9100
#define D3D_FEATURE_LEVEL_9_2            0x9200
#define D3D_FEATURE_LEVEL_9_3            0x9300
#define D3D_FEATURE_LEVEL_10_0           0xa000
#define D3D_FEATURE_LEVEL_10_1           0xa100
#define D3D_FEATURE_LEVEL_11_0           0xb000
#define D3D_FEATURE_LEVEL_11_1           0xb100

// D3D_DRIVER_TYPE — hardware/reference/null/software/warp.
#define D3D_DRIVER_TYPE_UNKNOWN          0
#define D3D_DRIVER_TYPE_HARDWARE         1
#define D3D_DRIVER_TYPE_REFERENCE        2
#define D3D_DRIVER_TYPE_NULL             3
#define D3D_DRIVER_TYPE_SOFTWARE         4
#define D3D_DRIVER_TYPE_WARP             5

// D3D11_USAGE — politica de upload/download do recurso.
#define D3D11_USAGE_DEFAULT              0
#define D3D11_USAGE_IMMUTABLE            1
#define D3D11_USAGE_DYNAMIC              2
#define D3D11_USAGE_STAGING              3

// D3D11_BIND_FLAG — onde o recurso pode ser amarrado no pipeline.
#define D3D11_BIND_VERTEX_BUFFER         0x1L
#define D3D11_BIND_INDEX_BUFFER          0x2L
#define D3D11_BIND_CONSTANT_BUFFER       0x4L
#define D3D11_BIND_SHADER_RESOURCE       0x8L
#define D3D11_BIND_STREAM_OUTPUT         0x10L
#define D3D11_BIND_RENDER_TARGET         0x20L
#define D3D11_BIND_DEPTH_STENCIL         0x40L

// D3D11_PRIMITIVE_TOPOLOGY — como interpretar a IA stream.
#define D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED     0
#define D3D11_PRIMITIVE_TOPOLOGY_POINTLIST     1
#define D3D11_PRIMITIVE_TOPOLOGY_LINELIST      2
#define D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST  4

// Constantes de slot maximos (D3D11 hardware caps).
#define D3D11_VS_INPUT_REGISTER_COUNT          32
#define D3D11_PS_OUTPUT_REGISTER_COUNT         8
#define D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX 15

// ============================================================================
//  Estruturas D3D11 publicas (subset minimo necessario).
// ============================================================================
#pragma pack(push, 4)

typedef struct D3D11_BUFFER_DESC {
    UINT  ByteWidth;
    UINT  Usage;
    UINT  BindFlags;
    UINT  CPUAccessFlags;
    UINT  MiscFlags;
    UINT  StructureByteStride;
} D3D11_BUFFER_DESC;

typedef struct D3D11_SUBRESOURCE_DATA {
    const void* pSysMem;
    UINT        SysMemPitch;
    UINT        SysMemSlicePitch;
} D3D11_SUBRESOURCE_DATA;

typedef struct D3D11_TEXTURE2D_DESC {
    UINT  Width;
    UINT  Height;
    UINT  MipLevels;
    UINT  ArraySize;
    UINT  Format;            // DXGI_FORMAT
    struct { UINT Count; UINT Quality; } SampleDesc;
    UINT  Usage;
    UINT  BindFlags;
    UINT  CPUAccessFlags;
    UINT  MiscFlags;
} D3D11_TEXTURE2D_DESC;

typedef struct D3D11_VIEWPORT {
    FLOAT TopLeftX;
    FLOAT TopLeftY;
    FLOAT Width;
    FLOAT Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
} D3D11_VIEWPORT;

typedef struct D3D11_INPUT_ELEMENT_DESC {
    const char*  SemanticName;
    UINT         SemanticIndex;
    UINT         Format;          // DXGI_FORMAT
    UINT         InputSlot;
    UINT         AlignedByteOffset;
    UINT         InputSlotClass;
    UINT         InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;

#pragma pack(pop)

// ============================================================================
//  Forward decls das interfaces.
// ============================================================================
struct ID3D11DeviceImpl;
struct ID3D11DeviceContextImpl;
struct ID3D11ResourceImpl;
struct ID3D11DeviceVtbl;
struct ID3D11DeviceContextVtbl;
struct ID3D11ResourceVtbl;

// ============================================================================
//  POOL ESTATICO de objetos (sem heap em ring 3).
//  Recursos genericos (buffers, textures, views, shaders, states) compartilham
//  o mesmo pool — tag identifica o tipo. Suficiente para um app DX11 simples
//  criar centenas de objetos sem refazer alocacao.
// ============================================================================

#define MAX_DEVICES      4
#define MAX_CONTEXTS    16
#define MAX_RESOURCES   64

// Tags de recurso — discriminam o tipo guardado no pool generico.
#define RES_BUFFER         1
#define RES_TEXTURE2D      2
#define RES_RTV            3   // render target view
#define RES_DSV            4   // depth stencil view
#define RES_SRV            5   // shader resource view
#define RES_VS             6   // vertex shader
#define RES_PS             7   // pixel shader
#define RES_INPUT_LAYOUT   8
#define RES_RASTERIZER     9
#define RES_BLEND         10
#define RES_DEPTH_STENCIL 11
#define RES_SAMPLER       12
#define RES_QUERY         13

typedef struct ID3D11ResourceImpl {
    const struct ID3D11ResourceVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    INT  tag;            // RES_* discriminator
    // Descriptors copiados; so o subset relevante por tag e usado.
    D3D11_BUFFER_DESC       buf_desc;
    D3D11_TEXTURE2D_DESC    tex_desc;
} ID3D11ResourceImpl;

typedef struct ID3D11DeviceContextImpl {
    const struct ID3D11DeviceContextVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    // Estado atual amarrado ao pipeline (apenas informativo, sem semantica real).
    ID3D11ResourceImpl* current_vs;
    ID3D11ResourceImpl* current_ps;
    ID3D11ResourceImpl* current_layout;
    ID3D11ResourceImpl* current_rt;        // primeiro RTV
    UINT                topology;
} ID3D11DeviceContextImpl;

typedef struct ID3D11DeviceImpl {
    const struct ID3D11DeviceVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT feature_level;
    UINT creation_flags;
    ID3D11DeviceContextImpl* immediate_ctx;
} ID3D11DeviceImpl;

static ID3D11DeviceImpl         g_devices  [MAX_DEVICES];
static ID3D11DeviceContextImpl  g_contexts [MAX_CONTEXTS];
static ID3D11ResourceImpl       g_resources[MAX_RESOURCES];

// ----------------------------------------------------------------------------
//  Utilitarios — memzero. Sem libc em ring 3.
// ----------------------------------------------------------------------------
static void mem_zero(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  Vtable: ID3D11Resource — base de todas as views/recursos.
//  Aqui colapsamos em UM unico vtable com TODOS os metodos comuns; cada metodo
//  apenas devolve S_OK. ABI compativel com ID3D11Buffer / ID3D11Texture2D /
//  ID3D11View / ID3D11Shader / ID3D11State.
// ============================================================================
typedef struct ID3D11ResourceVtbl {
    // --- IUnknown (3 entries) ---
    HRESULT (*QueryInterface)(ID3D11ResourceImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID3D11ResourceImpl* This);
    ULONG   (*Release)       (ID3D11ResourceImpl* This);
    // --- ID3D11DeviceChild (4 entries) ---
    void    (*GetDevice)     (ID3D11ResourceImpl* This, void** device);
    HRESULT (*GetPrivateData)(ID3D11ResourceImpl* T, REFGUID g, UINT* sz, void* d);
    HRESULT (*SetPrivateData)(ID3D11ResourceImpl* T, REFGUID g, UINT sz, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D11ResourceImpl* T, REFGUID g, const IUnknown* obj);
    // --- ID3D11Resource (4 entries) ---
    void    (*GetType)       (ID3D11ResourceImpl* This, UINT* dimension);
    void    (*SetEvictionPriority)(ID3D11ResourceImpl* This, UINT pri);
    UINT    (*GetEvictionPriority)(ID3D11ResourceImpl* This);
    HRESULT (*GetDesc)       (ID3D11ResourceImpl* This, void* desc);
} ID3D11ResourceVtbl;

static HRESULT Res_QueryInterface(ID3D11ResourceImpl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Res_AddRef (ID3D11ResourceImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Res_Release(ID3D11ResourceImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static void Res_GetDevice(ID3D11ResourceImpl* T, void** d) { (void)T; if (d) *d = 0; }
static HRESULT Res_GetPrivateData(ID3D11ResourceImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT Res_SetPrivateData(ID3D11ResourceImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT Res_SetPrivateDataInterface(ID3D11ResourceImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static void Res_GetType(ID3D11ResourceImpl* T, UINT* dim) {
    (void)T; if (dim) *dim = 1;
}
static void Res_SetEvictionPriority(ID3D11ResourceImpl* T, UINT p) { (void)T; (void)p; }
static UINT Res_GetEvictionPriority(ID3D11ResourceImpl* T) { (void)T; return 0; }
// GetDesc: dependendo do tag, devolve buffer_desc OU texture2d_desc OU nada.
static HRESULT Res_GetDesc(ID3D11ResourceImpl* This, void* desc) {
    if (!desc) return E_POINTER;
    if (This->tag == RES_BUFFER) {
        *(D3D11_BUFFER_DESC*)desc = This->buf_desc;
    } else if (This->tag == RES_TEXTURE2D) {
        *(D3D11_TEXTURE2D_DESC*)desc = This->tex_desc;
    } else {
        mem_zero(desc, sizeof(D3D11_BUFFER_DESC));
    }
    return S_OK;
}

static const ID3D11ResourceVtbl g_resourceVtbl = {
    Res_QueryInterface, Res_AddRef, Res_Release,
    Res_GetDevice, Res_GetPrivateData, Res_SetPrivateData, Res_SetPrivateDataInterface,
    Res_GetType, Res_SetEvictionPriority, Res_GetEvictionPriority, Res_GetDesc,
};

static ID3D11ResourceImpl* alloc_resource(INT tag) {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (!g_resources[i].used) {
            mem_zero(&g_resources[i], sizeof(g_resources[i]));
            g_resources[i].used     = 1;
            g_resources[i].refCount = 1;
            g_resources[i].lpVtbl   = &g_resourceVtbl;
            g_resources[i].tag      = tag;
            return &g_resources[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: ID3D11DeviceContext — comandos de pipeline. Stubs: registram apenas
//  o "estado corrente" no contexto (para um app inspecionar via Get*),
//  Draw/DrawIndexed nao desenham, Clear nao limpa, Present e do swap chain.
// ============================================================================
typedef struct ID3D11DeviceContextVtbl {
    // --- IUnknown (3) ---
    HRESULT (*QueryInterface)(ID3D11DeviceContextImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID3D11DeviceContextImpl* This);
    ULONG   (*Release)       (ID3D11DeviceContextImpl* This);
    // --- ID3D11DeviceChild (4) ---
    void    (*GetDevice)     (ID3D11DeviceContextImpl* This, void** device);
    HRESULT (*GetPrivateData)(ID3D11DeviceContextImpl* T, REFGUID g, UINT* sz, void* d);
    HRESULT (*SetPrivateData)(ID3D11DeviceContextImpl* T, REFGUID g, UINT sz, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D11DeviceContextImpl* T, REFGUID g, const IUnknown* o);
    // --- ID3D11DeviceContext (sample dos metodos mais usados; 25 entries) ---
    void    (*VSSetConstantBuffers)(ID3D11DeviceContextImpl* T, UINT slot, UINT n, void** bufs);
    void    (*PSSetShaderResources)(ID3D11DeviceContextImpl* T, UINT slot, UINT n, void** srvs);
    void    (*PSSetShader)         (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* ps,
                                    void** class_instances, UINT n_inst);
    void    (*PSSetSamplers)       (ID3D11DeviceContextImpl* T, UINT slot, UINT n, void** smp);
    void    (*VSSetShader)         (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* vs,
                                    void** class_instances, UINT n_inst);
    void    (*DrawIndexed)         (ID3D11DeviceContextImpl* T, UINT n_idx, UINT start, INT base);
    void    (*Draw)                (ID3D11DeviceContextImpl* T, UINT n_vtx, UINT start);
    HRESULT (*Map)                 (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* res,
                                    UINT subres, UINT map_type, UINT map_flags,
                                    void* mapped);
    void    (*Unmap)               (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* res, UINT subres);
    void    (*PSSetConstantBuffers)(ID3D11DeviceContextImpl* T, UINT slot, UINT n, void** bufs);
    void    (*IASetInputLayout)    (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* il);
    void    (*IASetVertexBuffers)  (ID3D11DeviceContextImpl* T, UINT slot, UINT n,
                                    void** bufs, const UINT* strides, const UINT* offsets);
    void    (*IASetIndexBuffer)    (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* buf,
                                    UINT fmt, UINT offset);
    void    (*DrawIndexedInstanced)(ID3D11DeviceContextImpl* T, UINT n_idx_per, UINT n_inst,
                                    UINT start_idx, INT base, UINT start_inst);
    void    (*DrawInstanced)       (ID3D11DeviceContextImpl* T, UINT n_vtx_per, UINT n_inst,
                                    UINT start_vtx, UINT start_inst);
    void    (*IASetPrimitiveTopology)(ID3D11DeviceContextImpl* T, UINT topo);
    void    (*RSSetState)          (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* st);
    void    (*RSSetViewports)      (ID3D11DeviceContextImpl* T, UINT n, const D3D11_VIEWPORT* vp);
    void    (*RSSetScissorRects)   (ID3D11DeviceContextImpl* T, UINT n, const void* rects);
    void    (*OMSetRenderTargets)  (ID3D11DeviceContextImpl* T, UINT n_rtv,
                                    ID3D11ResourceImpl** rtvs, ID3D11ResourceImpl* dsv);
    void    (*OMSetBlendState)     (ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* bs,
                                    const FLOAT* blend_factor, UINT sample_mask);
    void    (*OMSetDepthStencilState)(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* dss,
                                    UINT stencil_ref);
    void    (*ClearRenderTargetView)(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* rtv,
                                    const FLOAT color[4]);
    void    (*ClearDepthStencilView)(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* dsv,
                                    UINT flags, FLOAT depth, BYTE stencil);
    HRESULT (*FinishCommandList)   (ID3D11DeviceContextImpl* T, BOOL restore, void** cmdlist);
} ID3D11DeviceContextVtbl;

static HRESULT Ctx_QueryInterface(ID3D11DeviceContextImpl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Ctx_AddRef (ID3D11DeviceContextImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Ctx_Release(ID3D11DeviceContextImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static void Ctx_GetDevice(ID3D11DeviceContextImpl* T, void** d) { (void)T; if (d) *d = 0; }
static HRESULT Ctx_GetPrivateData(ID3D11DeviceContextImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT Ctx_SetPrivateData(ID3D11DeviceContextImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT Ctx_SetPrivateDataInterface(ID3D11DeviceContextImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
// Setters/getters de estado — apenas guardam o ponteiro corrente.
static void Ctx_VSSetConstantBuffers(ID3D11DeviceContextImpl* T, UINT s, UINT n, void** b) {
    (void)T; (void)s; (void)n; (void)b;
}
static void Ctx_PSSetShaderResources(ID3D11DeviceContextImpl* T, UINT s, UINT n, void** v) {
    (void)T; (void)s; (void)n; (void)v;
}
static void Ctx_PSSetShader(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* ps,
                            void** ci, UINT n) {
    (void)ci; (void)n; T->current_ps = ps;
}
static void Ctx_PSSetSamplers(ID3D11DeviceContextImpl* T, UINT s, UINT n, void** v) {
    (void)T; (void)s; (void)n; (void)v;
}
static void Ctx_VSSetShader(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* vs,
                            void** ci, UINT n) {
    (void)ci; (void)n; T->current_vs = vs;
}
static void Ctx_DrawIndexed(ID3D11DeviceContextImpl* T, UINT n, UINT s, INT b) {
    (void)T; (void)n; (void)s; (void)b;
}
static void Ctx_Draw(ID3D11DeviceContextImpl* T, UINT n, UINT s) {
    (void)T; (void)n; (void)s;
}
// Map: para D3D11_USAGE_DYNAMIC, retornaria um ponteiro mapeavel. Nao temos
// memoria de back-store; retornamos pSysMem=0 + RowPitch=0 + S_OK.
typedef struct D3D11_MAPPED_SUBRESOURCE { void* pData; UINT RowPitch; UINT DepthPitch; } D3D11_MAPPED_SUBRESOURCE;
static HRESULT Ctx_Map(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* r,
                       UINT sr, UINT mt, UINT mf, void* m) {
    (void)T; (void)r; (void)sr; (void)mt; (void)mf;
    if (!m) return E_POINTER;
    D3D11_MAPPED_SUBRESOURCE* msr = (D3D11_MAPPED_SUBRESOURCE*)m;
    msr->pData = 0; msr->RowPitch = 0; msr->DepthPitch = 0;
    return S_OK;
}
static void Ctx_Unmap(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* r, UINT s) {
    (void)T; (void)r; (void)s;
}
static void Ctx_PSSetConstantBuffers(ID3D11DeviceContextImpl* T, UINT s, UINT n, void** b) {
    (void)T; (void)s; (void)n; (void)b;
}
static void Ctx_IASetInputLayout(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* il) {
    T->current_layout = il;
}
static void Ctx_IASetVertexBuffers(ID3D11DeviceContextImpl* T, UINT s, UINT n,
                                    void** b, const UINT* st, const UINT* o) {
    (void)T; (void)s; (void)n; (void)b; (void)st; (void)o;
}
static void Ctx_IASetIndexBuffer(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* b,
                                  UINT f, UINT o) {
    (void)T; (void)b; (void)f; (void)o;
}
static void Ctx_DrawIndexedInstanced(ID3D11DeviceContextImpl* T, UINT a, UINT b,
                                      UINT c, INT d, UINT e) {
    (void)T; (void)a; (void)b; (void)c; (void)d; (void)e;
}
static void Ctx_DrawInstanced(ID3D11DeviceContextImpl* T, UINT a, UINT b, UINT c, UINT d) {
    (void)T; (void)a; (void)b; (void)c; (void)d;
}
static void Ctx_IASetPrimitiveTopology(ID3D11DeviceContextImpl* T, UINT topo) {
    T->topology = topo;
}
static void Ctx_RSSetState(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* s) { (void)T; (void)s; }
static void Ctx_RSSetViewports(ID3D11DeviceContextImpl* T, UINT n, const D3D11_VIEWPORT* v) {
    (void)T; (void)n; (void)v;
}
static void Ctx_RSSetScissorRects(ID3D11DeviceContextImpl* T, UINT n, const void* r) {
    (void)T; (void)n; (void)r;
}
static void Ctx_OMSetRenderTargets(ID3D11DeviceContextImpl* T, UINT n,
                                    ID3D11ResourceImpl** rtvs, ID3D11ResourceImpl* dsv) {
    (void)dsv;
    if (n > 0 && rtvs) T->current_rt = rtvs[0];
    else               T->current_rt = 0;
}
static void Ctx_OMSetBlendState(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* b,
                                 const FLOAT* bf, UINT sm) {
    (void)T; (void)b; (void)bf; (void)sm;
}
static void Ctx_OMSetDepthStencilState(ID3D11DeviceContextImpl* T,
                                        ID3D11ResourceImpl* d, UINT r) {
    (void)T; (void)d; (void)r;
}
static void Ctx_ClearRenderTargetView(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* r,
                                       const FLOAT c[4]) {
    (void)T; (void)r; (void)c;
}
static void Ctx_ClearDepthStencilView(ID3D11DeviceContextImpl* T, ID3D11ResourceImpl* d,
                                       UINT f, FLOAT z, BYTE s) {
    (void)T; (void)d; (void)f; (void)z; (void)s;
}
static HRESULT Ctx_FinishCommandList(ID3D11DeviceContextImpl* T, BOOL r, void** cl) {
    (void)T; (void)r;
    if (cl) *cl = 0;
    return S_OK;
}

static const ID3D11DeviceContextVtbl g_contextVtbl = {
    Ctx_QueryInterface, Ctx_AddRef, Ctx_Release,
    Ctx_GetDevice, Ctx_GetPrivateData, Ctx_SetPrivateData, Ctx_SetPrivateDataInterface,
    Ctx_VSSetConstantBuffers, Ctx_PSSetShaderResources, Ctx_PSSetShader,
    Ctx_PSSetSamplers, Ctx_VSSetShader, Ctx_DrawIndexed, Ctx_Draw,
    Ctx_Map, Ctx_Unmap, Ctx_PSSetConstantBuffers,
    Ctx_IASetInputLayout, Ctx_IASetVertexBuffers, Ctx_IASetIndexBuffer,
    Ctx_DrawIndexedInstanced, Ctx_DrawInstanced, Ctx_IASetPrimitiveTopology,
    Ctx_RSSetState, Ctx_RSSetViewports, Ctx_RSSetScissorRects,
    Ctx_OMSetRenderTargets, Ctx_OMSetBlendState, Ctx_OMSetDepthStencilState,
    Ctx_ClearRenderTargetView, Ctx_ClearDepthStencilView, Ctx_FinishCommandList,
};

static ID3D11DeviceContextImpl* alloc_context(void) {
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (!g_contexts[i].used) {
            mem_zero(&g_contexts[i], sizeof(g_contexts[i]));
            g_contexts[i].used     = 1;
            g_contexts[i].refCount = 1;
            g_contexts[i].lpVtbl   = &g_contextVtbl;
            g_contexts[i].topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            return &g_contexts[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: ID3D11Device — fabrica de recursos. Cada Create* aloca um slot do
//  pool generico, marca a tag e copia o desc para o slot.
// ============================================================================
typedef struct ID3D11DeviceVtbl {
    // --- IUnknown (3) ---
    HRESULT (*QueryInterface)(ID3D11DeviceImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID3D11DeviceImpl* This);
    ULONG   (*Release)       (ID3D11DeviceImpl* This);
    // --- ID3D11Device proprio (subset; 27 entries) ---
    HRESULT (*CreateBuffer)         (ID3D11DeviceImpl* This,
                                     const D3D11_BUFFER_DESC* desc,
                                     const D3D11_SUBRESOURCE_DATA* data,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateTexture1D)      (ID3D11DeviceImpl* This,
                                     const void* desc,
                                     const D3D11_SUBRESOURCE_DATA* data,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateTexture2D)      (ID3D11DeviceImpl* This,
                                     const D3D11_TEXTURE2D_DESC* desc,
                                     const D3D11_SUBRESOURCE_DATA* data,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateTexture3D)      (ID3D11DeviceImpl* This,
                                     const void* desc,
                                     const D3D11_SUBRESOURCE_DATA* data,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateShaderResourceView)(ID3D11DeviceImpl* This,
                                     ID3D11ResourceImpl* res,
                                     const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateUnorderedAccessView)(ID3D11DeviceImpl* This,
                                     ID3D11ResourceImpl* res,
                                     const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateRenderTargetView)(ID3D11DeviceImpl* This,
                                     ID3D11ResourceImpl* res,
                                     const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateDepthStencilView)(ID3D11DeviceImpl* This,
                                     ID3D11ResourceImpl* res,
                                     const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateInputLayout)    (ID3D11DeviceImpl* This,
                                     const D3D11_INPUT_ELEMENT_DESC* elements,
                                     UINT n_elements,
                                     const void* shader_bc,
                                     UINT bc_size,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateVertexShader)   (ID3D11DeviceImpl* This,
                                     const void* bc, UINT bc_size,
                                     IUnknown* class_linkage,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateGeometryShader) (ID3D11DeviceImpl* This,
                                     const void* bc, UINT bc_size,
                                     IUnknown* cl, ID3D11ResourceImpl** out);
    HRESULT (*CreateGeometryShaderWithStreamOutput)(ID3D11DeviceImpl* This,
                                     const void* bc, UINT bc_size,
                                     const void* so_entries, UINT so_n,
                                     const UINT* buffer_strides, UINT n_strides,
                                     UINT rasterized_stream,
                                     IUnknown* cl, ID3D11ResourceImpl** out);
    HRESULT (*CreatePixelShader)    (ID3D11DeviceImpl* This,
                                     const void* bc, UINT bc_size,
                                     IUnknown* cl, ID3D11ResourceImpl** out);
    HRESULT (*CreateHullShader)     (ID3D11DeviceImpl* This,
                                     const void* bc, UINT bc_size,
                                     IUnknown* cl, ID3D11ResourceImpl** out);
    HRESULT (*CreateDomainShader)   (ID3D11DeviceImpl* This,
                                     const void* bc, UINT bc_size,
                                     IUnknown* cl, ID3D11ResourceImpl** out);
    HRESULT (*CreateComputeShader)  (ID3D11DeviceImpl* This,
                                     const void* bc, UINT bc_size,
                                     IUnknown* cl, ID3D11ResourceImpl** out);
    HRESULT (*CreateClassLinkage)   (ID3D11DeviceImpl* This, IUnknown** linkage);
    HRESULT (*CreateBlendState)     (ID3D11DeviceImpl* This, const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateDepthStencilState)(ID3D11DeviceImpl* This, const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateRasterizerState)(ID3D11DeviceImpl* This, const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateSamplerState)   (ID3D11DeviceImpl* This, const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateQuery)          (ID3D11DeviceImpl* This, const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreatePredicate)      (ID3D11DeviceImpl* This, const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateCounter)        (ID3D11DeviceImpl* This, const void* desc,
                                     ID3D11ResourceImpl** out);
    HRESULT (*CreateDeferredContext)(ID3D11DeviceImpl* This, UINT flags,
                                     ID3D11DeviceContextImpl** out);
    void    (*GetImmediateContext)  (ID3D11DeviceImpl* This,
                                     ID3D11DeviceContextImpl** ctx);
    UINT    (*GetFeatureLevel)      (ID3D11DeviceImpl* This);
    UINT    (*GetCreationFlags)     (ID3D11DeviceImpl* This);
    HRESULT (*GetDeviceRemovedReason)(ID3D11DeviceImpl* This);
} ID3D11DeviceVtbl;

static HRESULT Dev_QueryInterface(ID3D11DeviceImpl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Dev_AddRef (ID3D11DeviceImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Dev_Release(ID3D11DeviceImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
// CreateBuffer: aloca slot, copia desc, ignora dados iniciais.
static HRESULT Dev_CreateBuffer(ID3D11DeviceImpl* This,
                                const D3D11_BUFFER_DESC* desc,
                                const D3D11_SUBRESOURCE_DATA* data,
                                ID3D11ResourceImpl** out) {
    (void)This; (void)data;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_BUFFER);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    if (desc) r->buf_desc = *desc;
    *out = r;
    return S_OK;
}
static HRESULT Dev_CreateTexture1D(ID3D11DeviceImpl* This, const void* desc,
                                    const D3D11_SUBRESOURCE_DATA* data,
                                    ID3D11ResourceImpl** out) {
    (void)This; (void)desc; (void)data;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_TEXTURE2D);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateTexture2D(ID3D11DeviceImpl* This,
                                    const D3D11_TEXTURE2D_DESC* desc,
                                    const D3D11_SUBRESOURCE_DATA* data,
                                    ID3D11ResourceImpl** out) {
    (void)This; (void)data;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_TEXTURE2D);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    if (desc) r->tex_desc = *desc;
    *out = r;
    return S_OK;
}
static HRESULT Dev_CreateTexture3D(ID3D11DeviceImpl* This, const void* desc,
                                    const D3D11_SUBRESOURCE_DATA* data,
                                    ID3D11ResourceImpl** out) {
    (void)This; (void)desc; (void)data;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_TEXTURE2D);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateShaderResourceView(ID3D11DeviceImpl* This,
                                             ID3D11ResourceImpl* res,
                                             const void* desc,
                                             ID3D11ResourceImpl** out) {
    (void)This; (void)res; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* v = alloc_resource(RES_SRV);
    if (!v) { *out = 0; return E_OUTOFMEMORY; }
    *out = v; return S_OK;
}
static HRESULT Dev_CreateUnorderedAccessView(ID3D11DeviceImpl* This,
                                              ID3D11ResourceImpl* res,
                                              const void* desc,
                                              ID3D11ResourceImpl** out) {
    (void)This; (void)res; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* v = alloc_resource(RES_SRV);
    if (!v) { *out = 0; return E_OUTOFMEMORY; }
    *out = v; return S_OK;
}
static HRESULT Dev_CreateRenderTargetView(ID3D11DeviceImpl* This,
                                           ID3D11ResourceImpl* res,
                                           const void* desc,
                                           ID3D11ResourceImpl** out) {
    (void)This; (void)res; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* v = alloc_resource(RES_RTV);
    if (!v) { *out = 0; return E_OUTOFMEMORY; }
    *out = v; return S_OK;
}
static HRESULT Dev_CreateDepthStencilView(ID3D11DeviceImpl* This,
                                           ID3D11ResourceImpl* res,
                                           const void* desc,
                                           ID3D11ResourceImpl** out) {
    (void)This; (void)res; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* v = alloc_resource(RES_DSV);
    if (!v) { *out = 0; return E_OUTOFMEMORY; }
    *out = v; return S_OK;
}
static HRESULT Dev_CreateInputLayout(ID3D11DeviceImpl* This,
                                      const D3D11_INPUT_ELEMENT_DESC* elem, UINT ne,
                                      const void* bc, UINT bcs,
                                      ID3D11ResourceImpl** out) {
    (void)This; (void)elem; (void)ne; (void)bc; (void)bcs;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_INPUT_LAYOUT);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateVertexShader(ID3D11DeviceImpl* This,
                                       const void* bc, UINT bcs,
                                       IUnknown* cl, ID3D11ResourceImpl** out) {
    (void)This; (void)bc; (void)bcs; (void)cl;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_VS);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateGeometryShader(ID3D11DeviceImpl* This,
                                         const void* bc, UINT bcs,
                                         IUnknown* cl, ID3D11ResourceImpl** out) {
    (void)This; (void)bc; (void)bcs; (void)cl;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_VS);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateGeometryShaderWithStreamOutput(ID3D11DeviceImpl* This,
            const void* bc, UINT bcs, const void* so, UINT son,
            const UINT* bs, UINT nbs, UINT rs,
            IUnknown* cl, ID3D11ResourceImpl** out) {
    (void)This; (void)bc; (void)bcs; (void)so; (void)son;
    (void)bs; (void)nbs; (void)rs; (void)cl;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_VS);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreatePixelShader(ID3D11DeviceImpl* This,
                                      const void* bc, UINT bcs,
                                      IUnknown* cl, ID3D11ResourceImpl** out) {
    (void)This; (void)bc; (void)bcs; (void)cl;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_PS);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateHullShader(ID3D11DeviceImpl* T, const void* bc, UINT bcs,
                                     IUnknown* cl, ID3D11ResourceImpl** out) {
    (void)T; (void)bc; (void)bcs; (void)cl;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_VS);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateDomainShader(ID3D11DeviceImpl* T, const void* bc, UINT bcs,
                                       IUnknown* cl, ID3D11ResourceImpl** out) {
    (void)T; (void)bc; (void)bcs; (void)cl;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_VS);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateComputeShader(ID3D11DeviceImpl* T, const void* bc, UINT bcs,
                                        IUnknown* cl, ID3D11ResourceImpl** out) {
    (void)T; (void)bc; (void)bcs; (void)cl;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_VS);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateClassLinkage(ID3D11DeviceImpl* T, IUnknown** l) {
    (void)T; if (l) *l = 0; return S_OK;
}
static HRESULT Dev_CreateBlendState(ID3D11DeviceImpl* T, const void* desc,
                                     ID3D11ResourceImpl** out) {
    (void)T; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_BLEND);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateDepthStencilState(ID3D11DeviceImpl* T, const void* desc,
                                            ID3D11ResourceImpl** out) {
    (void)T; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_DEPTH_STENCIL);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateRasterizerState(ID3D11DeviceImpl* T, const void* desc,
                                          ID3D11ResourceImpl** out) {
    (void)T; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_RASTERIZER);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateSamplerState(ID3D11DeviceImpl* T, const void* desc,
                                       ID3D11ResourceImpl** out) {
    (void)T; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_SAMPLER);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateQuery(ID3D11DeviceImpl* T, const void* desc,
                                ID3D11ResourceImpl** out) {
    (void)T; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_QUERY);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreatePredicate(ID3D11DeviceImpl* T, const void* desc,
                                    ID3D11ResourceImpl** out) {
    (void)T; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_QUERY);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateCounter(ID3D11DeviceImpl* T, const void* desc,
                                  ID3D11ResourceImpl** out) {
    (void)T; (void)desc;
    if (!out) return E_POINTER;
    ID3D11ResourceImpl* r = alloc_resource(RES_QUERY);
    if (!r) { *out = 0; return E_OUTOFMEMORY; }
    *out = r; return S_OK;
}
static HRESULT Dev_CreateDeferredContext(ID3D11DeviceImpl* T, UINT f,
                                          ID3D11DeviceContextImpl** out) {
    (void)T; (void)f;
    if (!out) return E_POINTER;
    ID3D11DeviceContextImpl* c = alloc_context();
    if (!c) { *out = 0; return E_OUTOFMEMORY; }
    *out = c; return S_OK;
}
static void Dev_GetImmediateContext(ID3D11DeviceImpl* T, ID3D11DeviceContextImpl** c) {
    if (!c) return;
    if (T->immediate_ctx) T->immediate_ctx->refCount++;
    *c = T->immediate_ctx;
}
static UINT Dev_GetFeatureLevel(ID3D11DeviceImpl* T) { return T->feature_level; }
static UINT Dev_GetCreationFlags(ID3D11DeviceImpl* T) { return T->creation_flags; }
static HRESULT Dev_GetDeviceRemovedReason(ID3D11DeviceImpl* T) { (void)T; return S_OK; }

static const ID3D11DeviceVtbl g_deviceVtbl = {
    Dev_QueryInterface, Dev_AddRef, Dev_Release,
    Dev_CreateBuffer, Dev_CreateTexture1D, Dev_CreateTexture2D, Dev_CreateTexture3D,
    Dev_CreateShaderResourceView, Dev_CreateUnorderedAccessView,
    Dev_CreateRenderTargetView, Dev_CreateDepthStencilView,
    Dev_CreateInputLayout,
    Dev_CreateVertexShader, Dev_CreateGeometryShader,
    Dev_CreateGeometryShaderWithStreamOutput,
    Dev_CreatePixelShader, Dev_CreateHullShader, Dev_CreateDomainShader,
    Dev_CreateComputeShader, Dev_CreateClassLinkage,
    Dev_CreateBlendState, Dev_CreateDepthStencilState,
    Dev_CreateRasterizerState, Dev_CreateSamplerState,
    Dev_CreateQuery, Dev_CreatePredicate, Dev_CreateCounter,
    Dev_CreateDeferredContext, Dev_GetImmediateContext,
    Dev_GetFeatureLevel, Dev_GetCreationFlags, Dev_GetDeviceRemovedReason,
};

static ID3D11DeviceImpl* alloc_device(UINT feature_level, UINT flags) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            mem_zero(&g_devices[i], sizeof(g_devices[i]));
            g_devices[i].used           = 1;
            g_devices[i].refCount       = 1;
            g_devices[i].lpVtbl         = &g_deviceVtbl;
            g_devices[i].feature_level  = feature_level;
            g_devices[i].creation_flags = flags;
            // Cria o immediate context (sempre vai junto com o device).
            g_devices[i].immediate_ctx  = alloc_context();
            return &g_devices[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados — assinaturas BATEM com a d3d11.dll real do Windows.
//  D3D11CreateDevice(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE software, UINT flags,
//                    const D3D_FEATURE_LEVEL* levels, UINT levels_n,
//                    UINT sdk_ver,
//                    ID3D11Device** ppDevice,
//                    D3D_FEATURE_LEVEL* pFeatureLevel,
//                    ID3D11DeviceContext** ppCtx);
//
//  D3D11CreateDeviceAndSwapChain — atalho que combina o device + swap chain.
//  Aqui o ponteiro para swap chain e tratado como void*: o caller (app) ja
//  acredita que pegou um IDXGISwapChain* (na verdade ele e do dxgi.dll).
// ============================================================================

// Escolhe o feature level efetivo: se o app passou uma lista, usa o primeiro
// suportado (todos retornam true aqui); se NULL, default 11_0.
static UINT pick_feature_level(const UINT* levels, UINT n) {
    if (levels && n > 0) return levels[0];
    return D3D_FEATURE_LEVEL_11_0;
}

__declspec(dllexport) HRESULT D3D11CreateDevice(
        void* adapter, UINT driver_type, HMODULE software,
        UINT flags, const UINT* levels, UINT levels_n,
        UINT sdk_ver,
        ID3D11DeviceImpl** ppDevice,
        UINT* pFeatureLevel,
        ID3D11DeviceContextImpl** ppCtx) {
    (void)adapter; (void)driver_type; (void)software; (void)sdk_ver;
    ID3D11DeviceImpl* dev = alloc_device(pick_feature_level(levels, levels_n), flags);
    if (!dev) {
        if (ppDevice) *ppDevice = 0;
        if (ppCtx)    *ppCtx    = 0;
        return E_OUTOFMEMORY;
    }
    if (ppDevice)      *ppDevice      = dev;
    if (pFeatureLevel) *pFeatureLevel = dev->feature_level;
    if (ppCtx) {
        if (dev->immediate_ctx) dev->immediate_ctx->refCount++;
        *ppCtx = dev->immediate_ctx;
    }
    return S_OK;
}

// ============================================================================
//  FASE 9.10 — IDXGISwapChain ABI-compativel embutido (sem dependencia de
//  dxgi.dll). Layout do vtable BATE com dll/win32/dxgi/dxgi.c — assim apps
//  podem chamar swap->lpVtbl->Present() sem importar dxgi separadamente.
//  Suporta os metodos minimos que o D3D11CreateDeviceAndSwapChain precisa
//  publicar: Present, GetBuffer, ResizeBuffers, GetDesc + IUnknown.
// ============================================================================
typedef struct ID3D11SwapChainImpl {
    const struct ID3D11SwapChainVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT width;
    UINT height;
    UINT format;
    UINT buffer_count;
    UINT current_buffer;
    HWND output_window;
    ID3D11DeviceImpl* device;
} ID3D11SwapChainImpl;

#define MAX_SWAPCHAINS_D3D11   4
static ID3D11SwapChainImpl g_swapchains_d3d11[MAX_SWAPCHAINS_D3D11];

typedef struct ID3D11SwapChainVtbl {
    // --- IUnknown (3) ---
    HRESULT (*QueryInterface)(ID3D11SwapChainImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ID3D11SwapChainImpl* This);
    ULONG   (*Release)       (ID3D11SwapChainImpl* This);
    // --- IDXGIObject (4) ---
    HRESULT (*SetPrivateData)(ID3D11SwapChainImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D11SwapChainImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*GetPrivateData)(ID3D11SwapChainImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*GetParent)     (ID3D11SwapChainImpl* T, REFIID r, void** p);
    // --- IDXGIDeviceSubObject (1) ---
    HRESULT (*GetDevice)     (ID3D11SwapChainImpl* T, REFIID r, void** d);
    // --- IDXGISwapChain proprio ---
    HRESULT (*Present)       (ID3D11SwapChainImpl* This, UINT sync_interval, UINT flags);
    HRESULT (*GetBuffer)     (ID3D11SwapChainImpl* This, UINT idx, REFIID r, void** surface);
    HRESULT (*SetFullscreenState)(ID3D11SwapChainImpl* T, BOOL fs, void* tgt);
    HRESULT (*GetFullscreenState)(ID3D11SwapChainImpl* T, BOOL* fs, void** tgt);
    HRESULT (*GetDesc)       (ID3D11SwapChainImpl* This, void* desc);
    HRESULT (*ResizeBuffers) (ID3D11SwapChainImpl* This, UINT count, UINT w, UINT h,
                              UINT fmt, UINT flags);
    HRESULT (*ResizeTarget)  (ID3D11SwapChainImpl* This, const void* m);
    HRESULT (*GetContainingOutput)(ID3D11SwapChainImpl* T, void** out);
    HRESULT (*GetFrameStatistics)(ID3D11SwapChainImpl* T, void* stats);
    HRESULT (*GetLastPresentCount)(ID3D11SwapChainImpl* T, UINT* cnt);
} ID3D11SwapChainVtbl;

static HRESULT D3D11Sc_QueryInterface(ID3D11SwapChainImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = T; T->refCount++; return S_OK;
}
static ULONG D3D11Sc_AddRef (ID3D11SwapChainImpl* T) { return (ULONG)(++T->refCount); }
static ULONG D3D11Sc_Release(ID3D11SwapChainImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT D3D11Sc_SetPrivateData(ID3D11SwapChainImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT D3D11Sc_SetPrivateDataInterface(ID3D11SwapChainImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT D3D11Sc_GetPrivateData(ID3D11SwapChainImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT D3D11Sc_GetParent(ID3D11SwapChainImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return E_NOINTERFACE;
}
static HRESULT D3D11Sc_GetDevice(ID3D11SwapChainImpl* T, REFIID r, void** d) {
    (void)r; if (!d) return E_POINTER;
    if (T->device) T->device->refCount++;
    *d = T->device; return S_OK;
}
// Present: avanca o buffer circular; no-op no FB (win32k continua dono da tela).
static HRESULT D3D11Sc_Present(ID3D11SwapChainImpl* T, UINT sync_interval, UINT flags) {
    (void)sync_interval; (void)flags;
    if (T->buffer_count > 0) {
        T->current_buffer = (T->current_buffer + 1) % T->buffer_count;
    }
    return S_OK;
}
static HRESULT D3D11Sc_GetBuffer(ID3D11SwapChainImpl* T, UINT idx, REFIID r, void** surface) {
    (void)idx; (void)r;
    if (!surface) return E_POINTER;
    // Devolve um RTV fake do pool de recursos d3d11 (compatibilidade com
    // ID3D11Texture2D — apps fazem GetBuffer(0, IID_ID3D11Texture2D, &tex)).
    ID3D11ResourceImpl* res = alloc_resource(RES_TEXTURE2D);
    if (!res) { *surface = 0; return E_OUTOFMEMORY; }
    res->tex_desc.Width  = T->width;
    res->tex_desc.Height = T->height;
    res->tex_desc.Format = T->format;
    *surface = res;
    return S_OK;
}
static HRESULT D3D11Sc_SetFullscreenState(ID3D11SwapChainImpl* T, BOOL fs, void* tgt) {
    (void)T; (void)fs; (void)tgt; return S_OK;
}
static HRESULT D3D11Sc_GetFullscreenState(ID3D11SwapChainImpl* T, BOOL* fs, void** tgt) {
    (void)T; if (fs) *fs = 0; if (tgt) *tgt = 0; return S_OK;
}
static HRESULT D3D11Sc_GetDesc(ID3D11SwapChainImpl* T, void* desc) {
    (void)T; if (!desc) return E_POINTER; mem_zero(desc, 96); return S_OK;
}
static HRESULT D3D11Sc_ResizeBuffers(ID3D11SwapChainImpl* T, UINT count, UINT w, UINT h,
                                      UINT fmt, UINT flags) {
    (void)flags;
    if (count) T->buffer_count = count;
    if (w)     T->width  = w;
    if (h)     T->height = h;
    if (fmt)   T->format = fmt;
    T->current_buffer = 0;
    return S_OK;
}
static HRESULT D3D11Sc_ResizeTarget(ID3D11SwapChainImpl* T, const void* m) {
    (void)T; (void)m; return S_OK;
}
static HRESULT D3D11Sc_GetContainingOutput(ID3D11SwapChainImpl* T, void** out) {
    (void)T; if (out) *out = 0; return S_OK;
}
static HRESULT D3D11Sc_GetFrameStatistics(ID3D11SwapChainImpl* T, void* stats) {
    (void)T; if (stats) mem_zero(stats, 32); return S_OK;
}
static HRESULT D3D11Sc_GetLastPresentCount(ID3D11SwapChainImpl* T, UINT* cnt) {
    (void)T; if (cnt) *cnt = 0; return S_OK;
}

static const ID3D11SwapChainVtbl g_d3d11SwapChainVtbl = {
    D3D11Sc_QueryInterface, D3D11Sc_AddRef, D3D11Sc_Release,
    D3D11Sc_SetPrivateData, D3D11Sc_SetPrivateDataInterface, D3D11Sc_GetPrivateData, D3D11Sc_GetParent,
    D3D11Sc_GetDevice,
    D3D11Sc_Present, D3D11Sc_GetBuffer,
    D3D11Sc_SetFullscreenState, D3D11Sc_GetFullscreenState,
    D3D11Sc_GetDesc, D3D11Sc_ResizeBuffers, D3D11Sc_ResizeTarget,
    D3D11Sc_GetContainingOutput, D3D11Sc_GetFrameStatistics, D3D11Sc_GetLastPresentCount,
};

static ID3D11SwapChainImpl* alloc_d3d11_swapchain(const void* desc, ID3D11DeviceImpl* dev) {
    (void)desc;
    for (int i = 0; i < MAX_SWAPCHAINS_D3D11; i++) {
        if (!g_swapchains_d3d11[i].used) {
            mem_zero(&g_swapchains_d3d11[i], sizeof(g_swapchains_d3d11[i]));
            g_swapchains_d3d11[i].used         = 1;
            g_swapchains_d3d11[i].refCount     = 1;
            g_swapchains_d3d11[i].lpVtbl       = &g_d3d11SwapChainVtbl;
            g_swapchains_d3d11[i].width        = 1024;
            g_swapchains_d3d11[i].height       = 768;
            g_swapchains_d3d11[i].format       = 87;   // DXGI_FORMAT_B8G8R8A8_UNORM
            g_swapchains_d3d11[i].buffer_count = 2;
            g_swapchains_d3d11[i].device       = dev;
            // Le os campos do DXGI_SWAP_CHAIN_DESC se o app forneceu (apenas
            // os campos minimos relevantes — Width/Height/Format/BufferCount).
            if (desc) {
                // Layout DXGI_SWAP_CHAIN_DESC: BufferDesc (44 bytes: W, H, refresh{N,D},
                // Format, ScanlineOrdering, Scaling). Lemos manualmente para nao
                // depender de typedefs.
                const UINT* d = (const UINT*)desc;
                if (d[0]) g_swapchains_d3d11[i].width  = d[0];
                if (d[1]) g_swapchains_d3d11[i].height = d[1];
            }
            return &g_swapchains_d3d11[i];
        }
    }
    return 0;
}

// D3D11CreateDeviceAndSwapChain — cria o device + um swap chain associado.
// FASE 9.10 — agora aloca um IDXGISwapChain ABI-compativel (vtable propria)
// em vez de devolver o device como swap chain (que crasheava em Present).
__declspec(dllexport) HRESULT D3D11CreateDeviceAndSwapChain(
        void* adapter, UINT driver_type, HMODULE software,
        UINT flags, const UINT* levels, UINT levels_n,
        UINT sdk_ver,
        const void* swap_chain_desc,
        void** ppSwapChain,
        ID3D11DeviceImpl** ppDevice,
        UINT* pFeatureLevel,
        ID3D11DeviceContextImpl** ppCtx) {
    HRESULT hr = D3D11CreateDevice(adapter, driver_type, software, flags,
                                    levels, levels_n, sdk_ver,
                                    ppDevice, pFeatureLevel, ppCtx);
    if (hr != S_OK) {
        if (ppSwapChain) *ppSwapChain = 0;
        return hr;
    }
    if (ppSwapChain) {
        ID3D11DeviceImpl* dev = ppDevice ? *ppDevice : 0;
        ID3D11SwapChainImpl* sc = alloc_d3d11_swapchain(swap_chain_desc, dev);
        if (!sc) {
            *ppSwapChain = 0;
            return E_OUTOFMEMORY;
        }
        *ppSwapChain = sc;
    }
    return S_OK;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
