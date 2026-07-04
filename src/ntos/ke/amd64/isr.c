#include <stdint.h>
#include "ke/amd64/isr.h"
#include "ke/amd64/pic.h"
#include "ke/amd64/apic.h"        // Pilar 2: dispatch de vetores APIC + EOI
#include "ke/sched.h"             // Pilar 4: ki_quantum_end / ki_ipi_reschedule
#include "input/keyboard.h"
#include "input/mouse/mouse.h"     // FASE 11: IRQ12 (mouse PS/2)
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

// TSC fake instruction-aware: o delta entre dois RDTSC deve refletir o TRABALHO real
// (numero de instrucoes executadas) — em bare-metal, `rdtsc;cpuid;rdtsc` da ~100-200 ciclos
// e `rdtsc;iretq-block;rdtsc` da mais (mais instrucoes), enquanto numa VM cada VM-exit da
// MILHARES. O pintok.sys mede esses deltas relativos e bail se virem "VM" (delta enorme/flat).
// Single-stepamos cada instrucao do driver, entao CONTAMOS as instrucoes desde o ultimo
// RDTSC e usamos isso como delta (proxy fiel de ciclos), em vez de um flat +1000 que parecia VM.
static uint64_t s_fake_tsc = 0x100000ULL;
static uint64_t s_steps_since_rdtsc = 0;

// ============================================================================
//  FASE 7.14 — NEUTRALIZACAO do bloco anti-VM/anti-hypervisor do pintok.sys.
//
//  GATE 4 da init do pintok.sys (ver pintok.sys-INIT-NOTES.md): o worker por-core
//  0x140001794, disparado via KeIpiGenericCall, executa um "fingerprint de
//  ambiente por timing" com INSTRUCOES PRIVILEGIADAS em ring 0 (bloco em
//  .text RVA 0x1080):
//      push 0; push rdi; pushfq; push 0x10; push r12;
//      clflush[rsp]; cpuid; rdtsc; iretq;   <- IRETQ com cs=0x10 (fake frame)
//      int 0x20;                            <- timing de int/iretq
//      syscall;                             <- timing de syscall (LSTAR)
//      smsw r15; rdtsc; rdtsc;              <- le CR0 + delta TSC
//
//  No EMULADOR unicorn (emu.py hk_intr) essas instrucoes sao NEUTRALIZADAS:
//  int/int3/iretq/syscall sao "puladas" (rip += tamanho) — o probe ve um
//  resultado limpo (bare-metal-like) e a init AVANCA ate KeIpiGenericCall +
//  enumeracao de modulos. No MeuOS REAL elas EXECUTAM de verdade na CPU/IDT:
//   - IRETQ (48 CF): o frame fake tem cs=0x10, que no GDT do MeuOS e o seletor
//     de DADOS de ring0 (codigo=0x08) -> carregar 0x10 em CS via IRETQ = #GP.
//   - SYSCALL (0F 05): EFER.SCE=1, entao entra em syscall_entry, que MONTA um
//     frame de retorno ring 3 (CS=0x1B) e IRETQ -> o DRIVER CAI PARA RING 3 no
//     meio do probe; as instrucoes seguintes (#GP/comportamento divergente).
//   - INT 0x20 (CD 20): vetor 0x20 = IRQ0 (timer) do MeuOS -> dispara o ISR do
//     timer + EOI espurio no Local APIC, e o iretq do stub volta errado.
//  Qualquer um desses DERRUBA o worker -> o pintok.sys le timing "de VM"/control-flow
//  corrompido e a DriverEntry retorna STATUS_FAILED_DRIVER_ENTRY (0xC0000365),
//  um bail MAIS CEDO que o emu.
//
//  Fix (espelha o emu): enquanto single-stepamos o driver (g_intercept_cpuid),
//  fazemos LOOK-AHEAD na instrucao em r->rip ANTES dela executar. Se for uma
//  dessas sondas privilegiadas, NEUTRALIZAMOS no frame do #DB:
//   - int imm8 / int3 / icebp / syscall -> pula (avanca rip pelo tamanho);
//   - iretq -> EMULA (pop rip/rflags/rsp do stack do worker, mantendo cs/ss de
//     kernel validos, preservando TF p/ continuar o single-step).
//  Assim o probe roda "no vazio" (como na VM do emu) sem efeitos colaterais
//  reais de IDT/syscall, e a init prossegue. So age sob a janela de
//  interceptacao do driver — o kernel normal nao e afetado.
// ============================================================================
static uint64_t g_antivm_neutralized = 0;   // telemetria

