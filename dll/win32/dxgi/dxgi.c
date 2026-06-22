// dxgi.dll  —  reimplementacao minima da DirectX Graphics Infrastructure 1.x
// (FASE 9.7+). E a "infra" comum por baixo de TODOS os Direct3D 10/11/12 do
// Windows moderno: enumera adapters (placas), outputs (monitores), cria swap
// chains (cadeia de back buffers que vao para a tela via Present) e expoe
// fences/eventos para sincronizar GPU<->CPU.
//
// No Windows real, dxgi.dll vive em RING 3 e fala com o dxgkrnl.sys (kernel)
// para abrir adapters reais. Aqui no MeuOS o dxgkrnl ja foi implementado em
// src/subsystems/dx/dxgkrnl (FASE 9.7) e BasicDisplay/gpu.c provee o backend
// real do framebuffer. As DLLs ring 3, no entanto, NAO chamam o kernel
// diretamente (nao temos syscalls UMD->dxgkrnl ainda); por isso, este stub
// limita-se ao ABI COM (vtable + refcount) e retorna handles fake com dados
// "razoaveis" (vendor/device do Bochs, descricao do BasicDisplay, etc.).
//
// COM ABI (estilo Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg
// "this". Em ABI ms_abi (x86_64-windows-gnu) os parametros entram em
// RCX,RDX,R8,R9 (essa e a ABI que o zig cc gera com -target windows-gnu).
//
// Em DLLs ring 3 do MeuOS nao temos VirtualAlloc/HeapAlloc Win32 reais
// (apenas stubs); por isso usamos POOLS ESTATICOS de objetos. Como sao
// apenas metadados (sem rendering real), os pools sao pequenos e suficientes
// para um app que so quer chamar CreateDXGIFactory + EnumAdapters + GetDesc.
//
// IMAGE BASE: 0x3F00000 — entre ddraw (0x3E00000) e a regiao do PMM
// (>=0x4000000, 64 MiB). Mesmas "zonas mortas" usadas por d3d9/ddraw.

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
typedef void*              HMONITOR;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              REFIID;
typedef void*              REFGUID;
typedef void*              IUnknown;
typedef unsigned char      BYTE;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef unsigned long long ULONG64;

#define S_OK                         0x00000000L
#define S_FALSE                      0x00000001L
#define E_NOTIMPL                    0x80004001L
#define E_NOINTERFACE                0x80004002L
#define E_POINTER                    0x80004003L
#define E_FAIL                       0x80004005L
#define E_INVALIDARG                 0x80070057L
#define E_OUTOFMEMORY                0x8007000EL

// Codigos de erro DXGI especificos (HRESULT FACILITY_DXGI=0x87A).
#define DXGI_ERROR_INVALID_CALL      0x887A0001L
#define DXGI_ERROR_NOT_FOUND         0x887A0002L
#define DXGI_ERROR_MORE_DATA         0x887A0003L
#define DXGI_ERROR_UNSUPPORTED       0x887A0004L
#define DXGI_ERROR_DEVICE_REMOVED    0x887A0005L
#define DXGI_ERROR_DEVICE_HUNG       0x887A0006L
#define DXGI_ERROR_DEVICE_RESET      0x887A0007L
#define DXGI_ERROR_WAS_STILL_DRAWING 0x887A000AL

// Flags de DXGI_PRESENT (subset). 0=apresenta de imediato; DO_NOT_SEQUENCE
// e DXGI_PRESENT_TEST sao no-ops aqui.
#define DXGI_PRESENT_TEST            0x00000001L
#define DXGI_PRESENT_DO_NOT_SEQUENCE 0x00000002L

// DXGI_SWAP_EFFECT (qual a politica do back buffer ao apresentar).
#define DXGI_SWAP_EFFECT_DISCARD             0
#define DXGI_SWAP_EFFECT_SEQUENTIAL          1
#define DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL     3
#define DXGI_SWAP_EFFECT_FLIP_DISCARD        4

// DXGI_FORMAT (subset). 0=UNKNOWN; 87=B8G8R8A8_UNORM (formato padrao XRGB do FB).
#define DXGI_FORMAT_UNKNOWN              0
#define DXGI_FORMAT_R8G8B8A8_UNORM       28
#define DXGI_FORMAT_B8G8R8A8_UNORM       87

// DXGI_USAGE (mascaras). 0x20=DXGI_USAGE_RENDER_TARGET_OUTPUT.
#define DXGI_USAGE_RENDER_TARGET_OUTPUT  0x00000020L
#define DXGI_USAGE_SHADER_INPUT          0x00000010L
#define DXGI_USAGE_BACK_BUFFER           0x00000040L

// DXGI_MODE_SCANLINE_ORDER / DXGI_MODE_SCALING.
#define DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED 0
#define DXGI_MODE_SCALING_UNSPECIFIED        0

// ============================================================================
//  Estruturas DXGI publicas (subset relevante; usadas como out-params).
// ============================================================================
#pragma pack(push, 8)

