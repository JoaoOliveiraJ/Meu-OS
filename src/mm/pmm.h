#pragma once
#include <stdint.h>

// Gerenciador de memoria fisica (frames de 4 KiB).
void     pmm_init(uint64_t mem_top);   // mem_top = endereco fisico no topo da RAM
uint64_t pmm_alloc_frame(void);        // retorna endereco de um frame de 4 KiB (0 = sem RAM)
uint64_t pmm_alloc_contiguous(uint64_t num_pages);   // FASE 7: aloca N frames CONTIGUOS, devolve base
void     pmm_free_frame(uint64_t addr);
uint64_t pmm_free_frames(void);        // quantidade de frames livres
uint64_t pmm_total_frames(void);       // total de frames geridos (topo da RAM / 4 KiB)
