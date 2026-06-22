#pragma once
#include <stddef.h>

// Heap do kernel: alocacao dinamica (base para o carregador de programas).
void  heap_init(void);
void* kmalloc(size_t n);
void  kfree(void* p);
