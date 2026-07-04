#pragma once
#include <stdint.h>

// ============================================================================
//  gpu.h — Wrapper de alto nivel sobre o driver de GPU disponivel.
//
//  Por enquanto so existe o backend Bochs VBE (bochsvbe.c). A interface usa
//  cores RGBA 32 bits (0xAARRGGBB) independente do BPP do hardware, e
//  internamente reduz quando preciso. Sem double-buffer ainda: gpu_present()
//  e no-op (escrevemos direto no LFB).
//
//  Coordenadas: (0,0) no canto superior esquerdo; X horizontal, Y vertical.
//  Clipping: feito em cada primitiva (off-screen e silenciosamente descartado).
// ============================================================================

// Inicializa a GPU em 'width' x 'height' (BPP escolhido pelo driver — 32 hoje).
// Retorna 1 em sucesso, 0 se o hardware nao existe ou a programacao falhou.
int      gpu_init(uint32_t width, uint32_t height);

// 1 se gpu_init concluiu (a tela esta em modo grafico LFB).
int      gpu_active(void);

uint32_t gpu_width(void);
uint32_t gpu_height(void);
uint32_t gpu_pitch(void);
uint32_t gpu_bpp(void);

// Desenho basico (cor RGBA: 0xAARRGGBB; alpha ignorado por ora).
void     gpu_clear(uint32_t rgba);
void     gpu_pixel(int x, int y, uint32_t rgba);
void     gpu_fill_rect(int x, int y, int w, int h, uint32_t rgba);
void     gpu_copy_rect(int sx, int sy, int dx, int dy, int w, int h);  // intra-LFB

// ----------------------------------------------------------------------------
//  Primitivas TRUE-COLOR (32 bpp) — a base da UI moderna (gradientes, sombras).
//  Todas operam em XRGB (0x00RRGGBB); so tem efeito no backend LFB 32 bpp.
// ----------------------------------------------------------------------------
// Le a cor XRGB de um pixel (0 se fora da tela / sem GPU).
uint32_t gpu_get_pixel(int x, int y);
// Gradiente VERTICAL: interpola 'top' (no topo do retangulo) ate 'bottom'
// (na base). Uma cor por linha — barato mesmo em tela cheia.
void     gpu_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
// Preenche um retangulo MISTURANDO 'rgb' com o fundo existente (alpha 0..255).
// alpha=255 => opaco (== fill_rect); alpha=0 => no-op. Base de sombras/acrilico.
void     gpu_blend_rect(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha);

// Apresenta o frame na tela. Sem double-buffer: no-op. Existe p/ contrato.
void     gpu_present(void);
// Apresenta SO um retangulo (cursor de software: move do mouse sem re-publicar a
// tela inteira). No Bochs VBE (LFB MMIO direto) e' no-op. Coordenadas em pixels.
void     gpu_present_rect(int x, int y, int w, int h);

// ----------------------------------------------------------------------------
//  Cursor de HARDWARE. No backend virtio-gpu, o host compoe o cursor sobre o
//  scanout: mover NAO recompoe o framebuffer (barato). No Bochs VBE nao ha
//  cursor de hardware — gpu_cursor_set devolve 0 e o caller usa sprite SW.
// ----------------------------------------------------------------------------
// Define a imagem do cursor (img = 64*64 uint32 BGRA; alpha!=0 = opaco) com
// hotspot e posicao inicial. Devolve 1 se o cursor de HW ficou ativo, 0 senao.
int      gpu_cursor_set(const uint32_t* img64x64, uint32_t hot_x, uint32_t hot_y,
                        int init_x, int init_y);
// Move o cursor de HW (no-op se nao houver). Barato: nao recompoe.
void     gpu_cursor_move(int x, int y);
// 1 se o cursor de HARDWARE esta ativo (entao o compose NAO desenha o sprite SW).
int      gpu_has_hw_cursor(void);
