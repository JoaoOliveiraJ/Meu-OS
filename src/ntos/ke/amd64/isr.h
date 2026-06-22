#pragma once
#include <stdint.h>

// Estado salvo pelos stubs de isr.asm (ordem casa com os pushes).
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;   // empilhados pela CPU
};

void isr_handler(struct regs* r);

extern volatile uint64_t g_ticks;   // contador do timer (IRQ0)
