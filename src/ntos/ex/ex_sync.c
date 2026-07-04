// ============================================================================
//  ex_sync.c — FASE FUNDACAO (Item 7): primitivos Ex reais (flag-gated).
//
//  Construidos sobre os KEVENT/KSEMAPHORE/spinlocks reais (Itens 3/5). O pintok
//  chama ExAcquireFastMutex(Unsafe)/ExAcquireResource*Lite — no modo legado
//  (ke_legacy_active: flag manual OU pintok rodando) eles voltam ao ANTIGO
//  (no-op / retorna FALSE, como os stubs) -> trajetoria do pintok preservada.
// ============================================================================
#include <stdint.h>
#include "ntddk.h"
#include "ke/sync.h"
#include "ke/sched.h"
#include "ex/ex_sync.h"

extern void  kputs(const char* s);
extern void  kput_hex(uint64_t v);
extern int   ke_legacy_active(void);
extern void* kmalloc(uint64_t);
extern void  kfree(void*);

// ---------------------------------------------------------------- FAST_MUTEX
void NTAPI ExInitializeFastMutex_k(PFAST_MUTEX M) {
    if (!M) return;
    M->Count = 1; M->Owner = 0; M->Contention = 0;
    KeInitializeEvent_k(&M->Event, SynchronizationEvent, 0);
}
void NTAPI ExAcquireFastMutex_k(PFAST_MUTEX M) {
    if (!M || ke_legacy_active()) return;   // ANTIGO: no-op (como o stub)
    if (__atomic_sub_fetch(&M->Count, 1, __ATOMIC_ACQUIRE) < 0) {
        M->Contention++;
        KeWaitForSingleObject_k(&M->Event, 0, 0, 0, 0);   // bloqueia (worker) / auto-resolve
    }
    M->Owner = ki_current_thread();
}
void NTAPI ExReleaseFastMutex_k(PFAST_MUTEX M) {
    if (!M || ke_legacy_active()) return;
    M->Owner = 0;
    if (__atomic_add_fetch(&M->Count, 1, __ATOMIC_RELEASE) <= 0)
        KeSetEvent_k(&M->Event, 0, 0);
}
BOOLEAN NTAPI ExTryToAcquireFastMutex_k(PFAST_MUTEX M) {
    if (!M) return 0;
    if (ke_legacy_active()) return 1;
    LONG expected = 1;
    return __atomic_compare_exchange_n(&M->Count, &expected, 0, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 1 : 0;
}
void NTAPI ExAcquireFastMutexUnsafe_k(PFAST_MUTEX M) { ExAcquireFastMutex_k(M); }
void NTAPI ExReleaseFastMutexUnsafe_k(PFAST_MUTEX M) { ExReleaseFastMutex_k(M); }

// ------------------------------------------------------------------ ERESOURCE
void NTAPI ExInitializeResourceLite_k(PERESOURCE R) {
    if (!R) return;
    R->ActiveCount = 0; R->ExclusiveWaiters = 0; R->SharedWaiters = 0; R->OwnerThread = 0;
    KeInitializeEvent_k(&R->ExclusiveEvent, SynchronizationEvent, 0);
    KeInitializeSemaphore_k(&R->SharedSem, 0, 0x7fffffff);
}
BOOLEAN NTAPI ExAcquireResourceExclusiveLite_k(PERESOURCE R, BOOLEAN Wait) {
    if (!R) return 0;
    if (ke_legacy_active()) return 0;   // ANTIGO: stub retornava 0 (FALSE)
    for (;;) {
        LONG expected = 0;
        if (__atomic_compare_exchange_n(&R->ActiveCount, &expected, -1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            R->OwnerThread = ki_current_thread();
            return 1;
        }
        if (!Wait) return 0;
        KeWaitForSingleObject_k(&R->ExclusiveEvent, 0, 0, 0, 0);
    }
}
BOOLEAN NTAPI ExAcquireResourceSharedLite_k(PERESOURCE R, BOOLEAN Wait) {
    if (!R) return 0;
    if (ke_legacy_active()) return 0;
    for (;;) {
        LONG cur = __atomic_load_n(&R->ActiveCount, __ATOMIC_ACQUIRE);
        if (cur >= 0) {
            if (__atomic_compare_exchange_n(&R->ActiveCount, &cur, cur + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return 1;
            continue;   // corrida: re-tenta
        }
        if (!Wait) return 0;
        KeWaitForSingleObject_k(&R->ExclusiveEvent, 0, 0, 0, 0);
    }
}
void NTAPI ExReleaseResourceLite_k(PERESOURCE R) {
    if (!R || ke_legacy_active()) return;
    LONG cur = __atomic_load_n(&R->ActiveCount, __ATOMIC_ACQUIRE);
    if (cur == -1) { R->OwnerThread = 0; __atomic_store_n(&R->ActiveCount, 0, __ATOMIC_RELEASE); }
    else if (cur > 0) __atomic_sub_fetch(&R->ActiveCount, 1, __ATOMIC_RELEASE);
    KeSetEvent_k(&R->ExclusiveEvent, 0, 0);   // acorda waiters
}
void NTAPI ExDeleteResourceLite_k(PERESOURCE R) { (void)R; }

// ------------------------------------------------------------------ Lookaside
void NTAPI ExInitializeNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L, PVOID A, PVOID F, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth) {
    (void)A; (void)F; (void)Flags; (void)Depth;
    if (!L) return;
    L->ListHead = 0; L->Lock = 0; L->Size = (uint32_t)Size; L->Tag = Tag; L->Depth = 0;
}
PVOID NTAPI ExAllocateFromNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L) {
    if (!L) return 0;
    KIRQL old = KeAcquireSpinLockRaiseToDpc_k(&L->Lock);
    void* p = L->ListHead;
    if (p) { L->ListHead = *(void**)p; if (L->Depth) L->Depth--; }
    KeReleaseSpinLock_k(&L->Lock, old);
    if (!p) p = kmalloc(L->Size ? L->Size : 16);   // miss -> pool/heap
    return p;
}
void NTAPI ExFreeToNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L, PVOID Entry) {
    if (!L || !Entry) return;
    KIRQL old = KeAcquireSpinLockRaiseToDpc_k(&L->Lock);
    *(void**)Entry = L->ListHead;   // empilha (LIFO)
    L->ListHead = Entry;
    L->Depth++;
    KeReleaseSpinLock_k(&L->Lock, old);
}
void NTAPI ExDeleteNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L) {
    if (!L) return;
    void* p = L->ListHead;
    while (p) { void* nx = *(void**)p; kfree(p); p = nx; }
    L->ListHead = 0;
}

