#pragma once
#include <stdint.h>

// ============================================================================
//  FASE 7.7 — CPU FEATURES (CR4 + XCR0).
//
//  Habilita as features estendidas que o NT real liga na inicializacao:
//   - CR4.OSXSAVE  (bit 18): permite XSAVE/XRSTOR e xsetbv  -> XCR0
//   - XCR0 = 0x7   (X87 + SSE + AVX, somente se AVX presente; XCR0=0x3 senao)
//   - CR4.SMEP     (bit 20): Supervisor Mode Execution Prevention
//   - CR4.SMAP     (bit 21): Supervisor Mode Access Prevention
//   - CR4.UMIP     (bit 11): User-Mode Instruction Prevention
//   - CR4.PCIDE    (bit 17): PCID Enable  (so habilita se CR3 valido e PCID=0)
//
//  CADA BIT e checado via CPUID antes de ser SETADO. Setar bit reservado em CR4
//  ou rodar xsetbv sem OSXSAVE = #GP imediato no boot.
//
//  Chamar APOS hal_cpu_init() (que ja preenche s_cpu via CPUID.1) e ANTES de
//  kpcr_init(): nao ha dependencia direta, mas mantemos a ordem do checklist.
// ============================================================================

// Inicializa CR4 + XCR0 com as features suportadas pelo HW. Loga o resultado.
void cpu_features_init(void);

// Snapshot do que ficou ligado (para debug futuro / pintok querer ler).
typedef struct cpu_features {
    uint64_t cr4_final;     // valor final do CR4 escrito
    uint64_t xcr0_final;    // valor final do XCR0 escrito (0 se OSXSAVE nao foi setado)
    uint8_t  has_osxsave;
    uint8_t  has_avx;
    uint8_t  has_smep;
    uint8_t  has_smap;
    uint8_t  has_umip;
    uint8_t  has_pcide;
    uint8_t  has_xsave;     // CPUID.1.ECX[26] = XSAVE base
    uint8_t  reserved;
} cpu_features_t;

const cpu_features_t* cpu_features_get(void);
