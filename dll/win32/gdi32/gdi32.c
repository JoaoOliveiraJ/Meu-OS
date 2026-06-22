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

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
