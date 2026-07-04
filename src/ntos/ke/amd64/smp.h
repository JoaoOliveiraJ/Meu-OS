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

// 1 se ALGUM AP desmascarou seu LVT timer local e portanto participa do
// escalonamento preemptivo (recebe ticks 0xD1 e roda ki_quantum_end). Enquanto
// 0, so o BSP escalona — threads NAO devem ser fixadas em CPUs != 0.
int      smp_ap_timer_online(void);

// --- Worker core do AP (paralelismo real sob TCG, sem o timer local) ---------
// smp_ap_working(): 1 se o AP entrou no seu worker loop (2o core executando).
// smp_ap_heartbeat(): contador que SO avanca se o AP esta rodando instrucoes.
// smp_ap_compute(): ultimo resultado do trabalho de ALU do AP (hash LCG).
int      smp_ap_working(void);
uint64_t smp_ap_heartbeat(void);
uint64_t smp_ap_compute(void);

// Entry C dos APs (chamada pelo ap_trampoline.asm em modo 64-bit).
void     ap_entry(void);
