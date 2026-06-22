#pragma once
#include <stdint.h>

// Gerenciador de memoria fisica (frames de 4 KiB).
void     pmm_init(uint64_t mem_top);   // mem_top = endereco fisico no topo da RAM
uint64_t pmm_alloc_frame(void);        // retorna endereco de um frame de 4 KiB (0 = sem RAM)
void     pmm_free_frame(uint64_t addr);
uint64_t pmm_free_frames(void);        // quantidade de frames livres
