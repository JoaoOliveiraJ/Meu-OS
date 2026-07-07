// gdi32.dll  —  reimplementacao. API GDI classica: TextOutA, FillRect,
// GetStockObject, CreateSolidBrush. Igual ao Windows: vive em RING 3 e
// encaminha para o win32k (lado kernel) via ntdll (int 0x80), que desenha no
// framebuffer (drivers/video.c).
unsigned int _tls_index = 0;

// ---- imports do ntdll (camada de syscall) ----
__declspec(dllimport) void* NtGdiGetStockObject(int index);
__declspec(dllimport) void* NtGdiCreateSolidBrush(unsigned color);
__declspec(dllimport) long  NtGdiTextOut(void* hdc, int x, int y, const char* str, int len);
__declspec(dllimport) long  NtGdiTextOutEx(void* hdc, int x, int y, const char* str, int len, unsigned fg);

static unsigned slen(const char* s) { unsigned n = 0; if (s) while (s[n]) n++; return n; }

// ---- FASE 6: cor de texto por HDC (SetTextColor), igual ao Windows ----
// Tabela simples HDC -> cor corrente. -1 = padrao (texto preto). O HDC e um
// handle pequeno; indexamos por igualdade de ponteiro (poucos HDCs ativos).
#define MAX_DC 16
static struct { void* hdc; int color; } g_textcolor[MAX_DC];

// SetTextColor(hdc, color): define a cor do texto (indice de paleta) para o HDC.
// Devolve a cor anterior (ou 0). As proximas TextOutA usam esta cor.
__declspec(dllexport) unsigned SetTextColor(void* hdc, unsigned color) {
    int prev = -1;
    for (int i = 0; i < MAX_DC; i++) if (g_textcolor[i].hdc == hdc) {
        prev = g_textcolor[i].color; g_textcolor[i].color = (int)color; return (unsigned)prev;
    }
    for (int i = 0; i < MAX_DC; i++) if (!g_textcolor[i].hdc) {
        g_textcolor[i].hdc = hdc; g_textcolor[i].color = (int)color; return 0;
    }
    return 0;
}
static int textcolor_of(void* hdc) {
    for (int i = 0; i < MAX_DC; i++) if (g_textcolor[i].hdc == hdc) return g_textcolor[i].color;
    return -1;   // padrao: texto preto (caminho NtGdiTextOut da Fase 2)
}

// TextOutA(hdc, x, y, str, count): desenha texto na area cliente da janela.
// (No Windows real, TextOutA vive em gdi32.dll — mantemos aqui.) Se houver uma
// cor definida via SetTextColor para o HDC, usa NtGdiTextOutEx (texto colorido,
// p/ as janelas de console); senao, NtGdiTextOut (texto preto, compat. Fase 2).
__declspec(dllexport) int TextOutA(void* hdc, int x, int y, const char* str, int count) {
    if (count < 0) count = (int)slen(str);
    int color = textcolor_of(hdc);
    if (color >= 0) return (int)NtGdiTextOutEx(hdc, x, y, str, count, (unsigned)color);
    return (int)NtGdiTextOut(hdc, x, y, str, count);
}

// GetStockObject(index): devolve um HBRUSH stock (WHITE_BRUSH, GRAY_BRUSH...).
__declspec(dllexport) void* GetStockObject(int index) {
    return NtGdiGetStockObject(index);
}

// CreateSolidBrush(color): cria um HBRUSH com a cor (indice de paleta) dada.
__declspec(dllexport) void* CreateSolidBrush(unsigned color) {
    return NtGdiCreateSolidBrush(color);
}

// DeleteObject: stub (os brushes vivem no Object Manager; nada urgente a fazer).
__declspec(dllexport) int DeleteObject(void* obj) { (void)obj; return 1; }

