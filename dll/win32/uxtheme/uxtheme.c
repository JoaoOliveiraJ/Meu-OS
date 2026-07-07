// uxtheme.dll — Visual Styles / theming p/ rodar o explorer.exe REAL. Cobre as 28
// funcoes de UxTheme.dll que o explorer importa (25 por nome + 3 por ordinal).
//
// FILOSOFIA (HONESTA, sem catch-all): nao temos motor de temas (sem atlas de partes de
// tema, sem .msstyles, sem composicao DWM). Entao reportamos "tema INATIVO" — exatamente
// como o Windows em modo classico:
//   - IsThemeActive/IsAppThemed/IsCompositionActive -> FALSE. O explorer entao usa o
//     caminho CLASSICO de desenho (GetSysColor/FillRect via GDI, que ja temos) em vez do
//     caminho tematizado. E' a degradacao correta e documentada.
//   - OpenThemeData* -> NULL (sem dados de tema). Codigo bem-comportado checa IsThemeActive
//     antes e nem chama; se chamar, NULL == "sem tema", tambem documentado.
//   - Getters (GetThemeColor/Font/Int/Bool/Margins/Metric/PartSize/BackgroundExtent) com
//     handle NULL -> E_HANDLE (0x80070006) e ZERAM o out-param (defensivo p/ quem ignora o
//     HRESULT). O chamador usa o proprio default.
//   - Draw* com handle NULL -> E_HANDLE (nada desenhado; o classico ja pinta).
//   - BufferedPaint: Init/UnInit -> S_OK; BeginBufferedPaint devolve NULL e aponta *phdc p/
//     o HDC alvo -> o app cai no fallback documentado (desenha direto no alvo). End/SetAlpha
//     -> S_OK; GetBufferedPaintBits -> E_FAIL (sem buffer).
//   - 3 ordinais privados (immersive/dark-mode, introduzidos no Win10): #138 =
//     IsDarkModeAllowedForWindow (BOOL) -> FALSE; #86/#126 = helpers immersive -> 0. Nao
//     suportamos tema immersive: 0/FALSE e' a resposta honesta.
// Stubs ESPECIFICOS e nomeados (um por funcao que o explorer importa), NUNCA um catch-all.
// Autocontido (sem ntdll/imports) -> caminho PMM+reloc limpo, igual a combase.

typedef unsigned long long ULL_;
typedef long               HRESULT_;
typedef int                BOOL_;
typedef unsigned long      DWORD_;
typedef unsigned int       UINT_;
typedef void*              HANDLE_;
typedef unsigned short     WCHAR_;
typedef unsigned long      COLORREF_;
unsigned int _tls_index = 0;

#define S_OK_        ((HRESULT_)0)
#define E_FAIL_      ((HRESULT_)0x80004005L)
#define E_HANDLE_    ((HRESULT_)0x80070006L)   // "handle invalido" — tema NULL == sem dados
#define FALSE_       0
#define TRUE_        1

typedef struct { long left, top, right, bottom; } RECT_;
typedef struct { long cx, cy; } SIZE_;
typedef struct { int cxLeftWidth, cxRightWidth, cyTopHeight, cyBottomHeight; } MARGINS_;
// LOGFONTW: 5 LONG + 8 BYTE + 32 WCHAR (mesmo layout do Win32; so precisamos do tamanho).
typedef struct { long lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
                 unsigned char b[8]; WCHAR_ lfFaceName[32]; } LOGFONTW_;

static void zero_(void* p, ULL_ n) { unsigned char* d = (unsigned char*)p; for (ULL_ i = 0; i < n; i++) d[i] = 0; }

// ---- DIAGNOSTICO opcional (int 0x80 SYS_WRITE, sem import) — gated OFF. ----
#define UXTHEME_DBG 0
#if UXTHEME_DBG
static void dbg_puts(const char* s) { ULL_ ret; __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(1ULL), "D"(s) : "memory", "rcx", "r11"); }
#define UXLOG(s) dbg_puts("[ux] " s "\n")
#else
#define UXLOG(s) ((void)0)
#endif

