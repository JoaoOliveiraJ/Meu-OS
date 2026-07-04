#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  timer.h — FASE FUNDACAO (Item 6): KTIMER real (flag-gated).
// ============================================================================

void KiInitializeTimerSubsystem(void);
// Chamado do ISR do timer (CPU0) a cada tick: expira timers vencidos (sinaliza
// + acorda waiters + enfileira DPC; re-arma os periodicos).
void KiTimerTick(void);
// Prova de boot (worker espera num timer que expira).
void KiTimerSelfTestSpawn(void);

// API NT (drivers). No modo legado (ke_legacy_active) voltam ao comportamento
// antigo: KeSetTimer retorna FALSE, KeInitializeTimer zera, etc.
void    NTAPI KeInitializeTimer_k(PKTIMER Timer);
void    NTAPI KeInitializeTimerEx_k(PKTIMER Timer, TIMER_TYPE Type);
BOOLEAN NTAPI KeSetTimer_k(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
BOOLEAN NTAPI KeSetTimerEx_k(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc);
BOOLEAN NTAPI KeCancelTimer_k(PKTIMER Timer);
BOOLEAN NTAPI KeReadStateTimer_k(PKTIMER Timer);
