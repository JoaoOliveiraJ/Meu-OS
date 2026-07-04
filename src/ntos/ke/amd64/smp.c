// ============================================================================
//  smp.c  —  Symmetric Multi-Processing (Pilar 3 da rodada NT foundation).
//
//  Referencias:
//   - Intel SDM Vol 3 Cap 8.4 (Multiple-Processor Initialization).
//   - ACPI Specification 6.5 Sec 5.2.12 (MADT layout).
//   - Windows Internals 7th ed. cap 2 ("Kernel"), HalpStartProcessor flow.
//   - ReactOS hal/halx86/smp/ipi.c (reimplementacao limpa do mesmo fluxo).
//
//  Fluxo:
//   1) Le RSDT phys de acpi_rsdt_phys() (caminha BIOS area, ja existe).
//   2) Caminha entradas RSDT, acha tabela com signature "APIC" (MADT).
//   3) Percorre MADT entries:
//        - Type 0 (Processor Local APIC) com flags bit 0 (Enabled): registra
//          APIC ID e' pode subir.
//        - Type 1 (IO APIC): so loga (ja temos o default 0xFEC00000 funcional).
//   4) Para cada APIC ID != BSP: aloca KPCR + stack, patcheia slots do
//      trampoline em phys 0x8000, manda INIT-SIPI-SIPI.
//   5) AP entra na trampoline (real -> protected -> long mode), pula em
//      ap_entry C abaixo, incrementa s_ap_alive_count atomicamente.
//   6) BSP polls com timeout e loga "AP alive".
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "ke/amd64/smp.h"
#include "ke/amd64/apic.h"
#include "ke/amd64/idt.h"
#include "ke/sched.h"
#include "mm/heap.h"
#include "acpi/acpi.h"

extern void  kputs(const char* s);
extern void  kputc(char c);
extern void  kput_hex(uint64_t v);
extern void  kput_dec(uint64_t v);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void* memset(void* dst, int v, size_t n);
extern uint64_t mm_get_cr3(void);
extern volatile uint64_t g_ticks;

// Trampoline simbolos (export do ap_trampoline.asm).
extern uint8_t  ap_trampoline_blob[];
extern uint8_t  ap_trampoline_blob_end[];
extern uint64_t ap_trampoline_size;
extern uint64_t ap_trampoline_stack_off;
extern uint64_t ap_trampoline_kpcr_off;
extern uint64_t ap_trampoline_entry_off;
extern uint64_t ap_trampoline_pml4_off;

#define MAX_CPUS      8
#define AP_BOOT_PHYS  0x8000ULL
#define AP_STACK_SIZE 16384

// --- ACPI structures (subset) -------------------------------------------
struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt_header {
    struct sdt_header sdt;
    uint32_t local_apic_addr;
    uint32_t flags;
} __attribute__((packed));

struct madt_entry_local_apic {
    uint8_t  type;       // 0
    uint8_t  length;     // 8
    uint8_t  acpi_pid;
    uint8_t  apic_id;
    uint32_t flags;      // bit 0 = enabled
} __attribute__((packed));

struct madt_entry_io_apic {
    uint8_t  type;       // 1
    uint8_t  length;     // 12
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed));

// --- estado --------------------------------------------------------------
static int      s_cpu_count = 0;
static uint8_t  s_apic_id_table[MAX_CPUS];
static volatile uint32_t s_ap_alive_count = 0;
static volatile uint8_t  s_ap_seen_id[MAX_CPUS] = {0};
static uint64_t s_ioapic_phys_from_madt = 0;
static volatile int      s_ap_timer_online = 0;   // >=1 AP com LVT timer ligado

int smp_ap_timer_online(void) {
    return __atomic_load_n(&s_ap_timer_online, __ATOMIC_ACQUIRE);
}

// --- AP worker core: heartbeat + trabalho real em paralelo com o BSP ---------
static volatile uint64_t s_ap_heartbeat = 0;   // avanca so se o 2o core executa
static volatile uint64_t s_ap_compute   = 0;   // resultado de computacao (ALU)
static volatile int      s_ap_working    = 0;   // 1 = AP entrou no worker loop