// ====================================================================
//  Estado do tema — INATIVO (modo classico). As 3 consultas booleanas.
// ====================================================================
__declspec(dllexport) BOOL_ IsThemeActive(void)       { UXLOG("IsThemeActive->FALSE"); return FALSE_; }
__declspec(dllexport) BOOL_ IsAppThemed(void)         { return FALSE_; }
__declspec(dllexport) BOOL_ IsCompositionActive(void) { return FALSE_; }   // sem DWM

// ====================================================================
//  Abertura/fechamento de dados de tema — sem tema disponivel.
// ====================================================================
__declspec(dllexport) HANDLE_ OpenThemeData(HANDLE_ hwnd, const WCHAR_* classList) {
    (void)hwnd; (void)classList; UXLOG("OpenThemeData->NULL"); return 0;   // sem dados de tema
}
__declspec(dllexport) HANDLE_ OpenThemeDataForDpi(HANDLE_ hwnd, const WCHAR_* classList, int dpi) {
    (void)hwnd; (void)classList; (void)dpi; return 0;
}
__declspec(dllexport) HRESULT_ CloseThemeData(HANDLE_ hTheme) { (void)hTheme; return S_OK_; }
__declspec(dllexport) HANDLE_ GetWindowTheme(HANDLE_ hwnd) { (void)hwnd; return 0; }
__declspec(dllexport) HRESULT_ SetWindowTheme(HANDLE_ hwnd, const WCHAR_* sub, const WCHAR_* id) {
    (void)hwnd; (void)sub; (void)id; return S_OK_;   // no-op: nada a aplicar sem motor de temas
}

// ====================================================================
//  Desenho de tema — sem handle de tema -> E_HANDLE (o caminho classico pinta).
// ====================================================================
__declspec(dllexport) HRESULT_ DrawThemeBackground(HANDLE_ hTheme, HANDLE_ hdc, int part, int state,
        const RECT_* pRect, const RECT_* pClip) {
    (void)hTheme; (void)hdc; (void)part; (void)state; (void)pRect; (void)pClip; return E_HANDLE_;
}
__declspec(dllexport) HRESULT_ DrawThemeParentBackground(HANDLE_ hwnd, HANDLE_ hdc, const RECT_* prc) {
    (void)hwnd; (void)hdc; (void)prc; return S_OK_;   // no classico o filho ja pinta seu fundo
}
// DrawThemeTextEx(hTheme, hdc, part, state, text, cch, flags, pRect, pOptions)
__declspec(dllexport) HRESULT_ DrawThemeTextEx(HANDLE_ hTheme, HANDLE_ hdc, int part, int state,
        const WCHAR_* text, int cch, DWORD_ flags, RECT_* pRect, const void* pOptions) {
    (void)hTheme; (void)hdc; (void)part; (void)state; (void)text; (void)cch; (void)flags; (void)pRect; (void)pOptions;
    return E_HANDLE_;
}

// ====================================================================
//  Getters — handle NULL -> E_HANDLE + zera o out (defensivo).
// ====================================================================
__declspec(dllexport) HRESULT_ GetThemeColor(HANDLE_ hT, int part, int state, int prop, COLORREF_* pColor) {
    (void)hT; (void)part; (void)state; (void)prop; if (pColor) *pColor = 0; return E_HANDLE_;
}
__declspec(dllexport) HRESULT_ GetThemeInt(HANDLE_ hT, int part, int state, int prop, int* piVal) {
    (void)hT; (void)part; (void)state; (void)prop; if (piVal) *piVal = 0; return E_HANDLE_;
}
__declspec(dllexport) HRESULT_ GetThemeBool(HANDLE_ hT, int part, int state, int prop, BOOL_* pfVal) {
    (void)hT; (void)part; (void)state; (void)prop; if (pfVal) *pfVal = FALSE_; return E_HANDLE_;
}
__declspec(dllexport) HRESULT_ GetThemeMetric(HANDLE_ hT, HANDLE_ hdc, int part, int state, int prop, int* piVal) {
    (void)hT; (void)hdc; (void)part; (void)state; (void)prop; if (piVal) *piVal = 0; return E_HANDLE_;
}
__declspec(dllexport) HRESULT_ GetThemeFont(HANDLE_ hT, HANDLE_ hdc, int part, int state, int prop, LOGFONTW_* pFont) {
    (void)hT; (void)hdc; (void)part; (void)state; (void)prop; if (pFont) zero_(pFont, sizeof(*pFont)); return E_HANDLE_;
}
__declspec(dllexport) HRESULT_ GetThemeMargins(HANDLE_ hT, HANDLE_ hdc, int part, int state, int prop,
        const RECT_* prc, MARGINS_* pM) {
    (void)hT; (void)hdc; (void)part; (void)state; (void)prop; (void)prc; if (pM) zero_(pM, sizeof(*pM)); return E_HANDLE_;
}
// GetThemePartSize(hTheme, hdc, part, state, prc, eSize, psz)
__declspec(dllexport) HRESULT_ GetThemePartSize(HANDLE_ hT, HANDLE_ hdc, int part, int state,
        const RECT_* prc, int eSize, SIZE_* psz) {
    (void)hT; (void)hdc; (void)part; (void)state; (void)prc; (void)eSize; if (psz) { psz->cx = 0; psz->cy = 0; } return E_HANDLE_;
}
__declspec(dllexport) HRESULT_ GetThemeBackgroundExtent(HANDLE_ hT, HANDLE_ hdc, int part, int state,
        const RECT_* pContentRect, RECT_* pExtentRect) {
    (void)hT; (void)hdc; (void)part; (void)state; (void)pContentRect; if (pExtentRect) zero_(pExtentRect, sizeof(*pExtentRect)); return E_HANDLE_;
}

