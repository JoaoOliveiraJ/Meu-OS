#include <stdint.h>
#include "ke/amd64/idt.h"

struct idt_entry {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

// Tabela com os 48 stubs, definida em isr.asm
extern void* isr_stub_table[];

static void set_gate(int n, void* handler, uint8_t type_attr) {
    uint64_t a = (uint64_t)handler;
    idt[n].off_low   = a & 0xFFFF;
    idt[n].selector  = 0x08;          // segmento de codigo 64-bit (ring 0)
    idt[n].ist       = 0;
    idt[n].type_attr = type_attr;     // 0x8E = DPL0; 0xEE = DPL3 (chamavel do ring 3)
    idt[n].off_mid   = (a >> 16) & 0xFFFF;
    idt[n].off_high  = (a >> 32) & 0xFFFFFFFF;
    idt[n].zero      = 0;
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;

    for (int i = 0; i < 48; i++) set_gate(i, isr_stub_table[i], 0x8E);

    extern void isr_stub_128(void);                 // entrada de syscall (int 0x80)
    set_gate(0x80, (void*)isr_stub_128, 0xEE);      // DPL=3: ring 3 pode invocar

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
