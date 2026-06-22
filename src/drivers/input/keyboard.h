#pragma once
#include <stdint.h>

// Tratador da IRQ1 (teclado PS/2): le o scancode e ecoa o caractere.
void keyboard_irq(void);

// Buffer de stdin do console (fila de teclas digitadas). Quando NAO ha janelas
// GUI, a IRQ1 enfileira os caracteres aqui para o NtReadFile do "console device"
// drenar (entrada interativa do cmd.exe com display). kbd_stdin_read copia ate
// 'max' bytes ja disponiveis e devolve quantos copiou (0 = nada digitado ainda).
uint32_t kbd_stdin_read(char* dst, uint32_t max);
