#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  ex_sync.h — FASE FUNDACAO (Item 7): primitivos Ex reais (flag-gated).
//  No modo legado (ke_legacy_active — flag manual OU pintok rodando) os que o
//  pintok chama (fast mutex, resource) voltam ao ANTIGO (no-op / FALSE).
// ============================================================================

// FAST_MUTEX
void    NTAPI ExInitializeFastMutex_k(PFAST_MUTEX M);
void    NTAPI ExAcquireFastMutex_k(PFAST_MUTEX M);
void    NTAPI ExReleaseFastMutex_k(PFAST_MUTEX M);
BOOLEAN NTAPI ExTryToAcquireFastMutex_k(PFAST_MUTEX M);
void    NTAPI ExAcquireFastMutexUnsafe_k(PFAST_MUTEX M);
void    NTAPI ExReleaseFastMutexUnsafe_k(PFAST_MUTEX M);

// ERESOURCE (simplificado)
void    NTAPI ExInitializeResourceLite_k(PERESOURCE R);
BOOLEAN NTAPI ExAcquireResourceExclusiveLite_k(PERESOURCE R, BOOLEAN Wait);
BOOLEAN NTAPI ExAcquireResourceSharedLite_k(PERESOURCE R, BOOLEAN Wait);
void    NTAPI ExReleaseResourceLite_k(PERESOURCE R);
void    NTAPI ExDeleteResourceLite_k(PERESOURCE R);

// Lookaside (NPaged)
void    NTAPI ExInitializeNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L, PVOID A, PVOID F, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth);
PVOID   NTAPI ExAllocateFromNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L);
void    NTAPI ExFreeToNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L, PVOID Entry);
void    NTAPI ExDeleteNPagedLookasideList_k(PNPAGED_LOOKASIDE_LIST L);

// ExInterlocked list ops
void        NTAPI ExInterlockedInsertHeadList_k(PLIST_ENTRY Head, PLIST_ENTRY Entry, PKSPIN_LOCK Lock);
void        NTAPI ExInterlockedInsertTailList_k(PLIST_ENTRY Head, PLIST_ENTRY Entry, PKSPIN_LOCK Lock);
PLIST_ENTRY NTAPI ExInterlockedRemoveHeadList_k(PLIST_ENTRY Head, PKSPIN_LOCK Lock);

// ExRaiseStatus (sem SEH unwind real)
void    NTAPI ExRaiseStatus_k(NTSTATUS Status);

// Prova de boot.
void KiExSelfTestSpawn(void);
