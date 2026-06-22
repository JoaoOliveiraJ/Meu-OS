// d3d12.dll  —  reimplementacao minima do Direct3D 12 (FASE 9.8).
//
// Acima do DXGI 1.x (FASE 9.7+), o D3D12 e a API grafica PRIMARIA do Windows 10
// em diante. Diferente de D3D11 (com immediate context "magico"), o D3D12 e
// EXPLICITO: o app precisa criar command allocators + command lists + command
// queues e sincronizar a si mesmo com fences. Mais perto do metal — modelo
// inspirado em Vulkan/Mantle.
//
// No real, d3d12.dll vive em RING 3, fala com d3d12_<vendor>.dll (driver UMD)
// e este por sua vez ataca dxgkrnl.sys via syscalls. Aqui no MeuOS nao temos
// rasterizador real (BasicDisplay e KMD-only para 2D); este stub se limita ao
// ABI COM completo de ID3D12Device + ID3D12CommandQueue + ID3D12CommandList +
// ID3D12Resource + ID3D12Fence + ID3D12PipelineState. TUDO retorna S_OK ou
// E_OUTOFMEMORY quando o pool acaba.
//
// COM ABI (estilo NT/Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg
// "this". Em ABI ms_abi (x86_64-windows-gnu) os parametros entram em
// RCX,RDX,R8,R9 (e essa a ABI que o zig cc gera com -target windows-gnu).
//
// Em DLLs ring 3 do MeuOS nao temos VirtualAlloc/HeapAlloc Win32 reais;
// por isso usamos POOLS ESTATICOS de objetos. Pools dimensionados para um
// app DX12 simples: 4 devices, 8 queues, 16 allocators, 32 lists, 64 resources.
//
// IMAGE BASE: 0x4600000 — sobreposicao com PMM_BASE (0x4000000). Para evitar
// colisao usamos --dynamicbase no build (.reloc), entao o loader pode realocar
// para qualquer endereco virtual livre. Este e o mesmo mecanismo usado por
// hello32.exe e drivers .sys, e tambem por d3d11.dll na mesma fase.

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

// D3D_FEATURE_LEVEL — same as D3D11. DX12 sempre exige >=11_0; alguns hardware
// suporta 12_0/12_1. Aqui aceitamos qualquer.
#define D3D_FEATURE_LEVEL_11_0           0xb000
#define D3D_FEATURE_LEVEL_11_1           0xb100
#define D3D_FEATURE_LEVEL_12_0           0xc000
#define D3D_FEATURE_LEVEL_12_1           0xc100
#define D3D_FEATURE_LEVEL_12_2           0xc200

// D3D12_COMMAND_LIST_TYPE.
#define D3D12_COMMAND_LIST_TYPE_DIRECT    0
#define D3D12_COMMAND_LIST_TYPE_BUNDLE    1
#define D3D12_COMMAND_LIST_TYPE_COMPUTE   2
#define D3D12_COMMAND_LIST_TYPE_COPY      3

// D3D12_HEAP_TYPE.
#define D3D12_HEAP_TYPE_DEFAULT          1
#define D3D12_HEAP_TYPE_UPLOAD           2
#define D3D12_HEAP_TYPE_READBACK         3
#define D3D12_HEAP_TYPE_CUSTOM           4

// D3D12_RESOURCE_STATE (subset).
#define D3D12_RESOURCE_STATE_COMMON              0
#define D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT 0x1
#define D3D12_RESOURCE_STATE_INDEX_BUFFER        0x2
#define D3D12_RESOURCE_STATE_RENDER_TARGET       0x4
#define D3D12_RESOURCE_STATE_UNORDERED_ACCESS    0x8
#define D3D12_RESOURCE_STATE_DEPTH_WRITE         0x10
#define D3D12_RESOURCE_STATE_DEPTH_READ          0x20
#define D3D12_RESOURCE_STATE_PRESENT             0
#define D3D12_RESOURCE_STATE_COPY_DEST           0x400
#define D3D12_RESOURCE_STATE_COPY_SOURCE         0x800

// D3D12_DESCRIPTOR_HEAP_TYPE.
#define D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV   0
#define D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER       1
#define D3D12_DESCRIPTOR_HEAP_TYPE_RTV           2
#define D3D12_DESCRIPTOR_HEAP_TYPE_DSV           3

// D3D_ROOT_SIGNATURE_VERSION.
#define D3D_ROOT_SIGNATURE_VERSION_1             0x1
#define D3D_ROOT_SIGNATURE_VERSION_1_0           0x1
#define D3D_ROOT_SIGNATURE_VERSION_1_1           0x2

// ============================================================================
//  Estruturas D3D12 publicas (subset minimo necessario).
// ============================================================================
#pragma pack(push, 8)

typedef struct D3D12_COMMAND_QUEUE_DESC {
    UINT  Type;       // D3D12_COMMAND_LIST_TYPE
    INT   Priority;
    UINT  Flags;
    UINT  NodeMask;
} D3D12_COMMAND_QUEUE_DESC;

typedef struct D3D12_DESCRIPTOR_HEAP_DESC {
    UINT  Type;       // D3D12_DESCRIPTOR_HEAP_TYPE
    UINT  NumDescriptors;
    UINT  Flags;
    UINT  NodeMask;
} D3D12_DESCRIPTOR_HEAP_DESC;

typedef struct D3D12_RESOURCE_DESC {
    UINT     Dimension;        // 0=BUFFER, 1=TEX1D, 2=TEX2D, 3=TEX3D
    UINT64   Alignment;
    UINT64   Width;
    UINT     Height;
    WORD     DepthOrArraySize;
    WORD     MipLevels;
    UINT     Format;            // DXGI_FORMAT
    struct { UINT Count; UINT Quality; } SampleDesc;
    UINT     Layout;
    UINT     Flags;
} D3D12_RESOURCE_DESC;

typedef struct D3D12_HEAP_PROPERTIES {
    UINT     Type;              // D3D12_HEAP_TYPE
    UINT     CPUPageProperty;
    UINT     MemoryPoolPreference;
    UINT     CreationNodeMask;
    UINT     VisibleNodeMask;
} D3D12_HEAP_PROPERTIES;

typedef struct D3D12_CPU_DESCRIPTOR_HANDLE { SIZE_T ptr; } D3D12_CPU_DESCRIPTOR_HANDLE;
typedef struct D3D12_GPU_DESCRIPTOR_HANDLE { UINT64 ptr; } D3D12_GPU_DESCRIPTOR_HANDLE;

typedef struct D3D12_VIEWPORT {
    FLOAT TopLeftX;
    FLOAT TopLeftY;
    FLOAT Width;
    FLOAT Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
} D3D12_VIEWPORT;

typedef struct D3D12_RECT { LONG left, top, right, bottom; } D3D12_RECT;

typedef struct D3D12_RESOURCE_BARRIER {
    UINT Type;      // 0=TRANSITION, 1=ALIASING, 2=UAV
    UINT Flags;
    union {
        struct { void* pResource; UINT Subresource; UINT StateBefore; UINT StateAfter; } Transition;
        struct { void* pResourceBefore; void* pResourceAfter; } Aliasing;
        struct { void* pResource; } UAV;
    } u;
} D3D12_RESOURCE_BARRIER;

#pragma pack(pop)

// ============================================================================
//  Forward decls.
// ============================================================================
struct ID3D12DeviceImpl;
struct ID3D12CommandQueueImpl;
struct ID3D12CommandAllocatorImpl;
struct ID3D12CommandListImpl;
struct ID3D12FenceImpl;
struct ID3D12ResourceImpl;
struct ID3D12PipelineStateImpl;
struct ID3D12RootSignatureImpl;
struct ID3D12DescriptorHeapImpl;
struct ID3DBlobImpl;

struct ID3D12DeviceVtbl;
struct ID3D12CommandQueueVtbl;
struct ID3D12CommandAllocatorVtbl;
struct ID3D12CommandListVtbl;
struct ID3D12FenceVtbl;
struct ID3D12ResourceVtbl;
struct ID3D12PipelineStateVtbl;
struct ID3D12RootSignatureVtbl;
struct ID3D12DescriptorHeapVtbl;
struct ID3DBlobVtbl;

// ============================================================================
//  POOLS ESTATICOS de objetos (sem heap em ring 3).
// ============================================================================
#define MAX_DEVICES          4
#define MAX_QUEUES           8
#define MAX_ALLOCATORS      16
#define MAX_LISTS           32
#define MAX_FENCES          16
#define MAX_RESOURCES       64
#define MAX_PIPELINES       16
#define MAX_ROOTSIGS        16
#define MAX_HEAPS           16
#define MAX_BLOBS           16

// Tag para objetos genericos (RootSig, PipelineState etc compartilham vtable).
#define D12_TAG_RESOURCE     1
#define D12_TAG_PIPELINE     2
#define D12_TAG_ROOTSIG      3
#define D12_TAG_HEAP         4

