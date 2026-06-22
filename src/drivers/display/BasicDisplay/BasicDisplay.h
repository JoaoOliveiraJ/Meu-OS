#pragma once
#include <stdint.h>

// ============================================================================
//  bochsvbe.h — Driver para o Bochs VBE Display Interface (BGA), padrao no
//  QEMU "-vga std" e Bochs. PCI vendor=0x1234 device=0x1111.
//
//  Programacao via duas portas de I/O:
//    0x01CE = INDEX  (qual registrador VBE Dispi quero acessar)
//    0x01CF = DATA   (le/escreve o registrador selecionado, 16 bits)
//  Indices: 0=ID, 1=XRES, 2=YRES, 3=BPP, 4=ENABLE, ...
//  ID = 0xB0C5 confirma a interface presente.
//
//  ENABLE bits importantes:
//    VBE_DISPI_ENABLED      (0x01) - liga o modo grafico
//    VBE_DISPI_LFB_ENABLED  (0x40) - habilita Linear FrameBuffer (BAR0)
//    VBE_DISPI_NOCLEARMEM   (0x80) - nao limpa VRAM ao trocar modo
//
//  A VRAM aparece linearmente na BAR0 do PCI (endereco fisico tipicamente
//  0xFD000000 no QEMU). O driver le essa BAR via HAL e mapeia a faixa em
//  virtual via mm_map_phys_range (ela esta FORA da identidade de 1 GiB).
// ============================================================================

#define VBE_DISPI_IOPORT_INDEX   0x01CE
#define VBE_DISPI_IOPORT_DATA    0x01CF

#define VBE_DISPI_INDEX_ID       0x0
#define VBE_DISPI_INDEX_XRES     0x1
#define VBE_DISPI_INDEX_YRES     0x2
#define VBE_DISPI_INDEX_BPP      0x3
#define VBE_DISPI_INDEX_ENABLE   0x4
#define VBE_DISPI_INDEX_BANK     0x5
#define VBE_DISPI_INDEX_VIRT_W   0x6
#define VBE_DISPI_INDEX_VIRT_H   0x7
#define VBE_DISPI_INDEX_X_OFFSET 0x8
#define VBE_DISPI_INDEX_Y_OFFSET 0x9

// IDs publicados pelas versoes da BGA. Aceitamos qualquer ID4/5 ou maior.
#define VBE_DISPI_ID0            0xB0C0
#define VBE_DISPI_ID4            0xB0C4
#define VBE_DISPI_ID5            0xB0C5

#define VBE_DISPI_DISABLED       0x00
#define VBE_DISPI_ENABLED        0x01
#define VBE_DISPI_GETCAPS        0x02
#define VBE_DISPI_8BIT_DAC       0x20
#define VBE_DISPI_LFB_ENABLED    0x40
#define VBE_DISPI_NOCLEARMEM     0x80

#define VBE_PCI_VENDOR           0x1234
#define VBE_PCI_DEVICE           0x1111

// Inicializa o driver: localiza o PCI 1234:1111, le BAR0 (LFB), mapeia em
// virtual via mm_map_phys_range, valida ID via porta de I/O e programa o
// modo (XRES/YRES/BPP). Retorna 1 em sucesso, 0 se nao achou o dispositivo
// ou se a programacao falhou. Loga cada passo na serial ([bvbe] ...).
int      bochsvbe_init(uint32_t width, uint32_t height, uint32_t bpp);

// Estado depois do init: ponteiro para o LFB (em virt), dimensoes/pitch/bpp.
uint8_t* bochsvbe_lfb(void);
uint32_t bochsvbe_width(void);
uint32_t bochsvbe_height(void);
uint32_t bochsvbe_pitch(void);   // bytes por linha (= width * bytes_per_pixel)
uint32_t bochsvbe_bpp(void);
int      bochsvbe_active(void);
