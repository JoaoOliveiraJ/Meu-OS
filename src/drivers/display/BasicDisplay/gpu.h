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

// Apresenta o frame na tela. Sem double-buffer: no-op. Existe p/ contrato.
void     gpu_present(void);
