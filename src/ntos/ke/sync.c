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

// ============================================================================
//  FASE FUNDACAO (Item 3) — spinlocks reais: atomico (xchg) + raise IRQL a
//  DISPATCH no acquire, restore no release. Livre de deadlock porque o ISR do
//  timer NAO preempta em IRQL>=DISPATCH (gating em isr.c) -> o holder nunca e'
//  trocado enquanto segura. KSPIN_LOCK e ULONGLONG (0=livre,1=preso). UP-correto
//  e MP-ready. pintok nao usa spinlock (baseline) -> inerte p/ ele.
// ============================================================================
static inline void ki_spin_acquire(PKSPIN_LOCK L) {
    while (__atomic_exchange_n(L, 1ULL, __ATOMIC_ACQUIRE) != 0)
        while (__atomic_load_n(L, __ATOMIC_RELAXED)) __asm__ volatile ("pause");
}
static inline void ki_spin_release(PKSPIN_LOCK L) {
    __atomic_store_n(L, 0ULL, __ATOMIC_RELEASE);
}
void NTAPI KeInitializeSpinLock_k(PKSPIN_LOCK SpinLock) {
    if (g_pintok_trace) kputs("  [trace] KeInitializeSpinLock\n");
    if (SpinLock) __atomic_store_n(SpinLock, 0ULL, __ATOMIC_RELEASE);
}
KIRQL NTAPI KeAcquireSpinLockRaiseToDpc_k(PKSPIN_LOCK SpinLock) {
    KIRQL old; KeRaiseIrql_k(DISPATCH_LEVEL, &old);
    if (SpinLock) ki_spin_acquire(SpinLock);
    return old;
}
void NTAPI KeReleaseSpinLock_k(PKSPIN_LOCK SpinLock, KIRQL OldIrql) {
    if (SpinLock) ki_spin_release(SpinLock);
    KeLowerIrql_k(OldIrql);
}
void NTAPI KeAcquireSpinLockAtDpcLevel_k(PKSPIN_LOCK SpinLock) { if (SpinLock) ki_spin_acquire(SpinLock); }
void NTAPI KeReleaseSpinLockFromDpcLevel_k(PKSPIN_LOCK SpinLock) { if (SpinLock) ki_spin_release(SpinLock); }
// Variantes adicionais (append-only na tabela EX).
void NTAPI KeAcquireSpinLock_k(PKSPIN_LOCK SpinLock, KIRQL* OldIrql) {
    KIRQL o = KeAcquireSpinLockRaiseToDpc_k(SpinLock); if (OldIrql) *OldIrql = o;
}
KIRQL NTAPI KfAcquireSpinLock_k(PKSPIN_LOCK SpinLock) { return KeAcquireSpinLockRaiseToDpc_k(SpinLock); }
void NTAPI KfReleaseSpinLock_k(PKSPIN_LOCK SpinLock, KIRQL OldIrql) { KeReleaseSpinLock_k(SpinLock, OldIrql); }
void NTAPI KeAcquireInStackQueuedSpinLock_k(PKSPIN_LOCK SpinLock, PKLOCK_QUEUE_HANDLE H) {
    if (H) { H->LockPtr = SpinLock; H->OldIrql = KeAcquireSpinLockRaiseToDpc_k(SpinLock); }
}
void NTAPI KeReleaseInStackQueuedSpinLock_k(PKLOCK_QUEUE_HANDLE H) {
    if (H) KeReleaseSpinLock_k(H->LockPtr, H->OldIrql);
}

// ============================================================================
//  FASE FUNDACAO (Item 1) — IRQL real. Espelhado em gs:[0x60] (KPCR.Irql, que
//  drivers leem direto via gs) E em CR8/TPR. CR8=N segura IRQs de classe <=N
//  (vetor>>4); o timer 0xD1 (classe 13) continua firando em DISPATCH, mantendo
//  o relogio. NAO usamos cli/sti: o bloqueio de PREEMPCAO em DISPATCH vem no
//  Item 2 (o ISR do timer passa a checar o IRQL). Nenhum caller interno usa
//  estas funcoes (so drivers, que sobem apos kpcr_init) -> aditivo/seguro.
// ============================================================================
KIRQL NTAPI KeGetCurrentIrql_k(void) {
    uint8_t v; __asm__ volatile ("mov %%gs:0x60, %0" : "=r"(v));
    return (KIRQL)v;
}
static inline void ki_irql_write(KIRQL v) {
    __asm__ volatile ("mov %0, %%gs:0x60" :: "r"(v) : "memory");
    __asm__ volatile ("mov %0, %%cr8"     :: "r"((uint64_t)v));
}
void NTAPI KeRaiseIrql_k(KIRQL NewIrql, KIRQL* OldIrql) {
    KIRQL old = KeGetCurrentIrql_k();
    if (OldIrql) *OldIrql = old;
    if (NewIrql > old) ki_irql_write(NewIrql);   // raise so sobe
}
KIRQL NTAPI KfRaiseIrql_k(KIRQL NewIrql) { KIRQL old; KeRaiseIrql_k(NewIrql, &old); return old; }
KIRQL NTAPI KeRaiseIrqlToDpcLevel_k(void) { KIRQL old; KeRaiseIrql_k(DISPATCH_LEVEL, &old); return old; }
void NTAPI KeLowerIrql_k(KIRQL NewIrql) {
    // FASE FUNDACAO (Item 2): ao cair abaixo de DISPATCH, drena a fila de DPC
    // pendente do CPU corrente (enquanto ainda em DISPATCH), depois baixa.
    KIRQL cur = KeGetCurrentIrql_k();
    if (cur >= DISPATCH_LEVEL && NewIrql < DISPATCH_LEVEL) {
        extern void KiCheckDpcDrain(void);
        KiCheckDpcDrain();
    }
    ki_irql_write(NewIrql);
}
void NTAPI KfLowerIrql_k(KIRQL NewIrql) { KeLowerIrql_k(NewIrql); }

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
