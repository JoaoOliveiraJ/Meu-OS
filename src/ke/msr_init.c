// ============================================================================
//  FASE 7.4 — Habilitacao da instrucao SYSCALL (Intel/AMD long mode).
//
//  Drivers reais do NT (pintok.sys, etc.) e ntdll.dll (em ring 3) usam SYSCALL
//  como a forma rapida de pedir um servico do kernel. Sem habilitar a extensao
//  no EFER, a instrucao gera #GP (general protection) — exatamente o que o
//  pintok.sys disparou apos a Fase 7.3 (rip=0x5EA6673, err=0, sem CR2 porque
//  nao e Page Fault).
//
//  O HW exige 4 MSRs configurados:
//   - EFER (0xC0000080) bit 0 (SCE): habilita SYSCALL/SYSRET.
//   - STAR (0xC0000081):
//        bits 47:32 = base do segmento de KERNEL (CS = base, SS = base+8).
//        bits 63:48 = base do segmento de USER  (SYSRET: CS = base+16, SS = base+8).
//   - LSTAR (0xC0000082): RIP do handler (em syscall_entry.asm, ring 0).
//   - SFMASK (0xC0000084): bits do RFLAGS LIMPOS na entrada. Padrao NT:
//        0x4700 = TF(0x100) | IF(0x200) | DF(0x400) | AC(0x40000).
//   - CSTAR (0xC0000083): handler de 32-bit "compat SYSCALL". Aqui apontamos
//        para o MESMO entry — o codigo no entry funciona para os dois modos
//        (32-bit raramente usa SYSCALL no x64; deixar 0 daria #GP se algo
//        tentasse). Caminho seguro.
//
//  GDT do MeuOS (de ke/gdt.c):
//      SEL_KCODE = 0x08  (ring0 64-bit code)
//      SEL_KDATA = 0x10  (ring0 data)
//      SEL_UCODE = 0x18  (ring3 64-bit code)
//      SEL_UDATA = 0x20  (ring3 data)
//
//  STAR.kernel = 0x08 ; CPU usa CS=0x08, SS=0x10  -> bate com nossa GDT.
//  STAR.user   = 0x10 ; CPU (SYSRETQ) usaria CS=0x20|3 e SS=0x18|3 — INVERTIDO
//                       em relacao ao layout NT classico, MAS nos NAO usamos
//                       SYSRETQ (o syscall_entry volta com IRETQ explicito).
//                       Entao podemos por qualquer base aqui — escolhemos um
//                       valor "neutro" que nao gera selector invalido se algum
//                       outro caminho tente SYSRET. Usamos 0x10 (KDATA): caso
//                       SYSRET seja chamado, CS=0x20|3=0x23 (UDATA|3) e
//                       SS=0x18|3=0x1B (UCODE|3) — TAMBEM invertidos, mas
//                       AMBOS sao DPL3 e existem na nossa GDT, entao NAO ha
//                       #GP por selector NULL/invalido. Caminho seguro.
//
//  Apos esta fase: 'syscall' executado de ring 3 ou ring 0 NAO mais #GP. Ele
//  cai em syscall_entry, monta o frame e chama isr_handler. Isso desbloqueia
//  qualquer driver que probe a instrucao (caso do pintok.sys).
// ============================================================================
#include <stdint.h>
#include "hal/cpu.h"          // HalWriteMsr / HalReadMsr (FASE 7)

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);

// Numeros de MSR (do CPU manual / WDK).
#define MSR_EFER         0xC0000080u
#define MSR_STAR         0xC0000081u
#define MSR_LSTAR        0xC0000082u
#define MSR_CSTAR        0xC0000083u
#define MSR_SFMASK       0xC0000084u

// Bit 0 do EFER = SCE (System Call Extensions). Necessario p/ SYSCALL/SYSRET.
#define EFER_SCE         (1ull << 0)

// Symbol do handler em syscall_entry.asm (RIP que o CPU carrega no SYSCALL).
extern void syscall_entry(void);

// Habilita SYSCALL: chamar APOS gdt_init() + idt_init() + kpcr_init(),
// pois usa HalWriteMsr e segmentos definidos pela GDT do kernel.
void syscall_msr_init(void) {
    // 1) STAR — segmentos de codigo p/ kernel e user.
    //    Bits 47:32 = 0x0008 (SEL_KCODE). CPU carregara CS=0x08, SS=0x10.
    //    Bits 63:48 = 0x0010 (KDATA "neutro"; ver explicacao no header). Nao
    //    usamos SYSRETQ, entao esses bits so precisam ser selectors validos.
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    HalWriteMsr(MSR_STAR, star);

    // 2) LSTAR — RIP do handler em ring 0 quando SYSCALL e executado em 64-bit.
    HalWriteMsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);

    // 3) CSTAR — RIP para SYSCALL em compatibility mode (32-bit). Apontamos
    //    para o MESMO handler (drivers reais ignoram, ntdll usa apenas LSTAR).
    HalWriteMsr(MSR_CSTAR, (uint64_t)(uintptr_t)&syscall_entry);

    // 4) SFMASK — bits do RFLAGS limpos na entrada. Padrao NT:
    //    IF (0x200) — interrupts off enquanto o entry ainda nao salvou o state.
    //    DF (0x400) — direction flag zerada (ABI exige DF=0 ao chamar C).
    //    TF (0x100) — single-step desligado.
    //    AC (0x40000) — alignment check desligado.
    //    Soma = 0x40700. NT classico usa 0x4700 (sem AC); usamos isso para
    //    bater com o que pintok.sys e ntdll esperam.
    HalWriteMsr(MSR_SFMASK, 0x4700ull);

    // 5) EFER.SCE = 1 — DEPOIS de configurar LSTAR/STAR. Le, faz OR, escreve.
    uint64_t efer = HalReadMsr(MSR_EFER);
    HalWriteMsr(MSR_EFER, efer | EFER_SCE);

    // 6) Log compacto p/ debug futuro (todos os MSRs do SYSCALL relidos).
    kputs("[syscall] EFER=");  kput_hex(HalReadMsr(MSR_EFER));
    kputs(" STAR=");           kput_hex(HalReadMsr(MSR_STAR));
    kputs(" LSTAR=");          kput_hex(HalReadMsr(MSR_LSTAR));
    kputs(" SFMASK=");         kput_hex(HalReadMsr(MSR_SFMASK));
    kputs("\n[syscall] SCE habilitado: instrucao SYSCALL (Intel/AMD) ativa em paralelo ao int 0x80.\n");
}
