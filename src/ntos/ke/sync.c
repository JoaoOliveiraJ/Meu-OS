// ============================================================================
//  FASE 7 — implementacao dos primitivos de sincronizacao (KEVENT/KSPIN_LOCK/
//  KMUTEX/KSEMAPHORE/KeWait*). Sem escalonador: Sets marcam estado; Waits leem
//  estado e (se infinite) retornam STATUS_SUCCESS imediato (modo "auto-resolve").
// ============================================================================
#include "ke/sync.h"
#include "ke/sched.h"      // Item 5: ki_create_thread/ki_ready_thread p/ o auto-teste

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern volatile uint64_t g_ticks;   // PIT 100 Hz (cpu/pit.c)
extern volatile int g_pintok_trace;

// FASE FUNDACAO (Item 5) — waits bloqueantes: usam o scheduler (ke/sched.c).
extern int  ki_can_block(void);
extern void ki_block_current(void* obj);
extern int  ki_wake(void* obj, int wake_all);
extern void ki_yield_processor(void);
extern void NTAPI KeStallExecutionProcessor_k(ULONG MicroSeconds);
extern int  ke_legacy_active(void);   // 1 = modo ANTIGO (flag manual OU pintok rodando)
static inline uint64_t sc_irq_save(void) { uint64_t f; __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void sc_irq_restore(uint64_t f) { __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc"); }
// Consome o sinal conforme o tipo do objeto ao satisfazer um wait.
static void sc_consume(PDISPATCHER_HEADER hdr) {
    if (hdr->Type == NotificationEvent || hdr->Type == 8 /*TimerNotification*/) return;  // fica sinalizado
    if (hdr->Type == 5 /*SemaphoreObject*/) { if (hdr->SignalState > 0) hdr->SignalState--; return; }
    hdr->SignalState = 0;                                    // SyncEvent / Mutant / SyncTimer(9)
}

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
    // Item 5: acorda waiters (so no modo CORRETO; no legado ninguem bloqueia).
    // NotificationEvent acorda TODOS; SynchronizationEvent acorda UM.
    if (!ke_legacy_active())
        ki_wake(&Event->Header, Event->Header.Type == NotificationEvent ? 1 : 0);
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
    PDISPATCHER_HEADER hdr = (PDISPATCHER_HEADER)Object;
    for (;;) {
        uint64_t f = sc_irq_save();          // cli: checagem do sinal + bloqueio atomicos (UP)
        if (hdr->SignalState) { sc_consume(hdr); sc_irq_restore(f); return STATUS_SUCCESS; }
        if (Timeout && Timeout->QuadPart == 0) { sc_irq_restore(f); return STATUS_TIMEOUT; }  // poll
        // Sem sinal. Bloqueio real SO p/ espera infinita (Timeout==NULL) de uma
        // thread worker real. Casos de auto-resolve (comportamento ANTIGO —
        // nunca trava): (a) modo legado (flag OU pintok rodando); (b) timeout
        // finito; (c) contexto idle/boot que nao pode bloquear.
        if (ke_legacy_active() || Timeout != 0 || !ki_can_block()) { sc_irq_restore(f); return STATUS_SUCCESS; }
        ki_block_current(hdr);               // WAITING + insere na lista de espera
        ki_yield_processor();                // swap; volta quando acordado
        sc_irq_restore(f);                   // reabilita IF; re-loop re-checa o sinal
    }
}
NTSTATUS NTAPI KeWaitForMultipleObjects_k(ULONG Count, PVOID Object[], WAIT_TYPE WaitType,
                                          KWAIT_REASON Reason, KPROCESSOR_MODE Mode,
                                          BOOLEAN Alertable, PLARGE_INTEGER Timeout, PVOID WaitBlockArray) {
    (void)Reason; (void)Mode; (void)Alertable; (void)Timeout; (void)WaitBlockArray;
    // Modo legado (flag OU pintok): comportamento ANTIGO (retorna sucesso).
    if (ke_legacy_active()) return STATUS_SUCCESS;
    // Poll (multi-objeto NAO bloqueia nesta fase — evita travar em Object[0]).
    // Bloqueio multi-objeto real vem com o wait-block array (fase posterior).
    uint64_t f = sc_irq_save();
    if (WaitType == WaitAny) {
        for (ULONG i = 0; i < Count; i++) {
            PDISPATCHER_HEADER h = (PDISPATCHER_HEADER)Object[i];
            if (h && h->SignalState) { sc_consume(h); sc_irq_restore(f); return STATUS_SUCCESS; }
        }
    } else {   // WaitAll
        ULONG sig = 0;
        for (ULONG i = 0; i < Count; i++) {
            PDISPATCHER_HEADER h = (PDISPATCHER_HEADER)Object[i];
            if (h && h->SignalState) sig++;
        }
        if (sig == Count) {
            for (ULONG i = 0; i < Count; i++) sc_consume((PDISPATCHER_HEADER)Object[i]);
            sc_irq_restore(f); return STATUS_SUCCESS;
        }
    }
    sc_irq_restore(f);
    return STATUS_SUCCESS;   // auto-resolve (nada satisfeito)
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
    Mutex->OwnerThread = 0;
    if (!ke_legacy_active()) ki_wake(&Mutex->Header, 0);   // Item 5: acorda um waiter (modo correto)
    return prev;
}

void NTAPI KeInitializeSemaphore_k(PKSEMAPHORE Sem, LONG Count, LONG Limit) {
    if (!Sem) return;
    Sem->Header.Type = 5;   // SemaphoreObject (evita colisao com SynchronizationEvent=1)
    Sem->Header.SignalState = Count;
    Sem->Limit = Limit;
}
LONG NTAPI KeReleaseSemaphore_k(PKSEMAPHORE Sem, KPRIORITY Inc, LONG Adj, BOOLEAN Wait) {
    (void)Inc; (void)Wait;
    if (!Sem) return 0;
    LONG prev = Sem->Header.SignalState;
    Sem->Header.SignalState += Adj;
    if (!ke_legacy_active()) ki_wake(&Sem->Header, 1);   // Item 5: acorda waiters (modo correto)
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

// ============================================================================
//  FASE FUNDACAO (Item 5) — auto-teste de block+wake com 2 threads worker.
//  waiter bloqueia (infinito) num SynchronizationEvent; signaler espera ~50ms
//  (garante que o waiter bloqueou) e sinaliza. Se "ACORDOU" aparecer na serial,
//  o bloqueio real + wake funcionam. Se so "vai bloquear" aparecer, o waiter
//  ficou preso (isolado — nao trava o resto do sistema).
// ============================================================================
static KEVENT s_wt_evt;
static void wt_waiter(void* a) {
    (void)a;
    kputs("[wait-test] waiter: vai bloquear em KeWaitForSingleObject (infinito)...\n");
    KeWaitForSingleObject_k(&s_wt_evt, 0, 0, 0, 0);
    kputs("[wait-test] waiter: ACORDOU -> block+wake real OK\n");
}
static void wt_signaler(void* a) {
    (void)a;
    KeStallExecutionProcessor_k(50000);   // ~50 ms
    kputs("[wait-test] signaler: KeSetEvent (acorda o waiter)\n");
    KeSetEvent_k(&s_wt_evt, 0, 0);
}
void ki_wait_selftest_spawn(void) {
    KeInitializeEvent_k(&s_wt_evt, SynchronizationEvent, 0);
    ki_thread_t* a = ki_create_thread(wt_waiter, 0, 8, 0);
    ki_thread_t* b = ki_create_thread(wt_signaler, 0, 8, 0);
    if (a) ki_ready_thread(a);
    if (b) ki_ready_thread(b);
}
