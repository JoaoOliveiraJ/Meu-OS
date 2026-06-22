#pragma once
#include <stdint.h>

// ============================================================================
//  video.h  —  Driver de video com FRAMEBUFFER grafico (VGA mode 13h).
//
//  Mode 13h: 320x200, 256 cores (8 bpp), framebuffer linear em 0xA0000 (ja
//  mapeado na identidade do 1o MiB). Programamos os registradores VGA DIRETO
//  (sem BIOS, que nao existe em long mode), incluindo a paleta DAC e uma fonte
//  bitmap 8x8 embutida para texto.
//
//  A SERIAL continua sendo o canal de log (kputc): cada operacao grafica
//  importante e logada para comprovar a logica mesmo em modo headless.
// ============================================================================

#define FB_WIDTH   320
#define FB_HEIGHT  200

// Cores da paleta padrao do mode 13h (os 16 primeiros indices = paleta EGA/CGA,
// que reprogramamos no fb_init para valores conhecidos). Use os indices abaixo
// ou um indice arbitrario 0..255 (a faixa 16..255 vira uma rampa de cinza/cor).
#define FB_BLACK         0
#define FB_BLUE          1
#define FB_GREEN         2
#define FB_CYAN          3
#define FB_RED           4
#define FB_MAGENTA       5
#define FB_BROWN         6
#define FB_LIGHT_GRAY    7
#define FB_DARK_GRAY     8
#define FB_LIGHT_BLUE    9
#define FB_LIGHT_GREEN   10
#define FB_LIGHT_CYAN    11
#define FB_LIGHT_RED     12
#define FB_LIGHT_MAGENTA 13
#define FB_YELLOW        14
#define FB_WHITE         15

// Inicializa o modo grafico 13h (programa CRTC/sequencer/GC/attribute + DAC).
// Apos chamar, a tela passa a ser grafica; o texto VGA (0xB8000) deixa de ser
// exibido, mas a serial continua. Retorna 1 em sucesso.
int  fb_init(void);

// Verdadeiro se o framebuffer grafico esta ativo (fb_init ja rodou).
int  fb_active(void);

// Operacoes de desenho (no-op seguro se !fb_active).
void fb_clear(uint8_t color);
void fb_pixel(int x, int y, uint8_t color);
uint8_t fb_get_pixel(int x, int y);
void fb_fill_rect(int x, int y, int w, int h, uint8_t color);
void fb_rect(int x, int y, int w, int h, uint8_t color);          // contorno (1px)
void fb_hline(int x, int y, int w, uint8_t color);
void fb_vline(int x, int y, int h, uint8_t color);

// Texto com a fonte bitmap 8x8 embutida.
void fb_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);  // bg=0xFF -> transparente
void fb_draw_text(int x, int y, const char* s, uint8_t fg, uint8_t bg);

// Ponteiro do framebuffer (0xA0000) — uso interno/avancado.
volatile uint8_t* fb_ptr(void);

// ============================================================================
//  FASE 9.2 — Cores 32 bits (XRGB888) p/ o backend gpu_* (LFB 32 bpp).
//
//  O LFB do Bochs VBE e BGRX em little-endian: armazenar 0x00RRGGBB num
//  uint32_t coloca B no byte 0, G no byte 1, R no byte 2 — exatamente o que a
//  VRAM espera. Ou seja, RGB32(r,g,b) = (r<<16)|(g<<8)|b serve direto.
//  O canal alpha do RGBA e ignorado por gpu_* (alpha = 0).
//
//  palette_8_to_32(idx) mapeia os 16 indices nomeados do mode 13h (FB_BLACK ..
//  FB_WHITE) para o equivalente em XRGB888. Indices >=16 (rampa de cinza)
//  ficam mapeados para uma rampa correspondente. Isto deixa o win32k usar UM
//  unico caminho de cores (uint8_t/uint32_t) e escolher o backend em runtime.
// ============================================================================
#define RGB32(r,g,b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGB32_BLACK         0x00000000u
#define RGB32_WHITE         0x00FFFFFFu
#define RGB32_BLUE          0x000000FFu
#define RGB32_GREEN         0x0000FF00u
#define RGB32_RED           0x00FF0000u
#define RGB32_GRAY          0x00C0C0C0u
#define RGB32_DARK_GRAY     0x00555555u
#define RGB32_LIGHT_GRAY    0x00AAAAAAu
#define RGB32_DESKTOP_BLUE  0x00103060u
#define RGB32_TITLE_BLUE    0x000A246Au
#define RGB32_TRANSPARENT   0xFF000000u   // alpha=0xFF => "transparente" (sentinela)

// Converte um indice de paleta (mode 13h) -> XRGB888 (mesmas cores nominais
// da paleta padrao do mode13h). Indices >=16 viram rampa de cinza linear.
uint32_t palette_8_to_32(uint8_t idx);
