// ============================================================================
//  acpi.c — ACPI stub do MeuOS (FASE 13).
//
//  Procura "RSD PTR " na BIOS area [0xE0000, 0xFFFFF] em passos de 16 bytes
//  (alinhamento exigido pelo spec ACPI). Se achar, le o RsdtAddress do RSDP
//  (offset 16). Sem checksum forte, sem parse das tables. Caminho seguro
//  total: o range [0..1 GiB) ja esta identidade-mapeado.
//
//  Sem AML interpreter — drivers de hardware reais que dependem de _CRS/_PRS
//  (ACPI namespace) caem em fallback sem ACPI. A FADT, MADT, HPET, etc nao
//  sao parseadas.
// ============================================================================
#include <stdint.h>
#include "acpi.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado interno -------------------------------------------------------
static int      s_initialized = 0;
static int      s_active      = 0;
static uint64_t s_rsdp_phys   = 0;
static uint64_t s_rsdt_phys   = 0;
static uint8_t  s_revision    = 0;
static char     s_oem[7]      = {0,0,0,0,0,0,0};   // 6 bytes + null

// Range BIOS classico (rsdp pode estar entre 0xE0000 e 0xFFFFF, alinhado em 16).
#define ACPI_BIOS_RANGE_START 0x000E0000ULL
#define ACPI_BIOS_RANGE_END   0x000FFFFFULL

// Match exato dos 8 bytes "RSD PTR " (espaco no fim — assinatura ACPI).
static int match_rsdp(const uint8_t* p) {
    return  p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' '
         && p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ';
}

int acpi_init(void) {
    if (s_initialized) return s_active;
    s_initialized = 1;

    kputs("[acpi] procurando RSDP em [0xE0000, 0xFFFFF] (BIOS range)...\n");

    // Varre em passos de 16 bytes (alinhamento RSDP).
    for (uint64_t addr = ACPI_BIOS_RANGE_START; addr < ACPI_BIOS_RANGE_END; addr += 16) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)addr;
        if (match_rsdp(p)) {
            s_rsdp_phys = addr;
            // RSDP layout:
            //   0..7   "RSD PTR "
            //   8      checksum
            //   9..14  OEMID (6 bytes ASCII)
            //   15     Revision (0 = ACPI 1.0, 2 = ACPI 2.0+)
            //   16..19 RsdtAddress (uint32, fisico)
            for (int i = 0; i < 6; i++) s_oem[i] = (char)p[9 + i];
            s_oem[6] = 0;
            s_revision = p[15];
            uint32_t rsdt = (uint32_t)p[16]       |
                           ((uint32_t)p[17] << 8) |
                           ((uint32_t)p[18] << 16)|
                           ((uint32_t)p[19] << 24);
            s_rsdt_phys = (uint64_t)rsdt;
            s_active = 1;

            kputs("[acpi] RSDP @ phys="); kput_hex(s_rsdp_phys); kputc('\n');
            kputs("[acpi]   OEM='"); kputs(s_oem); kputs("'\n");
            kputs("[acpi]   Revision="); kput_dec((uint64_t)s_revision);
            kputs(" RSDT phys="); kput_hex(s_rsdt_phys); kputc('\n');
            kputs("[acpi] ACPI tables nao processadas (stub — sem AML interpreter)\n");
            return 1;
        }
    }

    kputs("[acpi] RSDP nao achado no range BIOS (rodando sob -bios sem ACPI?). "
          "Caminho seguro: drivers de HW caem em modo sem ACPI.\n");
    s_active = 0;
    return 0;
}

int      acpi_active(void)    { return s_active; }
uint64_t acpi_rsdp_phys(void) { return s_rsdp_phys; }
uint64_t acpi_rsdt_phys(void) { return s_rsdt_phys; }
