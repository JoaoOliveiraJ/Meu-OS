#include <stdint.h>
#include "ke/amd64/usermode.h"
#include "ke/amd64/syscall.h"
#include "ke/amd64/kpcr.h"
#include "ke/amd64/gdt.h"       // tss_set_rsp0 (pilha de kernel da thread ring-3)
#include "ke/sched.h"           // ki_create_thread / ki_ready_thread / ki_thread_t (threads ring-3 preemptivas)
#include "hal/cpu.h"            // HalWriteMsr / MSR_IA32_GS_BASE
#include "mm/paging.h"
#include "mm/pmm.h"              // pmm_alloc_contiguous (pilha do worker ring-3)
#include "ps/process.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);

// Região de RAM (identidade) usada como pilha de modo usuário: [6 MiB, 7 MiB).
// Fica fora do kernel, então o ring 3 nunca enxerga a memória do kernel.
#define USTACK_BASE 0x600000ULL
#define USTACK_SIZE 0x100000ULL
#define USTACK_TOP  (USTACK_BASE + USTACK_SIZE)

// FRENTE 3 (Fase 3a) — TEB/PEB do processo ring-3. Um .exe REAL do Windows (e o
// CRT dele) le o TEB via gs:[..] logo no arranque: gs:[0x30]=TEB.Self,
// gs:[0x60]=PEB, e PEB+0x10=ImageBaseAddress. Colocamos o TEB e o PEB no FUNDO da
// janela de pilha (ja user-mapped [0x600000,0x700000)); a pilha desce de USTACK_TOP
// e nunca alcanca estes 2 KiB do fundo nas nossas apps. Sem colisao com ImageBases
// de app (nenhum usa a faixa 0x600000) nem com a regiao do PMM (>= 0x4000000).
#define TEB_ADDR      (USTACK_BASE)              // 0x600000
#define PEB_ADDR      (USTACK_BASE + 0x1000ULL)  // 0x601000
#define USTACK_LIMIT  (USTACK_BASE + 0x2000ULL)  // fundo util da pilha (acima do TEB/PEB)

// FRENTE 4 (Fase 4c) — linha de comando do processo (argv). O sys_createprocess seta
// esta string com a cmdline do filho antes de rodar a imagem; o build_teb_peb a escreve
// em PEB+0x800 (0x601800), e o CRT (ucrtbase) le/parseia em argv la'. Vazia p/ apps de
// boot (default "crthello.exe"). E' um global de kernel; nao esta na janela salva/restaurada.
static char g_proc_cmdline[256] = {0};
void usermode_set_cmdline(const char* s) {
    if (!s) { g_proc_cmdline[0] = 0; return; }
    int i = 0; while (s[i] && i < 255) { g_proc_cmdline[i] = s[i]; i++; }
    g_proc_cmdline[i] = 0;
}

// Monta um TEB + PEB minimos no fundo da janela de pilha. load_base = ImageBase
// REAL de carga da imagem (apos relocacao). Escreve direto na RAM identidade
// (mesmo frame fisico que a app enxerga no CR3 dela — a faixa baixa e clonada).
static void build_teb_peb(uint64_t load_base) {
    volatile uint8_t* t = (volatile uint8_t*)(uintptr_t)TEB_ADDR;
    volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)PEB_ADDR;
    for (int i = 0; i < 0x1000; i++) { t[i] = 0; p[i] = 0; }
    // TEB (layout NT x64):
    *(volatile uint64_t*)(uintptr_t)(TEB_ADDR + 0x08) = USTACK_TOP;    // NtTib.StackBase
    *(volatile uint64_t*)(uintptr_t)(TEB_ADDR + 0x10) = USTACK_LIMIT;  // NtTib.StackLimit
    *(volatile uint64_t*)(uintptr_t)(TEB_ADDR + 0x30) = TEB_ADDR;      // NtTib.Self  -> gs:[0x30]
    *(volatile uint64_t*)(uintptr_t)(TEB_ADDR + 0x60) = PEB_ADDR;      // ProcessEnvironmentBlock -> gs:[0x60]
    *(volatile uint64_t*)(uintptr_t)(TEB_ADDR + 0x68) = 0;             // LastErrorValue
    // PEB (subset): o que o CRT le primeiro.
    *(volatile uint64_t*)(uintptr_t)(PEB_ADDR + 0x10) = load_base;     // ImageBaseAddress
    // Fase 4c: linha de comando em PEB+0x800 (0x601800), lida pelo CRT p/ montar argv.
    // Vazio -> o CRT usa o default. A pilha desce de 0x700000 e nunca alcanca aqui.
    if (g_proc_cmdline[0]) {
        volatile char* c = (volatile char*)(uintptr_t)(PEB_ADDR + 0x800);
        int i = 0; while (g_proc_cmdline[i] && i < 255) { c[i] = g_proc_cmdline[i]; i++; }
        c[i] = 0;
    }
}

