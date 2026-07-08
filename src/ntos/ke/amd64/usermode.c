#include <stdint.h>
#include "ke/amd64/usermode.h"
#include "ke/amd64/syscall.h"
#include "ke/amd64/kpcr.h"
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

// FRENTE THREADS — roda um threadproc de ring-3 (do CreateThread real) numa PILHA + TEB
// NOVOS, no mesmo processo (mesmo CR3). Modelo COOPERATIVO com o esquema de UMA thread
// ring-3: a thread principal (parada em WaitForSingleObject sobre o handle do worker) e'
// ABANDONADA aqui — este threadproc PASSA A SER a thread ring-3 corrente. A preempcao do
// timer que ja funciona p/ 1 thread ring-3 (o explorer roda sobre o contexto idle/boot)
// segue valendo: o timer preempta o worker, roda os kthreads de kernel e volta pro worker.
// Assim o PROCESSO PERSISTE (nao chega ao DestroyWindow/ExitProcess). NAO retorna.
#define WK_STACK_SIZE 0x40000ULL   // 256 KiB por thread worker
void usermode_run_worker(uint64_t start, uint64_t param) {
    if (!start) return;
    uint64_t pages = WK_STACK_SIZE >> 12;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) { kputs("[thread] sem RAM p/ pilha do worker ring-3\n"); return; }
    mm_map_user(phys, WK_STACK_SIZE);                    // torna a pilha acessivel ao ring 3
    uint64_t teb = phys;                                 // TEB no fundo da regiao
    uint64_t stack_top = (phys + WK_STACK_SIZE) & ~0xFULL;
    // TEB minimo (compartilha o PEB do processo em PEB_ADDR — threads dividem o mesmo PEB).
    volatile uint8_t* t = (volatile uint8_t*)(uintptr_t)teb;
    for (int i = 0; i < 0x1000; i++) t[i] = 0;
    *(volatile uint64_t*)(uintptr_t)(teb + 0x08) = stack_top;      // NtTib.StackBase
    *(volatile uint64_t*)(uintptr_t)(teb + 0x10) = teb + 0x2000;   // NtTib.StackLimit
    *(volatile uint64_t*)(uintptr_t)(teb + 0x30) = teb;           // NtTib.Self  -> gs:[0x30]
    *(volatile uint64_t*)(uintptr_t)(teb + 0x60) = PEB_ADDR;      // PEB -> gs:[0x60]

    // STUB DE RETORNO da threadproc. A threadproc do worker (0x72270 no explorer) e' uma
    // funcao que RODA E RETORNA (nao um loop infinito — ver sys_worker_returned). Ao entrar
    // via IRETQ em `start` com RSP=stack_top-8, a CPU NAO empilha endereco de retorno (nao
    // houve `call`); logo [stack_top-8] (o slot que a threadproc ve como "seu endereco de
    // retorno") estava ZERADO e o `ret` final pulava p/ 0 -> #PF rip=0 -> HALT. Agora
    // escrevemos um stub REAL de ring-3 numa pagina user+exec da regiao do worker (teb+0x1000,
    // longe da pilha que desce do topo e do TEB no fundo) e apontamos [stack_top-8] p/ ele.
    // Quando a threadproc retorna, cai no stub, que passa o valor de retorno (rax) e chama o
    // syscall 51 (sys_worker_returned) -> loga + parka -> o explorer PERSISTE sem crashar.
    // Bytes do stub (x64):
    //   48 89 c7            mov rdi, rax        ; arg1 = valor de retorno da threadproc
    //   b8 33 00 00 00      mov eax, 51         ; SYS_WORKER_RETURNED (0x33)
    //   cd 80               int 0x80            ; -> sys_worker_returned (nao retorna)
    //   eb fe               jmp $               ; seguranca: spin se o syscall voltar
    {
        static const uint8_t retstub[] = {
            0x48, 0x89, 0xc7,               // mov rdi, rax
            0xb8, 0x33, 0x00, 0x00, 0x00,   // mov eax, 51
            0xcd, 0x80,                     // int 0x80
            0xeb, 0xfe                      // jmp $
        };
        uint64_t stub_va = teb + 0x1000;
        volatile uint8_t* s = (volatile uint8_t*)(uintptr_t)stub_va;
        for (unsigned i = 0; i < sizeof(retstub); i++) s[i] = retstub[i];
        *(volatile uint64_t*)(uintptr_t)(stack_top - 8) = stub_va;   // endereco de retorno da threadproc
    }

    kputs("[thread] worker ring-3: start="); kput_hex(start);
    kputs(" param="); kput_hex(param);
    kputs(" stack_top="); kput_hex(stack_top); kputs(" teb="); kput_hex(teb); kputs("\n");
    enter_ring3_arg(start, stack_top - 8, teb, param);   // vira a thread ring-3; NAO retorna
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
