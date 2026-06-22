// ============================================================================
//  FASE 7.7 — CPU FEATURES (CR4 + XCR0).
//
//  Habilita as features estendidas que o NT real liga: OSXSAVE, AVX (via XCR0),
//  SMEP/SMAP/UMIP/PCIDE quando o HW suporta. Cada bit e GATEADO por CPUID antes
//  de ser setado: bit reservado em CR4 ou xsetbv sem OSXSAVE = #GP imediato.
//
//  ORDEM IMPORTA:
//    1) CPUID.1   -> ECX/EDX (XSAVE base, PCID, OSXSAVE-capable)
//    2) CPUID.7.0 -> EBX/ECX (SMEP, SMAP, UMIP, etc.)
//    3) CPUID.D.0 -> EAX     (mascara XCR0 valida pelo CPU; AVX = bit 2)
//    4) Habilita OSXSAVE PRIMEIRO (se XSAVE disponivel).
//    5) xsetbv XCR0 <- 0x7 (FPU+SSE+AVX) se AVX, senao 0x3 (FPU+SSE).
//    6) Habilita SMEP / SMAP / UMIP / PCIDE (em separado, cada um gateado).
//
//  PCIDE so e seguro setar com CR3.PCID=0 (e o nosso caso: boot.asm carrega
//  cr3=p4_table sem PCID). Setar com CR3 invalido p/ PCIDE = #GP.
// ============================================================================
#include "ke/cpu_features.h"
#include "hal/cpu.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);

static cpu_features_t s_feat;

// --- Inline asm helpers (Intel SDM Vol.3, Ch.2 + Ch.13) -----------------------

// Le CR4.
static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}
// Escreve CR4.
static inline void write_cr4(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory");
}

// xsetbv: escreve um XCR. Requer CR4.OSXSAVE=1, senao #UD.
static inline void xsetbv_w(uint32_t xcr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("xsetbv" : : "a"(lo), "d"(hi), "c"(xcr));
}

// --- Constantes (Intel SDM) ---------------------------------------------------

// CR4 bits
#define CR4_OSFXSR    (1ull << 9)
#define CR4_OSXMMEXC  (1ull << 10)
#define CR4_UMIP      (1ull << 11)
#define CR4_PCIDE     (1ull << 17)
#define CR4_OSXSAVE   (1ull << 18)
#define CR4_SMEP      (1ull << 20)
#define CR4_SMAP      (1ull << 21)

// XCR0 bits
#define XCR0_X87      (1ull << 0)
#define XCR0_SSE      (1ull << 1)
#define XCR0_AVX      (1ull << 2)

// CPUID bits que nos interessam:
//   CPUID.1.ECX  : XSAVE   = bit 26
//                  OSXSAVE = bit 27  (espelho de CR4.OSXSAVE; nao usamos p/ detectar)
//                  AVX     = bit 28
//                  PCID    = bit 17
//   CPUID.7.0.EBX: SMEP    = bit 7
//                  SMAP    = bit 20
//   CPUID.7.0.ECX: UMIP    = bit 2
#define BIT(n) (1u << (n))

