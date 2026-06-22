#pragma once

// Seletores de segmento (igual ao espirito do NT: anel 0 e anel 3).
#define SEL_KCODE   0x08   // codigo  ring 0 (64-bit)
#define SEL_KDATA   0x10   // dados   ring 0
#define SEL_UCODE   0x18   // codigo  ring 3 (64-bit)
#define SEL_UDATA   0x20   // dados   ring 3
#define SEL_TSS     0x28   // Task State Segment (ocupa 2 slots: 0x28 e 0x30)
#define SEL_UCODE32 0x38   // codigo  ring 3 (32-bit, compatibility mode)

// Monta GDT (com segmentos de usuario) + TSS e carrega tudo.
void gdt_init(void);

// Define o RSP0 do TSS (pilha de kernel usada quando ring 3 -> ring 0).
void tss_set_rsp0(unsigned long long rsp0);
