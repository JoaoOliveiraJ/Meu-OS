// winmm.dll — reimplementacao minima da API multimedia legada do Windows
// (FASE 11 audio stack). Equivalente a Windows NT 3.1 winmm.dll.
//
// O winmm e a API mais antiga ainda relevante em Windows 10/11: PlaySoundA/W,
// waveOutOpen/Write/Close, timeGetTime, mciSendString. Apps modernos (ate VB6,
// .NET WinForms padrao) chamam PlaySound; jogos antigos usam waveOut*.
//
// Aqui no MeuOS este stub:
//   - PlaySound retorna TRUE (sem produzir som — driver audio.sys nao tem DMA).
//   - waveOutOpen devolve um HWAVEOUT fake; Write loga "playback fake".
//   - timeGetTime devolve um contador monotonico baseado em DllMain init time.
//   - mciSendString stub que sempre retorna 0 (sucesso).
//
// Nao depende de outras DLLs do MeuOS — encapsulado para nao puxar imports
// circulares. DllMain inicializa o timer interno.
//
// IMAGE BASE: 0x4D00000 — zona livre apos dsound (0x4C00000), com .reloc via
// --dynamicbase.

unsigned int _tls_index = 0;

// ============================================================================
//  Tipos Windows minimos.
// ============================================================================
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               LRESULT;
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef void*              HANDLE;
typedef void*              HWND;
typedef void*              HMODULE;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef unsigned char      BYTE;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef unsigned long long ULL;
typedef DWORD              MMRESULT;
typedef void*              HWAVEOUT;
typedef void*              HWAVEIN;
typedef void*              HMIDIOUT;
typedef void*              HMIDIIN;

#define MMSYSERR_NOERROR             0
#define MMSYSERR_ERROR               1
#define MMSYSERR_BADDEVICEID         2
#define MMSYSERR_NOTENABLED          3
#define MMSYSERR_ALLOCATED           4
#define MMSYSERR_INVALHANDLE         5
#define MMSYSERR_NODRIVER            6
#define MMSYSERR_NOMEM               7
#define MMSYSERR_NOTSUPPORTED        8
#define MMSYSERR_BADERRNUM           9
#define MMSYSERR_INVALFLAG           10
#define MMSYSERR_INVALPARAM          11

// Flags do PlaySound.
#define SND_SYNC            0x0000
#define SND_ASYNC           0x0001
#define SND_NODEFAULT       0x0002
#define SND_MEMORY          0x0004
#define SND_LOOP            0x0008
#define SND_NOSTOP          0x0010
#define SND_NOWAIT          0x00002000
#define SND_ALIAS           0x00010000
#define SND_FILENAME        0x00020000
#define SND_RESOURCE        0x00040004
#define SND_PURGE           0x0040
#define SND_APPLICATION     0x0080

// WAVEFORMATEX — usado por waveOutOpen.
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

typedef struct wavehdr_tag {
    void*  lpData;
    DWORD  dwBufferLength;
    DWORD  dwBytesRecorded;
    DWORD  dwUser;
    DWORD  dwFlags;
    DWORD  dwLoops;
    struct wavehdr_tag* lpNext;
    DWORD  reserved;
} WAVEHDR;
#pragma pack(pop)

#define WHDR_DONE       0x00000001
#define WHDR_PREPARED   0x00000002
#define WHDR_BEGINLOOP  0x00000004
#define WHDR_ENDLOOP    0x00000008
#define WHDR_INQUEUE    0x00000010

// ============================================================================
//  Estado interno.
// ============================================================================
//
//  Para timeGetTime usamos um contador que comeca em 0 e incrementa a cada
//  chamada. Apps que comparam diferencas (delta=tnow-tlast) recebem deltas
//  positivos. Sem PIT/TSC reais em ring 3 do MeuOS, esta e a aproximacao.
static DWORD g_tick = 0;
static DWORD g_tickStart = 0;

// Pool simples de WAVEOUT handles fake.
#define MAX_WAVE 8
typedef struct waveout_state {
    int  used;
    WAVEFORMATEX format;
    DWORD volume;
    void* callback;     // CALLBACK function pointer (no-op)
} waveout_state_t;
static waveout_state_t g_waveouts[MAX_WAVE];

