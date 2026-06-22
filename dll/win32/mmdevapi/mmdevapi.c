// mmdevapi.dll — reimplementacao minima da Multimedia Device API do Windows
// Vista+ (FASE 11 - stack de audio, alinhada com Windows 10/11).
//
// No Windows real, mmdevapi.dll vive em RING 3 e expoe IMMDeviceEnumerator
// (CLSID_MMDeviceEnumerator) para apps que querem listar/abrir dispositivos
// de audio (render = saida; capture = entrada). A enumeracao termina em
// IMMDevice -> Activate(IID_IAudioClient) que entrega um IAudioClient da
// Audioses.dll (WASAPI).
//
// Aqui no MeuOS nao temos PCM real (o driver audio.sys e um stub HD-Audio que
// so detecta o controlador no PCI). Este stub limita-se ao ABI COM (vtable +
// refcount) e devolve handles fake; objetos vivem em pools estaticos.
//
// COM ABI (estilo Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). Em ABI ms_abi (x86_64-
// windows-gnu) os parametros entram em RCX,RDX,R8,R9 — e essa a ABI que o
// zig cc gera com -target windows-gnu.
//
// IMAGE BASE: 0x4A00000 — zona livre apos dxcore (0x4900000), com .reloc via
// --dynamicbase (mesma estrategia das outras DLLs >= PMM_BASE 0x4000000).

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
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef void*              HANDLE;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              REFIID;
typedef void*              REFCLSID;
typedef void*              REFGUID;
typedef void*              IUnknown;
typedef unsigned char      BYTE;
typedef long               LONG;
typedef unsigned long      ULONG;

#define S_OK                         0x00000000L
#define S_FALSE                      0x00000001L
#define E_NOTIMPL                    0x80004001L
#define E_NOINTERFACE                0x80004002L
#define E_POINTER                    0x80004003L
#define E_FAIL                       0x80004005L
#define E_INVALIDARG                 0x80070057L
#define E_OUTOFMEMORY                0x8007000EL

// Constantes WASAPI / mmdevapi.
//
// EDataFlow: render = saida (alto-falante), capture = entrada (microfone),
// all = ambos. Ordem identica a do Windows.
#define eRender    0
#define eCapture   1
#define eAll       2

// ERole: console = uso geral, multimedia = jogos/musica, communications = chat.
#define eConsole         0
#define eMultimedia      1
#define eCommunications  2

// DEVICE_STATE_* — bitmask devolvido por IMMDevice::GetState.
#define DEVICE_STATE_ACTIVE      0x00000001
#define DEVICE_STATE_DISABLED    0x00000002
#define DEVICE_STATE_NOTPRESENT  0x00000004
#define DEVICE_STATE_UNPLUGGED   0x00000008
#define DEVICE_STATEMASK_ALL     0x0000000F

// ============================================================================
//  Forward decls das interfaces.
// ============================================================================
struct IMMDeviceEnumeratorImpl;
struct IMMDeviceCollectionImpl;
struct IMMDeviceImpl;
struct IPropertyStoreImpl;

// ============================================================================
//  POOLs estaticos (sem heap em ring 3).
//
//  Apesar de no MeuOS existir UM unico dispositivo virtual de audio (default),
//  o ABI exige enumerar via collection. Por isso mantemos pools pequenos.
// ============================================================================
#define MAX_ENUMS        4
#define MAX_COLLECTIONS  4
#define MAX_DEVICES      4
#define MAX_PROPSTORES   4

typedef struct IMMDeviceEnumeratorImpl {
    const struct IMMDeviceEnumeratorVtbl* lpVtbl;
    long refCount;
    int  used;
} IMMDeviceEnumeratorImpl;

typedef struct IMMDeviceCollectionImpl {
    const struct IMMDeviceCollectionVtbl* lpVtbl;
    long refCount;
    int  used;
    int  flow;          // eRender / eCapture
    UINT count;         // sempre 1 (o "Default Audio Device")
} IMMDeviceCollectionImpl;

