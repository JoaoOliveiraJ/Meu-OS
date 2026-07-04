// ============================================================================
//  kinterrupt.c — FASE FUNDACAO (trilha I/O, Fase 3): modelo de interrupcao.
//
//  KINTERRUPT + tabela por-vetor g_interrupt_table[256]. IoConnectInterrupt
//  registra o ISR do driver num vetor (e DESMASCARA a IO-APIC se houver GSI
//  real). O isr_handler (isr.c) chama ke_interrupt_dispatch ANTES da cadeia
//  hardcoded (teclado/mouse/timer legado) — additivo, nao os toca; tampouco o
//  bloco anti-VM (#DB). O ISR do driver roda em DIRQL com o spinlock do
//  interrupt segurado. Nenhum destes nomes esta na lista do pintok -> efeito
//  zero p/ ele.
// ============================================================================
#include <stdint.h>
#include "ntddk.h"
#include "ke/sync.h"
#include "ke/amd64/kinterrupt.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern void ioapic_set_irq_ex(uint8_t gsi, uint8_t vector, uint8_t apic_id, int level, int active_low, int masked);
extern uint32_t apic_bsp_id(void);

struct _KINTERRUPT {
    PKSERVICE_ROUTINE ServiceRoutine;
    PVOID       ServiceContext;
    PKSPIN_LOCK SpinLock;        // fornecido pelo driver, ou &ActualLock
    KSPIN_LOCK  ActualLock;      // usado se o driver nao forneceu
    ULONG       Vector;
    KIRQL       Irql;            // DIRQL do ISR
    KIRQL       SynchronizeIrql;
    int         Mode;
    int         Connected;
    uint32_t    Gsi;             // GSI real (0xFFFFFFFF = nenhum/soft)
};

static PKINTERRUPT g_interrupt_table[256];
static KINTERRUPT  s_int_pool[32];
static int         s_int_pool_n = 0;
static uint32_t    s_vec_gsi[256];      // vetor -> GSI (preenchido por HalGetInterruptVector)
static uint8_t     s_next_vec = 0x40;   // pool de vetores de sistema: 0x40..0x6F

void ke_interrupt_init(void) {
    for (int i = 0; i < 256; i++) { g_interrupt_table[i] = 0; s_vec_gsi[i] = 0xFFFFFFFFu; }
    s_int_pool_n = 0;
    s_next_vec = 0x40;
    kputs("[int] modelo de interrupcao inicializado (tabela por-vetor)\n");
}

static uint8_t alloc_vector(void) {
    if (s_next_vec > 0x6F) return 0;   // pool esgotado
    return s_next_vec++;
}

// Despacho do ISR do driver. Chamado pelo isr_handler.
int ke_interrupt_dispatch(uint64_t vector) {
    if (vector > 255) return 0;
    PKINTERRUPT ki = g_interrupt_table[vector & 0xFF];
    if (!ki || !ki->Connected || !ki->ServiceRoutine) return 0;
    // Roda em DIRQL, com o spinlock do interrupt segurado (raw, ja > DISPATCH).
    KIRQL old; KeRaiseIrql_k(ki->Irql, &old);
    KeAcquireSpinLockAtDpcLevel_k(ki->SpinLock);
    ki->ServiceRoutine(ki, ki->ServiceContext);
    KeReleaseSpinLockFromDpcLevel_k(ki->SpinLock);
    KeLowerIrql_k(old);
    return 1;
}

NTSTATUS NTAPI IoConnectInterrupt_k(PKINTERRUPT* InterruptObject, PKSERVICE_ROUTINE ServiceRoutine,
    PVOID ServiceContext, PKSPIN_LOCK SpinLock, ULONG Vector, KIRQL Irql, KIRQL SynchronizeIrql,
    KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector, KAFFINITY ProcessorEnableMask, BOOLEAN FloatingSave) {
    (void)ShareVector; (void)ProcessorEnableMask; (void)FloatingSave;
    if (!InterruptObject || Vector > 255 || s_int_pool_n >= 32) return STATUS_UNSUCCESSFUL;
    PKINTERRUPT ki = &s_int_pool[s_int_pool_n++];
    ki->ServiceRoutine  = ServiceRoutine;
    ki->ServiceContext  = ServiceContext;
    ki->ActualLock      = 0;
    ki->SpinLock        = SpinLock ? SpinLock : &ki->ActualLock;
    ki->Vector          = Vector;
    ki->Irql            = Irql ? Irql : DIRQL_DEFAULT_DEVICE;
    ki->SynchronizeIrql = SynchronizeIrql ? SynchronizeIrql : ki->Irql;
    ki->Mode            = (int)InterruptMode;
    ki->Gsi             = s_vec_gsi[Vector & 0xFF];
    ki->Connected       = 1;
    g_interrupt_table[Vector & 0xFF] = ki;
    // Se ha GSI real, DESMASCARA a IO-APIC agora (o driver esta pronto).
    if (ki->Gsi != 0xFFFFFFFFu)
        ioapic_set_irq_ex((uint8_t)ki->Gsi, (uint8_t)Vector, (uint8_t)apic_bsp_id(), 1, 1, 0);
    *InterruptObject = ki;
    kputs("[io] IoConnectInterrupt vetor=0x"); kput_hex(Vector); kputs(" -> conectado\n");
    return STATUS_SUCCESS;
}

