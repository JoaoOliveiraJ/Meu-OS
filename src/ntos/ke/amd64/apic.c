// ============================================================================
//  apic.c  —  Local APIC + IO-APIC (Pilar 2 da rodada NT foundation).
//
//  Referencia primaria: Intel SDM Vol 3 Cap 11 (Advanced Programmable Interrupt
//  Controller). Layout dos registradores e bits da SVR/LVT/ICR/IOREDTBL tira-
//  dos do SDM; convencao de vetores (CLOCK_VECTOR=0xD1, IPI=0xE1) tirada do
//  Windows Internals 7th ed. cap. 8 ("Interrupt Dispatching") e do hal.dll
//  desassemblado (NT 10 build 19041) — vetores fixos historicamente.
//
//  Onde tem palpite (calibracao do APIC timer pelo PIT 100 Hz): NT calibra
//  contra HPET ou TSC. Nao temos HPET aqui (acpi_init nao parseou tables) nem
//  TSC calibrado. PIT e' o que ha — e e' suficiente porque APIC timer rodando
//  e o que importa para a prova.
// ============================================================================
#include <stdint.h>
#include "ke/amd64/apic.h"
#include "ke/amd64/pic.h"
#include "hal/hal.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern volatile uint64_t g_ticks;     // alimentado por PIT ANTES do APIC, e
                                       // pelo APIC timer DEPOIS (mesmo simbolo).

// --- estado global -------------------------------------------------------
static volatile uint32_t* s_lapic   = 0;     // MMIO do Local APIC mapeado
static volatile uint32_t* s_ioapic  = 0;     // MMIO do IO-APIC mapeado
static int      s_apic_active       = 0;
static uint32_t s_bsp_id            = 0;
static uint32_t s_apic_freq_per_sec = 0;     // bus clock dividido pelo DCR

// --- Registradores LAPIC (offsets em bytes a partir do MMIO base) ---------
#define LAPIC_REG_ID       0x020
#define LAPIC_REG_VERSION  0x030
#define LAPIC_REG_EOI      0x0B0
#define LAPIC_REG_SVR      0x0F0
#define LAPIC_REG_ICR_LOW  0x300
#define LAPIC_REG_ICR_HIGH 0x310
#define LAPIC_REG_LVT_TMR  0x320
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_ERROR 0x370
#define LAPIC_REG_TMR_INIT 0x380
#define LAPIC_REG_TMR_CCR  0x390
#define LAPIC_REG_TMR_DCR  0x3E0

// --- IO-APIC indirect access ---------------------------------------------
#define IOAPIC_REG_IOREGSEL 0x00
#define IOAPIC_REG_IOWIN    0x10

static inline uint32_t lapic_read(uint32_t reg) {
    return s_lapic[reg / 4];
}
static inline void lapic_write(uint32_t reg, uint32_t value) {
    s_lapic[reg / 4] = value;
}
static uint32_t ioapic_read(uint8_t reg) {
    s_ioapic[IOAPIC_REG_IOREGSEL / 4] = reg;
    return s_ioapic[IOAPIC_REG_IOWIN / 4];
}
static void ioapic_write(uint8_t reg, uint32_t value) {
    s_ioapic[IOAPIC_REG_IOREGSEL / 4] = reg;
    s_ioapic[IOAPIC_REG_IOWIN / 4] = value;
}

// --- API publica ---------------------------------------------------------
void apic_eoi(void) {
    if (!s_lapic) return;
    lapic_write(LAPIC_REG_EOI, 0);
}
uint32_t apic_local_id(void) {
    if (!s_lapic) return 0;
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu;
}
int      apic_active(void) { return s_apic_active; }
uint32_t apic_bsp_id(void) { return s_bsp_id; }

// ============================================================================
//  Calibracao do APIC timer.
//
//  O APIC timer decrementa o Current Count Register a cada ciclo do bus do
//  Local APIC dividido pelo DCR. Para descobrir a frequencia em ciclos/segundo
//  rodamos com ICR=0xFFFFFFFF, esperamos ~50 ms via PIT (5 ticks @ 100 Hz),
//  lemos CCR e fazemos a conta:
//    delta_apic = 0xFFFFFFFF - CCR
//    bus_per_sec_dividido = delta_apic / 0.050
//  Depois programamos ICR = bus_per_sec_dividido / freq_desejada (100 Hz).
//
//  PIT precisa estar ATIVO durante a calibracao — quem chama apic_init deve
//  ainda nao ter rodado pic_disable.
// ============================================================================
static uint32_t apic_calibrate_against_pit(void) {
    // Divide por 1 (DCR = 0xB). Cada bus tick = 1 contagem.
    lapic_write(LAPIC_REG_TMR_DCR, 0xB);
    // Mascara o LVT timer pra ele NAO disparar interrupcao durante a calibracao.
    lapic_write(LAPIC_REG_LVT_TMR, 0x00010000u | APIC_VECTOR_TIMER);

    // Espera a virada do proximo tick do PIT (so para alinhar a janela).
    uint64_t t0 = g_ticks;
    while (g_ticks == t0) __asm__ volatile ("pause");

    // Inicia o APIC timer em free-running max.
    lapic_write(LAPIC_REG_TMR_INIT, 0xFFFFFFFFu);

    // Conta 5 PIT ticks = ~50 ms.
    const uint64_t window = 5;
    uint64_t target = g_ticks + window;
    while (g_ticks < target) __asm__ volatile ("pause");

    // Le o que sobrou. Para o APIC timer calibrando, valor decresce.
    uint32_t ccr = lapic_read(LAPIC_REG_TMR_CCR);
    uint32_t delta = 0xFFFFFFFFu - ccr;       // ticks consumidos em ~50 ms
    uint32_t bus_per_sec = (uint32_t)((uint64_t)delta * 1000ULL / 50ULL);  // *1000/50 = *20

    s_apic_freq_per_sec = bus_per_sec;
    return bus_per_sec;
}

