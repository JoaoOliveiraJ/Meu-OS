// ============================================================================
//  FASE 7.2 — KPCR init: aloca a estrutura, preenche os campos minimos e
//  programa GS_BASE/KERNEL_GS_BASE (MSRs 0xC0000101 / 0xC0000102).
//
//  Em UP (uniprocessor) basta UM KPCR. Mantemos um ponteiro estatico (g_kpcr).
//  A pagina inteira de 4 KiB e zerada antes de preencher; campos nao tocados
//  ficam 0 (caminho seguro: muitos drivers tratam 0/null como "nao habilitado").
//
//  Tamanho garantido: sizeof(KPCR) <= 4096. Se isso quebrar (mudanca de layout),
//  o kpcr_init() loga e ABORTA o caminho — o boot continua sem KPCR (drivers
//  que leem gs:[..] ainda quebram, mas o kernel nao crasha).
// ============================================================================
#include "ke/kpcr.h"
#include "hal/cpu.h"      // HalWriteMsr / HalReadMsr
#include "mm/heap.h"
#include <stddef.h>

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern void* memset(void* dst, int v, size_t n);

// O KPCR de CPU 0 (UP). Alocado dinamicamente em kpcr_init() para garantir que
// caiba uma pagina inteira contigua e zerada.
static KPCR* g_kpcr = 0;

// KTHREAD fake: drivers leem gs:[0x188] (KPRCB.CurrentThread). Se ainda nao
// temos um scheduler real, criamos um objeto KTHREAD opaco zerado que aceita
// tudo. 512 bytes cobrem o que a maioria dos drivers consulta (Header + APC).
static uint8_t* g_fake_thread = 0;

KPCR* kpcr_get(void) { return g_kpcr; }

uint32_t kpcr_processor_number(void) {
    return g_kpcr ? g_kpcr->Prcb.Number : 0;
}
void* kpcr_current_thread(void) {
    return g_kpcr ? g_kpcr->Prcb.CurrentThread : 0;
}

void kpcr_init(void) {
    // Sanidade: a struct deve caber em 4 KiB. Se nao, o layout esta errado.
    if (sizeof(KPCR) > 4096) {
        kputs("[kpcr] ERRO: sizeof(KPCR) > 4096; nao mapearei GS_BASE.\n");
        return;
    }

    // 1) Aloca uma pagina inteira para o KPCR. Usa kmalloc (o heap pode dar
    //    blocos > 4 KiB com baixo overhead, mas garantimos zerar 4096 bytes).
    void* mem = kmalloc(4096);
    if (!mem) {
        kputs("[kpcr] ERRO: kmalloc(4096) p/ KPCR falhou.\n");
        return;
    }
    memset(mem, 0, 4096);
    g_kpcr = (KPCR*)mem;

    // 2) Aloca o KTHREAD fake (512 bytes zerados).
    g_fake_thread = (uint8_t*)kmalloc(512);
    if (g_fake_thread) memset(g_fake_thread, 0, 512);

    // 3) Preenche os campos mais consultados por drivers reais.
    //    Self / Used_Self / CurrentPrcb sao os "alvos" classicos de
    //      mov rax, gs:[0x18]   ; Self
    //      lea rax, gs:[0x180]  ; CurrentPrcb (ou via gs:[0x20])
    g_kpcr->Self        = g_kpcr;
    g_kpcr->Used_Self   = g_kpcr;
    g_kpcr->CurrentPrcb = &g_kpcr->Prcb;
    g_kpcr->GdtBase     = 0;       // sem ponteiro real publicado por enquanto
    g_kpcr->TssBase     = 0;
    g_kpcr->IdtBase     = 0;
    g_kpcr->LockArray   = 0;
    g_kpcr->Irql        = 0;       // PASSIVE_LEVEL
    g_kpcr->MajorVersion= 1;
    g_kpcr->MinorVersion= 1;
    g_kpcr->StallScaleFactor = 100;
    g_kpcr->SecondLevelCacheAssociativity = 8;

    // 4) Preenche o PRCB inline (subset).
    g_kpcr->Prcb.MxCsr         = 0x1F80;        // valor de reset
    g_kpcr->Prcb.LegacyNumber  = 0;
    g_kpcr->Prcb.CurrentThread = g_fake_thread; // gs:[0x190] = 0x180 + 0x10
    g_kpcr->Prcb.NextThread    = 0;
    g_kpcr->Prcb.IdleThread    = g_fake_thread;
    g_kpcr->Prcb.Number        = 0;             // CPU 0

    // 5) Programa GS_BASE e KERNEL_GS_BASE com o endereco do KPCR. Mantemos
    //    ambos iguais (sem swapgs assimetrico): se um driver fizer swapgs,
    //    continuara apontando para o mesmo KPCR (caminho seguro em UP).
    uint64_t addr = (uint64_t)(uintptr_t)g_kpcr;
    HalWriteMsr(MSR_IA32_GS_BASE,        addr);
    HalWriteMsr(MSR_IA32_KERNEL_GS_BASE, addr);

    // 6) Log compacto: enderecos uteis p/ debug futuro.
    kputs("[kpcr] KPCR @ "); kput_hex(addr);
    kputs(" | Self="); kput_hex((uint64_t)(uintptr_t)g_kpcr->Self);
    kputs(" | Prcb="); kput_hex((uint64_t)(uintptr_t)g_kpcr->CurrentPrcb);
    kputs(" | gs_base="); kput_hex(HalReadMsr(MSR_IA32_GS_BASE));
    kputc('\n');
    kputs("[kpcr] sizeof(KPCR)="); kput_dec(sizeof(KPCR));
    kputs(" sizeof(KPRCB)="); kput_dec(sizeof(KPRCB));
    kputs(" CurrentThread@gs:0x190="); kput_hex((uint64_t)(uintptr_t)g_kpcr->Prcb.CurrentThread);
    kputc('\n');
}

// ----------------------------------------------------------------------------
//  Exports adicionais que drivers podem chamar (em vez de ler gs:[..] direto).
//  Usamos ms_abi pra bater com a convenicao do Windows (NTAPI).
// ----------------------------------------------------------------------------
__attribute__((ms_abi)) ULONG KeGetCurrentProcessorNumber_k(void) {
    // Le gs:[0x180 + 0x80] = gs:[0x200]. Se KPCR estiver mapeado, devolve 0
    // (CPU 0). Senao, fallback p/ ler o campo da estrutura.
    if (!g_kpcr) return 0;
    uint32_t n = 0;
    __asm__ volatile ("mov %%gs:0x200, %0" : "=r"(n));
    return (ULONG)n;
}
__attribute__((ms_abi)) ULONG KeGetCurrentProcessorNumberEx_k(void* ProcNumber) {
    (void)ProcNumber;   // PROCESSOR_NUMBER opcional (Group=0, Number=0, Reserved=0)
    return KeGetCurrentProcessorNumber_k();
}
