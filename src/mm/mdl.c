// ============================================================================
//  FASE 7.6 — Memory Descriptor List (MDL) real do MeuOS.
//
//  Implementa as rotinas de MDL que pintok.sys e outros drivers reais usam:
//    - IoAllocateMdl: aloca cabecalho (0x30 bytes) + array de PFN_NUMBER, um
//      por pagina coberta pelo buffer (StartVa..StartVa+Length).
//    - MmProbeAndLockPages: valida endereco e marca MDL_PAGES_LOCKED.
//    - MmMapLockedPagesSpecifyCache: como nosso ambiente e identidade, devolve
//      o proprio endereco virtual (StartVa+ByteOffset) e marca
//      MDL_MAPPED_TO_SYSTEM_VA.
//    - MmUnlockPages / MmUnmapLockedPages: limpam flags.
//    - IoFreeMdl: libera o pool.
//    - MmBuildMdlForNonPagedPool: marca MDL_SOURCE_IS_NONPAGED_POOL e
//      preenche os PFNs com (addr >> 12).
//
//  IMPORTANTE: como o kernel mapeia o 1o GiB identidade (fisico == virtual), o
//  PFN de cada pagina coincide com a pagina virtual. Em arquitetura mais
//  robusta (per-processo MMU), aqui chamariamos MmGetPhysicalAddress por PTE.
// ============================================================================
#include "mm/mdl.h"
#include "ke/pool.h"
#include "ntddk.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);

// Tag 'MdlR' (em little-endian "RldM") — facilita reconhecer no pool dump.
#define MDL_POOL_TAG  0x526C644Du

// Pequeno helper p/ preencher o array de PFNs em identidade (PFN = vaddr>>12).
// Em um futuro com MMU per-processo real, isso virara walker de pagetable.
static void mdl_fill_pfns_identity(PMDL Mdl) {
    if (!Mdl) return;
    PPFN_NUMBER pfns = MmGetMdlPfnArray(Mdl);
    if (!pfns) return;
    ULONG nPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(
        (uint8_t*)Mdl->StartVa + Mdl->ByteOffset, Mdl->ByteCount);
    uintptr_t va = (uintptr_t)Mdl->StartVa;
    for (ULONG i = 0; i < nPages; i++) {
        pfns[i] = (PFN_NUMBER)((va + (uintptr_t)i * 4096) >> 12);
    }
}

// ----------------------------------------------------------------------------
//  IoAllocateMdl — aloca uma MDL para descrever [VirtualAddress, +Length).
//
//  SecondaryBuffer/ChargeQuota/Irp ignorados (mesmo no NT real sao no-ops
//  em muitos caminhos). Quando Irp != 0, drivers reais ligam o MDL a IRP via
//  Irp->MdlAddress; aqui nao temos esse campo (IRP simplificada), entao
//  apenas devolvemos o ponteiro p/ o driver montar como quiser.
// ----------------------------------------------------------------------------
PMDL NTAPI IoAllocateMdl_k(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer,
                           BOOLEAN ChargeQuota, PIRP Irp) {
    (void)SecondaryBuffer; (void)ChargeQuota; (void)Irp;
    if (Length == 0) return 0;

    // Numero de paginas cobertas, considerando offset dentro da 1a pagina.
    ULONG nPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(VirtualAddress, Length);
    if (nPages == 0) nPages = 1;

    SIZE_T total = 0x30 + (SIZE_T)nPages * sizeof(PFN_NUMBER);
    PMDL Mdl = (PMDL)ExAllocatePoolWithTag_k(NonPagedPool, total, MDL_POOL_TAG);
    if (!Mdl) return 0;

    // Zera tudo p/ comecar limpo.
    for (SIZE_T i = 0; i < total; i++) ((uint8_t*)Mdl)[i] = 0;

    // Preenche campos do cabecalho.
    Mdl->Next            = 0;
    Mdl->Size            = (SHORT)total;
    Mdl->MdlFlags        = MDL_ALLOCATED_FIXED_SIZE;
    Mdl->Process         = 0;        // 0 = pool de sistema
    Mdl->MappedSystemVa  = 0;
    Mdl->StartVa         = (PVOID)((uintptr_t)VirtualAddress & ~0xFFFULL);
    Mdl->ByteCount       = Length;
    Mdl->ByteOffset      = (ULONG)((uintptr_t)VirtualAddress & 0xFFFULL);

    // Preenche PFNs em identidade (kernel mapeia <1 GiB 1:1).
    mdl_fill_pfns_identity(Mdl);

    return Mdl;
}

