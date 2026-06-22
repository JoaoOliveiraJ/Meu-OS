// ============================================================================
//  gpu.c — Camada generica de GPU sobre o(s) driver(s) instalado(s).
//
//  Backend atual: bochsvbe (QEMU std-vga / Bochs Display Interface). Cada
//  primitiva escreve direto no LFB mapeado (sem double-buffer). As cores sao
//  recebidas em RGBA 0xAARRGGBB; em 32 bpp o LFB usa BGRX (B no byte 0), entao
//  re-empacotamos no formato esperado pela VRAM.
//
//  Clipping em cada primitiva — coordenadas fora da tela sao descartadas.
// ============================================================================
#include <stdint.h>
#include "gpu.h"
#include "BasicDisplay.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

static int s_active = 0;

int gpu_init(uint32_t width, uint32_t height) {
    if (s_active) return 1;

    // Tenta Bochs VBE em 32 bpp (formato ideal: 4 bytes por pixel).
    if (!bochsvbe_init(width, height, 32)) {
        kputs("[gpu] bochsvbe_init falhou; GPU NAO ativada (fallback mode13h continua).\n");
        return 0;
    }
    s_active = 1;
    kputs("[gpu] init ok ");
    kput_dec(bochsvbe_width()); kputs("x");
    kput_dec(bochsvbe_height()); kputs("x");
    kput_dec(bochsvbe_bpp());
    kputs("  LFB=0x"); kput_hex((uint64_t)(uintptr_t)bochsvbe_lfb());
    kputs("  pitch="); kput_dec(bochsvbe_pitch());
    kputs("\n");
    return 1;
}

int      gpu_active(void) { return s_active && bochsvbe_active(); }
uint32_t gpu_width(void)  { return bochsvbe_width(); }
uint32_t gpu_height(void) { return bochsvbe_height(); }
uint32_t gpu_pitch(void)  { return bochsvbe_pitch(); }
uint32_t gpu_bpp(void)    { return bochsvbe_bpp(); }

// Re-empacota um RGBA (0xAARRGGBB) para 32 bpp BGRX (formato do LFB do Bochs).
//   byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = X (0)
// Como armazenamos como uint32_t em little-endian, a palavra fica
// 0x00RRGGBB — exatamente o mesmo padrao do RGBA sem alpha. Funciona direto.
static inline uint32_t pack32(uint32_t rgba) {
    return rgba & 0x00FFFFFFu;
}

void gpu_clear(uint32_t rgba) {
    if (!gpu_active()) return;
    uint8_t*  fb = bochsvbe_lfb();
    uint32_t  pitch = bochsvbe_pitch();
    uint32_t  h = bochsvbe_height();
    uint32_t  w = bochsvbe_width();
    uint32_t  v = pack32(rgba);

    if (bochsvbe_bpp() == 32) {
        for (uint32_t y = 0; y < h; y++) {
            uint32_t* row = (uint32_t*)(fb + y * pitch);
            for (uint32_t x = 0; x < w; x++) row[x] = v;
        }
    }
}

void gpu_pixel(int x, int y, uint32_t rgba) {
    if (!gpu_active()) return;
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= bochsvbe_width() || (uint32_t)y >= bochsvbe_height()) return;
    uint8_t* fb = bochsvbe_lfb();
    if (bochsvbe_bpp() == 32) {
        uint32_t* row = (uint32_t*)(fb + (uint32_t)y * bochsvbe_pitch());
        row[x] = pack32(rgba);
    }
}

void gpu_fill_rect(int x, int y, int w, int h, uint32_t rgba) {
    if (!gpu_active()) return;
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; int y1 = y + h;
    if (x1 > (int)bochsvbe_width())  x1 = (int)bochsvbe_width();
    if (y1 > (int)bochsvbe_height()) y1 = (int)bochsvbe_height();
    if (x1 <= x0 || y1 <= y0) return;

    uint8_t* fb = bochsvbe_lfb();
    uint32_t pitch = bochsvbe_pitch();
    uint32_t v = pack32(rgba);

    if (bochsvbe_bpp() == 32) {
        for (int yy = y0; yy < y1; yy++) {
            uint32_t* row = (uint32_t*)(fb + (uint32_t)yy * pitch);
            for (int xx = x0; xx < x1; xx++) row[xx] = v;
        }
    }
}

// Copia retangulo intra-LFB. Handle overlap fazendo a copia em ordem
// reversa quando o destino esta abaixo/depois da origem.
void gpu_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    if (!gpu_active()) return;
    if (w <= 0 || h <= 0) return;
    int W = (int)bochsvbe_width(), H = (int)bochsvbe_height();
    // Clip basico: cancela copia se qualquer canto sai da tela.
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0) return;
    if (sx + w > W || sy + h > H) return;
    if (dx + w > W || dy + h > H) return;

    uint8_t* fb = bochsvbe_lfb();
    uint32_t pitch = bochsvbe_pitch();
    uint32_t bpp_b = bochsvbe_bpp() / 8;

    if (dy > sy || (dy == sy && dx > sx)) {
        // Backwards (linhas de baixo p/ cima) p/ tolerar overlap descendente.
        for (int j = h - 1; j >= 0; j--) {
            uint8_t* src = fb + (sy + j) * pitch + sx * bpp_b;
            uint8_t* dst = fb + (dy + j) * pitch + dx * bpp_b;
            for (int i = w * (int)bpp_b - 1; i >= 0; i--) dst[i] = src[i];
        }
    } else {
        for (int j = 0; j < h; j++) {
            uint8_t* src = fb + (sy + j) * pitch + sx * bpp_b;
            uint8_t* dst = fb + (dy + j) * pitch + dx * bpp_b;
            for (int i = 0; i < w * (int)bpp_b; i++) dst[i] = src[i];
        }
    }
}

void gpu_present(void) {
    // Sem double-buffer: nada a fazer. Reservado pra quando houver back-buffer.
}