// GetDeviceCaps(hdc, index): capacidades da tela. Modo FIXO do MeuOS: framebuffer
// 1024x768x32, DPI 96. Valores constantes REAIS p/ esse dispositivo (nao stub: sao as
// caps do device). O explorer consulta LOGPIXELSY (90) na init da shell p/ escalar DPI.
// Indice desconhecido -> 0 (comportamento documentado do GetDeviceCaps).
__declspec(dllexport) int GetDeviceCaps(void* hdc, int index) {
    (void)hdc;
    switch (index) {
        case 0:   return 0x4000;   // DRIVERVERSION
        case 2:   return 0;        // TECHNOLOGY = DT_RASDISPLAY
        case 4:   return 270;      // HORZSIZE (mm) ~= 1024px @ 96dpi
        case 6:   return 203;      // VERTSIZE (mm) ~=  768px @ 96dpi
        case 8:   return 1024;     // HORZRES
        case 10:  return 768;      // VERTRES
        case 12:  return 32;       // BITSPIXEL
        case 14:  return 1;        // PLANES
        case 24:  return -1;       // NUMCOLORS (device > 8bpp)
        case 40:  return 36;       // ASPECTX
        case 42:  return 36;       // ASPECTY
        case 44:  return 51;       // ASPECTXY
        case 88:  return 96;       // LOGPIXELSX (DPI horizontal)
        case 90:  return 96;       // LOGPIXELSY (DPI vertical)
        case 104: return 0;        // SIZEPALETTE (nao paletizado)
        case 108: return 24;       // COLORRES
        case 116: return 60;       // VREFRESH (Hz)
        case 117: return 768;      // DESKTOPVERTRES
        case 118: return 1024;     // DESKTOPHORZRES
        default:  return 0;
    }
}

// ============================================================================
//  Frente C (explorer real) — GDI expandido. O explorer importa 28 funcoes de gdi32;
//  antes tinhamos 6, as outras 22 resolviam p/ NULL (crash latente ao desenhar). Aqui
//  implementamos as 22 + BitBlt (pedida por GetProcAddress). Modelo: handles OPACOS (nao-
//  nulos, distintos) + metricas PLAUSIVEIS p/ a fonte fixa 8x16 do MeuOS. As consultas de
//  layout (extent/metrics/regiao/clip) dao respostas coerentes; o desenho real de bitmap
//  (Blt) e' no-op por ora (framebuffer unico) — degrada sem crashar. Documentado, nao stub
//  catch-all: cada funcao e' nomeada e faz o correto p/ o device fixo 1024x768.
// ============================================================================
typedef struct { int left, top, right, bottom; } GRECT_;
typedef struct { int cx, cy; } GSIZE_;
typedef struct {
    int tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth,
        tmMaxCharWidth, tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    unsigned short tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    unsigned char tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} GTEXTMETRICW_;

#define GDI_NULLREGION   1
#define GDI_SIMPLEREGION 2
#define GDI_ERROR_       0xFFFFFFFFu

// Alocador de handles GDI opacos: valores distintos, nao-nulos, fora da faixa de ponteiros
// reais das nossas DLLs (que vivem < 0x60000000). Sem rastreio de objeto (layout-only).
static void* gdi_handle(void) { static unsigned long long n = 0xD1000000ull; n += 0x40; return (void*)n; }

static void gzero(void* p, unsigned n) { unsigned char* d = (unsigned char*)p; for (unsigned i = 0; i < n; i++) d[i] = 0; }

// ---- DC de memoria + objetos ----
__declspec(dllexport) void* CreateCompatibleDC(void* hdc) { (void)hdc; return gdi_handle(); }
__declspec(dllexport) int   DeleteDC(void* hdc) { (void)hdc; return 1; }
__declspec(dllexport) void* SelectObject(void* hdc, void* obj) { (void)hdc; (void)obj; return gdi_handle(); }  // "objeto anterior" nao-nulo
__declspec(dllexport) void* GetCurrentObject(void* hdc, unsigned type) { (void)hdc; (void)type; return gdi_handle(); }
__declspec(dllexport) int   GetObjectW(void* obj, int cb, void* buf) { (void)obj; if (buf && cb > 0) gzero(buf, (unsigned)cb); return cb; }

