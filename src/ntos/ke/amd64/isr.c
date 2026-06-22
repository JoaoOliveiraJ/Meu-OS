#include <stdint.h>
#include "ke/amd64/isr.h"
#include "ke/amd64/pic.h"
#include "input/keyboard.h"
#include "ke/amd64/syscall.h"
#include "mm/paging.h"   // FASE 7.9: mm_map_zero_page para recuperacao de PF

// Funcoes de saida definidas em kernel.c
extern void kputc(char c);
extern void kputs(const char* s);
extern void kput_hex(uint64_t v);

volatile uint64_t g_ticks = 0;

// ============================================================================
//  FASE 7.13 — single-step CPUID interception (software "mini-hipervisor").
//
//  Quando g_intercept_cpuid != 0, antes de chamar DriverEntry o driver.c liga
//  RFLAGS.TF=1 (Trap Flag). A CPU dispara #DB (int 1) DEPOIS de cada instrucao
//  executada. No handler abaixo, se a instrucao anterior foi CPUID (opcode
//  0F A2), reescrevemos EAX/EBX/ECX/EDX no 'struct regs' — quando IRETQ
//  restaurar os registradores, o driver vera os valores FAKE que escolhemos
//  em vez do que o QEMU TCG retornou. Tudo em ring 0, sem VMX/SVM.
//
//  Mascaras aplicadas:
//   - CPUID.1: zera bit 31 de ECX (HypervisorPresent)
//   - CPUID.0x40000000..0x4FFFFFFF: zera EAX/EBX/ECX/EDX (esconde TCGTCGTCGTCG)
//
//  Anti-loop NAO e necessario aqui (TF e por design: cada iret = +1 step ate
//  desligarmos TF).
// ============================================================================
volatile int g_intercept_cpuid = 0;
volatile uint64_t g_cpuid_intercepts = 0;   // contador p/ telemetria
volatile uint64_t g_rdtsc_intercepts = 0;
volatile uint64_t g_rdmsr_intercepts = 0;

// TSC fake: cresce monotonicamente em deltas pequenos (1000 "ciclos" por leitura).
// Evita revelar que estamos single-stepping (deltas reais seriam ENORMES).
static uint64_t s_fake_tsc = 0x100000ULL;

static int try_intercept(struct regs* r) {
    if (!g_intercept_cpuid) return 0;
    if (r->rip < 2) return 0;

    // RDTSCP e 3 bytes: 0F 01 F9. Checa antes de RDTSC porque os ultimos 2
    // bytes de RDTSCP sao "01 F9" que NAO conflita com nada interessante.
    if (r->rip >= 3) {
        uint8_t b0 = *(volatile uint8_t*)(uintptr_t)(r->rip - 3);
        uint8_t b1 = *(volatile uint8_t*)(uintptr_t)(r->rip - 2);
        uint8_t b2 = *(volatile uint8_t*)(uintptr_t)(r->rip - 1);
        if (b0 == 0x0F && b1 == 0x01 && b2 == 0xF9) {   // RDTSCP
            s_fake_tsc += 1000;
            r->rax = s_fake_tsc & 0xFFFFFFFFULL;
            r->rdx = s_fake_tsc >> 32;
            // RDTSCP tambem escreve em ECX (TSC_AUX/processor id) — zero.
            r->rcx = 0;
            g_rdtsc_intercepts++;
            return 1;
        }
    }

    uint16_t opc = *(volatile uint16_t*)(uintptr_t)(r->rip - 2);

    // CPUID: 0F A2
    if (opc == 0xA20F) {
        uint32_t leaf = (uint32_t)r->rax;
        // A CPU ja executou o CPUID; rax/rbx/rcx/rdx tem os valores REAIS do TCG.
        // Sobrescrevemos no frame — IRETQ restaura os fakes para o driver.
        if (leaf >= 0x40000000u && leaf <= 0x4FFFFFFFu) {
            r->rax = 0; r->rbx = 0; r->rcx = 0; r->rdx = 0;
        } else if (leaf == 1u) {
            r->rcx &= ~(1ULL << 31);   // limpa HypervisorPresent
        }
        // Loga o leaf na primeira ocorrencia de cada (para nao floodar serial).
        static uint32_t s_seen[16]; static int s_nseen = 0;
        int already = 0;
        for (int i = 0; i < s_nseen; i++) if (s_seen[i] == leaf) { already = 1; break; }
        if (!already && s_nseen < 16) {
            s_seen[s_nseen++] = leaf;
            kputs("  [intercept] CPUID leaf=0x"); kput_hex(leaf); kputs("\n");
        }
        g_cpuid_intercepts++;
        return 1;
    }

    // RDTSC: 0F 31  — devolve TSC fake (delta pequeno consistente)
    if (opc == 0x310F) {
        s_fake_tsc += 1000;
        r->rax = s_fake_tsc & 0xFFFFFFFFULL;
        r->rdx = s_fake_tsc >> 32;
        g_rdtsc_intercepts++;
        return 1;
    }

    // RDMSR: 0F 32  — loga MSR e devolve 0 (driver ve MSR nao-implementado)
    if (opc == 0x320F) {
        uint32_t msr = (uint32_t)r->rcx;
        r->rax = 0; r->rdx = 0;
        // Log so na 1a vez de cada MSR.
        static uint32_t s_msr_seen[32]; static int s_nmsr = 0;
        int already = 0;
        for (int i = 0; i < s_nmsr; i++) if (s_msr_seen[i] == msr) { already = 1; break; }
        if (!already && s_nmsr < 32) {
            s_msr_seen[s_nmsr++] = msr;
            kputs("  [intercept] RDMSR msr=0x"); kput_hex(msr); kputs(" -> 0\n");
        }
        g_rdmsr_intercepts++;
        return 1;
    }

    return 0;
}

uint64_t cpuid_intercept_count(void) { return g_cpuid_intercepts; }
uint64_t rdtsc_intercept_count(void) { return g_rdtsc_intercepts; }
uint64_t rdmsr_intercept_count(void) { return g_rdmsr_intercepts; }

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
        // FASE 7.13: #DB (int 1) gerado pelo Trap Flag (single-step). Se
        // estamos interceptando CPUID, tenta reescrever o frame e retorna.
        // CPU ja limpou TF na RFLAGS atual; a RFLAGS salvada no frame ainda
        // tem TF=1, entao o IRETQ continua o single-step.
        if (r->int_no == 1) {
            if (try_intercept(r)) return;
            // #DB nao causado por intercept conhecido (instrucao normal sob TF).
            // No-op: IRETQ continua o single-step ate desligarmos TF.
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