typedef struct ID3D12FenceImpl {
    const struct ID3D12FenceVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT64 value;        // valor atual do fence (CPU view)
} ID3D12FenceImpl;

typedef struct ID3D12ResourceImpl {
    const struct ID3D12ResourceVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    D3D12_RESOURCE_DESC desc;
    UINT state;          // estado de recurso atual
} ID3D12ResourceImpl;

typedef struct ID3D12PipelineStateImpl {
    const struct ID3D12PipelineStateVtbl* lpVtbl;
    LONG refCount;
    INT  used;
} ID3D12PipelineStateImpl;

typedef struct ID3D12RootSignatureImpl {
    const struct ID3D12RootSignatureVtbl* lpVtbl;
    LONG refCount;
    INT  used;
} ID3D12RootSignatureImpl;

typedef struct ID3D12DescriptorHeapImpl {
    const struct ID3D12DescriptorHeapVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    D3D12_DESCRIPTOR_HEAP_DESC desc;
} ID3D12DescriptorHeapImpl;

typedef struct ID3D12CommandAllocatorImpl {
    const struct ID3D12CommandAllocatorVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT type;           // D3D12_COMMAND_LIST_TYPE
} ID3D12CommandAllocatorImpl;

typedef struct ID3D12CommandListImpl {
    const struct ID3D12CommandListVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT type;
    BOOL recording;      // 0 entre Close() e Reset(); 1 entre Reset() e Close()
} ID3D12CommandListImpl;

typedef struct ID3D12CommandQueueImpl {
    const struct ID3D12CommandQueueVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    D3D12_COMMAND_QUEUE_DESC desc;
} ID3D12CommandQueueImpl;

typedef struct ID3D12DeviceImpl {
    const struct ID3D12DeviceVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT feature_level;
} ID3D12DeviceImpl;

// ID3DBlob — buffer arbitrario retornado por SerializeRootSignature/CompileShader.
typedef struct ID3DBlobImpl {
    const struct ID3DBlobVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    SIZE_T size;
    BYTE data[256];     // pool: 256 bytes embutidos por blob (suficiente para um root sig pequeno)
} ID3DBlobImpl;

static ID3D12DeviceImpl            g_devices    [MAX_DEVICES];
static ID3D12CommandQueueImpl      g_queues     [MAX_QUEUES];
static ID3D12CommandAllocatorImpl  g_allocators [MAX_ALLOCATORS];
static ID3D12CommandListImpl       g_lists      [MAX_LISTS];
static ID3D12FenceImpl             g_fences     [MAX_FENCES];
static ID3D12ResourceImpl          g_resources  [MAX_RESOURCES];
static ID3D12PipelineStateImpl     g_pipelines  [MAX_PIPELINES];
static ID3D12RootSignatureImpl     g_rootsigs   [MAX_ROOTSIGS];
static ID3D12DescriptorHeapImpl    g_heaps      [MAX_HEAPS];
static ID3DBlobImpl                g_blobs      [MAX_BLOBS];

