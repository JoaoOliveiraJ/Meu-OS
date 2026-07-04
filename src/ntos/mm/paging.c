#include "mm/paging.h"
#include "mm/pmm.h"
#include <stdint.h>

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);
extern volatile uint64_t g_ticks;

#define PT_MASK    0x000FFFFFFFFFF000ULL       // bits de endereco fisico (12..51)
#define PG_PRESENT 0x1ULL
#define PG_RW      0x2ULL
#define PG_USER    0x4ULL                      // bit U/S

static uint64_t* pt_addr(uint64_t entry) { return (uint64_t*)(entry & PT_MASK); }

uint64_t mm_get_cr3(void) {
    uint64_t cr3; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void mm_switch_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void mm_map_user(uint64_t addr, uint64_t size) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint64_t* pml4 = (uint64_t*)(cr3 & PT_MASK);
    pml4[0] |= PG_USER;                         // abre o nivel PML4
    uint64_t* pdpt = pt_addr(pml4[0]);
    pdpt[0] |= PG_USER;                         // abre o nivel PDPT
    uint64_t* pd = pt_addr(pdpt[0]);            // diretorio de paginas de 2 MiB

    uint64_t start = addr & ~0x1FFFFFULL;       // alinha em 2 MiB
    uint64_t end   = addr + size;
    for (uint64_t a = start; a < end; a += 0x200000ULL) {
        uint64_t i = a >> 21;                    // indice no PD
        if (i < 512) pd[i] |= PG_USER;           // libera a pagina de 2 MiB ao usuario
    }

    __asm__ volatile ("mov %%cr3, %%rax\n mov %%rax, %%cr3" ::: "rax", "memory"); // flush TLB
}

// Copia uma tabela de paginas (512 entradas) para um frame novo do PMM.
// Como a RAM e identidade-mapeada (fisico == virtual no 1o GiB), escrevemos
// direto no endereco do frame. Retorna o frame fisico (0 = sem RAM).
static uint64_t clone_table(const uint64_t* src) {
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return 0;
    uint64_t* dst = (uint64_t*)frame;
    for (int i = 0; i < 512; i++) dst[i] = src[i];
    return frame;
}

uint64_t mm_create_address_space(void) {
    uint64_t cr3 = mm_get_cr3();
    uint64_t* kpml4 = (uint64_t*)(cr3 & PT_MASK);

    // O kernel so mapeia o 1o GiB: PML4[0] -> PDPT, PDPT[0] -> PD (2 MiB pages).
    // Clonamos PD, depois PDPT (apontando para o novo PD) e PML4 (para o PDPT).
    if (!(kpml4[0] & PG_PRESENT)) return 0;
    uint64_t* kpdpt = pt_addr(kpml4[0]);
    if (!(kpdpt[0] & PG_PRESENT)) return 0;
    uint64_t* kpd = pt_addr(kpdpt[0]);

    uint64_t new_pd = clone_table(kpd);
    if (!new_pd) return 0;

    uint64_t new_pdpt_f = pmm_alloc_frame();
    if (!new_pdpt_f) { pmm_free_frame(new_pd); return 0; }
    uint64_t* new_pdpt = (uint64_t*)new_pdpt_f;
    for (int i = 0; i < 512; i++) new_pdpt[i] = kpdpt[i];
    // entrada 0 aponta para o nosso PD, preservando flags (P/RW/US) do original.
    new_pdpt[0] = (kpdpt[0] & ~PT_MASK) | (new_pd & PT_MASK);

    uint64_t new_pml4_f = pmm_alloc_frame();
    if (!new_pml4_f) { pmm_free_frame(new_pd); pmm_free_frame(new_pdpt_f); return 0; }
    uint64_t* new_pml4 = (uint64_t*)new_pml4_f;
    for (int i = 0; i < 512; i++) new_pml4[i] = kpml4[i];
    new_pml4[0] = (kpml4[0] & ~PT_MASK) | (new_pdpt_f & PT_MASK);

    return new_pml4_f;   // endereco fisico da nova PML4 (para CR3)
}

