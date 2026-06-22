#pragma once
#include <stdint.h>
#include "ntddk.h"

#ifndef MS_ABI
#define MS_ABI __attribute__((ms_abi))
#endif

// ============================================================================
//  FASE 7 — HAL expansion: MSR + CPUID (drivers fazem ambos).
//  + KeQueryActiveProcessorCount, KeQueryActiveProcessors, etc.
// ============================================================================

// MSR (Model Specific Register)
MS_ABI uint64_t HalReadMsr(uint32_t msr);
MS_ABI void     HalWriteMsr(uint32_t msr, uint64_t value);

// CPUID raw (4 dwords saida).
MS_ABI void HalCpuid(uint32_t leaf, uint32_t subleaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d);

// Info do CPU (vendor, family, model, stepping) preenchida via CPUID.
typedef struct hal_cpu_info {
    char     vendor[13];        // "GenuineIntel"/"AuthenticAMD"
    uint8_t  family;
    uint8_t  model;
    uint8_t  stepping;
    uint32_t feature_ecx;       // CPUID.1.ECX (SSE3 etc.)
    uint32_t feature_edx;       // CPUID.1.EDX
} hal_cpu_info_t;

void hal_cpu_init(void);
const hal_cpu_info_t* hal_cpu_get(void);

// Funcoes Ke* publicas do NT que dependem disso.
ULONG     NTAPI KeQueryActiveProcessorCount_k(void* ActiveProcessors);
ULONGLONG NTAPI KeQueryActiveProcessors_k(void);
void      NTAPI KeQueryPerformanceCounter_k(PLARGE_INTEGER PerformanceFrequency);

// Hal Get/Set (apoiados em rdtsc + PIT).
uint64_t  hal_rdtsc(void);
