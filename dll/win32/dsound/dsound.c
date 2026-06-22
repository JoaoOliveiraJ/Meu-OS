// dsound.dll — reimplementacao minima do DirectSound (legado DirectX) — FASE 11.
//
// O DirectSound foi a API de audio do Windows desde DirectX 1.0 (1995) ate
// DirectX 8 ser substituido por XAudio2/WASAPI. Em Windows 10/11 a dsound.dll
// ainda existe e e usada por jogos antigos: por baixo, ela hoje delega para
// WASAPI (Audioses), mantendo a interface IDirectSound8 + IDirectSoundBuffer.
//
// Aqui no MeuOS oferecemos o ABI COM completo (IDirectSound8 + IDirectSoundBuffer8)
// retornando DS_OK em quase tudo. Apps DirectSound podem chamar:
//   DirectSoundCreate8 -> SetCooperativeLevel -> CreateSoundBuffer
//   -> IDirectSoundBuffer::Lock/Unlock/Play/Stop/SetVolume/SetFrequency
//
// Sem PCM real (driver audio.sys e stub HD-Audio so com PCI detection); o
// stub apenas mantem metadados nos pools estaticos.
//
// COM ABI (estilo Microsoft): vtable + thiscall ms_abi.
//
// IMAGE BASE: 0x4C00000 — zona livre apos audioses (0x4B00000), com .reloc
// via --dynamicbase (mesma estrategia das outras DLLs >= PMM_BASE).

unsigned int _tls_index = 0;

// ============================================================================
//  Tipos Windows minimos.
// ============================================================================
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               HRESULT;
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef void*              HANDLE;
typedef void*              HWND;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              REFIID;
typedef void*              REFCLSID;
typedef void*              REFGUID;
typedef void*              IUnknown;
typedef unsigned char      BYTE;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef long long          LONGLONG;
typedef unsigned long long ULL;

#define DS_OK                        0x00000000L
#define DS_NO_VIRTUALIZATION         0x0878000AL
#define DSERR_GENERIC                0x80004005L
#define DSERR_INVALIDPARAM           0x80070057L
#define DSERR_OUTOFMEMORY            0x8007000EL
#define DSERR_NOINTERFACE            0x80004002L
#define DSERR_NOAGGREGATION          0x80040110L
#define DSERR_ALREADYINITIALIZED     0x88780008L
#define DSERR_BUFFERLOST             0x88780096L
#define DSERR_BADFORMAT              0x88780064L

// DSBCAPS_* — flags no DSBUFFERDESC.dwFlags. Subset.
#define DSBCAPS_PRIMARYBUFFER        0x00000001
#define DSBCAPS_STATIC               0x00000002
#define DSBCAPS_LOCSOFTWARE          0x00000008
#define DSBCAPS_LOCHARDWARE          0x00000004
#define DSBCAPS_CTRLFREQUENCY        0x00000020
#define DSBCAPS_CTRLPAN              0x00000040
#define DSBCAPS_CTRLVOLUME           0x00000080
#define DSBCAPS_CTRLPOSITIONNOTIFY   0x00000100
#define DSBCAPS_CTRL3D               0x00000010
#define DSBCAPS_GLOBALFOCUS          0x00008000

// DSBSTATUS_* — bitmask devolvido por GetStatus.
#define DSBSTATUS_PLAYING            0x00000001
#define DSBSTATUS_BUFFERLOST         0x00000002
#define DSBSTATUS_LOOPING            0x00000004

// DSSCL_* — niveis de cooperacao para SetCooperativeLevel.
#define DSSCL_NORMAL                 1
#define DSSCL_PRIORITY               2
#define DSSCL_EXCLUSIVE              3
#define DSSCL_WRITEPRIMARY           4

// ============================================================================
//  WAVEFORMATEX + DSBUFFERDESC.
// ============================================================================
#pragma pack(push, 1)
typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX;

typedef struct _DSBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    WAVEFORMATEX* lpwfxFormat;
    char guid3DAlgorithm[16];   // GUID_NULL aceita
} DSBUFFERDESC;

typedef struct _DSBCAPS {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwUnlockTransferRate;
    DWORD dwPlayCpuOverhead;
} DSBCAPS;
#pragma pack(pop)