// ============================================================================
//  FASE 7.1 — KUSER_SHARED_DATA (0xFFFFF78000000000).
//
//  Drivers Windows leem campos dessa pagina (TickCount, NtMajorVersion, etc.)
//  diretamente, sem chamar API. Sem ela mapeada, qualquer driver real cai em
//  Page Fault na primeira acesso (foi exatamente o que aconteceu com pintok.sys
//  em cr2=0xFFFFF78000000014 = offset 0x14 = TickCount.LowPart).
//
//  Aqui mapeamos 1 pagina (4 KiB) em 0xFFFFF78000000000 com os campos minimos
//  que drivers reais consultam — TickCount(0x14), SystemTime(0x20),
//  NtMajor/MinorVersion(0x26C/0x270), NtBuildNumber(0x260), etc.
//
//  Atualizacao do TickCount fica para uma rodada futura (TODO: atualizar do
//  handler do PIT). Por ora e estatica — drivers veem ticks fixos.
// ============================================================================

static uint64_t* g_kuser_page = 0;   // ponteiro p/ atualizar campos depois

static uint64_t* ensure_table(uint64_t* parent, uint64_t idx, uint64_t flags) {
    if (!(parent[idx] & PG_PRESENT)) {
        uint64_t f = pmm_alloc_frame();
        if (!f) return 0;
        uint64_t* p = (uint64_t*)f;
        for (int i = 0; i < 512; i++) p[i] = 0;
        parent[idx] = f | flags;
    }
    return pt_addr(parent[idx]);
}

