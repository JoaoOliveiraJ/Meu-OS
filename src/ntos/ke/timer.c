// ============================================================================
//  timer.c — FASE FUNDACAO (Item 6): KTIMER real (flag-gated).
//
//  Lista global de timers armados; KiTimerTick (chamado do ISR do timer no CPU0)
//  expira os vencidos: sinaliza (acorda waiters via Item 5) + enfileira o DPC
//  opcional; re-arma os periodicos. No modo legado (ke_legacy_active — flag
//  manual OU pintok rodando) KeSetTimer volta ao ANTIGO (retorna FALSE / nao
//  arma), preservando a trajetoria do pintok.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "ntddk.h"
#include "ke/timer.h"
#include "ke/dpc.h"
#include "ke/sched.h"
#include "ke/sync.h"

extern void kputs(const char* s);
extern volatile uint64_t g_ticks;
extern int  ke_legacy_active(void);
extern int  ki_wake(void* obj, int wake_all);

static LIST_ENTRY   s_timers;
static volatile int s_tlock;
static int          s_ready = 0;

static void tl(void) { while (__atomic_test_and_set(&s_tlock, __ATOMIC_ACQUIRE)) __asm__ volatile ("pause"); }
static void tu(void) { __atomic_clear(&s_tlock, __ATOMIC_RELEASE); }

void KiInitializeTimerSubsystem(void) {
    s_timers.Flink = &s_timers;
    s_timers.Blink = &s_timers;
    s_tlock = 0;
    s_ready = 1;
    kputs("[timer] subsistema KTIMER inicializado\n");
}

// DueTime NT (unidades de 100ns; negativo=relativo, positivo=absoluto epoch2026)
// -> tick alvo (g_ticks @ 100 Hz; 1 tick = 10 ms = 100000 unidades de 100ns).
static uint64_t due_to_tick(LARGE_INTEGER due) {
    if (due.QuadPart < 0) {
        uint64_t t = (uint64_t)(-due.QuadPart) / 100000ULL;
        return g_ticks + (t ? t : 1);
    }
    static const uint64_t EPOCH_2026 = 132934176000000000ULL;   // 1 Jan 2026 (~), bate com KeQuerySystemTime
    uint64_t ns100 = (uint64_t)due.QuadPart;
    return (ns100 <= EPOCH_2026) ? g_ticks : g_ticks + (ns100 - EPOCH_2026) / 100000ULL;
}

static void timer_unlink(PKTIMER t) {
    if (t->TimerListEntry.Flink) {
        t->TimerListEntry.Blink->Flink = t->TimerListEntry.Flink;
        t->TimerListEntry.Flink->Blink = t->TimerListEntry.Blink;
        t->TimerListEntry.Flink = 0;
    }
}
static void timer_link(PKTIMER t) {
    t->TimerListEntry.Blink = s_timers.Blink;
    t->TimerListEntry.Flink = &s_timers;
    s_timers.Blink->Flink = &t->TimerListEntry;
    s_timers.Blink = &t->TimerListEntry;
}

void NTAPI KeInitializeTimerEx_k(PKTIMER Timer, TIMER_TYPE Type) {
    if (!Timer) return;
    for (int i = 0; i < (int)sizeof(KTIMER); i++) ((volatile uint8_t*)Timer)[i] = 0;
    if (ke_legacy_active()) return;   // ANTIGO: so zera (como o stub NT_KeInitializeTimer)
    Timer->Header.Type = (Type == SynchronizationTimer) ? 9 : 8;
    Timer->Header.WaitListHead.Flink = &Timer->Header.WaitListHead;
    Timer->Header.WaitListHead.Blink = &Timer->Header.WaitListHead;
    Timer->TimerListEntry.Flink = 0;   // fora da lista
}
void NTAPI KeInitializeTimer_k(PKTIMER Timer) { KeInitializeTimerEx_k(Timer, NotificationTimer); }

BOOLEAN NTAPI KeSetTimerEx_k(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc) {
    if (!Timer) return 0;
    if (ke_legacy_active() || !s_ready) return 0;   // ANTIGO: KeSetTimer devolvia FALSE
    tl();
    BOOLEAN was_set = (Timer->TimerListEntry.Flink != 0) ? 1 : 0;
    timer_unlink(Timer);
    Timer->DueTime.QuadPart   = (LONGLONG)due_to_tick(DueTime);
    Timer->Dpc                = Dpc;
    Timer->Period             = Period;
    Timer->Header.SignalState = 0;
    timer_link(Timer);
    tu();
    return was_set;
}
BOOLEAN NTAPI KeSetTimer_k(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc) {
    return KeSetTimerEx_k(Timer, DueTime, 0, Dpc);
}
BOOLEAN NTAPI KeCancelTimer_k(PKTIMER Timer) {
    if (!Timer) return 0;
    if (ke_legacy_active() || !s_ready) return 1;   // ANTIGO: retornava TRUE
    tl();
    BOOLEAN was_set = (Timer->TimerListEntry.Flink != 0) ? 1 : 0;
    timer_unlink(Timer);
    tu();
    return was_set;
}
BOOLEAN NTAPI KeReadStateTimer_k(PKTIMER Timer) {
    return (Timer && Timer->Header.SignalState) ? 1 : 0;
}

// Chamado do ISR do timer (CPU0, IF=0) a cada tick.
void KiTimerTick(void) {
    if (!s_ready) return;
    PKTIMER batch[16]; int n = 0;
    tl();
    LIST_ENTRY* e = s_timers.Flink;
    while (e && e != &s_timers && n < 16) {
        LIST_ENTRY* nx = e->Flink;
        PKTIMER t = (PKTIMER)((uint8_t*)e - offsetof(KTIMER, TimerListEntry));
        if ((uint64_t)t->DueTime.QuadPart <= g_ticks) { timer_unlink(t); batch[n++] = t; }
        e = nx;
    }
    tu();
    for (int i = 0; i < n; i++) {
        PKTIMER t = batch[i];
        t->Header.SignalState = 1;
        ki_wake(&t->Header, (t->Header.Type == 9) ? 0 : 1);   // sync: um; notificacao: todos
        if (t->Dpc) KeInsertQueueDpc_k((PKDPC)t->Dpc, 0, 0);
        if (t->Period > 0) {                                   // periodico: re-arma
            uint64_t ticks = (uint64_t)t->Period / 10;         // ms -> ticks (10ms)
            tl();
            t->DueTime.QuadPart   = (LONGLONG)(g_ticks + (ticks ? ticks : 1));
            t->Header.SignalState = 0;
            timer_link(t);
            tu();
        }
    }
}

// --- Prova de boot: worker arma um timer de 2s e bloqueia nele. -------------
static void tt_waiter(void* a) {
    (void)a;
    static KTIMER tmr;
    LARGE_INTEGER due; due.QuadPart = -20000000LL;   // 2 s relativo (100ns)
    KeInitializeTimer_k(&tmr);
    kputs("[timer-test] worker: arma timer 2s e bloqueia em KeWaitForSingleObject...\n");
    KeSetTimer_k(&tmr, due, 0);
    KeWaitForSingleObject_k(&tmr, 0, 0, 0, 0);       // bloqueia; expira -> acorda
    kputs("[timer-test] worker: timer EXPIROU e acordou -> KTIMER OK\n");
}
void KiTimerSelfTestSpawn(void) {
    ki_thread_t* t = ki_create_thread(tt_waiter, 0, 8, 0);
    if (t) ki_ready_thread(t);
}