static void mem_zero(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  ID3DBlob vtable.
// ============================================================================
typedef struct ID3DBlobVtbl {
    HRESULT (*QueryInterface)(ID3DBlobImpl* T, REFIID r, void** ppv);
    ULONG   (*AddRef)        (ID3DBlobImpl* T);
    ULONG   (*Release)       (ID3DBlobImpl* T);
    LPVOID  (*GetBufferPointer)(ID3DBlobImpl* T);
    SIZE_T  (*GetBufferSize)   (ID3DBlobImpl* T);
} ID3DBlobVtbl;

static HRESULT Blob_QI(ID3DBlobImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG Blob_AR(ID3DBlobImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Blob_RL(ID3DBlobImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static LPVOID Blob_GetBufferPointer(ID3DBlobImpl* T) { return T->data; }
static SIZE_T Blob_GetBufferSize(ID3DBlobImpl* T) { return T->size; }

static const ID3DBlobVtbl g_blobVtbl = {
    Blob_QI, Blob_AR, Blob_RL, Blob_GetBufferPointer, Blob_GetBufferSize,
};

static ID3DBlobImpl* alloc_blob(SIZE_T sz) {
    for (int i = 0; i < MAX_BLOBS; i++) {
        if (!g_blobs[i].used) {
            mem_zero(&g_blobs[i], sizeof(g_blobs[i]));
            g_blobs[i].used     = 1;
            g_blobs[i].refCount = 1;
            g_blobs[i].lpVtbl   = &g_blobVtbl;
            g_blobs[i].size     = sz > 256 ? 256 : sz;
            return &g_blobs[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12Fence vtable — sync GPU/CPU.
// ============================================================================
typedef struct ID3D12FenceVtbl {
    HRESULT (*QueryInterface)(ID3D12FenceImpl* T, REFIID r, void** ppv);
    ULONG   (*AddRef)        (ID3D12FenceImpl* T);
    ULONG   (*Release)       (ID3D12FenceImpl* T);
    HRESULT (*GetPrivateData)(ID3D12FenceImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12FenceImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12FenceImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12FenceImpl* T, const WCHAR* name);
    HRESULT (*GetDevice)     (ID3D12FenceImpl* T, REFIID r, void** ppv);
    UINT64  (*GetCompletedValue)(ID3D12FenceImpl* T);
    HRESULT (*SetEventOnCompletion)(ID3D12FenceImpl* T, UINT64 value, HANDLE evt);
    HRESULT (*Signal)        (ID3D12FenceImpl* T, UINT64 value);
} ID3D12FenceVtbl;

static HRESULT Fen_QI(ID3D12FenceImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG Fen_AR(ID3D12FenceImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Fen_RL(ID3D12FenceImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Fen_GetPrivateData(ID3D12FenceImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT Fen_SetPrivateData(ID3D12FenceImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT Fen_SetPrivateDataInterface(ID3D12FenceImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT Fen_SetName(ID3D12FenceImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT Fen_GetDevice(ID3D12FenceImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}
// GetCompletedValue: como nao temos GPU, sempre devolve o ultimo valor sinalizado.
static UINT64 Fen_GetCompletedValue(ID3D12FenceImpl* T) { return T->value; }
static HRESULT Fen_SetEventOnCompletion(ID3D12FenceImpl* T, UINT64 v, HANDLE e) {
    (void)T; (void)v; (void)e;
    // Sem scheduler de eventos em ring 3; assume disparado.
    return S_OK;
}
static HRESULT Fen_Signal(ID3D12FenceImpl* T, UINT64 v) { T->value = v; return S_OK; }

static const ID3D12FenceVtbl g_fenceVtbl = {
    Fen_QI, Fen_AR, Fen_RL, Fen_GetPrivateData, Fen_SetPrivateData,
    Fen_SetPrivateDataInterface, Fen_SetName, Fen_GetDevice,
    Fen_GetCompletedValue, Fen_SetEventOnCompletion, Fen_Signal,
};

static ID3D12FenceImpl* alloc_fence(UINT64 initial) {
    for (int i = 0; i < MAX_FENCES; i++) {
        if (!g_fences[i].used) {
            mem_zero(&g_fences[i], sizeof(g_fences[i]));
            g_fences[i].used     = 1;
            g_fences[i].refCount = 1;
            g_fences[i].lpVtbl   = &g_fenceVtbl;
            g_fences[i].value    = initial;
            return &g_fences[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12Resource vtable.
// ============================================================================
typedef struct ID3D12ResourceVtbl {
    HRESULT (*QueryInterface)(ID3D12ResourceImpl* T, REFIID r, void** ppv);
    ULONG   (*AddRef)        (ID3D12ResourceImpl* T);
    ULONG   (*Release)       (ID3D12ResourceImpl* T);
    HRESULT (*GetPrivateData)(ID3D12ResourceImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12ResourceImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12ResourceImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12ResourceImpl* T, const WCHAR* n);
    HRESULT (*GetDevice)     (ID3D12ResourceImpl* T, REFIID r, void** ppv);
    HRESULT (*Map)           (ID3D12ResourceImpl* T, UINT sub, const void* range, void** data);
    void    (*Unmap)         (ID3D12ResourceImpl* T, UINT sub, const void* range);
    D3D12_RESOURCE_DESC (*GetDesc)(ID3D12ResourceImpl* T);
    UINT64  (*GetGPUVirtualAddress)(ID3D12ResourceImpl* T);
    HRESULT (*WriteToSubresource)  (ID3D12ResourceImpl* T, UINT sub, const void* box,
                                    const void* src, UINT srp, UINT ssp);
    HRESULT (*ReadFromSubresource) (ID3D12ResourceImpl* T, void* dst, UINT drp, UINT dsp,
                                    UINT sub, const void* box);
    HRESULT (*GetHeapProperties)   (ID3D12ResourceImpl* T, D3D12_HEAP_PROPERTIES* hp,
                                    UINT* flags);
} ID3D12ResourceVtbl;

static HRESULT R12_QI(ID3D12ResourceImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG R12_AR(ID3D12ResourceImpl* T) { return (ULONG)(++T->refCount); }
static ULONG R12_RL(ID3D12ResourceImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT R12_GetPrivateData(ID3D12ResourceImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT R12_SetPrivateData(ID3D12ResourceImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT R12_SetPrivateDataInterface(ID3D12ResourceImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT R12_SetName(ID3D12ResourceImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT R12_GetDevice(ID3D12ResourceImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}
// Map: sem heap real, devolve NULL — mas mantemos S_OK para nao quebrar fluxo.
static HRESULT R12_Map(ID3D12ResourceImpl* T, UINT s, const void* r, void** d) {
    (void)T; (void)s; (void)r; if (d) *d = 0; return S_OK;
}
static void R12_Unmap(ID3D12ResourceImpl* T, UINT s, const void* r) { (void)T; (void)s; (void)r; }
static D3D12_RESOURCE_DESC R12_GetDesc(ID3D12ResourceImpl* T) { return T->desc; }
// GetGPUVirtualAddress: devolve um valor "razoavel" mas falso.
// Apps DX12 usam isso pra IASetVertexBuffers; nao deferenciamos.
static UINT64 R12_GetGPUVirtualAddress(ID3D12ResourceImpl* T) {
    // Pega o endereco do objeto (CPU) como handle informativo — nao vai ser usado.
    return (UINT64)(unsigned long long)T;
}
static HRESULT R12_WriteToSubresource(ID3D12ResourceImpl* T, UINT s, const void* b,
                                       const void* src, UINT sr, UINT ss) {
    (void)T; (void)s; (void)b; (void)src; (void)sr; (void)ss; return S_OK;
}
static HRESULT R12_ReadFromSubresource(ID3D12ResourceImpl* T, void* d, UINT dr, UINT ds,
                                        UINT s, const void* b) {
    (void)T; (void)dr; (void)ds; (void)s; (void)b;
    if (d) mem_zero(d, 64);
    return S_OK;
}
static HRESULT R12_GetHeapProperties(ID3D12ResourceImpl* T, D3D12_HEAP_PROPERTIES* hp,
                                      UINT* flags) {
    (void)T;
    if (hp) { mem_zero(hp, sizeof(*hp)); hp->Type = D3D12_HEAP_TYPE_DEFAULT; }
    if (flags) *flags = 0;
    return S_OK;
}

static const ID3D12ResourceVtbl g_resourceVtbl = {
    R12_QI, R12_AR, R12_RL,
    R12_GetPrivateData, R12_SetPrivateData, R12_SetPrivateDataInterface, R12_SetName,
    R12_GetDevice, R12_Map, R12_Unmap, R12_GetDesc,
    R12_GetGPUVirtualAddress, R12_WriteToSubresource, R12_ReadFromSubresource,
    R12_GetHeapProperties,
};

static ID3D12ResourceImpl* alloc_resource(const D3D12_RESOURCE_DESC* desc, UINT state) {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (!g_resources[i].used) {
            mem_zero(&g_resources[i], sizeof(g_resources[i]));
            g_resources[i].used     = 1;
            g_resources[i].refCount = 1;
            g_resources[i].lpVtbl   = &g_resourceVtbl;
            g_resources[i].state    = state;
            if (desc) g_resources[i].desc = *desc;
            return &g_resources[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12PipelineState vtable.
// ============================================================================
typedef struct ID3D12PipelineStateVtbl {
    HRESULT (*QueryInterface)(ID3D12PipelineStateImpl* T, REFIID r, void** p);
    ULONG   (*AddRef)        (ID3D12PipelineStateImpl* T);
    ULONG   (*Release)       (ID3D12PipelineStateImpl* T);
    HRESULT (*GetPrivateData)(ID3D12PipelineStateImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12PipelineStateImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12PipelineStateImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12PipelineStateImpl* T, const WCHAR* n);
    HRESULT (*GetDevice)     (ID3D12PipelineStateImpl* T, REFIID r, void** p);
    HRESULT (*GetCachedBlob) (ID3D12PipelineStateImpl* T, ID3DBlobImpl** out);
} ID3D12PipelineStateVtbl;

static HRESULT PS12_QI(ID3D12PipelineStateImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG PS12_AR(ID3D12PipelineStateImpl* T) { return (ULONG)(++T->refCount); }
static ULONG PS12_RL(ID3D12PipelineStateImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT PS12_GetPrivateData(ID3D12PipelineStateImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT PS12_SetPrivateData(ID3D12PipelineStateImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT PS12_SetPrivateDataInterface(ID3D12PipelineStateImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT PS12_SetName(ID3D12PipelineStateImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT PS12_GetDevice(ID3D12PipelineStateImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}
static HRESULT PS12_GetCachedBlob(ID3D12PipelineStateImpl* T, ID3DBlobImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    *out = alloc_blob(0);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static const ID3D12PipelineStateVtbl g_pipelineVtbl = {
    PS12_QI, PS12_AR, PS12_RL,
    PS12_GetPrivateData, PS12_SetPrivateData, PS12_SetPrivateDataInterface, PS12_SetName,
    PS12_GetDevice, PS12_GetCachedBlob,
};

static ID3D12PipelineStateImpl* alloc_pipeline(void) {
    for (int i = 0; i < MAX_PIPELINES; i++) {
        if (!g_pipelines[i].used) {
            mem_zero(&g_pipelines[i], sizeof(g_pipelines[i]));
            g_pipelines[i].used     = 1;
            g_pipelines[i].refCount = 1;
            g_pipelines[i].lpVtbl   = &g_pipelineVtbl;
            return &g_pipelines[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12RootSignature vtable.
// ============================================================================
typedef struct ID3D12RootSignatureVtbl {
    HRESULT (*QueryInterface)(ID3D12RootSignatureImpl* T, REFIID r, void** p);
    ULONG   (*AddRef)        (ID3D12RootSignatureImpl* T);
    ULONG   (*Release)       (ID3D12RootSignatureImpl* T);
    HRESULT (*GetPrivateData)(ID3D12RootSignatureImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12RootSignatureImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12RootSignatureImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12RootSignatureImpl* T, const WCHAR* n);
    HRESULT (*GetDevice)     (ID3D12RootSignatureImpl* T, REFIID r, void** p);
} ID3D12RootSignatureVtbl;

static HRESULT RS12_QI(ID3D12RootSignatureImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG RS12_AR(ID3D12RootSignatureImpl* T) { return (ULONG)(++T->refCount); }
static ULONG RS12_RL(ID3D12RootSignatureImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT RS12_GetPrivateData(ID3D12RootSignatureImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT RS12_SetPrivateData(ID3D12RootSignatureImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT RS12_SetPrivateDataInterface(ID3D12RootSignatureImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT RS12_SetName(ID3D12RootSignatureImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT RS12_GetDevice(ID3D12RootSignatureImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}

static const ID3D12RootSignatureVtbl g_rootsigVtbl = {
    RS12_QI, RS12_AR, RS12_RL,
    RS12_GetPrivateData, RS12_SetPrivateData, RS12_SetPrivateDataInterface, RS12_SetName,
    RS12_GetDevice,
};

static ID3D12RootSignatureImpl* alloc_rootsig(void) {
    for (int i = 0; i < MAX_ROOTSIGS; i++) {
        if (!g_rootsigs[i].used) {
            mem_zero(&g_rootsigs[i], sizeof(g_rootsigs[i]));
            g_rootsigs[i].used     = 1;
            g_rootsigs[i].refCount = 1;
            g_rootsigs[i].lpVtbl   = &g_rootsigVtbl;
            return &g_rootsigs[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12DescriptorHeap vtable.
// ============================================================================
typedef struct ID3D12DescriptorHeapVtbl {
    HRESULT (*QueryInterface)(ID3D12DescriptorHeapImpl* T, REFIID r, void** p);
    ULONG   (*AddRef)        (ID3D12DescriptorHeapImpl* T);
    ULONG   (*Release)       (ID3D12DescriptorHeapImpl* T);
    HRESULT (*GetPrivateData)(ID3D12DescriptorHeapImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12DescriptorHeapImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12DescriptorHeapImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12DescriptorHeapImpl* T, const WCHAR* n);
    HRESULT (*GetDevice)     (ID3D12DescriptorHeapImpl* T, REFIID r, void** p);
    D3D12_DESCRIPTOR_HEAP_DESC (*GetDesc)(ID3D12DescriptorHeapImpl* T);
    D3D12_CPU_DESCRIPTOR_HANDLE (*GetCPUDescriptorHandleForHeapStart)(ID3D12DescriptorHeapImpl* T);
    D3D12_GPU_DESCRIPTOR_HANDLE (*GetGPUDescriptorHandleForHeapStart)(ID3D12DescriptorHeapImpl* T);
} ID3D12DescriptorHeapVtbl;

static HRESULT DH_QI(ID3D12DescriptorHeapImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG DH_AR(ID3D12DescriptorHeapImpl* T) { return (ULONG)(++T->refCount); }
static ULONG DH_RL(ID3D12DescriptorHeapImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT DH_GetPrivateData(ID3D12DescriptorHeapImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT DH_SetPrivateData(ID3D12DescriptorHeapImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT DH_SetPrivateDataInterface(ID3D12DescriptorHeapImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT DH_SetName(ID3D12DescriptorHeapImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT DH_GetDevice(ID3D12DescriptorHeapImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}
static D3D12_DESCRIPTOR_HEAP_DESC DH_GetDesc(ID3D12DescriptorHeapImpl* T) { return T->desc; }
// Devolvemos um handle baseado no proprio objeto — informativo, nao dereferenciamos.
static D3D12_CPU_DESCRIPTOR_HANDLE DH_GetCPUHandle(ID3D12DescriptorHeapImpl* T) {
    D3D12_CPU_DESCRIPTOR_HANDLE h; h.ptr = (SIZE_T)(unsigned long long)T; return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE DH_GetGPUHandle(ID3D12DescriptorHeapImpl* T) {
    D3D12_GPU_DESCRIPTOR_HANDLE h; h.ptr = (UINT64)(unsigned long long)T; return h;
}

static const ID3D12DescriptorHeapVtbl g_heapVtbl = {
    DH_QI, DH_AR, DH_RL,
    DH_GetPrivateData, DH_SetPrivateData, DH_SetPrivateDataInterface, DH_SetName,
    DH_GetDevice, DH_GetDesc, DH_GetCPUHandle, DH_GetGPUHandle,
};

static ID3D12DescriptorHeapImpl* alloc_heap(const D3D12_DESCRIPTOR_HEAP_DESC* desc) {
    for (int i = 0; i < MAX_HEAPS; i++) {
        if (!g_heaps[i].used) {
            mem_zero(&g_heaps[i], sizeof(g_heaps[i]));
            g_heaps[i].used     = 1;
            g_heaps[i].refCount = 1;
            g_heaps[i].lpVtbl   = &g_heapVtbl;
            if (desc) g_heaps[i].desc = *desc;
            return &g_heaps[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12CommandAllocator vtable.
// ============================================================================
typedef struct ID3D12CommandAllocatorVtbl {
    HRESULT (*QueryInterface)(ID3D12CommandAllocatorImpl* T, REFIID r, void** p);
    ULONG   (*AddRef)        (ID3D12CommandAllocatorImpl* T);
    ULONG   (*Release)       (ID3D12CommandAllocatorImpl* T);
    HRESULT (*GetPrivateData)(ID3D12CommandAllocatorImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12CommandAllocatorImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12CommandAllocatorImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12CommandAllocatorImpl* T, const WCHAR* n);
    HRESULT (*GetDevice)     (ID3D12CommandAllocatorImpl* T, REFIID r, void** p);
    HRESULT (*Reset)         (ID3D12CommandAllocatorImpl* T);
} ID3D12CommandAllocatorVtbl;

static HRESULT CA_QI(ID3D12CommandAllocatorImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG CA_AR(ID3D12CommandAllocatorImpl* T) { return (ULONG)(++T->refCount); }
static ULONG CA_RL(ID3D12CommandAllocatorImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT CA_GetPrivateData(ID3D12CommandAllocatorImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT CA_SetPrivateData(ID3D12CommandAllocatorImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT CA_SetPrivateDataInterface(ID3D12CommandAllocatorImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT CA_SetName(ID3D12CommandAllocatorImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT CA_GetDevice(ID3D12CommandAllocatorImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}
static HRESULT CA_Reset(ID3D12CommandAllocatorImpl* T) { (void)T; return S_OK; }

static const ID3D12CommandAllocatorVtbl g_allocatorVtbl = {
    CA_QI, CA_AR, CA_RL,
    CA_GetPrivateData, CA_SetPrivateData, CA_SetPrivateDataInterface, CA_SetName,
    CA_GetDevice, CA_Reset,
};

static ID3D12CommandAllocatorImpl* alloc_allocator(UINT type) {
    for (int i = 0; i < MAX_ALLOCATORS; i++) {
        if (!g_allocators[i].used) {
            mem_zero(&g_allocators[i], sizeof(g_allocators[i]));
            g_allocators[i].used     = 1;
            g_allocators[i].refCount = 1;
            g_allocators[i].lpVtbl   = &g_allocatorVtbl;
            g_allocators[i].type     = type;
            return &g_allocators[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12GraphicsCommandList vtable — comandos de recording. Subset minimo
//  dos metodos mais usados. NAO sao executados — apenas registrados.
// ============================================================================
typedef struct ID3D12CommandListVtbl {
    HRESULT (*QueryInterface)(ID3D12CommandListImpl* T, REFIID r, void** p);
    ULONG   (*AddRef)        (ID3D12CommandListImpl* T);
    ULONG   (*Release)       (ID3D12CommandListImpl* T);
    HRESULT (*GetPrivateData)(ID3D12CommandListImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12CommandListImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12CommandListImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12CommandListImpl* T, const WCHAR* n);
    HRESULT (*GetDevice)     (ID3D12CommandListImpl* T, REFIID r, void** p);
    UINT    (*GetType)       (ID3D12CommandListImpl* T);
    // GraphicsCommandList: Close/Reset + comandos.
    HRESULT (*Close)              (ID3D12CommandListImpl* T);
    HRESULT (*Reset)              (ID3D12CommandListImpl* T, ID3D12CommandAllocatorImpl* a,
                                   ID3D12PipelineStateImpl* ps);
    void    (*ClearState)         (ID3D12CommandListImpl* T, ID3D12PipelineStateImpl* ps);
    void    (*DrawInstanced)      (ID3D12CommandListImpl* T, UINT nv, UINT ni, UINT sv, UINT si);
    void    (*DrawIndexedInstanced)(ID3D12CommandListImpl* T, UINT ni, UINT in_, UINT si, INT bv, UINT sii);
    void    (*Dispatch)           (ID3D12CommandListImpl* T, UINT x, UINT y, UINT z);
    void    (*CopyBufferRegion)   (ID3D12CommandListImpl* T, ID3D12ResourceImpl* dst,
                                   UINT64 ofs_dst, ID3D12ResourceImpl* src,
                                   UINT64 ofs_src, UINT64 nb);
    void    (*CopyTextureRegion)  (ID3D12CommandListImpl* T, const void* dst,
                                   UINT x, UINT y, UINT z, const void* src,
                                   const void* box);
    void    (*CopyResource)       (ID3D12CommandListImpl* T, ID3D12ResourceImpl* dst,
                                   ID3D12ResourceImpl* src);
    void    (*ResourceBarrier)    (ID3D12CommandListImpl* T, UINT n,
                                   const D3D12_RESOURCE_BARRIER* barriers);
    void    (*ClearRenderTargetView)(ID3D12CommandListImpl* T, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                     const FLOAT color[4], UINT n_rects,
                                     const D3D12_RECT* rects);
    void    (*ClearDepthStencilView)(ID3D12CommandListImpl* T, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                     UINT flags, FLOAT depth, BYTE stencil,
                                     UINT n_rects, const D3D12_RECT* rects);
    void    (*RSSetViewports)     (ID3D12CommandListImpl* T, UINT n, const D3D12_VIEWPORT* vp);
    void    (*RSSetScissorRects)  (ID3D12CommandListImpl* T, UINT n, const D3D12_RECT* r);
    void    (*OMSetRenderTargets) (ID3D12CommandListImpl* T, UINT n,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_handle,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
    void    (*SetPipelineState)   (ID3D12CommandListImpl* T, ID3D12PipelineStateImpl* ps);
    void    (*SetGraphicsRootSignature)(ID3D12CommandListImpl* T, ID3D12RootSignatureImpl* rs);
    void    (*SetComputeRootSignature)(ID3D12CommandListImpl* T, ID3D12RootSignatureImpl* rs);
    void    (*SetDescriptorHeaps) (ID3D12CommandListImpl* T, UINT n, ID3D12DescriptorHeapImpl** heaps);
    void    (*IASetPrimitiveTopology)(ID3D12CommandListImpl* T, UINT topo);
    void    (*IASetVertexBuffers) (ID3D12CommandListImpl* T, UINT slot, UINT n, const void* views);
    void    (*IASetIndexBuffer)   (ID3D12CommandListImpl* T, const void* view);
    void    (*ExecuteBundle)      (ID3D12CommandListImpl* T, ID3D12CommandListImpl* bundle);
} ID3D12CommandListVtbl;

static HRESULT CL_QI(ID3D12CommandListImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG CL_AR(ID3D12CommandListImpl* T) { return (ULONG)(++T->refCount); }
static ULONG CL_RL(ID3D12CommandListImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT CL_GetPrivateData(ID3D12CommandListImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT CL_SetPrivateData(ID3D12CommandListImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT CL_SetPrivateDataInterface(ID3D12CommandListImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT CL_SetName(ID3D12CommandListImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT CL_GetDevice(ID3D12CommandListImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}
static UINT CL_GetType(ID3D12CommandListImpl* T) { return T->type; }
// Close/Reset — alternam o flag de recording.
static HRESULT CL_Close(ID3D12CommandListImpl* T) { T->recording = 0; return S_OK; }
static HRESULT CL_Reset(ID3D12CommandListImpl* T, ID3D12CommandAllocatorImpl* a,
                        ID3D12PipelineStateImpl* ps) {
    (void)a; (void)ps; T->recording = 1; return S_OK;
}
// Comandos restantes — apenas no-ops.
static void CL_ClearState(ID3D12CommandListImpl* T, ID3D12PipelineStateImpl* p) {
    (void)T; (void)p;
}
static void CL_DrawInstanced(ID3D12CommandListImpl* T, UINT a, UINT b, UINT c, UINT d) {
    (void)T; (void)a; (void)b; (void)c; (void)d;
}
static void CL_DrawIndexedInstanced(ID3D12CommandListImpl* T, UINT a, UINT b, UINT c, INT d, UINT e) {
    (void)T; (void)a; (void)b; (void)c; (void)d; (void)e;
}
static void CL_Dispatch(ID3D12CommandListImpl* T, UINT a, UINT b, UINT c) {
    (void)T; (void)a; (void)b; (void)c;
}
static void CL_CopyBufferRegion(ID3D12CommandListImpl* T, ID3D12ResourceImpl* d, UINT64 a,
                                 ID3D12ResourceImpl* s, UINT64 b, UINT64 n) {
    (void)T; (void)d; (void)a; (void)s; (void)b; (void)n;
}
static void CL_CopyTextureRegion(ID3D12CommandListImpl* T, const void* d, UINT x, UINT y, UINT z,
                                  const void* s, const void* b) {
    (void)T; (void)d; (void)x; (void)y; (void)z; (void)s; (void)b;
}
static void CL_CopyResource(ID3D12CommandListImpl* T, ID3D12ResourceImpl* d,
                             ID3D12ResourceImpl* s) {
    (void)T; (void)d; (void)s;
}
// ResourceBarrier — atualiza estado dos recursos envolvidos em uma transition.
static void CL_ResourceBarrier(ID3D12CommandListImpl* T, UINT n,
                                const D3D12_RESOURCE_BARRIER* b) {
    (void)T;
    if (!b) return;
    for (UINT i = 0; i < n; i++) {
        if (b[i].Type == 0 /*TRANSITION*/) {
            ID3D12ResourceImpl* r = (ID3D12ResourceImpl*)b[i].u.Transition.pResource;
            if (r && r->used) r->state = b[i].u.Transition.StateAfter;
        }
    }
}
static void CL_ClearRenderTargetView(ID3D12CommandListImpl* T, D3D12_CPU_DESCRIPTOR_HANDLE r,
                                      const FLOAT c[4], UINT n, const D3D12_RECT* rc) {
    (void)T; (void)r; (void)c; (void)n; (void)rc;
}
static void CL_ClearDepthStencilView(ID3D12CommandListImpl* T, D3D12_CPU_DESCRIPTOR_HANDLE d,
                                      UINT f, FLOAT dp, BYTE st, UINT n, const D3D12_RECT* rc) {
    (void)T; (void)d; (void)f; (void)dp; (void)st; (void)n; (void)rc;
}
static void CL_RSSetViewports(ID3D12CommandListImpl* T, UINT n, const D3D12_VIEWPORT* v) {
    (void)T; (void)n; (void)v;
}
static void CL_RSSetScissorRects(ID3D12CommandListImpl* T, UINT n, const D3D12_RECT* r) {
    (void)T; (void)n; (void)r;
}
static void CL_OMSetRenderTargets(ID3D12CommandListImpl* T, UINT n,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, BOOL sh,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    (void)T; (void)n; (void)rtv; (void)sh; (void)dsv;
}
static void CL_SetPipelineState(ID3D12CommandListImpl* T, ID3D12PipelineStateImpl* p) {
    (void)T; (void)p;
}
static void CL_SetGraphicsRootSignature(ID3D12CommandListImpl* T, ID3D12RootSignatureImpl* r) {
    (void)T; (void)r;
}
static void CL_SetComputeRootSignature(ID3D12CommandListImpl* T, ID3D12RootSignatureImpl* r) {
    (void)T; (void)r;
}
static void CL_SetDescriptorHeaps(ID3D12CommandListImpl* T, UINT n, ID3D12DescriptorHeapImpl** h) {
    (void)T; (void)n; (void)h;
}
static void CL_IASetPrimitiveTopology(ID3D12CommandListImpl* T, UINT t) { (void)T; (void)t; }
static void CL_IASetVertexBuffers(ID3D12CommandListImpl* T, UINT s, UINT n, const void* v) {
    (void)T; (void)s; (void)n; (void)v;
}
static void CL_IASetIndexBuffer(ID3D12CommandListImpl* T, const void* v) { (void)T; (void)v; }
static void CL_ExecuteBundle(ID3D12CommandListImpl* T, ID3D12CommandListImpl* b) {
    (void)T; (void)b;
}

static const ID3D12CommandListVtbl g_listVtbl = {
    CL_QI, CL_AR, CL_RL,
    CL_GetPrivateData, CL_SetPrivateData, CL_SetPrivateDataInterface, CL_SetName,
    CL_GetDevice, CL_GetType,
    CL_Close, CL_Reset, CL_ClearState,
    CL_DrawInstanced, CL_DrawIndexedInstanced, CL_Dispatch,
    CL_CopyBufferRegion, CL_CopyTextureRegion, CL_CopyResource,
    CL_ResourceBarrier,
    CL_ClearRenderTargetView, CL_ClearDepthStencilView,
    CL_RSSetViewports, CL_RSSetScissorRects, CL_OMSetRenderTargets,
    CL_SetPipelineState, CL_SetGraphicsRootSignature, CL_SetComputeRootSignature,
    CL_SetDescriptorHeaps, CL_IASetPrimitiveTopology,
    CL_IASetVertexBuffers, CL_IASetIndexBuffer, CL_ExecuteBundle,
};

static ID3D12CommandListImpl* alloc_list(UINT type) {
    for (int i = 0; i < MAX_LISTS; i++) {
        if (!g_lists[i].used) {
            mem_zero(&g_lists[i], sizeof(g_lists[i]));
            g_lists[i].used      = 1;
            g_lists[i].refCount  = 1;
            g_lists[i].lpVtbl    = &g_listVtbl;
            g_lists[i].type      = type;
            g_lists[i].recording = 1;
            return &g_lists[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12CommandQueue vtable.
// ============================================================================
typedef struct ID3D12CommandQueueVtbl {
    HRESULT (*QueryInterface)(ID3D12CommandQueueImpl* T, REFIID r, void** p);
    ULONG   (*AddRef)        (ID3D12CommandQueueImpl* T);
    ULONG   (*Release)       (ID3D12CommandQueueImpl* T);
    HRESULT (*GetPrivateData)(ID3D12CommandQueueImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12CommandQueueImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12CommandQueueImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12CommandQueueImpl* T, const WCHAR* n);
    HRESULT (*GetDevice)     (ID3D12CommandQueueImpl* T, REFIID r, void** p);
    void    (*UpdateTileMappings)  (ID3D12CommandQueueImpl* T, void* a, UINT b, const void* c,
                                    const void* d, void* e, UINT f, const void* g,
                                    const void* h, const UINT64* i, UINT flags);
    void    (*CopyTileMappings)    (ID3D12CommandQueueImpl* T, void* a, const void* b,
                                    void* c, const void* d, const void* e, UINT flags);
    void    (*ExecuteCommandLists) (ID3D12CommandQueueImpl* T, UINT n,
                                    ID3D12CommandListImpl* const* lists);
    void    (*SetMarker)           (ID3D12CommandQueueImpl* T, UINT m, const void* d, UINT s);
    void    (*BeginEvent)          (ID3D12CommandQueueImpl* T, UINT m, const void* d, UINT s);
    void    (*EndEvent)            (ID3D12CommandQueueImpl* T);
    HRESULT (*Signal)              (ID3D12CommandQueueImpl* T, ID3D12FenceImpl* f, UINT64 v);
    HRESULT (*Wait)                (ID3D12CommandQueueImpl* T, ID3D12FenceImpl* f, UINT64 v);
    HRESULT (*GetTimestampFrequency)(ID3D12CommandQueueImpl* T, UINT64* freq);
    HRESULT (*GetClockCalibration) (ID3D12CommandQueueImpl* T, UINT64* gpu, UINT64* cpu);
    D3D12_COMMAND_QUEUE_DESC (*GetDesc)(ID3D12CommandQueueImpl* T);
} ID3D12CommandQueueVtbl;

static HRESULT CQ_QI(ID3D12CommandQueueImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG CQ_AR(ID3D12CommandQueueImpl* T) { return (ULONG)(++T->refCount); }
static ULONG CQ_RL(ID3D12CommandQueueImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT CQ_GetPrivateData(ID3D12CommandQueueImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT CQ_SetPrivateData(ID3D12CommandQueueImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT CQ_SetPrivateDataInterface(ID3D12CommandQueueImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT CQ_SetName(ID3D12CommandQueueImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static HRESULT CQ_GetDevice(ID3D12CommandQueueImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return S_OK;
}
static void CQ_UpdateTileMappings(ID3D12CommandQueueImpl* T, void* a, UINT b, const void* c,
                                   const void* d, void* e, UINT f, const void* g,
                                   const void* h, const UINT64* i, UINT fl) {
    (void)T; (void)a; (void)b; (void)c; (void)d; (void)e;
    (void)f; (void)g; (void)h; (void)i; (void)fl;
}
static void CQ_CopyTileMappings(ID3D12CommandQueueImpl* T, void* a, const void* b,
                                 void* c, const void* d, const void* e, UINT f) {
    (void)T; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
// ExecuteCommandLists — em hardware real submete o batch ao GPU. Aqui no-op.
static void CQ_ExecuteCommandLists(ID3D12CommandQueueImpl* T, UINT n,
                                    ID3D12CommandListImpl* const* lists) {
    (void)T; (void)n; (void)lists;
}
static void CQ_SetMarker(ID3D12CommandQueueImpl* T, UINT m, const void* d, UINT s) {
    (void)T; (void)m; (void)d; (void)s;
}
static void CQ_BeginEvent(ID3D12CommandQueueImpl* T, UINT m, const void* d, UINT s) {
    (void)T; (void)m; (void)d; (void)s;
}
static void CQ_EndEvent(ID3D12CommandQueueImpl* T) { (void)T; }
// Signal/Wait — atualizam o fence imediatamente, sem GPU.
static HRESULT CQ_Signal(ID3D12CommandQueueImpl* T, ID3D12FenceImpl* f, UINT64 v) {
    (void)T; if (f && f->used) f->value = v; return S_OK;
}
static HRESULT CQ_Wait(ID3D12CommandQueueImpl* T, ID3D12FenceImpl* f, UINT64 v) {
    (void)T; (void)f; (void)v; return S_OK;
}
static HRESULT CQ_GetTimestampFrequency(ID3D12CommandQueueImpl* T, UINT64* freq) {
    (void)T; if (freq) *freq = 10000000ULL;     // 10 MHz "GPU clock"
    return S_OK;
}
static HRESULT CQ_GetClockCalibration(ID3D12CommandQueueImpl* T, UINT64* g, UINT64* c) {
    (void)T; if (g) *g = 0; if (c) *c = 0; return S_OK;
}
static D3D12_COMMAND_QUEUE_DESC CQ_GetDesc(ID3D12CommandQueueImpl* T) { return T->desc; }

static const ID3D12CommandQueueVtbl g_queueVtbl = {
    CQ_QI, CQ_AR, CQ_RL,
    CQ_GetPrivateData, CQ_SetPrivateData, CQ_SetPrivateDataInterface, CQ_SetName,
    CQ_GetDevice,
    CQ_UpdateTileMappings, CQ_CopyTileMappings, CQ_ExecuteCommandLists,
    CQ_SetMarker, CQ_BeginEvent, CQ_EndEvent,
    CQ_Signal, CQ_Wait,
    CQ_GetTimestampFrequency, CQ_GetClockCalibration, CQ_GetDesc,
};

static ID3D12CommandQueueImpl* alloc_queue(const D3D12_COMMAND_QUEUE_DESC* desc) {
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (!g_queues[i].used) {
            mem_zero(&g_queues[i], sizeof(g_queues[i]));
            g_queues[i].used     = 1;
            g_queues[i].refCount = 1;
            g_queues[i].lpVtbl   = &g_queueVtbl;
            if (desc) g_queues[i].desc = *desc;
            return &g_queues[i];
        }
    }
    return 0;
}

// ============================================================================
//  ID3D12Device vtable — fabrica central. Subset suficiente para os apps DX12
//  basicos: Create*CommandQueue/Allocator/List + recursos + fences + heaps.
// ============================================================================
typedef struct ID3D12DeviceVtbl {
    HRESULT (*QueryInterface)(ID3D12DeviceImpl* T, REFIID r, void** p);
    ULONG   (*AddRef)        (ID3D12DeviceImpl* T);
    ULONG   (*Release)       (ID3D12DeviceImpl* T);
    HRESULT (*GetPrivateData)(ID3D12DeviceImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*SetPrivateData)(ID3D12DeviceImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(ID3D12DeviceImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*SetName)       (ID3D12DeviceImpl* T, const WCHAR* n);
    UINT    (*GetNodeCount)  (ID3D12DeviceImpl* T);
    HRESULT (*CreateCommandQueue)(ID3D12DeviceImpl* T, const D3D12_COMMAND_QUEUE_DESC* d,
                                  REFIID r, void** q);
    HRESULT (*CreateCommandAllocator)(ID3D12DeviceImpl* T, UINT type, REFIID r, void** alloc);
    HRESULT (*CreateGraphicsPipelineState)(ID3D12DeviceImpl* T, const void* desc, REFIID r, void** ps);
    HRESULT (*CreateComputePipelineState) (ID3D12DeviceImpl* T, const void* desc, REFIID r, void** ps);
    HRESULT (*CreateCommandList)(ID3D12DeviceImpl* T, UINT node, UINT type,
                                  ID3D12CommandAllocatorImpl* a,
                                  ID3D12PipelineStateImpl* ps,
                                  REFIID r, void** out);
    HRESULT (*CheckFeatureSupport)(ID3D12DeviceImpl* T, UINT feature, void* data, UINT size);
    HRESULT (*CreateDescriptorHeap)(ID3D12DeviceImpl* T, const D3D12_DESCRIPTOR_HEAP_DESC* d,
                                     REFIID r, void** out);
    UINT    (*GetDescriptorHandleIncrementSize)(ID3D12DeviceImpl* T, UINT type);
    HRESULT (*CreateRootSignature)(ID3D12DeviceImpl* T, UINT node, const void* sig, SIZE_T sz,
                                    REFIID r, void** out);
    void    (*CreateConstantBufferView)(ID3D12DeviceImpl* T, const void* desc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE h);
    void    (*CreateShaderResourceView) (ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                         const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h);
    void    (*CreateUnorderedAccessView)(ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                         ID3D12ResourceImpl* counter, const void* desc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE h);
    void    (*CreateRenderTargetView)   (ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                         const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h);
    void    (*CreateDepthStencilView)   (ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                         const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h);
    void    (*CreateSampler)            (ID3D12DeviceImpl* T, const void* desc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE h);
    void    (*CopyDescriptors)          (ID3D12DeviceImpl* T, UINT n_dst,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* dst,
                                         const UINT* sizes_dst, UINT n_src,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* src,
                                         const UINT* sizes_src, UINT type);
    void    (*CopyDescriptorsSimple)    (ID3D12DeviceImpl* T, UINT n,
                                         D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                         D3D12_CPU_DESCRIPTOR_HANDLE src, UINT type);
    void    (*GetResourceAllocationInfo)(ID3D12DeviceImpl* T, void* info, UINT node, UINT n,
                                          const void* descs);
    void    (*GetCustomHeapProperties)  (ID3D12DeviceImpl* T, D3D12_HEAP_PROPERTIES* hp,
                                          UINT node, UINT type);
    HRESULT (*CreateCommittedResource)  (ID3D12DeviceImpl* T,
                                          const D3D12_HEAP_PROPERTIES* hp, UINT flags,
                                          const D3D12_RESOURCE_DESC* desc, UINT state,
                                          const void* clear_value, REFIID r, void** out);
    HRESULT (*CreateHeap)               (ID3D12DeviceImpl* T, const void* desc, REFIID r, void** out);
    HRESULT (*CreatePlacedResource)     (ID3D12DeviceImpl* T, void* heap, UINT64 ofs,
                                          const D3D12_RESOURCE_DESC* desc, UINT state,
                                          const void* clear_value, REFIID r, void** out);
    HRESULT (*CreateReservedResource)   (ID3D12DeviceImpl* T, const D3D12_RESOURCE_DESC* desc,
                                          UINT state, const void* clear_value,
                                          REFIID r, void** out);
    HRESULT (*CreateSharedHandle)       (ID3D12DeviceImpl* T, IUnknown* obj, const void* attr,
                                          UINT access, const WCHAR* name, HANDLE* h);
    HRESULT (*OpenSharedHandle)         (ID3D12DeviceImpl* T, HANDLE h, REFIID r, void** out);
    HRESULT (*OpenSharedHandleByName)   (ID3D12DeviceImpl* T, const WCHAR* n, UINT access, HANDLE* h);
    HRESULT (*MakeResident)             (ID3D12DeviceImpl* T, UINT n, IUnknown* const* objs);
    HRESULT (*Evict)                    (ID3D12DeviceImpl* T, UINT n, IUnknown* const* objs);
    HRESULT (*CreateFence)              (ID3D12DeviceImpl* T, UINT64 initial, UINT flags,
                                          REFIID r, void** out);
    HRESULT (*GetDeviceRemovedReason)   (ID3D12DeviceImpl* T);
    void    (*GetCopyableFootprints)    (ID3D12DeviceImpl* T, const D3D12_RESOURCE_DESC* desc,
                                          UINT first, UINT num, UINT64 base,
                                          void* layouts, UINT* row_counts, UINT64* row_sizes,
                                          UINT64* total);
    HRESULT (*CreateQueryHeap)          (ID3D12DeviceImpl* T, const void* desc, REFIID r, void** out);
    HRESULT (*SetStablePowerState)      (ID3D12DeviceImpl* T, BOOL enable);
    HRESULT (*CreateCommandSignature)   (ID3D12DeviceImpl* T, const void* desc,
                                          ID3D12RootSignatureImpl* root, REFIID r, void** out);
    void    (*GetResourceTiling)        (ID3D12DeviceImpl* T, ID3D12ResourceImpl* res, UINT* tiles,
                                          void* packed, void* tile_shape, UINT* nsubres, UINT first,
                                          void* per_sub);
    void    (*GetAdapterLuid)           (ID3D12DeviceImpl* T, void* luid);
} ID3D12DeviceVtbl;

static HRESULT D12_QI(ID3D12DeviceImpl* T, REFIID r, void** p) {
    (void)r; if (!p) return E_POINTER; *p = T; T->refCount++; return S_OK;
}
static ULONG D12_AR(ID3D12DeviceImpl* T) { return (ULONG)(++T->refCount); }
static ULONG D12_RL(ID3D12DeviceImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT D12_GetPrivateData(ID3D12DeviceImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT D12_SetPrivateData(ID3D12DeviceImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT D12_SetPrivateDataInterface(ID3D12DeviceImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT D12_SetName(ID3D12DeviceImpl* T, const WCHAR* n) { (void)T; (void)n; return S_OK; }
static UINT D12_GetNodeCount(ID3D12DeviceImpl* T) { (void)T; return 1; }   // single-GPU.
static HRESULT D12_CreateCommandQueue(ID3D12DeviceImpl* T, const D3D12_COMMAND_QUEUE_DESC* d,
                                       REFIID r, void** q) {
    (void)T; (void)r;
    if (!q) return E_POINTER;
    ID3D12CommandQueueImpl* obj = alloc_queue(d);
    if (!obj) { *q = 0; return E_OUTOFMEMORY; }
    *q = obj; return S_OK;
}
static HRESULT D12_CreateCommandAllocator(ID3D12DeviceImpl* T, UINT type, REFIID r, void** a) {
    (void)T; (void)r;
    if (!a) return E_POINTER;
    ID3D12CommandAllocatorImpl* o = alloc_allocator(type);
    if (!o) { *a = 0; return E_OUTOFMEMORY; }
    *a = o; return S_OK;
}
static HRESULT D12_CreateGraphicsPipelineState(ID3D12DeviceImpl* T, const void* d, REFIID r, void** p) {
    (void)T; (void)d; (void)r;
    if (!p) return E_POINTER;
    ID3D12PipelineStateImpl* o = alloc_pipeline();
    if (!o) { *p = 0; return E_OUTOFMEMORY; }
    *p = o; return S_OK;
}
static HRESULT D12_CreateComputePipelineState(ID3D12DeviceImpl* T, const void* d, REFIID r, void** p) {
    return D12_CreateGraphicsPipelineState(T, d, r, p);
}
static HRESULT D12_CreateCommandList(ID3D12DeviceImpl* T, UINT node, UINT type,
                                      ID3D12CommandAllocatorImpl* a,
                                      ID3D12PipelineStateImpl* ps,
                                      REFIID r, void** out) {
    (void)T; (void)node; (void)a; (void)ps; (void)r;
    if (!out) return E_POINTER;
    ID3D12CommandListImpl* o = alloc_list(type);
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o; return S_OK;
}
// CheckFeatureSupport: zera o buffer e devolve S_OK. Suficiente para apps que
// chamam D3D12_FEATURE_DATA_D3D12_OPTIONS antes de criar resources.
static HRESULT D12_CheckFeatureSupport(ID3D12DeviceImpl* T, UINT feature, void* data, UINT size) {
    (void)T; (void)feature;
    if (data && size) mem_zero(data, size);
    return S_OK;
}
static HRESULT D12_CreateDescriptorHeap(ID3D12DeviceImpl* T, const D3D12_DESCRIPTOR_HEAP_DESC* d,
                                         REFIID r, void** out) {
    (void)T; (void)r;
    if (!out) return E_POINTER;
    ID3D12DescriptorHeapImpl* o = alloc_heap(d);
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o; return S_OK;
}
// GetDescriptorHandleIncrementSize: bytes por descriptor — valores tipicos NVIDIA.
static UINT D12_GetDescriptorHandleIncrementSize(ID3D12DeviceImpl* T, UINT type) {
    (void)T;
    switch (type) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return 32;
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:     return 32;
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:         return 32;
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:         return 32;
        default:                                     return 32;
    }
}
static HRESULT D12_CreateRootSignature(ID3D12DeviceImpl* T, UINT node, const void* sig, SIZE_T sz,
                                        REFIID r, void** out) {
    (void)T; (void)node; (void)sig; (void)sz; (void)r;
    if (!out) return E_POINTER;
    ID3D12RootSignatureImpl* o = alloc_rootsig();
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o; return S_OK;
}
static void D12_CreateConstantBufferView(ID3D12DeviceImpl* T, const void* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)T; (void)d; (void)h;
}
static void D12_CreateShaderResourceView(ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                          const void* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)T; (void)res; (void)d; (void)h;
}
static void D12_CreateUnorderedAccessView(ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                           ID3D12ResourceImpl* cnt, const void* d,
                                           D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)T; (void)res; (void)cnt; (void)d; (void)h;
}
static void D12_CreateRenderTargetView(ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                        const void* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)T; (void)res; (void)d; (void)h;
}
static void D12_CreateDepthStencilView(ID3D12DeviceImpl* T, ID3D12ResourceImpl* res,
                                        const void* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)T; (void)res; (void)d; (void)h;
}
static void D12_CreateSampler(ID3D12DeviceImpl* T, const void* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)T; (void)d; (void)h;
}
static void D12_CopyDescriptors(ID3D12DeviceImpl* T, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst,
                                 const UINT* sd, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* src,
                                 const UINT* ss, UINT type) {
    (void)T; (void)nd; (void)dst; (void)sd; (void)ns; (void)src; (void)ss; (void)type;
}
static void D12_CopyDescriptorsSimple(ID3D12DeviceImpl* T, UINT n,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                       D3D12_CPU_DESCRIPTOR_HANDLE src, UINT type) {
    (void)T; (void)n; (void)dst; (void)src; (void)type;
}
// GetResourceAllocationInfo: devolve um stub fixo no buffer (16 byte struct).
static void D12_GetResourceAllocationInfo(ID3D12DeviceImpl* T, void* info, UINT n, UINT nu,
                                           const void* desc) {
    (void)T; (void)n; (void)nu; (void)desc;
    if (info) {
        UINT64* p = (UINT64*)info;
        p[0] = 64 * 1024;   // SizeInBytes
        p[1] = 65536;       // Alignment
    }
}
static void D12_GetCustomHeapProperties(ID3D12DeviceImpl* T, D3D12_HEAP_PROPERTIES* hp,
                                         UINT n, UINT type) {
    (void)T; (void)n;
    if (hp) { mem_zero(hp, sizeof(*hp)); hp->Type = type; }
}
// CreateCommittedResource — o "atalho" mais comum para criar recursos em DX12.
static HRESULT D12_CreateCommittedResource(ID3D12DeviceImpl* T,
                                            const D3D12_HEAP_PROPERTIES* hp, UINT flags,
                                            const D3D12_RESOURCE_DESC* desc, UINT state,
                                            const void* cv, REFIID r, void** out) {
    (void)T; (void)hp; (void)flags; (void)cv; (void)r;
    if (!out) return E_POINTER;
    ID3D12ResourceImpl* o = alloc_resource(desc, state);
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o; return S_OK;
}
static HRESULT D12_CreateHeap(ID3D12DeviceImpl* T, const void* desc, REFIID r, void** out) {
    (void)T; (void)desc; (void)r;
    if (!out) return E_POINTER;
    ID3D12DescriptorHeapImpl* o = alloc_heap(0);
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o; return S_OK;
}
static HRESULT D12_CreatePlacedResource(ID3D12DeviceImpl* T, void* h, UINT64 o,
                                         const D3D12_RESOURCE_DESC* desc, UINT s,
                                         const void* cv, REFIID r, void** out) {
    (void)T; (void)h; (void)o; (void)cv; (void)r;
    if (!out) return E_POINTER;
    ID3D12ResourceImpl* res = alloc_resource(desc, s);
    if (!res) { *out = 0; return E_OUTOFMEMORY; }
    *out = res; return S_OK;
}
static HRESULT D12_CreateReservedResource(ID3D12DeviceImpl* T, const D3D12_RESOURCE_DESC* desc,
                                           UINT s, const void* cv, REFIID r, void** out) {
    (void)T; (void)cv; (void)r;
    if (!out) return E_POINTER;
    ID3D12ResourceImpl* res = alloc_resource(desc, s);
    if (!res) { *out = 0; return E_OUTOFMEMORY; }
    *out = res; return S_OK;
}
static HRESULT D12_CreateSharedHandle(ID3D12DeviceImpl* T, IUnknown* o, const void* a,
                                       UINT ac, const WCHAR* n, HANDLE* h) {
    (void)T; (void)o; (void)a; (void)ac; (void)n;
    if (h) *h = (HANDLE)1;
    return S_OK;
}
static HRESULT D12_OpenSharedHandle(ID3D12DeviceImpl* T, HANDLE h, REFIID r, void** out) {
    (void)T; (void)h; (void)r;
    if (out) *out = 0;
    return S_OK;
}
static HRESULT D12_OpenSharedHandleByName(ID3D12DeviceImpl* T, const WCHAR* n, UINT a, HANDLE* h) {
    (void)T; (void)n; (void)a;
    if (h) *h = (HANDLE)1;
    return S_OK;
}
static HRESULT D12_MakeResident(ID3D12DeviceImpl* T, UINT n, IUnknown* const* o) {
    (void)T; (void)n; (void)o; return S_OK;
}
static HRESULT D12_Evict(ID3D12DeviceImpl* T, UINT n, IUnknown* const* o) {
    (void)T; (void)n; (void)o; return S_OK;
}
static HRESULT D12_CreateFence(ID3D12DeviceImpl* T, UINT64 initial, UINT flags,
                                REFIID r, void** out) {
    (void)T; (void)flags; (void)r;
    if (!out) return E_POINTER;
    ID3D12FenceImpl* o = alloc_fence(initial);
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o; return S_OK;
}
static HRESULT D12_GetDeviceRemovedReason(ID3D12DeviceImpl* T) { (void)T; return S_OK; }
static void D12_GetCopyableFootprints(ID3D12DeviceImpl* T, const D3D12_RESOURCE_DESC* d,
                                       UINT f, UINT n, UINT64 b, void* l, UINT* rc, UINT64* rs,
                                       UINT64* t) {
    (void)T; (void)d; (void)f; (void)n; (void)b; (void)l;
    if (rc) *rc = 1;
    if (rs) *rs = 256;
    if (t)  *t  = 256;
}
static HRESULT D12_CreateQueryHeap(ID3D12DeviceImpl* T, const void* d, REFIID r, void** out) {
    (void)T; (void)d; (void)r;
    if (!out) return E_POINTER;
    *out = alloc_heap(0);
    return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT D12_SetStablePowerState(ID3D12DeviceImpl* T, BOOL e) { (void)T; (void)e; return S_OK; }
static HRESULT D12_CreateCommandSignature(ID3D12DeviceImpl* T, const void* d,
                                           ID3D12RootSignatureImpl* rs, REFIID r, void** out) {
    (void)T; (void)d; (void)rs; (void)r;
    if (!out) return E_POINTER;
    *out = alloc_rootsig();
    return *out ? S_OK : E_OUTOFMEMORY;
}
static void D12_GetResourceTiling(ID3D12DeviceImpl* T, ID3D12ResourceImpl* r, UINT* tiles,
                                   void* packed, void* shape, UINT* ns, UINT first, void* per) {
    (void)T; (void)r; (void)packed; (void)shape; (void)first; (void)per;
    if (tiles) *tiles = 0;
    if (ns)    *ns    = 0;
}
static void D12_GetAdapterLuid(ID3D12DeviceImpl* T, void* luid) {
    (void)T;
    if (luid) {
        struct { UINT LowPart; LONG HighPart; }* l = luid;
        l->LowPart = 0x1000;
        l->HighPart = 0;
    }
}

static const ID3D12DeviceVtbl g_deviceVtbl = {
    D12_QI, D12_AR, D12_RL,
    D12_GetPrivateData, D12_SetPrivateData, D12_SetPrivateDataInterface, D12_SetName,
    D12_GetNodeCount,
    D12_CreateCommandQueue, D12_CreateCommandAllocator,
    D12_CreateGraphicsPipelineState, D12_CreateComputePipelineState,
    D12_CreateCommandList, D12_CheckFeatureSupport, D12_CreateDescriptorHeap,
    D12_GetDescriptorHandleIncrementSize, D12_CreateRootSignature,
    D12_CreateConstantBufferView, D12_CreateShaderResourceView, D12_CreateUnorderedAccessView,
    D12_CreateRenderTargetView, D12_CreateDepthStencilView, D12_CreateSampler,
    D12_CopyDescriptors, D12_CopyDescriptorsSimple,
    D12_GetResourceAllocationInfo, D12_GetCustomHeapProperties,
    D12_CreateCommittedResource, D12_CreateHeap, D12_CreatePlacedResource,
    D12_CreateReservedResource,
    D12_CreateSharedHandle, D12_OpenSharedHandle, D12_OpenSharedHandleByName,
    D12_MakeResident, D12_Evict, D12_CreateFence,
    D12_GetDeviceRemovedReason, D12_GetCopyableFootprints, D12_CreateQueryHeap,
    D12_SetStablePowerState, D12_CreateCommandSignature, D12_GetResourceTiling,
    D12_GetAdapterLuid,
};

static ID3D12DeviceImpl* alloc_device(UINT feature_level) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            mem_zero(&g_devices[i], sizeof(g_devices[i]));
            g_devices[i].used          = 1;
            g_devices[i].refCount      = 1;
            g_devices[i].lpVtbl        = &g_deviceVtbl;
            g_devices[i].feature_level = feature_level;
            return &g_devices[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados.
//  D3D12CreateDevice(IDXGIAdapter* adapter, D3D_FEATURE_LEVEL min,
//                    REFIID iid, void** ppDevice).
//  D3D12GetDebugInterface(REFIID, void**) — devolve o "debug layer". Aqui um
//    objeto stub que basicamente nao faz nada.
//  D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC*, version,
//                              ID3DBlob** out, ID3DBlob** err).
// ============================================================================

__declspec(dllexport) HRESULT D3D12CreateDevice(void* adapter, UINT min_feature,
                                                 REFIID iid, void** ppDevice) {
    (void)adapter; (void)iid;
    if (!ppDevice) return E_POINTER;
    ID3D12DeviceImpl* d = alloc_device(min_feature ? min_feature : D3D_FEATURE_LEVEL_12_0);
    if (!d) { *ppDevice = 0; return E_OUTOFMEMORY; }
    *ppDevice = d;
    return S_OK;
}

// Debug interface: como nao temos validation layer, devolvemos um "device" como
// stand-in. Apps chamam EnableDebugLayer() na sequencia, que nao precisa fazer
// nada de util — basta a chamada ter sucesso.
__declspec(dllexport) HRESULT D3D12GetDebugInterface(REFIID iid, void** ppDebug) {
    (void)iid;
    if (!ppDebug) return E_POINTER;
    // Recicla a vtable de device (qualquer COM com refcount serve).
    ID3D12DeviceImpl* d = alloc_device(D3D_FEATURE_LEVEL_12_0);
    if (!d) { *ppDebug = 0; return E_OUTOFMEMORY; }
    *ppDebug = d;
    return S_OK;
}

// SerializeRootSignature: o app cria um D3D12_ROOT_SIGNATURE_DESC e essa funcao
// serializa em bytecode opaco. Aqui devolvemos um blob vazio (sem bytecode);
// CreateRootSignature() ja aceita qualquer ponteiro mesmo.
__declspec(dllexport) HRESULT D3D12SerializeRootSignature(const void* desc, UINT version,
                                                          ID3DBlobImpl** ppBlob,
                                                          ID3DBlobImpl** ppError) {
    (void)desc; (void)version;
    if (ppError) *ppError = 0;
    if (!ppBlob) return E_POINTER;
    ID3DBlobImpl* b = alloc_blob(8);
    if (!b) { *ppBlob = 0; return E_OUTOFMEMORY; }
    // Preenche com um cabecalho fake de root signature (informativo).
    b->data[0] = 'R'; b->data[1] = 'S'; b->data[2] = 0x01; b->data[3] = 0;
    *ppBlob = b;
    return S_OK;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