void mm_map_kuser_shared_data(void) {
    if (g_kuser_page) return;   // ja mapeado

    uint64_t addr = 0xFFFFF78000000000ULL;
    uint64_t* pml4 = (uint64_t*)(mm_get_cr3() & PT_MASK);

    uint64_t pml4_idx = (addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (addr >> 12) & 0x1FF;

    uint64_t* pdpt = ensure_table(pml4, pml4_idx, PG_PRESENT | PG_RW);
    if (!pdpt) { kputs("[mm] KUSER_SHARED_DATA: PDPT falhou\n"); return; }
    uint64_t* pd   = ensure_table(pdpt, pdpt_idx, PG_PRESENT | PG_RW);
    if (!pd)   { kputs("[mm] KUSER_SHARED_DATA: PD falhou\n"); return; }
    uint64_t* pt   = ensure_table(pd,   pd_idx,   PG_PRESENT | PG_RW);
    if (!pt)   { kputs("[mm] KUSER_SHARED_DATA: PT falhou\n"); return; }

    uint64_t data_phys = pmm_alloc_frame();
    if (!data_phys) { kputs("[mm] KUSER_SHARED_DATA: frame data falhou\n"); return; }

    uint8_t* data = (uint8_t*)data_phys;
    for (int i = 0; i < 4096; i++) data[i] = 0;

    // Campos importantes do KUSER_SHARED_DATA (offsets do NT 10):
    // 0x000 TickCountLowDeprecated (ULONG, legacy)
    // 0x004 TickCountMultiplier
    // 0x014 TickCount: { LowPart, High1Time, High2Time } -> 12 bytes
    // 0x020 SystemTime: KSYSTEM_TIME (12 bytes)
    // 0x02C TimeZoneBias
    // 0x100 InterruptTime (KSYSTEM_TIME)
    // 0x260 NtBuildNumber (ULONG)
    // 0x264 NtProductType (NT_PRODUCT_TYPE)
    // 0x268 ProductTypeIsValid (BOOLEAN)
    // 0x26C NtMajorVersion (ULONG)
    // 0x270 NtMinorVersion (ULONG)
    // 0x2D4 NumberOfPhysicalPages
    *(uint32_t*)(data + 0x004) = 0x0FA00000;   // TickCountMultiplier ~ 100ns/tick * 100Hz
    *(uint32_t*)(data + 0x014) = (uint32_t)(g_ticks * 10);  // TickCount.LowPart (em ms)
    *(uint32_t*)(data + 0x018) = 0;
    *(uint32_t*)(data + 0x01C) = 0;
    *(uint32_t*)(data + 0x260) = 26100;        // BuildNumber
    *(uint32_t*)(data + 0x264) = 1;            // ProductType = NtProductWinNt
    *(uint8_t* )(data + 0x268) = 1;            // ProductTypeIsValid
    *(uint32_t*)(data + 0x26C) = 10;           // NtMajorVersion
    *(uint32_t*)(data + 0x270) = 0;            // NtMinorVersion
    *(uint32_t*)(data + 0x2D4) = 65536;        // NumberOfPhysicalPages (256 MiB)
    // 0x030 NtSystemRoot (WCHAR[260]) = L"C:\Windows". pintok.sys faz wcslen() disso e monta
    // um path com ele; campo vazio (zeros) quebrava o path -> bail. [GATE 5-prereq do pintok.sys]
    { static const uint16_t kSysRoot[] = { 'C',':','\\','W','i','n','d','o','w','s',0 };
      for (int i = 0; i <= 10; i++) *(uint16_t*)(data + 0x30 + i*2) = kSysRoot[i]; }

    pt[pt_idx] = data_phys | PG_PRESENT | PG_RW;
    g_kuser_page = (uint64_t*)data_phys;

    // Flush TLB.
    __asm__ volatile ("mov %%cr3, %%rax\n mov %%rax, %%cr3" ::: "rax", "memory");

    kputs("[mm] KUSER_SHARED_DATA mapeado em 0xFFFFF78000000000 -> phys=");
    kput_hex(data_phys); kputs("\n");
}

// ============================================================================
//  FASE GPU — mm_map_phys_range: mapeia uma faixa fisica arbitraria.
//
//  Caminha PML4 -> PDPT -> PD -> PT (usando ensure_table para criar tabelas
//  sob demanda) e instala um PTE por pagina de 4 KiB, apontando para
//  phys + (offset desde virt). Util para mapear LFB de GPUs (BAR alta como
//  0xFD000000 do Bochs VBE) que ficam FORA da identidade de 1 GiB.
//
//  Importante: se a faixa cair sobre uma entrada de PD que ja e huge page
//  (PS=1, como o PD[0] da identidade do boot), recusamos porque nao
//  podemos sub-tabular sem reformatar o range; devolvemos falha. Em
//  enderecos virtuais altos (> 1 GiB) isso nunca ocorre.
//
//  Por padrao seto PG_USER tambem (consistente com mm_map_zero_page e o
//  resto do kernel, que e single-ring). 'flags' adicionais (PCD/PWT/NX)
//  vem do chamador.
//
//  Retorna 1 sucesso, 0 falha. Faz flush global do TLB no fim.
// ============================================================================
int mm_map_phys_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    if (size == 0) return 0;

    // Alinha por baixo virt/phys em 4 KiB; arredonda 'size' para cobrir o fim.
    uint64_t v0 = virt & ~0xFFFULL;
    uint64_t p0 = phys & ~0xFFFULL;
    uint64_t end = (virt + size + 0xFFFULL) & ~0xFFFULL;

    // Garante bits minimos. PG_USER por consistencia com o kernel single-ring.
    uint64_t pte_flags  = flags | PG_PRESENT | PG_RW | PG_USER;
    uint64_t tbl_flags  = PG_PRESENT | PG_RW | PG_USER;   // tabelas intermediarias

    uint64_t* pml4 = (uint64_t*)(mm_get_cr3() & PT_MASK);

    for (uint64_t v = v0, p = p0; v < end; v += 0x1000ULL, p += 0x1000ULL) {
        uint64_t pml4_idx = (v >> 39) & 0x1FF;
        uint64_t pdpt_idx = (v >> 30) & 0x1FF;
        uint64_t pd_idx   = (v >> 21) & 0x1FF;
        uint64_t pt_idx   = (v >> 12) & 0x1FF;

        uint64_t* pdpt = ensure_table(pml4, pml4_idx, tbl_flags);
        if (!pdpt) return 0;
        uint64_t* pd   = ensure_table(pdpt, pdpt_idx, tbl_flags);
        if (!pd)   return 0;

        // Se PD ja aponta para huge page (PS=1), nao da pra sub-tabular sem
        // reformatar — recusa (range invalido para este caminho).
        if (pd[pd_idx] & 0x80ULL) return 0;

        uint64_t* pt = ensure_table(pd, pd_idx, tbl_flags);
        if (!pt) return 0;

        // Sobrescreve direto: se ja havia mapeamento, e re-mapeamento explicito.
        pt[pt_idx] = (p & PT_MASK) | pte_flags;
    }

    // Flush TLB local (reload CR3). Single-CPU: basta isso.
    __asm__ volatile ("mov %%cr3, %%rax\n mov %%rax, %%cr3" ::: "rax", "memory");
    return 1;
}