// DXGI_ADAPTER_DESC — descricao "legacy" (sem flags). DXGI 1.0.
typedef struct DXGI_ADAPTER_DESC {
    WCHAR  Description[128];       // nome do adapter (string UTF-16, terminada em 0)
    UINT   VendorId;
    UINT   DeviceId;
    UINT   SubSysId;
    UINT   Revision;
    UINT64 DedicatedVideoMemory;
    UINT64 DedicatedSystemMemory;
    UINT64 SharedSystemMemory;
    struct { UINT LowPart; LONG HighPart; } AdapterLuid;
} DXGI_ADAPTER_DESC;

// DXGI_ADAPTER_DESC1 — DXGI 1.1: adiciona Flags (SOFTWARE/REMOTE).
typedef struct DXGI_ADAPTER_DESC1 {
    WCHAR  Description[128];
    UINT   VendorId;
    UINT   DeviceId;
    UINT   SubSysId;
    UINT   Revision;
    UINT64 DedicatedVideoMemory;
    UINT64 DedicatedSystemMemory;
    UINT64 SharedSystemMemory;
    struct { UINT LowPart; LONG HighPart; } AdapterLuid;
    UINT   Flags;
} DXGI_ADAPTER_DESC1;

// DXGI_OUTPUT_DESC — descricao do monitor.
typedef struct DXGI_OUTPUT_DESC {
    WCHAR    DeviceName[32];        // nome (ex. L"\\\\.\\DISPLAY1")
    struct { LONG left, top, right, bottom; } DesktopCoordinates;
    BOOL     AttachedToDesktop;
    UINT     Rotation;              // DXGI_MODE_ROTATION_IDENTITY=1
    HMONITOR Monitor;
} DXGI_OUTPUT_DESC;

// DXGI_RATIONAL — fracao numerador/denominador (refresh rate).
typedef struct DXGI_RATIONAL {
    UINT Numerator;
    UINT Denominator;
} DXGI_RATIONAL;

// DXGI_MODE_DESC — modo de tela (resolucao + format + refresh).
typedef struct DXGI_MODE_DESC {
    UINT          Width;
    UINT          Height;
    DXGI_RATIONAL RefreshRate;
    UINT          Format;             // DXGI_FORMAT
    UINT          ScanlineOrdering;   // DXGI_MODE_SCANLINE_ORDER
    UINT          Scaling;            // DXGI_MODE_SCALING
} DXGI_MODE_DESC;

// DXGI_SAMPLE_DESC — multisample (Count=1, Quality=0 -> sem AA).
typedef struct DXGI_SAMPLE_DESC {
    UINT Count;
    UINT Quality;
} DXGI_SAMPLE_DESC;

// DXGI_SWAP_CHAIN_DESC — descricao completa de uma swap chain (usada por
// IDXGIFactory::CreateSwapChain).
typedef struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE_DESC   BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    DWORD            BufferUsage;     // DXGI_USAGE_*
    UINT             BufferCount;     // 1..4 normalmente
    HWND             OutputWindow;
    BOOL             Windowed;
    UINT             SwapEffect;      // DXGI_SWAP_EFFECT_*
    UINT             Flags;
} DXGI_SWAP_CHAIN_DESC;

#pragma pack(pop)

// ============================================================================
//  Forward decls das interfaces (as vtables vem mais abaixo).
// ============================================================================
struct IDXGIFactoryImpl;
struct IDXGIAdapterImpl;
struct IDXGIOutputImpl;
struct IDXGISwapChainImpl;
struct IDXGIFactoryVtbl;
struct IDXGIAdapterVtbl;
struct IDXGIOutputVtbl;
struct IDXGISwapChainVtbl;

// ============================================================================
//  POOL ESTATICO. Sem heap em ring 3 — todas as instancias vivem aqui.
//  Tamanhos definidos pelo briefing da fase: 4 factories / 4 adapters /
//  4 outputs / 8 swap chains. Apenas metadados (sem pixels reais).
// ============================================================================

#define MAX_FACTORIES    4
#define MAX_ADAPTERS     4
#define MAX_OUTPUTS      4
#define MAX_SWAPCHAINS   8

typedef struct IDXGIFactoryImpl {
    const struct IDXGIFactoryVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT version;   // 0=DXGI1.0, 1=1.1, 2=1.2, ...; informativo
} IDXGIFactoryImpl;

typedef struct IDXGIAdapterImpl {
    const struct IDXGIAdapterVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT index;     // ordinal (0 = primario)
} IDXGIAdapterImpl;

typedef struct IDXGIOutputImpl {
    const struct IDXGIOutputVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    IDXGIAdapterImpl* parent;
} IDXGIOutputImpl;

typedef struct IDXGISwapChainImpl {
    const struct IDXGISwapChainVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    DXGI_SWAP_CHAIN_DESC desc;     // copia da descricao usada na criacao
    UINT  current_buffer;          // indice circular do back buffer atual
} IDXGISwapChainImpl;

static IDXGIFactoryImpl   g_factories  [MAX_FACTORIES];
static IDXGIAdapterImpl   g_adapters   [MAX_ADAPTERS];
static IDXGIOutputImpl    g_outputs    [MAX_OUTPUTS];
static IDXGISwapChainImpl g_swapchains [MAX_SWAPCHAINS];

// ----------------------------------------------------------------------------
//  Utilitarios — string e memzero. Sem libc em ring 3.
// ----------------------------------------------------------------------------