uint64_t smp_ap_heartbeat(void) { return __atomic_load_n(&s_ap_heartbeat, __ATOMIC_RELAXED); }
uint64_t smp_ap_compute(void)   { return __atomic_load_n(&s_ap_compute,   __ATOMIC_RELAXED); }
int      smp_ap_working(void)   { return __atomic_load_n(&s_ap_working,    __ATOMIC_ACQUIRE); }

// Loop dedicado do AP (nao retorna). Roda SIMULTANEO ao BSP: mistura um hash LCG
// (trabalho de ALU real) e publica heartbeat/resultado em memoria compartilhada.
// O BSP le esses valores e os mostra na taskbar — o numero so cresce se o 2o core
// esta de fato executando instrucoes ao mesmo tempo. SMP genuino.
static void ap_worker_loop(uint32_t cpu) {
    (void)cpu;
    __atomic_store_n(&s_ap_working, 1, __ATOMIC_RELEASE);
    uint64_t acc = 0x9E3779B97F4A7C15ULL, n = 0;
    for (;;) {
        n++;
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL + n;  // LCG
        __atomic_store_n(&s_ap_compute,   acc, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_ap_heartbeat, 1, __ATOMIC_RELAXED);
        __asm__ volatile ("pause");
    }
}

int      smp_cpu_count(void) { return s_cpu_count; }
uint8_t  smp_apic_id_of(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= s_cpu_count) return 0xFF;
    return s_apic_id_table[cpu_index];
}
uint32_t smp_ap_alive_count(void) { return s_ap_alive_count; }
int      smp_ap_seen(uint8_t apic_id) {
    if (apic_id >= MAX_CPUS) return 0;
    return s_ap_seen_id[apic_id];
}

