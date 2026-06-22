// ============================================================================
//  dxgmms.c — DirectX Memory Manager Subsystem (page tracker / residency).
//
//  Implementacao stub: a memoria fisica esta toda em RAM mapeada 1:1 (nao
//  temos VRAM dedicada nem GART), entao "residency" e quase sempre
//  DXGMMS_RESIDENT_IN_RAM. O state machine ja esta correto e cresce sozinho
//  quando houver KMD com aperture real.
//
//  Decisao de design: pool estatico de 64 descritores (vs lista dinamica)
//  pra simplificar o lookup/cleanup e nao depender de uma free-list
//  no caminho de inicializacao do kernel. Cobre folgadamente o caso normal
//  (1 dxdemo aberto -> ~5 alocacoes).
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "dxgmms.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kput_dec(uint64_t v);
extern void kput_hex(uint64_t v);

// Pool fixo de descritores. Tamanho conservador — qualquer driver real
// substituiria isso por uma red-black tree por allocation_id.
#define DXGMMS_POOL_MAX 64
static DXGMMS_RESIDENCY g_pool[DXGMMS_POOL_MAX];
static int              g_dxgmms_initialized = 0;
static uint64_t         g_next_alloc_id      = 1;   // 0 reservado pra "sem id"

// Busca o primeiro slot livre. Retorna -1 se o pool encheu.
static int dxgmms_find_free_slot(void) {
    for (int i = 0; i < DXGMMS_POOL_MAX; i++)
        if (!g_pool[i].in_use) return i;
    return -1;
}

NTSTATUS NTAPI DxgMmsInitialize(void) {
    if (g_dxgmms_initialized) {
        kputs("[dxgmms] init: ja inicializado (no-op).\n");
        return STATUS_SUCCESS;
    }
    // Zera o pool. Como e static, ja vem zerado, mas a chamada deixa explicito
    // que o estado e reinicializavel (DxgMmsShutdown + DxgMmsInitialize).
    for (int i = 0; i < DXGMMS_POOL_MAX; i++) {
        g_pool[i].in_use        = 0;
        g_pool[i].size          = 0;
        g_pool[i].base          = 0;
        g_pool[i].state         = DXGMMS_RESIDENT_NONE;
        g_pool[i].lock_count    = 0;
        g_pool[i].allocation_id = 0;
    }
    g_next_alloc_id = 1;
    g_dxgmms_initialized = 1;
    kputs("[dxgmms] init OK (pool="); kput_dec(DXGMMS_POOL_MAX);
    kputs(" descritores, backend=heap kernel)\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI DxgMmsAllocate(uint64_t size, DXGMMS_RESIDENCY** out) {
    if (out) *out = 0;
    if (!g_dxgmms_initialized) {
        // Caminho seguro: inicializa sob demanda se alguem chamou cedo.
        DxgMmsInitialize();
    }
    if (size == 0 || size > (uint64_t)0x10000000ULL /* 256 MiB sanidade */) {
        kputs("[dxgmms] allocate: size invalido (");
        kput_dec(size); kputs(" bytes).\n");
        return STATUS_INVALID_PARAMETER;
    }
    int slot = dxgmms_find_free_slot();
    if (slot < 0) {
        kputs("[dxgmms] allocate: pool cheio.\n");
        return STATUS_NO_MEMORY;
    }
    // kmalloc opera em size_t. Em x86-64 size_t e 64 bits, mas como o heap do
    // kernel e relativamente pequeno, exigimos <= 256 MiB acima — passa.
    void* base = kmalloc((size_t)size);
    if (!base) {
        kputs("[dxgmms] allocate: kmalloc("); kput_dec(size);
        kputs(") devolveu NULL.\n");
        return STATUS_NO_MEMORY;
    }
    DXGMMS_RESIDENCY* r = &g_pool[slot];
    r->in_use        = 1;
    r->size          = size;
    r->base          = base;
    r->state         = DXGMMS_RESIDENT_IN_RAM;   // sem VRAM, sempre RAM
    r->lock_count    = 0;
    r->allocation_id = g_next_alloc_id++;
    if (out) *out = r;
    kputs("[dxgmms] allocate: id=");      kput_dec(r->allocation_id);
    kputs(" size=");                      kput_dec(size);
    kputs(" base=");                      kput_hex((uint64_t)(uintptr_t)base);
    kputs(" state=IN_RAM slot=");         kput_dec(slot);
    kputs("\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI DxgMmsFree(DXGMMS_RESIDENCY* res) {
    if (!res) {
        kputs("[dxgmms] free: NULL (no-op).\n");
        return STATUS_SUCCESS;   // tolerante: free(NULL) e legal
    }
    if (!res->in_use) {
        kputs("[dxgmms] free: descritor ja livre (double-free?).\n");
        return STATUS_SUCCESS;
    }
    if (res->lock_count > 0) {
        kputs("[dxgmms] free: alocacao id="); kput_dec(res->allocation_id);
        kputs(" ainda travada (lock_count="); kput_dec(res->lock_count);
        kputs("); liberando assim mesmo.\n");
    }
    if (res->base) kfree(res->base);
    kputs("[dxgmms] free: id="); kput_dec(res->allocation_id);
    kputs(" liberada (");        kput_dec(res->size);
    kputs(" bytes).\n");
    res->in_use        = 0;
    res->base          = 0;
    res->size          = 0;
    res->state         = DXGMMS_RESIDENT_NONE;
    res->lock_count    = 0;
    res->allocation_id = 0;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI DxgMmsLock(DXGMMS_RESIDENCY* res) {
    if (!res || !res->in_use) return STATUS_INVALID_PARAMETER;
    // Sem GC paginando: lock e so um contador para detectar use-after-free
    // e marcar o estado pro tooling de debug.
    res->lock_count++;
    res->state = DXGMMS_RESIDENT_LOCKED;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI DxgMmsUnlock(DXGMMS_RESIDENCY* res) {
    if (!res || !res->in_use) return STATUS_INVALID_PARAMETER;
    if (res->lock_count == 0) {
        kputs("[dxgmms] unlock: id="); kput_dec(res->allocation_id);
        kputs(" ja em lock_count=0 (unlock excessivo).\n");
        return STATUS_INVALID_PARAMETER;
    }
    res->lock_count--;
    if (res->lock_count == 0) res->state = DXGMMS_RESIDENT_IN_RAM;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI DxgMmsShutdown(void) {
    int freed = 0;
    for (int i = 0; i < DXGMMS_POOL_MAX; i++) {
        if (g_pool[i].in_use) {
            DxgMmsFree(&g_pool[i]);
            freed++;
        }
    }
    g_dxgmms_initialized = 0;
    kputs("[dxgmms] shutdown: "); kput_dec(freed);
    kputs(" alocacoes liberadas.\n");
    return STATUS_SUCCESS;
}
