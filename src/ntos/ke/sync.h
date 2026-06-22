#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 7 — Sincronizacao do Kernel Executive (Ke*).
//
//  Drivers chamam KeInitializeEvent/KeSetEvent/KeWaitForSingleObject e os
//  irmaos para sincronizar threads/IRPs. Sem escalonador, esta camada e um
//  no-op SEGURO: Set marca o estado como signaled; Wait com timeout != 0 le
//  o estado, retorna STATUS_SUCCESS se signaled, STATUS_TIMEOUT senao.
//  Para "infinite wait" devolvemos STATUS_SUCCESS imediatamente (sem deadlock
//  num so processador sem scheduler).
//
//  KSPIN_LOCK em UP sem preempcao: armazenamos um flag interno; Acquire/Release
//  ajustam-no. KeRaiseIrql/Lower devolvem o IRQL nominal (PASSIVE_LEVEL etc.).
// ============================================================================

void NTAPI KeInitializeEvent_k(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State);
LONG NTAPI KeSetEvent_k(PKEVENT Event, KPRIORITY Increment, BOOLEAN Wait);
LONG NTAPI KeResetEvent_k(PKEVENT Event);
LONG NTAPI KeClearEvent_k(PKEVENT Event);
LONG NTAPI KeReadStateEvent_k(PKEVENT Event);

NTSTATUS NTAPI KeWaitForSingleObject_k(PVOID Object, KWAIT_REASON Reason, KPROCESSOR_MODE Mode,
                                       BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS NTAPI KeWaitForMultipleObjects_k(ULONG Count, PVOID Object[], WAIT_TYPE WaitType,
                                          KWAIT_REASON Reason, KPROCESSOR_MODE Mode,
                                          BOOLEAN Alertable, PLARGE_INTEGER Timeout, PVOID WaitBlockArray);
NTSTATUS NTAPI KeDelayExecutionThread_k(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);

void NTAPI KeInitializeSpinLock_k(PKSPIN_LOCK SpinLock);
KIRQL NTAPI KeAcquireSpinLockRaiseToDpc_k(PKSPIN_LOCK SpinLock);
void NTAPI KeReleaseSpinLock_k(PKSPIN_LOCK SpinLock, KIRQL OldIrql);
void NTAPI KeAcquireSpinLockAtDpcLevel_k(PKSPIN_LOCK SpinLock);
void NTAPI KeReleaseSpinLockFromDpcLevel_k(PKSPIN_LOCK SpinLock);

KIRQL NTAPI KeGetCurrentIrql_k(void);
void  NTAPI KeRaiseIrql_k(KIRQL NewIrql, KIRQL* OldIrql);
void  NTAPI KeLowerIrql_k(KIRQL NewIrql);

void NTAPI KeInitializeMutex_k(PKMUTEX Mutex, ULONG Level);
LONG NTAPI KeReleaseMutex_k(PKMUTEX Mutex, BOOLEAN Wait);

// Semaforos (subset).
void NTAPI KeInitializeSemaphore_k(PKSEMAPHORE Sem, LONG Count, LONG Limit);
LONG NTAPI KeReleaseSemaphore_k(PKSEMAPHORE Sem, KPRIORITY Inc, LONG Adj, BOOLEAN Wait);

// Timer relativo: pega ticks do PIT (10 ms cada — pit_init(100)).
void NTAPI KeQuerySystemTime_k(PLARGE_INTEGER CurrentTime);
ULONGLONG NTAPI KeQueryInterruptTime_k(void);
ULONG     NTAPI KeQueryTimeIncrement_k(void);