// --- AP entry (chamada pelo ap_trampoline.asm em long mode) -------------
//
// Pilar 4: AP setup completo:
//   1) Le APIC ID (sinaliza alive — caminho Pilar 3 preservado para a prova).
//   2) LIDT na IDT compartilhada do kernel.
//   3) Habilita SVR.Software_Enable + programa LAPIC timer local.
//   4) Registra-se no scheduler (ki_init_processor).
//   5) Cai em ki_idle_loop com IF=1; a partir daqui o APIC timer do AP comeca
//      a disparar (vetor 0xD1) e o ki_quantum_end pode pegar threads ready
//      para esta CPU.
void ap_entry(void) {
    uint32_t my_id = apic_local_id();

    __atomic_add_fetch(&s_ap_alive_count, 1, __ATOMIC_SEQ_CST);
    if (my_id < MAX_CPUS) {
        __atomic_store_n(&s_ap_seen_id[my_id], 1, __ATOMIC_SEQ_CST);
    }

    // Pilar 4 — SETUP DO AP em ordem segura:
    //   (i)   idt_load() — IDT compartilhada. Sem efeito ate ter exceptions
    //         ou interrupcoes (IF=0 ainda).
    //   (ii)  apic_enable_local() — SVR ON + LVT entries mascaradas.
    //         CRITICO: LVT Timer fica MASCARADO (bit 16=1) e TMR_INIT NAO
    //         e' escrito — countdown NAO comeca. Diagnostico anterior: ligar
    //         o timer aqui correlaciona com hang BSP via TCG.
    //   (iii) ki_init_processor() — monta KPRCB do AP. Memoria compartilhada
    //         + atomic_add no g_ki_cpu_count.
    //   (iv)  apic_unmask_timer_local() — desmascara LVT_TMR + escreve
    //         TMR_INIT. SO AGORA o LAPIC do AP comeca a contar p/ 0xD1.
    //         PRCB ja esta pronto, scheduler pode entregar threads.
    //   (v)   sti — IF=1. O primeiro tick pendente cai aqui dentro do dispatcher.
    //   (vi)  ki_idle_loop — sti; hlt; preemptado quando houver ready.
    // Pilar 4 — SETUP DO AP em ordem segura ja com bug do SIMD store
    // contornado em ki_init_processor (atomic_store explicitos):
    //   (i)   idt_load            — IDT compartilhada
    //   (ii)  apic_enable_local   — SVR ON, LVT_TMR MASCARADO (mask=1)
    //   (iii) ki_init_processor   — KPRCB do AP (sem SIMD zero-stores)
    //   (iv)  apic_unmask_timer_local — desmascara + escreve TMR_INIT
    //   (v)   sti                 — IF=1, primeiro tick cai no dispatcher
    //   (vi)  ki_idle_loop        — sti; hlt; preemptado quando houver ready
    // Pilar 4 — SETUP DO AP em ordem segura:
    //   (i)   idt_load            — IDT compartilhada
    //   (ii)  apic_enable_local   — SVR ON; LVT_TMR programado MASCARADO
    //                               (bit 16=1), TMR_INIT NAO escrito ainda
    //   (iii) ki_init_processor   — KPRCB do AP (com workaround SIMD-store)
    //   (iv)  apic_unmask_timer_local — desmascara + escreve TMR_INIT
    //                                  (AP a 99 Hz, BSP a 100 Hz — workaround
    //                                  TCG phase-lock; ver apic.c)
    //   (v)   sti                 — IF=1, primeiro tick cai no dispatcher
    //   (vi)  ki_idle_loop        — sti; hlt; preemptado quando houver ready
    idt_load();
    apic_enable_local();
    ki_init_processor(my_id, my_id);

    // -----------------------------------------------------------------------
    //  SMP sob QEMU TCG (sem WHPX/KVM neste host): NAO desmascaramos o LVT timer
    //  LOCAL do AP. Testado: um SEGUNDO LAPIC timer periodico (mesmo em frequencia
    //  distinta) congela DETERMINISTICAMENTE o BSP no proprio boot (bug de
    //  contencao do LAPIC emulado do TCG multi-thread). Em hardware real ou sob
    //  WHPX/KVM os LAPICs sao independentes e bastaria apic_unmask_timer_local()
    //  aqui para escalonamento preemptivo POR-CORE (o codigo do scheduler ja
    //  suporta — ki_quantum_end roda em qualquer CPU).
    //
    //  Em vez disso o AP roda um WORKER LOOP dedicado: executa trabalho REAL em
    //  paralelo com o BSP e publica um heartbeat + resultado de computacao. Isto
    //  e' SMP genuino — dois cores executando SIMULTANEAMENTE — apenas sem
    //  preempcao no 2o core. O BSP mantem multithreading preemptivo completo.
    // -----------------------------------------------------------------------
    __asm__ volatile ("sti");
    ap_worker_loop(my_id);   // nao retorna
}

