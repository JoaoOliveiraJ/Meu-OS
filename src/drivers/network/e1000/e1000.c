// ============================================================================
//  e1000.c — Driver Intel 8254x (E1000) stub do MeuOS (Fase 12).
//
//  Detection apenas: procura no PCI class=0x02 sub=0x00 (Ethernet) e prefere
//  Intel 0x8086 device 0x100E (82540EM-A, default do '-device e1000' do QEMU).
//
//  Sem DMA, sem rings TDR/TDH/RDH/RDT, sem programar o MAC, sem ler EEPROM.
//  Esta fase prova que existe uma NIC compativel; as proximas (12.2+) programam
//  o controlador real e plugam na NDIS (que ja existe como framework, Fase 12.1).
//
//  Logs em '[e1000] ...' — todas as chamadas comprovadas em headless.
// ============================================================================
#include <stdint.h>
#include "e1000.h"
#include "hal/hal.h"
#include "../ndis/ndis.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado do driver ----------------------------------------------------
static int      s_active   = 0;
static uint16_t s_vendor   = 0;
static uint16_t s_device   = 0;
static uint64_t s_bar0     = 0;
static uint8_t  s_bus      = 0;
static uint8_t  s_dev_num  = 0;
static uint8_t  s_func     = 0;

// Decodifica o device id Intel em string informativa.
static const char* intel_e1000_model(uint16_t devid) {
    switch (devid) {
        case 0x100E: return "82540EM-A (default QEMU)";
        case 0x100F: return "82545EM";
        case 0x10D3: return "82574L (Gigabit)";
        case 0x1004: return "82543GC";
        case 0x1019: return "82547EI";
        case 0x101A: return "82547EI Mobile";
        case 0x1010: return "82546EB-Copper";
        case 0x1011: return "82545EM-Fiber";
        case 0x1012: return "82546EB-Fiber";
        case 0x1013: return "82541EI";
        case 0x1015: return "82540EP-A";
        case 0x1016: return "82540EP-A Mobile";
        case 0x1017: return "82540EP";
        default:     return "Intel ethernet (modelo desconhecido)";
    }
}

int e1000_init(void) {
    if (s_active) return 1;  // idempotente

    kputs("[e1000] procurando NIC Ethernet (PCI class=0x02 sub=0x00)...\n");

    // Itera TODOS os Ethernet controllers — prefere Intel vendor 0x8086.
    const hal_pci_device_t* best = 0;
    int total_count = hal_pci_count();
    int eth_count = 0;
    for (int i = 0; i < total_count; i++) {
        const hal_pci_device_t* d = hal_pci_get(i);
        if (!d) continue;
        if (d->class_code == E1000_PCI_CLASS && d->subclass == E1000_PCI_SUBCLASS) {
            eth_count++;
            if (d->vendor_id == E1000_VENDOR_INTEL) {
                best = d;
                break;
            }
            if (!best) best = d;
        }
    }
    if (!best) {
        kputs("[e1000] nenhum Ethernet controller no PCI. "
              "(adicione '-device e1000' no run.ps1)\n");
        return 0;
    }

    s_vendor  = best->vendor_id;
    s_device  = best->device_id;
    s_bus     = best->bus;
    s_dev_num = best->device;
    s_func    = best->function;
    // BAR0: registradores MMIO do controller. Pode estar marcado com bits
    // baixos (prefetchable + 64-bit indicador); mascaramos.
    s_bar0    = (uint64_t)(best->bar[0] & ~0xFULL);

    kputs("[e1000] Ethernet controller achado em PCI ");
    kput_dec(s_bus); kputc(':'); kput_dec(s_dev_num); kputc('.'); kput_dec(s_func); kputc('\n');
    kputs("[e1000]   vendor:device = ");
    kput_hex(s_vendor); kputc(':'); kput_hex(s_device); kputc('\n');
    if (s_vendor == E1000_VENDOR_INTEL) {
        kputs("[e1000]   modelo Intel: "); kputs(intel_e1000_model(s_device)); kputc('\n');
        kputs("[e1000] Intel 82540EM detectado bus="); kput_dec(s_bus);
        kputs(" dev="); kput_dec(s_dev_num);
        kputs(" func="); kput_dec(s_func);
        kputs(" BAR0="); kput_hex(s_bar0); kputc('\n');
    } else if (s_vendor == 0x10EC) {
        kputs("[e1000]   vendor=Realtek (provavel RTL8139/8169)\n");
    } else if (s_vendor == 0x14E4) {
        kputs("[e1000]   vendor=Broadcom\n");
    } else {
        kputs("[e1000]   vendor desconhecido — mantendo so log de detection\n");
    }
    if (eth_count > 1) {
        kputs("[e1000]   (mais de 1 NIC achada: total="); kput_dec(eth_count); kputs(")\n");
    }

    // Caminho seguro: registramos como miniport NDIS (so anuncio) e paramos.
    // Sem MMIO ao BAR0, sem programar TX/RX. Em apps reais, ws2_32 funciona em
    // paralelo (sockets fakes que devolvem 0 bytes em recv).
    ndis_register_miniport("e1000.sys", (void*)(uintptr_t)s_bar0);

    kputs("[e1000] Fase 12 stub: detection apenas (sem MMIO, sem rings). "
          "ndis.sys e tcpip.sys carregados; ws2_32 disponivel em ring 3.\n");

    s_active = 1;
    return 1;
}

int      e1000_active(void)    { return s_active; }
uint16_t e1000_vendor_id(void) { return s_vendor; }
uint16_t e1000_device_id(void) { return s_device; }
uint64_t e1000_bar0(void)      { return s_bar0; }