// ============================================================================
//  FASE 7.9 — mm_map_zero_page: caminho de recuperacao de page-fault.
//
//  Aloca um frame, zera, e mapeia em 'va' (alinhado por baixo em 4 KiB) via
//  mm_map_phys_range — reusa toda a logica de walk/ensure_table. Marca
//  PG_USER por consistencia com o resto do kernel (single-ring).
//
//  Retorno: 1 = pagina presente apos a chamada, 0 = falha (sem RAM, etc.).
// ============================================================================
int mm_map_zero_page(uint64_t va) {
    if (!va) return 0;
    uint64_t addr = va & ~0xFFFULL;             // alinha por baixo em 4 KiB

    // Caso especial: o caminho de recovery e chamado MUITAS vezes em sequencia
    // pelo mesmo cr2; se a pagina ja esta presente, simplesmente re-zera o
    // frame existente em vez de alocar um novo (preserva contabilidade do PMM).
    uint64_t* pml4 = (uint64_t*)(mm_get_cr3() & PT_MASK);
    uint64_t pml4_idx = (addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (addr >> 12) & 0x1FF;

    if (pml4[pml4_idx] & PG_PRESENT) {
        uint64_t* pdpt = pt_addr(pml4[pml4_idx]);
        if (pdpt[pdpt_idx] & PG_PRESENT) {
            uint64_t* pd = pt_addr(pdpt[pdpt_idx]);
            // Huge page (PS=1) ja cobre o endereco — apenas devolve sucesso.
            if (pd[pd_idx] & 0x80ULL) return 1;
            if (pd[pd_idx] & PG_PRESENT) {
                uint64_t* pt = pt_addr(pd[pd_idx]);
                if (pt[pt_idx] & PG_PRESENT) {
                    // Reaproveita: zera o frame ja mapeado (sem alocar novo).
                    uint8_t* p = (uint8_t*)(pt[pt_idx] & PT_MASK);
                    for (int i = 0; i < 4096; i++) p[i] = 0;
                    return 1;
                }
            }
        }
    }

    // Caminho comum: aloca frame, zera, mapeia via mm_map_phys_range.
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return 0;
    uint8_t* p = (uint8_t*)frame;
    for (int i = 0; i < 4096; i++) p[i] = 0;
    return mm_map_phys_range(addr, frame, 0x1000ULL, PG_PRESENT | PG_RW | PG_USER);
}

// ============================================================================
//  Pilar 1 (NT foundation) — paginacao dinamica.
//
//  mm_unmap_range: tira [virt, virt+size) do mapa de paginas. Caminha PML4 ->
//  PDPT -> PD -> PT, zera os PTEs cobertos e roda INVLPG em cada VA para tirar
//  do TLB (single-CPU; em SMP precisa de TLB shootdown via IPI, fase futura).
//  Recusa silenciosamente entradas ja nao-presentes (NOP). Recusa hard (return
//  0) se cair sobre huge page de 2 MiB ja mapeada — caminho NT e' o mesmo: nao
//  fragmenta huge page implicitamente. Tabelas intermediarias NAO sao
//  liberadas (algumas outras paginas no mesmo PT podem continuar mapeadas).
// ============================================================================
int mm_unmap_range(uint64_t virt, uint64_t size) {
    if (size == 0) return 0;
    uint64_t v0  = virt & ~0xFFFULL;
    uint64_t end = (virt + size + 0xFFFULL) & ~0xFFFULL;

    uint64_t* pml4 = (uint64_t*)(mm_get_cr3() & PT_MASK);

    for (uint64_t v = v0; v < end; v += 0x1000ULL) {
        uint64_t pml4_idx = (v >> 39) & 0x1FF;
        uint64_t pdpt_idx = (v >> 30) & 0x1FF;
        uint64_t pd_idx   = (v >> 21) & 0x1FF;
        uint64_t pt_idx   = (v >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PG_PRESENT)) continue;
        uint64_t* pdpt = pt_addr(pml4[pml4_idx]);
        if (!(pdpt[pdpt_idx] & PG_PRESENT)) continue;
        uint64_t* pd = pt_addr(pdpt[pdpt_idx]);
        if (pd[pd_idx] & 0x80ULL) return 0;   // huge page: recusa
        if (!(pd[pd_idx] & PG_PRESENT)) continue;
        uint64_t* pt = pt_addr(pd[pd_idx]);

        pt[pt_idx] = 0;
        __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
    }
    return 1;
}

