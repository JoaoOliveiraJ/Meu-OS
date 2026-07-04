#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  haldma.h — FASE FUNDACAO (trilha I/O, Fase 5): HAL DMA minimo.
// ============================================================================
#ifndef MS_ABI
#define MS_ABI __attribute__((ms_abi))
#endif

MS_ABI PDMA_ADAPTER HalGetDmaAdapter(PVOID Pdo, PVOID DeviceDescription, PULONG NumberOfMapRegisters);
// Funcoes Hal diretas (legadas) que o pintok referencia — flag-gated.
MS_ABI PVOID HalAllocateCommonBuffer(PVOID Adapter, ULONG Length, PPHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled);
MS_ABI void  HalFreeCommonBuffer(PVOID Adapter, ULONG Length, PHYSICAL_ADDRESS LogicalAddress, PVOID VirtualAddress, BOOLEAN CacheEnabled);

// Prova de boot.
void KiDmaSelfTest(void);
