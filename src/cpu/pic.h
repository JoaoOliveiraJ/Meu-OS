#pragma once

// Controlador de interrupcoes 8259 (PIC).
void pic_remap(void);   // remapeia IRQ0..15 para os vetores 32..47
void pic_eoi(int irq);  // End-Of-Interrupt