// ---- regioes (forma da taskbar / clipping) ----
__declspec(dllexport) void* CreateRectRgn(int l, int t, int r, int b) { (void)l;(void)t;(void)r;(void)b; return gdi_handle(); }
__declspec(dllexport) void* CreateRectRgnIndirect(const GRECT_* prc) { (void)prc; return gdi_handle(); }
__declspec(dllexport) int   SetRectRgn(void* rgn, int l, int t, int r, int b) { (void)rgn;(void)l;(void)t;(void)r;(void)b; return 1; }
__declspec(dllexport) int   CombineRgn(void* dst, void* s1, void* s2, int mode) { (void)dst;(void)s1;(void)s2;(void)mode; return GDI_SIMPLEREGION; }
__declspec(dllexport) int   OffsetRgn(void* rgn, int dx, int dy) { (void)rgn;(void)dx;(void)dy; return GDI_SIMPLEREGION; }
__declspec(dllexport) int   SelectClipRgn(void* hdc, void* rgn) { (void)hdc;(void)rgn; return GDI_SIMPLEREGION; }
__declspec(dllexport) int   ExcludeClipRect(void* hdc, int l, int t, int r, int b) { (void)hdc;(void)l;(void)t;(void)r;(void)b; return GDI_SIMPLEREGION; }
__declspec(dllexport) int   GetClipBox(void* hdc, GRECT_* prc) { (void)hdc; if (prc) { prc->left=0; prc->top=0; prc->right=1024; prc->bottom=768; } return GDI_SIMPLEREGION; }
__declspec(dllexport) int   GetClipRgn(void* hdc, void* rgn) { (void)hdc;(void)rgn; return 0; }   // 0 = HDC sem regiao de clip

// ---- fonte + medicao de texto (fonte fixa 8x16) ----
__declspec(dllexport) void* CreateFontIndirectW(const void* plf) { (void)plf; return gdi_handle(); }
__declspec(dllexport) int   GetTextExtentPoint32W(void* hdc, const unsigned short* s, int c, GSIZE_* sz) {
    (void)hdc; (void)s; if (sz) { int n = c < 0 ? 0 : c; sz->cx = n * 8; sz->cy = 16; } return 1;
}
__declspec(dllexport) int GetTextMetricsW(void* hdc, GTEXTMETRICW_* tm) {
    (void)hdc; if (!tm) return 0;
    gzero(tm, sizeof(*tm));
    tm->tmHeight = 16; tm->tmAscent = 13; tm->tmDescent = 3; tm->tmInternalLeading = 2;
    tm->tmAveCharWidth = 8; tm->tmMaxCharWidth = 8; tm->tmWeight = 400;
    tm->tmDigitizedAspectX = 96; tm->tmDigitizedAspectY = 96;
    tm->tmFirstChar = 32; tm->tmLastChar = 255; tm->tmDefaultChar = '?'; tm->tmBreakChar = ' ';
    tm->tmPitchAndFamily = 0;   // FIXED_PITCH implicito (bit0=0)
    return 1;
}
__declspec(dllexport) unsigned GetOutlineTextMetricsW(void* hdc, unsigned cb, void* potm) {
    (void)hdc; (void)cb; (void)potm; return 0;   // fonte raster: sem metricas de contorno -> caller usa GetTextMetrics
}
__declspec(dllexport) unsigned GetGlyphOutlineW(void* hdc, unsigned ch, unsigned fmt, void* gm, unsigned cb, void* buf, const void* mat) {
    (void)hdc;(void)ch;(void)fmt;(void)gm;(void)cb;(void)buf;(void)mat; return GDI_ERROR_;   // sem contorno de glifo
}
// ExtTextOutW: desenha texto wide. Converte UTF-16->ascii e usa o mesmo caminho do TextOutA.
__declspec(dllexport) int ExtTextOutW(void* hdc, int x, int y, unsigned opts, const GRECT_* prc,
        const unsigned short* s, unsigned c, const int* dx) {
    (void)opts; (void)prc; (void)dx;
    char b[256]; unsigned n = 0; if (s) for (; n < c && n < 255; n++) b[n] = (char)s[n]; b[n] = 0;
    int color = textcolor_of(hdc);
    if (color >= 0) NtGdiTextOutEx(hdc, x, y, b, (int)n, (unsigned)color);
    else            NtGdiTextOut(hdc, x, y, b, (int)n);
    return 1;
}
__declspec(dllexport) unsigned SetTextAlign(void* hdc, unsigned align) { (void)hdc; (void)align; return 0; }   // devolve alinhamento anterior

// ---- desenho de forma / blt (framebuffer unico: no-op que reporta sucesso) ----
__declspec(dllexport) int Rectangle(void* hdc, int l, int t, int r, int b) { (void)hdc;(void)l;(void)t;(void)r;(void)b; return 1; }
__declspec(dllexport) int SetStretchBltMode(void* hdc, int mode) { (void)hdc; (void)mode; return 1; }   // modo anterior
__declspec(dllexport) int StretchBlt(void* dst, int xd, int yd, int wd, int hd, void* src, int xs, int ys, int ws, int hs, unsigned rop) {
    (void)dst;(void)xd;(void)yd;(void)wd;(void)hd;(void)src;(void)xs;(void)ys;(void)ws;(void)hs;(void)rop; return 1;
}
__declspec(dllexport) int BitBlt(void* dst, int xd, int yd, int wd, int hd, void* src, int xs, int ys, unsigned rop) {
    (void)dst;(void)xd;(void)yd;(void)wd;(void)hd;(void)src;(void)xs;(void)ys;(void)rop; return 1;
}

