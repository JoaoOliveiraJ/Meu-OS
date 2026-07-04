// ============================================================================
//  haldma.c — FASE FUNDACAO (trilha I/O, Fase 5): HAL DMA minimo.
//
//  HalGetDmaAdapter devolve um DMA_ADAPTER com a vtable DMA_OPERATIONS. Common
//  buffer via pmm_alloc_contiguous (phys==virt no identity map). Suficiente p/
//  um driver bus-master simples. As funcoes Hal diretas (HalAllocate/FreeCommon
//  Buffer), que o pintok referencia, sao flag-gated (legado -> 0/no-op).
// ============================================================================
#include <stdint.h>
#include "ntddk.h"
#include "hal/haldma.h"

extern void     kputs(const char* s);
extern void     kput_hex(uint64_t v);
extern uint64_t pmm_alloc_contiguous(uint64_t num_pages);
extern void     pmm_free_frame(uint64_t addr);
extern int      ke_legacy_active(void);

static PVOID NTAPI dma_alloc_common(PDMA_ADAPTER a, ULONG Length, PPHYSICAL_ADDRESS Logical, BOOLEAN Cache) {
    (void)a; (void)Cache;
    uint64_t pages = ((uint64_t)Length + 4095) / 4096; if (!pages) pages = 1;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) { if (Logical) Logical->QuadPart = 0; return 0; }
    for (uint64_t i = 0; i < (uint64_t)Length; i++) ((volatile uint8_t*)(uintptr_t)phys)[i] = 0;
    if (Logical) Logical->QuadPart = (LONGLONG)phys;
    return (PVOID)(uintptr_t)phys;   // identity: phys == virt
}
static void NTAPI dma_free_common(PDMA_ADAPTER a, ULONG Length, PHYSICAL_ADDRESS Logical, PVOID Virtual, BOOLEAN Cache) {
    (void)a; (void)Virtual; (void)Cache;
    uint64_t pages = ((uint64_t)Length + 4095) / 4096; if (!pages) pages = 1;
    for (uint64_t i = 0; i < pages; i++) pmm_free_frame((uint64_t)Logical.QuadPart + i * 4096);
}
static ULONG NTAPI dma_get_alignment(PDMA_ADAPTER a) { (void)a; return 64; }

static DMA_OPERATIONS s_dma_ops = {
    .Size                 = sizeof(DMA_OPERATIONS),
    .AllocateCommonBuffer = dma_alloc_common,
    .FreeCommonBuffer     = dma_free_common,
    .GetDmaAlignment      = dma_get_alignment,
    // resto = 0 (stubs seguros)
};
static DMA_ADAPTER s_dma_adapter = { 1, (USHORT)sizeof(DMA_ADAPTER), &s_dma_ops };

MS_ABI PDMA_ADAPTER HalGetDmaAdapter(PVOID Pdo, PVOID DeviceDescription, PULONG NumberOfMapRegisters) {
    (void)Pdo; (void)DeviceDescription;
    if (ke_legacy_active()) return 0;   // pintok: comportamento antigo (stub)
    if (NumberOfMapRegisters) *NumberOfMapRegisters = 16;
    return &s_dma_adapter;
}
MS_ABI PVOID HalAllocateCommonBuffer(PVOID Adapter, ULONG Length, PPHYSICAL_ADDRESS Logical, BOOLEAN Cache) {
    (void)Adapter;
    if (ke_legacy_active()) { if (Logical) Logical->QuadPart = 0; return 0; }   // ANTIGO
    return dma_alloc_common(&s_dma_adapter, Length, Logical, Cache);
}
MS_ABI void HalFreeCommonBuffer(PVOID Adapter, ULONG Length, PHYSICAL_ADDRESS Logical, PVOID Virtual, BOOLEAN Cache) {
    (void)Adapter;
    if (ke_legacy_active()) return;   // ANTIGO: no-op
    dma_free_common(&s_dma_adapter, Length, Logical, Virtual, Cache);
}

// --- Prova de boot: pega o adapter, aloca common buffer pela vtable, checa. ---
void KiDmaSelfTest(void) {
    ULONG nregs = 0;
    PDMA_ADAPTER a = HalGetDmaAdapter(0, 0, &nregs);
    if (!a || !a->DmaOperations || !a->DmaOperations->AllocateCommonBuffer) {
        kputs("[dma-test] FALHOU (sem adapter/vtable)\n"); return;
    }
    PHYSICAL_ADDRESS pa; pa.QuadPart = 0;
    void* va = a->DmaOperations->AllocateCommonBuffer(a, 4096, &pa, 1);
    if (va && (uint64_t)(uintptr_t)va == (uint64_t)pa.QuadPart && nregs == 16)
        kputs("[dma-test] HalGetDmaAdapter + AllocateCommonBuffer (phys==virt) OK\n");
    else
        kputs("[dma-test] FALHOU\n");
    if (va) a->DmaOperations->FreeCommonBuffer(a, 4096, pa, va, 1);
}
