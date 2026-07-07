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

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