static void zero_mem(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

// ============================================================================
//  PlaySound — API mais usada do winmm.
//
//  PlaySoundA(pszSound, hmod, fdwSound):
//    pszSound: nome do .wav, alias, ou ponteiro para memoria (com SND_MEMORY)
//    hmod: HMODULE de onde o recurso vem (SND_RESOURCE)
//    fdwSound: flags SND_*
//
//  Sem driver de PCM real, sempre devolve TRUE (sucesso "logico").
// ============================================================================
__declspec(dllexport) BOOL PlaySoundA(const char* sound, HMODULE mod, DWORD flags) {
    (void)sound; (void)mod; (void)flags;
    return 1;   // TRUE: "played" sem audio real
}

__declspec(dllexport) BOOL PlaySoundW(const WCHAR* sound, HMODULE mod, DWORD flags) {
    (void)sound; (void)mod; (void)flags;
    return 1;
}

// Alias clasico.
__declspec(dllexport) BOOL sndPlaySoundA(const char* sound, UINT flags) {
    (void)sound; (void)flags;
    return 1;
}

__declspec(dllexport) BOOL sndPlaySoundW(const WCHAR* sound, UINT flags) {
    (void)sound; (void)flags;
    return 1;
}

// ============================================================================
//  waveOut* — API mais antiga para playback PCM bruto.
// ============================================================================
__declspec(dllexport) UINT waveOutGetNumDevs(void) { return 1; }

__declspec(dllexport) MMRESULT waveOutOpen(HWAVEOUT* out, UINT devId,
        WAVEFORMATEX* fmt, DWORD callback, DWORD instance, DWORD flags) {
    (void)devId; (void)flags; (void)instance;
    if (!out) return MMSYSERR_INVALPARAM;
    for (int i = 0; i < MAX_WAVE; i++) {
        if (!g_waveouts[i].used) {
            zero_mem(&g_waveouts[i], sizeof(g_waveouts[i]));
            g_waveouts[i].used     = 1;
            g_waveouts[i].volume   = 0xFFFFFFFF;     // max volume
            g_waveouts[i].callback = (void*)(ULL)callback;
            if (fmt) g_waveouts[i].format = *fmt;
            else {
                g_waveouts[i].format.wFormatTag      = 1;
                g_waveouts[i].format.nChannels       = 2;
                g_waveouts[i].format.nSamplesPerSec  = 44100;
                g_waveouts[i].format.wBitsPerSample  = 16;
            }
            *out = (HWAVEOUT)((ULL)0xA0D10000ULL | (ULL)i);
            return MMSYSERR_NOERROR;
        }
    }
    *out = 0;
    return MMSYSERR_ALLOCATED;
}

static int handle_to_index(HWAVEOUT h) {
    ULL v = (ULL)h;
    if ((v & ~0xFFULL) != 0xA0D10000ULL) return -1;
    int idx = (int)(v & 0xFF);
    if (idx < 0 || idx >= MAX_WAVE) return -1;
    if (!g_waveouts[idx].used) return -1;
    return idx;
}

__declspec(dllexport) MMRESULT waveOutClose(HWAVEOUT h) {
    int i = handle_to_index(h);
    if (i < 0) return MMSYSERR_INVALHANDLE;
    g_waveouts[i].used = 0;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR* hdr, UINT sz) {
    (void)sz; (void)h;
    if (!hdr) return MMSYSERR_INVALPARAM;
    hdr->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR* hdr, UINT sz) {
    (void)sz; (void)h;
    if (!hdr) return MMSYSERR_INVALPARAM;
    hdr->dwFlags &= ~WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR* hdr, UINT sz) {
    (void)sz;
    if (handle_to_index(h) < 0) return MMSYSERR_INVALHANDLE;
    if (!hdr) return MMSYSERR_INVALPARAM;
    // "Toca" o buffer instantaneamente: marca WHDR_DONE.
    hdr->dwFlags |= WHDR_DONE;
    hdr->dwFlags &= ~WHDR_INQUEUE;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutReset(HWAVEOUT h) {
    if (handle_to_index(h) < 0) return MMSYSERR_INVALHANDLE;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutPause(HWAVEOUT h) {
    if (handle_to_index(h) < 0) return MMSYSERR_INVALHANDLE;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutRestart(HWAVEOUT h) {
    if (handle_to_index(h) < 0) return MMSYSERR_INVALHANDLE;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutSetVolume(HWAVEOUT h, DWORD vol) {
    int i = handle_to_index(h);
    if (i < 0) return MMSYSERR_INVALHANDLE;
    g_waveouts[i].volume = vol;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT waveOutGetVolume(HWAVEOUT h, DWORD* vol) {
    int i = handle_to_index(h);
    if (i < 0) return MMSYSERR_INVALHANDLE;
    if (!vol) return MMSYSERR_INVALPARAM;
    *vol = g_waveouts[i].volume;
    return MMSYSERR_NOERROR;
}

// waveOutGetDevCapsA: descreve o dispositivo. Estrutura WAVEOUTCAPS varia em
// tamanho conforme o app; preencher um nome curto e suficiente.
typedef struct {
    WORD wMid;
    WORD wPid;
    DWORD vDriverVersion;
    char szPname[32];
    DWORD dwFormats;
    WORD wChannels;
    WORD wReserved1;
    DWORD dwSupport;
} WAVEOUTCAPSA;

__declspec(dllexport) MMRESULT waveOutGetDevCapsA(UINT devId, WAVEOUTCAPSA* caps, UINT sz) {
    (void)devId; (void)sz;
    if (!caps) return MMSYSERR_INVALPARAM;
    caps->wMid = 0;
    caps->wPid = 0;
    caps->vDriverVersion = 0x0100;
    const char name[] = "MeuOS Wave Out";
    int j;
    for (j = 0; name[j] && j < 31; j++) caps->szPname[j] = name[j];
    caps->szPname[j] = 0;
    caps->dwFormats = 0x000000FF;  // todos os formatos PCM comuns
    caps->wChannels = 2;
    caps->wReserved1 = 0;
    caps->dwSupport = 0;
    return MMSYSERR_NOERROR;
}

// ============================================================================
//  timeGetTime / timeGetDevCaps / timeBeginPeriod / timeEndPeriod.
// ============================================================================
__declspec(dllexport) DWORD timeGetTime(void) {
    // Cada chamada avanca o contador em 10 ms (apps tipicos chamam <10 vezes/s).
    g_tick += 10;
    return g_tick - g_tickStart;
}

typedef struct {
    UINT wPeriodMin;
    UINT wPeriodMax;
} TIMECAPS;

__declspec(dllexport) MMRESULT timeGetDevCaps(TIMECAPS* caps, UINT sz) {
    (void)sz;
    if (!caps) return MMSYSERR_INVALPARAM;
    caps->wPeriodMin = 1;
    caps->wPeriodMax = 1000000;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT timeBeginPeriod(UINT period) {
    (void)period; return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT timeEndPeriod(UINT period) {
    (void)period; return MMSYSERR_NOERROR;
}

// ============================================================================
//  mciSendString — controle de MCI (Multimedia Control Interface) legacy.
//
//  Apps muito antigos (anos 90) usavam mciSendString("play foo.wav") etc.
//  O stub aceita qualquer comando e retorna 0 (sucesso).
// ============================================================================
__declspec(dllexport) DWORD mciSendStringA(const char* cmd, char* ret, UINT retLen, HANDLE cb) {
    (void)cmd; (void)cb;
    if (ret && retLen > 0) ret[0] = 0;
    return 0;
}

__declspec(dllexport) DWORD mciSendStringW(const WCHAR* cmd, WCHAR* ret, UINT retLen, HANDLE cb) {
    (void)cmd; (void)cb;
    if (ret && retLen > 0) ret[0] = 0;
    return 0;
}

__declspec(dllexport) DWORD mciGetErrorStringA(DWORD code, char* buf, UINT len) {
    (void)code;
    if (!buf || len == 0) return 0;
    const char msg[] = "OK";
    UINT i;
    for (i = 0; i < len - 1 && msg[i]; i++) buf[i] = msg[i];
    buf[i] = 0;
    return 1;
}

// ============================================================================
//  midiOut* — stubs minimos (apps muito raros).
// ============================================================================
__declspec(dllexport) MMRESULT midiOutOpen(HMIDIOUT* out, UINT devId,
        DWORD cb, DWORD inst, DWORD flags) {
    (void)devId; (void)cb; (void)inst; (void)flags;
    if (!out) return MMSYSERR_INVALPARAM;
    *out = (HMIDIOUT)0xD1D10000ULL;
    return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT midiOutClose(HMIDIOUT h) {
    (void)h; return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT midiOutShortMsg(HMIDIOUT h, DWORD msg) {
    (void)h; (void)msg; return MMSYSERR_NOERROR;
}

__declspec(dllexport) UINT midiOutGetNumDevs(void) { return 1; }

// ============================================================================
//  joyGetPos / joyGetNumDevs — apenas para nao quebrar imports.
// ============================================================================
__declspec(dllexport) UINT joyGetNumDevs(void) { return 0; }

__declspec(dllexport) MMRESULT joyGetPos(UINT id, void* info) {
    (void)id; (void)info; return MMSYSERR_NODRIVER;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reserved;
    // DLL_PROCESS_ATTACH = 1
    if (reason == 1) {
        g_tickStart = 0;
        g_tick = 0;
    }
    return 1;
}
