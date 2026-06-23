#pragma once

// Inicializa e carrega a IDT (Interrupt Descriptor Table) de 64 bits.
void idt_init(void);

// Pilar 4: APs precisam LIDT na mesma tabela do BSP pra rodar handlers.
// Apenas executa LIDT do idtp existente (sem reinicializar entradas).
void idt_load(void);