void apic_init(void) {
    if (s_apic_active) return;

    kputs("\n--- APIC (Local APIC + IO-APIC) ---\n");

    // 1) Mapeia LAPIC (0xFEE00000). Quem nao tem Pilar 1 retorna 0 aqui.
    s_lapic = (volatile uint32_t*)hal_map_mmio(0xFEE00000ULL, 0x1000ULL);
    if (!s_lapic) { kputs("[apic] FAIL: hal_map_mmio(LAPIC) -> 0\n"); return; }

    // 2) Mapeia IO-APIC (0xFEC00000 — default do chipset; ler o real da MADT no Pilar 3).
    s_ioapic = (volatile uint32_t*)hal_map_mmio(0xFEC00000ULL, 0x1000ULL);
    if (!s_ioapic) { kputs("[apic] FAIL: hal_map_mmio(IO-APIC) -> 0\n"); return; }

    // 3) Le o APIC ID do BSP (so guardar para o Pilar 3).
    s_bsp_id = apic_local_id();
    uint32_t lapic_ver = lapic_read(LAPIC_REG_VERSION) & 0xFFu;
    kputs("[apic] LAPIC mapeado virt="); kput_hex((uint64_t)(uintptr_t)s_lapic);
    kputs(" BSP_ID="); kput_dec(s_bsp_id);
    kputs(" version="); kput_hex(lapic_ver); kputc('\n');

    uint32_t ioa_id  = (ioapic_read(0x00) >> 24) & 0xFFu;
    uint32_t ioa_ver = ioapic_read(0x01);
    uint32_t ioa_max_redir = ((ioa_ver >> 16) & 0xFFu) + 1u;
    kputs("[apic] IO-APIC mapeado virt="); kput_hex((uint64_t)(uintptr_t)s_ioapic);
    kputs(" id="); kput_dec(ioa_id);
    kputs(" max_redirs="); kput_dec(ioa_max_redir); kputc('\n');

    // 4) Habilita LAPIC: SVR bit 8 (Software Enable) + spurious vector 0xFF.
    //    NAO mascara LINT0 aqui: durante a calibracao o PIT precisa entregar
    //    IRQ0 pelo caminho 8259 -> LINT0 (modo ExtINT, default da BSP). Se
    //    masclassem LINT0 agora, IRQ0 some e o loop "espera g_ticks subir"
    //    enrosca para sempre. Mascara so DEPOIS de calibrar + ligar APIC timer.
    lapic_write(LAPIC_REG_SVR, (1u << 8) | APIC_VECTOR_SPURIOUS);

    // 5) Calibra APIC timer contra PIT (que esta ativo nesse momento).
    uint32_t freq = apic_calibrate_against_pit();
    if (freq < 1000u) {
        // Defensivo: se a calibracao falhar (PIT nao rodou, etc.), usa um chute.
        freq = 100000000u;
        kputs("[apic] WARN: calibracao deu valor irrisorio; usando default 100 MHz\n");
    }
    uint32_t target_hz = 100u;                       // 100 Hz = 10 ms por tick
    uint32_t initial_count = freq / target_hz;
    kputs("[apic] timer freq bus="); kput_dec(freq);
    kputs(" Hz; programando "); kput_dec(target_hz);
    kputs(" Hz com ICR="); kput_hex(initial_count); kputc('\n');

    // 6) Programa LVT Timer periodico no vetor 0xD1 (CLOCK_VECTOR).
    //    bit 17 (timer mode) = 01 = periodic; bit 16 (mask) = 0.
    lapic_write(LAPIC_REG_TMR_DCR, 0xB);             // divide by 1 (manter consistente)
    lapic_write(LAPIC_REG_LVT_TMR, (1u << 17) | APIC_VECTOR_TIMER);
    lapic_write(LAPIC_REG_TMR_INIT, initial_count);

    // 7) IO-APIC: redireciona IRQ1 (teclado) -> vetor 0x21, destino BSP fisico.
    ioapic_set_irq(1, APIC_VECTOR_KBD, (uint8_t)s_bsp_id);

    // 8) Agora SIM mascara LINT0/LINT1/Error: o PIT nao precisa mais entregar
    //    via LINT0 (APIC timer ja esta rodando), e o PIC esta prestes a sair.
    lapic_write(LAPIC_REG_LVT_LINT0, 0x00010000u);
    lapic_write(LAPIC_REG_LVT_LINT1, 0x00010000u);
    lapic_write(LAPIC_REG_LVT_ERROR, 0x00010000u);

    // 9) Desliga totalmente o 8259 (PIC). Agora SOMENTE APIC dispara IRQs.
    //    A IRQ0 (PIT) some — daqui pra frente g_ticks e' alimentado SOMENTE
    //    pelo APIC timer (vetor 0xD1 em isr.c).
    pic_disable();

    s_apic_active = 1;
    kputs("[apic] PIC 8259 desligado; APIC ativo (timer 0xD1, kbd via IO-APIC 0x21)\n");
}

