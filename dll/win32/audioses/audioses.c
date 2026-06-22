// audioses.dll — reimplementacao minima da Audio Session API (WASAPI) do
// Windows Vista+ (FASE 11 - stack de audio, alinhada com Windows 10/11).
//
// No Windows real, Audioses.dll vive em RING 3 e expoe as interfaces principais
// de WASAPI: IAudioClient (gerencia o stream PCM), IAudioRenderClient (escreve
// frames para o speaker) e IAudioCaptureClient (le frames do mic). Apps a
// obtem via IMMDevice::Activate(IID_IAudioClient, ...) na mmdevapi.dll.
//
// Aqui no MeuOS nao temos PCM real (audio.sys driver e stub HD-Audio que so
// detecta o controlador no PCI). Este stub limita-se ao ABI COM completo:
//   IAudioClient::Initialize/GetBufferSize/GetService/Start/Stop/GetCurrentPadding
//   IAudioRenderClient::GetBuffer/ReleaseBuffer
//   IAudioCaptureClient::GetBuffer/ReleaseBuffer/GetNextPacketSize
//   IAudioStreamVolume::GetChannelCount/SetChannelVolume/GetChannelVolume
//   ISimpleAudioVolume::SetMasterVolume/GetMasterVolume/SetMute/GetMute
//   IAudioClock::GetFrequency/GetPosition
//
// Tudo retorna S_OK + handles fake; suficiente para apps WASAPI que so checam
// codigos de erro e exercitam o ABI sem produzir som.
//
// COM ABI (estilo Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). Em ABI ms_abi (x86_64-
// windows-gnu) os parametros entram em RCX,RDX,R8,R9 — e essa a ABI que o
// zig cc gera com -target windows-gnu.
//
// IMAGE BASE: 0x4B00000 — zona livre apos mmdevapi (0x4A00000), com .reloc
// via --dynamicbase (mesma estrategia das outras DLLs >= PMM_BASE 0x4000000).

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
typedef float              FLOAT;

#define S_OK                         0x00000000L
#define S_FALSE                      0x00000001L
#define E_NOTIMPL                    0x80004001L
#define E_NOINTERFACE                0x80004002L
#define E_POINTER                    0x80004003L
#define E_FAIL                       0x80004005L
#define E_INVALIDARG                 0x80070057L
#define E_OUTOFMEMORY                0x8007000EL

// Codigos AUDCLNT_ (subset). Usados como retorno em situacoes "esperadas".
#define AUDCLNT_E_NOT_INITIALIZED         0x88890001L
#define AUDCLNT_E_ALREADY_INITIALIZED     0x88890002L
#define AUDCLNT_E_WRONG_ENDPOINT_TYPE     0x88890003L
#define AUDCLNT_E_DEVICE_INVALIDATED      0x88890004L
#define AUDCLNT_E_NOT_STOPPED             0x88890005L
#define AUDCLNT_E_BUFFER_TOO_LARGE        0x88890006L
#define AUDCLNT_E_OUT_OF_ORDER            0x88890007L
#define AUDCLNT_E_UNSUPPORTED_FORMAT      0x88890008L
#define AUDCLNT_E_INVALID_SIZE            0x88890009L
#define AUDCLNT_E_DEVICE_IN_USE           0x8889000AL
#define AUDCLNT_E_BUFFER_OPERATION_PENDING 0x8889000BL
#define AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED 0x88890019L
#define AUDCLNT_S_BUFFER_EMPTY            0x08890001L

// AUDCLNT_SHAREMODE — shared (mistura com outros apps) ou exclusive (toma o
// hardware). Apps WASAPI tipicos usam SHARED.
#define AUDCLNT_SHAREMODE_SHARED          0
#define AUDCLNT_SHAREMODE_EXCLUSIVE       1

// AUDCLNT_STREAMFLAGS_* — flags para Initialize. Subset.
#define AUDCLNT_STREAMFLAGS_CROSSPROCESS  0x00010000
#define AUDCLNT_STREAMFLAGS_LOOPBACK      0x00020000
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00040000
#define AUDCLNT_STREAMFLAGS_NOPERSIST     0x00080000

