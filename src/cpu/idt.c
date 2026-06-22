#include <stdint.h>
#include "cpu/idt.h"

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

static void set_gate(int n, void* handler) {
    uint64_t a = (uint64_t)handler;
    idt[n].off_low   = a & 0xFFFF;
    idt[n].selector  = 0x08;          // segmento de codigo 64-bit (GDT do boot.asm)
    idt[n].ist       = 0;
    idt[n].type_attr = 0x8E;          // present, DPL=0, interrupt gate de 64 bits
    idt[n].off_mid   = (a >> 16) & 0xFFFF;
    idt[n].off_high  = (a >> 32) & 0xFFFFFFFF;
    idt[n].zero      = 0;
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;

    for (int i = 0; i < 48; i++) set_gate(i, isr_stub_table[i]);

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