typedef struct IMMDeviceImpl {
    const struct IMMDeviceVtbl* lpVtbl;
    long refCount;
    int  used;
    int  flow;          // eRender / eCapture
    DWORD state;        // DEVICE_STATE_ACTIVE
} IMMDeviceImpl;

typedef struct IPropertyStoreImpl {
    const struct IPropertyStoreVtbl* lpVtbl;
    long refCount;
    int  used;
} IPropertyStoreImpl;

static IMMDeviceEnumeratorImpl  g_enums[MAX_ENUMS];
static IMMDeviceCollectionImpl  g_collections[MAX_COLLECTIONS];
static IMMDeviceImpl            g_devices[MAX_DEVICES];
static IPropertyStoreImpl       g_propstores[MAX_PROPSTORES];

// ============================================================================
//  Vtables.
// ============================================================================
typedef struct IPropertyStoreVtbl {
    HRESULT (*QueryInterface)(IPropertyStoreImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IPropertyStoreImpl* This);
    ULONG   (*Release)       (IPropertyStoreImpl* This);

    HRESULT (*GetCount)      (IPropertyStoreImpl* This, DWORD* count);
    HRESULT (*GetAt)         (IPropertyStoreImpl* This, DWORD idx, void* key);
    HRESULT (*GetValue)      (IPropertyStoreImpl* This, void* key, void* pv);
    HRESULT (*SetValue)      (IPropertyStoreImpl* This, void* key, void* pv);
    HRESULT (*Commit)        (IPropertyStoreImpl* This);
} IPropertyStoreVtbl;

typedef struct IMMDeviceVtbl {
    HRESULT (*QueryInterface)(IMMDeviceImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IMMDeviceImpl* This);
    ULONG   (*Release)       (IMMDeviceImpl* This);

    HRESULT (*Activate)      (IMMDeviceImpl* This, REFIID iid, DWORD ctx,
                              void* params, void** intf);
    HRESULT (*OpenPropertyStore)(IMMDeviceImpl* This, DWORD access,
                              IPropertyStoreImpl** store);
    HRESULT (*GetId)         (IMMDeviceImpl* This, WCHAR** id);
    HRESULT (*GetState)      (IMMDeviceImpl* This, DWORD* state);
} IMMDeviceVtbl;

typedef struct IMMDeviceCollectionVtbl {
    HRESULT (*QueryInterface)(IMMDeviceCollectionImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IMMDeviceCollectionImpl* This);
    ULONG   (*Release)       (IMMDeviceCollectionImpl* This);

    HRESULT (*GetCount)      (IMMDeviceCollectionImpl* This, UINT* count);
    HRESULT (*Item)          (IMMDeviceCollectionImpl* This, UINT idx,
                              IMMDeviceImpl** dev);
} IMMDeviceCollectionVtbl;

typedef struct IMMDeviceEnumeratorVtbl {
    HRESULT (*QueryInterface)(IMMDeviceEnumeratorImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IMMDeviceEnumeratorImpl* This);
    ULONG   (*Release)       (IMMDeviceEnumeratorImpl* This);

    HRESULT (*EnumAudioEndpoints)(IMMDeviceEnumeratorImpl* This, int flow,
                              DWORD stateMask, IMMDeviceCollectionImpl** col);
    HRESULT (*GetDefaultAudioEndpoint)(IMMDeviceEnumeratorImpl* This, int flow,
                              int role, IMMDeviceImpl** dev);
    HRESULT (*GetDevice)     (IMMDeviceEnumeratorImpl* This, WCHAR* id,
                              IMMDeviceImpl** dev);
    HRESULT (*RegisterEndpointNotificationCallback)(IMMDeviceEnumeratorImpl* This,
                              void* cb);
    HRESULT (*UnregisterEndpointNotificationCallback)(IMMDeviceEnumeratorImpl* This,
                              void* cb);
} IMMDeviceEnumeratorVtbl;