// ============================================================================
//  Forwards.
// ============================================================================
struct IDirectSound8Impl;
struct IDirectSoundBuffer8Impl;

// ============================================================================
//  POOLs.
// ============================================================================
#define MAX_DSOUND      4
#define MAX_DSBUFFER    16

typedef struct IDirectSoundBuffer8Impl {
    const struct IDirectSoundBuffer8Vtbl* lpVtbl;
    long refCount;
    int  used;
    int  is_primary;
    DWORD flags;
    DWORD bufferBytes;
    DWORD position;       // play cursor
    DWORD volume;         // 0..100
    DWORD frequency;      // Hz
    DWORD status;         // bitmask DSBSTATUS_*
    BYTE  storage[16 * 1024];   // backing store para Lock/Unlock
    WAVEFORMATEX format;
} IDirectSoundBuffer8Impl;

typedef struct IDirectSound8Impl {
    const struct IDirectSound8Vtbl* lpVtbl;
    long refCount;
    int  used;
    DWORD coopLevel;
} IDirectSound8Impl;

static IDirectSound8Impl        g_dsounds[MAX_DSOUND];
static IDirectSoundBuffer8Impl  g_dsbuffers[MAX_DSBUFFER];

static void zero_mem(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  Vtables.
// ============================================================================
typedef struct IDirectSoundBuffer8Vtbl {
    HRESULT (*QueryInterface)(IDirectSoundBuffer8Impl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDirectSoundBuffer8Impl* This);
    ULONG   (*Release)       (IDirectSoundBuffer8Impl* This);

    HRESULT (*GetCaps)       (IDirectSoundBuffer8Impl* This, DSBCAPS* caps);
    HRESULT (*GetCurrentPosition)(IDirectSoundBuffer8Impl* This, DWORD* play, DWORD* write);
    HRESULT (*GetFormat)     (IDirectSoundBuffer8Impl* This, WAVEFORMATEX* fmt,
                              DWORD sz, DWORD* outSz);
    HRESULT (*GetVolume)     (IDirectSoundBuffer8Impl* This, LONG* vol);
    HRESULT (*GetPan)        (IDirectSoundBuffer8Impl* This, LONG* pan);
    HRESULT (*GetFrequency)  (IDirectSoundBuffer8Impl* This, DWORD* freq);
    HRESULT (*GetStatus)     (IDirectSoundBuffer8Impl* This, DWORD* status);
    HRESULT (*Initialize)    (IDirectSoundBuffer8Impl* This, void* ds, DSBUFFERDESC* d);
    HRESULT (*Lock)          (IDirectSoundBuffer8Impl* This, DWORD off, DWORD bytes,
                              void** p1, DWORD* b1, void** p2, DWORD* b2, DWORD fl);
    HRESULT (*Play)          (IDirectSoundBuffer8Impl* This, DWORD r1, DWORD r2, DWORD fl);
    HRESULT (*SetCurrentPosition)(IDirectSoundBuffer8Impl* This, DWORD pos);
    HRESULT (*SetFormat)     (IDirectSoundBuffer8Impl* This, WAVEFORMATEX* fmt);
    HRESULT (*SetVolume)     (IDirectSoundBuffer8Impl* This, LONG vol);
    HRESULT (*SetPan)        (IDirectSoundBuffer8Impl* This, LONG pan);
    HRESULT (*SetFrequency)  (IDirectSoundBuffer8Impl* This, DWORD freq);
    HRESULT (*Stop)          (IDirectSoundBuffer8Impl* This);
    HRESULT (*Unlock)        (IDirectSoundBuffer8Impl* This, void* p1, DWORD b1,
                              void* p2, DWORD b2);
    HRESULT (*Restore)       (IDirectSoundBuffer8Impl* This);
    // IDirectSoundBuffer8 estende com:
    HRESULT (*SetFX)         (IDirectSoundBuffer8Impl* This, DWORD cnt, void* fxd, DWORD* res);
    HRESULT (*AcquireResources)(IDirectSoundBuffer8Impl* This, DWORD fl, DWORD cnt, DWORD* res);
    HRESULT (*GetObjectInPath)(IDirectSoundBuffer8Impl* This, REFGUID o, DWORD i, REFGUID iid, void** ppv);
} IDirectSoundBuffer8Vtbl;

typedef struct IDirectSound8Vtbl {
    HRESULT (*QueryInterface)(IDirectSound8Impl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDirectSound8Impl* This);
    ULONG   (*Release)       (IDirectSound8Impl* This);

    HRESULT (*CreateSoundBuffer)(IDirectSound8Impl* This, DSBUFFERDESC* d,
                              IDirectSoundBuffer8Impl** buf, IUnknown outer);
    HRESULT (*GetCaps)       (IDirectSound8Impl* This, void* caps);
    HRESULT (*DuplicateSoundBuffer)(IDirectSound8Impl* This,
                              IDirectSoundBuffer8Impl* orig,
                              IDirectSoundBuffer8Impl** dup);
    HRESULT (*SetCooperativeLevel)(IDirectSound8Impl* This, HWND hw, DWORD lvl);
    HRESULT (*Compact)       (IDirectSound8Impl* This);
    HRESULT (*GetSpeakerConfig)(IDirectSound8Impl* This, DWORD* cfg);
    HRESULT (*SetSpeakerConfig)(IDirectSound8Impl* This, DWORD cfg);
    HRESULT (*Initialize)    (IDirectSound8Impl* This, REFGUID guid);
    HRESULT (*VerifyCertification)(IDirectSound8Impl* This, DWORD* cert);
} IDirectSound8Vtbl;

// ============================================================================
//  IDirectSoundBuffer8.
// ============================================================================
static HRESULT Buf_QueryInterface(IDirectSoundBuffer8Impl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return DSERR_INVALIDPARAM;
    *ppv = This; This->refCount++; return DS_OK;
}
static ULONG Buf_AddRef(IDirectSoundBuffer8Impl* This)  { return (ULONG)(++This->refCount); }
static ULONG Buf_Release(IDirectSoundBuffer8Impl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Buf_GetCaps(IDirectSoundBuffer8Impl* This, DSBCAPS* c) {
    if (!c) return DSERR_INVALIDPARAM;
    c->dwSize = sizeof(*c);
    c->dwFlags = This->flags;
    c->dwBufferBytes = This->bufferBytes;
    c->dwUnlockTransferRate = 0;
    c->dwPlayCpuOverhead = 0;
    return DS_OK;
}
static HRESULT Buf_GetCurrentPosition(IDirectSoundBuffer8Impl* This,
        DWORD* play, DWORD* write) {
    if (play) {
        // Avanca a posicao para apps que polling.
        This->position += 1024;
        if (This->bufferBytes && This->position >= This->bufferBytes)
            This->position %= This->bufferBytes;
        *play = This->position;
    }
    if (write) *write = This->position + 512;
    return DS_OK;
}
static HRESULT Buf_GetFormat(IDirectSoundBuffer8Impl* This, WAVEFORMATEX* f,
        DWORD sz, DWORD* outSz) {
    if (outSz) *outSz = sizeof(WAVEFORMATEX);
    if (f && sz >= sizeof(WAVEFORMATEX)) *f = This->format;
    return DS_OK;
}
static HRESULT Buf_GetVolume(IDirectSoundBuffer8Impl* This, LONG* v) {
    if (!v) return DSERR_INVALIDPARAM;
    *v = (LONG)This->volume; return DS_OK;
}
static HRESULT Buf_GetPan(IDirectSoundBuffer8Impl* This, LONG* p) {
    (void)This; if (!p) return DSERR_INVALIDPARAM;
    *p = 0; return DS_OK;
}
static HRESULT Buf_GetFrequency(IDirectSoundBuffer8Impl* This, DWORD* f) {
    if (!f) return DSERR_INVALIDPARAM;
    *f = This->frequency; return DS_OK;
}
static HRESULT Buf_GetStatus(IDirectSoundBuffer8Impl* This, DWORD* s) {
    if (!s) return DSERR_INVALIDPARAM;
    *s = This->status; return DS_OK;
}
static HRESULT Buf_Initialize(IDirectSoundBuffer8Impl* This, void* ds, DSBUFFERDESC* d) {
    (void)ds; (void)d; (void)This; return DSERR_ALREADYINITIALIZED;
}
static HRESULT Buf_Lock(IDirectSoundBuffer8Impl* This, DWORD off, DWORD bytes,
        void** p1, DWORD* b1, void** p2, DWORD* b2, DWORD fl) {
    (void)fl;
    if (!p1 || !b1) return DSERR_INVALIDPARAM;
    if (off >= sizeof(This->storage)) off = 0;
    DWORD avail = (DWORD)sizeof(This->storage) - off;
    DWORD use   = bytes ? bytes : avail;
    if (use > avail) use = avail;
    *p1 = &This->storage[off];
    *b1 = use;
    if (p2) *p2 = 0;
    if (b2) *b2 = 0;
    return DS_OK;
}
static HRESULT Buf_Play(IDirectSoundBuffer8Impl* This, DWORD r1, DWORD r2, DWORD fl) {
    (void)r1; (void)r2;
    This->status = DSBSTATUS_PLAYING | (fl & 1 ? DSBSTATUS_LOOPING : 0);
    return DS_OK;
}
static HRESULT Buf_SetCurrentPosition(IDirectSoundBuffer8Impl* This, DWORD p) {
    This->position = p; return DS_OK;
}
static HRESULT Buf_SetFormat(IDirectSoundBuffer8Impl* This, WAVEFORMATEX* f) {
    if (!f) return DSERR_INVALIDPARAM;
    This->format = *f; return DS_OK;
}
static HRESULT Buf_SetVolume(IDirectSoundBuffer8Impl* This, LONG v) {
    This->volume = (DWORD)v; return DS_OK;
}
static HRESULT Buf_SetPan(IDirectSoundBuffer8Impl* This, LONG p) {
    (void)This; (void)p; return DS_OK;
}
static HRESULT Buf_SetFrequency(IDirectSoundBuffer8Impl* This, DWORD f) {
    This->frequency = f; return DS_OK;
}
static HRESULT Buf_Stop(IDirectSoundBuffer8Impl* This) {
    This->status &= ~(DSBSTATUS_PLAYING | DSBSTATUS_LOOPING);
    return DS_OK;
}
static HRESULT Buf_Unlock(IDirectSoundBuffer8Impl* This, void* p1, DWORD b1,
        void* p2, DWORD b2) {
    (void)This; (void)p1; (void)b1; (void)p2; (void)b2; return DS_OK;
}
static HRESULT Buf_Restore(IDirectSoundBuffer8Impl* This) {
    This->status &= ~DSBSTATUS_BUFFERLOST; return DS_OK;
}
static HRESULT Buf_SetFX(IDirectSoundBuffer8Impl* This, DWORD c, void* fxd, DWORD* res) {
    (void)This; (void)fxd;
    if (res) for (DWORD i = 0; i < c; i++) res[i] = 0;
    return DS_OK;
}
static HRESULT Buf_AcquireResources(IDirectSoundBuffer8Impl* This, DWORD f,
        DWORD c, DWORD* res) {
    (void)This; (void)f;
    if (res) for (DWORD i = 0; i < c; i++) res[i] = 0;
    return DS_OK;
}
static HRESULT Buf_GetObjectInPath(IDirectSoundBuffer8Impl* This, REFGUID o,
        DWORD i, REFGUID iid, void** ppv) {
    (void)This; (void)o; (void)i; (void)iid;
    if (ppv) *ppv = 0;
    return DSERR_NOINTERFACE;
}

static const IDirectSoundBuffer8Vtbl g_bufVtbl = {
    Buf_QueryInterface, Buf_AddRef, Buf_Release,
    Buf_GetCaps, Buf_GetCurrentPosition, Buf_GetFormat,
    Buf_GetVolume, Buf_GetPan, Buf_GetFrequency, Buf_GetStatus,
    Buf_Initialize, Buf_Lock, Buf_Play, Buf_SetCurrentPosition,
    Buf_SetFormat, Buf_SetVolume, Buf_SetPan, Buf_SetFrequency,
    Buf_Stop, Buf_Unlock, Buf_Restore,
    Buf_SetFX, Buf_AcquireResources, Buf_GetObjectInPath,
};

static IDirectSoundBuffer8Impl* alloc_dsbuffer(DSBUFFERDESC* d) {
    for (int i = 0; i < MAX_DSBUFFER; i++) {
        if (!g_dsbuffers[i].used) {
            zero_mem(&g_dsbuffers[i], sizeof(g_dsbuffers[i]));
            g_dsbuffers[i].used      = 1;
            g_dsbuffers[i].refCount  = 1;
            g_dsbuffers[i].lpVtbl    = &g_bufVtbl;
            g_dsbuffers[i].volume    = 100;
            g_dsbuffers[i].frequency = 44100;
            if (d) {
                g_dsbuffers[i].flags       = d->dwFlags;
                g_dsbuffers[i].bufferBytes = d->dwBufferBytes;
                g_dsbuffers[i].is_primary  = (d->dwFlags & DSBCAPS_PRIMARYBUFFER) ? 1 : 0;
                if (d->lpwfxFormat) {
                    g_dsbuffers[i].format    = *d->lpwfxFormat;
                    g_dsbuffers[i].frequency = d->lpwfxFormat->nSamplesPerSec;
                } else {
                    // default: 44.1 kHz 16-bit stereo
                    g_dsbuffers[i].format.wFormatTag      = 1;
                    g_dsbuffers[i].format.nChannels       = 2;
                    g_dsbuffers[i].format.nSamplesPerSec  = 44100;
                    g_dsbuffers[i].format.wBitsPerSample  = 16;
                    g_dsbuffers[i].format.nBlockAlign     = 4;
                    g_dsbuffers[i].format.nAvgBytesPerSec = 44100 * 4;
                }
            } else {
                g_dsbuffers[i].format.wFormatTag      = 1;
                g_dsbuffers[i].format.nChannels       = 2;
                g_dsbuffers[i].format.nSamplesPerSec  = 44100;
                g_dsbuffers[i].format.wBitsPerSample  = 16;
                g_dsbuffers[i].format.nBlockAlign     = 4;
                g_dsbuffers[i].format.nAvgBytesPerSec = 44100 * 4;
            }
            return &g_dsbuffers[i];
        }
    }
    return 0;
}

// ============================================================================
//  IDirectSound8.
// ============================================================================
static HRESULT DS_QueryInterface(IDirectSound8Impl* This, REFIID r, void** ppv) {
    (void)r; if (!ppv) return DSERR_INVALIDPARAM;
    *ppv = This; This->refCount++; return DS_OK;
}
static ULONG DS_AddRef(IDirectSound8Impl* This)  { return (ULONG)(++This->refCount); }
static ULONG DS_Release(IDirectSound8Impl* This) {
    long n = --This->refCount;
    if (n <= 0) { This->used = 0; This->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT DS_CreateSoundBuffer(IDirectSound8Impl* This, DSBUFFERDESC* d,
        IDirectSoundBuffer8Impl** b, IUnknown outer) {
    (void)This; (void)outer;
    if (!b || !d) return DSERR_INVALIDPARAM;
    IDirectSoundBuffer8Impl* p = alloc_dsbuffer(d);
    if (!p) { *b = 0; return DSERR_OUTOFMEMORY; }
    *b = p; return DS_OK;
}
static HRESULT DS_GetCaps(IDirectSound8Impl* This, void* c) {
    (void)This; (void)c; return DS_OK;
}
static HRESULT DS_DuplicateSoundBuffer(IDirectSound8Impl* This,
        IDirectSoundBuffer8Impl* orig, IDirectSoundBuffer8Impl** dup) {
    (void)This;
    if (!orig || !dup) return DSERR_INVALIDPARAM;
    IDirectSoundBuffer8Impl* p = alloc_dsbuffer(0);
    if (!p) { *dup = 0; return DSERR_OUTOFMEMORY; }
    p->flags       = orig->flags;
    p->bufferBytes = orig->bufferBytes;
    p->volume      = orig->volume;
    p->frequency   = orig->frequency;
    p->format      = orig->format;
    *dup = p; return DS_OK;
}
static HRESULT DS_SetCooperativeLevel(IDirectSound8Impl* This, HWND h, DWORD lvl) {
    (void)h; This->coopLevel = lvl; return DS_OK;
}
static HRESULT DS_Compact(IDirectSound8Impl* This) { (void)This; return DS_OK; }
static HRESULT DS_GetSpeakerConfig(IDirectSound8Impl* This, DWORD* c) {
    (void)This; if (c) *c = 4 | (1 << 16);    // DSSPEAKER_STEREO | GEOMETRY_NARROW<<16
    return DS_OK;
}
static HRESULT DS_SetSpeakerConfig(IDirectSound8Impl* This, DWORD c) {
    (void)This; (void)c; return DS_OK;
}
static HRESULT DS_Initialize(IDirectSound8Impl* This, REFGUID g) {
    (void)This; (void)g; return DS_OK;
}
static HRESULT DS_VerifyCertification(IDirectSound8Impl* This, DWORD* cert) {
    (void)This; if (cert) *cert = 1;       // DS_CERTIFIED
    return DS_OK;
}

static const IDirectSound8Vtbl g_dsVtbl = {
    DS_QueryInterface, DS_AddRef, DS_Release,
    DS_CreateSoundBuffer, DS_GetCaps, DS_DuplicateSoundBuffer,
    DS_SetCooperativeLevel, DS_Compact, DS_GetSpeakerConfig,
    DS_SetSpeakerConfig, DS_Initialize, DS_VerifyCertification,
};

static IDirectSound8Impl* alloc_dsound(void) {
    for (int i = 0; i < MAX_DSOUND; i++) {
        if (!g_dsounds[i].used) {
            zero_mem(&g_dsounds[i], sizeof(g_dsounds[i]));
            g_dsounds[i].used     = 1;
            g_dsounds[i].refCount = 1;
            g_dsounds[i].lpVtbl   = &g_dsVtbl;
            return &g_dsounds[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry points exportados (mesmos do dsound.dll do Windows).
// ============================================================================

// DirectSoundCreate(guid, **out, outer): a versao DirectSound 1.x.
__declspec(dllexport) HRESULT DirectSoundCreate(REFGUID guid,
        IDirectSound8Impl** out, IUnknown outer) {
    (void)guid; (void)outer;
    if (!out) return DSERR_INVALIDPARAM;
    IDirectSound8Impl* p = alloc_dsound();
    if (!p) { *out = 0; return DSERR_OUTOFMEMORY; }
    *out = p; return DS_OK;
}

// DirectSoundCreate8(guid, **out, outer): a versao DirectSound 8 (canonica).
__declspec(dllexport) HRESULT DirectSoundCreate8(REFGUID guid,
        IDirectSound8Impl** out, IUnknown outer) {
    (void)guid; (void)outer;
    if (!out) return DSERR_INVALIDPARAM;
    IDirectSound8Impl* p = alloc_dsound();
    if (!p) { *out = 0; return DSERR_OUTOFMEMORY; }
    *out = p; return DS_OK;
}

// DirectSoundEnumerate(cb, ctx): enumera dispositivos. Chama callback uma vez
// com o nosso "MeuOS Default Audio".
typedef BOOL (*LPDSENUMCALLBACKA)(void* guid, const char* desc, const char* drvName, void* ctx);
__declspec(dllexport) HRESULT DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, void* ctx) {
    if (cb) {
        static char desc[] = "MeuOS Default Audio Device";
        static char drv[]  = "audio.sys";
        cb(0, desc, drv, ctx);
    }
    return DS_OK;
}

// DirectSoundCaptureCreate8: o stub trata captura igual a render (apenas devolve
// um IDirectSound8 equivalente — apps de captura tipicamente usam metodos
// distintos, mas o ABI inicial e o mesmo "create object" + cooperative level).
__declspec(dllexport) HRESULT DirectSoundCaptureCreate8(REFGUID guid,
        IDirectSound8Impl** out, IUnknown outer) {
    return DirectSoundCreate8(guid, out, outer);
}

// DllGetClassObject: stub COM in-proc server. Devolve um IDirectSound8.
__declspec(dllexport) HRESULT DllGetClassObject(REFCLSID rclsid, REFIID riid,
        void** ppv) {
    (void)rclsid; (void)riid;
    if (!ppv) return DSERR_INVALIDPARAM;
    IDirectSound8Impl* p = alloc_dsound();
    if (!p) { *ppv = 0; return DSERR_OUTOFMEMORY; }
    *ppv = p; return DS_OK;
}

__declspec(dllexport) HRESULT DllCanUnloadNow(void) { return 1; /* S_FALSE */ }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
