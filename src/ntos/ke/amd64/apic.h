#pragma once
#include <stdint.h>

// ============================================================================
//  apic.h — Local APIC + IO-APIC (Pilar 2 da rodada NT foundation).
//
//  Substitui o par 8259/PIT como fonte de tempo e roteador de IRQs. Espelha
//  a HAL APIC do NT (halacpi.dll antes do Windows 8; agora consolidado em
//  hal.dll). Os vetores escolhidos seguem o NT em x64:
//
//    APIC_VECTOR_TIMER    = 0xD1   CLOCK_VECTOR     (CLOCK_LEVEL = 13)
//    APIC_VECTOR_IPI      = 0xE1   IPI_VECTOR       (IPI_LEVEL   = 14)
//    APIC_VECTOR_SPURIOUS = 0xFF   sempre 0xFF      (Intel/AMD SDM)
//    IRQ1 (teclado)       = 0x21   mantemos o mesmo numero que PIC tinha,
//                                  para nao reescrever o keyboard_irq dispatch.
//
//  Local APIC MMIO @ 0xFEE00000  (fixo na arquitetura x86; APIC base MSR
//  pode reposicionar, nao mexemos). IO-APIC MMIO @ 0xFEC00000 no chipset
//  PIIX/Q35 do QEMU (poderia vir da MADT; lemos do default da arquitetura
//  e validamos via MADT no Pilar 3).
// ============================================================================

#define APIC_VECTOR_TIMER     0xD1u   // NT CLOCK_VECTOR
#define APIC_VECTOR_IPI       0xE1u   // NT IPI vector
#define APIC_VECTOR_SPURIOUS  0xFFu   // NT/Intel: low 4 bits = 1111b
#define APIC_VECTOR_KBD       0x21u   // mantem compatibilidade com keyboard_irq

// Inicializa o subsistema APIC:
//  1) Mapeia Local APIC (0xFEE00000) e IO-APIC (0xFEC00000) via hal_map_mmio
//     (precisa do Pilar 1 — fisico acima de 1 GiB).
//  2) Habilita o Local APIC pelo SVR (bit 8 = software enable), instalando o
//     vetor spurious 0xFF (nao precisa de EOI no spurious — SDM Vol 3 11.9).
//  3) Mascara LINT0/LINT1 (legacy, nao usamos).
//  4) Calibra o APIC timer contra o PIT que esta rodando: roda ICR=0xFFFFFFFF
//     por 100 ms (10 ticks PIT @ 100 Hz), le CCR, deriva freq do bus do APIC,
//     programa ICR para gerar APIC_VECTOR_TIMER em 100 Hz periodico.
//  5) Programa IO-APIC redirection entry de IRQ1 (teclado) -> APIC_VECTOR_KBD
//     destino CPU 0 (modo fisico), polaridade ativa-alta, edge-triggered.
//  6) Desabilita totalmente o PIC 8259 (pic_disable).
//
//  Apos retornar: g_ticks continua sendo incrementado, mas pelo APIC timer
//  (vetor 0xD1) — nao mais pelo PIT.
void apic_init(void);

// Sinaliza End-Of-Interrupt no Local APIC (escreve 0 em LAPIC[0xB0]).
// Substitui pic_eoi para IRQs vindo do IO-APIC, APIC timer e IPI.
void apic_eoi(void);

// Le e devolve o APIC ID do Local APIC corrente (bits 24..31 do registrador
// LAPIC ID em offset 0x20). Em SMP cada core devolve seu proprio ID.
uint32_t apic_local_id(void);

// 1 se apic_init ja rodou (caminho de roteamento da EOI no isr_handler).
int apic_active(void);

// Redireciona uma IRQ do IO-APIC para um vetor da CPU. Usa o registrador
// IOREDTBL[gsi] (2 dwords). Polaridade/trigger defaults seguros (ativa-alta,
// edge) — ISA IRQs sao assim. Para IRQs de PCI Express (level-trigger,
// active-low) usar uma variante futura. apic_id = destino fisico.
void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t apic_id);

// Le o APIC ID do BSP (CPU 0). Util para o Pilar 3.
uint32_t apic_bsp_id(void);

// Pilar 3 (SMP): IPI primitives — usam o Local APIC ICR (Interrupt Command
// Register) para enviar comandos entre CPUs. Os enums casam com o NT.
typedef enum {
    APIC_IPI_FIXED      = 0,   // entrega um vetor especifico
    APIC_IPI_LOWEST     = 1,   // lowest priority (raramente usado)
    APIC_IPI_SMI        = 2,
    APIC_IPI_NMI        = 4,
    APIC_IPI_INIT       = 5,   // INIT IPI
    APIC_IPI_STARTUP    = 6,   // SIPI (com 8-bit start page)
} apic_ipi_kind_t;

// Envia IPI para um APIC ID especifico (modo destinacao fisica). Bloqueia
// ate o ICR aceitar o envio (bit 12 zerado).
void apic_send_ipi(uint8_t dest_apic_id, apic_ipi_kind_t kind, uint8_t vector_or_page);

// Espera o IPI sair (Delivery Status do ICR Low = 0).
void apic_wait_ipi(void);

// Pilar 4: chamado pelo AP em ap_entry. Habilita o Local APIC corrente
// (SVR.Software_Enable + spurious vector 0xFF) e mascara LINT0/LINT1/Error.
// IMPORTANTE: o LVT Timer e' configurado MASCARADO (bit 16=1) — a contagem
// nao comeca aqui. Isto e' deliberado: enquanto o KPRCB do AP estiver sendo
// construido por ki_init_processor, NAO podemos aceitar tick algum, mesmo
// com IF=0 do lado do CPU — o LAPIC armazenando IRR ja conta como side
// effect, e sob QEMU TCG multi-thread isso correlaciona com hang determi-
// nistico do BSP. Caminho do NT: SVR ON cedo, mas LVT_TMR programado em
// HalpInitializeClock SO depois do PRCB estar montado.
void apic_enable_local(void);

// Pilar 4: desmascara o LVT Timer local + escreve TMR_INIT. Chamado pelo
// AP no FINAL do ap_entry, com o KPRCB ja completo. A partir daqui o AP
// recebe vetor 0xD1 a 100 Hz.
void apic_unmask_timer_local(void);

// QUIESCE: mascara o LVT Timer da CPU corrente (bit 16). Usado para PAUSAR o
// APIC timer do BSP apos as provas P1-P3 enquanto o scheduler MP (Pilar 4)
// esta pausado — o timer ISR (0xD1 -> mm_kuser_tick) re-entra no init pos-P3
// e trava o boot antes do desktop sob TCG (ver FUTURE.md). Reativar faz parte
// de retomar o Pilar 4.
void apic_mask_timer_local(void);
