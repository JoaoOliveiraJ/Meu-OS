#pragma once
#include <stdint.h>

// ============================================================================
//  smp.h — Symmetric Multi-Processing (Pilar 3 da rodada NT foundation).
//
//  Funcoes:
//   - smp_init(): parseia ACPI MADT (RSDT -> "APIC"), descobre os processadores
//     habilitados, lanca cada AP via INIT-SIPI-SIPI usando o trampoline em
//     phys 0x8000.
//   - Cada AP, ao executar a trampoline (ap_trampoline.asm) e cair em ap_entry
//     (smp.c), incrementa atomicamente s_ap_alive_count e marca s_ap_seen_id
//     com base no seu APIC ID lido do Local APIC.
//   - O BSP polls s_ap_alive_count com timeout e loga o resultado.
//
//  Referencia: Intel SDM Vol 3 8.4 (Multiple-Processor Initialization);
//  ACPI 6.5 Sec 5.2.12 (MADT). NT real usa o mesmo fluxo no HalpStartProcessor.
// ============================================================================

void     smp_init(void);
int      smp_cpu_count(void);
uint8_t  smp_apic_id_of(int cpu_index);
uint32_t smp_ap_alive_count(void);
int      smp_ap_seen(uint8_t apic_id);

// Entry C dos APs (chamada pelo ap_trampoline.asm em modo 64-bit).
void     ap_entry(void);
