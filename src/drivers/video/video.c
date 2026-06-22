#include <stdint.h>
#include "video.h"
#include "io.h"

// ============================================================================
//  Driver de framebuffer grafico — VGA mode 13h (320x200x256), sem BIOS.
//
//  Em long mode nao temos a BIOS (int 10h) para trocar o modo de video, entao
//  programamos os blocos de registradores da VGA diretamente:
//    - Miscellaneous Output (0x3C2)
//    - Sequencer            (indice 0x3C4, dado 0x3C5)
//    - CRT Controller       (indice 0x3D4, dado 0x3D5)
//    - Graphics Controller  (indice 0x3CE, dado 0x3CF)
//    - Attribute Controller (indice/dado 0x3C0; leitura de status 0x3DA reseta o flip-flop)
//    - DAC / paleta de 256 cores (0x3C8 escreve indice, 0x3C9 RGB 6-bit x3)
//
//  Os valores abaixo sao o conjunto classico do mode 13h (mesmo dump usado por
//  FreeVGA/OSDev). O framebuffer linear fica em 0xA0000 (1 byte = 1 pixel,
//  indice na paleta), ja dentro da identidade de 1 GiB (primeira pagina 2 MiB).
// ============================================================================

#define VGA_FB ((volatile uint8_t*)0xA0000)

// --- portas dos blocos de registradores VGA ---
#define VGA_MISC_WRITE   0x3C2
#define VGA_SEQ_INDEX    0x3C4
#define VGA_SEQ_DATA     0x3C5
#define VGA_GC_INDEX     0x3CE
#define VGA_GC_DATA      0x3CF
#define VGA_CRTC_INDEX   0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_AC_INDEX     0x3C0   // attribute controller: indice E dado na mesma porta
#define VGA_AC_WRITE     0x3C0
#define VGA_INSTAT_READ  0x3DA   // ler reseta o flip-flop indice/dado do AC
#define VGA_DAC_WR_INDEX 0x3C8
#define VGA_DAC_DATA     0x3C9

static int s_active = 0;

// --- dumps de registradores do mode 13h ----------------------------------
// (ordem: o registrador i vai para o indice i de cada bloco)
static const uint8_t MISC_13H = 0x63;

static const uint8_t SEQ_13H[5] = {
    0x03, 0x01, 0x0F, 0x00, 0x0E
};

static const uint8_t CRTC_13H[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF
};

static const uint8_t GC_13H[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
    0xFF
};

static const uint8_t AC_13H[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

// Paleta: os 16 primeiros indices = paleta padrao EGA/CGA (RGB 0..255), que
// convertemos para os 6 bits do DAC (>>2). Acima de 16, uma rampa de cinza
// util para depuracao. Valores classicos do DOS.
static const uint8_t PAL16[16][3] = {
    {0x00,0x00,0x00}, {0x00,0x00,0xAA}, {0x00,0xAA,0x00}, {0x00,0xAA,0xAA},
    {0xAA,0x00,0x00}, {0xAA,0x00,0xAA}, {0xAA,0x55,0x00}, {0xAA,0xAA,0xAA},
    {0x55,0x55,0x55}, {0x55,0x55,0xFF}, {0x55,0xFF,0x55}, {0x55,0xFF,0xFF},
    {0xFF,0x55,0x55}, {0xFF,0x55,0xFF}, {0xFF,0xFF,0x55}, {0xFF,0xFF,0xFF}
};

static void write_regs(void) {
    // Miscellaneous Output
    outb(VGA_MISC_WRITE, MISC_13H);

    // Sequencer
    for (uint8_t i = 0; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        outb(VGA_SEQ_DATA, SEQ_13H[i]);
    }

    // Destrava os registradores 0..7 do CRTC (bit 7 do CRTC[0x11] = protect).
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & 0x7F));

    // CRT Controller (mantem o bit de protecao limpo nos valores que escrevemos)
    for (uint8_t i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, i);
        outb(VGA_CRTC_DATA, CRTC_13H[i]);
    }

    // Graphics Controller
    for (uint8_t i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        outb(VGA_GC_DATA, GC_13H[i]);
    }

    // Attribute Controller: reseta o flip-flop lendo INSTAT, escreve indice+dado
    // na MESMA porta (0x3C0) alternando, e no fim seta o bit 5 (PAS) para
    // reabilitar a saida de video.
    (void)inb(VGA_INSTAT_READ);
    for (uint8_t i = 0; i < 21; i++) {
        outb(VGA_AC_INDEX, i);
        outb(VGA_AC_WRITE, AC_13H[i]);
    }
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);   // bit5=1: liga a tela (Palette Address Source)
}