// Troca para ring 3 montando um frame de iretq (SS, RSP, RFLAGS, CS, RIP).
static void enter_ring3(void (*entry)(void), uint64_t stack_top, uint64_t teb) {
    __asm__ volatile (
        "movw $0x23, %%ax\n"               // UDATA | RPL 3 nos registradores de dados
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
        // O 'movw %%ax,%%gs' acima zerou a base de gs (descritor flat). Setamos
        // IA32_GS_BASE = TEB AQUI (apos o load do seletor, antes do iretq — o iretq
        // p/ ring 3 nao toca na base de gs). Assim a app le o TEB por gs:[0x30] /
        // o PEB por gs:[0x60], igual ao Windows. KERNEL_GS_BASE (0xC0000102) fica
        // intocado apontando p/ o KPCR (sem swapgs assimetrico no kernel).
        "movl $0xC0000101, %%ecx\n"        // IA32_GS_BASE
        "movq %2, %%rax\n"                 // eax = TEB (32 baixos)
        "movq %2, %%rdx\n"
        "shrq $32, %%rdx\n"                // edx = TEB (32 altos)
        "wrmsr\n"
        "pushq $0x23\n"                    // SS  = UDATA | 3
        "pushq %0\n"                       // RSP
        "pushq $0x202\n"                   // RFLAGS (IF=1)
        "pushq $0x1B\n"                    // CS  = UCODE | 3
        "pushq %1\n"                       // RIP
        "iretq\n"
        : : "r"(stack_top), "r"(entry), "r"(teb) : "rax", "rcx", "rdx", "memory");
}

// Entra em ring 3 de 32 bits (compatibility mode). O iretq, em long mode, sempre
// empilha 5 qwords (RIP/CS/RFLAGS/RSP/SS); ao carregar um CS com L=0 e D/B=1
// (SEL_UCODE32), a CPU passa a executar codigo de 32 bits. EIP/ESP usam so os
// 32 bits baixos dos valores empilhados (a imagem PE32 e a pilha vivem < 4 GiB).
static void enter_ring3_32(uint32_t entry32, uint32_t stack_top32) {
    uint64_t rip = (uint64_t)entry32;
    uint64_t rsp = (uint64_t)stack_top32;
    __asm__ volatile (
        "movw $0x23, %%ax\n"               // UDATA | RPL 3 (D/B=1) p/ ds/es/fs/gs
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
        "pushq $0x23\n"                    // SS  = UDATA | 3
        "pushq %0\n"                       // RSP (low 32 -> ESP)
        "pushq $0x202\n"                   // RFLAGS (IF=1)
        "pushq $0x3B\n"                    // CS  = UCODE32 | 3  (L=0, D/B=1)
        "pushq %1\n"                       // RIP (low 32 -> EIP)
        "iretq\n"
        : : "r"(rsp), "r"(rip) : "rax", "memory");
}

// enter_ring3 com ARGUMENTO em rcx (p/ o threadproc(param) do CreateThread real).
// Igual ao enter_ring3, mas carrega rcx=arg antes do iretq (1o param x64).
static void enter_ring3_arg(uint64_t entry, uint64_t stack_top, uint64_t teb, uint64_t arg) {
    __asm__ volatile (
        "movw $0x23, %%ax\n"
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
        "movl $0xC0000101, %%ecx\n"        // IA32_GS_BASE
        "movq %2, %%rax\n movq %2, %%rdx\n shrq $32, %%rdx\n wrmsr\n"
        "movq %3, %%rcx\n"                 // rcx = arg (1o param do threadproc)
        "pushq $0x23\n"                    // SS  = UDATA | 3
        "pushq %0\n"                       // RSP
        "pushq $0x202\n"                   // RFLAGS (IF=1 -> preemptivel pelo timer)
        "pushq $0x1B\n"                    // CS  = UCODE | 3
        "pushq %1\n"                       // RIP
        "iretq\n"
        : : "r"(stack_top), "r"(entry), "r"(teb), "r"(arg) : "rax", "rcx", "rdx", "memory");
}