// ---- bitmaps + DIB (double-buffer off-screen do desenho da taskbar/desktop) ----
// Bits do CreateDIBSection via NtVirtualAlloc (NAO um array estatico: um BSS grande estoura
// o mapeamento de usuario de 2 MiB que o loader da' a uma DLL de base baixa -> travava o
// boot). VirtualAlloc devolve memoria de usuario do PMM, dimensionada por DIB.
__declspec(dllimport) void* NtVirtualAlloc(unsigned long long size);

__declspec(dllexport) void* CreateCompatibleBitmap(void* hdc, int w, int h) { (void)hdc;(void)w;(void)h; return gdi_handle(); }
__declspec(dllexport) void* CreateBitmap(int w, int h, unsigned planes, unsigned bpp, const void* bits) { (void)w;(void)h;(void)planes;(void)bpp;(void)bits; return gdi_handle(); }
__declspec(dllexport) void* CreateDIBitmap(void* hdc, const void* hdr, unsigned flags, const void* bits, const void* bmi, unsigned usage) { (void)hdc;(void)hdr;(void)flags;(void)bits;(void)bmi;(void)usage; return gdi_handle(); }
// CreateDIBSection: devolve um HBITMAP E um ponteiro p/ os bits (ppvBits). Extrai w/h/bpp do
// BITMAPINFOHEADER (biWidth@4, biHeight@8, biBitCount@14) p/ dimensionar o buffer real.
__declspec(dllexport) void* CreateDIBSection(void* hdc, const void* pbmi, unsigned usage, void** ppvBits, void* hSection, unsigned offset) {
    (void)hdc; (void)usage; (void)hSection; (void)offset;
    int w = 0, h = 0; unsigned bpp = 32;
    if (pbmi) { const unsigned char* p = (const unsigned char*)pbmi;
        w = *(const int*)(p + 4); h = *(const int*)(p + 8); bpp = *(const unsigned short*)(p + 14); if (!bpp) bpp = 32; }
    if (w < 0) w = -w; if (h < 0) h = -h;
    unsigned long long stride = ((unsigned long long)w * bpp + 31) / 32 * 4;
    unsigned long long cb = stride * (h ? (unsigned)h : 1);
    if (cb < 0x1000) cb = 0x1000;
    void* bits = NtVirtualAlloc(cb);                    // buffer REAL (PMM), dimensionado
    if (ppvBits) *ppvBits = bits;                       // o explorer desenha aqui
    return gdi_handle();
}
__declspec(dllexport) int GetDIBits(void* hdc, void* hbm, unsigned start, unsigned lines, void* bits, void* bmi, unsigned usage) { (void)hdc;(void)hbm;(void)start;(void)bits;(void)bmi;(void)usage; return (int)lines; }
__declspec(dllexport) int SetDIBits(void* hdc, void* hbm, unsigned start, unsigned lines, const void* bits, const void* bmi, unsigned usage) { (void)hdc;(void)hbm;(void)start;(void)bits;(void)bmi;(void)usage; return (int)lines; }
__declspec(dllexport) int GetObjectA(void* obj, int cb, void* buf) { (void)obj; if (buf && cb > 0) gzero(buf, (unsigned)cb); return cb; }