// --- MADT parse ----------------------------------------------------------
static int parse_madt(uint64_t rsdt_phys) {
    if (!rsdt_phys) {
        kputs("[smp] RSDT phys=0; sem MADT (rodando UP)\n");
        return 0;
    }
    if (rsdt_phys >= 0x40000000ULL) {
        // Caminho desnecessario p/ QEMU em -m 256, mas defensivo: se ACPI ficar
        // acima de 1 GiB precisariamos hal_map_mmio. Falla limpo por enquanto.
        kputs("[smp] RSDT phys "); kput_hex(rsdt_phys);
        kputs(" acima de 1 GiB; nao mapeado nesta rodada\n");
        return 0;
    }
    const struct sdt_header* rsdt = (const struct sdt_header*)(uintptr_t)rsdt_phys;
    if (rsdt->signature[0] != 'R' || rsdt->signature[1] != 'S' ||
        rsdt->signature[2] != 'D' || rsdt->signature[3] != 'T') {
        kputs("[smp] RSDT signature invalida\n");
        return 0;
    }

    uint32_t entries = (rsdt->length - sizeof(struct sdt_header)) / 4;
    const uint32_t* entry_ptrs = (const uint32_t*)((const uint8_t*)rsdt + sizeof(struct sdt_header));

    kputs("[smp] RSDT @ "); kput_hex(rsdt_phys);
    kputs(" length="); kput_dec(rsdt->length);
    kputs(" entries="); kput_dec(entries); kputc('\n');

    const struct madt_header* madt = 0;
    for (uint32_t i = 0; i < entries; i++) {
        const struct sdt_header* tbl = (const struct sdt_header*)(uintptr_t)entry_ptrs[i];
        char sig[5] = {tbl->signature[0], tbl->signature[1], tbl->signature[2], tbl->signature[3], 0};
        kputs("[smp]  RSDT["); kput_dec(i); kputs("] sig='");
        kputs(sig); kputs("' @ "); kput_hex((uint64_t)(uintptr_t)tbl);
        kputs(" length="); kput_dec(tbl->length); kputc('\n');
        if (tbl->signature[0] == 'A' && tbl->signature[1] == 'P' &&
            tbl->signature[2] == 'I' && tbl->signature[3] == 'C') {
            madt = (const struct madt_header*)tbl;
        }
    }
    if (!madt) { kputs("[smp] MADT (sig 'APIC') nao encontrado no RSDT\n"); return 0; }

    kputs("[smp] MADT @ "); kput_hex((uint64_t)(uintptr_t)madt);
    kputs(" length="); kput_dec(madt->sdt.length);
    kputs(" LAPIC_default=0x"); kput_hex(madt->local_apic_addr);
    kputs(" flags=0x"); kput_hex(madt->flags); kputc('\n');

    const uint8_t* p   = (const uint8_t*)madt + sizeof(struct madt_header);
    const uint8_t* end = (const uint8_t*)madt + madt->sdt.length;

    int io_apics = 0;
    while (p < end) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len == 0) break;
        switch (type) {
            case 0: {
                const struct madt_entry_local_apic* la = (const struct madt_entry_local_apic*)p;
                if ((la->flags & 1u) && s_cpu_count < MAX_CPUS) {
                    s_apic_id_table[s_cpu_count] = la->apic_id;
                    kputs("[smp]  CPU["); kput_dec(s_cpu_count);
                    kputs("]: ACPI_PID="); kput_dec(la->acpi_pid);
                    kputs(" APIC_ID="); kput_dec(la->apic_id);
                    kputs(" enabled\n");
                    s_cpu_count++;
                }
                break;
            }
            case 1: {
                const struct madt_entry_io_apic* ia = (const struct madt_entry_io_apic*)p;
                kputs("[smp]  IO-APIC id="); kput_dec(ia->ioapic_id);
                kputs(" addr=0x"); kput_hex(ia->ioapic_addr);
                kputs(" GSI_base="); kput_dec(ia->gsi_base); kputc('\n');
                if (!s_ioapic_phys_from_madt) s_ioapic_phys_from_madt = ia->ioapic_addr;
                io_apics++;
                break;
            }
            default:
                // 2 = ISO (Interrupt Source Override), 4 = NMI Source, 5 = LAPIC
                // Address Override, 9 = LAPIC x2 etc. Ignoramos por enquanto.
                // (Confirmado nesta rodada: QEMU nao tem ISO p/ IRQ12 — GSI 12
                // direto, edge/active-high. So IRQ0->GSI2 e os PCI 5/9/10/11.)
                break;
        }
        p += len;
    }
    kputs("[smp] MADT parse: "); kput_dec(s_cpu_count);
    kputs(" CPUs habilitados, "); kput_dec(io_apics); kputs(" IO-APICs\n");
    return s_cpu_count > 0;
}

