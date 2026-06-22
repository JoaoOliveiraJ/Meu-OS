// ============================================================================
//  FASE 7 — Ex*Pool* (apoiado no heap do kernel). Tag tracking + log p/
//  diferenciar Paged vs NonPaged (mesmo backing kmalloc, mas o tipo do pool e
//  preservado no cabecalho para auditar no pool_dump).
//
//  FASE 5 (Mm*): cabecalho expandido com PoolType para que pool_dump consiga
//  separar PagedPool / NonPagedPool nas estatisticas. Ainda alocamos do mesmo
//  heap (kmalloc), so registramos a categoria.
// ============================================================================
#include "ex/pool.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);

// Cabecalho oculto: ainda 16 bytes antes do bloco devolvido (struct packed).
// FASE 5: PoolType comprimido em 1 byte dentro do "Magic" (3 bytes restantes
// guardam o magic). Assim mantemos 16 bytes (16-aligned do kmalloc), sem
// invalidar layouts antigos.
typedef struct __attribute__((packed)) _POOL_HDR {
    ULONG    Tag;
    uint64_t Size;
    uint16_t Magic;     // 'PL'
    uint8_t  PoolType;  // POOL_TYPE original (NonPagedPool/PagedPool/etc.)
    uint8_t  Reserved;
} POOL_HDR;

#define POOL_MAGIC 0x4C50u    // 'PL' (mantem distincao do antigo 'POOL' 32-bit)

// FASE 5: contadores separados por categoria (Paged/NonPaged) para que o
// pool_dump consiga reportar bytes_outstanding de cada um. Mesmo backing
// kmalloc, mas a contabilidade reflete o uso real.
static uint64_t s_allocs       = 0;
static uint64_t s_frees        = 0;
static uint64_t s_bytes        = 0;
static uint64_t s_bytes_paged  = 0;
static uint64_t s_bytes_nonpag = 0;

PVOID NTAPI ExAllocatePool_k(POOL_TYPE PoolType, SIZE_T NumberOfBytes) {
    return ExAllocatePoolWithTag_k(PoolType, NumberOfBytes, 0x20656E6Eu);   // 'Non '
}
extern volatile int g_pintok_trace;
extern void kput_dec(uint64_t v);
PVOID NTAPI ExAllocatePoolWithTag_k(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag) {
    if (g_pintok_trace) {
        kputs("  [trace] ExAllocatePoolWithTag type="); kput_dec(PoolType);
        kputs(" bytes="); kput_dec(NumberOfBytes);
        kputs(" tag=0x"); kput_hex(Tag); kputs("\n");
    }
    if (NumberOfBytes == 0) return 0;
    void* raw = kmalloc(NumberOfBytes + sizeof(POOL_HDR));
    if (!raw) return 0;
    POOL_HDR* h = (POOL_HDR*)raw;
    h->Tag      = Tag;
    h->Size     = NumberOfBytes;
    h->Magic    = POOL_MAGIC;
    h->PoolType = (uint8_t)PoolType;
    h->Reserved = 0;
    s_allocs++;
    s_bytes += NumberOfBytes;
    // FASE 5: separa contagem Paged vs NonPaged. PoolType==1 (PagedPool).
    if ((uint8_t)PoolType == 1) s_bytes_paged  += NumberOfBytes;
    else                        s_bytes_nonpag += NumberOfBytes;
    return (PVOID)((uint8_t*)raw + sizeof(POOL_HDR));
}
PVOID NTAPI ExAllocatePool2_k(uint64_t Flags, SIZE_T NumberOfBytes, ULONG Tag) {
    (void)Flags;
    PVOID p = ExAllocatePoolWithTag_k(NonPagedPool, NumberOfBytes, Tag);
    // POOL_FLAG_ZERO_INIT (bit 0): zera o buffer.
    if (p && (Flags & 1)) for (SIZE_T i = 0; i < NumberOfBytes; i++) ((uint8_t*)p)[i] = 0;
    return p;
}
PVOID NTAPI ExAllocatePool3_k(uint64_t Flags, SIZE_T NumberOfBytes, ULONG Tag, PVOID Params) {
    (void)Params;
    return ExAllocatePool2_k(Flags, NumberOfBytes, Tag);
}
PVOID NTAPI ExAllocatePoolUninitialized_k(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag) {
    return ExAllocatePoolWithTag_k(PoolType, NumberOfBytes, Tag);
}
void NTAPI ExFreePool_k(PVOID P) { ExFreePoolWithTag_k(P, 0); }
void NTAPI ExFreePoolWithTag_k(PVOID P, ULONG Tag) {
    if (g_pintok_trace) {
        kputs("  [trace] ExFreePoolWithTag ptr="); kput_hex((uint64_t)(uintptr_t)P);
        kputs(" tag=0x"); kput_hex(Tag); kputs("\n");
    }
    (void)Tag;
    if (!P) return;
    POOL_HDR* h = (POOL_HDR*)((uint8_t*)P - sizeof(POOL_HDR));
    if (h->Magic != POOL_MAGIC) {
        kputs("[pool] ExFreePool magic invalido @"); kput_hex((uint64_t)(uintptr_t)P); kputs(" (ignorado)\n");
        return;   // nao foi alocado por nos; nao corrompe o heap
    }
    s_frees++;
    if (s_bytes >= h->Size) s_bytes -= h->Size; else s_bytes = 0;
    // FASE 5: subtrai do contador correto (Paged/NonPaged) pelo PoolType original.
    if (h->PoolType == 1) {
        if (s_bytes_paged >= h->Size) s_bytes_paged -= h->Size; else s_bytes_paged = 0;
    } else {
        if (s_bytes_nonpag >= h->Size) s_bytes_nonpag -= h->Size; else s_bytes_nonpag = 0;
    }
    h->Magic = 0;
    kfree(h);
}

uint64_t pool_total_allocs(void)     { return s_allocs; }
uint64_t pool_total_frees(void)      { return s_frees; }
uint64_t pool_bytes_outstanding(void){ return s_bytes; }
// FASE 5: contadores separados Paged/NonPaged (somam s_bytes_outstanding).
uint64_t pool_bytes_paged(void)      { return s_bytes_paged; }
uint64_t pool_bytes_nonpaged(void)   { return s_bytes_nonpag; }
