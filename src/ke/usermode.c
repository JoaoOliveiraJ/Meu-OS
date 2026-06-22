#include <stdint.h>
#include "ke/usermode.h"
#include "ke/syscall.h"
#include "mm/paging.h"
#include "nt/process.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);

// Região de RAM (identidade) usada como pilha de modo usuário: [6 MiB, 7 MiB).
// Fica fora do kernel, então o ring 3 nunca enxerga a memória do kernel.
#define USTACK_BASE 0x600000ULL
#define USTACK_SIZE 0x100000ULL
#define USTACK_TOP  (USTACK_BASE + USTACK_SIZE)

// Troca para ring 3 montando um frame de iretq (SS, RSP, RFLAGS, CS, RIP).
static void enter_ring3(void (*entry)(void), uint64_t stack_top) {
    __asm__ volatile (
        "movw $0x23, %%ax\n"               // UDATA | RPL 3 nos registradores de dados
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
        "pushq $0x23\n"                    // SS  = UDATA | 3
        "pushq %0\n"                       // RSP
        "pushq $0x202\n"                   // RFLAGS (IF=1)
        "pushq $0x1B\n"                    // CS  = UCODE | 3
        "pushq %1\n"                       // RIP
        "iretq\n"
        : : "r"(stack_top), "r"(entry) : "rax", "memory");
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

    if (proc_cr3) {
        kputs("[mm] PML4 por-processo @ "); kput_hex(proc_cr3);
        kputs(" (kernel CR3 "); kput_hex(kernel_cr3); kputs(")\n");
    }

    if (__builtin_setjmp(g_user_exit) == 0) {
        if (proc_cr3) mm_switch_cr3(proc_cr3);   // entra no espaco do processo
        enter_ring3(entry, USTACK_TOP - 8);      // RSP alinhado para a ABI
        // não retorna por aqui
    } else {
        // De volta ao kernel: restaura o CR3 do kernel e os segmentos de ring 0.
        if (proc_cr3) mm_switch_cr3(kernel_cr3);
        __asm__ volatile (
            "movw $0x10, %%ax\n"
            "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
            : : : "rax", "memory");
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
