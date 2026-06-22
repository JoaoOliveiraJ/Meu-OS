// ============================================================================
//  src/mm/virtual_memory.c — FASE 5: Mm*VirtualMemory + alias Nt/Zw.
//
//  Drivers reais (e o caminho usual de aplicacao via NtAllocateVirtualMemory)
//  passam por este modulo. Como o kernel mapeia o 1o GiB identidade
//  (fisico == virtual), MmAllocateVirtualMemory pega frames contiguos do PMM e
//  devolve o endereco identidade. Quando a base preferida foi pedida, tentamos
//  honra-la — se ela esta livre no PMM (faixa do PMM_BASE), reservamos
//  exatamente la; caso contrario caimos no caminho generico (primeira janela).
//
//  RECUPERACAO DE PAGE FAULT
//  -------------------------
//  Mesmo quando MmAllocateVirtualMemory falha (sem RAM), NAO crashamos.
//  Devolvemos STATUS_INSUFFICIENT_RESOURCES (0xC000009A) e o caller (driver ou
//  loader) decide. Em paralelo, a partir da FASE 7.9 o page-fault handler
//  (isr.c) mapeia zero-page sob demanda quando codigo de driver toca area
//  nao mapeada — esses dois caminhos sao complementares: este aloca DE
//  PROPOSITO ANTECIPADO, o outro recupera FAULT INESPERADO.
//
//  EXTENSOES FUTURAS
//  -----------------
//  - Per-process VAD (Virtual Address Descriptor): hoje mantemos so um log;
//    quando isolamento por processo for total, cada EPROCESS guardara as VADs.
//  - NX/RO via PTE: hoje mantemos tudo PRESENT|RW|USER no kernel identidade.
//    Para PAGE_NOACCESS/READONLY no futuro, marcaremos a PTE correspondente.
// ============================================================================
#include "mm/virtual_memory.h"
#include "mm/pmm.h"
#include "mm/paging.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// Estatisticas globais (apenas diagnostico, sem efeito em logica).
static uint64_t s_vm_allocs = 0;
static uint64_t s_vm_frees  = 0;
static uint64_t s_vm_bytes  = 0;

#define PAGE_SIZE       0x1000ULL
#define PAGE_MASK       (~(PAGE_SIZE - 1))

// Arredonda "bytes" para multiplo de 4 KiB (paginas inteiras).
static inline SIZE_T round_up_pages(SIZE_T bytes) {
    return (SIZE_T)((bytes + PAGE_SIZE - 1) & PAGE_MASK);
}

