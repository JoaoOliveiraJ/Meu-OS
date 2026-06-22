#pragma once
#include <stdint.h>

// ============================================================================
//  HDAudio.h — Driver Intel HD Audio (HDA) stub do MeuOS.
//
//  Espelha em miniatura o HDAudBus.sys + hdaudio.sys do Windows: o objetivo
//  desta FASE 11 do stack de audio e APENAS DETECTAR o controlador HD Audio
//  no PCI (vendor=0x8086 device varia, class=0x04 subclass=0x03) e logar o
//  achado na serial. Sem DMA stream, sem CORB/RIRB, sem PCM real — apenas
//  prova que existe um controlador de audio no barramento. As DLLs ring 3
//  (mmdevapi/audioses/dsound/winmm) coexistem com o stub: quando um app real
//  abre IAudioClient ou DirectSoundCreate, o caminho UMD nao chama o KMD.
//
//  Class codes PCI (regra Microsoft):
//    0x04 = Multimedia controller
//    0x04 0x00 = Multimedia video controller
//    0x04 0x01 = Multimedia audio controller (Sound Blaster era)
//    0x04 0x03 = Audio device (HD Audio)
//
//  No QEMU, '-device intel-hda' (sub-device como 'hda-output' ou 'hda-duplex')
//  cria o controlador 0x8086:0x2668 (ICH6) ou 0x8086:0x293E (ICH9). Vamos
//  detectar QUALQUER device com class=0x04 subclass=0x03 — funcoa com
//  Realtek, Creative, VIA, Intel, etc.
// ============================================================================

// HDA PCI class identifiers.
#define HDA_PCI_CLASS         0x04   // Multimedia controller
#define HDA_PCI_SUBCLASS_AUDIO 0x03  // HD Audio device
#define HDA_PCI_SUBCLASS_LEGACY 0x01 // Legacy audio (SB16/AC97)

// Inicializa o driver: enumera PCI procurando classe 0x04 subclasse 0x03 (HD
// Audio). Se achar, le BAR0 (controller MMIO base) e loga vendor:device + BAR0
// na serial. Retorna 1 se achou, 0 caso contrario. Logo a 1a chamada e
// idempotente apos isso (cache em estado interno).
int hda_init(void);

// Estado apos init: 1 se o controlador foi achado, 0 caso contrario.
int hda_active(void);

// Vendor/device do controlador achado (0 se nao achado).
uint16_t hda_vendor_id(void);
uint16_t hda_device_id(void);

// BAR0 fisica (registradores MMIO do controller). Tipicamente 0xFEBF0000 no
// QEMU intel-hda. 0 se nao achado.
uint64_t hda_bar0(void);
