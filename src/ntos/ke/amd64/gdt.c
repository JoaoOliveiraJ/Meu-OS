#include <stdint.h>
#include "ke/amd64/gdt.h"

// GDT: null, codigo/dados ring0, codigo/dados ring3, o TSS (2 slots) e, no fim,
// um segmento de codigo ring3 de 32 bits (compatibility mode) p/ rodar PE32.
static uint64_t gdt[8];

struct __attribute__((packed)) gdtr { uint16_t limit; uint64_t base; };
static struct gdtr g_gdtr;

// TSS de 64 bits (104 bytes).
struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
};
static struct tss64 g_tss;

// Pilha de kernel usada nas transicoes ring3 -> ring0 (RSP0 do TSS).
static uint8_t kstack[16384] __attribute__((aligned(16)));

// Descritor de segmento (base=0, limite=4 GiB); so importam access e flags.
static uint64_t make_seg(uint8_t access, uint8_t flags) {
    uint64_t d = 0xFFFFULL;                 // limite 0:15
    d |= ((uint64_t)0xF) << 48;             // limite 16:19
    d |= (uint64_t)access << 40;            // byte de acesso
    d |= (uint64_t)(flags & 0xF) << 52;     // flags (G, DB, L, AVL)
    return d;
}

void tss_set_rsp0(unsigned long long rsp0) { g_tss.rsp0 = rsp0; }
unsigned long long tss_get_rsp0(void) { return g_tss.rsp0; }

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = make_seg(0x9A, 0xA);   // ring0 code: P,DPL0,S,exec,read | L=1
    gdt[2] = make_seg(0x92, 0xC);   // ring0 data: P,DPL0,S,read/write
    gdt[3] = make_seg(0xFA, 0xA);   // ring3 code: P,DPL3,S,exec,read | L=1
    gdt[4] = make_seg(0xF2, 0xC);   // ring3 data: P,DPL3,S,read/write

    // TSS
    g_tss.rsp0 = (uint64_t)(kstack + sizeof(kstack));
    g_tss.iopb = sizeof(struct tss64);
    uint64_t base  = (uint64_t)&g_tss;
    uint32_t limit = sizeof(struct tss64) - 1;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | ((uint64_t)0x89 << 40)               // present, type=9 (TSS 64 disponivel)
           | (((uint64_t)(limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = (base >> 32) & 0xFFFFFFFF;

    // Segmento de codigo ring3 de 32 bits (compatibility mode): L=0, D/B=1.
    // base=0, limite=4 GiB (G=1). Usado para entrar em ring 3 rodando PE32.
    // Os dados/pilha de 32 bits reutilizam o SEL_UDATA (ja com D/B=1, DPL3).
    gdt[7] = make_seg(0xFA, 0xC);   // P,DPL3,S,exec,read | G=1,D/B=1,L=0

    g_gdtr.limit = sizeof(gdt) - 1;
    g_gdtr.base  = (uint64_t)gdt;

    // Carrega a GDT, recarrega os segmentos de dados e o CS (far-return).
    __asm__ volatile (
        "lgdt %0\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n movw %%ax, %%gs\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        : : "m"(g_gdtr) : "rax", "memory");

    // Carrega o Task Register com o seletor do TSS.
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)SEL_TSS));
}
