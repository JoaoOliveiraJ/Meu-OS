#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  dxgmms.h  —  DirectX Memory Manager Subsystem (estilo dxgmms2.sys do W10/11).
//
//  Cuida da memoria de video: tracking de residency (RAM vs VRAM), GART/
//  paging entre as duas, e lock/unlock pra DMA. No Windows real e separado
//  do dxgkrnl porque a "policy" de paginacao e completamente diferente
//  da "policy" de despacho de IRPs — DXGMMS gerencia gigabytes de
//  alocacoes que entram/saem da VRAM dinamicamente.
//
//  No nosso OS (LFB Bochs VBE, sem VRAM dedicada) DXGMMS e um "page tracker"
//  simples: registra a alocacao, marca como residente, e o lock/unlock e
//  no-op (esta tudo na RAM mesmo, mapeada 1:1 pelo kernel). Stub seguro
//  que vai crescer quando tivermos um KMD com aperture/GART real.
// ============================================================================

// Estados de residency (espelha D3DKMT_RESIDENCYSTATUS do WDDM).
typedef enum {
    DXGMMS_RESIDENT_NONE    = 0,   // alocacao recem-criada, ainda nao paginada
    DXGMMS_RESIDENT_IN_RAM  = 1,   // esta na RAM do sistema (paginada out de VRAM)
    DXGMMS_RESIDENT_IN_VRAM = 2,   // esta na VRAM (ideal para uso direto da GPU)
    DXGMMS_RESIDENT_LOCKED  = 3,   // travada pra DMA (CPU pode tocar com seguranca)
} DXGMMS_RESIDENCY_STATE;

// Descritor de uma alocacao gerenciada pelo dxgmms.
typedef struct DXGMMS_RESIDENCY {
    int                    in_use;        // 1 se o slot esta vivo
    uint64_t               size;          // tamanho em bytes
    PVOID                  base;          // endereco virtual da alocacao
    DXGMMS_RESIDENCY_STATE state;         // estado atual de residency
    uint32_t               lock_count;    // contador (lock/unlock recursivo)
    uint64_t               allocation_id; // id unico no pool
} DXGMMS_RESIDENCY;

// ============================================================================
//  APIs exportadas pelo dxgmms — usadas pelo dxgkrnl e por DLLs UMD.
//  NTAPI (ms_abi) — mesma ABI que os exports de ntoskrnl.
// ============================================================================

// Inicializa o page tracker. Chamado depois do heap_init (precisa do kmalloc).
NTSTATUS NTAPI DxgMmsInitialize(void);

// Reserva uma alocacao: pega memoria do heap (ExAllocatePool no NT real),
// marca como DXGMMS_RESIDENT_IN_RAM e devolve o descritor pelo out param.
// Caminho seguro: sem memoria suficiente retorna STATUS_NO_MEMORY com out=NULL.
NTSTATUS NTAPI DxgMmsAllocate(uint64_t size, DXGMMS_RESIDENCY** out);

// Libera uma alocacao. Decrementa o lock_count ate 0 antes de devolver
// a memoria ao heap. Tolerante a NULL/double-free (loga e retorna SUCCESS).
NTSTATUS NTAPI DxgMmsFree(DXGMMS_RESIDENCY* res);

// Trava a alocacao pra DMA: garante que a base esta em RAM e nao sera
// movida pelo GC enquanto durar. Incrementa lock_count.
NTSTATUS NTAPI DxgMmsLock(DXGMMS_RESIDENCY* res);

// Destrava a alocacao (libera o GC pra mover/paginar). Decrementa lock_count.
NTSTATUS NTAPI DxgMmsUnlock(DXGMMS_RESIDENCY* res);

// Shutdown do subsistema. Libera todas as alocacoes vivas.
NTSTATUS NTAPI DxgMmsShutdown(void);