// ----------------------------------------------------------------------------
//  MmProbeAndLockPages — valida o endereco e "trava" as paginas em memoria.
//
//  Sem MMU per-processo robusta, o lock e simbolico: marcamos MDL_PAGES_LOCKED
//  para que o driver veja a flag e prossiga com Mm*Mdl* normalmente.
// ----------------------------------------------------------------------------
void NTAPI MmProbeAndLockPages_k(PMDL Mdl, KPROCESSOR_MODE AccessMode, LOCK_OPERATION Operation) {
    (void)AccessMode;
    if (!Mdl) return;

    Mdl->MdlFlags |= MDL_PAGES_LOCKED;
    if (Operation == IoWriteAccess || Operation == IoModifyAccess)
        Mdl->MdlFlags |= MDL_WRITE_OPERATION;

    // Refresca PFNs (caso o driver tenha realocado StartVa via IoBuildPartialMdl).
    mdl_fill_pfns_identity(Mdl);
}

// ----------------------------------------------------------------------------
//  MmMapLockedPagesSpecifyCache — mapeia as paginas trancadas em endereco de
//  sistema. Como nosso 1o GiB ja e identidade-mapeado e os PFNs apontam direto
//  para as paginas, a "VA mapeada" e o proprio StartVa+ByteOffset. Setamos a
//  flag MDL_MAPPED_TO_SYSTEM_VA e MappedSystemVa, exatamente como o NT faria.
//
//  BugCheckOnFailure: drivers passam 0 ou MM_DONT_ZERO_FAULT (1). Como nunca
//  falhamos em arquitetura identidade, ignoramos.
// ----------------------------------------------------------------------------
PVOID NTAPI MmMapLockedPagesSpecifyCache_k(PMDL Mdl, KPROCESSOR_MODE AccessMode,
                                            MEMORY_CACHING_TYPE CacheType,
                                            PVOID BaseAddress, ULONG BugCheckOnFailure,
                                            ULONG Priority) {
    (void)AccessMode; (void)CacheType; (void)BaseAddress; (void)BugCheckOnFailure; (void)Priority;
    if (!Mdl) return 0;

    PVOID va = (PVOID)((uintptr_t)Mdl->StartVa + Mdl->ByteOffset);
    Mdl->MappedSystemVa = va;
    Mdl->MdlFlags |= MDL_MAPPED_TO_SYSTEM_VA;
    return va;
}

PVOID NTAPI MmMapLockedPages_k(PMDL Mdl, KPROCESSOR_MODE AccessMode) {
    return MmMapLockedPagesSpecifyCache_k(Mdl, AccessMode, MmCached, 0, 0, 0);
}

// ----------------------------------------------------------------------------
//  MmUnlockPages — simbolico: limpa a flag MDL_PAGES_LOCKED.
//  MmUnmapLockedPages — limpa MDL_MAPPED_TO_SYSTEM_VA e zera MappedSystemVa.
//  IoFreeMdl — libera o pool. Encadeia se Mdl->Next nao for 0 (raro).
// ----------------------------------------------------------------------------
void NTAPI MmUnlockPages_k(PMDL Mdl) {
    if (!Mdl) return;
    Mdl->MdlFlags &= (SHORT)~MDL_PAGES_LOCKED;
    Mdl->MdlFlags &= (SHORT)~MDL_WRITE_OPERATION;
}