// ============================================================================
//  MMIO arena: faixa virtual reservada no half KERNEL para mapeamentos de
//  dispositivos cuja BAR esta acima da identidade de 1 GiB (Local APIC em
//  0xFEE00000, IO-APIC em 0xFEC00000, BARs de PCI Express em > 4 GiB, etc.).
//
//  Range escolhido: [0xFFFFE10000000000, 0xFFFFE20000000000) — 1 TiB.
//   - bit 47 = 1 (kernel half canonico)
//   - PML4 idx = 450 (livre: nada do kernel ate aqui usa essa entrada)
//   - NAO colide com KUSER_SHARED_DATA (PML4 482 @ 0xFFFFF78000000000)
//
//  Por que pre-alocar o PDPT no boot? mm_create_address_space copia o PML4
//  POR VALOR. Se a primeira chamada a hal_map_mmio acontecer DEPOIS de um
//  processo criar sua PML4 propria, a entrada PML4[450] que criamos no kernel
//  CR3 nao aparecera na PML4 do processo — drivers cairam em #PF. Solucao
//  canonical do NT (PsInitSystemPhase) e a mesma: as entradas kernel-half do
//  PML4 sao alocadas EARLY e ficam fixas para o resto do boot — assim toda
//  PML4 nova herda ponteiros para os mesmos PDPTs.
// ============================================================================
#define MMIO_ARENA_BASE  0xFFFFE10000000000ULL
#define MMIO_ARENA_END   0xFFFFE20000000000ULL
#define MMIO_ARENA_PML4  450u                  // (MMIO_ARENA_BASE >> 39) & 0x1FF

static uint64_t s_mmio_next     = MMIO_ARENA_BASE;
static int      s_mmio_inited   = 0;

