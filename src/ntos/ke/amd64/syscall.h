#pragma once
#include "ke/amd64/isr.h"

// Despacho de syscalls (chamado pelo handler de int 0x80).
// Convencao: RAX = numero; RDI = arg1.  Retorno em RAX.
void syscall_dispatch(struct regs* r);

// Buffer de retorno (setjmp) para a syscall de "exit" voltar ao kernel.
extern void* g_user_exit[5];
