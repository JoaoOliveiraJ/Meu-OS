// ============================================================================
//  FASE 7 — implementacao dos primitivos de sincronizacao (KEVENT/KSPIN_LOCK/
//  KMUTEX/KSEMAPHORE/KeWait*). Sem escalonador: Sets marcam estado; Waits leem
//  estado e (se infinite) retornam STATUS_SUCCESS imediato (modo "auto-resolve").
// ============================================================================
#include "ke/sync.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern volatile uint64_t g_ticks;   // PIT 100 Hz (cpu/pit.c)
extern volatile int g_pintok_trace;

void NTAPI KeInitializeEvent_k(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State) {
    if (g_pintok_trace) {
        kputs("  [trace] KeInitializeEvent type="); kput_hex(Type);
        kputs(" state="); kput_hex(State); kputs("\n");
    }
    if (!Event) return;
    Event->Header.Type = (UCHAR)Type;
    Event->Header.SignalState = State ? 1 : 0;
    Event->Header.WaitListHead.Flink = &Event->Header.WaitListHead;
    Event->Header.WaitListHead.Blink = &Event->Header.WaitListHead;
}
LONG NTAPI KeSetEvent_k(PKEVENT Event, KPRIORITY Increment, BOOLEAN Wait) {
    (void)Increment; (void)Wait;
    if (!Event) return 0;
    LONG prev = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    return prev;
}
LONG NTAPI KeResetEvent_k(PKEVENT Event) {
    if (!Event) return 0;
    LONG prev = Event->Header.SignalState;
    Event->Header.SignalState = 0;
    return prev;
}
LONG NTAPI KeClearEvent_k(PKEVENT Event) {
    return KeResetEvent_k(Event);
}
LONG NTAPI KeReadStateEvent_k(PKEVENT Event) {
    return Event ? Event->Header.SignalState : 0;
}

