#pragma once
#include <stdint.h>

// ============================================================================
//  usbhub.h — USB Hub class driver do MeuOS (FASE 13).
//
//  Espelha o usbhub.sys do Windows: class driver para hubs USB. Em um stack
//  real, o usbhub fala com cada hub fisico via URBs (GET_PORT_STATUS,
//  SET_FEATURE PORT_RESET, GET_DEVICE_DESCRIPTOR) e expoe os filhos para o
//  PnP Manager montar drivers de classe (HID, mass storage, etc).
//
//  No MeuOS stub: nao ha enumeracao real, apenas anuncio. Como o stack
//  esta vazio, contamos zero portas.
//
//  Logs em '[usbhub] ...'.
// ============================================================================

// Inicializa o hub class driver. Idempotente. Loga '[usbhub] init'.
int usbhub_init(void);

// Numero de root hubs conhecidos (igual ao numero de HCDs com porta default).
int usbhub_root_hub_count(void);