void mm_mmio_init(void) {
    if (s_mmio_inited) return;
    uint64_t* pml4 = (uint64_t*)(mm_get_cr3() & PT_MASK);

    // Aloca PDPT do arena se ainda nao existir e fixa em PML4[450].
    if (!(pml4[MMIO_ARENA_PML4] & PG_PRESENT)) {
        uint64_t f = pmm_alloc_frame();
        if (!f) { kputs("[mm] mmio_init: PMM sem frame p/ PDPT do arena\n"); return; }
        uint64_t* pdpt = (uint64_t*)f;
        for (int i = 0; i < 512; i++) pdpt[i] = 0;
        // Bits do kernel: P + RW (sem U — somente ring 0 acessa MMIO).
        pml4[MMIO_ARENA_PML4] = f | PG_PRESENT | PG_RW;
        kputs("[mm] MMIO arena PDPT em PML4["); kput_hex(MMIO_ARENA_PML4);
        kputs("] phys="); kput_hex(f); kputs(" (kernel-only)\n");
    }
    s_mmio_inited = 1;
    kputs("[mm] MMIO arena pronto: ["); kput_hex(MMIO_ARENA_BASE);
    kputs(", "); kput_hex(MMIO_ARENA_END); kputs(")\n");
}

uint64_t mm_mmio_reserve(uint64_t size) {
    if (!s_mmio_inited) mm_mmio_init();
    if (size == 0) return 0;
    uint64_t bytes = (size + 0xFFFULL) & ~0xFFFULL;
    if (s_mmio_next + bytes > MMIO_ARENA_END) {
        kputs("[mm] MMIO arena cheio\n");
        return 0;
    }
    uint64_t va = s_mmio_next;
    s_mmio_next += bytes;
    return va;
}

// ============================================================================
//  PF probe (prova): captura #PF esperado no handler em isr.c.
//
//  Antes do load: setamos g_mm_pf_resume_rip pro label apos a instrucao, depois
//  ligamos g_mm_pf_expect=1, depois executamos a instrucao alvo. Se a CPU
//  faltar #PF, isr_handler le g_mm_pf_expect, marca g_mm_pf_caught=1, sobrescreve
//  r->rip com g_mm_pf_resume_rip e retorna. IRETQ leva a CPU para o label.
//  Se nao houver fault, caimos no proprio label, escrevemos g_mm_pf_expect=0
//  e seguimos. Idiomatico ao __ex_table do Linux / fixup-em-load do BSD.
// ============================================================================
volatile int      g_mm_pf_expect    = 0;
volatile int      g_mm_pf_caught    = 0;
volatile uint64_t g_mm_pf_resume_rip = 0;

int mm_probe_read_u32(volatile uint32_t* va, uint32_t* out_value) {
    g_mm_pf_caught = 0;
    uint32_t v = 0;
    // Sequencia em asm: instala resume rip, liga expect, executa load, label.
    // "=&r"(v) com early-clobber para nao colidir com o ponteiro de entrada.
    __asm__ volatile (
        "leaq 1f(%%rip), %%rax\n"
        "movq %%rax, %1\n"
        "movl $1, %2\n"
        "movl (%3), %0\n"
        "1:\n"
        "movl $0, %2\n"
        : "=&r"(v), "+m"(g_mm_pf_resume_rip), "+m"(g_mm_pf_expect)
        : "r"(va)
        : "rax", "memory");
    if (out_value) *out_value = v;
    return !g_mm_pf_caught;
}

// Atualiza campos voláteis (TickCount, SystemTime, InterruptTime). Chamado
// periodicamente — pode ser do handler do PIT (IRQ0). Sem isso o tick fica fixo.
void mm_kuser_tick(void) {
    if (!g_kuser_page) return;
    // Durante o single-step do DriverEntry (g_intercept_cpuid=1) o TSC fake esta CONGELADO.
    // Se o tempo do KUSD (0x014/0x100) continuar avancando a cada IRQ, o pintok.sys ve relogio
    // de parede andando contra TSC parado = inconsistencia tipica de VM. Congela junto. [anti-VM]
    extern volatile int g_intercept_cpuid;
    if (g_intercept_cpuid) return;
    uint8_t* data = (uint8_t*)g_kuser_page;
    uint64_t ms = g_ticks * 10;
    *(uint32_t*)(data + 0x014) = (uint32_t)ms;       // TickCount.LowPart
    *(uint32_t*)(data + 0x100) = (uint32_t)(ms * 10000);  // InterruptTime (100-ns units)
    *(uint32_t*)(data + 0x104) = 0;
}
