#pragma once

// Controlador de interrupcoes 8259 (PIC).
void pic_remap(void);   // remapeia IRQ0..15 para os vetores 32..47
void pic_eoi(int irq);  // End-Of-Interrupt

// Pilar 2 (APIC): desabilita o 8259 mascarando TODOS os pinos. Depois disso
// somente o IO-APIC pode disparar IRQs. Idempotente.
void pic_disable(void);