// ============================================================================
//  THREADS RING-3 PREEMPTIVAS — cada CreateThread do explorer vira um KTHREAD real
//  que roda em ring 3 e e' escalonado pelo timer LADO A LADO com a thread principal.
//
//  POR QUE PREEMPTIVO (e nao mais cooperativo): o explorer real cria a Worker Window
//  e a thread dela (0x72270) DURANTE o wWinMain e SEGUE rodando (mais setup do shell).
//  So la no fim (~WorkerWindow) ele faz WaitForSingleObject(thread). No modelo antigo
//  (rodar a threadproc so no WaitForSingleObject) a thread principal ficava CONGELADA
//  no wait e a worker rodava tarde e sozinha. Agora as duas rodam CONCORRENTES, na
//  ordem certa: a principal segue o fluxo COMPLETO e a worker roda em paralelo.
// ============================================================================
#define WK_STACK_SIZE 0x40000ULL   // 256 KiB de pilha de USUARIO por thread worker

// Aloca pilha de usuario + TEB + stub de retorno p/ uma thread ring-3. Devolve o TEB
// (no fundo da regiao) e o RSP inicial (topo-8, ja com o endereco do stub de retorno).
// Regiao contigua do PMM, mapeada user (2 MiB granular -> user+exec). O TEB compartilha
// o PEB do processo (PEB_ADDR): threads dividem o mesmo PEB, como no NT real.
static int usermode_alloc_ring3_stack(uint64_t* out_teb, uint64_t* out_rsp) {
    uint64_t pages = WK_STACK_SIZE >> 12;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) { kputs("[thread] sem RAM p/ pilha de thread ring-3\n"); return 0; }
    mm_map_user(phys, WK_STACK_SIZE);                    // pilha acessivel ao ring 3
    uint64_t teb = phys;                                 // TEB no fundo da regiao
    uint64_t stack_top = (phys + WK_STACK_SIZE) & ~0xFULL;
    volatile uint8_t* t = (volatile uint8_t*)(uintptr_t)teb;
    for (int i = 0; i < 0x1000; i++) t[i] = 0;
    *(volatile uint64_t*)(uintptr_t)(teb + 0x08) = stack_top;      // NtTib.StackBase
    *(volatile uint64_t*)(uintptr_t)(teb + 0x10) = teb + 0x2000;   // NtTib.StackLimit
    *(volatile uint64_t*)(uintptr_t)(teb + 0x30) = teb;           // NtTib.Self  -> gs:[0x30]
    *(volatile uint64_t*)(uintptr_t)(teb + 0x60) = PEB_ADDR;      // PEB -> gs:[0x60]

    // STUB DE RETORNO. A threadproc entra via IRETQ (nao via `call`), entao a CPU nao
    // empilha endereco de retorno; se a threadproc RETORNA (a 0x72270 do explorer e' uma
    // rotina de init que retorna com STATUS_SUCCESS), o `ret` pularia p/ [stack_top-8]=0
    // -> #PF rip=0. Escrevemos um stub REAL de ring-3 (pagina user+exec da regiao) e
    // apontamos [stack_top-8] p/ ele. Ao retornar, a threadproc cai no stub, que faz o
    // syscall 51 (sys_thread_exit) -> ki_ring3_thread_exit: a thread ring-3 termina
    // LIMPO (marca TERMINATED + acorda quem espera nela). Bytes (x64):
    //   48 89 c7            mov rdi, rax   ; arg1 = valor de retorno da threadproc (log)
    //   b8 33 00 00 00      mov eax, 51    ; SYS_THREAD_EXIT (0x33)
    //   cd 80               int 0x80       ; -> sys_thread_exit (nao retorna)
    //   eb fe               jmp $          ; seguranca
    static const uint8_t retstub[] = {
        0x48, 0x89, 0xc7, 0xb8, 0x33, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xeb, 0xfe
    };
    uint64_t stub_va = teb + 0x1000;
    volatile uint8_t* s = (volatile uint8_t*)(uintptr_t)stub_va;
    for (unsigned i = 0; i < sizeof(retstub); i++) s[i] = retstub[i];
    *(volatile uint64_t*)(uintptr_t)(stack_top - 8) = stub_va;    // endereco de retorno

    *out_teb = teb;
    *out_rsp = stack_top - 8;
    return 1;
}

