// dwmapi.dll — Desktop Window Manager API (MeuOS). O explorer real importa 16 funcoes
// (8 nomeadas + 8 ordinais PRIVADOS noname) daqui. Como o MeuOS NAO tem composicao DWM,
// as respostas sao HONESTAS: DwmIsCompositionEnabled -> FALSE (o explorer entao usa o
// caminho CLASSICO/nao-composto, que e o que conseguimos desenhar); atributos de janela e
// thumbnails viram no-ops que devolvem S_OK (o explorer trata a ausencia de DWM sem quebrar).
// Sem catch-all: cada export e nomeado. Os 8 ordinais privados sao exportados por ordinal
// via dwmapi.def (aditivo aos __declspec, igual ao user32.def/uxtheme.def).
#include <stdint.h>

unsigned int _tls_index = 0;   // simbolo que o linker/CRT espera (igual as outras DLLs)

typedef long HRESULT;
#define S_OK          ((HRESULT)0)
#define E_NOTIMPL     ((HRESULT)0x80004001L)
#define DWM_E_COMPOSITIONDISABLED ((HRESULT)0x80263001L)
typedef int BOOL;

__attribute__((used)) int _DllMainCRTStartup(void* h, unsigned r, void* p) { (void)h;(void)r;(void)p; return 1; }
int DllMain(void* h, unsigned r, void* p) { (void)h;(void)r;(void)p; return 1; }

// ---- API nomeada que o explorer importa ----
// DwmIsCompositionEnabled: HONESTO -> composicao DESLIGADA (nao temos DWM). O explorer usa
// o caminho classico (sem blur/vidro), que e o que o nosso win32k consegue compor.
__declspec(dllexport) HRESULT DwmIsCompositionEnabled(BOOL* pfEnabled) { if (pfEnabled) *pfEnabled = 0; return S_OK; }

// DwmGetWindowAttribute(hwnd, attr, pvAttribute, cbAttribute): zera o buffer e devolve S_OK.
__declspec(dllexport) HRESULT DwmGetWindowAttribute(void* hwnd, unsigned attr, void* pv, unsigned cb) {
    (void)hwnd; (void)attr; if (pv) { unsigned char* b = (unsigned char*)pv; for (unsigned i = 0; i < cb; i++) b[i] = 0; } return S_OK;
}
// DwmSetWindowAttribute: no-op (atributos de composicao nao se aplicam sem DWM) -> S_OK.
__declspec(dllexport) HRESULT DwmSetWindowAttribute(void* hwnd, unsigned attr, const void* pv, unsigned cb) { (void)hwnd;(void)attr;(void)pv;(void)cb; return S_OK; }
// DwmEnableBlurBehindWindow: no-op -> S_OK (sem blur, mas o explorer segue).
__declspec(dllexport) HRESULT DwmEnableBlurBehindWindow(void* hwnd, const void* pBlurBehind) { (void)hwnd;(void)pBlurBehind; return S_OK; }

// Thumbnails (live preview da taskbar): sem composicao, damos um handle fake e no-ops S_OK.
static unsigned long long g_thumb = 0x7000;
__declspec(dllexport) HRESULT DwmRegisterThumbnail(void* dest, void* src, void** phThumbnailId) { (void)dest;(void)src; if (phThumbnailId) { g_thumb += 4; *phThumbnailId = (void*)g_thumb; } return S_OK; }
__declspec(dllexport) HRESULT DwmUnregisterThumbnail(void* hThumbnailId) { (void)hThumbnailId; return S_OK; }
__declspec(dllexport) HRESULT DwmUpdateThumbnailProperties(void* hThumbnailId, const void* ptnProperties) { (void)hThumbnailId;(void)ptnProperties; return S_OK; }
__declspec(dllexport) HRESULT DwmQueryThumbnailSourceSize(void* hThumbnailId, void* pSize) { (void)hThumbnailId; if (pSize) { unsigned* s = (unsigned*)pSize; s[0] = 0; s[1] = 0; } return S_OK; }

// ---- ordinais PRIVADOS noname que o explorer importa POR ORDINAL (dwmapi.def) ----
// Sem nome publico na dwmapi real; retornam S_OK (0) — o padrao seguro p/ funcoes DWM de
// init (a maioria devolve HRESULT). Se alguma precisar de out-param, ajustar quando o
// fluxo (pos-threads) as exercitar de fato.
__declspec(dllexport) HRESULT Dwm_ord113(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
__declspec(dllexport) HRESULT Dwm_ord114(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
__declspec(dllexport) HRESULT Dwm_ord124(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
__declspec(dllexport) HRESULT Dwm_ord138(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
__declspec(dllexport) HRESULT Dwm_ord139(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
__declspec(dllexport) HRESULT Dwm_ord140(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
__declspec(dllexport) HRESULT Dwm_ord141(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
__declspec(dllexport) HRESULT Dwm_ord159(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return S_OK; }
