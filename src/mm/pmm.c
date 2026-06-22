#include "pmm.h"

#define FRAME       4096ULL
#define PMM_BASE    0x4000000ULL              // 64 MiB: gerencia frames a partir daqui
#define MAX_PHYS    0x40000000ULL             // 1 GiB (limite do mapeamento identidade)
#define MAX_FRAMES  (MAX_PHYS / FRAME)        // 262144 frames

static uint8_t  bitmap[MAX_FRAMES / 8];       // 1 bit por frame (1 = usado)
static uint64_t s_free;
static uint64_t s_top_frame;

static void set_used(uint64_t f) { bitmap[f / 8] |=  (uint8_t)(1u << (f % 8)); }
static void set_free(uint64_t f) { bitmap[f / 8] &= (uint8_t)~(1u << (f % 8)); }
static int  is_used (uint64_t f) { return bitmap[f / 8] & (1u << (f % 8)); }

void pmm_init(uint64_t mem_top) {
    if (mem_top > MAX_PHYS) mem_top = MAX_PHYS;
    s_top_frame = mem_top / FRAME;

    for (uint64_t i = 0; i < MAX_FRAMES / 8; i++) bitmap[i] = 0xFF;  // tudo usado
    s_free = 0;

    for (uint64_t a = PMM_BASE; a + FRAME <= mem_top; a += FRAME) {  // libera faixa util
        set_free(a / FRAME);
        s_free++;
    }
}

uint64_t pmm_alloc_frame(void) {
    for (uint64_t f = PMM_BASE / FRAME; f < s_top_frame; f++) {
        if (!is_used(f)) { set_used(f); s_free--; return f * FRAME; }
    }
    return 0;
}

void pmm_free_frame(uint64_t addr) {
    uint64_t f = addr / FRAME;
    if (f < MAX_FRAMES && is_used(f)) { set_free(f); s_free++; }
}

uint64_t pmm_free_frames(void) { return s_free; }