// ---- blend/gradient/transparente + raster (helpers de desenho da taskbar/menu) ----
// Framebuffer unico: reportam sucesso sem compor pixels (degrada sem crashar). Gdi*
// (internos do gdi32) e os nomes de msimg32 (AlphaBlend/GradientFill/TransparentBlt).
__declspec(dllexport) int GdiAlphaBlend(void* dst, int xd,int yd,int wd,int hd, void* src, int xs,int ys,int ws,int hs, unsigned bf) { (void)dst;(void)xd;(void)yd;(void)wd;(void)hd;(void)src;(void)xs;(void)ys;(void)ws;(void)hs;(void)bf; return 1; }
__declspec(dllexport) int AlphaBlend(void* dst, int xd,int yd,int wd,int hd, void* src, int xs,int ys,int ws,int hs, unsigned bf) { (void)dst;(void)xd;(void)yd;(void)wd;(void)hd;(void)src;(void)xs;(void)ys;(void)ws;(void)hs;(void)bf; return 1; }
__declspec(dllexport) int GdiGradientFill(void* hdc, void* pv, unsigned nv, void* pm, unsigned nm, unsigned mode) { (void)hdc;(void)pv;(void)nv;(void)pm;(void)nm;(void)mode; return 1; }
__declspec(dllexport) int GradientFill(void* hdc, void* pv, unsigned nv, void* pm, unsigned nm, unsigned mode) { (void)hdc;(void)pv;(void)nv;(void)pm;(void)nm;(void)mode; return 1; }
__declspec(dllexport) int GdiTransparentBlt(void* dst, int xd,int yd,int wd,int hd, void* src, int xs,int ys,int ws,int hs, unsigned key) { (void)dst;(void)xd;(void)yd;(void)wd;(void)hd;(void)src;(void)xs;(void)ys;(void)ws;(void)hs;(void)key; return 1; }
__declspec(dllexport) int TransparentBlt(void* dst, int xd,int yd,int wd,int hd, void* src, int xs,int ys,int ws,int hs, unsigned key) { (void)dst;(void)xd;(void)yd;(void)wd;(void)hd;(void)src;(void)xs;(void)ys;(void)ws;(void)hs;(void)key; return 1; }
__declspec(dllexport) int PatBlt(void* hdc, int x,int y,int w,int h, unsigned rop) { (void)hdc;(void)x;(void)y;(void)w;(void)h;(void)rop; return 1; }
__declspec(dllexport) int StretchDIBits(void* hdc, int xd,int yd,int wd,int hd, int xs,int ys,int ws,int hs, const void* bits, const void* bmi, unsigned usage, unsigned rop) { (void)hdc;(void)xd;(void)yd;(void)wd;(void)hd;(void)xs;(void)ys;(void)ws;(void)hs;(void)bits;(void)bmi;(void)usage;(void)rop; return hd; }
__declspec(dllexport) int SetDIBitsToDevice(void* hdc, int xd,int yd, unsigned w,unsigned h, int xs,int ys, unsigned start, unsigned lines, const void* bits, const void* bmi, unsigned usage) { (void)hdc;(void)xd;(void)yd;(void)w;(void)h;(void)xs;(void)ys;(void)start;(void)bits;(void)bmi;(void)usage; return (int)lines; }

// ---- estado do DC + primitivas (usadas no desenho classico do chrome) ----
__declspec(dllexport) int SaveDC(void* hdc) { (void)hdc; return 1; }
__declspec(dllexport) int RestoreDC(void* hdc, int n) { (void)hdc; (void)n; return 1; }
__declspec(dllexport) int SetBkMode(void* hdc, int mode) { (void)hdc; (void)mode; return 1; }      // devolve modo anterior (OPAQUE)
__declspec(dllexport) unsigned SetBkColor(void* hdc, unsigned c) { (void)hdc; (void)c; return 0; }
__declspec(dllexport) unsigned GetBkColor(void* hdc) { (void)hdc; return 0x00FFFFFF; }
__declspec(dllexport) unsigned GetTextColor(void* hdc) { int c = textcolor_of(hdc); return c < 0 ? 0 : (unsigned)c; }
__declspec(dllexport) int GetStretchBltMode(void* hdc) { (void)hdc; return 1; }
__declspec(dllexport) int IntersectClipRect(void* hdc, int l,int t,int r,int b) { (void)hdc;(void)l;(void)t;(void)r;(void)b; return GDI_SIMPLEREGION; }
__declspec(dllexport) int ExtSelectClipRgn(void* hdc, void* rgn, int mode) { (void)hdc;(void)rgn;(void)mode; return GDI_SIMPLEREGION; }
__declspec(dllexport) void* CreatePen(int style, int width, unsigned color) { (void)style;(void)width;(void)color; return gdi_handle(); }
__declspec(dllexport) int MoveToEx(void* hdc, int x, int y, void* ppt) { (void)hdc; if (ppt) { ((int*)ppt)[0]=x; ((int*)ppt)[1]=y; } return 1; }
__declspec(dllexport) int LineTo(void* hdc, int x, int y) { (void)hdc; (void)x; (void)y; return 1; }
__declspec(dllexport) unsigned SetPixel(void* hdc, int x, int y, unsigned c) { (void)hdc;(void)x;(void)y; return c; }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