// ------------------------------------------------------- ExInterlocked list ops
void NTAPI ExInterlockedInsertTailList_k(PLIST_ENTRY Head, PLIST_ENTRY Entry, PKSPIN_LOCK Lock) {
    KIRQL old = KeAcquireSpinLockRaiseToDpc_k(Lock);
    Entry->Blink = Head->Blink; Entry->Flink = Head;
    Head->Blink->Flink = Entry; Head->Blink = Entry;
    KeReleaseSpinLock_k(Lock, old);
}
void NTAPI ExInterlockedInsertHeadList_k(PLIST_ENTRY Head, PLIST_ENTRY Entry, PKSPIN_LOCK Lock) {
    KIRQL old = KeAcquireSpinLockRaiseToDpc_k(Lock);
    Entry->Flink = Head->Flink; Entry->Blink = Head;
    Head->Flink->Blink = Entry; Head->Flink = Entry;
    KeReleaseSpinLock_k(Lock, old);
}
PLIST_ENTRY NTAPI ExInterlockedRemoveHeadList_k(PLIST_ENTRY Head, PKSPIN_LOCK Lock) {
    KIRQL old = KeAcquireSpinLockRaiseToDpc_k(Lock);
    PLIST_ENTRY e = 0;
    if (Head->Flink != Head) { e = Head->Flink; e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink; }
    KeReleaseSpinLock_k(Lock, old);
    return e;
}

// ------------------------------------------------------------- ExRaiseStatus
void NTAPI ExRaiseStatus_k(NTSTATUS Status) {
    if (ke_legacy_active()) return;   // ANTIGO: no-op (stub retornava)
    kputs("[ex] ExRaiseStatus 0x"); kput_hex((uint64_t)(uint32_t)Status);
    kputs(" — sem SEH unwind real; halt.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

// --------------------------------------------------------------- Prova de boot
static void ex_test(void* a) {
    (void)a;
    static FAST_MUTEX m;
    ExInitializeFastMutex_k(&m);
    ExAcquireFastMutex_k(&m);
    BOOLEAN held = ExTryToAcquireFastMutex_k(&m) ? 0 : 1;   // preso -> Try FALHA -> held=1
    ExReleaseFastMutex_k(&m);
    BOOLEAN free_ok = ExTryToAcquireFastMutex_k(&m);        // livre -> Try SUCEDE
    ExReleaseFastMutex_k(&m);
    static NPAGED_LOOKASIDE_LIST la;
    ExInitializeNPagedLookasideList_k(&la, 0, 0, 0, 64, 0x74736554, 0);
    void* p1 = ExAllocateFromNPagedLookasideList_k(&la);
    ExFreeToNPagedLookasideList_k(&la, p1);
    void* p2 = ExAllocateFromNPagedLookasideList_k(&la);   // deve reusar p1
    if (held && free_ok && p1 && p2 == p1)
        kputs("[ex-test] FAST_MUTEX (try locked/free) + lookaside (reuse) OK\n");
    else {
        kputs("[ex-test] FALHOU held="); kput_hex(held);
        kputs(" free="); kput_hex(free_ok);
        kputs(" reuse="); kput_hex(p2 == p1); kputs("\n");
    }
}
void KiExSelfTestSpawn(void) {
    ki_thread_t* t = ki_create_thread(ex_test, 0, 8, 0);
    if (t) ki_ready_thread(t);
}
