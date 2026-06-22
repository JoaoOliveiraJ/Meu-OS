// dxcore.dll  —  reimplementacao minima do DXCore (FASE 9.9).
//
// DXCore foi introduzida no Windows 11 (e tambem disponibilizada via WCOS) como
// uma alternativa LEVE a DXGI para enumeracao de adapters. Diferente da DXGI
// (que vem com swap chains, outputs, frame statistics etc.), DXCore expoe APENAS
// adapters via IDXCoreAdapterFactory + IDXCoreAdapterList + IDXCoreAdapter. Ideal
// para apps headless (D3D12 + compute), servicos, ML (DirectML) e ambientes onde
// DXGI puxaria dependencias desnecessarias do desktop window manager.
//
// Pipeline real: app -> dxcore.dll -> dxgkrnl.sys -> driver. Aqui no MeuOS o
// dxgkrnl ja foi implementado em src/subsystems/dx/dxgkrnl (FASE 9.7); aqui no
// ring 3 esta DLL e um stub COM puro, retornando 1 adapter "BasicDisplay" com
// propriedades hardcoded (mesma estrategia da dxgi.dll).
//
// COM ABI (estilo Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg
// "this". Em ABI ms_abi (x86_64-windows-gnu) os parametros entram em
// RCX,RDX,R8,R9 (essa e a ABI que o zig cc gera com -target windows-gnu).
//
// Pools estaticos pequenos: 4 factories, 4 lists, 4 adapters.
//
// IMAGE BASE: 0x4900000 — sobreposicao com PMM_BASE (0x4000000). Para evitar
// colisao usamos --dynamicbase no build (.reloc), entao o loader pode realocar
// para qualquer endereco virtual livre. Mesmo mecanismo de d3d11/d3d12/d2d1/dwrite.

unsigned int _tls_index = 0;

// ============================================================================
//  Tipos Windows minimos.
// ============================================================================
typedef unsigned int       UINT;
typedef unsigned int       UINT32;
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

#define DXGI_ERROR_NOT_FOUND         0x887A0002L
#define DXGI_ERROR_MORE_DATA         0x887A0003L

// DXCoreAdapterProperty — IDs das propriedades suportadas por GetProperty.
#define DXCoreAdapterProperty_InstanceLuid                  0
#define DXCoreAdapterProperty_DriverVersion                 1
#define DXCoreAdapterProperty_DriverDescription             2
#define DXCoreAdapterProperty_HardwareID                    3
#define DXCoreAdapterProperty_KmdModelVersion               4
#define DXCoreAdapterProperty_ComputePreemptionGranularity  5
#define DXCoreAdapterProperty_GraphicsPreemptionGranularity 6
#define DXCoreAdapterProperty_DedicatedAdapterMemory        7
#define DXCoreAdapterProperty_DedicatedSystemMemory         8
#define DXCoreAdapterProperty_SharedSystemMemory            9
#define DXCoreAdapterProperty_AcgCompatible                10
#define DXCoreAdapterProperty_IsHardware                   11
#define DXCoreAdapterProperty_IsIntegrated                 12
#define DXCoreAdapterProperty_IsDetachable                 13
#define DXCoreAdapterProperty_HardwareIDParts              14
#define DXCoreAdapterProperty_PhysicalAdapterCount         15
#define DXCoreAdapterProperty_AdapterEngineCount           16
#define DXCoreAdapterProperty_AdapterEngineName            17

// DXCoreAdapterState (subset).
#define DXCoreAdapterState_IsDriverUpdateInProgress         0
#define DXCoreAdapterState_AdapterMemoryBudget              1
#define DXCoreAdapterState_AdapterMemoryBudgetNodeSegment   2

// ============================================================================
//  Estruturas DXCore publicas (subset).
// ============================================================================
#pragma pack(push, 8)

// LUID — identificador unico do adapter (64 bits dividos LowPart/HighPart).
typedef struct LUID { UINT LowPart; LONG HighPart; } LUID;