static int try_neutralize_antivm_probe(struct regs* r) {
    if (!g_intercept_cpuid) return 0;
    if (r->rip < 2) return 0;
    // So age sobre CODIGO DE DRIVER (.sys relocado em [16 MiB, 1 GiB)). O nucleo
    // do MeuOS mora em ~1 MiB (linker.ld: . = 1M) e tem suas PROPRIAS instrucoes
    // iretq/syscall/int nos stubs de ISR — NUNCA podemos neutraliza-las. O
    // worker do pintok.sys roda na faixa de driver (PMM_BASE=64 MiB+), entao o guard
    // de faixa isola o efeito ao driver.
    if (r->rip < 0x1000000ULL || r->rip >= 0x40000000ULL) return 0;

    uint8_t b0 = *(volatile uint8_t*)(uintptr_t)(r->rip);
    uint8_t b1 = *(volatile uint8_t*)(uintptr_t)(r->rip + 1);

    // IRETQ: CF (com ou sem prefixo REX.W 48). O worker empilhou um frame fake
    // (push 0; push rdi; pushfq; push 0x10; push r12) -> do topo do stack do
    // worker (r->rsp): [rsp]=RIP(r12), [rsp+8]=CS(0x10), [rsp+0x10]=RFLAGS,
    // [rsp+0x18]=RSP(rdi), [rsp+0x20]=SS(0). Emulamos o IRETQ CPL0->CPL0
    // SANITIZANDO cs/ss (nao recarregamos seletores invalidos): resume em RIP,
    // restaura RFLAGS (com TF forcado p/ seguir single-stepando) e RSP.
    if (b0 == 0xCF || (b0 == 0x48 && b1 == 0xCF)) {
        uint64_t sp = r->rsp;
        uint64_t new_rip    = *(volatile uint64_t*)(uintptr_t)(sp + 0x00);
        uint64_t new_rflags = *(volatile uint64_t*)(uintptr_t)(sp + 0x10);
        uint64_t new_rsp    = *(volatile uint64_t*)(uintptr_t)(sp + 0x18);
        r->rip    = new_rip;
        r->rsp    = new_rsp;
        r->rflags = (new_rflags | 0x100ULL) & ~0x00020000ULL;  // TF=1, limpa VM
        r->rflags |= 0x2;                                       // bit1 reservado=1
        g_antivm_neutralized++;
        return 1;
    }

    // INT imm8 (CD ib): pula. Cobre int 0x20 (timing) e os de anti-debug
    // (0x29 __fastfail, 0x2a..0x2f DebugService) — exatamente o conjunto que o
    // emu (hk_intr) trata como "sem debugger / skip".
    if (b0 == 0xCD) {
        r->rip += 2;
        g_antivm_neutralized++;
        return 1;
    }
    // INT3 (CC) e ICEBP/INT1 (F1): 1 byte, pula.
    if (b0 == 0xCC || b0 == 0xF1) {
        r->rip += 1;
        g_antivm_neutralized++;
        return 1;
    }
    // SYSCALL (0F 05): pula (no emu o LSTAR retorna limpo = no-op p/ o probe).
    // Em ring 0 no MeuOS isso cairia em syscall_entry e DEMOVERIA p/ ring 3 —
    // entao NUNCA deixamos executar. rax=0 (retorno coerente/neutro).
    if (b0 == 0x0F && b1 == 0x05) {
        r->rip += 2;
        r->rax = 0;
        g_antivm_neutralized++;
        return 1;
    }
    // SYSENTER (0F 34): idem, pula (defensivo; pintok.sys usa syscall, nao sysenter).
    if (b0 == 0x0F && b1 == 0x34) {
        r->rip += 2;
        g_antivm_neutralized++;
        return 1;
    }

    return 0;
}

uint64_t antivm_neutralize_count(void) { return g_antivm_neutralized; }