void NTAPI IoDisconnectInterrupt_k(PKINTERRUPT ki) {
    if (!ki) return;
    ki->Connected = 0;
    g_interrupt_table[ki->Vector & 0xFF] = 0;
    if (ki->Gsi != 0xFFFFFFFFu)
        ioapic_set_irq_ex((uint8_t)ki->Gsi, (uint8_t)ki->Vector, (uint8_t)apic_bsp_id(), 1, 1, 1);   // re-mascara
}

void NTAPI KeInitializeInterrupt_k(PKINTERRUPT ki, PKSERVICE_ROUTINE Routine, PVOID Ctx,
    PKSPIN_LOCK Lock, ULONG Vector, KIRQL Irql, KIRQL SyncIrql, KINTERRUPT_MODE Mode, BOOLEAN Shared,
    KAFFINITY Aff, BOOLEAN Float) {
    (void)Shared; (void)Aff; (void)Float;
    if (!ki) return;
    ki->ServiceRoutine  = Routine;
    ki->ServiceContext  = Ctx;
    ki->ActualLock      = 0;
    ki->SpinLock        = Lock ? Lock : &ki->ActualLock;
    ki->Vector          = Vector;
    ki->Irql            = Irql;
    ki->SynchronizeIrql = SyncIrql;
    ki->Mode            = (int)Mode;
    ki->Gsi             = 0xFFFFFFFFu;
    ki->Connected       = 0;
}

BOOLEAN NTAPI KeConnectInterrupt_k(PKINTERRUPT ki) {
    if (!ki) return 0;
    ki->Connected = 1; g_interrupt_table[ki->Vector & 0xFF] = ki; return 1;
}
BOOLEAN NTAPI KeDisconnectInterrupt_k(PKINTERRUPT ki) {
    if (!ki) return 0;
    ki->Connected = 0; g_interrupt_table[ki->Vector & 0xFF] = 0; return 1;
}

BOOLEAN NTAPI KeSynchronizeExecution_k(PKINTERRUPT ki, PKSYNCHRONIZE_ROUTINE Routine, PVOID Ctx) {
    if (!Routine) return 0;
    if (!ki) return Routine(Ctx);
    KIRQL old; KeRaiseIrql_k(ki->SynchronizeIrql, &old);
    KeAcquireSpinLockAtDpcLevel_k(ki->SpinLock);
    BOOLEAN r = Routine(Ctx);
    KeReleaseSpinLockFromDpcLevel_k(ki->SpinLock);
    KeLowerIrql_k(old);
    return r;
}

__attribute__((ms_abi)) ULONG HalGetInterruptVector(ULONG BusType, ULONG BusNumber,
    ULONG BusInterruptLevel, ULONG BusInterruptVector, KIRQL* Irql, KAFFINITY* Affinity) {
    (void)BusType; (void)BusNumber; (void)BusInterruptVector;
    uint8_t gsi = (uint8_t)BusInterruptLevel;
    uint8_t vec = alloc_vector();
    if (!vec) return 0;
    s_vec_gsi[vec] = gsi;
    // IO-APIC: level-triggered + active-low (convencao PCI INTx), MASCARADA
    // (IoConnectInterrupt desmascara quando o driver conecta).
    ioapic_set_irq_ex(gsi, vec, (uint8_t)apic_bsp_id(), 1, 1, 1);
    if (Irql) *Irql = DIRQL_DEFAULT_DEVICE;
    if (Affinity) *Affinity = 1;
    kputs("[hal] HalGetInterruptVector gsi="); kput_dec(gsi);
    kputs(" -> vetor 0x"); kput_hex(vec); kputs(" irql=11 (mascarado)\n");
    return vec;
}

// --- Prova de boot: conecta um vetor SOFT (0x70, fora do pool) e dispara. ----
static volatile int s_int_proof = 0;
static BOOLEAN NTAPI test_isr(PKINTERRUPT I, PVOID Ctx) { (void)I; s_int_proof = (int)(uintptr_t)Ctx; return 1; }
void KiInterruptSelfTest(void) {
    PKINTERRUPT io = 0;
    static KSPIN_LOCK lock;
    lock = 0;
    s_int_proof = 0;
    NTSTATUS st = IoConnectInterrupt_k(&io, test_isr, (PVOID)(uintptr_t)0xABC, &lock, 0x70,
                                       DIRQL_DEFAULT_DEVICE, DIRQL_DEFAULT_DEVICE, LevelSensitive, 0, 1, 0);
    if (!NT_SUCCESS(st)) { kputs("[int-test] IoConnectInterrupt FALHOU\n"); return; }
    __asm__ volatile ("int $0x70");     // dispara como se fosse a IRQ do dispositivo
    if (s_int_proof == 0xABC) kputs("[int-test] IoConnectInterrupt + ISR dispatch em DIRQL OK\n");
    else { kputs("[int-test] FALHOU proof=0x"); kput_hex((uint64_t)(uint32_t)s_int_proof); kputs("\n"); }
    IoDisconnectInterrupt_k(io);
}