void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t apic_id) {
    if (!s_ioapic) return;
    // IOREDTBL[gsi] -> regs 0x10+gsi*2 (low) e 0x10+gsi*2+1 (high).
    uint8_t reg_low  = (uint8_t)(0x10 + gsi * 2);
    uint8_t reg_high = (uint8_t)(reg_low + 1);

    // Low 32 bits:
    //  bits 7..0   = vector
    //  bits 10..8  = delivery mode (000 = fixed)
    //  bit 11      = destination mode (0 = physical)
    //  bit 12      = delivery status (RO)
    //  bit 13      = polarity (0 = active high)
    //  bit 14      = remote IRR (RO)
    //  bit 15      = trigger mode (0 = edge)
    //  bit 16      = mask (1 = mask). Comecamos UNMASKED.
    uint32_t low  = (uint32_t)vector;     // resto zero = fixed/phys/active-hi/edge/unmasked
    uint32_t high = (uint32_t)apic_id << 24;

    // Spec: programar HIGH antes de LOW pra evitar entrega com destino errado.
    ioapic_write(reg_high, high);
    ioapic_write(reg_low,  low);

    kputs("[apic] IO-APIC redir IRQ"); kput_dec(gsi);
    kputs(" -> vector "); kput_hex(vector);
    kputs(" dest_cpu "); kput_dec(apic_id); kputc('\n');
}

// ============================================================================
//  IPI primitives (Pilar 3).
//
//  ICR_HIGH (0x310): bits 24..31 = destination APIC ID (physical mode).
//  ICR_LOW  (0x300):
//    bits 7..0   = vector
//    bits 10..8  = delivery mode (000=Fixed, 101=INIT, 110=Start-Up)
//    bit 11      = destination mode (0=physical)
//    bit 12      = delivery status (RO; 1 enquanto pendente)
//    bit 14      = level (1 = assert; usado por SIPI/INIT level-trigger)
//    bit 15      = trigger mode (0 = edge p/ SIPI)
//    bits 19..18 = destination shorthand (00=use dest field, 01=self,
//                                          10=all, 11=all-but-self)
//  Para INIT-SIPI-SIPI: o sequencia classica do AP startup.
// ============================================================================
// Espera bit 12 (Delivery Status) do ICR_LOW limpar. Bounded para nao
// enroscar quando o TCG/hw nunca completa (caminho seguro).
void apic_wait_ipi(void) {
    if (!s_lapic) return;
    for (uint32_t i = 0; i < 100000u; i++) {
        if (!(lapic_read(LAPIC_REG_ICR_LOW) & (1u << 12))) return;
        __asm__ volatile ("pause");
    }
    // Timeout silencioso — caminho do Linux: confia que a entrega aconteceu
    // mesmo se a flag nao limpou no tempo esperado (alguns chips xAPIC tem
    // race entre write e leitura subsequente).
}

void apic_send_ipi(uint8_t dest_apic_id, apic_ipi_kind_t kind, uint8_t vector_or_page) {
    if (!s_lapic) return;
    // SO espera ANTES do write — assim a flag de delivery do PROXIMO write
    // espelha realmente este IPI. Linux/ReactOS fazem o mesmo.
    apic_wait_ipi();
    lapic_write(LAPIC_REG_ICR_HIGH, ((uint32_t)dest_apic_id) << 24);

    uint32_t cmd = (uint32_t)vector_or_page;
    cmd |= ((uint32_t)kind & 0x7u) << 8;         // delivery mode bits 10..8
    // Level: 1 (assert) so para INIT classico. STARTUP, SDM diz: tratada
    // sempre como deassert pelo hw. Fixed/NMI: level irrelevante.
    if (kind == APIC_IPI_INIT) {
        cmd |= (1u << 14);
    }
    lapic_write(LAPIC_REG_ICR_LOW, cmd);
}