void cpu_features_init(void) {
    uint32_t a, b, c, d;

    // ------------------------------------------------------------------------
    // 1) CPUID basicos: 1 e 7.0 (precisamos checar antes que 7 esta disponivel
    //    consultando o maximo leaf no CPUID.0.EAX — todo CPU x86-64 expoe >= 7,
    //    mas a checagem mantem o codigo correto em HW raro).
    // ------------------------------------------------------------------------
    HalCpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;

    HalCpuid(1, 0, &a, &b, &c, &d);
    uint32_t cpuid1_ecx = c;
    uint32_t cpuid1_edx = d;
    (void)cpuid1_edx;

    uint32_t cpuid7_ebx = 0, cpuid7_ecx = 0;
    if (max_leaf >= 7) {
        HalCpuid(7, 0, &a, &b, &c, &d);
        cpuid7_ebx = b;
        cpuid7_ecx = c;
    }

    s_feat.has_xsave   = (cpuid1_ecx & BIT(26)) ? 1 : 0;
    // AVX requer XSAVE + AVX bit em CPUID.1.ECX[28].
    s_feat.has_avx     = (s_feat.has_xsave && (cpuid1_ecx & BIT(28))) ? 1 : 0;
    s_feat.has_pcide   = (cpuid1_ecx & BIT(17)) ? 1 : 0;
    s_feat.has_smep    = (cpuid7_ebx & BIT(7))  ? 1 : 0;
    s_feat.has_smap    = (cpuid7_ebx & BIT(20)) ? 1 : 0;
    s_feat.has_umip    = (cpuid7_ecx & BIT(2))  ? 1 : 0;
    s_feat.has_osxsave = s_feat.has_xsave;   // OSXSAVE so faz sentido com XSAVE

    // ------------------------------------------------------------------------
    // 2) Habilita OSXSAVE PRIMEIRO (se XSAVE base existir). Sem isso, a
    //    instrucao 'xsetbv' gera #UD. Setar OSXSAVE com XSAVE ausente = #GP,
    //    por isso o gate em has_xsave.
    // ------------------------------------------------------------------------
    uint64_t cr4 = read_cr4();
    uint64_t cr4_before = cr4;

    if (s_feat.has_osxsave) {
        cr4 |= CR4_OSXSAVE;
        write_cr4(cr4);
    }

    // ------------------------------------------------------------------------
    // 3) XCR0: monta a mascara real consultando CPUID.0D.0.EAX (bits validos
    //    do XCR0 segundo o CPU). Mantemos X87+SSE (sempre validos em x86-64
    //    com OSFXSR ja setado pelo boot.asm) e somamos AVX se suportado.
    // ------------------------------------------------------------------------
    if (s_feat.has_osxsave) {
        uint64_t xcr0 = XCR0_X87 | XCR0_SSE;
        if (s_feat.has_avx && max_leaf >= 0x0D) {
            uint32_t da, db, dc, dd;
            HalCpuid(0x0D, 0, &da, &db, &dc, &dd);
            // EAX = bits validos do XCR0; checa que AVX (bit 2) realmente
            // esta no conjunto suportado pelo CPU (segunda camada de seguranca).
            if (da & BIT(2)) xcr0 |= XCR0_AVX;
        }
        xsetbv_w(0, xcr0);
        s_feat.xcr0_final = xcr0;
    } else {
        s_feat.xcr0_final = 0;
    }

    // ------------------------------------------------------------------------
    // 4) SMEP/SMAP/UMIP/PCIDE — cada um gateado pelo CPUID.
    //    OBS: SMAP, depois de ligado, faz acessos do kernel a paginas com
    //    PTE.U=1 (user) gerarem #PF a menos que o codigo execute STAC/CLAC.
    //    O kernel do MeuOS hoje toca regioes user (PE loader copia bytes para
    //    o image base em USER pages quando carrega .exe). Por seguranca,
    //    DEIXAMOS SMAP COMENTADO ate adicionarmos os pares stac/clac no copy
    //    path. Mesma logica vale parcialmente p/ SMEP (impede CPL0 executar
    //    pagina user); como o kernel nunca pula p/ codigo user direto (sempre
    //    via IRET com CS=ring3), SMEP e seguro.
    // ------------------------------------------------------------------------
    if (s_feat.has_smep)  cr4 |= CR4_SMEP;
    // if (s_feat.has_smap)  cr4 |= CR4_SMAP;   // CUIDADO: ver comentario acima.
    if (s_feat.has_umip)  cr4 |= CR4_UMIP;
    // PCIDE: setar exige que CR3[11:0] == 0 (sem PCID em uso). Nosso boot.asm
    // carregou cr3=p4_table com PCID=0, entao e seguro.
    if (s_feat.has_pcide) cr4 |= CR4_PCIDE;

    write_cr4(cr4);
    s_feat.cr4_final = cr4;
    // s_feat.has_smap reflete capacidade (nao se foi setado). Se voce
    // descomentar a linha do SMAP acima, atualize a logica conforme necessario.

    // ------------------------------------------------------------------------
    // 5) Log (sempre na serial p/ regressao).
    // ------------------------------------------------------------------------
    kputs("[cpu] CR4="); kput_hex(cr4);
    kputs(" (was "); kput_hex(cr4_before); kputs(")");
    if (s_feat.has_osxsave) { kputs(" XCR0="); kput_hex(s_feat.xcr0_final); }
    kputs(" features:");
    if (s_feat.has_osxsave) kputs(" OSXSAVE");
    if (s_feat.has_avx)     kputs(" AVX");
    if (s_feat.has_smep)    kputs(" SMEP");
    if (s_feat.has_smap)    kputs(" SMAP(detected,not-set)");
    if (s_feat.has_umip)    kputs(" UMIP");
    if (s_feat.has_pcide)   kputs(" PCIDE");
    kputc('\n');
}

const cpu_features_t* cpu_features_get(void) { return &s_feat; }
