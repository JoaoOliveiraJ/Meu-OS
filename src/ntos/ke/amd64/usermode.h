#pragma once
#include <stdint.h>

// Entra em RING 3 (modo usuário) no 'entry' dado, com uma pilha de usuário.
// Retorna ao chamador (ring 0) quando o código de usuário faz a syscall de saída.
void usermode_enter(void (*entry)(void));

// Entra em RING 3 de 32 bits (compatibility mode) no 'entry32' (endereco < 4 GiB,
// codigo PE32). Mesma mecanica de saida (int 0x80 -> sys_exit -> longjmp).
void usermode_enter32(uint32_t entry32);

// FRENTE 4 (Fase 4c) — define a linha de comando do PROXIMO processo a entrar em ring 3
// (o build_teb_peb a escreve em PEB+0x800; o CRT le/parseia em argv). s==0 ou "" limpa.
// Setado pelo sys_createprocess antes de rodar o filho; limpo depois.
void usermode_set_cmdline(const char* s);

// FRENTE THREADS — roda um threadproc de ring-3 (CreateThread real) numa pilha+TEB novos, no
// processo corrente. Modelo cooperativo: a thread principal que chama isto (parada em
// WaitForSingleObject sobre o handle do worker) e' abandonada; o threadproc vira a thread
// ring-3 corrente (preempcao do timer segue valendo). NAO retorna. rcx=param no threadproc.
void usermode_run_worker(uint64_t start, uint64_t param);