// ====================================================================
//  Buffered paint — sem buffer real; fallback documentado (desenha direto).
// ====================================================================
__declspec(dllexport) HRESULT_ BufferedPaintInit(void)   { return S_OK_; }
__declspec(dllexport) HRESULT_ BufferedPaintUnInit(void) { return S_OK_; }
// BeginBufferedPaint(hdcTarget, prcTarget, dwFormat, pPaintParams, phdc) -> HPAINTBUFFER
// Devolve NULL e aponta *phdc p/ o HDC alvo: o app cai no fallback (desenha no alvo direto).
__declspec(dllexport) HANDLE_ BeginBufferedPaint(HANDLE_ hdcTarget, const RECT_* prc, DWORD_ fmt,
        void* pPaintParams, HANDLE_* phdc) {
    (void)prc; (void)fmt; (void)pPaintParams;
    if (phdc) *phdc = hdcTarget;   // fallback: desenha direto no alvo
    return 0;                      // sem buffer -> app usa o caminho nao-bufferizado
}
__declspec(dllexport) HRESULT_ EndBufferedPaint(HANDLE_ hbp, BOOL_ fUpdate) {
    (void)hbp; (void)fUpdate; return S_OK_;   // nada a compor (Begin devolveu NULL)
}
__declspec(dllexport) HRESULT_ BufferedPaintSetAlpha(HANDLE_ hbp, const RECT_* prc, unsigned char alpha) {
    (void)hbp; (void)prc; (void)alpha; return S_OK_;
}
__declspec(dllexport) HRESULT_ GetBufferedPaintBits(HANDLE_ hbp, void** ppb, int* pcxRow) {
    (void)hbp; if (ppb) *ppb = 0; if (pcxRow) *pcxRow = 0; return E_FAIL_;   // sem buffer
}

// ====================================================================
//  Ordinais privados (immersive/dark-mode do Win10). Exportados via uxtheme.def
//  nos ordinais #86/#126/#138. Nao suportamos tema immersive -> 0/FALSE (honesto).
//  #138 = IsDarkModeAllowedForWindow(HWND) -> FALSE. #86/#126 = helpers immersive -> 0.
// ====================================================================
__declspec(dllexport) BOOL_ ux_ord138_IsDarkModeAllowedForWindow(HANDLE_ hwnd) { (void)hwnd; return FALSE_; }
__declspec(dllexport) ULL_  ux_ord86_immersive(ULL_ a, ULL_ b, ULL_ c, ULL_ d) { (void)a; (void)b; (void)c; (void)d; return 0; }
__declspec(dllexport) ULL_  ux_ord126_immersive(ULL_ a, ULL_ b, ULL_ c, ULL_ d) { (void)a; (void)b; (void)c; (void)d; return 0; }

// DllMain — sem TLS/estado; entrada minima.
__declspec(dllexport) int DllMain(void* h, unsigned r, void* v) { (void)h; (void)r; (void)v; return 1; }