// DXCoreHardwareID — vendor/device/subsys/revision.
typedef struct DXCoreHardwareID {
    UINT32 vendorID;
    UINT32 deviceID;
    UINT32 subSysID;
    UINT32 revision;
} DXCoreHardwareID;

// DXCoreHardwareIDParts (Win11) — vendor/device como UINT32 separados,
// e info do "subVendor"/"subDevice" da subSysID.
typedef struct DXCoreHardwareIDParts {
    UINT32 vendorID;
    UINT32 deviceID;
    UINT32 subSystemID;
    UINT32 subVendorID;
    UINT32 revisionID;
} DXCoreHardwareIDParts;

#pragma pack(pop)

// ============================================================================
//  Forward decls.
// ============================================================================
struct IDXCoreAdapterFactoryImpl;
struct IDXCoreAdapterListImpl;
struct IDXCoreAdapterImpl;

struct IDXCoreAdapterFactoryVtbl;
struct IDXCoreAdapterListVtbl;
struct IDXCoreAdapterVtbl;

// ============================================================================
//  POOLS ESTATICOS.
// ============================================================================
#define MAX_FACTORIES   4
#define MAX_LISTS       4
#define MAX_ADAPTERS    4

typedef struct IDXCoreAdapterImpl {
    const struct IDXCoreAdapterVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT index;
    LUID luid;
    DXCoreHardwareID hwId;
} IDXCoreAdapterImpl;

typedef struct IDXCoreAdapterListImpl {
    const struct IDXCoreAdapterListVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT count;                              // numero de adapters nesta lista
    IDXCoreAdapterImpl* items[MAX_ADAPTERS]; // ponteiros para os impls
} IDXCoreAdapterListImpl;

typedef struct IDXCoreAdapterFactoryImpl {
    const struct IDXCoreAdapterFactoryVtbl* lpVtbl;
    LONG refCount;
    INT  used;
} IDXCoreAdapterFactoryImpl;

static IDXCoreAdapterFactoryImpl g_factories [MAX_FACTORIES];
static IDXCoreAdapterListImpl    g_lists     [MAX_LISTS];
static IDXCoreAdapterImpl        g_adapters  [MAX_ADAPTERS];

// ----------------------------------------------------------------------------
//  Utilitarios.
// ----------------------------------------------------------------------------
static void mem_zero(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

static void mem_copy(void* dst, const void* src, unsigned n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned i = 0; i < n; i++) d[i] = s[i];
}

// Strings de propriedade — ASCII para simplificar (em real seria UTF-8/16).
static UINT str_len_z(const char* s) {
    UINT n = 0;
    if (s) while (s[n]) n++;
    return n + 1; // inclui terminador
}

// ============================================================================
//  Vtable: IDXCoreAdapter — a "placa" enumerada.
// ============================================================================
typedef struct IDXCoreAdapterVtbl {
    HRESULT (*QueryInterface)(IDXCoreAdapterImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IDXCoreAdapterImpl* This);
    ULONG   (*Release)       (IDXCoreAdapterImpl* This);
    BOOL    (*IsValid)       (IDXCoreAdapterImpl* This);
    BOOL    (*IsAttributeSupported)(IDXCoreAdapterImpl* This, REFGUID attribute);
    BOOL    (*IsPropertySupported)(IDXCoreAdapterImpl* This, UINT prop);
    HRESULT (*GetProperty)   (IDXCoreAdapterImpl* This, UINT prop, SIZE_T size, void* out);
    HRESULT (*GetPropertySize)(IDXCoreAdapterImpl* This, UINT prop, SIZE_T* size);
    BOOL    (*IsQueryStateSupported)(IDXCoreAdapterImpl* This, UINT state);
    HRESULT (*QueryState)    (IDXCoreAdapterImpl* This, UINT state, SIZE_T inSize,
                                const void* inBuf, SIZE_T outSize, void* outBuf);
    BOOL    (*IsSetStateSupported)(IDXCoreAdapterImpl* This, UINT state);
    HRESULT (*SetState)      (IDXCoreAdapterImpl* This, UINT state, SIZE_T inSize,
                                const void* inBuf, SIZE_T dataSize, const void* data);
    HRESULT (*GetFactory)    (IDXCoreAdapterImpl* This, REFIID riid, void** out);
} IDXCoreAdapterVtbl;

