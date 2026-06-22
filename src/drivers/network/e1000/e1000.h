#pragma once
#include <stdint.h>

// ============================================================================
//  e1000.h — Driver Intel 8254x (E1000) stub do MeuOS (Fase 12).
//
//  Espelha o e1000.sys do Windows na fase de detection. QEMU expoe esta NIC
//  com '-device e1000' ou '-net nic,model=e1000':
//
//    PCI vendor=0x8086 device=0x100E (82540EM, modelo Intel desktop default)
//    Class=0x02 (Network controller) subclass=0x00 (Ethernet)
//    BAR0: registradores MMIO; BAR1: I/O space; BAR6: ROM opcional
//
//  Esta fase APENAS detecta no PCI, le BAR0 e loga vendor:device + bus/dev/func.
//  Sem MMIO real, sem rings de TX/RX, sem MAC programado. Quando uma stack
//  real for adicionada (Fase 12.2+), os ringer descriptors viram em BAR0.
// ============================================================================

// Class codes PCI (regra Microsoft):
//   0x02 0x00 = Ethernet controller
//   0x02 0x01 = Token Ring
//   0x02 0x02 = FDDI
//   0x02 0x80 = Other Network controller
#define E1000_PCI_CLASS         0x02
#define E1000_PCI_SUBCLASS      0x00

// Intel vendor + device ids cobertos.
#define E1000_VENDOR_INTEL      0x8086
#define E1000_DEVICE_82540EM    0x100E  // Intel 82540EM-A (default QEMU)
#define E1000_DEVICE_82545EM    0x100F  // Intel 82545EM
#define E1000_DEVICE_82574L     0x10D3  // Intel 82574L (Gigabit)

// Inicializa o driver: enumera PCI, acha class=0x02 sub=0x00, prefere Intel
// (vendor=0x8086 device=0x100E). Loga vendor:device + bus/dev/func + BAR0.
// Idempotente. Retorna 1 se achou, 0 caso contrario.
int e1000_init(void);

// Estado apos init.
int      e1000_active(void);
uint16_t e1000_vendor_id(void);
uint16_t e1000_device_id(void);
uint64_t e1000_bar0(void);
