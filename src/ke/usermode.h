#pragma once
#include <stdint.h>

// Entra em RING 3 (modo usuário) no 'entry' dado, com uma pilha de usuário.
// Retorna ao chamador (ring 0) quando o código de usuário faz a syscall de saída.
void usermode_enter(void (*entry)(void));

// Entra em RING 3 de 32 bits (compatibility mode) no 'entry32' (endereco < 4 GiB,
// codigo PE32). Mesma mecanica de saida (int 0x80 -> sys_exit -> longjmp).
void usermode_enter32(uint32_t entry32);