// ============================================================================
//  Helpers de alocacao + utilitarios para zerar pools.
// ============================================================================
static void zero_mem(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  IPropertyStore — stub que reporta zero propriedades.
// ============================================================================
static HRESULT PS_QueryInterface(IPropertyStoreImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG PS_AddRef(IPropertyStoreImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG PS_Release(IPropertyStoreImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT PS_GetCount(IPropertyStoreImpl* This, DWORD* c) {
    (void)This; if (c) *c = 0; return S_OK;
}
static HRESULT PS_GetAt(IPropertyStoreImpl* This, DWORD i, void* k) {
    (void)This; (void)i; (void)k; return S_FALSE;
}
static HRESULT PS_GetValue(IPropertyStoreImpl* This, void* k, void* pv) {
    (void)This; (void)k; (void)pv; return S_OK;
}
static HRESULT PS_SetValue(IPropertyStoreImpl* This, void* k, void* pv) {
    (void)This; (void)k; (void)pv; return S_OK;
}
static HRESULT PS_Commit(IPropertyStoreImpl* This) { (void)This; return S_OK; }

static const IPropertyStoreVtbl g_psVtbl = {
    PS_QueryInterface, PS_AddRef, PS_Release,
    PS_GetCount, PS_GetAt, PS_GetValue, PS_SetValue, PS_Commit,
};

static IPropertyStoreImpl* alloc_propstore(void) {
    for (int i = 0; i < MAX_PROPSTORES; i++) {
        if (!g_propstores[i].used) {
            zero_mem(&g_propstores[i], sizeof(g_propstores[i]));
            g_propstores[i].used     = 1;
            g_propstores[i].refCount = 1;
            g_propstores[i].lpVtbl   = &g_psVtbl;
            return &g_propstores[i];
        }
    }
    return 0;
}

// ============================================================================
//  IMMDevice — representa um endpoint de audio (default render/capture).
//
//  Activate(iid, ...) deveria devolver um IAudioClient da Audioses.dll. Para
//  preservar a separacao de modulos (mmdevapi nao importa audioses) e fazer
//  o equivalente do que o NT faz internamente (CoCreateInstance helper), o
//  app real precisa importar Audioses e chamar diretamente; aqui devolvemos
//  ponteiro fake nao-nulo para apps que so checam HRESULT/ppv != NULL.
// ============================================================================
static HRESULT Dev_QueryInterface(IMMDeviceImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG Dev_AddRef(IMMDeviceImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG Dev_Release(IMMDeviceImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}

// IID_IAudioClient = {1CB9AD4C-DBFA-4c32-B178-C2F568A703B2}. Para o stub nao
// distinguir IIDs (Activate aceita IID_IAudioClient, IID_IAudioEndpointVolume,
// IID_IAudioSessionManager, IID_IAudioSessionManager2). Sempre devolve um
// handle fake nao-nulo.
static HRESULT Dev_Activate(IMMDeviceImpl* This, REFIID iid, DWORD ctx,
        void* params, void** intf) {
    (void)This; (void)iid; (void)ctx; (void)params;
    if (!intf) return E_POINTER;
    // Handle "ativo" fake — basta nao ser NULL. Apps que tentem chamar metodos
    // dependem da Audioses.dll (que faz a alocacao real e devolve vtable).
    *intf = (void*)0xAAD10C11ULL;     // "AUDIO-CLI" decifrado em hex valido
    return S_OK;
}
static HRESULT Dev_OpenPropertyStore(IMMDeviceImpl* This, DWORD acc,
        IPropertyStoreImpl** st) {
    (void)This; (void)acc;
    if (!st) return E_POINTER;
    IPropertyStoreImpl* p = alloc_propstore();
    if (!p) { *st = 0; return E_OUTOFMEMORY; }
    *st = p; return S_OK;
}
// GetId: devolve um WCHAR* fake. Apps que tentem CoTaskMemFree o ponteiro
// vao bater num stub que nao libera nada — aceitavel para apps que so logam.
static const WCHAR g_dev_id_default[] = {
    '{', '0', '.', '0', '.', '0', '.', '0', '0', '0', '0', '0', '0', '0', '0', '}',
    '.', '{', 'M', 'e', 'u', 'O', 'S', '-', 'A', 'u', 'd', 'i', 'o', '-', '0', '}',
    0
};
static HRESULT Dev_GetId(IMMDeviceImpl* This, WCHAR** id) {
    (void)This; if (!id) return E_POINTER;
    *id = (WCHAR*)g_dev_id_default;
    return S_OK;
}
static HRESULT Dev_GetState(IMMDeviceImpl* This, DWORD* state) {
    if (!state) return E_POINTER;
    *state = This->state ? This->state : DEVICE_STATE_ACTIVE;
    return S_OK;
}

static const IMMDeviceVtbl g_devVtbl = {
    Dev_QueryInterface, Dev_AddRef, Dev_Release,
    Dev_Activate, Dev_OpenPropertyStore, Dev_GetId, Dev_GetState,
};

static IMMDeviceImpl* alloc_device(int flow) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            zero_mem(&g_devices[i], sizeof(g_devices[i]));
            g_devices[i].used     = 1;
            g_devices[i].refCount = 1;
            g_devices[i].lpVtbl   = &g_devVtbl;
            g_devices[i].flow     = flow;
            g_devices[i].state    = DEVICE_STATE_ACTIVE;
            return &g_devices[i];
        }
    }
    return 0;
}

// ============================================================================
//  IMMDeviceCollection — sempre exatamente 1 dispositivo (default).
// ============================================================================
static HRESULT Col_QueryInterface(IMMDeviceCollectionImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG Col_AddRef(IMMDeviceCollectionImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG Col_Release(IMMDeviceCollectionImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Col_GetCount(IMMDeviceCollectionImpl* This, UINT* count) {
    if (!count) return E_POINTER;
    *count = This->count;
    return S_OK;
}
static HRESULT Col_Item(IMMDeviceCollectionImpl* This, UINT idx, IMMDeviceImpl** dev) {
    if (!dev) return E_POINTER;
    if (idx >= This->count) { *dev = 0; return E_INVALIDARG; }
    IMMDeviceImpl* d = alloc_device(This->flow);
    if (!d) { *dev = 0; return E_OUTOFMEMORY; }
    *dev = d;
    return S_OK;
}

static const IMMDeviceCollectionVtbl g_colVtbl = {
    Col_QueryInterface, Col_AddRef, Col_Release,
    Col_GetCount, Col_Item,
};

static IMMDeviceCollectionImpl* alloc_collection(int flow) {
    for (int i = 0; i < MAX_COLLECTIONS; i++) {
        if (!g_collections[i].used) {
            zero_mem(&g_collections[i], sizeof(g_collections[i]));
            g_collections[i].used     = 1;
            g_collections[i].refCount = 1;
            g_collections[i].lpVtbl   = &g_colVtbl;
            g_collections[i].flow     = flow;
            g_collections[i].count    = 1;
            return &g_collections[i];
        }
    }
    return 0;
}

// ============================================================================
//  IMMDeviceEnumerator — o "ponto de entrada" da API.
//
//  Apps fazem:  CoCreateInstance(CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator)
//  -> EnumAudioEndpoints / GetDefaultAudioEndpoint -> IMMDevice -> Activate
//  -> IAudioClient (Audioses.dll).
// ============================================================================
static HRESULT Enum_QueryInterface(IMMDeviceEnumeratorImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG Enum_AddRef(IMMDeviceEnumeratorImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG Enum_Release(IMMDeviceEnumeratorImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Enum_EnumAudioEndpoints(IMMDeviceEnumeratorImpl* This, int flow,
        DWORD mask, IMMDeviceCollectionImpl** col) {
    (void)This; (void)mask;
    if (!col) return E_POINTER;
    IMMDeviceCollectionImpl* c = alloc_collection(flow);
    if (!c) { *col = 0; return E_OUTOFMEMORY; }
    *col = c; return S_OK;
}
static HRESULT Enum_GetDefaultAudioEndpoint(IMMDeviceEnumeratorImpl* This,
        int flow, int role, IMMDeviceImpl** dev) {
    (void)This; (void)role;
    if (!dev) return E_POINTER;
    IMMDeviceImpl* d = alloc_device(flow);
    if (!d) { *dev = 0; return E_OUTOFMEMORY; }
    *dev = d; return S_OK;
}
static HRESULT Enum_GetDevice(IMMDeviceEnumeratorImpl* This, WCHAR* id,
        IMMDeviceImpl** dev) {
    (void)This; (void)id;
    if (!dev) return E_POINTER;
    IMMDeviceImpl* d = alloc_device(eRender);
    if (!d) { *dev = 0; return E_OUTOFMEMORY; }
    *dev = d; return S_OK;
}
static HRESULT Enum_RegisterEndpointNotificationCallback(
        IMMDeviceEnumeratorImpl* This, void* cb) {
    (void)This; (void)cb; return S_OK;
}
static HRESULT Enum_UnregisterEndpointNotificationCallback(
        IMMDeviceEnumeratorImpl* This, void* cb) {
    (void)This; (void)cb; return S_OK;
}

static const IMMDeviceEnumeratorVtbl g_enumVtbl = {
    Enum_QueryInterface, Enum_AddRef, Enum_Release,
    Enum_EnumAudioEndpoints, Enum_GetDefaultAudioEndpoint, Enum_GetDevice,
    Enum_RegisterEndpointNotificationCallback,
    Enum_UnregisterEndpointNotificationCallback,
};

static IMMDeviceEnumeratorImpl* alloc_enum(void) {
    for (int i = 0; i < MAX_ENUMS; i++) {
        if (!g_enums[i].used) {
            zero_mem(&g_enums[i], sizeof(g_enums[i]));
            g_enums[i].used     = 1;
            g_enums[i].refCount = 1;
            g_enums[i].lpVtbl   = &g_enumVtbl;
            return &g_enums[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados.
//
//  No Windows real, mmdevapi.dll NAO exporta CoCreateInstance — ela e
//  registrada como in-proc server e o COM resolve via CLSCTX_INPROC_SERVER.
//  Aqui, sem registry COM, exportamos um helper "CoCreateInstance" stub para
//  apps minimalistas + uma funcao MMDevApiCreateEnumerator que e o atalho
//  oficial usado por exemplos.
// ============================================================================

// CoCreateInstance(clsid, outer, ctx, iid, **ppv): aceita qualquer CLSID/IID e
// devolve um IMMDeviceEnumerator. Reflete o comportamento Win10/11 quando o
// CLSID requerido e o CLSID_MMDeviceEnumerator.
__declspec(dllexport) HRESULT CoCreateInstance(REFCLSID clsid, IUnknown outer,
        DWORD ctx, REFIID iid, void** ppv) {
    (void)clsid; (void)outer; (void)ctx; (void)iid;
    if (!ppv) return E_POINTER;
    IMMDeviceEnumeratorImpl* e = alloc_enum();
    if (!e) { *ppv = 0; return E_OUTOFMEMORY; }
    *ppv = e;
    return S_OK;
}

// MMDevApiCreateEnumerator(**ppEnum): atalho direto que nao depende do COM
// registry. Usado por exemplos de teste do MeuOS.
__declspec(dllexport) HRESULT MMDevApiCreateEnumerator(IMMDeviceEnumeratorImpl** ppe) {
    if (!ppe) return E_POINTER;
    IMMDeviceEnumeratorImpl* e = alloc_enum();
    if (!e) { *ppe = 0; return E_OUTOFMEMORY; }
    *ppe = e; return S_OK;
}

// ActivateAudioInterfaceAsync (Win8+): assinatura oficial pede um operation
// handle; aqui devolvemos S_OK sem callback (sem timers/threads em ring 3).
__declspec(dllexport) HRESULT ActivateAudioInterfaceAsync(WCHAR* devInterface,
        REFIID iid, void* params, void* completionHandler, void** asyncOp) {
    (void)devInterface; (void)iid; (void)params; (void)completionHandler;
    if (asyncOp) *asyncOp = (void*)0xAAD10C12ULL;
    return S_OK;
}

// DllMain padrao — apenas devolve TRUE.
int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