// ENTRY DE KERNEL de uma thread ring-3 (rodado por ki_thread_startup apos o 1o dispatch,
// com IF=1). Le os parametros ring-3 do proprio KTHREAD e faz IRETQ p/ ring 3. A partir
// daqui a thread e' preemptada pelo timer como qualquer outra; o trap dela aterrissa na
// pilha de kernel DESTA thread (TSS.rsp0, ja programado pelo ki_quantum_end ao escala-la;
// re-afirmamos aqui por seguranca no 1o entry). NAO retorna: a threadproc roda e, ao
// retornar, cai no stub -> sys_thread_exit -> ki_ring3_thread_exit.
static void ring3_trampoline(void* arg) {
    (void)arg;
    ki_thread_t* self = ki_current_thread();
    if (!self) { for (;;) __asm__ volatile ("hlt"); }
    tss_set_rsp0(self->kstack_top);                      // trap de ring 3 -> pilha DESTA thread
    kputs("[thread] ring-3 preemptiva: start="); kput_hex(self->user_start);
    kputs(" param="); kput_hex(self->user_param);
    kputs(" rsp="); kput_hex(self->user_stack_top); kputs(" teb="); kput_hex(self->user_teb);
    kputs(" tid="); kput_hex(self->tid); kputs("\n");
    enter_ring3_arg(self->user_start, self->user_stack_top, self->user_teb, self->user_param);
    // nao retorna
}

// Cria e enfileira uma thread RING-3 preemptiva. Fixa no CPU 0 (TSS unico -> so o CPU 0
// pode entrar em ring 3 com troca de pilha de kernel). Chamado por sys_createthread.
ki_thread_t* ki_launch_ring3_thread(uint64_t user_start, uint64_t user_param) {
    if (!user_start) return 0;
    uint64_t teb = 0, rsp = 0;
    if (!usermode_alloc_ring3_stack(&teb, &rsp)) return 0;

    // KTHREAD com pilha de KERNEL propria (16 KiB, alocada em ki_create_thread). O entry
    // de kernel e' ring3_trampoline; afinidade 0 = so CPU 0.
    ki_thread_t* t = ki_create_thread(ring3_trampoline, 0, 8 /*prio*/, 0 /*cpu0*/);
    if (!t) { kputs("[thread] ki_create_thread falhou p/ thread ring-3\n"); return 0; }
    t->is_ring3       = 1;
    t->user_teb       = teb;
    t->user_start     = user_start;
    t->user_param     = user_param;
    t->user_stack_top = rsp;
    g_ring3_active    = 1;   // liga a troca de TSS.rsp0/gs.base por-thread no ki_quantum_end
    ki_ready_thread(t);      // enfileira -> o timer a escalona lado a lado com a principal
    return t;
}

