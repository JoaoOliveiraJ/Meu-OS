// ============================================================================
//  usbport.c — USB Port framework do MeuOS (FASE 13 stub).
//
//  Mantem uma tabela estatica de HCDs registrados (xhci/ehci/uhci/ohci).
//  Sem URBs reais, sem schedule, sem RX/TX. Apenas anuncia a presenca do
//  framework e da slots para os HCDs anuncia-rem-se via
//  usbport_register_hcd. Logs em '[usbport] ...'.
// ============================================================================
#include <stdint.h>
#include "usbport.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado interno -------------------------------------------------------
#define USBPORT_MAX_HCDS 8

typedef struct usbport_hcd_entry {
    int         used;
    uint32_t    type;       // USB_HCD_TYPE_*
    const char* name;       // "xhci.sys", etc.
    uint64_t    bar0;       // MMIO base (apenas para log)
} usbport_hcd_entry_t;

static int                  s_initialized = 0;
static int                  s_hcd_count   = 0;
static usbport_hcd_entry_t  s_hcds[USBPORT_MAX_HCDS];

static const char* hcd_type_name(uint32_t t) {
    switch (t) {
        case USB_HCD_TYPE_UHCI: return "UHCI (USB 1.1)";
        case USB_HCD_TYPE_OHCI: return "OHCI (USB 1.1)";
        case USB_HCD_TYPE_EHCI: return "EHCI (USB 2.0)";
        case USB_HCD_TYPE_XHCI: return "xHCI (USB 3.0+)";
        default:                return "(desconhecido)";
    }
}

int usbport_init(void) {
    if (s_initialized) return 1;
    for (int i = 0; i < USBPORT_MAX_HCDS; i++) {
        s_hcds[i].used = 0;
        s_hcds[i].name = 0;
        s_hcds[i].bar0 = 0;
        s_hcds[i].type = 0;
    }
    s_hcd_count   = 0;
    s_initialized = 1;
    kputs("[usbport] init: framework USB carregado (slots HCD=");
    kput_dec(USBPORT_MAX_HCDS); kputs(")\n");
    return 1;
}

int usbport_hcd_count(void) { return s_hcd_count; }

int usbport_register_hcd(uint32_t type, const char* name, uint64_t bar0) {
    if (!s_initialized) usbport_init();
    if (s_hcd_count >= USBPORT_MAX_HCDS) {
        kputs("[usbport] register_hcd: SEM SLOT (max=");
        kput_dec(USBPORT_MAX_HCDS); kputs(")\n");
        return 0;
    }
    s_hcds[s_hcd_count].used = 1;
    s_hcds[s_hcd_count].type = type;
    s_hcds[s_hcd_count].name = name ? name : "(anon)";
    s_hcds[s_hcd_count].bar0 = bar0;
    s_hcd_count++;
    kputs("[usbport] HCD registrado: '"); kputs(name ? name : "(anon)");
    kputs("' tipo="); kputs(hcd_type_name(type));
    kputs(" BAR0="); kput_hex(bar0);
    kputs(" total="); kput_dec(s_hcd_count); kputc('\n');
    return 1;
}