static void load_palette(void) {
    outb(VGA_DAC_WR_INDEX, 0);
    // 16 cores nomeadas (DAC usa 6 bits por canal -> divide por 4)
    for (int i = 0; i < 16; i++) {
        outb(VGA_DAC_DATA, (uint8_t)(PAL16[i][0] >> 2));
        outb(VGA_DAC_DATA, (uint8_t)(PAL16[i][1] >> 2));
        outb(VGA_DAC_DATA, (uint8_t)(PAL16[i][2] >> 2));
    }
    // 16..255: rampa de cinza (0..63), util para depurar gradientes.
    for (int i = 16; i < 256; i++) {
        uint8_t g = (uint8_t)((i * 63) / 255);
        outb(VGA_DAC_DATA, g);
        outb(VGA_DAC_DATA, g);
        outb(VGA_DAC_DATA, g);
    }
}

int fb_init(void) {
    write_regs();
    load_palette();
    s_active = 1;
    // limpa o framebuffer (320*200 = 64000 bytes)
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) VGA_FB[i] = FB_BLACK;
    return 1;
}

int fb_active(void) { return s_active; }

volatile uint8_t* fb_ptr(void) { return VGA_FB; }

void fb_clear(uint8_t color) {
    if (!s_active) return;
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) VGA_FB[i] = color;
}

void fb_pixel(int x, int y, uint8_t color) {
    if (!s_active) return;
    if ((unsigned)x >= FB_WIDTH || (unsigned)y >= FB_HEIGHT) return;
    VGA_FB[y * FB_WIDTH + x] = color;
}

uint8_t fb_get_pixel(int x, int y) {
    if (!s_active) return 0;
    if ((unsigned)x >= FB_WIDTH || (unsigned)y >= FB_HEIGHT) return 0;
    return VGA_FB[y * FB_WIDTH + x];
}

void fb_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (!s_active) return;
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > FB_WIDTH)  x1 = FB_WIDTH;
    int y1 = y + h; if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;
    for (int yy = y0; yy < y1; yy++) {
        volatile uint8_t* row = VGA_FB + yy * FB_WIDTH;
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

void fb_hline(int x, int y, int w, uint8_t color) { fb_fill_rect(x, y, w, 1, color); }
void fb_vline(int x, int y, int h, uint8_t color) { fb_fill_rect(x, y, 1, h, color); }

void fb_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    fb_hline(x, y, w, color);
    fb_hline(x, y + h - 1, w, color);
    fb_vline(x, y, h, color);
    fb_vline(x + w - 1, y, h, color);
}

// --- fonte bitmap 8x8 embutida (ASCII 0x20..0x7F) -------------------------
// Cada caractere = 8 bytes; cada byte e uma linha (bit 7 = pixel mais a esquerda).
// Fonte de dominio publico no estilo da fonte 8x8 do PC/VGA (subset imprimivel).
extern const uint8_t g_font8x8[96][8];

void fb_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (!s_active) return;
    unsigned char uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7F) uc = '?';
    const uint8_t* glyph = g_font8x8[uc - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_pixel(x + col, y + row, fg);
            } else if (bg != 0xFF) {
                fb_pixel(x + col, y + row, bg);
            }
        }
    }
}

void fb_draw_text(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    if (!s_active) return;
    int cx = x;
    int cy = y;
    while (*s) {
        char c = *s++;
        if (c == '\n') { cx = x; cy += 8; continue; }
        fb_draw_char(cx, cy, c, fg, bg);
        cx += 8;
        if (cx + 8 > FB_WIDTH) { cx = x; cy += 8; }
    }
}

// ============================================================================
//  FASE 9.2 — palette_8_to_32: converte um indice de paleta de 8 bits (mode
//  13h) para XRGB888 (formato 32 bpp do LFB do Bochs VBE). Mantemos as MESMAS
//  cores nominais da paleta padrao do mode 13h (PAL16[]) para que codigo que
//  hoje passa FB_BLUE/FB_RED continue produzindo o azul/vermelho do DOS no
//  novo backend, sem precisar reescrever cada chamada.
//  Indices 16..255 viram uma rampa de cinza linear (mesma ideia do load_palette).
// ============================================================================
uint32_t palette_8_to_32(uint8_t idx) {
    if (idx < 16) {
        uint8_t r = PAL16[idx][0];
        uint8_t g = PAL16[idx][1];
        uint8_t b = PAL16[idx][2];
        return RGB32(r, g, b);
    }
    // Rampa de cinza para 16..255 (0..255 linear).
    uint8_t g = (uint8_t)idx;     // simples e suficiente para depuracao visual
    return RGB32(g, g, g);
}
