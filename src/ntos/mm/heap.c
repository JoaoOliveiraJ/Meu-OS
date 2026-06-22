#include <stdint.h>
#include "heap.h"

// Heap simples (first-fit com split + coalescing) numa regiao fixa
// identidade-mapeada. Suficiente como base; depois trocamos por algo
// que cresce via PMM/paginacao.
#define HEAP_BASE  0x2000000ULL    // 32 MiB
#define HEAP_SIZE  0x1000000ULL    // 16 MiB

typedef struct block {
    size_t        size;            // bytes uteis deste bloco
    int           free;
    struct block* next;
} block_t;

static block_t* s_head;

void heap_init(void) {
    s_head = (block_t*)(uintptr_t)HEAP_BASE;
    s_head->size = (size_t)(HEAP_SIZE - sizeof(block_t));
    s_head->free = 1;
    s_head->next = 0;
}

void* kmalloc(size_t n) {
    n = (n + 15) & ~((size_t)15);                 // alinha em 16
    for (block_t* b = s_head; b; b = b->next) {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(block_t) + 16) {   // divide o bloco
                block_t* nb = (block_t*)((uint8_t*)(b + 1) + n);
                nb->size = b->size - n - sizeof(block_t);
                nb->free = 1;
                nb->next = b->next;
                b->size  = n;
                b->next  = nb;
            }
            b->free = 0;
            return (void*)(b + 1);
        }
    }
    return 0;   // sem espaco
}

void kfree(void* p) {
    if (!p) return;
    block_t* b = (block_t*)p - 1;
    b->free = 1;
    // junta blocos livres adjacentes
    for (block_t* c = s_head; c; c = c->next) {
        while (c->free && c->next && c->next->free) {
            c->size += sizeof(block_t) + c->next->size;
            c->next  = c->next->next;
        }
    }
}