void usermode_enter(void (*entry)(void)) {
    mm_map_user(USTACK_BASE, USTACK_SIZE);   // pilha de usuario acessivel ao ring 3

    // Espaco de enderecamento POR-PROCESSO: cria uma PML4 propria (copia dos
    // mapeamentos do kernel, com os bits U/S ja setados pelos mm_map_user
    // anteriores) e troca o CR3 ao entrar em ring 3. O kernel continua mapeado
    // (identidade de 1 GiB), entao IDT/syscalls/pilha de kernel seguem validos.
    // Se nao houver RAM para a nova tabela, seguimos no espaco compartilhado.
    uint64_t kernel_cr3 = mm_get_cr3();
    uint64_t proc_cr3   = mm_create_address_space();
    PEPROCESS cur = PsGetCurrentProcess();
    if (proc_cr3 && cur) { cur->DirectoryTableBase = proc_cr3; cur->PML4 = (void*)proc_cr3; }

    // Monta o TEB/PEB no fundo da pilha e liga o PEB no EPROCESS. load_base = a
    // ImageBase REAL de carga (apos relocacao) que o ldr_run gravou no EPROCESS.
    uint64_t load_base = cur ? cur->ImageBase : 0;
    build_teb_peb(load_base);
    if (cur) cur->Peb = (void*)(uintptr_t)PEB_ADDR;

    if (proc_cr3) {
        kputs("[mm] PML4 por-processo @ "); kput_hex(proc_cr3);
        kputs(" (kernel CR3 "); kput_hex(kernel_cr3); kputs(")\n");
    }
    kputs("[ps] TEB @ "); kput_hex(TEB_ADDR); kputs(" PEB @ "); kput_hex(PEB_ADDR);
    kputs(" (PEB->ImageBase="); kput_hex(load_base); kputs(")\n");

    // FRENTE THREADS RING-3 PREEMPTIVAS — marca a thread CORRENTE (a idle/boot do CPU 0,
    // sobre a qual o .exe roda) como thread ring-3 com este TEB. A partir daqui, quando o
    // timer preemptar a thread principal p/ rodar uma worker (e depois voltar), o
    // ki_quantum_end restaura gs.base=TEB_ADDR e TSS.rsp0=pilha do boot desta thread. Sem
    // isso, ao voltar da worker a principal rodaria com gs.base/pilha da worker. Tambem
    // LIGA g_ring3_active (a logica so age no CPU 0; o cenario pintok nao chama isto).
    ki_mark_current_ring3(TEB_ADDR);

    if (__builtin_setjmp(g_user_exit) == 0) {
        if (proc_cr3) mm_switch_cr3(proc_cr3);        // entra no espaco do processo
        enter_ring3(entry, USTACK_TOP - 8, TEB_ADDR); // RSP alinhado; gs:base = TEB
        // não retorna por aqui
    } else {
        // De volta ao kernel: restaura o CR3 do kernel e os segmentos de ring 0.
        if (proc_cr3) mm_switch_cr3(kernel_cr3);
        __asm__ volatile (
            "movw $0x10, %%ax\n"
            "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
            : : : "rax", "memory");
        // O 'movw %%ax,%%gs' acima zerou a base de gs do KERNEL. Restaura
        // IA32_GS_BASE = KPCR (corrige o bug latente: antes o kernel seguia com
        // gs-base 0 depois que uma app rodava). KERNEL_GS_BASE ja aponta p/ o KPCR.
        HalWriteMsr(MSR_IA32_GS_BASE, (uint64_t)(uintptr_t)kpcr_get());
    }
}

// Versao de 32 bits: roda um PE32 (codigo x86) em ring 3 compatibility mode.
// Mesma estrutura do usermode_enter, mas com o frame iretq de 32 bits. A saida
// (int 0x80 -> sys_exit) volta por aqui via longjmp, restaurando o kernel 64-bit.
void usermode_enter32(uint32_t entry32) {
    mm_map_user(USTACK_BASE, USTACK_SIZE);   // pilha de usuario acessivel ao ring 3

    uint64_t kernel_cr3 = mm_get_cr3();
    uint64_t proc_cr3   = mm_create_address_space();
    PEPROCESS cur = PsGetCurrentProcess();
    if (proc_cr3 && cur) { cur->DirectoryTableBase = proc_cr3; cur->PML4 = (void*)proc_cr3; }

    if (proc_cr3) {
        kputs("[mm] PML4 por-processo @ "); kput_hex(proc_cr3);
        kputs(" (kernel CR3 "); kput_hex(kernel_cr3); kputs(")\n");
    }

    if (__builtin_setjmp(g_user_exit) == 0) {
        if (proc_cr3) mm_switch_cr3(proc_cr3);
        enter_ring3_32(entry32, (uint32_t)(USTACK_TOP - 16));
        // não retorna por aqui
    } else {
        if (proc_cr3) mm_switch_cr3(kernel_cr3);
        __asm__ volatile (
            "movw $0x10, %%ax\n"
            "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
            : : : "rax", "memory");
    }
}
