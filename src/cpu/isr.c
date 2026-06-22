#include <stdint.h>
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "drivers/keyboard.h"

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

void isr_handler(struct regs* r) {
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
