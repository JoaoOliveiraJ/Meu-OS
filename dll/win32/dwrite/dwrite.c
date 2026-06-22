// dwrite.dll  —  reimplementacao minima do DirectWrite (FASE 9.9).
//
// DirectWrite e a API moderna de texto do Windows 7+, substituindo Uniscribe e
// trabalhando junto com Direct2D para renderizar texto de alta qualidade.
// Apps modernos (Edge/Office/Acrobat) usam DirectWrite para layout de texto:
// quebra de linha, ligaduras, fontes OpenType, anti-aliasing sub-pixel
// (ClearType + DirectWrite outline) e cache de glifos por GPU.
//
// Pipeline real: app -> dwrite.dll (fontes/layout) -> d2d1.dll (raster) ->
// d3d10/11 -> dxgi -> dxgkrnl -> driver. Aqui no MeuOS so temos um framebuffer
// linear (BasicDisplay) e font8x8 bitmap em modo texto; entao este stub se
// limita ao ABI COM (vtable + refcount) de IDWriteFactory + IDWriteTextFormat
// + IDWriteTextLayout + IDWriteFontCollection. TUDO retorna S_OK com handles
// fake e metricas falsas porem coerentes.
//
// COM ABI (estilo Microsoft): cada interface e {const Vtbl* lpVtbl; ...};
// chamada virtual = lpVtbl->Metodo(this, args...). thiscall = primeiro arg
// "this". Em ABI ms_abi (x86_64-windows-gnu) os parametros entram em
// RCX,RDX,R8,R9 (essa e a ABI que o zig cc gera com -target windows-gnu).
//
// Pools estaticos: 4 factories, 16 text formats, 16 text layouts, 4 font collections,
// 16 font families, 16 fonts. Pequenos porque a maioria dos apps cria umas poucas
// instancias e as reutiliza para muitos draws.
//
// IMAGE BASE: 0x4800000 — sobreposicao com PMM_BASE (0x4000000). Para evitar
// colisao usamos --dynamicbase no build (.reloc), entao o loader pode realocar
// para qualquer endereco virtual livre. Mesmo mecanismo de d3d11/d3d12/d2d1.

unsigned int _tls_index = 0;

// ============================================================================
//  Tipos Windows minimos.
// ============================================================================
typedef unsigned int       UINT;
typedef unsigned int       UINT32;
typedef unsigned short     UINT16;
typedef short              INT16;
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

// DWRITE_FACTORY_TYPE — SHARED reutiliza recursos do sistema; ISOLATED tem cache proprio.
#define DWRITE_FACTORY_TYPE_SHARED   0
#define DWRITE_FACTORY_TYPE_ISOLATED 1

// DWRITE_FONT_WEIGHT.
#define DWRITE_FONT_WEIGHT_THIN          100
#define DWRITE_FONT_WEIGHT_LIGHT         300
#define DWRITE_FONT_WEIGHT_NORMAL        400
#define DWRITE_FONT_WEIGHT_REGULAR       400
#define DWRITE_FONT_WEIGHT_MEDIUM        500
#define DWRITE_FONT_WEIGHT_SEMI_BOLD     600
#define DWRITE_FONT_WEIGHT_BOLD          700
#define DWRITE_FONT_WEIGHT_BLACK         900

// DWRITE_FONT_STYLE.
#define DWRITE_FONT_STYLE_NORMAL         0
#define DWRITE_FONT_STYLE_OBLIQUE        1
#define DWRITE_FONT_STYLE_ITALIC         2

// DWRITE_FONT_STRETCH.
#define DWRITE_FONT_STRETCH_NORMAL       5
#define DWRITE_FONT_STRETCH_MEDIUM       5

// DWRITE_TEXT_ALIGNMENT.
#define DWRITE_TEXT_ALIGNMENT_LEADING    0
#define DWRITE_TEXT_ALIGNMENT_TRAILING   1
#define DWRITE_TEXT_ALIGNMENT_CENTER     2
#define DWRITE_TEXT_ALIGNMENT_JUSTIFIED  3

// DWRITE_PARAGRAPH_ALIGNMENT.
#define DWRITE_PARAGRAPH_ALIGNMENT_NEAR  0
#define DWRITE_PARAGRAPH_ALIGNMENT_FAR   1
#define DWRITE_PARAGRAPH_ALIGNMENT_CENTER 2

// DWRITE_WORD_WRAPPING.
#define DWRITE_WORD_WRAPPING_WRAP        0
#define DWRITE_WORD_WRAPPING_NO_WRAP     1

// DWRITE_READING_DIRECTION.
#define DWRITE_READING_DIRECTION_LTR     0
#define DWRITE_READING_DIRECTION_RTL     1

// DWRITE_FLOW_DIRECTION.
#define DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM 0

// ============================================================================
//  Estruturas DirectWrite publicas (subset).
// ============================================================================
#pragma pack(push, 8)

// DWRITE_TEXT_METRICS — info de medida de um layout.
typedef struct DWRITE_TEXT_METRICS {
    FLOAT  left;
    FLOAT  top;
    FLOAT  width;
    FLOAT  widthIncludingTrailingWhitespace;
    FLOAT  height;
    FLOAT  layoutWidth;
    FLOAT  layoutHeight;
    UINT32 maxBidiReorderingDepth;
    UINT32 lineCount;
} DWRITE_TEXT_METRICS;

// DWRITE_FONT_METRICS — info de uma fonte (em design units).
typedef struct DWRITE_FONT_METRICS {
    UINT16 designUnitsPerEm;
    UINT16 ascent;
    UINT16 descent;
    INT16  lineGap;
    UINT16 capHeight;
    UINT16 xHeight;
    INT16  underlinePosition;
    UINT16 underlineThickness;
    INT16  strikethroughPosition;
    UINT16 strikethroughThickness;
} DWRITE_FONT_METRICS;

// DWRITE_LINE_METRICS.
typedef struct DWRITE_LINE_METRICS {
    UINT32 length;
    UINT32 trailingWhitespaceLength;
    UINT32 newlineLength;
    FLOAT  height;
    FLOAT  baseline;
    BOOL   isTrimmed;
} DWRITE_LINE_METRICS;

#pragma pack(pop)

// ============================================================================
//  Forward decls.
// ============================================================================
struct IDWriteFactoryImpl;
struct IDWriteTextFormatImpl;
struct IDWriteTextLayoutImpl;
struct IDWriteFontCollectionImpl;
struct IDWriteFontFamilyImpl;
struct IDWriteFontImpl;

struct IDWriteFactoryVtbl;
struct IDWriteTextFormatVtbl;
struct IDWriteTextLayoutVtbl;
struct IDWriteFontCollectionVtbl;
struct IDWriteFontFamilyVtbl;
struct IDWriteFontVtbl;

