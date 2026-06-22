#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 7 — Section Objects (memoria compartilhada estilo NtCreateSection).
//
//  Section = "objeto de mapeamento de arquivo" no NT (CreateFileMappingW na
//  Win32). Drivers usam isto p/ alocar um pool de bytes que pode ser mapeado
//  no kernel E no usuario com o mesmo conteudo. Como nosso espaco e identidade
//  -mapeado (1 GiB compartilhado entre kernel e ring 3), o "map view" devolve
//  o proprio buffer base — kernel e usuario veem os MESMOS bytes (igual ao
//  efeito de uma section nao-paginada). TODO: quando isolamento por-processo
//  for total, mapear a section por PML4.
// ============================================================================

typedef struct _SECTION_OBJECT {
    uint64_t Size;
    void*    Base;          // ponteiro kmalloc'd (identidade)
    uint32_t Protect;
    int      Inherited;     // ViewShare/ViewUnmap
    char     Name[64];      // opcional (\BaseNamedObjects\Nome)
} SECTION_OBJECT, *PSECTION_OBJECT;

NTSTATUS NTAPI NtCreateSection_k(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes,
                                 PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection,
                                 ULONG AllocationAttributes, HANDLE FileHandle);
NTSTATUS NTAPI NtOpenSection_k(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS NTAPI NtMapViewOfSection_k(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID* BaseAddress,
                                    uint64_t ZeroBits, uint64_t CommitSize, PLARGE_INTEGER SectionOffset,
                                    uint64_t* ViewSize, SECTION_INHERIT InheritDisposition,
                                    ULONG AllocationType, ULONG Win32Protect);
NTSTATUS NTAPI NtUnmapViewOfSection_k(HANDLE ProcessHandle, PVOID BaseAddress);

// Lado kernel: cria uma section anonima (sem handle) — usada p/ comunicacao
// rapida entre kernel e ring 3 do mesmo processo, igual no NT.
PSECTION_OBJECT section_create_anon(uint64_t size, const char* name_opt);
void            section_destroy(PSECTION_OBJECT s);

// Mm* helpers usados pelos drivers (lado kernel):
PHYSICAL_ADDRESS NTAPI MmGetPhysicalAddress_k(PVOID BaseAddress);
PVOID            NTAPI MmMapIoSpace_k(PHYSICAL_ADDRESS PhysicalAddress, SIZE_T NumberOfBytes, MEMORY_CACHING_TYPE Caching);
void             NTAPI MmUnmapIoSpace_k(PVOID BaseAddress, SIZE_T NumberOfBytes);
PVOID            NTAPI MmAllocateContiguousMemory_k(SIZE_T NumberOfBytes, PHYSICAL_ADDRESS HighestAcceptableAddress);
void             NTAPI MmFreeContiguousMemory_k(PVOID BaseAddress);
PVOID            NTAPI MmAllocateNonCachedMemory_k(SIZE_T NumberOfBytes);
void             NTAPI MmFreeNonCachedMemory_k(PVOID BaseAddress, SIZE_T NumberOfBytes);
NTSTATUS         NTAPI MmProtectMdlSystemAddress_k(PVOID Mdl, ULONG NewProtect);
BOOLEAN          NTAPI MmIsAddressValid_k(PVOID VirtualAddress);
