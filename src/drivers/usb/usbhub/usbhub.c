// ============================================================================
//  usbhub.c — USB Hub class driver do MeuOS (FASE 13 stub).
//
//  Consulta o usbport_hcd_count() e considera cada HCD como dono de um root
//  hub. Sem enumeracao de portas reais, sem leitura de descritores. Apenas
//  loga quantos root hubs foram contabilizados.
// ============================================================================
#include <stdint.h>
#include "usbhub.h"
#include "../usbport/usbport.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_dec(uint64_t v);

static int s_initialized   = 0;
static int s_root_hub_count = 0;

int usbhub_init(void) {
    if (s_initialized) return 1;
    // Cada HCD que se registrou no usbport ganha um root hub logico.
    int hcds = usbport_hcd_count();
    s_root_hub_count = hcds;
    s_initialized = 1;
    kputs("[usbhub] init: hub class driver carregado, root hubs=");
    kput_dec((uint64_t)hcds);
    kputs(" (cada HCD anuncia 1 root hub logico)\n");
    return 1;
}

int usbhub_root_hub_count(void) { return s_root_hub_count; }