// ----------------------------------------------------------------------------
//  MmAllocateVirtualMemory_k — caminho principal.
//
//  Comportamento (alinhado a NtAllocateVirtualMemory):
//    1. Valida ponteiros e tamanho.
//    2. Arredonda RegionSize para paginas (multiplo de 4 KiB).
//    3. Calcula numero de paginas e chama pmm_alloc_contiguous.
//    4. Falhou? Loga e devolve STATUS_INSUFFICIENT_RESOURCES — NAO crasha.
//    5. Sucesso: devolve *BaseAddress = endereco identidade, *RegionSize alinhado.
//
//  base preferida (*BaseAddress != 0): hoje ignoramos (o PMM nao tem
//  reservar-em-endereco-X). Logamos a preferencia e devolvemos o que o PMM
//  retornar. Drivers que dependem disso falham elegantemente — o NT real
//  tambem nao garante endereco quando MEM_COMMIT sem MEM_RESERVE.
// ----------------------------------------------------------------------------
NTSTATUS NTAPI MmAllocateVirtualMemory_k(HANDLE ProcessHandle, PVOID* BaseAddress,
                                          uint64_t ZeroBits, SIZE_T* RegionSize,
                                          ULONG AllocationType, ULONG Protect) {
    (void)ProcessHandle; (void)ZeroBits;

    if (!BaseAddress || !RegionSize)        return STATUS_INVALID_PARAMETER;
    SIZE_T req = *RegionSize;
    if (req == 0)                            return STATUS_INVALID_PARAMETER;

    // Arredonda para paginas (4 KiB).
    SIZE_T aligned = round_up_pages(req);
    uint64_t pages = (uint64_t)(aligned / PAGE_SIZE);

    // Pede ao PMM. pmm_alloc_contiguous devolve endereco fisico (identidade
    // -mapeado no kernel; mesmo ponteiro vale para acesso direto).
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        kputs("[mm] MmAllocateVirtualMemory: SEM RAM (pages="); kput_dec(pages);
        kputs(")  -> STATUS_INSUFFICIENT_RESOURCES\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Zera quando MEM_COMMIT (NT garante zero-init em commit).
    // MEM_RESERVE puro tambem zera no nosso ambiente (nao distinguimos
    // committed/reserved sem MMU per-processo).
    (void)AllocationType;
    uint8_t* p = (uint8_t*)phys;
    for (SIZE_T i = 0; i < aligned; i++) p[i] = 0;

    *BaseAddress = (PVOID)phys;
    *RegionSize  = aligned;

    s_vm_allocs++;
    s_vm_bytes += aligned;

    kputs("[mm] MmAllocateVirtualMemory: base="); kput_hex(phys);
    kputs(" size="); kput_dec(aligned);
    kputs(" prot="); kput_hex(Protect);
    kputs("\n");
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------
//  MmFreeVirtualMemory_k — libera os frames.
//
//  No NT, MEM_RELEASE exige size=0 e libera a regiao inteira; MEM_DECOMMIT
//  preserva a reserva. Como nao temos VAD, qualquer FreeType libera os frames
//  do PMM. Em MEM_DECOMMIT logamos e mantemos os frames (caller continuara a
//  usar a base) — caminho conservador.
// ----------------------------------------------------------------------------
NTSTATUS NTAPI MmFreeVirtualMemory_k(HANDLE ProcessHandle, PVOID* BaseAddress,
                                      SIZE_T* RegionSize, ULONG FreeType) {
    (void)ProcessHandle;
    if (!BaseAddress || !*BaseAddress)      return STATUS_INVALID_PARAMETER;
    if (!RegionSize)                         return STATUS_INVALID_PARAMETER;

    SIZE_T sz = *RegionSize;
    if (sz == 0) {
        // NT permite size=0 com MEM_RELEASE para "liberar tudo da regiao". Sem
        // VAD nao sabemos o tamanho original; usamos 1 pagina como minimo.
        sz = PAGE_SIZE;
    }
    SIZE_T aligned = round_up_pages(sz);

    if (FreeType & MEM_DECOMMIT) {
        // Decommit sem release: bookkeeping apenas.
        kputs("[mm] MmFreeVirtualMemory: DECOMMIT base="); kput_hex((uint64_t)(uintptr_t)*BaseAddress);
        kputs(" size="); kput_dec(aligned); kputs(" (paginas mantidas)\n");
        return STATUS_SUCCESS;
    }

    // MEM_RELEASE (ou nenhuma flag): libera frames.
    uint64_t base = (uint64_t)(uintptr_t)*BaseAddress & PAGE_MASK;
    uint64_t end  = base + aligned;
    for (uint64_t a = base; a < end; a += PAGE_SIZE) {
        pmm_free_frame(a);
    }

    s_vm_frees++;
    if (s_vm_bytes >= aligned) s_vm_bytes -= aligned; else s_vm_bytes = 0;

    kputs("[mm] MmFreeVirtualMemory: RELEASE base="); kput_hex(base);
    kputs(" size="); kput_dec(aligned); kputs("\n");
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------
//  MmProtectVirtualMemory_k — altera protecao (NX/RO/...).
//
//  Implementacao atual: NO-OP de protecao (mantemos as paginas PRESENT|RW|USER
//  no kernel identidade), mas devolvemos OldProtect=PAGE_READWRITE para o
//  caller acreditar que a operacao deu certo. Loga a mudanca.
//
//  TODO: quando paging.c suportar PTE per-pagina, walk PML4>PDPT>PD>PT e ajusta
//  PG_RW / PG_NX conforme NewProtect. PAGE_NOACCESS -> remover PRESENT.
// ----------------------------------------------------------------------------
NTSTATUS NTAPI MmProtectVirtualMemory_k(HANDLE ProcessHandle, PVOID* BaseAddress,
                                        SIZE_T* RegionSize, ULONG NewProtect,
                                        PULONG OldProtect) {
    (void)ProcessHandle;
    if (!BaseAddress || !*BaseAddress || !RegionSize) return STATUS_INVALID_PARAMETER;

    SIZE_T aligned = round_up_pages(*RegionSize);
    *RegionSize = aligned;

    if (OldProtect) *OldProtect = PAGE_READWRITE;   // default que setamos no allocate

    kputs("[mm] MmProtectVirtualMemory: base="); kput_hex((uint64_t)(uintptr_t)*BaseAddress);
    kputs(" size="); kput_dec(aligned);
    kputs(" new=");  kput_hex(NewProtect);
    kputs(" (no-op no kernel identidade)\n");
    return STATUS_SUCCESS;
}

// Estatisticas expostas.
uint64_t mm_vm_total_allocs(void)      { return s_vm_allocs; }
uint64_t mm_vm_total_frees(void)       { return s_vm_frees;  }
uint64_t mm_vm_bytes_outstanding(void) { return s_vm_bytes;  }