// ============================================================================
//  POOLS ESTATICOS.
// ============================================================================
#define MAX_FACTORIES         4
#define MAX_TEXT_FORMATS     16
#define MAX_TEXT_LAYOUTS     16
#define MAX_FONT_COLLECTIONS  4
#define MAX_FONT_FAMILIES    16
#define MAX_FONTS            16

// Comprimento maximo de strings (font family name etc.). Textos longos
// truncam silenciosamente em len-1 caracteres + nul.
#define MAX_STR_LEN          64

typedef struct IDWriteFontImpl {
    const struct IDWriteFontVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT weight;
    UINT style;
    UINT stretch;
} IDWriteFontImpl;

typedef struct IDWriteFontFamilyImpl {
    const struct IDWriteFontFamilyVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    WCHAR name[MAX_STR_LEN];
} IDWriteFontFamilyImpl;

typedef struct IDWriteFontCollectionImpl {
    const struct IDWriteFontCollectionVtbl* lpVtbl;
    LONG refCount;
    INT  used;
} IDWriteFontCollectionImpl;

typedef struct IDWriteTextFormatImpl {
    const struct IDWriteTextFormatVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    WCHAR fontFamily[MAX_STR_LEN];
    UINT  weight;
    UINT  style;
    UINT  stretch;
    FLOAT fontSize;
    UINT  textAlignment;
    UINT  paragraphAlignment;
    UINT  wordWrapping;
    UINT  readingDirection;
    UINT  flowDirection;
} IDWriteTextFormatImpl;

typedef struct IDWriteTextLayoutImpl {
    const struct IDWriteTextLayoutVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    WCHAR text[256];           // cache da string original (truncada se >255)
    UINT  textLen;
    FLOAT maxWidth;
    FLOAT maxHeight;
    IDWriteTextFormatImpl* format;
} IDWriteTextLayoutImpl;

typedef struct IDWriteFactoryImpl {
    const struct IDWriteFactoryVtbl* lpVtbl;
    LONG refCount;
    INT  used;
    UINT factoryType;
} IDWriteFactoryImpl;

static IDWriteFactoryImpl         g_factories       [MAX_FACTORIES];
static IDWriteTextFormatImpl      g_text_formats    [MAX_TEXT_FORMATS];
static IDWriteTextLayoutImpl      g_text_layouts    [MAX_TEXT_LAYOUTS];
static IDWriteFontCollectionImpl  g_font_collections[MAX_FONT_COLLECTIONS];
static IDWriteFontFamilyImpl      g_font_families   [MAX_FONT_FAMILIES];
static IDWriteFontImpl            g_fonts           [MAX_FONTS];

