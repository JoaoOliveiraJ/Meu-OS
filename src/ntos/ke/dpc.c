// ============================================================================
//  dpc.c — FASE FUNDACAO (Item 2): DPC (Deferred Procedure Call) real.
//
//  Fila per-CPU mantida AQUI (fora do ki_prcb_t, p/ nao mexer no struct sensivel
//  a fusao SIMD do scheduler). Semantica:
//    - DPC enfileirado em IRQL < DISPATCH: dispara "inline" (raise DISPATCH ->
//      lower, e o lower drena via KiCheckDpcDrain).
//    - DPC enfileirado em IRQL >= DISPATCH: fica pendente; dispara quando o
//      codigo baixa o IRQL abaixo de DISPATCH (KeLowerIrql -> KiCheckDpcDrain).
//  NAO tocamos o ISR do timer. pintok NAO enfileira DPC (baseline) -> inerte p/
//  ele; so KeInitializeDpc (que ele chama) passa a stampar campos reais em vez
//  de zeros — verificado contra o baseline.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "ntddk.h"
#include "ke/sync.h"
#include "ke/sched.h"
#include "ke/dpc.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);
extern volatile int g_pintok_trace;

#define DPC_MAX_CPUS 8

static LIST_ENTRY   s_q[DPC_MAX_CPUS];
static volatile int s_lock[DPC_MAX_CPUS];
static volatile int s_pending[DPC_MAX_CPUS];
static int          s_ready = 0;

static void ql(int c) { while (__atomic_test_and_set(&s_lock[c], __ATOMIC_ACQUIRE)) __asm__ volatile ("pause"); }
static void qu(int c) { __atomic_clear(&s_lock[c], __ATOMIC_RELEASE); }

static int cur_cpu(void) {
    int c = (int)ki_current_cpu_index();
    return (c < 0 || c >= DPC_MAX_CPUS) ? 0 : c;
}

void KiInitializeDpcSubsystem(void) {
    for (int i = 0; i < DPC_MAX_CPUS; i++) {
        s_q[i].Flink = &s_q[i];
        s_q[i].Blink = &s_q[i];
        s_lock[i]    = 0;
        s_pending[i] = 0;
    }
    s_ready = 1;
    kputs("[dpc] subsistema DPC inicializado (fila per-CPU)\n");
}

void NTAPI KeInitializeDpc_k(PKDPC Dpc, PVOID DeferredRoutine, PVOID DeferredContext) {
    if (!Dpc) return;
    for (int i = 0; i < (int)sizeof(KDPC); i++) ((volatile uint8_t*)Dpc)[i] = 0;
    Dpc->Type            = 0x13;     // DpcObject
    Dpc->Importance      = 1;        // MediumImportance
    Dpc->Number          = 0xFFFF;   // nao enfileirado
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    if (g_pintok_trace) {
        kputs("  [trace] KeInitializeDpc routine=0x");
        kput_hex((uint64_t)(uintptr_t)DeferredRoutine); kputs("\n");
    }
}

void NTAPI KeSetImportanceDpc_k(PKDPC Dpc, int Importance) {
    if (Dpc) Dpc->Importance = (UCHAR)Importance;
}

void KiRetireDpcList(int cpu) {
    if (cpu < 0 || cpu >= DPC_MAX_CPUS || !s_ready) return;
    for (;;) {
        ql(cpu);
        LIST_ENTRY* head = &s_q[cpu];
        if (head->Flink == head) { s_pending[cpu] = 0; qu(cpu); return; }
        LIST_ENTRY* e = head->Flink;
        e->Blink->Flink = e->Flink;
        e->Flink->Blink = e->Blink;
        PKDPC dpc = (PKDPC)((uint8_t*)e - offsetof(KDPC, DpcListEntry));
        dpc->Number = 0xFFFF;
        PKDEFERRED_ROUTINE r = (PKDEFERRED_ROUTINE)dpc->DeferredRoutine;
        PVOID ctx = dpc->DeferredContext;
        PVOID a1  = dpc->SystemArgument1;
        PVOID a2  = dpc->SystemArgument2;
        qu(cpu);   // libera o lock ANTES de chamar (o DPC pode enfileirar outro)
        if (r) r(dpc, ctx, a1, a2);
    }
}

void KiCheckDpcDrain(void) {
    if (!s_ready) return;
    int c = cur_cpu();
    if (s_pending[c]) KiRetireDpcList(c);
}

BOOLEAN NTAPI KeInsertQueueDpc_k(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2) {
    if (!Dpc || !s_ready) return 0;
    int c = cur_cpu();
    ql(c);
    if (Dpc->Number != 0xFFFF) { qu(c); return 0; }   // ja na fila
    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;
    Dpc->Number = (USHORT)c;
    LIST_ENTRY* e    = &Dpc->DpcListEntry;
    LIST_ENTRY* head = &s_q[c];
    e->Blink = head->Blink;
    e->Flink = head;
    head->Blink->Flink = e;
    head->Blink        = e;
    s_pending[c] = 1;
    qu(c);
    // Se abaixo de DISPATCH, dispara agora: o par raise/lower drena a fila
    // (KeLowerIrql -> KiCheckDpcDrain). Se >= DISPATCH, espera o proximo lower.
    uint8_t irql; __asm__ volatile ("mov %%gs:0x60, %0" : "=r"(irql));
    if (irql < 2) {
        KIRQL old; KeRaiseIrql_k(DISPATCH_LEVEL, &old);
        KeLowerIrql_k(old);
    }
    return 1;
}

BOOLEAN NTAPI KeRemoveQueueDpc_k(PKDPC Dpc) {
    if (!Dpc || !s_ready) return 0;
    int c = (int)Dpc->Number;
    if (c < 0 || c >= DPC_MAX_CPUS) return 0;
    ql(c);
    if (Dpc->Number == 0xFFFF) { qu(c); return 0; }
    LIST_ENTRY* e = &Dpc->DpcListEntry;
    e->Blink->Flink = e->Flink;
    e->Flink->Blink = e->Blink;
    Dpc->Number = 0xFFFF;
    qu(c);
    return 1;
}

// --- Prova de boot: enfileira 1 DPC em PASSIVE -> deve disparar inline. -------
static volatile int s_proof = 0;
static void NTAPI dpc_proof_routine(PKDPC d, PVOID ctx, PVOID a1, PVOID a2) {
    (void)d; (void)a1; (void)a2;
    s_proof = (int)(uintptr_t)ctx;
}
void KiDpcSelfTest(void) {
    static KDPC t;
    s_proof = 0;
    KeInitializeDpc_k(&t, (PVOID)dpc_proof_routine, (PVOID)(uintptr_t)0x1234);
    KeInsertQueueDpc_k(&t, 0, 0);   // IRQL PASSIVE -> dispara inline (raise/lower)
    if (s_proof == 0x1234) kputs("[dpc] PROVA: DPC disparou inline em PASSIVE -> OK\n");
    else { kputs("[dpc] PROVA FALHOU: ran=0x"); kput_hex((uint64_t)(uint32_t)s_proof); kputs("\n"); }
}
