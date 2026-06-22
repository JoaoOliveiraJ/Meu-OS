// ============================================================================
//  win32kfull.c  —  Subsistema Win32 (lado kernel) — LADO FULL (NT 6.4+).
//
//  Conforme o modelo Windows 8.1+, o win32kfull contem o GDI "complexo" e o
//  que o desktop normal nao precisa (advanced GDI, printing, formatos pesados
//  de DIB). Implementacoes minimas mas representativas:
//
//    - NtGdiTextOut_k / NtGdiTextOutEx_k  (TextOut com fundo opcional/cor)
//    - NtGdiFillRect_k                    (FillRect resolvido em brush real)
//    - NtGdiCreateDIBSection_k            (DIB minimo XRGB888 no heap kernel)
//
//  ATENCAO — modelo de inclusao: igual ao win32kbase.c, este arquivo NAO e
//  compilado isolado: o shim win32k.c faz #include "win32kbase.c" PRIMEIRO e
//  depois #include "win32kfull.c" (depende de tipos / s_wins / w32k_* /
//  draw helpers definidos no BASE).
// ============================================================================

// Resolve o HDC -> janela dona. O HDC e um HANDLE do Object Manager.
static WND* dc_window(void* hdc) {
    W32_DC* dc = (W32_DC*)ob_handle_to_object((HANDLE)hdc, OB_TYPE_EVENT);
    if (!dc) return 0;
    return wnd_from_id(dc->windowId);
}

// Converte (x,y) RELATIVO a area cliente da janela para coordenadas do desktop.
// A area cliente comeca logo abaixo da barra de titulo.
static int client_origin(WND* w, int* ox, int* oy) {
    if (!w) return 0;
    *ox = w->x + 1;                 // 1px da moldura
    *oy = w->y + TITLE_H + 1;       // abaixo da barra de titulo
    return 1;
}

// Implementacao compartilhada de TextOut. 'fg' = cor do texto; 'bg' = fundo
// (0xFF = transparente). Converte para coords do desktop e desenha so o que cabe
// na area cliente da janela (clipping vertical simples), sem invadir a barra de
// titulo nem ultrapassar a base da janela.
static uintptr_t textout_impl(void* hdc, int x, int y, const char* str, int len,
                              uint8_t fg, uint8_t bg) {
    ensure_fb();
    WND* w = dc_window(hdc);
    int ox = 0, oy = 0;
    if (!client_origin(w, &ox, &oy)) { ox = 0; oy = 0; }   // HDC do desktop

    // Clipping vertical a area cliente da janela (se houver janela).
    if (w && !(w->flags & WNDF_DESKTOP)) {
        int client_bottom = w->y + w->h - 1;          // dentro da moldura
        if (oy + y < w->y + TITLE_H + 1) return 1;      // acima da area cliente
        if (oy + y + 8 > client_bottom) return 1;       // abaixo da area cliente
    }

    char buf[64]; int n = 0;
    if (str) for (n = 0; n < len && n < 63 && str[n]; n++) buf[n] = str[n];
    buf[n] = 0;
    w32k_draw_text(ox + x, oy + y, buf, fg, bg);
    kputs("[gdi] TextOut(HDC, x="); kput_dec((uint64_t)x); kputs(", y=");
    kput_dec((uint64_t)y); kputs(", fg="); kput_dec((uint64_t)fg);
    kputs(", \""); kputs(buf); kputs("\") -> desktop(");
    kput_dec((uint64_t)(ox + x)); kputc(','); kput_dec((uint64_t)(oy + y));
    kputs(")\n");
    return 1;
}

uintptr_t NtGdiTextOut_k(void* hdc, int x, int y, const char* str, int len) {
    // Texto preto sobre fundo transparente (compat. Fase 2: janelas cinza).
    return textout_impl(hdc, x, y, str, len, FB_BLACK, 0xFF);
}

// FASE 6: TextOut com cor explicita (SetTextColor) — usado pelas janelas de
// console (texto claro sobre fundo escuro). bg=0xFF = transparente.
uintptr_t NtGdiTextOutEx_k(void* hdc, int x, int y, const char* str, int len,
                           uint32_t fg, uint32_t bg) {
    return textout_impl(hdc, x, y, str, len, (uint8_t)fg, (uint8_t)bg);
}

// ============================================================================
//  FASE 9.2 — DIB (Device Independent Bitmap) MINIMO.
//
//  Aloca um buffer de width*height*4 bytes (XRGB888) no heap do kernel e
//  devolve um HBITMAP (na verdade, ponteiro para um objeto W32_DIB com width/
//  height + ponteiro p/ os bits). Sem GDI completo: nao ha BitBlt aqui — apps
//  podem usar como back-buffer e copiar manualmente quando precisarem.
//
//  Em ppBits o chamador recebe o ponteiro p/ os bits (igual a CreateDIBSection
//  do GDI, que devolve um ponteiro p/ a memoria do bitmap fora do hbm).
// ============================================================================
typedef struct _W32_DIB {
    int      width;
    int      height;
    uint32_t bpp;        // sempre 32 (XRGB888)
    void*    bits;       // buffer de width*height*4 bytes
} W32_DIB;

uintptr_t NtGdiCreateDIBSection_k(int width, int height, void** ppBits) {
    if (width <= 0 || height <= 0) return 0;
    // Limite defensivo (max ~4 MiB por DIB; 1024x768 cabe).
    if ((uint64_t)width * (uint64_t)height > (uint64_t)(4 * 1024 * 1024)) return 0;

    W32_DIB* d = (W32_DIB*)kmalloc(sizeof(W32_DIB));
    if (!d) return 0;
    d->width  = width;
    d->height = height;
    d->bpp    = 32;
    size_t nbytes = (size_t)width * (size_t)height * 4u;
    d->bits = kmalloc(nbytes);
    if (!d->bits) { return 0; }   // sem kfree(d): heap simples sem coalesce robusto

    // Zera o bitmap (preto opaco).
    uint8_t* p = (uint8_t*)d->bits;
    for (size_t i = 0; i < nbytes; i++) p[i] = 0;

    if (ppBits) *ppBits = d->bits;

    kputs("[gdi] CreateDIBSection("); kput_dec((uint64_t)width); kputc('x');
    kput_dec((uint64_t)height); kputs("x32) -> HBITMAP=");
    kput_hex((uint64_t)(uintptr_t)d);
    kputs(" bits="); kput_hex((uint64_t)(uintptr_t)d->bits);
    kputs(" ("); kput_dec((uint64_t)nbytes); kputs(" bytes)\n");
    return (uintptr_t)d;
}

uintptr_t NtGdiFillRect_k(void* hdc, int x, int y, int w, int h, void* hbrush) {
    ensure_fb();
    WND* win = dc_window(hdc);
    int ox = 0, oy = 0;
    client_origin(win, &ox, &oy);
    uint8_t color = FB_WHITE;
    W32_BRUSH* b = (W32_BRUSH*)hbrush;
    if (b) color = (uint8_t)b->color;   // brushes sao ponteiros diretos
    w32k_fill_rect(ox + x, oy + y, w, h, color);
    kputs("[gdi] FillRect(HDC, x="); kput_dec((uint64_t)x); kputs(", y=");
    kput_dec((uint64_t)y); kputs(", w="); kput_dec((uint64_t)w); kputs(", h=");
    kput_dec((uint64_t)h); kputs(", cor="); kput_dec((uint64_t)color);
    kputs(") -> desktop("); kput_dec((uint64_t)(ox + x)); kputc(',');
    kput_dec((uint64_t)(oy + y)); kputs(")\n");
    return 1;
}
