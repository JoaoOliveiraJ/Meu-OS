#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 7.6 — Memory Descriptor List (MDL) real.
//
//  Substitui o stub antigo de 64 bytes opacos do ntexec.c. Agora a MDL tem o
//  layout NT x64 verdadeiro (definido em sdk/ntddk.h), com header de 0x30
//  bytes + array variavel de PFN_NUMBER (1 por pagina coberta pelo buffer).
//
//  No nosso ambiente (RAM identidade-mapeada em 1 GiB), os PFNs do array
//  inicial sao simplesmente (VirtualAddress >> 12) — ou seja, o numero da
//  pagina fisica coincide com o numero da pagina virtual. MmProbeAndLock e
//  uma validacao leve, MmMapLockedPagesSpecifyCache e identidade.
// ============================================================================

PMDL  NTAPI IoAllocateMdl_k(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer,
                            BOOLEAN ChargeQuota, PIRP Irp);
void  NTAPI IoFreeMdl_k(PMDL Mdl);
void  NTAPI MmProbeAndLockPages_k(PMDL Mdl, KPROCESSOR_MODE AccessMode, LOCK_OPERATION Operation);
PVOID NTAPI MmMapLockedPagesSpecifyCache_k(PMDL Mdl, KPROCESSOR_MODE AccessMode,
                                            MEMORY_CACHING_TYPE CacheType,
                                            PVOID BaseAddress, ULONG BugCheckOnFailure,
                                            ULONG Priority);
PVOID NTAPI MmMapLockedPages_k(PMDL Mdl, KPROCESSOR_MODE AccessMode);
void  NTAPI MmUnlockPages_k(PMDL Mdl);
void  NTAPI MmUnmapLockedPages_k(PVOID BaseAddress, PMDL Mdl);
void  NTAPI MmBuildMdlForNonPagedPool_k(PMDL Mdl);
PVOID NTAPI MmGetSystemAddressForMdlSafe_k(PMDL Mdl, ULONG Priority);
PVOID NTAPI MmGetMdlVirtualAddress_k(PMDL Mdl);
ULONG NTAPI MmGetMdlByteCount_k(PMDL Mdl);
ULONG NTAPI MmGetMdlByteOffset_k(PMDL Mdl);
SIZE_T NTAPI MmSizeOfMdl_k(PVOID Base, SIZE_T Length);