// Copia "src" (ASCII, terminada em 0) para "dst" como UTF-16 (cada char vira
// um WCHAR = 16 bits). Usado para preencher Description[128] / DeviceName[32].
// Trunca em "max-1" e termina em 0. max=0 nao faz nada.
static void ascii_to_wide(WCHAR* dst, const char* src, UINT max) {
    if (!dst || !max) return;
    UINT i = 0;
    if (src) {
        while (src[i] && i < max - 1) {
            dst[i] = (WCHAR)(unsigned char)src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void mem_zero(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  Vtable: IDXGIObject (base de todas as interfaces DXGI; SetPrivateData /
//  GetParent etc.). Aqui usamos como parent-interface generica das outras.
//  Mas em vez de implementarmos a hierarquia COM completa (IDXGIObject ->
//  IDXGIDeviceSubObject -> IDXGISwapChain), achatamos: cada interface tem o
//  prefixo IUnknown obrigatorio e seus metodos proprios.
// ============================================================================

// ----------------------------------------------------------------------------
//  IDXGIOutput vtable + impl (vai PRIMEIRO porque IDXGIAdapter usa).
// ----------------------------------------------------------------------------
typedef struct IDXGIOutputVtbl {
    // --- IUnknown (3 entries) ---
    HRESULT (*QueryInterface)(IDXGIOutputImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IDXGIOutputImpl* This);
    ULONG   (*Release)       (IDXGIOutputImpl* This);
    // --- IDXGIObject (4 entries) — SetPrivateData/GetPrivateData/GetParent ---
    HRESULT (*SetPrivateData)(IDXGIOutputImpl* This, REFGUID guid, UINT sz, const void* data);
    HRESULT (*SetPrivateDataInterface)(IDXGIOutputImpl* This, REFGUID guid, const IUnknown* obj);
    HRESULT (*GetPrivateData)(IDXGIOutputImpl* This, REFGUID guid, UINT* sz, void* data);
    HRESULT (*GetParent)     (IDXGIOutputImpl* This, REFIID riid, void** parent);
    // --- IDXGIOutput proprio (9 entries) ---
    HRESULT (*GetDesc)         (IDXGIOutputImpl* This, DXGI_OUTPUT_DESC* desc);
    HRESULT (*GetDisplayModeList)(IDXGIOutputImpl* This, UINT format, UINT flags,
                                  UINT* count, DXGI_MODE_DESC* modes);
    HRESULT (*FindClosestMatchingMode)(IDXGIOutputImpl* This,
                                  const DXGI_MODE_DESC* mode_in,
                                  DXGI_MODE_DESC* closest,
                                  IUnknown* concerned_device);
    HRESULT (*WaitForVBlank)   (IDXGIOutputImpl* This);
    HRESULT (*TakeOwnership)   (IDXGIOutputImpl* This, IUnknown* dev, BOOL exclusive);
    void    (*ReleaseOwnership)(IDXGIOutputImpl* This);
    HRESULT (*GetGammaControlCapabilities)(IDXGIOutputImpl* This, void* caps);
    HRESULT (*SetGammaControl)(IDXGIOutputImpl* This, const void* gamma);
    HRESULT (*GetGammaControl)(IDXGIOutputImpl* This, void* gamma);
} IDXGIOutputVtbl;

static HRESULT Out_QueryInterface(IDXGIOutputImpl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Out_AddRef (IDXGIOutputImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Out_Release(IDXGIOutputImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Out_SetPrivateData(IDXGIOutputImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT Out_SetPrivateDataInterface(IDXGIOutputImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT Out_GetPrivateData(IDXGIOutputImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT Out_GetParent(IDXGIOutputImpl* This, REFIID riid, void** parent) {
    (void)riid;
    if (!parent) return E_POINTER;
    if (This->parent) { This->parent->refCount++; *parent = This->parent; return S_OK; }
    *parent = 0;
    return E_FAIL;
}
// GetDesc: monitor primario do BasicDisplay. DeviceName padrao do NT.
static HRESULT Out_GetDesc(IDXGIOutputImpl* This, DXGI_OUTPUT_DESC* d) {
    (void)This;
    if (!d) return E_POINTER;
    mem_zero(d, sizeof(*d));
    ascii_to_wide(d->DeviceName, "\\\\.\\DISPLAY1", 32);
    d->DesktopCoordinates.left   = 0;
    d->DesktopCoordinates.top    = 0;
    d->DesktopCoordinates.right  = 1024;
    d->DesktopCoordinates.bottom = 768;
    d->AttachedToDesktop = 1;
    d->Rotation = 1;                  // DXGI_MODE_ROTATION_IDENTITY
    d->Monitor  = (HMONITOR)0x1;      // handle fake
    return S_OK;
}
// GetDisplayModeList(format, flags, &count, modes):
//   - modes==0: devolve em *count o numero de modos suportados (1 aqui).
//   - modes!=0: preenche um modo (1024x768 60Hz B8G8R8A8_UNORM).
static HRESULT Out_GetDisplayModeList(IDXGIOutputImpl* T, UINT fmt, UINT flags,
                                      UINT* count, DXGI_MODE_DESC* modes) {
    (void)T; (void)fmt; (void)flags;
    if (!count) return E_POINTER;
    if (!modes) { *count = 1; return S_OK; }
    if (*count < 1)  { *count = 1; return DXGI_ERROR_MORE_DATA; }
    mem_zero(&modes[0], sizeof(modes[0]));
    modes[0].Width            = 1024;
    modes[0].Height           = 768;
    modes[0].RefreshRate.Numerator   = 60;
    modes[0].RefreshRate.Denominator = 1;
    modes[0].Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    modes[0].ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    modes[0].Scaling          = DXGI_MODE_SCALING_UNSPECIFIED;
    *count = 1;
    return S_OK;
}
static HRESULT Out_FindClosestMatchingMode(IDXGIOutputImpl* T,
        const DXGI_MODE_DESC* in_mode, DXGI_MODE_DESC* closest, IUnknown* dev) {
    (void)T; (void)in_mode; (void)dev;
    if (!closest) return E_POINTER;
    mem_zero(closest, sizeof(*closest));
    closest->Width  = 1024;
    closest->Height = 768;
    closest->RefreshRate.Numerator   = 60;
    closest->RefreshRate.Denominator = 1;
    closest->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    return S_OK;
}
static HRESULT Out_WaitForVBlank(IDXGIOutputImpl* T) { (void)T; return S_OK; }
static HRESULT Out_TakeOwnership(IDXGIOutputImpl* T, IUnknown* d, BOOL ex) {
    (void)T; (void)d; (void)ex; return S_OK;
}
static void    Out_ReleaseOwnership(IDXGIOutputImpl* T) { (void)T; }
static HRESULT Out_GetGammaControlCapabilities(IDXGIOutputImpl* T, void* c) {
    (void)T; (void)c; return S_OK;
}
static HRESULT Out_SetGammaControl(IDXGIOutputImpl* T, const void* g) {
    (void)T; (void)g; return S_OK;
}
static HRESULT Out_GetGammaControl(IDXGIOutputImpl* T, void* g) {
    (void)T; (void)g; return S_OK;
}

static const IDXGIOutputVtbl g_outputVtbl = {
    Out_QueryInterface, Out_AddRef, Out_Release,
    Out_SetPrivateData, Out_SetPrivateDataInterface, Out_GetPrivateData, Out_GetParent,
    Out_GetDesc, Out_GetDisplayModeList, Out_FindClosestMatchingMode,
    Out_WaitForVBlank, Out_TakeOwnership, Out_ReleaseOwnership,
    Out_GetGammaControlCapabilities, Out_SetGammaControl, Out_GetGammaControl,
};

static IDXGIOutputImpl* alloc_output(IDXGIAdapterImpl* parent) {
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!g_outputs[i].used) {
            mem_zero(&g_outputs[i], sizeof(g_outputs[i]));
            g_outputs[i].used     = 1;
            g_outputs[i].refCount = 1;
            g_outputs[i].lpVtbl   = &g_outputVtbl;
            g_outputs[i].parent   = parent;
            return &g_outputs[i];
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  IDXGIAdapter vtable + impl.
// ----------------------------------------------------------------------------
typedef struct IDXGIAdapterVtbl {
    HRESULT (*QueryInterface)(IDXGIAdapterImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IDXGIAdapterImpl* This);
    ULONG   (*Release)       (IDXGIAdapterImpl* This);
    HRESULT (*SetPrivateData)(IDXGIAdapterImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(IDXGIAdapterImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*GetPrivateData)(IDXGIAdapterImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*GetParent)     (IDXGIAdapterImpl* T, REFIID r, void** p);
    HRESULT (*EnumOutputs)   (IDXGIAdapterImpl* This, UINT idx, IDXGIOutputImpl** out);
    HRESULT (*GetDesc)       (IDXGIAdapterImpl* This, DXGI_ADAPTER_DESC* desc);
    HRESULT (*CheckInterfaceSupport)(IDXGIAdapterImpl* This, REFGUID iface,
                                     ULONG64* umd_version);
    // IDXGIAdapter1: 1 metodo extra.
    HRESULT (*GetDesc1)      (IDXGIAdapterImpl* This, DXGI_ADAPTER_DESC1* desc);
} IDXGIAdapterVtbl;

static HRESULT Adp_QueryInterface(IDXGIAdapterImpl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Adp_AddRef (IDXGIAdapterImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Adp_Release(IDXGIAdapterImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Adp_SetPrivateData(IDXGIAdapterImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT Adp_SetPrivateDataInterface(IDXGIAdapterImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT Adp_GetPrivateData(IDXGIAdapterImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT Adp_GetParent(IDXGIAdapterImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return E_NOINTERFACE;
}
// EnumOutputs(idx, &out): so existe um monitor (idx=0).
static HRESULT Adp_EnumOutputs(IDXGIAdapterImpl* This, UINT idx, IDXGIOutputImpl** out) {
    if (!out) return E_POINTER;
    if (idx > 0) { *out = 0; return DXGI_ERROR_NOT_FOUND; }
    IDXGIOutputImpl* o = alloc_output(This);
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o;
    return S_OK;
}
// GetDesc: vendor/device 0x1234/0x1111 (Bochs/QEMU VBE — bate com o que o
// HAL PCI enxerga no QEMU std-vga). Description bate com o BasicDisplay.
// DedicatedVideoMemory: 64 MiB (consistente com gpu.c).
static HRESULT Adp_GetDesc(IDXGIAdapterImpl* This, DXGI_ADAPTER_DESC* d) {
    (void)This;
    if (!d) return E_POINTER;
    mem_zero(d, sizeof(*d));
    ascii_to_wide(d->Description, "MeuOS Bochs Display Driver", 128);
    d->VendorId              = 0x1234;
    d->DeviceId              = 0x1111;
    d->SubSysId              = 0x00000000;
    d->Revision              = 0;
    d->DedicatedVideoMemory  = (UINT64)64 * 1024 * 1024;   // 64 MiB
    d->DedicatedSystemMemory = 0;
    d->SharedSystemMemory    = (UINT64)16 * 1024 * 1024;   // 16 MiB
    d->AdapterLuid.LowPart   = 0x1000;
    d->AdapterLuid.HighPart  = 0;
    return S_OK;
}
static HRESULT Adp_CheckInterfaceSupport(IDXGIAdapterImpl* T, REFGUID iface, ULONG64* v) {
    (void)T; (void)iface;
    // "Sim, suportamos qualquer interface" — Direct3D UMD version 1.0.0.0.
    if (v) *v = (((ULONG64)1) << 48);
    return S_OK;
}
// GetDesc1: igual GetDesc + Flags=0 (nao somos software/remote).
static HRESULT Adp_GetDesc1(IDXGIAdapterImpl* This, DXGI_ADAPTER_DESC1* d) {
    if (!d) return E_POINTER;
    mem_zero(d, sizeof(*d));
    ascii_to_wide(d->Description, "MeuOS Bochs Display Driver", 128);
    d->VendorId              = 0x1234;
    d->DeviceId              = 0x1111;
    d->DedicatedVideoMemory  = (UINT64)64 * 1024 * 1024;
    d->SharedSystemMemory    = (UINT64)16 * 1024 * 1024;
    d->AdapterLuid.LowPart   = 0x1000;
    d->AdapterLuid.HighPart  = 0;
    d->Flags                 = 0;     // nao software, nao remote
    (void)This;
    return S_OK;
}

static const IDXGIAdapterVtbl g_adapterVtbl = {
    Adp_QueryInterface, Adp_AddRef, Adp_Release,
    Adp_SetPrivateData, Adp_SetPrivateDataInterface, Adp_GetPrivateData, Adp_GetParent,
    Adp_EnumOutputs, Adp_GetDesc, Adp_CheckInterfaceSupport,
    Adp_GetDesc1,
};

static IDXGIAdapterImpl* alloc_adapter(UINT idx) {
    for (int i = 0; i < MAX_ADAPTERS; i++) {
        if (!g_adapters[i].used) {
            mem_zero(&g_adapters[i], sizeof(g_adapters[i]));
            g_adapters[i].used     = 1;
            g_adapters[i].refCount = 1;
            g_adapters[i].lpVtbl   = &g_adapterVtbl;
            g_adapters[i].index    = idx;
            return &g_adapters[i];
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  IDXGISwapChain vtable + impl.
// ----------------------------------------------------------------------------

// Backend: ponteiro fraco para DxgkPresentDisplayOnly. So fazemos blit se um
// kernel forneceu o symbol (em ring 3 nao temos, entao Present e no-op).
// Definimos como ponteiro fraco que SE existir e chamavel via syscall em
// versoes futuras; por enquanto deixamos NULL e o Present so retorna S_OK.
//
// Quando o caminho UMD->dxgkrnl existir (futuro), podemos plugar aqui.
typedef long (*DxgkPresentDO_fn)(void* /*adapter*/, void* /*src*/,
                                 UINT /*pitch*/, INT /*w*/, INT /*h*/);
static DxgkPresentDO_fn g_dxgk_present_do = 0;

typedef struct IDXGISwapChainVtbl {
    HRESULT (*QueryInterface)(IDXGISwapChainImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IDXGISwapChainImpl* This);
    ULONG   (*Release)       (IDXGISwapChainImpl* This);
    HRESULT (*SetPrivateData)(IDXGISwapChainImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(IDXGISwapChainImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*GetPrivateData)(IDXGISwapChainImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*GetParent)     (IDXGISwapChainImpl* T, REFIID r, void** p);
    HRESULT (*GetDevice)     (IDXGISwapChainImpl* T, REFIID r, void** d);
    // --- IDXGISwapChain proprio ---
    HRESULT (*Present)       (IDXGISwapChainImpl* This, UINT sync_interval, UINT flags);
    HRESULT (*GetBuffer)     (IDXGISwapChainImpl* This, UINT idx, REFIID r, void** surface);
    HRESULT (*SetFullscreenState)(IDXGISwapChainImpl* T, BOOL fs, IDXGIOutputImpl* tgt);
    HRESULT (*GetFullscreenState)(IDXGISwapChainImpl* T, BOOL* fs, IDXGIOutputImpl** tgt);
    HRESULT (*GetDesc)       (IDXGISwapChainImpl* This, DXGI_SWAP_CHAIN_DESC* desc);
    HRESULT (*ResizeBuffers) (IDXGISwapChainImpl* This, UINT count, UINT w, UINT h,
                              UINT fmt, UINT flags);
    HRESULT (*ResizeTarget)  (IDXGISwapChainImpl* This, const DXGI_MODE_DESC* m);
    HRESULT (*GetContainingOutput)(IDXGISwapChainImpl* T, IDXGIOutputImpl** out);
    HRESULT (*GetFrameStatistics)(IDXGISwapChainImpl* T, void* stats);
    HRESULT (*GetLastPresentCount)(IDXGISwapChainImpl* T, UINT* cnt);
} IDXGISwapChainVtbl;

static HRESULT Sc_QueryInterface(IDXGISwapChainImpl* This, REFIID r, void** ppv) {
    (void)r;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Sc_AddRef (IDXGISwapChainImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Sc_Release(IDXGISwapChainImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Sc_SetPrivateData(IDXGISwapChainImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT Sc_SetPrivateDataInterface(IDXGISwapChainImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT Sc_GetPrivateData(IDXGISwapChainImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT Sc_GetParent(IDXGISwapChainImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return E_NOINTERFACE;
}
static HRESULT Sc_GetDevice(IDXGISwapChainImpl* T, REFIID r, void** d) {
    (void)T; (void)r; if (d) *d = 0; return DXGI_ERROR_NOT_FOUND;
}
// Present(sync_interval, flags): se houver um caminho dxgkrnl plugado em runtime
// (via SetDxgkPresentBackend), delega para ele com o "buffer atual"; se nao,
// no-op. flags & DXGI_PRESENT_TEST nao apresenta — so checa.
static HRESULT Sc_Present(IDXGISwapChainImpl* This, UINT sync_interval, UINT flags) {
    (void)sync_interval;
    if (flags & DXGI_PRESENT_TEST) return S_OK;
    if (g_dxgk_present_do) {
        // Backbuffer fake — sem heap nao reservamos memoria; passamos NULL.
        // Em ABI real, GetBuffer alocaria uma ID3D11Texture2D e o app
        // escreveria nela; aqui apenas avisamos o kernel que houve um present.
        g_dxgk_present_do(0, 0, This->desc.BufferDesc.Width * 4,
                          (INT)This->desc.BufferDesc.Width,
                          (INT)This->desc.BufferDesc.Height);
    }
    // Avanca o buffer circular (consistente com SWAP_EFFECT_SEQUENTIAL).
    if (This->desc.BufferCount > 0) {
        This->current_buffer = (This->current_buffer + 1) % This->desc.BufferCount;
    }
    return S_OK;
}
static HRESULT Sc_GetBuffer(IDXGISwapChainImpl* T, UINT idx, REFIID r, void** surf) {
    (void)idx; (void)r;
    if (!surf) return E_POINTER;
    // Devolve o proprio swap chain como "surface" — apps DX11 simples chamam
    // GetBuffer(0, IID_ID3D11Texture2D, &tex); aqui o ponteiro e fake e nao
    // sera dereferenciado por nada exceto por d3d11.dll (que nao existe).
    *surf = T;
    T->refCount++;
    return S_OK;
}
static HRESULT Sc_SetFullscreenState(IDXGISwapChainImpl* T, BOOL fs, IDXGIOutputImpl* tgt) {
    (void)T; (void)fs; (void)tgt; return S_OK;
}
static HRESULT Sc_GetFullscreenState(IDXGISwapChainImpl* T, BOOL* fs, IDXGIOutputImpl** tgt) {
    (void)T;
    if (fs)  *fs  = 0;          // sempre windowed (apps fazem fullscreen via win32k)
    if (tgt) *tgt = 0;
    return S_OK;
}
static HRESULT Sc_GetDesc(IDXGISwapChainImpl* This, DXGI_SWAP_CHAIN_DESC* d) {
    if (!d) return E_POINTER;
    *d = This->desc;
    return S_OK;
}
static HRESULT Sc_ResizeBuffers(IDXGISwapChainImpl* This, UINT count, UINT w, UINT h,
                                 UINT fmt, UINT flags) {
    (void)flags;
    if (count) This->desc.BufferCount = count;
    if (w)     This->desc.BufferDesc.Width  = w;
    if (h)     This->desc.BufferDesc.Height = h;
    if (fmt)   This->desc.BufferDesc.Format = fmt;
    This->current_buffer = 0;
    return S_OK;
}
static HRESULT Sc_ResizeTarget(IDXGISwapChainImpl* This, const DXGI_MODE_DESC* m) {
    if (!m) return E_POINTER;
    This->desc.BufferDesc = *m;
    return S_OK;
}
static HRESULT Sc_GetContainingOutput(IDXGISwapChainImpl* T, IDXGIOutputImpl** out) {
    if (!out) return E_POINTER;
    IDXGIOutputImpl* o = alloc_output(0);
    if (!o) { *out = 0; return E_OUTOFMEMORY; }
    *out = o;
    (void)T;
    return S_OK;
}
static HRESULT Sc_GetFrameStatistics(IDXGISwapChainImpl* T, void* stats) {
    (void)T;
    if (stats) mem_zero(stats, 32);
    return S_OK;
}
static HRESULT Sc_GetLastPresentCount(IDXGISwapChainImpl* T, UINT* cnt) {
    (void)T; if (cnt) *cnt = 0; return S_OK;
}

static const IDXGISwapChainVtbl g_swapchainVtbl = {
    Sc_QueryInterface, Sc_AddRef, Sc_Release,
    Sc_SetPrivateData, Sc_SetPrivateDataInterface, Sc_GetPrivateData, Sc_GetParent,
    Sc_GetDevice,
    Sc_Present, Sc_GetBuffer,
    Sc_SetFullscreenState, Sc_GetFullscreenState,
    Sc_GetDesc, Sc_ResizeBuffers, Sc_ResizeTarget,
    Sc_GetContainingOutput, Sc_GetFrameStatistics, Sc_GetLastPresentCount,
};

static IDXGISwapChainImpl* alloc_swapchain(const DXGI_SWAP_CHAIN_DESC* desc) {
    for (int i = 0; i < MAX_SWAPCHAINS; i++) {
        if (!g_swapchains[i].used) {
            mem_zero(&g_swapchains[i], sizeof(g_swapchains[i]));
            g_swapchains[i].used     = 1;
            g_swapchains[i].refCount = 1;
            g_swapchains[i].lpVtbl   = &g_swapchainVtbl;
            if (desc) g_swapchains[i].desc = *desc;
            // Defaults seguros (apps frequentemente passam BufferCount=0):
            if (g_swapchains[i].desc.BufferCount == 0) g_swapchains[i].desc.BufferCount = 1;
            if (g_swapchains[i].desc.BufferDesc.Width  == 0)
                g_swapchains[i].desc.BufferDesc.Width  = 1024;
            if (g_swapchains[i].desc.BufferDesc.Height == 0)
                g_swapchains[i].desc.BufferDesc.Height = 768;
            return &g_swapchains[i];
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  IDXGIFactory vtable + impl. Inclui metodos da Factory1 (EnumAdapters1 +
//  IsCurrent) — apps que pegam Factory1 e chamam EnumAdapters1 funcionam.
//  Para Factory2+ (CreateSwapChainForHwnd, CreateSwapChainForCoreWindow) ja
//  serao chamadas via QueryInterface e nao via vtable — entao deixamos so o
//  necessario aqui.
// ----------------------------------------------------------------------------
typedef struct IDXGIFactoryVtbl {
    HRESULT (*QueryInterface)(IDXGIFactoryImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IDXGIFactoryImpl* This);
    ULONG   (*Release)       (IDXGIFactoryImpl* This);
    HRESULT (*SetPrivateData)(IDXGIFactoryImpl* T, REFGUID g, UINT s, const void* d);
    HRESULT (*SetPrivateDataInterface)(IDXGIFactoryImpl* T, REFGUID g, const IUnknown* o);
    HRESULT (*GetPrivateData)(IDXGIFactoryImpl* T, REFGUID g, UINT* s, void* d);
    HRESULT (*GetParent)     (IDXGIFactoryImpl* T, REFIID r, void** p);
    // --- IDXGIFactory proprio ---
    HRESULT (*EnumAdapters)        (IDXGIFactoryImpl* This, UINT idx, IDXGIAdapterImpl** out);
    HRESULT (*MakeWindowAssociation)(IDXGIFactoryImpl* T, HWND wnd, UINT flags);
    HRESULT (*GetWindowAssociation) (IDXGIFactoryImpl* T, HWND* wnd);
    HRESULT (*CreateSwapChain)     (IDXGIFactoryImpl* This, IUnknown* device,
                                    DXGI_SWAP_CHAIN_DESC* desc,
                                    IDXGISwapChainImpl** out);
    HRESULT (*CreateSoftwareAdapter)(IDXGIFactoryImpl* This, void* module,
                                     IDXGIAdapterImpl** out);
    // --- IDXGIFactory1 ---
    HRESULT (*EnumAdapters1)(IDXGIFactoryImpl* This, UINT idx, IDXGIAdapterImpl** out);
    BOOL    (*IsCurrent)    (IDXGIFactoryImpl* T);
} IDXGIFactoryVtbl;

static HRESULT Fac_QueryInterface(IDXGIFactoryImpl* This, REFIID riid, void** ppv) {
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++;
    return S_OK;
}
static ULONG Fac_AddRef (IDXGIFactoryImpl* This) { return (ULONG)(++This->refCount); }
static ULONG Fac_Release(IDXGIFactoryImpl* This) {
    LONG n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Fac_SetPrivateData(IDXGIFactoryImpl* T, REFGUID g, UINT s, const void* d) {
    (void)T; (void)g; (void)s; (void)d; return S_OK;
}
static HRESULT Fac_SetPrivateDataInterface(IDXGIFactoryImpl* T, REFGUID g, const IUnknown* o) {
    (void)T; (void)g; (void)o; return S_OK;
}
static HRESULT Fac_GetPrivateData(IDXGIFactoryImpl* T, REFGUID g, UINT* s, void* d) {
    (void)T; (void)g; (void)d; if (s) *s = 0; return S_OK;
}
static HRESULT Fac_GetParent(IDXGIFactoryImpl* T, REFIID r, void** p) {
    (void)T; (void)r; if (p) *p = 0; return E_NOINTERFACE;
}
// EnumAdapters(idx, &out): so existe 1 placa (Bochs); idx>=1 -> NOT_FOUND.
static HRESULT Fac_EnumAdapters(IDXGIFactoryImpl* This, UINT idx, IDXGIAdapterImpl** out) {
    (void)This;
    if (!out) return E_POINTER;
    if (idx > 0) { *out = 0; return DXGI_ERROR_NOT_FOUND; }
    IDXGIAdapterImpl* a = alloc_adapter(idx);
    if (!a) { *out = 0; return E_OUTOFMEMORY; }
    *out = a;
    return S_OK;
}
static HRESULT Fac_MakeWindowAssociation(IDXGIFactoryImpl* T, HWND w, UINT f) {
    (void)T; (void)w; (void)f; return S_OK;
}
static HRESULT Fac_GetWindowAssociation(IDXGIFactoryImpl* T, HWND* w) {
    (void)T; if (w) *w = 0; return S_OK;
}
// CreateSwapChain(device, desc, &out): aloca slot e copia desc.
static HRESULT Fac_CreateSwapChain(IDXGIFactoryImpl* This, IUnknown* device,
                                    DXGI_SWAP_CHAIN_DESC* desc,
                                    IDXGISwapChainImpl** out) {
    (void)This; (void)device;
    if (!out) return E_POINTER;
    IDXGISwapChainImpl* sc = alloc_swapchain(desc);
    if (!sc) { *out = 0; return E_OUTOFMEMORY; }
    *out = sc;
    return S_OK;
}
static HRESULT Fac_CreateSoftwareAdapter(IDXGIFactoryImpl* T, void* mod,
                                          IDXGIAdapterImpl** out) {
    (void)T; (void)mod;
    if (!out) return E_POINTER;
    IDXGIAdapterImpl* a = alloc_adapter(0);
    if (!a) { *out = 0; return E_OUTOFMEMORY; }
    *out = a;
    return S_OK;
}
// EnumAdapters1: igual EnumAdapters (mesmo objeto; GetDesc1 funciona).
static HRESULT Fac_EnumAdapters1(IDXGIFactoryImpl* This, UINT idx, IDXGIAdapterImpl** out) {
    return Fac_EnumAdapters(This, idx, out);
}
static BOOL Fac_IsCurrent(IDXGIFactoryImpl* T) { (void)T; return 1; }

static const IDXGIFactoryVtbl g_factoryVtbl = {
    Fac_QueryInterface, Fac_AddRef, Fac_Release,
    Fac_SetPrivateData, Fac_SetPrivateDataInterface, Fac_GetPrivateData, Fac_GetParent,
    Fac_EnumAdapters, Fac_MakeWindowAssociation, Fac_GetWindowAssociation,
    Fac_CreateSwapChain, Fac_CreateSoftwareAdapter,
    Fac_EnumAdapters1, Fac_IsCurrent,
};

static IDXGIFactoryImpl* alloc_factory(UINT version) {
    for (int i = 0; i < MAX_FACTORIES; i++) {
        if (!g_factories[i].used) {
            mem_zero(&g_factories[i], sizeof(g_factories[i]));
            g_factories[i].used     = 1;
            g_factories[i].refCount = 1;
            g_factories[i].lpVtbl   = &g_factoryVtbl;
            g_factories[i].version  = version;
            return &g_factories[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados — assinaturas BATEM com as dxgi.dll real do Windows.
//  CreateDXGIFactory(riid, &out)         (DXGI 1.0)
//  CreateDXGIFactory1(riid, &out)        (DXGI 1.1)
//  CreateDXGIFactory2(flags, riid, &out) (DXGI 1.3, Win8.1+)
// ============================================================================

__declspec(dllexport) HRESULT CreateDXGIFactory(REFIID riid, void** ppFactory) {
    (void)riid;
    if (!ppFactory) return E_POINTER;
    IDXGIFactoryImpl* f = alloc_factory(0);
    if (!f) { *ppFactory = 0; return E_OUTOFMEMORY; }
    *ppFactory = f;
    return S_OK;
}

__declspec(dllexport) HRESULT CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    (void)riid;
    if (!ppFactory) return E_POINTER;
    IDXGIFactoryImpl* f = alloc_factory(1);
    if (!f) { *ppFactory = 0; return E_OUTOFMEMORY; }
    *ppFactory = f;
    return S_OK;
}

__declspec(dllexport) HRESULT CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
    (void)flags; (void)riid;
    if (!ppFactory) return E_POINTER;
    IDXGIFactoryImpl* f = alloc_factory(2);
    if (!f) { *ppFactory = 0; return E_OUTOFMEMORY; }
    *ppFactory = f;
    return S_OK;
}

// Hook opcional: o kernel (via syscall ou shim) pode plugar aqui o ponteiro
// para DxgkPresentDisplayOnly. Enquanto NAO for chamado, Present() e no-op.
__declspec(dllexport) void DXGISetPresentBackend(void* fn) {
    g_dxgk_present_do = (DxgkPresentDO_fn)fn;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
