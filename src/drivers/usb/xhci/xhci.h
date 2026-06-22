#pragma once
#include <stdint.h>

// ============================================================================
//  xhci.h — USB 3.0+ host controller stub do MeuOS (FASE 13).
//
//  Espelha o usbxhci.sys do Windows: driver de controladores xHCI (USB 3.x).
//  Class codes PCI:
//    0x0C 0x03 0x30 = xHCI (USB 3.0+)
//    0x0C 0x03 0x20 = EHCI (USB 2.0)
//    0x0C 0x03 0x10 = OHCI (USB 1.1)
//    0x0C 0x03 0x00 = UHCI (USB 1.1, Intel)
//
//  No QEMU '-device qemu-xhci' aparece como vendor=0x1B36 device=0x000D
//  class=0x0C subclass=0x03 prog_if=0x30. Detection apenas.
//
//  Sem TRBs, sem command ring, sem event ring, sem device contexts —
//  apenas log do achado e registro no usbport.
//
//  Logs em '[xhci] ...'.
// ============================================================================

// PCI class codes para serial bus / USB.
#define XHCI_PCI_CLASS         0x0C   // Serial bus controller
#define XHCI_PCI_SUBCLASS_USB  0x03   // USB controller
#define XHCI_PCI_PROGIF_UHCI   0x00
#define XHCI_PCI_PROGIF_OHCI   0x10
#define XHCI_PCI_PROGIF_EHCI   0x20
#define XHCI_PCI_PROGIF_XHCI   0x30

// Inicializa o driver xHCI. Procura no PCI por um controlador USB; se achar,
// registra no usbport. Loga o achado. Idempotente.
int xhci_init(void);

// Estado: 1 se algum controlador USB foi achado.
int xhci_active(void);

// Vendor/device do controlador achado (0 se nao achado).
uint16_t xhci_vendor_id(void);
uint16_t xhci_device_id(void);

// BAR0 fisica (registradores xHCI op/cap registers). 0 se nao achado.
uint64_t xhci_bar0(void);
