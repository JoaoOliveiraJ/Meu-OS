#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  dpc.h — FASE FUNDACAO (Item 2): DPC (Deferred Procedure Call) real.
// ============================================================================

// Init do subsistema (chamado no boot, apos ki_init_processor/kpcr).
void KiInitializeDpcSubsystem(void);
// Executa todos os DPCs da fila do CPU 'cpu'. Deve rodar em IRQL == DISPATCH.
void KiRetireDpcList(int cpu);
// Drena a fila do CPU corrente se ha DPC pendente. Chamado por KeLowerIrql ao
// cair abaixo de DISPATCH.
void KiCheckDpcDrain(void);

// API NT (drivers).
void    NTAPI KeInitializeDpc_k(PKDPC Dpc, PVOID DeferredRoutine, PVOID DeferredContext);
BOOLEAN NTAPI KeInsertQueueDpc_k(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
BOOLEAN NTAPI KeRemoveQueueDpc_k(PKDPC Dpc);
void    NTAPI KeSetImportanceDpc_k(PKDPC Dpc, int Importance);

// Prova de boot (enfileira 1 DPC em PASSIVE e confirma que disparou).
void KiDpcSelfTest(void);