// ----------------------------------------------------------------------------
//  Utilitarios.
// ----------------------------------------------------------------------------
static void mem_zero(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

static void wstr_copy(WCHAR* dst, const WCHAR* src, UINT max) {
    if (!dst || !max) return;
    UINT i = 0;
    if (src) {
        while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    }
    dst[i] = 0;
}

static UINT wstr_len(const WCHAR* s) {
    UINT n = 0;
    if (s) while (s[n]) n++;
    return n;
}

// ascii -> WCHAR. Util pra "system" font collection sentinels.
static void ascii_to_wide(WCHAR* dst, const char* src, UINT max) {
    if (!dst || !max) return;
    UINT i = 0;
    if (src) while (src[i] && i < max - 1) { dst[i] = (WCHAR)(unsigned char)src[i]; i++; }
    dst[i] = 0;
}

// ============================================================================
//  Vtable: IDWriteFont. Representa uma face concreta (peso+estilo+stretch).
// ============================================================================
typedef struct IDWriteFontVtbl {
    HRESULT (*QueryInterface)(IDWriteFontImpl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDWriteFontImpl* This);
    ULONG   (*Release)       (IDWriteFontImpl* This);
    HRESULT (*GetFontFamily) (IDWriteFontImpl* This, IDWriteFontFamilyImpl** out);
    UINT    (*GetWeight)     (IDWriteFontImpl* This);
    UINT    (*GetStretch)    (IDWriteFontImpl* This);
    UINT    (*GetStyle)      (IDWriteFontImpl* This);
    BOOL    (*IsSymbolFont)  (IDWriteFontImpl* This);
    HRESULT (*GetFaceNames)  (IDWriteFontImpl* This, void** names);
    HRESULT (*GetInformationalStrings)(IDWriteFontImpl* This, UINT id,
                                        void** strings, BOOL* exists);
    UINT    (*GetSimulations)(IDWriteFontImpl* This);
    void    (*GetMetrics)    (IDWriteFontImpl* This, DWRITE_FONT_METRICS* m);
    HRESULT (*HasCharacter)  (IDWriteFontImpl* This, UINT32 ch, BOOL* exists);
    HRESULT (*CreateFontFace)(IDWriteFontImpl* This, void** out);
} IDWriteFontVtbl;

static HRESULT Fnt_QueryInterface(IDWriteFontImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Fnt_AddRef (IDWriteFontImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Fnt_Release(IDWriteFontImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Fnt_GetFontFamily(IDWriteFontImpl* T, IDWriteFontFamilyImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    *out = &g_font_families[0];      // todas pertencem a "Segoe UI" sentinel
    (*out)->refCount++;
    return S_OK;
}
static UINT Fnt_GetWeight (IDWriteFontImpl* T) { return T->weight; }
static UINT Fnt_GetStretch(IDWriteFontImpl* T) { return T->stretch; }
static UINT Fnt_GetStyle  (IDWriteFontImpl* T) { return T->style; }
static BOOL Fnt_IsSymbolFont(IDWriteFontImpl* T) { (void)T; return 0; }
static HRESULT Fnt_GetFaceNames(IDWriteFontImpl* T, void** n) { (void)T; if (n) *n = (void*)0x1; return S_OK; }
static HRESULT Fnt_GetInformationalStrings(IDWriteFontImpl* T, UINT id, void** s, BOOL* e) {
    (void)T; (void)id; if (s) *s = (void*)0x1; if (e) *e = 1; return S_OK;
}
static UINT Fnt_GetSimulations(IDWriteFontImpl* T) { (void)T; return 0; }
// Metrica em design units. Segoe UI tipica: 2048 EM, ascent 1577, descent 423.
static void Fnt_GetMetrics(IDWriteFontImpl* T, DWRITE_FONT_METRICS* m) {
    (void)T;
    if (!m) return;
    mem_zero(m, sizeof(*m));
    m->designUnitsPerEm     = 2048;
    m->ascent               = 1577;
    m->descent              = 423;
    m->lineGap              = 0;
    m->capHeight            = 1434;
    m->xHeight              = 1062;
    m->underlinePosition    = -200;
    m->underlineThickness   = 100;
    m->strikethroughPosition = 530;
    m->strikethroughThickness = 100;
}
static HRESULT Fnt_HasCharacter(IDWriteFontImpl* T, UINT32 ch, BOOL* e) {
    (void)T; (void)ch; if (e) *e = 1; return S_OK;
}
static HRESULT Fnt_CreateFontFace(IDWriteFontImpl* T, void** out) {
    (void)T; if (out) *out = (void*)0x1; return S_OK;
}

static const IDWriteFontVtbl g_fontVtbl = {
    Fnt_QueryInterface, Fnt_AddRef, Fnt_Release,
    Fnt_GetFontFamily, Fnt_GetWeight, Fnt_GetStretch, Fnt_GetStyle,
    Fnt_IsSymbolFont, Fnt_GetFaceNames, Fnt_GetInformationalStrings,
    Fnt_GetSimulations, Fnt_GetMetrics, Fnt_HasCharacter, Fnt_CreateFontFace,
};

static IDWriteFontImpl* alloc_font(UINT weight, UINT style, UINT stretch) {
    for (int i = 0; i < MAX_FONTS; i++) {
        if (!g_fonts[i].used) {
            mem_zero(&g_fonts[i], sizeof(g_fonts[i]));
            g_fonts[i].used     = 1;
            g_fonts[i].refCount = 1;
            g_fonts[i].lpVtbl   = &g_fontVtbl;
            g_fonts[i].weight   = weight  ? weight  : DWRITE_FONT_WEIGHT_NORMAL;
            g_fonts[i].style    = style;
            g_fonts[i].stretch  = stretch ? stretch : DWRITE_FONT_STRETCH_NORMAL;
            return &g_fonts[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: IDWriteFontFamily.
// ============================================================================
typedef struct IDWriteFontFamilyVtbl {
    HRESULT (*QueryInterface)(IDWriteFontFamilyImpl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDWriteFontFamilyImpl* This);
    ULONG   (*Release)       (IDWriteFontFamilyImpl* This);
    HRESULT (*GetFontCollection)(IDWriteFontFamilyImpl* This, IDWriteFontCollectionImpl** out);
    UINT    (*GetFontCount) (IDWriteFontFamilyImpl* This);
    HRESULT (*GetFont)      (IDWriteFontFamilyImpl* This, UINT idx, IDWriteFontImpl** out);
    HRESULT (*GetFamilyNames)(IDWriteFontFamilyImpl* This, void** names);
    HRESULT (*GetFirstMatchingFont)(IDWriteFontFamilyImpl* This, UINT w, UINT s, UINT st,
                                     IDWriteFontImpl** out);
    HRESULT (*GetMatchingFonts)(IDWriteFontFamilyImpl* This, UINT w, UINT s, UINT st,
                                 void** out);
} IDWriteFontFamilyVtbl;

static HRESULT Fam_QueryInterface(IDWriteFontFamilyImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Fam_AddRef (IDWriteFontFamilyImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Fam_Release(IDWriteFontFamilyImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Fam_GetFontCollection(IDWriteFontFamilyImpl* T, IDWriteFontCollectionImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    *out = &g_font_collections[0];
    (*out)->refCount++;
    return S_OK;
}
static UINT Fam_GetFontCount(IDWriteFontFamilyImpl* T) { (void)T; return 4; }   // Regular/Bold/Italic/BoldItalic
static HRESULT Fam_GetFont(IDWriteFontFamilyImpl* T, UINT idx, IDWriteFontImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    UINT weight = (idx & 1) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    UINT style  = (idx & 2) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    IDWriteFontImpl* f = alloc_font(weight, style, DWRITE_FONT_STRETCH_NORMAL);
    if (!f) { *out = 0; return E_OUTOFMEMORY; }
    *out = f;
    return S_OK;
}
static HRESULT Fam_GetFamilyNames(IDWriteFontFamilyImpl* T, void** n) {
    (void)T; if (n) *n = (void*)0x1; return S_OK;
}
static HRESULT Fam_GetFirstMatchingFont(IDWriteFontFamilyImpl* T, UINT w, UINT s, UINT st,
                                          IDWriteFontImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    IDWriteFontImpl* f = alloc_font(w, s, st);
    if (!f) { *out = 0; return E_OUTOFMEMORY; }
    *out = f;
    return S_OK;
}
static HRESULT Fam_GetMatchingFonts(IDWriteFontFamilyImpl* T, UINT w, UINT s, UINT st, void** out) {
    (void)T; (void)w; (void)s; (void)st;
    if (out) *out = (void*)0x1;
    return S_OK;
}

static const IDWriteFontFamilyVtbl g_familyVtbl = {
    Fam_QueryInterface, Fam_AddRef, Fam_Release,
    Fam_GetFontCollection, Fam_GetFontCount, Fam_GetFont,
    Fam_GetFamilyNames, Fam_GetFirstMatchingFont, Fam_GetMatchingFonts,
};

static IDWriteFontFamilyImpl* alloc_family(const WCHAR* name) {
    for (int i = 0; i < MAX_FONT_FAMILIES; i++) {
        if (!g_font_families[i].used) {
            mem_zero(&g_font_families[i], sizeof(g_font_families[i]));
            g_font_families[i].used     = 1;
            g_font_families[i].refCount = 1;
            g_font_families[i].lpVtbl   = &g_familyVtbl;
            if (name) wstr_copy(g_font_families[i].name, name, MAX_STR_LEN);
            else      ascii_to_wide(g_font_families[i].name, "Segoe UI", MAX_STR_LEN);
            return &g_font_families[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: IDWriteFontCollection.
// ============================================================================
typedef struct IDWriteFontCollectionVtbl {
    HRESULT (*QueryInterface)(IDWriteFontCollectionImpl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDWriteFontCollectionImpl* This);
    ULONG   (*Release)       (IDWriteFontCollectionImpl* This);
    UINT    (*GetFontFamilyCount)(IDWriteFontCollectionImpl* This);
    HRESULT (*GetFontFamily) (IDWriteFontCollectionImpl* This, UINT idx,
                              IDWriteFontFamilyImpl** out);
    HRESULT (*FindFamilyName)(IDWriteFontCollectionImpl* This, const WCHAR* name,
                              UINT* idx, BOOL* exists);
    HRESULT (*GetFontFromFontFace)(IDWriteFontCollectionImpl* This, void* face,
                                    IDWriteFontImpl** out);
} IDWriteFontCollectionVtbl;

static HRESULT Coll_QueryInterface(IDWriteFontCollectionImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Coll_AddRef (IDWriteFontCollectionImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Coll_Release(IDWriteFontCollectionImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
// "Sistema" tem 1 familia (Segoe UI sentinel). Apps que iteram param em 1.
static UINT Coll_GetFontFamilyCount(IDWriteFontCollectionImpl* T) { (void)T; return 1; }
static HRESULT Coll_GetFontFamily(IDWriteFontCollectionImpl* T, UINT idx,
                                    IDWriteFontFamilyImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    if (idx > 0) { *out = 0; return E_INVALIDARG; }
    IDWriteFontFamilyImpl* f = alloc_family(0);
    if (!f) { *out = 0; return E_OUTOFMEMORY; }
    *out = f;
    return S_OK;
}
static HRESULT Coll_FindFamilyName(IDWriteFontCollectionImpl* T, const WCHAR* name,
                                     UINT* idx, BOOL* exists) {
    (void)T; (void)name;
    if (idx) *idx = 0;
    if (exists) *exists = 1;       // ja que so temos 1, fingimos sempre achar
    return S_OK;
}
static HRESULT Coll_GetFontFromFontFace(IDWriteFontCollectionImpl* T, void* face,
                                         IDWriteFontImpl** out) {
    (void)T; (void)face;
    if (!out) return E_POINTER;
    IDWriteFontImpl* f = alloc_font(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL);
    if (!f) { *out = 0; return E_OUTOFMEMORY; }
    *out = f;
    return S_OK;
}

static const IDWriteFontCollectionVtbl g_collectionVtbl = {
    Coll_QueryInterface, Coll_AddRef, Coll_Release,
    Coll_GetFontFamilyCount, Coll_GetFontFamily,
    Coll_FindFamilyName, Coll_GetFontFromFontFace,
};

static IDWriteFontCollectionImpl* alloc_collection(void) {
    for (int i = 0; i < MAX_FONT_COLLECTIONS; i++) {
        if (!g_font_collections[i].used) {
            mem_zero(&g_font_collections[i], sizeof(g_font_collections[i]));
            g_font_collections[i].used     = 1;
            g_font_collections[i].refCount = 1;
            g_font_collections[i].lpVtbl   = &g_collectionVtbl;
            return &g_font_collections[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: IDWriteTextFormat. Holds default formatting (font, size, alignment).
// ============================================================================
typedef struct IDWriteTextFormatVtbl {
    HRESULT (*QueryInterface)(IDWriteTextFormatImpl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDWriteTextFormatImpl* This);
    ULONG   (*Release)       (IDWriteTextFormatImpl* This);
    HRESULT (*SetTextAlignment)     (IDWriteTextFormatImpl* This, UINT a);
    HRESULT (*SetParagraphAlignment)(IDWriteTextFormatImpl* This, UINT a);
    HRESULT (*SetWordWrapping)      (IDWriteTextFormatImpl* This, UINT w);
    HRESULT (*SetReadingDirection)  (IDWriteTextFormatImpl* This, UINT d);
    HRESULT (*SetFlowDirection)     (IDWriteTextFormatImpl* This, UINT d);
    HRESULT (*SetIncrementalTabStop)(IDWriteTextFormatImpl* This, FLOAT t);
    HRESULT (*SetTrimming)          (IDWriteTextFormatImpl* This, const void* tr, void* sign);
    HRESULT (*SetLineSpacing)       (IDWriteTextFormatImpl* This, UINT method, FLOAT h, FLOAT b);
    UINT    (*GetTextAlignment)     (IDWriteTextFormatImpl* This);
    UINT    (*GetParagraphAlignment)(IDWriteTextFormatImpl* This);
    UINT    (*GetWordWrapping)      (IDWriteTextFormatImpl* This);
    UINT    (*GetReadingDirection)  (IDWriteTextFormatImpl* This);
    UINT    (*GetFlowDirection)     (IDWriteTextFormatImpl* This);
    FLOAT   (*GetIncrementalTabStop)(IDWriteTextFormatImpl* This);
    HRESULT (*GetTrimming)          (IDWriteTextFormatImpl* This, void* tr, void** sign);
    HRESULT (*GetLineSpacing)       (IDWriteTextFormatImpl* This, UINT* m, FLOAT* h, FLOAT* b);
    HRESULT (*GetFontCollection)    (IDWriteTextFormatImpl* This, IDWriteFontCollectionImpl** out);
    UINT    (*GetFontFamilyNameLength)(IDWriteTextFormatImpl* This);
    HRESULT (*GetFontFamilyName)    (IDWriteTextFormatImpl* This, WCHAR* name, UINT len);
    UINT    (*GetFontWeight)        (IDWriteTextFormatImpl* This);
    UINT    (*GetFontStyle)         (IDWriteTextFormatImpl* This);
    UINT    (*GetFontStretch)       (IDWriteTextFormatImpl* This);
    FLOAT   (*GetFontSize)          (IDWriteTextFormatImpl* This);
    UINT    (*GetLocaleNameLength)  (IDWriteTextFormatImpl* This);
    HRESULT (*GetLocaleName)        (IDWriteTextFormatImpl* This, WCHAR* name, UINT len);
} IDWriteTextFormatVtbl;

static HRESULT Tf_QueryInterface(IDWriteTextFormatImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Tf_AddRef (IDWriteTextFormatImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Tf_Release(IDWriteTextFormatImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Tf_SetTextAlignment     (IDWriteTextFormatImpl* T, UINT a) { T->textAlignment = a; return S_OK; }
static HRESULT Tf_SetParagraphAlignment(IDWriteTextFormatImpl* T, UINT a) { T->paragraphAlignment = a; return S_OK; }
static HRESULT Tf_SetWordWrapping      (IDWriteTextFormatImpl* T, UINT w) { T->wordWrapping = w; return S_OK; }
static HRESULT Tf_SetReadingDirection  (IDWriteTextFormatImpl* T, UINT d) { T->readingDirection = d; return S_OK; }
static HRESULT Tf_SetFlowDirection     (IDWriteTextFormatImpl* T, UINT d) { T->flowDirection = d; return S_OK; }
static HRESULT Tf_SetIncrementalTabStop(IDWriteTextFormatImpl* T, FLOAT t) { (void)T; (void)t; return S_OK; }
static HRESULT Tf_SetTrimming(IDWriteTextFormatImpl* T, const void* tr, void* sign) {
    (void)T; (void)tr; (void)sign; return S_OK;
}
static HRESULT Tf_SetLineSpacing(IDWriteTextFormatImpl* T, UINT m, FLOAT h, FLOAT b) {
    (void)T; (void)m; (void)h; (void)b; return S_OK;
}
static UINT Tf_GetTextAlignment     (IDWriteTextFormatImpl* T) { return T->textAlignment; }
static UINT Tf_GetParagraphAlignment(IDWriteTextFormatImpl* T) { return T->paragraphAlignment; }
static UINT Tf_GetWordWrapping      (IDWriteTextFormatImpl* T) { return T->wordWrapping; }
static UINT Tf_GetReadingDirection  (IDWriteTextFormatImpl* T) { return T->readingDirection; }
static UINT Tf_GetFlowDirection     (IDWriteTextFormatImpl* T) { return T->flowDirection; }
static FLOAT Tf_GetIncrementalTabStop(IDWriteTextFormatImpl* T) { (void)T; return 0.0f; }
static HRESULT Tf_GetTrimming(IDWriteTextFormatImpl* T, void* tr, void** s) {
    (void)T; (void)tr; if (s) *s = 0; return S_OK;
}
static HRESULT Tf_GetLineSpacing(IDWriteTextFormatImpl* T, UINT* m, FLOAT* h, FLOAT* b) {
    (void)T; if (m) *m = 0; if (h) *h = 0; if (b) *b = 0; return S_OK;
}
static HRESULT Tf_GetFontCollection(IDWriteTextFormatImpl* T, IDWriteFontCollectionImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    *out = &g_font_collections[0];
    (*out)->refCount++;
    return S_OK;
}
static UINT Tf_GetFontFamilyNameLength(IDWriteTextFormatImpl* T) { return wstr_len(T->fontFamily); }
static HRESULT Tf_GetFontFamilyName(IDWriteTextFormatImpl* T, WCHAR* name, UINT len) {
    if (!name) return E_POINTER;
    wstr_copy(name, T->fontFamily, len);
    return S_OK;
}
static UINT Tf_GetFontWeight (IDWriteTextFormatImpl* T) { return T->weight; }
static UINT Tf_GetFontStyle  (IDWriteTextFormatImpl* T) { return T->style; }
static UINT Tf_GetFontStretch(IDWriteTextFormatImpl* T) { return T->stretch; }
static FLOAT Tf_GetFontSize  (IDWriteTextFormatImpl* T) { return T->fontSize; }
static UINT Tf_GetLocaleNameLength(IDWriteTextFormatImpl* T) { (void)T; return 5; }  // "en-US"
static HRESULT Tf_GetLocaleName(IDWriteTextFormatImpl* T, WCHAR* name, UINT len) {
    (void)T;
    if (!name) return E_POINTER;
    ascii_to_wide(name, "en-US", len);
    return S_OK;
}

static const IDWriteTextFormatVtbl g_textFormatVtbl = {
    Tf_QueryInterface, Tf_AddRef, Tf_Release,
    Tf_SetTextAlignment, Tf_SetParagraphAlignment, Tf_SetWordWrapping,
    Tf_SetReadingDirection, Tf_SetFlowDirection, Tf_SetIncrementalTabStop,
    Tf_SetTrimming, Tf_SetLineSpacing,
    Tf_GetTextAlignment, Tf_GetParagraphAlignment, Tf_GetWordWrapping,
    Tf_GetReadingDirection, Tf_GetFlowDirection, Tf_GetIncrementalTabStop,
    Tf_GetTrimming, Tf_GetLineSpacing, Tf_GetFontCollection,
    Tf_GetFontFamilyNameLength, Tf_GetFontFamilyName,
    Tf_GetFontWeight, Tf_GetFontStyle, Tf_GetFontStretch, Tf_GetFontSize,
    Tf_GetLocaleNameLength, Tf_GetLocaleName,
};

static IDWriteTextFormatImpl* alloc_format(const WCHAR* family, UINT weight, UINT style,
                                            UINT stretch, FLOAT size) {
    for (int i = 0; i < MAX_TEXT_FORMATS; i++) {
        if (!g_text_formats[i].used) {
            mem_zero(&g_text_formats[i], sizeof(g_text_formats[i]));
            g_text_formats[i].used     = 1;
            g_text_formats[i].refCount = 1;
            g_text_formats[i].lpVtbl   = &g_textFormatVtbl;
            if (family) wstr_copy(g_text_formats[i].fontFamily, family, MAX_STR_LEN);
            else        ascii_to_wide(g_text_formats[i].fontFamily, "Segoe UI", MAX_STR_LEN);
            g_text_formats[i].weight   = weight  ? weight  : DWRITE_FONT_WEIGHT_NORMAL;
            g_text_formats[i].style    = style;
            g_text_formats[i].stretch  = stretch ? stretch : DWRITE_FONT_STRETCH_NORMAL;
            g_text_formats[i].fontSize = size    ? size    : 12.0f;
            g_text_formats[i].textAlignment      = DWRITE_TEXT_ALIGNMENT_LEADING;
            g_text_formats[i].paragraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            g_text_formats[i].wordWrapping       = DWRITE_WORD_WRAPPING_WRAP;
            g_text_formats[i].readingDirection   = DWRITE_READING_DIRECTION_LTR;
            g_text_formats[i].flowDirection      = DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM;
            return &g_text_formats[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: IDWriteTextLayout. Texto + format + caixa (maxWidth/Height).
//  Draw() chama o IDWriteTextRenderer do app — aqui e no-op (sem rasterizer).
// ============================================================================
typedef struct IDWriteTextLayoutVtbl {
    HRESULT (*QueryInterface)(IDWriteTextLayoutImpl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDWriteTextLayoutImpl* This);
    ULONG   (*Release)       (IDWriteTextLayoutImpl* This);
    // herda metodos de IDWriteTextFormat (interface base). Aqui aplicamos a
    // mesma vtable do format mas com slots adicionais; para simplicidade,
    // pulamos a heranca e expomos so os essenciais.
    HRESULT (*SetMaxWidth)   (IDWriteTextLayoutImpl* This, FLOAT w);
    HRESULT (*SetMaxHeight)  (IDWriteTextLayoutImpl* This, FLOAT h);
    FLOAT   (*GetMaxWidth)   (IDWriteTextLayoutImpl* This);
    FLOAT   (*GetMaxHeight)  (IDWriteTextLayoutImpl* This);
    HRESULT (*GetMetrics)    (IDWriteTextLayoutImpl* This, DWRITE_TEXT_METRICS* m);
    HRESULT (*GetLineMetrics)(IDWriteTextLayoutImpl* This, DWRITE_LINE_METRICS* lines,
                                UINT maxLines, UINT* actualCount);
    HRESULT (*HitTestPoint)  (IDWriteTextLayoutImpl* This, FLOAT x, FLOAT y,
                                BOOL* trailing, BOOL* inside, void* hit);
    HRESULT (*HitTestTextPosition)(IDWriteTextLayoutImpl* This, UINT pos, BOOL trailing,
                                     FLOAT* x, FLOAT* y, void* hit);
    HRESULT (*Draw)          (IDWriteTextLayoutImpl* This, void* ctx, void* renderer,
                                FLOAT x, FLOAT y);
    HRESULT (*SetFontSize)   (IDWriteTextLayoutImpl* This, FLOAT s, void* range);
    HRESULT (*SetFontWeight) (IDWriteTextLayoutImpl* This, UINT w, void* range);
    HRESULT (*SetFontStyle)  (IDWriteTextLayoutImpl* This, UINT s, void* range);
    HRESULT (*SetFontFamilyName)(IDWriteTextLayoutImpl* This, const WCHAR* n, void* range);
    HRESULT (*SetUnderline)  (IDWriteTextLayoutImpl* This, BOOL b, void* range);
    HRESULT (*SetStrikethrough)(IDWriteTextLayoutImpl* This, BOOL b, void* range);
    HRESULT (*SetDrawingEffect)(IDWriteTextLayoutImpl* This, IUnknown* fx, void* range);
} IDWriteTextLayoutVtbl;

static HRESULT Tl_QueryInterface(IDWriteTextLayoutImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Tl_AddRef (IDWriteTextLayoutImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Tl_Release(IDWriteTextLayoutImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Tl_SetMaxWidth (IDWriteTextLayoutImpl* T, FLOAT w) { T->maxWidth = w; return S_OK; }
static HRESULT Tl_SetMaxHeight(IDWriteTextLayoutImpl* T, FLOAT h) { T->maxHeight = h; return S_OK; }
static FLOAT Tl_GetMaxWidth (IDWriteTextLayoutImpl* T) { return T->maxWidth; }
static FLOAT Tl_GetMaxHeight(IDWriteTextLayoutImpl* T) { return T->maxHeight; }
// Estimativa simples de metricas — largura = textLen * size/2; altura = size*1.2.
static HRESULT Tl_GetMetrics(IDWriteTextLayoutImpl* T, DWRITE_TEXT_METRICS* m) {
    if (!m) return E_POINTER;
    mem_zero(m, sizeof(*m));
    FLOAT size = T->format ? T->format->fontSize : 12.0f;
    m->width  = (FLOAT)T->textLen * (size * 0.5f);
    m->height = size * 1.2f;
    if (m->width  > T->maxWidth  && T->maxWidth  > 0) m->width  = T->maxWidth;
    if (m->height > T->maxHeight && T->maxHeight > 0) m->height = T->maxHeight;
    m->widthIncludingTrailingWhitespace = m->width;
    m->layoutWidth  = T->maxWidth;
    m->layoutHeight = T->maxHeight;
    m->lineCount    = 1;
    m->maxBidiReorderingDepth = 1;
    return S_OK;
}
static HRESULT Tl_GetLineMetrics(IDWriteTextLayoutImpl* T, DWRITE_LINE_METRICS* l,
                                   UINT max, UINT* actual) {
    if (actual) *actual = 1;
    if (l && max > 0) {
        mem_zero(&l[0], sizeof(l[0]));
        l[0].length   = T->textLen;
        l[0].height   = T->format ? T->format->fontSize * 1.2f : 14.4f;
        l[0].baseline = T->format ? T->format->fontSize * 0.8f : 9.6f;
    }
    return S_OK;
}
static HRESULT Tl_HitTestPoint(IDWriteTextLayoutImpl* T, FLOAT x, FLOAT y, BOOL* tr, BOOL* in, void* h) {
    (void)T; (void)x; (void)y; (void)h;
    if (tr) *tr = 0; if (in) *in = 0;
    return S_OK;
}
static HRESULT Tl_HitTestTextPosition(IDWriteTextLayoutImpl* T, UINT pos, BOOL tr,
                                        FLOAT* x, FLOAT* y, void* h) {
    (void)T; (void)pos; (void)tr; (void)h;
    if (x) *x = 0; if (y) *y = 0;
    return S_OK;
}
static HRESULT Tl_Draw(IDWriteTextLayoutImpl* T, void* ctx, void* rnd, FLOAT x, FLOAT y) {
    (void)T; (void)ctx; (void)rnd; (void)x; (void)y;
    // no-op — sem rasterizer; o backend grafico real e win32k/FB
    return S_OK;
}
static HRESULT Tl_SetFontSize(IDWriteTextLayoutImpl* T, FLOAT s, void* range) {
    (void)range; if (T->format) T->format->fontSize = s; return S_OK;
}
static HRESULT Tl_SetFontWeight(IDWriteTextLayoutImpl* T, UINT w, void* range) {
    (void)range; if (T->format) T->format->weight = w; return S_OK;
}
static HRESULT Tl_SetFontStyle(IDWriteTextLayoutImpl* T, UINT s, void* range) {
    (void)range; if (T->format) T->format->style = s; return S_OK;
}
static HRESULT Tl_SetFontFamilyName(IDWriteTextLayoutImpl* T, const WCHAR* n, void* range) {
    (void)range; if (T->format && n) wstr_copy(T->format->fontFamily, n, MAX_STR_LEN); return S_OK;
}
static HRESULT Tl_SetUnderline(IDWriteTextLayoutImpl* T, BOOL b, void* r) { (void)T; (void)b; (void)r; return S_OK; }
static HRESULT Tl_SetStrikethrough(IDWriteTextLayoutImpl* T, BOOL b, void* r) { (void)T; (void)b; (void)r; return S_OK; }
static HRESULT Tl_SetDrawingEffect(IDWriteTextLayoutImpl* T, IUnknown* f, void* r) { (void)T; (void)f; (void)r; return S_OK; }

static const IDWriteTextLayoutVtbl g_textLayoutVtbl = {
    Tl_QueryInterface, Tl_AddRef, Tl_Release,
    Tl_SetMaxWidth, Tl_SetMaxHeight, Tl_GetMaxWidth, Tl_GetMaxHeight,
    Tl_GetMetrics, Tl_GetLineMetrics,
    Tl_HitTestPoint, Tl_HitTestTextPosition,
    Tl_Draw,
    Tl_SetFontSize, Tl_SetFontWeight, Tl_SetFontStyle, Tl_SetFontFamilyName,
    Tl_SetUnderline, Tl_SetStrikethrough, Tl_SetDrawingEffect,
};

static IDWriteTextLayoutImpl* alloc_layout(const WCHAR* text, UINT len,
                                            IDWriteTextFormatImpl* fmt,
                                            FLOAT maxW, FLOAT maxH) {
    for (int i = 0; i < MAX_TEXT_LAYOUTS; i++) {
        if (!g_text_layouts[i].used) {
            mem_zero(&g_text_layouts[i], sizeof(g_text_layouts[i]));
            g_text_layouts[i].used     = 1;
            g_text_layouts[i].refCount = 1;
            g_text_layouts[i].lpVtbl   = &g_textLayoutVtbl;
            // Copia texto truncado para nosso buffer interno.
            UINT n = (len > 255) ? 255 : len;
            if (text) for (UINT k = 0; k < n; k++) g_text_layouts[i].text[k] = text[k];
            g_text_layouts[i].text[n] = 0;
            g_text_layouts[i].textLen = n;
            g_text_layouts[i].format  = fmt;
            g_text_layouts[i].maxWidth  = maxW;
            g_text_layouts[i].maxHeight = maxH;
            return &g_text_layouts[i];
        }
    }
    return 0;
}

// ============================================================================
//  Vtable: IDWriteFactory. Cria text formats, layouts, font collections.
// ============================================================================
typedef struct IDWriteFactoryVtbl {
    HRESULT (*QueryInterface)(IDWriteFactoryImpl* This, REFIID r, void** ppv);
    ULONG   (*AddRef)        (IDWriteFactoryImpl* This);
    ULONG   (*Release)       (IDWriteFactoryImpl* This);
    HRESULT (*GetSystemFontCollection)(IDWriteFactoryImpl* This,
                                          IDWriteFontCollectionImpl** out,
                                          BOOL checkForUpdates);
    HRESULT (*CreateCustomFontCollection)(IDWriteFactoryImpl* This, void* loader,
                                            const void* key, UINT keySize,
                                            IDWriteFontCollectionImpl** out);
    HRESULT (*RegisterFontCollectionLoader)(IDWriteFactoryImpl* This, void* loader);
    HRESULT (*UnregisterFontCollectionLoader)(IDWriteFactoryImpl* This, void* loader);
    HRESULT (*CreateFontFileReference)(IDWriteFactoryImpl* This, const WCHAR* path,
                                         const void* lastWrite, void** out);
    HRESULT (*CreateCustomFontFileReference)(IDWriteFactoryImpl* This, const void* key,
                                                UINT keySize, void* loader, void** out);
    HRESULT (*CreateFontFace)(IDWriteFactoryImpl* This, UINT type, UINT numFiles,
                                void* const* files, UINT idx, UINT sim, void** out);
    HRESULT (*CreateRenderingParams)(IDWriteFactoryImpl* This, void** out);
    HRESULT (*CreateMonitorRenderingParams)(IDWriteFactoryImpl* This, void* mon, void** out);
    HRESULT (*CreateCustomRenderingParams)(IDWriteFactoryImpl* This, FLOAT gamma,
                                             FLOAT enhancedContrast, FLOAT clear,
                                             UINT pixelGeo, UINT mode, void** out);
    HRESULT (*RegisterFontFileLoader)(IDWriteFactoryImpl* This, void* loader);
    HRESULT (*UnregisterFontFileLoader)(IDWriteFactoryImpl* This, void* loader);
    HRESULT (*CreateTextFormat)(IDWriteFactoryImpl* This, const WCHAR* family,
                                  IDWriteFontCollectionImpl* coll, UINT weight,
                                  UINT style, UINT stretch, FLOAT size,
                                  const WCHAR* locale, IDWriteTextFormatImpl** out);
    HRESULT (*CreateTypography)(IDWriteFactoryImpl* This, void** out);
    HRESULT (*GetGdiInterop)   (IDWriteFactoryImpl* This, void** out);
    HRESULT (*CreateTextLayout)(IDWriteFactoryImpl* This, const WCHAR* text, UINT len,
                                  IDWriteTextFormatImpl* fmt, FLOAT maxW, FLOAT maxH,
                                  IDWriteTextLayoutImpl** out);
    HRESULT (*CreateGdiCompatibleTextLayout)(IDWriteFactoryImpl* This, const WCHAR* text,
                                                UINT len, IDWriteTextFormatImpl* fmt,
                                                FLOAT layoutW, FLOAT layoutH, FLOAT pixelsPerDip,
                                                const void* transform, BOOL useGdi,
                                                IDWriteTextLayoutImpl** out);
    HRESULT (*CreateEllipsisTrimmingSign)(IDWriteFactoryImpl* This, IDWriteTextFormatImpl* fmt,
                                            void** out);
    HRESULT (*CreateTextAnalyzer)(IDWriteFactoryImpl* This, void** out);
    HRESULT (*CreateNumberSubstitution)(IDWriteFactoryImpl* This, UINT method,
                                          const WCHAR* locale, BOOL ignoreUserOverride,
                                          void** out);
    HRESULT (*CreateGlyphRunAnalysis)(IDWriteFactoryImpl* This, const void* glyphRun,
                                       FLOAT pixelsPerDip, const void* transform,
                                       UINT mode, UINT measuring, FLOAT x, FLOAT y,
                                       void** out);
} IDWriteFactoryVtbl;

static HRESULT Fy_QueryInterface(IDWriteFactoryImpl* T, REFIID r, void** ppv) {
    (void)r; if (!ppv) return E_POINTER; *ppv = T; T->refCount++; return S_OK;
}
static ULONG Fy_AddRef (IDWriteFactoryImpl* T) { return (ULONG)(++T->refCount); }
static ULONG Fy_Release(IDWriteFactoryImpl* T) {
    LONG n = --T->refCount;
    if (n <= 0) { T->used = 0; T->refCount = 0; return 0; }
    return (ULONG)n;
}
static HRESULT Fy_GetSystemFontCollection(IDWriteFactoryImpl* T,
                                            IDWriteFontCollectionImpl** out, BOOL chk) {
    (void)T; (void)chk;
    if (!out) return E_POINTER;
    if (!g_font_collections[0].used) {
        IDWriteFontCollectionImpl* c = alloc_collection();
        if (!c) { *out = 0; return E_OUTOFMEMORY; }
    } else {
        g_font_collections[0].refCount++;
    }
    *out = &g_font_collections[0];
    return S_OK;
}
static HRESULT Fy_CreateCustomFontCollection(IDWriteFactoryImpl* T, void* l, const void* k,
                                               UINT ks, IDWriteFontCollectionImpl** out) {
    (void)T; (void)l; (void)k; (void)ks;
    if (!out) return E_POINTER;
    IDWriteFontCollectionImpl* c = alloc_collection();
    if (!c) { *out = 0; return E_OUTOFMEMORY; }
    *out = c;
    return S_OK;
}
static HRESULT Fy_RegisterFontCollectionLoader(IDWriteFactoryImpl* T, void* l) { (void)T; (void)l; return S_OK; }
static HRESULT Fy_UnregisterFontCollectionLoader(IDWriteFactoryImpl* T, void* l) { (void)T; (void)l; return S_OK; }
static HRESULT Fy_CreateFontFileReference(IDWriteFactoryImpl* T, const WCHAR* p, const void* lw, void** out) {
    (void)T; (void)p; (void)lw; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_CreateCustomFontFileReference(IDWriteFactoryImpl* T, const void* k, UINT ks, void* l, void** out) {
    (void)T; (void)k; (void)ks; (void)l; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_CreateFontFace(IDWriteFactoryImpl* T, UINT type, UINT n, void* const* f,
                                   UINT i, UINT s, void** out) {
    (void)T; (void)type; (void)n; (void)f; (void)i; (void)s;
    if (out) *out = (void*)0x1;
    return S_OK;
}
static HRESULT Fy_CreateRenderingParams(IDWriteFactoryImpl* T, void** out) {
    (void)T; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_CreateMonitorRenderingParams(IDWriteFactoryImpl* T, void* m, void** out) {
    (void)T; (void)m; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_CreateCustomRenderingParams(IDWriteFactoryImpl* T, FLOAT g, FLOAT ec,
                                               FLOAT cl, UINT pg, UINT md, void** out) {
    (void)T; (void)g; (void)ec; (void)cl; (void)pg; (void)md;
    if (out) *out = (void*)0x1;
    return S_OK;
}
static HRESULT Fy_RegisterFontFileLoader(IDWriteFactoryImpl* T, void* l) { (void)T; (void)l; return S_OK; }
static HRESULT Fy_UnregisterFontFileLoader(IDWriteFactoryImpl* T, void* l) { (void)T; (void)l; return S_OK; }

static HRESULT Fy_CreateTextFormat(IDWriteFactoryImpl* T, const WCHAR* family,
                                     IDWriteFontCollectionImpl* coll, UINT weight,
                                     UINT style, UINT stretch, FLOAT size,
                                     const WCHAR* locale, IDWriteTextFormatImpl** out) {
    (void)T; (void)coll; (void)locale;
    if (!out) return E_POINTER;
    IDWriteTextFormatImpl* f = alloc_format(family, weight, style, stretch, size);
    if (!f) { *out = 0; return E_OUTOFMEMORY; }
    *out = f;
    return S_OK;
}

static HRESULT Fy_CreateTypography(IDWriteFactoryImpl* T, void** out) {
    (void)T; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_GetGdiInterop(IDWriteFactoryImpl* T, void** out) {
    (void)T; if (out) *out = (void*)0x1; return S_OK;
}

static HRESULT Fy_CreateTextLayout(IDWriteFactoryImpl* T, const WCHAR* text, UINT len,
                                     IDWriteTextFormatImpl* fmt, FLOAT mw, FLOAT mh,
                                     IDWriteTextLayoutImpl** out) {
    (void)T;
    if (!out) return E_POINTER;
    IDWriteTextLayoutImpl* l = alloc_layout(text, len, fmt, mw, mh);
    if (!l) { *out = 0; return E_OUTOFMEMORY; }
    *out = l;
    return S_OK;
}
static HRESULT Fy_CreateGdiCompatibleTextLayout(IDWriteFactoryImpl* T, const WCHAR* text,
                                                  UINT len, IDWriteTextFormatImpl* fmt,
                                                  FLOAT lw, FLOAT lh, FLOAT ppd,
                                                  const void* tr, BOOL gdi,
                                                  IDWriteTextLayoutImpl** out) {
    (void)T; (void)ppd; (void)tr; (void)gdi;
    return Fy_CreateTextLayout(T, text, len, fmt, lw, lh, out);
}
static HRESULT Fy_CreateEllipsisTrimmingSign(IDWriteFactoryImpl* T, IDWriteTextFormatImpl* f, void** out) {
    (void)T; (void)f; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_CreateTextAnalyzer(IDWriteFactoryImpl* T, void** out) {
    (void)T; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_CreateNumberSubstitution(IDWriteFactoryImpl* T, UINT m, const WCHAR* l,
                                              BOOL ig, void** out) {
    (void)T; (void)m; (void)l; (void)ig; if (out) *out = (void*)0x1; return S_OK;
}
static HRESULT Fy_CreateGlyphRunAnalysis(IDWriteFactoryImpl* T, const void* gr, FLOAT ppd,
                                            const void* tr, UINT md, UINT me, FLOAT x, FLOAT y,
                                            void** out) {
    (void)T; (void)gr; (void)ppd; (void)tr; (void)md; (void)me; (void)x; (void)y;
    if (out) *out = (void*)0x1;
    return S_OK;
}

static const IDWriteFactoryVtbl g_factoryVtbl = {
    Fy_QueryInterface, Fy_AddRef, Fy_Release,
    Fy_GetSystemFontCollection, Fy_CreateCustomFontCollection,
    Fy_RegisterFontCollectionLoader, Fy_UnregisterFontCollectionLoader,
    Fy_CreateFontFileReference, Fy_CreateCustomFontFileReference,
    Fy_CreateFontFace,
    Fy_CreateRenderingParams, Fy_CreateMonitorRenderingParams,
    Fy_CreateCustomRenderingParams,
    Fy_RegisterFontFileLoader, Fy_UnregisterFontFileLoader,
    Fy_CreateTextFormat, Fy_CreateTypography, Fy_GetGdiInterop,
    Fy_CreateTextLayout, Fy_CreateGdiCompatibleTextLayout,
    Fy_CreateEllipsisTrimmingSign, Fy_CreateTextAnalyzer,
    Fy_CreateNumberSubstitution, Fy_CreateGlyphRunAnalysis,
};

static IDWriteFactoryImpl* alloc_factory(UINT type) {
    for (int i = 0; i < MAX_FACTORIES; i++) {
        if (!g_factories[i].used) {
            mem_zero(&g_factories[i], sizeof(g_factories[i]));
            g_factories[i].used        = 1;
            g_factories[i].refCount    = 1;
            g_factories[i].lpVtbl      = &g_factoryVtbl;
            g_factories[i].factoryType = type;
            return &g_factories[i];
        }
    }
    return 0;
}

// ============================================================================
//  Entry point exportado — assinatura BATE com dwrite.dll real.
//  DWriteCreateFactory(DWRITE_FACTORY_TYPE, REFIID, IUnknown**)
// ============================================================================

__declspec(dllexport) HRESULT DWriteCreateFactory(UINT factoryType, REFIID riid,
                                                    void** ppFactory) {
    (void)riid;
    if (!ppFactory) return E_POINTER;
    IDWriteFactoryImpl* f = alloc_factory(factoryType);
    if (!f) { *ppFactory = 0; return E_OUTOFMEMORY; }
    *ppFactory = f;
    return S_OK;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