static HRESULT Ad_QueryInterface(IDXCoreAdapterImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Ad_AddRef (IDXCoreAdapterImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Ad_Release(IDXCoreAdapterImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static BOOL Ad_IsValid(IDXCoreAdapterImpl* T) { return T->used; }
static BOOL Ad_IsAttributeSupported(IDXCoreAdapterImpl* T, REFGUID a) {
    (void)T; (void)a;
    return 1;   // fingimos sempre suportar (D3D12_GRAPHICS, D3D12_CORE_COMPUTE etc.)
}
static BOOL Ad_IsPropertySupported(IDXCoreAdapterImpl* T, UINT prop) {
    (void)T;
    if (prop <= DXCoreAdapterProperty_AdapterEngineName) return 1;
    return 0;
}

// Retorna o tamanho que GetProperty espera para cada PROP.
static HRESULT Ad_GetPropertySize(IDXCoreAdapterImpl* T, UINT prop, SIZE_T* sz) {
    (void)T;
    if (!sz) return E_POINTER;
    switch (prop) {
    case DXCoreAdapterProperty_InstanceLuid:                    *sz = sizeof(LUID); return S_OK;
    case DXCoreAdapterProperty_DriverVersion:                   *sz = sizeof(UINT64); return S_OK;
    case DXCoreAdapterProperty_DriverDescription:               *sz = str_len_z("BasicDisplay (MeuOS)"); return S_OK;
    case DXCoreAdapterProperty_HardwareID:                      *sz = sizeof(DXCoreHardwareID); return S_OK;
    case DXCoreAdapterProperty_KmdModelVersion:                 *sz = sizeof(UINT64); return S_OK;
    case DXCoreAdapterProperty_ComputePreemptionGranularity:    *sz = sizeof(UINT32); return S_OK;
    case DXCoreAdapterProperty_GraphicsPreemptionGranularity:   *sz = sizeof(UINT32); return S_OK;
    case DXCoreAdapterProperty_DedicatedAdapterMemory:          *sz = sizeof(UINT64); return S_OK;
    case DXCoreAdapterProperty_DedicatedSystemMemory:           *sz = sizeof(UINT64); return S_OK;
    case DXCoreAdapterProperty_SharedSystemMemory:              *sz = sizeof(UINT64); return S_OK;
    case DXCoreAdapterProperty_AcgCompatible:                   *sz = sizeof(BOOL); return S_OK;
    case DXCoreAdapterProperty_IsHardware:                      *sz = sizeof(BOOL); return S_OK;
    case DXCoreAdapterProperty_IsIntegrated:                    *sz = sizeof(BOOL); return S_OK;
    case DXCoreAdapterProperty_IsDetachable:                    *sz = sizeof(BOOL); return S_OK;
    case DXCoreAdapterProperty_HardwareIDParts:                 *sz = sizeof(DXCoreHardwareIDParts); return S_OK;
    case DXCoreAdapterProperty_PhysicalAdapterCount:            *sz = sizeof(UINT32); return S_OK;
    case DXCoreAdapterProperty_AdapterEngineCount:              *sz = sizeof(UINT32); return S_OK;
    case DXCoreAdapterProperty_AdapterEngineName:               *sz = str_len_z("3D"); return S_OK;
    default:                                                     *sz = 0; return E_INVALIDARG;
    }
}

// Get a property into out (size must match GetPropertySize).
static HRESULT Ad_GetProperty(IDXCoreAdapterImpl* T, UINT prop, SIZE_T size, void* out) {
    if (!out) return E_POINTER;
    switch (prop) {
    case DXCoreAdapterProperty_InstanceLuid:
        if (size < sizeof(LUID)) return E_INVALIDARG;
        mem_copy(out, &T->luid, sizeof(LUID));
        return S_OK;
    case DXCoreAdapterProperty_DriverVersion: {
        if (size < sizeof(UINT64)) return E_INVALIDARG;
        UINT64 v = ((UINT64)31 << 48) | ((UINT64)0 << 32) | 15 << 16 | 1;  // 31.0.15.1
        mem_copy(out, &v, sizeof(v));
        return S_OK;
    }
    case DXCoreAdapterProperty_DriverDescription: {
        const char* desc = "BasicDisplay (MeuOS)";
        SIZE_T need = str_len_z(desc);
        if (size < need) return E_INVALIDARG;
        mem_copy(out, desc, (unsigned)need);
        return S_OK;
    }
    case DXCoreAdapterProperty_HardwareID:
        if (size < sizeof(DXCoreHardwareID)) return E_INVALIDARG;
        mem_copy(out, &T->hwId, sizeof(DXCoreHardwareID));
        return S_OK;
    case DXCoreAdapterProperty_KmdModelVersion: {
        if (size < sizeof(UINT64)) return E_INVALIDARG;
        UINT64 v = ((UINT64)3 << 32) | 0;     // WDDM 3.0
        mem_copy(out, &v, sizeof(v));
        return S_OK;
    }
    case DXCoreAdapterProperty_ComputePreemptionGranularity:
    case DXCoreAdapterProperty_GraphicsPreemptionGranularity: {
        if (size < sizeof(UINT32)) return E_INVALIDARG;
        UINT32 g = 0;
        mem_copy(out, &g, sizeof(g));
        return S_OK;
    }
    case DXCoreAdapterProperty_DedicatedAdapterMemory: {
        if (size < sizeof(UINT64)) return E_INVALIDARG;
        UINT64 m = (UINT64)16 * 1024 * 1024;   // 16 MiB de VRAM falsa
        mem_copy(out, &m, sizeof(m));
        return S_OK;
    }
    case DXCoreAdapterProperty_DedicatedSystemMemory: {
        if (size < sizeof(UINT64)) return E_INVALIDARG;
        UINT64 m = 0;
        mem_copy(out, &m, sizeof(m));
        return S_OK;
    }
    case DXCoreAdapterProperty_SharedSystemMemory: {
        if (size < sizeof(UINT64)) return E_INVALIDARG;
        UINT64 m = (UINT64)128 * 1024 * 1024;  // 128 MiB shared
        mem_copy(out, &m, sizeof(m));
        return S_OK;
    }
    case DXCoreAdapterProperty_AcgCompatible: {
        if (size < sizeof(BOOL)) return E_INVALIDARG;
        BOOL b = 1; mem_copy(out, &b, sizeof(b));
        return S_OK;
    }
    case DXCoreAdapterProperty_IsHardware: {
        if (size < sizeof(BOOL)) return E_INVALIDARG;
        BOOL b = 0; // software adapter (BasicDisplay)
        mem_copy(out, &b, sizeof(b));
        return S_OK;
    }
    case DXCoreAdapterProperty_IsIntegrated: {
        if (size < sizeof(BOOL)) return E_INVALIDARG;
        BOOL b = 1; mem_copy(out, &b, sizeof(b));
        return S_OK;
    }
    case DXCoreAdapterProperty_IsDetachable: {
        if (size < sizeof(BOOL)) return E_INVALIDARG;
        BOOL b = 0; mem_copy(out, &b, sizeof(b));
        return S_OK;
    }
    case DXCoreAdapterProperty_HardwareIDParts: {
        if (size < sizeof(DXCoreHardwareIDParts)) return E_INVALIDARG;
        DXCoreHardwareIDParts p = { 0x1234, 0x1111, 0, 0x1234, 0 };
        mem_copy(out, &p, sizeof(p));
        return S_OK;
    }
    case DXCoreAdapterProperty_PhysicalAdapterCount: {
        if (size < sizeof(UINT32)) return E_INVALIDARG;
        UINT32 c = 1; mem_copy(out, &c, sizeof(c));
        return S_OK;
    }
    case DXCoreAdapterProperty_AdapterEngineCount: {
        if (size < sizeof(UINT32)) return E_INVALIDARG;
        UINT32 c = 1; mem_copy(out, &c, sizeof(c));
        return S_OK;
    }
    case DXCoreAdapterProperty_AdapterEngineName: {
        const char* en = "3D";
        SIZE_T need = str_len_z(en);
        if (size < need) return E_INVALIDARG;
        mem_copy(out, en, (unsigned)need);
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

static BOOL Ad_IsQueryStateSupported(IDXCoreAdapterImpl* T, UINT state) {
    (void)T; (void)state; return 0;        // nao suportamos states queryaveis
}
static HRESULT Ad_QueryState(IDXCoreAdapterImpl* T, UINT state, SIZE_T isz, const void* ibuf,
                              SIZE_T osz, void* obuf) {
    (void)T; (void)state; (void)isz; (void)ibuf; (void)osz; (void)obuf;
    return E_NOTIMPL;
}
static BOOL Ad_IsSetStateSupported(IDXCoreAdapterImpl* T, UINT state) {
    (void)T; (void)state; return 0;
}
static HRESULT Ad_SetState(IDXCoreAdapterImpl* T, UINT state, SIZE_T isz, const void* ibuf,
                            SIZE_T dsz, const void* data) {
    (void)T; (void)state; (void)isz; (void)ibuf; (void)dsz; (void)data;
    return E_NOTIMPL;
}
static HRESULT Ad_GetFactory(IDXCoreAdapterImpl* T, REFIID r, void** out) {
    (void)T; (void)r;
    if (!out) return E_POINTER;
    *out = &g_factories[0];
    g_factories[0].refCount++;
    return S_OK;
}

static const IDXCoreAdapterVtbl g_adapterVtbl = {
    Ad_QueryInterface, Ad_AddRef, Ad_Release,
    Ad_IsValid, Ad_IsAttributeSupported, Ad_IsPropertySupported,
    Ad_GetProperty, Ad_GetPropertySize,
    Ad_IsQueryStateSupported, Ad_QueryState,
    Ad_IsSetStateSupported, Ad_SetState,
    Ad_GetFactory,
};

static IDXCoreAdapterImpl* alloc_adapter(UINT idx) {
    for (int i = 0; i < MAX_ADAPTERS; i++) {
        if (!g_adapters[i].used) {
            mem_zero(&g_adapters[i], sizeof(g_adapters[i]));
            g_adapters[i].used     = 1;
            g_adapters[i].refCount = 1;
            g_adapters[i].lpVtbl   = &g_adapterVtbl;
            g_adapters[i].index    = idx;
            // LUID arbitrario porem unico (incrementa com idx).
            g_adapters[i].luid.LowPart  = 0xABCD0000 | idx;
            g_adapters[i].luid.HighPart = 0;
            // Vendor/Device — 0x1234 (Bochs/VirtualBox standard); compativel
            // com o que o BasicDisplay reporta no MeuOS.
            g_adapters[i].hwId.vendorID = 0x1234;
            g_adapters[i].hwId.deviceID = 0x1111;
            g_adapters[i].hwId.subSysID = 0;
            g_adapters[i].hwId.revision = 0;
            return &g_adapters[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: IDXCoreAdapterList — colecao de adapters retornada por CreateAdapterList.
// ============================================================================
typedef struct IDXCoreAdapterListVtbl {
    HRESULT (*QueryInterface)(IDXCoreAdapterListImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IDXCoreAdapterListImpl* This);
    ULONG   (*Release)       (IDXCoreAdapterListImpl* This);
    HRESULT (*GetAdapter)    (IDXCoreAdapterListImpl* This, UINT idx, REFIID riid, void** out);
    UINT    (*GetAdapterCount)(IDXCoreAdapterListImpl* This);
    BOOL    (*IsStale)       (IDXCoreAdapterListImpl* This);
    HRESULT (*GetFactory)    (IDXCoreAdapterListImpl* This, REFIID riid, void** out);
    HRESULT (*Sort)          (IDXCoreAdapterListImpl* This, UINT count, const UINT* prefs);
    BOOL    (*IsAdapterPreferenceSupported)(IDXCoreAdapterListImpl* This, UINT pref);
} IDXCoreAdapterListVtbl;

static HRESULT Ls_QueryInterface(IDXCoreAdapterListImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Ls_AddRef (IDXCoreAdapterListImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Ls_Release(IDXCoreAdapterListImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Ls_GetAdapter(IDXCoreAdapterListImpl* T, UINT idx, REFIID r, void** out) {
    (void)r;
    if (!out) return E_POINTER;
    if (idx >= T->count) { *out = 0; return DXGI_ERROR_NOT_FOUND; }
    T->items[idx]->refCount++;
    *out = T->items[idx];
    return S_OK;
}
static UINT Ls_GetAdapterCount(IDXCoreAdapterListImpl* T) { return T->count; }
static BOOL Ls_IsStale(IDXCoreAdapterListImpl* T) { (void)T; return 0; }
static HRESULT Ls_GetFactory(IDXCoreAdapterListImpl* T, REFIID r, void** out) {
    (void)T; (void)r;
    if (!out) return E_POINTER;
    *out = &g_factories[0];
    g_factories[0].refCount++;
    return S_OK;
}
static HRESULT Ls_Sort(IDXCoreAdapterListImpl* T, UINT c, const UINT* p) {
    (void)T; (void)c; (void)p; return S_OK;
}
static BOOL Ls_IsAdapterPreferenceSupported(IDXCoreAdapterListImpl* T, UINT p) {
    (void)T; (void)p; return 0;
}

static const IDXCoreAdapterListVtbl g_listVtbl = {
    Ls_QueryInterface, Ls_AddRef, Ls_Release,
    Ls_GetAdapter, Ls_GetAdapterCount, Ls_IsStale,
    Ls_GetFactory, Ls_Sort, Ls_IsAdapterPreferenceSupported,
};

static IDXCoreAdapterListImpl* alloc_list(void) {
    for (int i = 0; i < MAX_LISTS; i++) {
        if (!g_lists[i].used) {
            mem_zero(&g_lists[i], sizeof(g_lists[i]));
            g_lists[i].used     = 1;
            g_lists[i].refCount = 1;
            g_lists[i].lpVtbl   = &g_listVtbl;
            // Sempre populamos com 1 adapter (BasicDisplay).
            IDXCoreAdapterImpl* a = alloc_adapter(0);
            if (a) {
                g_lists[i].items[0] = a;
                g_lists[i].count    = 1;
            }
            return &g_lists[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: IDXCoreAdapterFactory.
// ============================================================================
typedef struct IDXCoreAdapterFactoryVtbl {
    HRESULT (*QueryInterface)(IDXCoreAdapterFactoryImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IDXCoreAdapterFactoryImpl* This);
    ULONG   (*Release)       (IDXCoreAdapterFactoryImpl* This);
    HRESULT (*CreateAdapterList)(IDXCoreAdapterFactoryImpl* This, UINT numAttrs,
                                   const void* attrs, REFIID riid, void** out);
    HRESULT (*GetAdapterByLuid)(IDXCoreAdapterFactoryImpl* This, const LUID* luid,
                                  REFIID riid, void** out);
    BOOL    (*IsNotificationTypeSupported)(IDXCoreAdapterFactoryImpl* This, UINT type);
    HRESULT (*RegisterEventNotification)(IDXCoreAdapterFactoryImpl* This,
                                           IUnknown* obj, UINT type,
                                           void* callback, void* ctx, UINT* cookie);
    HRESULT (*UnregisterEventNotification)(IDXCoreAdapterFactoryImpl* This, UINT cookie);
} IDXCoreAdapterFactoryVtbl;

static HRESULT Fc_QueryInterface(IDXCoreAdapterFactoryImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Fc_AddRef (IDXCoreAdapterFactoryImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Fc_Release(IDXCoreAdapterFactoryImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Fc_CreateAdapterList(IDXCoreAdapterFactoryImpl* T, UINT n, const void* a,
                                      REFIID r, void** out) {
    (void)T; (void)n; (void)a; (void)r;
    if (!out) return E_POINTER;
    IDXCoreAdapterListImpl* l = alloc_list();
    if (!l) { *out = 0; return E_OUTOFMEMORY; }
    *out = l;
    return S_OK;
}
static HRESULT Fc_GetAdapterByLuid(IDXCoreAdapterFactoryImpl* T, const LUID* luid,
                                     REFIID r, void** out) {
    (void)T; (void)r;
    if (!out) return E_POINTER;
    if (!luid) { *out = 0; return E_POINTER; }
    // Procura no pool ja existente.
    for (int i = 0; i < MAX_ADAPTERS; i++) {
        if (g_adapters[i].used &&
            g_adapters[i].luid.LowPart  == luid->LowPart &&
            g_adapters[i].luid.HighPart == luid->HighPart) {
            g_adapters[i].refCount++;
            *out = &g_adapters[i];
            return S_OK;
        }
    }
    *out = 0;
    return DXGI_ERROR_NOT_FOUND;
}
static BOOL Fc_IsNotificationTypeSupported(IDXCoreAdapterFactoryImpl* T, UINT t) {
    (void)T; (void)t; return 0;
}
static HRESULT Fc_RegisterEventNotification(IDXCoreAdapterFactoryImpl* T, IUnknown* o,
                                              UINT t, void* cb, void* ctx, UINT* c) {
    (void)T; (void)o; (void)t; (void)cb; (void)ctx;
    if (c) *c = 0;
    return S_OK;
}
static HRESULT Fc_UnregisterEventNotification(IDXCoreAdapterFactoryImpl* T, UINT c) {
    (void)T; (void)c; return S_OK;
}

static const IDXCoreAdapterFactoryVtbl g_factoryVtbl = {
    Fc_QueryInterface, Fc_AddRef, Fc_Release,
    Fc_CreateAdapterList, Fc_GetAdapterByLuid,
    Fc_IsNotificationTypeSupported,
    Fc_RegisterEventNotification, Fc_UnregisterEventNotification,
};

static IDXCoreAdapterFactoryImpl* alloc_factory(void) {
    for (int i = 0; i < MAX_FACTORIES; i++) {
        if (!g_factories[i].used) {
            mem_zero(&g_factories[i], sizeof(g_factories[i]));
            g_factories[i].used     = 1;
            g_factories[i].refCount = 1;
            g_factories[i].lpVtbl   = &g_factoryVtbl;
            return &g_factories[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry point exportado — assinatura BATE com dxcore.dll real.
//  DXCoreCreateAdapterFactory(REFIID, void**)
// ============================================================================

__declspec(dllexport) HRESULT DXCoreCreateAdapterFactory(REFIID riid, void** ppFactory) {
    (void)riid;
    if (!ppFactory) return E_POINTER;
    IDXCoreAdapterFactoryImpl* f = alloc_factory();
    if (!f) { *ppFactory = 0; return E_OUTOFMEMORY; }
    *ppFactory = f;
    return S_OK;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
