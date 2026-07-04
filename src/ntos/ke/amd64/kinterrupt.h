#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  kinterrupt.h — FASE FUNDACAO (trilha I/O, Fase 3): modelo de interrupcao.
// ============================================================================

void ke_interrupt_init(void);
// Chamado pelo isr_handler (isr.c) p/ um vetor: 1 se havia KINTERRUPT registrado
// e o ISR do driver foi despachado (em DIRQL); 0 se cai na cadeia legada.
int  ke_interrupt_dispatch(uint64_t vector);
// Prova de boot (conecta um vetor, dispara via 'int', confere o ISR rodou).
void KiInterruptSelfTest(void);

// API NT (drivers).
NTSTATUS NTAPI IoConnectInterrupt_k(PKINTERRUPT* InterruptObject, PKSERVICE_ROUTINE ServiceRoutine,
    PVOID ServiceContext, PKSPIN_LOCK SpinLock, ULONG Vector, KIRQL Irql, KIRQL SynchronizeIrql,
    KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector, KAFFINITY ProcessorEnableMask, BOOLEAN FloatingSave);
void     NTAPI IoDisconnectInterrupt_k(PKINTERRUPT InterruptObject);
void     NTAPI KeInitializeInterrupt_k(PKINTERRUPT Interrupt, PKSERVICE_ROUTINE Routine, PVOID Ctx,
    PKSPIN_LOCK Lock, ULONG Vector, KIRQL Irql, KIRQL SyncIrql, KINTERRUPT_MODE Mode, BOOLEAN Shared,
    KAFFINITY Aff, BOOLEAN Float);
BOOLEAN  NTAPI KeConnectInterrupt_k(PKINTERRUPT Interrupt);
BOOLEAN  NTAPI KeDisconnectInterrupt_k(PKINTERRUPT Interrupt);
BOOLEAN  NTAPI KeSynchronizeExecution_k(PKINTERRUPT Interrupt, PKSYNCHRONIZE_ROUTINE Routine, PVOID Ctx);

// Hal (aloca vetor de sistema p/ um IRQ de barramento; programa IO-APIC mascarada).
__attribute__((ms_abi)) ULONG HalGetInterruptVector(ULONG BusType, ULONG BusNumber,
    ULONG BusInterruptLevel, ULONG BusInterruptVector, KIRQL* Irql, KAFFINITY* Affinity);