// ============================================================================
//  Forward decls.
// ============================================================================
struct IAudioClientImpl;
struct IAudioRenderClientImpl;
struct IAudioCaptureClientImpl;
struct IAudioStreamVolumeImpl;
struct ISimpleAudioVolumeImpl;
struct IAudioClockImpl;

// ============================================================================
//  WAVEFORMATEX — assinatura do formato PCM (44100 Hz, 16-bit, stereo, ...).
//  Usado por Initialize / IsFormatSupported / GetMixFormat.
// ============================================================================
#pragma pack(push, 1)
typedef struct tWAVEFORMATEX {
    WORD wFormatTag;        // 1 = PCM, 3 = IEEE float, 0xFFFE = WAVE_FORMAT_EXTENSIBLE
    WORD nChannels;         // 1=mono, 2=stereo
    DWORD nSamplesPerSec;   // 44100, 48000, ...
    DWORD nAvgBytesPerSec;  // = nSamplesPerSec * nBlockAlign
    WORD nBlockAlign;       // = nChannels * (wBitsPerSample/8)
    WORD wBitsPerSample;    // 8, 16, 24, 32
    WORD cbSize;            // 0 para PCM puro
} WAVEFORMATEX;
#pragma pack(pop)

// ============================================================================
//  POOLs estaticos (sem heap em ring 3).
// ============================================================================
#define MAX_CLIENTS         8
#define MAX_RENDERS         8
#define MAX_CAPTURES        4
#define MAX_STREAM_VOL      4
#define MAX_SIMPLE_VOL      4
#define MAX_CLOCKS          4

typedef struct IAudioClientImpl {
    const struct IAudioClientVtbl* lpVtbl;
    long refCount;
    int  used;
    int  initialized;         // 1 apos Initialize OK
    int  running;             // 1 entre Start e Stop
    UINT bufferFrames;        // tamanho do buffer em frames
    UINT channels;
    UINT samplesPerSec;
    UINT bitsPerSample;
    UINT padding;             // frames "em uso" (fake)
} IAudioClientImpl;

typedef struct IAudioRenderClientImpl {
    const struct IAudioRenderClientVtbl* lpVtbl;
    long refCount;
    int  used;
    IAudioClientImpl* parent;
    // Buffer interno fake (16 KiB). Apps WASAPI escrevem aqui em GetBuffer e
    // o stub "consome" em ReleaseBuffer (atualiza padding).
    BYTE  buffer[16 * 1024];
} IAudioRenderClientImpl;

typedef struct IAudioCaptureClientImpl {
    const struct IAudioCaptureClientVtbl* lpVtbl;
    long refCount;
    int  used;
    IAudioClientImpl* parent;
    BYTE  buffer[8 * 1024];   // zero-filled (silencio)
} IAudioCaptureClientImpl;

typedef struct IAudioStreamVolumeImpl {
    const struct IAudioStreamVolumeVtbl* lpVtbl;
    long refCount;
    int  used;
    IAudioClientImpl* parent;
    FLOAT volumes[8];         // ate 7.1 (8 canais)
} IAudioStreamVolumeImpl;

typedef struct ISimpleAudioVolumeImpl {
    const struct ISimpleAudioVolumeVtbl* lpVtbl;
    long refCount;
    int  used;
    IAudioClientImpl* parent;
    FLOAT master;             // 0.0 .. 1.0
    BOOL  mute;
} ISimpleAudioVolumeImpl;

typedef struct IAudioClockImpl {
    const struct IAudioClockVtbl* lpVtbl;
    long refCount;
    int  used;
    IAudioClientImpl* parent;
    UINT64 position;
} IAudioClockImpl;

static IAudioClientImpl         g_clients[MAX_CLIENTS];
static IAudioRenderClientImpl   g_renders[MAX_RENDERS];
static IAudioCaptureClientImpl  g_captures[MAX_CAPTURES];
static IAudioStreamVolumeImpl   g_streamVols[MAX_STREAM_VOL];
static ISimpleAudioVolumeImpl   g_simpleVols[MAX_SIMPLE_VOL];
static IAudioClockImpl          g_clocks[MAX_CLOCKS];

