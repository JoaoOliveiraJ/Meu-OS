// ============================================================================
//  xhci.c — USB host controller stub do MeuOS (FASE 13).
//
//  Procura PCI class=0x0C subclass=0x03 (USB controller) e prefere xHCI
//  (prog_if=0x30). Se nao achar, cai em EHCI/OHCI/UHCI. Sem TRBs, sem
//  programar o controlador; apenas log + registro no usbport.
//
//  No QEMU:
//    '-device qemu-xhci' -> vendor=0x1B36 device=0x000D prog_if=0x30
//    '-usb'              -> UHCI Intel PIIX3 vendor=0x8086 prog_if=0x00
// ============================================================================
#include <stdint.h>
#include "xhci.h"
#include "hal/hal.h"
#include "../usbport/usbport.h"

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
static uint8_t  s_prog_if  = 0;

static const char* progif_name(uint8_t p) {
    switch (p) {
        case XHCI_PCI_PROGIF_UHCI: return "UHCI (USB 1.1)";
        case XHCI_PCI_PROGIF_OHCI: return "OHCI (USB 1.1)";
        case XHCI_PCI_PROGIF_EHCI: return "EHCI (USB 2.0)";
        case XHCI_PCI_PROGIF_XHCI: return "xHCI (USB 3.0+)";
        default:                   return "(USB prog_if desconhecido)";
    }
}

static uint32_t progif_to_hcd_type(uint8_t p) {
    switch (p) {
        case XHCI_PCI_PROGIF_UHCI: return USB_HCD_TYPE_UHCI;
        case XHCI_PCI_PROGIF_OHCI: return USB_HCD_TYPE_OHCI;
        case XHCI_PCI_PROGIF_EHCI: return USB_HCD_TYPE_EHCI;
        case XHCI_PCI_PROGIF_XHCI: return USB_HCD_TYPE_XHCI;
        default:                   return USB_HCD_TYPE_XHCI;
    }
}

int xhci_init(void) {
    if (s_active) return 1;     // idempotente

    kputs("[xhci] procurando controlador USB (PCI class=0x0C sub=0x03)...\n");

    // Itera TODOS os USB controllers — prefere xHCI > EHCI > OHCI > UHCI.
    const hal_pci_device_t* best = 0;
    int best_score = -1;
    int total = hal_pci_count();
    int usb_count = 0;
    for (int i = 0; i < total; i++) {
        const hal_pci_device_t* d = hal_pci_get(i);
        if (!d) continue;
        if (d->class_code == XHCI_PCI_CLASS && d->subclass == XHCI_PCI_SUBCLASS_USB) {
            usb_count++;
            int score = 0;
            switch (d->prog_if) {
                case XHCI_PCI_PROGIF_XHCI: score = 4; break;
                case XHCI_PCI_PROGIF_EHCI: score = 3; break;
                case XHCI_PCI_PROGIF_OHCI: score = 2; break;
                case XHCI_PCI_PROGIF_UHCI: score = 1; break;
                default:                   score = 0; break;
            }
            if (score > best_score) {
                best = d;
                best_score = score;
            }
        }
    }
    if (!best) {
        kputs("[xhci] USB 3.0 controller nao detectado "
              "(adicione '-device qemu-xhci' no run.ps1)\n");
        return 0;
    }

    s_vendor  = best->vendor_id;
    s_device  = best->device_id;
    s_bus     = best->bus;
    s_dev_num = best->device;
    s_func    = best->function;
    s_prog_if = best->prog_if;
    // BAR0: registradores MMIO (cap/op). Mascaramos bits baixos (tipo + prefetch).
    s_bar0    = (uint64_t)(best->bar[0] & ~0xFULL);

    kputs("[xhci] USB ");
    kputs(progif_name(s_prog_if));
    kputs(" controller @ PCI ");
    kput_dec(s_bus); kputc(':'); kput_dec(s_dev_num); kputc('.'); kput_dec(s_func);
    kputc('\n');
    kputs("[xhci]   vendor:device = ");
    kput_hex((uint64_t)s_vendor); kputc(':'); kput_hex((uint64_t)s_device); kputc('\n');
    kputs("[xhci]   BAR0 = "); kput_hex(s_bar0); kputc('\n');
    if (usb_count > 1) {
        kputs("[xhci]   (mais de 1 USB controller achado: total=");
        kput_dec(usb_count); kputs("; usando o de maior versao)\n");
    }

    // Caminho seguro: registra no usbport (sem mexer no MMIO).
    usbport_register_hcd(progif_to_hcd_type(s_prog_if),
                         (s_prog_if == XHCI_PCI_PROGIF_XHCI) ? "xhci.sys" :
                         (s_prog_if == XHCI_PCI_PROGIF_EHCI) ? "ehci.sys" :
                         (s_prog_if == XHCI_PCI_PROGIF_OHCI) ? "ohci.sys" :
                                                               "uhci.sys",
                         s_bar0);

    kputs("[xhci] Fase 13 stub: detection apenas (sem TRBs, sem command ring).\n");

    s_active = 1;
    return 1;
}

int      xhci_active(void)    { return s_active; }
uint16_t xhci_vendor_id(void) { return s_vendor; }
uint16_t xhci_device_id(void) { return s_device; }
uint64_t xhci_bar0(void)      { return s_bar0; }