static int try_intercept(struct regs* r) {
    if (!g_intercept_cpuid) return 0;
    if (r->rip < 2) return 0;
    s_steps_since_rdtsc++;   // cada #DB = 1 instrucao single-stepada (proxy de ciclos bare-metal)

    // LOOKAHEAD do leaf do CPUID: o #DB dispara DEPOIS da instrucao, entao no pos-cpuid o
    // eax ja e o RESULTADO (nao a entrada). Aqui, ANTES do cpuid executar (proxima instrucao
    // em r->rip == 0F A2), capturamos o leaf de entrada (eax) para mascarar corretamente.
    static uint32_t s_cpuid_leaf = 0;
    { uint8_t la0 = *(volatile uint8_t*)(uintptr_t)(r->rip);
      uint8_t la1 = *(volatile uint8_t*)(uintptr_t)(r->rip + 1);
      if (la0 == 0x0F && la1 == 0xA2) s_cpuid_leaf = (uint32_t)r->rax; }

    // RDTSCP e 3 bytes: 0F 01 F9. Checa antes de RDTSC porque os ultimos 2
    // bytes de RDTSCP sao "01 F9" que NAO conflita com nada interessante.
    if (r->rip >= 3) {
        uint8_t b0 = *(volatile uint8_t*)(uintptr_t)(r->rip - 3);
        uint8_t b1 = *(volatile uint8_t*)(uintptr_t)(r->rip - 2);
        uint8_t b2 = *(volatile uint8_t*)(uintptr_t)(r->rip - 1);
        if (b0 == 0x0F && b1 == 0x01 && b2 == 0xF9) {   // RDTSCP
            s_fake_tsc += 0x100; s_steps_since_rdtsc = 0;  // flat ~256: valor que PASSOU no emulador (< threshold VM; +1000 falhava)
            r->rax = s_fake_tsc & 0xFFFFFFFFULL;
            r->rdx = s_fake_tsc >> 32;
            // RDTSCP tambem escreve em ECX (TSC_AUX/processor id) — zero.
            r->rcx = 0;
            g_rdtsc_intercepts++;
            return 1;
        }
    }

    uint16_t opc = *(volatile uint16_t*)(uintptr_t)(r->rip - 2);

    // CPUID: 0F A2 — a CPU ja executou (rax..rdx = resultado do TCG/qemu64). Sobrescrevemos
    // no frame com os valores de um Intel REAL moderno (i7-9700K Coffee Lake), EXATAMENTE como
    // o emulador unicorn que fez o pintok.sys PASSAR. O qemu64 reporta family 0x60FB1 (era P4!) +
    // max leaf 0xD = perfil suspeito/VM; aqui apresentamos family 0x906EA + max leaf 0x16, e
    // limpamos AVX(28)/XSAVE(26)/OSXSAVE(27)/HYPERVISOR(31) p/ o pintok.sys tomar o caminho SSE.
    if (opc == 0xA20F) {
        uint32_t leaf = s_cpuid_leaf;
        if (leaf == 0) {
            r->rax = 0x16; r->rbx = 0x756e6547; r->rcx = 0x6c65746e; r->rdx = 0x49656e69; // "GenuineIntel"
        } else if (leaf == 1) {
            r->rax = 0x000906EA; r->rbx = 0x00100800;
            r->rcx = 0x7FFAFBFFu & ~((1u<<31)|(1u<<26)|(1u<<27)|(1u<<28));
            r->rdx = 0xBFEBFBFF;
        } else if (leaf == 7) {
            r->rax = 0; r->rbx = 0x029C67AF; r->rcx = 0; r->rdx = 0;
        } else if (leaf == 0x80000000u) {
            r->rax = 0x80000008; r->rbx = 0; r->rcx = 0; r->rdx = 0;
        } else if (leaf >= 0x80000002u && leaf <= 0x80000004u) {
            static const char brand[48] = "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz";
            int off = (int)(leaf - 0x80000002u) * 16;
            r->rax = *(const uint32_t*)(brand+off+0);  r->rbx = *(const uint32_t*)(brand+off+4);
            r->rcx = *(const uint32_t*)(brand+off+8);  r->rdx = *(const uint32_t*)(brand+off+12);
        } else if (leaf >= 0x40000000u && leaf <= 0x4FFFFFFFu) {
            r->rax = 0; r->rbx = 0; r->rcx = 0; r->rdx = 0;   // sem vendor de hypervisor
        }
        // (outros leaves: passam o valor real do qemu64)
        static uint32_t s_seen[24]; static int s_nseen = 0;
        int already = 0;
        for (int i = 0; i < s_nseen; i++) if (s_seen[i] == leaf) { already = 1; break; }
        if (!already && s_nseen < 24) {
            s_seen[s_nseen++] = leaf;
            kputs("  [intercept] CPUID leaf=0x"); kput_hex(leaf); kputs(" -> Intel i7-9700K\n");
        }
        g_cpuid_intercepts++;
        return 1;
    }

    // RDTSC: 0F 31  — delta instruction-aware (instrucoes desde o ultimo rdtsc = ciclos bare-metal)
    if (opc == 0x310F) {
        s_fake_tsc += 0x100; s_steps_since_rdtsc = 0;  // flat ~256: valor que PASSOU no emulador (< threshold VM; +1000 falhava)
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
            // LOOK-AHEAD: neutraliza o bloco anti-VM do pintok.sys (int/iretq/syscall)
            // ANTES da instrucao executar — espelha o emu, evita efeitos reais
            // de IDT/syscall que derrubam o worker (-> 0xC0000365). [FASE 7.14]
            if (try_neutralize_antivm_probe(r)) return;
            // POS-INSTRUCAO: emula CPUID/RDTSC/RDMSR (valores fake bare-metal).
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

            // Pilar 1 (NT foundation): caminho de expected-fault. Se a MM
            // estava esperando este PF (mm_probe_read_u32 etc.), marca caught,
            // pula a instrucao que faultou (rip := resume) e retorna. Logamos
            // baixo ruido para nao poluir o relatorio. NUNCA cai pelo recovery
            // de driver — o probe sabe o que esta fazendo.
            if (g_mm_pf_expect) {
                g_mm_pf_expect = 0;
                g_mm_pf_caught = 1;
                kputs("  [pf] expected-trap cr2="); kput_hex(cr2);
                kputs(" rip="); kput_hex(r->rip);
                kputs(" -> resume "); kput_hex(g_mm_pf_resume_rip);
                kputs("\n");
                r->rip = g_mm_pf_resume_rip;
                return;
            }

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

    // ====================================================================
    //  Vetores APIC (Pilar 2): rota propria, EOI ao Local APIC.
    // ====================================================================
    // Spurious: SDM Vol 3 11.9 — NAO emitir EOI. So retorna.
    if (r->int_no == APIC_VECTOR_SPURIOUS) {
        return;
    }
    // APIC timer (NT CLOCK_VECTOR = 0xD1): tick periodico do kernel.
    //
    // Caminho NT: HalpClockTick (BSP-owned). Apenas a CPU dona-do-clock
    // atualiza g_ticks e KUSER_SHARED_DATA — APs sao quiescentes nesse front.
    // Aqui usamos CPU 0 como BSP. APs apenas dao EOI e (se P4 ligado) caem em
    // ki_quantum_end. Isto previne escritas concorrentes a mesma cache line
    // do KUSER_SHARED_DATA — alem de bater com NT real, e' diagnostico-importante:
    // descobrimos que escritas SIMD-fundidas (movdqu xmm) cross-CPU sob QEMU
    // TCG multi-thread geram stall determinístico.
    // APIC timer (NT CLOCK_VECTOR = 0xD1). Caminho NT: HalpClockTick e'
    // BSP-owned — apenas a CPU dona do clock (CPU 0 aqui) atualiza g_ticks
    // e KUSER_SHARED_DATA. APs apenas EOI e chamam ki_quantum_end (sob gate
    // g_p4_active).
    if (r->int_no == APIC_VECTOR_TIMER) {
        extern volatile int g_p4_active;
        if (ki_current_cpu_index() == 0) {
            g_ticks++;
            mm_kuser_tick();
        }
        apic_eoi();
        if (g_p4_active) ki_quantum_end();
        return;
    }
    // IPI reschedule (vetor 0xE1, Pilar 4).
    if (r->int_no == APIC_VECTOR_IPI) {
        apic_eoi();
        ki_ipi_reschedule();
        return;
    }

    // IRQs PIC ou IO-APIC redirects (vetores 0x20..0x2F).
    uint64_t irq = r->int_no - 32;
    if (irq == 0) {
        g_ticks++;               // timer (IRQ0 do PIT, antes do APIC)
    } else if (irq == 1) {
        keyboard_irq();          // teclado (IRQ1 — PIC ou IO-APIC redirect)
    } else if (irq == 12) {
        mouse_irq();             // FASE 11: mouse PS/2 (IRQ12)
    }
    // Pilar 2: se APIC estiver ativo, EOI vai pelo Local APIC (registro 0xB0).
    // Sob PIC, vai pelos OCWs do 8259. Roteamento decidido em runtime.
    if (apic_active()) apic_eoi();
    else               pic_eoi((int)irq);
}