NTSTATUS NTAPI KeWaitForSingleObject_k(PVOID Object, KWAIT_REASON Reason, KPROCESSOR_MODE Mode,
                                       BOOLEAN Alertable, PLARGE_INTEGER Timeout) {
    (void)Reason; (void)Mode; (void)Alertable;
    if (!Object) return STATUS_INVALID_PARAMETER;
    // Se ja sinalizado, retorna sucesso. Senao, sem scheduler, "auto-resolve":
    // tratamos como sucesso (o driver continua). Caminho seguro p/ headless.
    PDISPATCHER_HEADER hdr = (PDISPATCHER_HEADER)Object;
    if (hdr->SignalState) {
        // Eventos de sincronizacao (auto-reset) zeram o estado apos um waiter pegar.
        if (hdr->Type == SynchronizationEvent) hdr->SignalState = 0;
        return STATUS_SUCCESS;
    }
    // Timeout 0 = poll: retorna TIMEOUT sem bloquear.
    if (Timeout && Timeout->QuadPart == 0) return STATUS_TIMEOUT;
    // TODO: implementar bloqueio real quando houver escalonador. Por ora, sucesso.
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI KeWaitForMultipleObjects_k(ULONG Count, PVOID Object[], WAIT_TYPE WaitType,
                                          KWAIT_REASON Reason, KPROCESSOR_MODE Mode,
                                          BOOLEAN Alertable, PLARGE_INTEGER Timeout, PVOID WaitBlockArray) {
    (void)WaitBlockArray;
    for (ULONG i = 0; i < Count; i++) {
        NTSTATUS st = KeWaitForSingleObject_k(Object[i], Reason, Mode, Alertable, Timeout);
        if (WaitType == WaitAny && NT_SUCCESS(st)) return STATUS_SUCCESS;
        if (WaitType == WaitAll && !NT_SUCCESS(st)) return st;
    }
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI KeDelayExecutionThread_k(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval) {
    (void)WaitMode; (void)Alertable;
    if (!Interval) return STATUS_SUCCESS;
    // Interval positivo = absoluto; negativo = relativo em 100-ns. Implementamos
    // delay relativo grosseiro via PIT (100 Hz -> tick = 10 ms = 100000 unidades de 100-ns).
    int64_t v = Interval->QuadPart;
    if (v >= 0) return STATUS_SUCCESS;   // tempo absoluto no passado: nao bloqueia
    uint64_t hundred_ns = (uint64_t)(-v);
    uint64_t ticks = hundred_ns / 100000ULL;
    if (ticks == 0) return STATUS_SUCCESS;
    uint64_t target = g_ticks + ticks;
    while (g_ticks < target) __asm__ volatile ("sti; hlt");
    return STATUS_SUCCESS;
}

// SpinLocks em UP sem preempcao: armazenam um flag interno; sem race com IRQ.
void NTAPI KeInitializeSpinLock_k(PKSPIN_LOCK SpinLock) {
    if (g_pintok_trace) kputs("  [trace] KeInitializeSpinLock\n");
    if (SpinLock) *SpinLock = 0;
}
KIRQL NTAPI KeAcquireSpinLockRaiseToDpc_k(PKSPIN_LOCK SpinLock) {
    if (SpinLock) *SpinLock = 1;
    return PASSIVE_LEVEL;   // OldIrql
}
void NTAPI KeReleaseSpinLock_k(PKSPIN_LOCK SpinLock, KIRQL OldIrql) {
    (void)OldIrql; if (SpinLock) *SpinLock = 0;
}
void NTAPI KeAcquireSpinLockAtDpcLevel_k(PKSPIN_LOCK SpinLock) { if (SpinLock) *SpinLock = 1; }
void NTAPI KeReleaseSpinLockFromDpcLevel_k(PKSPIN_LOCK SpinLock) { if (SpinLock) *SpinLock = 0; }

KIRQL NTAPI KeGetCurrentIrql_k(void) { return PASSIVE_LEVEL; }
void  NTAPI KeRaiseIrql_k(KIRQL NewIrql, KIRQL* OldIrql) { (void)NewIrql; if (OldIrql) *OldIrql = PASSIVE_LEVEL; }
void  NTAPI KeLowerIrql_k(KIRQL NewIrql) { (void)NewIrql; }

void NTAPI KeInitializeMutex_k(PKMUTEX Mutex, ULONG Level) {
    (void)Level;
    if (!Mutex) return;
    Mutex->Header.Type = 2;             // mutex
    Mutex->Header.SignalState = 1;      // libre
    Mutex->OwnerThread = 0;
    Mutex->Abandoned = 0;
    Mutex->ApcDisable = 0;
}
LONG NTAPI KeReleaseMutex_k(PKMUTEX Mutex, BOOLEAN Wait) {
    (void)Wait;
    if (!Mutex) return 0;
    LONG prev = Mutex->Header.SignalState;
    Mutex->Header.SignalState = 1;
    return prev;
}

void NTAPI KeInitializeSemaphore_k(PKSEMAPHORE Sem, LONG Count, LONG Limit) {
    if (!Sem) return;
    Sem->Header.Type = 1;
    Sem->Header.SignalState = Count;
    Sem->Limit = Limit;
}
LONG NTAPI KeReleaseSemaphore_k(PKSEMAPHORE Sem, KPRIORITY Inc, LONG Adj, BOOLEAN Wait) {
    (void)Inc; (void)Wait;
    if (!Sem) return 0;
    LONG prev = Sem->Header.SignalState;
    Sem->Header.SignalState += Adj;
    return prev;
}

void NTAPI KeQuerySystemTime_k(PLARGE_INTEGER CurrentTime) {
    if (!CurrentTime) return;
    // Tempo do sistema em unidades de 100 ns desde 1601. Nao temos RTC ligado,
    // entao usamos um epoch fixo (1 Jan 2026) + ticks*10ms. Suficiente p/ drivers
    // que so usam isso pra "monotonic timestamp".
    static const uint64_t EPOCH_2026 = 132934176000000000ULL;   // 1 Jan 2026 UTC (~)
    uint64_t ns100 = EPOCH_2026 + (uint64_t)g_ticks * 100000ULL;
    CurrentTime->QuadPart = (LONGLONG)ns100;
}
ULONGLONG NTAPI KeQueryInterruptTime_k(void) {
    return (ULONGLONG)g_ticks * 100000ULL;
}
ULONG NTAPI KeQueryTimeIncrement_k(void) { return 100000UL; }   // 10 ms em unidades de 100 ns
