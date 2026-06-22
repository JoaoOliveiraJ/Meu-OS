#include <stdint.h>
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "drivers/keyboard.h"
#include "ke/syscall.h"
#include "mm/paging.h"   // FASE 7.9: mm_map_zero_page para recuperacao de PF

// Funcoes de saida definidas em kernel.c
extern void kputc(char c);
extern void kputs(const char* s);
extern void kput_hex(uint64_t v);

volatile uint64_t g_ticks = 0;

static const char* exc_names[32] = {
    "Divisao por zero", "Debug", "NMI", "Breakpoint (int3)", "Overflow",
    "BOUND fora de faixa", "Opcode invalido", "Dispositivo indisponivel",
    "Double Fault", "Coprocessor Segment", "Invalid TSS", "Segmento ausente",
    "Stack-Segment Fault", "General Protection", "Page Fault", "Reservado",
    "x87 FP", "Alignment Check", "Machine Check", "SIMD FP", "Virtualizacao",
    "Control Protection", "Reservado", "Reservado", "Reservado", "Reservado",
    "Reservado", "Reservado", "Hypervisor", "VMM Comm", "Security", "Reservado"
};

// ============================================================================
//  FASE 7.9 — recuperacao de Page Fault em codigo de driver.
//
//  Heuristica: se o RIP que faultou esta NA FAIXA onde drivers .sys sao
//  carregados (>= 0x1000000 e < 0x40000000, ou seja, 16 MiB .. 1 GiB), e o
//  CR2 esta em endereco "razoavel" (NAO no kernel nucleo: NAO 0..16 MiB) e
//  o fault NAO foi causado por execucao em pagina nao mapeada (I=1), entao
//  MAPEAMOS uma pagina zerada em CR2 e RETORNAMOS — a CPU re-executa a
//  instrucao que faultou e prossegue.
//
//  Anti-loop: guardamos os ultimos N pares (rip,cr2). Se o mesmo par repete
//  3 vezes, abandonamos a recuperacao e halt (caso contrario, instrucao que
//  faulta por outro motivo entraria em loop infinito recuperando).
// ============================================================================

// Faixa onde os drivers .sys do MeuOS sao carregados.
#define DRV_LOAD_BASE   0x1000000ULL    // 16 MiB
#define DRV_LOAD_TOP    0x40000000ULL   // 1 GiB (limite da identidade)

// Limite inferior: NAO mapear zero-page no kernel-nucleo (0..16 MiB) — la mora
// o codigo do MeuOS e estruturas criticas; um fault ali e bug do kernel.
#define MIN_CR2_RECOVER 0x1000000ULL    // 16 MiB

#define PF_HIST  4
static uint64_t s_pf_hist_rip[PF_HIST];
static uint64_t s_pf_hist_cr2[PF_HIST];
static uint32_t s_pf_hist_cnt[PF_HIST];
static int      s_pf_hist_pos = 0;

// Retorna 1 se o par (rip,cr2) ja excedeu o limite de repeticao (anti-loop).
static int pf_history_seen_too_many(uint64_t rip, uint64_t cr2) {
    for (int i = 0; i < PF_HIST; i++) {
        if (s_pf_hist_rip[i] == rip && s_pf_hist_cr2[i] == cr2) {
            s_pf_hist_cnt[i]++;
            return s_pf_hist_cnt[i] >= 3;
        }
    }
    // Nao estava no historico — registra (substitui slot circular).
    s_pf_hist_rip[s_pf_hist_pos] = rip;
    s_pf_hist_cr2[s_pf_hist_pos] = cr2;
    s_pf_hist_cnt[s_pf_hist_pos] = 1;
    s_pf_hist_pos = (s_pf_hist_pos + 1) % PF_HIST;
    return 0;
}

// Tenta recuperar Page Fault. Retorna 1 se recuperou (caller deve retornar
// do isr_handler para a CPU re-executar a instrucao), 0 se nao recuperavel.
static int try_pagefault_recover(struct regs* r, uint64_t cr2) {
    // Bit 4 do err_code = I (Instruction fetch). NAO mapeamos zero-page p/
    // recuperar execucao: o codigo nao esta la. Halt.
    if (r->err_code & 0x10) return 0;

    // RIP deve estar dentro da faixa de drivers (NAO no kernel nucleo).
    if (r->rip < DRV_LOAD_BASE || r->rip >= DRV_LOAD_TOP) return 0;

    // CR2 nao pode bater no kernel nucleo.
    if (cr2 < MIN_CR2_RECOVER) return 0;

    // Anti-loop: pares (rip,cr2) repetidos demais -> abandona.
    if (pf_history_seen_too_many(r->rip, cr2)) {
        kputs("  [pf] ANTI-LOOP: mesmo (rip,cr2) faultou 3x — abandonando recovery.\n");
        return 0;
    }

    if (!mm_map_zero_page(cr2)) {
        kputs("  [pf] mm_map_zero_page FALHOU (sem RAM?) — nao recupera.\n");
        return 0;
    }

    kputs("  [pf] map-zero recovery rip=");
    kput_hex(r->rip);
    kputs(" cr2=");
    kput_hex(cr2);
    kputs("\n");
    return 1;
}

void isr_handler(struct regs* r) {
    if (r->int_no == 0x80) {            // syscall vinda do ring 3
        syscall_dispatch(r);
        return;
    }
    if (r->int_no < 32) {
        // int3 (breakpoint): tratamos como nao-fatal so para demonstrar a IDT.
        if (r->int_no == 3) {
            kputs("\n[exc] int3 capturado pela IDT — continuando normalmente.\n");
            return;
        }
        kputs("\n[EXCECAO] vetor=");
        kput_hex(r->int_no);
        kputs(" (");
        kputs(exc_names[r->int_no]);
        kputs(")  err=");
        kput_hex(r->err_code);
        kputs("  rip=");
        kput_hex(r->rip);
        // Page Fault: CR2 contem o endereco que causou o fault — essencial p/ diagnostico.
        if (r->int_no == 14) {
            uint64_t cr2; __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kputs("  cr2="); kput_hex(cr2);
            kputs("\n  err bits: P="); kput_hex(r->err_code & 1);
            kputs(" W="); kput_hex((r->err_code >> 1) & 1);
            kputs(" U="); kput_hex((r->err_code >> 2) & 1);
            kputs(" I="); kput_hex((r->err_code >> 4) & 1);
            kputs("\n");

            // FASE 7.9: tenta map-zero recovery se o fault aconteceu em codigo
            // de driver. Sucesso -> retorna SEM HALT, CPU re-executa a
            // instrucao que faultou (agora a pagina esta presente).
            if (try_pagefault_recover(r, cr2)) {
                return;     // recuperado — continua execucao do driver
            }
        }
        kputs("\nSistema parado.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // IRQs (vetores 32..47)
    uint64_t irq = r->int_no - 32;
    if (irq == 0) {
        g_ticks++;               // timer (IRQ0)
    } else if (irq == 1) {
        keyboard_irq();          // teclado (IRQ1)
    }
    pic_eoi((int)irq);           // avisa o PIC que tratamos a interrupcao
}
