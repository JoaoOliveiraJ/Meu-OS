#pragma once
#include <stdint.h>

// ============================================================================
//  usbport.h — USB Port driver do MeuOS (FASE 13).
//
//  Espelha o usbport.sys do Windows: e a camada base do stack USB que
//  abstrai os controladores de host (xHCI/EHCI/UHCI/OHCI). Drivers de
//  controlador (xhci.sys, ehci.sys) registram-se aqui; usbhub.sys consulta
//  esta camada para enumerar dispositivos. Camadas acima:
//
//      classe (HID, mass storage, CDC)  <- usbhub  <- usbport <- HCD
//
//  Sem URBs reais, sem programar TRBs/QHs/TDs, sem PCM USB. Apenas
//  framework: registra HCDs, conta-os, expoe init idempotente.
//
//  Logs em '[usbport] ...'.
// ============================================================================

// Tipos de host controller (na ordem cronologica de aparicao no PC).
#define USB_HCD_TYPE_UHCI 1   // USB 1.1 Intel (PCI class=0x0C sub=0x03 prog=0x00)
#define USB_HCD_TYPE_OHCI 2   // USB 1.1 nao-Intel (prog=0x10)
#define USB_HCD_TYPE_EHCI 3   // USB 2.0 (prog=0x20)
#define USB_HCD_TYPE_XHCI 4   // USB 3.0+ (prog=0x30)

// Inicializa o framework USB Port. Idempotente. Loga '[usbport] init'.
int usbport_init(void);

// Quantos HCDs registrados (apos os driver inits xhci/ehci/...).
int usbport_hcd_count(void);

// Registra um HCD recem-detectado. Retorna 1 em sucesso, 0 se a tabela
// estourou (limite estatico).
int usbport_register_hcd(uint32_t type, const char* name, uint64_t bar0);