// --- helpers --------------------------------------------------------------
static void* alloc_zeroed(uint64_t bytes) {
    void* p = kmalloc(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

// Delay grosseiro independente de g_ticks.
static void crude_delay(uint64_t pauses) {
    for (volatile uint64_t i = 0; i < pauses; i++) __asm__ volatile ("pause");
}

// --- AP launch -----------------------------------------------------------
static int smp_start_aps(void) {
    if (s_cpu_count <= 1) {
        kputs("[smp] so 1 CPU detectado; nada a lancar\n");
        return 0;
    }

    // 1) Copia o trampoline para phys 0x8000 (vetor SIPI 0x08).
    uint8_t* dst = (uint8_t*)(uintptr_t)AP_BOOT_PHYS;
    memcpy(dst, ap_trampoline_blob, ap_trampoline_size);
    kputs("[smp] trampoline copiado para phys=0x");
    kput_hex(AP_BOOT_PHYS);
    kputs(" size="); kput_dec(ap_trampoline_size); kputc('\n');

    // 2) Slots constantes (entry + pml4 herdado do BSP).
    uint64_t pml4 = mm_get_cr3() & ~0xFFFULL;
    extern void ap_entry(void);
    *(uint64_t*)(dst + ap_trampoline_pml4_off)  = pml4;
    *(uint64_t*)(dst + ap_trampoline_entry_off) = (uint64_t)(uintptr_t)&ap_entry;

    // 3) Para cada APIC ID != BSP: aloca recursos por-AP, INIT-SIPI-SIPI.
    uint32_t bsp = apic_bsp_id();
    int launched = 0;
    for (int i = 0; i < s_cpu_count; i++) {
        uint8_t aid = s_apic_id_table[i];
        if (aid == (uint8_t)bsp) continue;

        void* kpcr      = alloc_zeroed(4096);
        void* stack_buf = alloc_zeroed(AP_STACK_SIZE);
        if (!kpcr || !stack_buf) {
            kputs("[smp] sem memoria p/ AP "); kput_dec(aid); kputc('\n');
            continue;
        }
        // Topo da stack alinhado a 16 (System V).
        uint64_t stack_top = (((uint64_t)(uintptr_t)stack_buf) + AP_STACK_SIZE) & ~0xFULL;

        *(uint64_t*)(dst + ap_trampoline_stack_off) = stack_top;
        *(uint64_t*)(dst + ap_trampoline_kpcr_off)  = (uint64_t)(uintptr_t)kpcr;

        kputs("[smp] lancando AP APIC_ID="); kput_dec(aid);
        kputs(" stack_top=0x"); kput_hex(stack_top);
        kputs(" kpcr=0x"); kput_hex((uint64_t)(uintptr_t)kpcr); kputc('\n');

        // INIT IPI -> delay -> SIPI x2 (SDM 8.4.4 "Universal Start-up").
        apic_send_ipi(aid, APIC_IPI_INIT, 0);
        crude_delay(1000000ULL);                    // ~10 ms em TCG
        apic_send_ipi(aid, APIC_IPI_STARTUP, (uint8_t)(AP_BOOT_PHYS >> 12));
        crude_delay(100000ULL);                     // ~1 ms
        apic_send_ipi(aid, APIC_IPI_STARTUP, (uint8_t)(AP_BOOT_PHYS >> 12));

        launched++;
    }

    // 4) Espera APs sinalizarem alive (polling sem depender de g_ticks).
    kputs("[smp] aguardando "); kput_dec((uint64_t)launched);
    kputs(" AP(s) sinalizarem alive...\n");
    for (int spin = 0; spin < 200; spin++) {          // ~10 s @ TCG
        if (s_ap_alive_count >= (uint32_t)launched) break;
        crude_delay(500000ULL);                       // ~5ms
    }
    kputs("[smp] alive_count="); kput_dec(s_ap_alive_count);
    kputs(" de "); kput_dec((uint64_t)launched); kputs(" lancados\n");

    for (uint8_t i = 0; i < MAX_CPUS; i++) {
        if (s_ap_seen_id[i]) {
            kputs("[smp]   AP APIC_ID="); kput_dec(i); kputs(" alive\n");
        }
    }
    return launched;
}

void smp_init(void) {
    kputs("\n--- SMP (MADT + INIT-SIPI-SIPI) ---\n");
    uint64_t rsdt = acpi_rsdt_phys();
    if (!parse_madt(rsdt)) {
        kputs("[smp] sem MADT — boot continua como UP\n");
        s_cpu_count = 1;
        s_apic_id_table[0] = (uint8_t)apic_bsp_id();
        return;
    }
    smp_start_aps();
}
