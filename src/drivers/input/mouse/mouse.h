#pragma once
#include <stdint.h>

// ============================================================================
//  mouse.h  —  Driver PS/2 do mouse (IRQ12).
//
//  Estilo Windows i8042prt.sys: inicializa o dispositivo auxiliar do controlador
//  i8042 (porta 0x60 / 0x64), habilita IRQ12 no slave PIC, e configura o mouse
//  com defaults + stream mode. O pacote padrao tem 3 bytes que sao montados
//  por uma maquina de estado dentro de mouse_irq() (cada IRQ entrega 1 byte).
//
//  Eventos sao postados ao win32k via win32k_on_mouse_event(dx, dy, buttons),
//  que atualiza a posicao do cursor (clamp a tela) e roteia WM_MOUSEMOVE /
//  WM_LBUTTONDOWN / WM_LBUTTONUP / WM_RBUTTONDOWN / WM_RBUTTONUP para a janela
//  que esta debaixo do cursor (hit-test). Mesmo headless: cada IRQ e logado
//  na serial (regra 3).
// ============================================================================

// Botoes (bits no estado retornado por mouse_buttons()).
#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

// Inicializa o mouse PS/2: habilita o dispositivo auxiliar (Enable Aux Device),
// desmascara a IRQ12 no slave PIC + na cascade do master, manda set defaults
// (0xF6) e enable stream mode (0xF4). Idempotente.
void mouse_init(void);

// Tratador da IRQ12 (vector 32+12=44 apos pic_remap). Le 1 byte por chamada do
// port 0x60; quando o pacote de 3 bytes esta completo, decodifica e posta um
// evento para o win32k.
void mouse_irq(void);

// Estado atual do cursor (atualizado pela IRQ12). Coordenadas em pixels do
// framebuffer ativo (LFB 32 bpp ou mode13h 8 bpp). Botoes: bitmask MOUSE_BTN_*.
int32_t  mouse_x(void);
int32_t  mouse_y(void);
uint32_t mouse_buttons(void);
