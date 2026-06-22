// ============================================================================
//  HDAudio.c — Driver Intel HD Audio (FASE 11 stub).
//
//  Procura no PCI um dispositivo de classe 0x04 (Multimedia) subclasse 0x03
//  (HD Audio). Equivalente do HDAudBus.sys do Windows na fase de PnP detection.
//
//  Sem DMA / sem CORB/RIRB / sem PCM real — esta fase apenas confirma que
//  existe um controlador de audio no barramento e expoe vendor/device + BAR0
//  para as proximas fases poderem programar streams reais (Fase 11.2+).
//
//  Tudo logado na serial ([hda] ...) para comprovar em headless.
// ============================================================================
#include <stdint.h>
#include "HDAudio.h"
#include "hal/hal.h"

// kputs / kputc / kput_hex / kput_dec vivem em src/ntos/init/main.c.
extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado do driver ----------------------------------------------------
static int      s_active    = 0;
static uint16_t s_vendor    = 0;
static uint16_t s_device    = 0;
static uint64_t s_bar0      = 0;
static uint8_t  s_subclass  = 0;

// Procura QUALQUER dispositivo class=0x04 subclass=0x03 (HD Audio) OU
// subclass=0x01 (legacy audio Sound Blaster era). Hoje o QEMU intel-hda
// expoe 0x8086:0x2668 ou 0x8086:0x293E com class=0x04 sub=0x03. Em PCs
// reais aparecem Realtek (0x10EC), Creative (0x1102), VIA (0x1106), etc.
static const hal_pci_device_t* find_audio_controller(void) {
    // 1a tentativa: HD Audio puro (class=0x04 subclass=0x03).
    const hal_pci_device_t* dev = hal_pci_find_class(HDA_PCI_CLASS,
                                                    HDA_PCI_SUBCLASS_AUDIO);
    if (dev) return dev;
    // 2a tentativa: legacy audio (subclass=0x01: SB16/AC97).
    dev = hal_pci_find_class(HDA_PCI_CLASS, HDA_PCI_SUBCLASS_LEGACY);
    return dev;
}

int hda_init(void) {
    if (s_active) return 1;     // idempotente

    kputs("[hda] procurando controlador HD Audio (PCI class=0x04 sub=0x03)...\n");
    const hal_pci_device_t* dev = find_audio_controller();
    if (!dev) {
        kputs("[hda] nenhum dispositivo de audio encontrado no PCI "
              "(class=0x04). O stack de DLLs ring 3 (mmdevapi/audioses/dsound/"
              "winmm) opera sem KMD.\n");
        return 0;
    }

    s_vendor   = dev->vendor_id;
    s_device   = dev->device_id;
    s_subclass = dev->subclass;
    // BAR0: registradores MMIO do controller (HDA spec). Pode estar marcado
    // como prefetchable + 64-bit; aqui interpretamos como faixa de 32 bits
    // (suficiente para logar; QEMU intel-hda usa BAR0 em 0xFEBF0000).
    s_bar0 = (uint64_t)(dev->bar[0] & ~0xFULL);

    kputs("[hda] PCI device class=0x04 subclass=");
    kput_hex(s_subclass);
    kputs(" (audio) achado:\n");
    kputs("[hda]   vendor:device = "); kput_hex(s_vendor); kputs(":"); kput_hex(s_device); kputc('\n');
    kputs("[hda]   bus="); kput_dec(dev->bus);
    kputs(" dev="); kput_dec(dev->device);
    kputs(" func="); kput_dec(dev->function); kputc('\n');
    kputs("[hda]   BAR0 (controller MMIO base) = "); kput_hex(s_bar0); kputc('\n');

    // Decodifica fabricante conhecido.
    if (s_vendor == 0x8086) {
        kputs("[hda]   vendor=Intel (provavel QEMU intel-hda ou ICH6/9 real).\n");
    } else if (s_vendor == 0x10EC) {
        kputs("[hda]   vendor=Realtek (provavel ALC HD Audio).\n");
    } else if (s_vendor == 0x1102) {
        kputs("[hda]   vendor=Creative (provavel Sound Blaster).\n");
    } else if (s_vendor == 0x1106) {
        kputs("[hda]   vendor=VIA Technologies.\n");
    } else if (s_vendor == 0x1274) {
        kputs("[hda]   vendor=Ensoniq (AudioPCI).\n");
    } else {
        kputs("[hda]   vendor desconhecido (mantemos so a deteccao + log).\n");
    }

    // Caminho seguro: NAO mexemos no controller (sem CORB/RIRB, sem DMA).
    // As proximas fases (11.2 PCM stream, 11.3 codec discovery) leem/escrevem
    // registradores no BAR0 mapeado via hal_map_mmio.
    kputs("[hda] Fase 11 stub: detection apenas (sem DMA, sem CORB/RIRB). "
          "As DLLs ring 3 do stack de audio funcionam sem o KMD.\n");

    s_active = 1;
    return 1;
}

int      hda_active(void)    { return s_active; }
uint16_t hda_vendor_id(void) { return s_vendor; }
uint16_t hda_device_id(void) { return s_device; }
uint64_t hda_bar0(void)      { return s_bar0; }