static void zero_mem(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  Vtables.
// ============================================================================
typedef struct IAudioRenderClientVtbl {
    HRESULT (*QueryInterface)(IAudioRenderClientImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IAudioRenderClientImpl* This);
    ULONG   (*Release)       (IAudioRenderClientImpl* This);

    HRESULT (*GetBuffer)     (IAudioRenderClientImpl* This, UINT numFrames, BYTE** data);
    HRESULT (*ReleaseBuffer) (IAudioRenderClientImpl* This, UINT numFrames, DWORD flags);
} IAudioRenderClientVtbl;

typedef struct IAudioCaptureClientVtbl {
    HRESULT (*QueryInterface)(IAudioCaptureClientImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IAudioCaptureClientImpl* This);
    ULONG   (*Release)       (IAudioCaptureClientImpl* This);

    HRESULT (*GetBuffer)     (IAudioCaptureClientImpl* This, BYTE** data,
                              UINT* numFrames, DWORD* flags, UINT64* devPos,
                              UINT64* qpcPos);
    HRESULT (*ReleaseBuffer) (IAudioCaptureClientImpl* This, UINT numFrames);
    HRESULT (*GetNextPacketSize)(IAudioCaptureClientImpl* This, UINT* numFrames);
} IAudioCaptureClientVtbl;

typedef struct IAudioStreamVolumeVtbl {
    HRESULT (*QueryInterface)(IAudioStreamVolumeImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IAudioStreamVolumeImpl* This);
    ULONG   (*Release)       (IAudioStreamVolumeImpl* This);

    HRESULT (*GetChannelCount)  (IAudioStreamVolumeImpl* This, UINT* count);
    HRESULT (*SetChannelVolume) (IAudioStreamVolumeImpl* This, UINT idx, FLOAT vol);
    HRESULT (*GetChannelVolume) (IAudioStreamVolumeImpl* This, UINT idx, FLOAT* vol);
    HRESULT (*SetAllVolumes)    (IAudioStreamVolumeImpl* This, UINT count, FLOAT* vols);
    HRESULT (*GetAllVolumes)    (IAudioStreamVolumeImpl* This, UINT count, FLOAT* vols);
} IAudioStreamVolumeVtbl;

typedef struct ISimpleAudioVolumeVtbl {
    HRESULT (*QueryInterface)(ISimpleAudioVolumeImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (ISimpleAudioVolumeImpl* This);
    ULONG   (*Release)       (ISimpleAudioVolumeImpl* This);

    HRESULT (*SetMasterVolume)  (ISimpleAudioVolumeImpl* This, FLOAT vol, void* ctx);
    HRESULT (*GetMasterVolume)  (ISimpleAudioVolumeImpl* This, FLOAT* vol);
    HRESULT (*SetMute)          (ISimpleAudioVolumeImpl* This, BOOL mute, void* ctx);
    HRESULT (*GetMute)          (ISimpleAudioVolumeImpl* This, BOOL* mute);
} ISimpleAudioVolumeVtbl;

typedef struct IAudioClockVtbl {
    HRESULT (*QueryInterface)(IAudioClockImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IAudioClockImpl* This);
    ULONG   (*Release)       (IAudioClockImpl* This);

    HRESULT (*GetFrequency)  (IAudioClockImpl* This, UINT64* freq);
    HRESULT (*GetPosition)   (IAudioClockImpl* This, UINT64* pos, UINT64* qpcPos);
    HRESULT (*GetCharacteristics)(IAudioClockImpl* This, DWORD* flags);
} IAudioClockVtbl;

typedef struct IAudioClientVtbl {
    HRESULT (*QueryInterface)(IAudioClientImpl* This, REFIID riid, void** ppv);
    ULONG   (*AddRef)        (IAudioClientImpl* This);
    ULONG   (*Release)       (IAudioClientImpl* This);

    HRESULT (*Initialize)    (IAudioClientImpl* This, DWORD shareMode,
                              DWORD streamFlags, INT64 bufferDuration,
                              INT64 periodicity, WAVEFORMATEX* format,
                              REFGUID sessionGuid);
    HRESULT (*GetBufferSize) (IAudioClientImpl* This, UINT* numFrames);
    HRESULT (*GetStreamLatency)(IAudioClientImpl* This, INT64* latency);
    HRESULT (*GetCurrentPadding)(IAudioClientImpl* This, UINT* padding);
    HRESULT (*IsFormatSupported)(IAudioClientImpl* This, DWORD shareMode,
                              WAVEFORMATEX* format, WAVEFORMATEX** closest);
    HRESULT (*GetMixFormat)  (IAudioClientImpl* This, WAVEFORMATEX** format);
    HRESULT (*GetDevicePeriod)(IAudioClientImpl* This, INT64* defaultPer,
                              INT64* minPer);
    HRESULT (*Start)         (IAudioClientImpl* This);
    HRESULT (*Stop)          (IAudioClientImpl* This);
    HRESULT (*Reset)         (IAudioClientImpl* This);
    HRESULT (*SetEventHandle)(IAudioClientImpl* This, HANDLE evt);
    HRESULT (*GetService)    (IAudioClientImpl* This, REFIID iid, void** ppv);
} IAudioClientVtbl;

// ============================================================================
//  IAudioClock — stub que sempre devolve freq=44100 Hz e posicao crescente.
// ============================================================================
static HRESULT Clk_QueryInterface(IAudioClockImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG Clk_AddRef(IAudioClockImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG Clk_Release(IAudioClockImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Clk_GetFrequency(IAudioClockImpl* This, UINT64* freq) {
    (void)This; if (freq) *freq = 44100ULL; return S_OK;
}
static HRESULT Clk_GetPosition(IAudioClockImpl* This, UINT64* pos, UINT64* qpc) {
    if (pos) { *pos = This->position; This->position += 1024; }
    if (qpc) *qpc = 0;
    return S_OK;
}
static HRESULT Clk_GetCharacteristics(IAudioClockImpl* This, DWORD* fl) {
    (void)This; if (fl) *fl = 0; return S_OK;
}
static const IAudioClockVtbl g_clkVtbl = {
    Clk_QueryInterface, Clk_AddRef, Clk_Release,
    Clk_GetFrequency, Clk_GetPosition, Clk_GetCharacteristics,
};

static IAudioClockImpl* alloc_clock(IAudioClientImpl* parent) {
    for (int i = 0; i < MAX_CLOCKS; i++) {
        if (!g_clocks[i].used) {
            zero_mem(&g_clocks[i], sizeof(g_clocks[i]));
            g_clocks[i].used     = 1;
            g_clocks[i].refCount = 1;
            g_clocks[i].lpVtbl   = &g_clkVtbl;
            g_clocks[i].parent   = parent;
            return &g_clocks[i];
        }
    }
    return 0;
}

// ============================================================================
//  IAudioStreamVolume / ISimpleAudioVolume — stubs com volumes cacheados.
// ============================================================================
static HRESULT SV_QueryInterface(IAudioStreamVolumeImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG SV_AddRef(IAudioStreamVolumeImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG SV_Release(IAudioStreamVolumeImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT SV_GetChannelCount(IAudioStreamVolumeImpl* This, UINT* c) {
    if (!c) return E_POINTER;
    *c = This->parent ? This->parent->channels : 2;
    return S_OK;
}
static HRESULT SV_SetChannelVolume(IAudioStreamVolumeImpl* This, UINT i, FLOAT v) {
    if (i >= 8) return E_INVALIDARG;
    This->volumes[i] = v; return S_OK;
}
static HRESULT SV_GetChannelVolume(IAudioStreamVolumeImpl* This, UINT i, FLOAT* v) {
    if (!v || i >= 8) return E_INVALIDARG;
    *v = This->volumes[i]; return S_OK;
}
static HRESULT SV_SetAllVolumes(IAudioStreamVolumeImpl* This, UINT c, FLOAT* v) {
    if (!v) return E_POINTER;
    for (UINT i = 0; i < c && i < 8; i++) This->volumes[i] = v[i];
    return S_OK;
}
static HRESULT SV_GetAllVolumes(IAudioStreamVolumeImpl* This, UINT c, FLOAT* v) {
    if (!v) return E_POINTER;
    for (UINT i = 0; i < c && i < 8; i++) v[i] = This->volumes[i];
    return S_OK;
}
static const IAudioStreamVolumeVtbl g_svVtbl = {
    SV_QueryInterface, SV_AddRef, SV_Release,
    SV_GetChannelCount, SV_SetChannelVolume, SV_GetChannelVolume,
    SV_SetAllVolumes, SV_GetAllVolumes,
};

static IAudioStreamVolumeImpl* alloc_streamvol(IAudioClientImpl* parent) {
    for (int i = 0; i < MAX_STREAM_VOL; i++) {
        if (!g_streamVols[i].used) {
            zero_mem(&g_streamVols[i], sizeof(g_streamVols[i]));
            g_streamVols[i].used     = 1;
            g_streamVols[i].refCount = 1;
            g_streamVols[i].lpVtbl   = &g_svVtbl;
            g_streamVols[i].parent   = parent;
            for (int j = 0; j < 8; j++) g_streamVols[i].volumes[j] = 1.0f;
            return &g_streamVols[i];
        }
    }
    return 0;
}

static HRESULT SMV_QueryInterface(ISimpleAudioVolumeImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG SMV_AddRef(ISimpleAudioVolumeImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG SMV_Release(ISimpleAudioVolumeImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT SMV_SetMasterVolume(ISimpleAudioVolumeImpl* This, FLOAT v, void* c) {
    (void)c; This->master = v; return S_OK;
}
static HRESULT SMV_GetMasterVolume(ISimpleAudioVolumeImpl* This, FLOAT* v) {
    if (!v) return E_POINTER; *v = This->master; return S_OK;
}
static HRESULT SMV_SetMute(ISimpleAudioVolumeImpl* This, BOOL m, void* c) {
    (void)c; This->mute = m; return S_OK;
}
static HRESULT SMV_GetMute(ISimpleAudioVolumeImpl* This, BOOL* m) {
    if (!m) return E_POINTER; *m = This->mute; return S_OK;
}
static const ISimpleAudioVolumeVtbl g_smvVtbl = {
    SMV_QueryInterface, SMV_AddRef, SMV_Release,
    SMV_SetMasterVolume, SMV_GetMasterVolume, SMV_SetMute, SMV_GetMute,
};

static ISimpleAudioVolumeImpl* alloc_simplevol(IAudioClientImpl* parent) {
    for (int i = 0; i < MAX_SIMPLE_VOL; i++) {
        if (!g_simpleVols[i].used) {
            zero_mem(&g_simpleVols[i], sizeof(g_simpleVols[i]));
            g_simpleVols[i].used     = 1;
            g_simpleVols[i].refCount = 1;
            g_simpleVols[i].lpVtbl   = &g_smvVtbl;
            g_simpleVols[i].parent   = parent;
            g_simpleVols[i].master   = 1.0f;
            g_simpleVols[i].mute     = 0;
            return &g_simpleVols[i];
        }
    }
    return 0;
}

// ============================================================================
//  IAudioRenderClient.
//
//  GetBuffer(numFrames, &data): devolve um ponteiro para o buffer interno.
//  ReleaseBuffer(numFrames, flags): "consome" os frames (atualiza padding).
//
//  Sem PCM real: o conteudo escrito pelo app nao toca em nada (drivers/audio/
//  HDAudio nao expoe DMA stream). Apps podem escrever sem corromper memoria.
// ============================================================================
static HRESULT RC_QueryInterface(IAudioRenderClientImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG RC_AddRef(IAudioRenderClientImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG RC_Release(IAudioRenderClientImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT RC_GetBuffer(IAudioRenderClientImpl* This, UINT n, BYTE** data) {
    (void)n; if (!data) return E_POINTER;
    *data = This->buffer;
    return S_OK;
}
static HRESULT RC_ReleaseBuffer(IAudioRenderClientImpl* This, UINT n, DWORD f) {
    (void)f;
    if (This->parent) {
        // Atualiza padding (sem nunca passar do buffer total).
        This->parent->padding += n;
        if (This->parent->padding > This->parent->bufferFrames)
            This->parent->padding = This->parent->bufferFrames;
    }
    return S_OK;
}

static const IAudioRenderClientVtbl g_rcVtbl = {
    RC_QueryInterface, RC_AddRef, RC_Release,
    RC_GetBuffer, RC_ReleaseBuffer,
};

static IAudioRenderClientImpl* alloc_renderclient(IAudioClientImpl* parent) {
    for (int i = 0; i < MAX_RENDERS; i++) {
        if (!g_renders[i].used) {
            zero_mem(&g_renders[i], sizeof(g_renders[i]));
            g_renders[i].used     = 1;
            g_renders[i].refCount = 1;
            g_renders[i].lpVtbl   = &g_rcVtbl;
            g_renders[i].parent   = parent;
            return &g_renders[i];
        }
    }
    return 0;
}

// ============================================================================
//  IAudioCaptureClient.
//
//  GetBuffer(&data, &n, ...): devolve um ponteiro para buffer de silencio
//  (zero-filled). ReleaseBuffer: no-op.
// ============================================================================
static HRESULT CC_QueryInterface(IAudioCaptureClientImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG CC_AddRef(IAudioCaptureClientImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG CC_Release(IAudioCaptureClientImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT CC_GetBuffer(IAudioCaptureClientImpl* This, BYTE** data,
        UINT* n, DWORD* flags, UINT64* devPos, UINT64* qpcPos) {
    if (!data || !n) return E_POINTER;
    *data = This->buffer;
    *n    = 0;          // sem dados disponiveis (silencio absoluto)
    if (flags)  *flags  = 0;
    if (devPos) *devPos = 0;
    if (qpcPos) *qpcPos = 0;
    return AUDCLNT_S_BUFFER_EMPTY;
}
static HRESULT CC_ReleaseBuffer(IAudioCaptureClientImpl* This, UINT n) {
    (void)This; (void)n; return S_OK;
}
static HRESULT CC_GetNextPacketSize(IAudioCaptureClientImpl* This, UINT* n) {
    (void)This; if (n) *n = 0; return S_OK;
}

static const IAudioCaptureClientVtbl g_ccVtbl = {
    CC_QueryInterface, CC_AddRef, CC_Release,
    CC_GetBuffer, CC_ReleaseBuffer, CC_GetNextPacketSize,
};

static IAudioCaptureClientImpl* alloc_captureclient(IAudioClientImpl* parent) {
    for (int i = 0; i < MAX_CAPTURES; i++) {
        if (!g_captures[i].used) {
            zero_mem(&g_captures[i], sizeof(g_captures[i]));
            g_captures[i].used     = 1;
            g_captures[i].refCount = 1;
            g_captures[i].lpVtbl   = &g_ccVtbl;
            g_captures[i].parent   = parent;
            return &g_captures[i];
        }
    }
    return 0;
}

// ============================================================================
//  IAudioClient — interface central de WASAPI.
//
//  Initialize(shareMode, flags, bufferDuration100ns, periodicity, &format, ...):
//    bufferDuration esta em 100-nanosegundos (REFERENCE_TIME). Convertemos para
//    frames assumindo 44100 Hz (ou nSamplesPerSec do format se nao-nulo).
// ============================================================================
static HRESULT Cl_QueryInterface(IAudioClientImpl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER;
    *ppv = This; This->refCount++; return S_OK;
}
static ULONG Cl_AddRef(IAudioClientImpl* This)  { return (ULONG)(++This->refCount); }
static ULONG Cl_Release(IAudioClientImpl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; This->running = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Cl_Initialize(IAudioClientImpl* This, DWORD share, DWORD flags,
        INT64 bufDur, INT64 period, WAVEFORMATEX* fmt, REFGUID sg) {
    (void)share; (void)flags; (void)period; (void)sg;
    if (This->initialized) return AUDCLNT_E_ALREADY_INITIALIZED;
    if (!fmt) return E_POINTER;
    This->channels       = fmt->nChannels      ? fmt->nChannels      : 2;
    This->samplesPerSec  = fmt->nSamplesPerSec ? fmt->nSamplesPerSec : 44100;
    This->bitsPerSample  = fmt->wBitsPerSample ? fmt->wBitsPerSample : 16;
    // bufferDuration em 100ns -> frames: n = (dur / 1e7) * samplesPerSec.
    // bufDur=0 (ou negativo): usa default 10 ms.
    INT64 dur = bufDur > 0 ? bufDur : 100000;       // 10 ms = 100000 * 100ns
    UINT64 frames = (UINT64)dur * (UINT64)This->samplesPerSec / 10000000ULL;
    if (frames == 0) frames = 1024;
    if (frames > 65536) frames = 65536;
    This->bufferFrames = (UINT)frames;
    This->initialized  = 1;
    This->padding      = 0;
    return S_OK;
}
static HRESULT Cl_GetBufferSize(IAudioClientImpl* This, UINT* n) {
    if (!This->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (!n) return E_POINTER;
    *n = This->bufferFrames; return S_OK;
}
static HRESULT Cl_GetStreamLatency(IAudioClientImpl* This, INT64* l) {
    if (!This->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (!l) return E_POINTER;
    *l = 100000;        // 10 ms em 100ns
    return S_OK;
}
static HRESULT Cl_GetCurrentPadding(IAudioClientImpl* This, UINT* p) {
    if (!This->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (!p) return E_POINTER;
    // "Drena" 1 packet por chamada (simula consumo do hardware).
    if (This->padding > 480) This->padding -= 480;
    else This->padding = 0;
    *p = This->padding;
    return S_OK;
}
static HRESULT Cl_IsFormatSupported(IAudioClientImpl* This, DWORD share,
        WAVEFORMATEX* fmt, WAVEFORMATEX** closest) {
    (void)This; (void)share; (void)fmt;
    if (closest) *closest = 0;     // aceitamos qualquer formato
    return S_OK;
}
// GetMixFormat: devolve um WAVEFORMATEX fake (44100/16/stereo). O app espera
// um ponteiro alocado por COM (CoTaskMemFree depois); usamos um static que
// nunca e liberado — ainda assim, apps que so leem e ignoram o free funcionam.
static WAVEFORMATEX g_mixFormat = {
    1, 2, 44100, 44100 * 2 * 2, 4, 16, 0
};
static HRESULT Cl_GetMixFormat(IAudioClientImpl* This, WAVEFORMATEX** fmt) {
    (void)This; if (!fmt) return E_POINTER;
    *fmt = &g_mixFormat;
    return S_OK;
}
static HRESULT Cl_GetDevicePeriod(IAudioClientImpl* This, INT64* defaultPer,
        INT64* minPer) {
    (void)This;
    if (defaultPer) *defaultPer = 100000;  // 10 ms
    if (minPer)     *minPer     = 30000;   // 3 ms
    return S_OK;
}
static HRESULT Cl_Start(IAudioClientImpl* This) {
    if (!This->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    This->running = 1; return S_OK;
}
static HRESULT Cl_Stop(IAudioClientImpl* This) {
    if (!This->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    This->running = 0; return S_OK;
}
static HRESULT Cl_Reset(IAudioClientImpl* This) {
    if (!This->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (This->running) return AUDCLNT_E_NOT_STOPPED;
    This->padding = 0; return S_OK;
}
static HRESULT Cl_SetEventHandle(IAudioClientImpl* This, HANDLE evt) {
    (void)This; (void)evt; return S_OK;
}
// GetService(IID_IAudioRenderClient / Capture / StreamVolume / SimpleVolume /
// AudioClock): decide qual servico devolver. IIDs em ms_abi nao distinguem
// pelo ponteiro (apps passam um GUID*); usamos um truque: alocamos um render
// client por default e, se o codigo do app preferir captura, ele faria duas
// chamadas. Aceitavel para stubs (apps de teste que so checam *ppv != NULL).
static HRESULT Cl_GetService(IAudioClientImpl* This, REFIID iid, void** ppv) {
    (void)iid; if (!ppv) return E_POINTER;
    if (!This->initialized) { *ppv = 0; return AUDCLNT_E_NOT_INITIALIZED; }
    // Heuristica simples: o primeiro byte do GUID diferencia algumas das IIDs:
    //   IID_IAudioRenderClient    = F294ACFC -> primeiro byte 0xFC
    //   IID_IAudioCaptureClient   = C8ADBD64 -> primeiro byte 0x64
    //   IID_IAudioStreamVolume    = 93014887 -> primeiro byte 0x87
    //   IID_ISimpleAudioVolume    = 87CE5498 -> primeiro byte 0x98
    //   IID_IAudioClock           = CD63314F -> primeiro byte 0x4F
    // Em ms_abi GUIDs sao passados como ponteiros; aqui apenas distinguimos
    // pelo primeiro byte. Caso o app passe um IID nao mapeado, default = render.
    void* result = 0;
    if (iid) {
        unsigned char b0 = *(unsigned char*)iid;
        if      (b0 == 0x64) { result = alloc_captureclient(This); }
        else if (b0 == 0x87) { result = alloc_streamvol(This);     }
        else if (b0 == 0x98) { result = alloc_simplevol(This);     }
        else if (b0 == 0x4F) { result = alloc_clock(This);         }
        else                 { result = alloc_renderclient(This);  }
    } else {
        result = alloc_renderclient(This);
    }
    if (!result) { *ppv = 0; return E_OUTOFMEMORY; }
    *ppv = result;
    return S_OK;
}

static const IAudioClientVtbl g_clVtbl = {
    Cl_QueryInterface, Cl_AddRef, Cl_Release,
    Cl_Initialize, Cl_GetBufferSize, Cl_GetStreamLatency,
    Cl_GetCurrentPadding, Cl_IsFormatSupported, Cl_GetMixFormat,
    Cl_GetDevicePeriod, Cl_Start, Cl_Stop, Cl_Reset, Cl_SetEventHandle,
    Cl_GetService,
};

static IAudioClientImpl* alloc_audioclient(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) {
            zero_mem(&g_clients[i], sizeof(g_clients[i]));
            g_clients[i].used     = 1;
            g_clients[i].refCount = 1;
            g_clients[i].lpVtbl   = &g_clVtbl;
            return &g_clients[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados.
//
//  No Windows real, Audioses.dll nao expoe nenhuma funcao C exportada — todas
//  as suas interfaces sao obtidas via IMMDevice::Activate(IID_IAudioClient).
//  Aqui, para apps minimalistas que importem audioses diretamente, exportamos
//  helpers explicitos.
// ============================================================================

// AudiosesCreateAudioClient(**ppc): atalho que devolve um IAudioClient pronto
// para o app chamar Initialize/Start/Stop/GetService.
__declspec(dllexport) HRESULT AudiosesCreateAudioClient(IAudioClientImpl** ppc) {
    if (!ppc) return E_POINTER;
    IAudioClientImpl* c = alloc_audioclient();
    if (!c) { *ppc = 0; return E_OUTOFMEMORY; }
    *ppc = c; return S_OK;
}

// DllGetClassObject (in-proc COM server): stub que devolve um IAudioClient
// quando o app pede o CLSID via CoCreateInstance(audioses).
__declspec(dllexport) HRESULT DllGetClassObject(REFCLSID rclsid, REFIID riid,
        void** ppv) {
    (void)rclsid; (void)riid;
    if (!ppv) return E_POINTER;
    IAudioClientImpl* c = alloc_audioclient();
    if (!c) { *ppv = 0; return E_OUTOFMEMORY; }
    *ppv = c; return S_OK;
}

// DllCanUnloadNow: COM in-proc. Sempre S_FALSE (estamos sempre carregados).
__declspec(dllexport) HRESULT DllCanUnloadNow(void) { return S_FALSE; }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
