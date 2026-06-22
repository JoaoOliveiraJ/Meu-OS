#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 5 (Mm* Virtual Memory) — APIs estilo NT que drivers e processos usam
//  para reservar/comprometer/proteger memoria virtual.
//
//  NT na Win32 expoe NtAllocateVirtualMemory (alias ZwAllocateVirtualMemory)
//  para drivers e tambem expoe MmAllocateVirtualMemory internamente. Nosso
//  ambiente e identidade-mapeado (1 GiB), entao "virtual" == "fisico"; o
//  alocador real e o PMM (pmm_alloc_contiguous), que devolve uma janela de
//  frames contiguos em [16 MiB .. 1 GiB).
//
//  A semantica das constantes AllocationType e Protect e a mesma do NT:
//    AllocationType: MEM_COMMIT (0x1000) | MEM_RESERVE (0x2000) | MEM_RESET (0x80000)
//    FreeType:       MEM_DECOMMIT (0x4000) | MEM_RELEASE (0x8000)
//    Protect:        PAGE_NOACCESS/READONLY/READWRITE/EXECUTE/... (ja em ntddk.h)
//
//  Como o ambiente atual NAO tem MMU por-processo robusta, MmProtect e apenas
//  bookkeeping (loga e devolve OldProtect = PAGE_READWRITE). Quando a fase de
//  paginacao por-processo amadurecer, aqui setamos NX/RO/USER bit nas PTEs.
// ============================================================================

// ===== Flags AllocationType / FreeType (subset NT). =====
#ifndef MEM_COMMIT
#define MEM_COMMIT      0x00001000
#define MEM_RESERVE     0x00002000
#define MEM_DECOMMIT    0x00004000
#define MEM_RELEASE     0x00008000
#define MEM_RESET       0x00080000
#define MEM_TOP_DOWN    0x00100000
#define MEM_LARGE_PAGES 0x20000000
#endif

#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#endif

// ============================================================================
//  MmAllocateVirtualMemory_k — aloca paginas comprometidas no espaco do
//  processo "ProcessHandle" (ignorado: usamos o espaco compartilhado).
//
//  - *BaseAddress IN: base preferida (0 = qualquer); OUT: base devolvida.
//  - ZeroBits: ignorado (heuristica NT antiga p/ alocacao em endereco baixo).
//  - *RegionSize IN: tamanho em bytes (sera arredondado para multiplo de 4 KiB);
//                OUT: tamanho efetivo alocado.
//  - AllocationType: combinacao de MEM_COMMIT/MEM_RESERVE.
//  - Protect: PAGE_READWRITE etc. (so log; sem MMU per-pagina por enquanto).
//
//  Retorno: STATUS_SUCCESS, STATUS_INVALID_PARAMETER, STATUS_INSUFFICIENT_RESOURCES.
// ============================================================================
NTSTATUS NTAPI MmAllocateVirtualMemory_k(HANDLE ProcessHandle, PVOID* BaseAddress,
                                          uint64_t ZeroBits, SIZE_T* RegionSize,
                                          ULONG AllocationType, ULONG Protect);

// MmFreeVirtualMemory_k — libera paginas alocadas por MmAllocateVirtualMemory_k.
//   FreeType: MEM_RELEASE (libera tudo) ou MEM_DECOMMIT (apenas marca livre).
NTSTATUS NTAPI MmFreeVirtualMemory_k(HANDLE ProcessHandle, PVOID* BaseAddress,
                                      SIZE_T* RegionSize, ULONG FreeType);

// MmProtectVirtualMemory_k — altera protecao das paginas.
//   *OldProtect OUT: protecao anterior (sempre PAGE_READWRITE aqui).
NTSTATUS NTAPI MmProtectVirtualMemory_k(HANDLE ProcessHandle, PVOID* BaseAddress,
                                        SIZE_T* RegionSize, ULONG NewProtect,
                                        PULONG OldProtect);

// Estatisticas para diagnostico (pool_dump-like).
uint64_t mm_vm_total_allocs(void);
uint64_t mm_vm_total_frees(void);
uint64_t mm_vm_bytes_outstanding(void);
