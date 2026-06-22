#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 7 — Pool de memoria estilo Ex* do NT (apoiado no kmalloc/kfree).
//
//  Drivers usam ExAllocatePool/ExFreePool/ExAllocatePoolWithTag para alocar
//  memoria de kernel. Como nosso kernel tem heap unico (kmalloc), a paginacao
//  nao distingue paged/nonpaged — sempre devolvemos memoria nao-paginada
//  fisicamente identidade-mapeada (faixa < 1 GiB). Os tags sao guardados num
//  cabecalho oculto antes do bloco devolvido, p/ rastrear vazamentos via
//  ExAllocatePoolStatistics / pool_dump.
// ============================================================================

PVOID NTAPI ExAllocatePool_k        (POOL_TYPE PoolType, SIZE_T NumberOfBytes);
PVOID NTAPI ExAllocatePoolWithTag_k (POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
PVOID NTAPI ExAllocatePool2_k       (uint64_t Flags, SIZE_T NumberOfBytes, ULONG Tag);  // Win10+
PVOID NTAPI ExAllocatePool3_k       (uint64_t Flags, SIZE_T NumberOfBytes, ULONG Tag, PVOID Params);
void  NTAPI ExFreePool_k            (PVOID P);
void  NTAPI ExFreePoolWithTag_k     (PVOID P, ULONG Tag);
PVOID NTAPI ExAllocatePoolUninitialized_k(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);

// Stats para o test driver consultar.
uint64_t pool_total_allocs(void);
uint64_t pool_total_frees(void);
uint64_t pool_bytes_outstanding(void);
// FASE 5: separa Paged vs NonPaged outstanding (somam pool_bytes_outstanding).
uint64_t pool_bytes_paged(void);
uint64_t pool_bytes_nonpaged(void);