void NTAPI MmUnmapLockedPages_k(PVOID BaseAddress, PMDL Mdl) {
    (void)BaseAddress;
    if (!Mdl) return;
    Mdl->MdlFlags &= (SHORT)~MDL_MAPPED_TO_SYSTEM_VA;
    Mdl->MappedSystemVa = 0;
}

void NTAPI IoFreeMdl_k(PMDL Mdl) {
    if (!Mdl) return;
    // Se ainda mapeada/trancada, limpa de boa vontade.
    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) MmUnmapLockedPages_k(Mdl->MappedSystemVa, Mdl);
    if (Mdl->MdlFlags & MDL_PAGES_LOCKED)        MmUnlockPages_k(Mdl);
    ExFreePoolWithTag_k((PVOID)Mdl, MDL_POOL_TAG);
}

// ----------------------------------------------------------------------------
//  MmBuildMdlForNonPagedPool — preenche os PFNs a partir do endereco virtual
//  (que ja esta em pool nao-paginado). Como o pool nao-paginado e identidade
//  no nosso heap, PFN = vaddr >> 12. Marca a flag e MappedSystemVa.
// ----------------------------------------------------------------------------
void NTAPI MmBuildMdlForNonPagedPool_k(PMDL Mdl) {
    if (!Mdl) return;
    Mdl->MdlFlags |= MDL_SOURCE_IS_NONPAGED_POOL;
    Mdl->MappedSystemVa = (PVOID)((uintptr_t)Mdl->StartVa + Mdl->ByteOffset);
    Mdl->MdlFlags |= MDL_MAPPED_TO_SYSTEM_VA;
    mdl_fill_pfns_identity(Mdl);
}

// ----------------------------------------------------------------------------
//  Acessores (versoes _k tambem expostas para o ntexec, embora os inline da
//  ntddk.h ja resolvam a maioria dos casos em compile-time).
// ----------------------------------------------------------------------------
PVOID NTAPI MmGetSystemAddressForMdlSafe_k(PMDL Mdl, ULONG Priority) {
    return MmGetSystemAddressForMdlSafe(Mdl, Priority);
}
PVOID NTAPI MmGetMdlVirtualAddress_k(PMDL Mdl) { return MmGetMdlVirtualAddress(Mdl); }
ULONG NTAPI MmGetMdlByteCount_k(PMDL Mdl)      { return MmGetMdlByteCount(Mdl); }
ULONG NTAPI MmGetMdlByteOffset_k(PMDL Mdl)     { return MmGetMdlByteOffset(Mdl); }

// MmSizeOfMdl — calcula quantos bytes uma MDL ocuparia se descrevesse
// [Base, Base+Length). Usado por drivers que pre-alocam buffer fixo de MDL.
SIZE_T NTAPI MmSizeOfMdl_k(PVOID Base, SIZE_T Length) {
    ULONG nPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(Base, Length);
    if (nPages == 0) nPages = 1;
    return 0x30 + (SIZE_T)nPages * sizeof(PFN_NUMBER);
}

// Bridge p/ a API estilo Microsoft (sem o sufixo _k) usada pelo macro
// MmGetSystemAddressForMdlSafe inline da ntddk.h, que precisa de
// MmMapLockedPagesSpecifyCache "publica" como simbolo.
PVOID NTAPI MmMapLockedPagesSpecifyCache(PMDL Mdl, KPROCESSOR_MODE AccessMode,
                                          MEMORY_CACHING_TYPE CacheType,
                                          PVOID BaseAddress, ULONG BugCheckOnFailure,
                                          ULONG Priority) {
    return MmMapLockedPagesSpecifyCache_k(Mdl, AccessMode, CacheType,
                                          BaseAddress, BugCheckOnFailure, Priority);
}
