// ============================================================================
//  FASE 7.8 — Deteccao de HYPERVISOR via CPUID.
//
//  Le e LOGA na serial:
//    1) Bit 31 de CPUID.1.ECX ("Hypervisor Present").
//    2) Leaf 0x40000000 (max leaf + vendor ID do hypervisor).
//    3) Leaves 0x40000001..0x40000005 (apenas as expostas pelo CPU/hypervisor).
//
//  No QEMU TCG SEM KVM, o esperado e:
//    - bit 31 = 0
//    - leaf 0x40000000 = zeros (ou lixo da leaf anterior)
//  Isso e o caminho seguro: drivers reais interpretam "sem hypervisor".
//
//  HalCpuidEx e exposto via ntoskrnl para drivers que queiram acessar a
//  instrucao CPUID com sub-leaf explicito (mesma semantica que HalCpuid).
// ============================================================================
#include "hal/hypervisor.h"
#include "hal/cpu.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

static hv_info_t s_hv;

// Imprime 4 bytes de um uint32_t como caracteres ASCII (substitui nao-imprimiveis
// por '.'). Usado para mostrar o vendor string sem confundir o terminal serial.
static void put_ascii4(uint32_t v) {
    for (int i = 0; i < 4; i++) {
        char c = (char)((v >> (i * 8)) & 0xFF);
        kputc((c >= 0x20 && c < 0x7F) ? c : '.');
    }
}

// Loga uma leaf 0x4000_00xx no formato:
//   [hv] leaf 0x4000000N: EAX=0x........ EBX=0x........ ECX=0x........ EDX=0x........
static void log_leaf(uint32_t leaf, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    kputs("[hv] leaf ");
    kput_hex((uint64_t)leaf);
    kputs(": EAX=");  kput_hex((uint64_t)a);
    kputs(" EBX=");   kput_hex((uint64_t)b);
    kputs(" ECX=");   kput_hex((uint64_t)c);
    kputs(" EDX=");   kput_hex((uint64_t)d);
    kputc('\n');
}

void hv_detect_init(void) {
    uint32_t a, b, c, d;

    // ------------------------------------------------------------------------
    // 1) CPUID.1 -> ECX bit 31 ("Hypervisor Present").
    //    Convencao Intel/AMD: reservado em HW real (0). Hypervisors que
    //    seguem o padrao acendem este bit quando o convidado executa CPUID.
    // ------------------------------------------------------------------------
    HalCpuid(1, 0, &a, &b, &c, &d);
    s_hv.hypervisor_present = (c & (1u << 31)) ? 1 : 0;
    kputs("[hv] CPUID.1.ECX bit31 (HypervisorPresent) = ");
    kput_dec(s_hv.hypervisor_present);
    kputc('\n');

    // ------------------------------------------------------------------------
    // 2) Leaf 0x40000000: max leaf + vendor string (EBX+ECX+EDX, 12 bytes).
    //    Em HW real (sem hypervisor) e em QEMU TCG sem paravirt, o CPU
    //    normalmente retorna zeros — mas algumas CPUs/QEMU retornam o ultimo
    //    valor valido (lixo): por isso LOGAMOS os bytes crus em hex E como
    //    ASCII para o usuario poder ver o que veio.
    // ------------------------------------------------------------------------
    HalCpuid(0x40000000, 0, &a, &b, &c, &d);
    s_hv.leaf_4000_0000_eax = a;
    s_hv.leaf_4000_0000_ebx = b;
    s_hv.leaf_4000_0000_ecx = c;
    s_hv.leaf_4000_0000_edx = d;

    // Monta a string vendor[12] = EBX | ECX | EDX (little-endian byte order).
    ((uint32_t*)s_hv.vendor)[0] = b;
    ((uint32_t*)s_hv.vendor)[1] = c;
    ((uint32_t*)s_hv.vendor)[2] = d;
    s_hv.vendor[12] = 0;

    log_leaf(0x40000000, a, b, c, d);
    kputs("[hv] vendor (ASCII) = '");
    put_ascii4(b); put_ascii4(c); put_ascii4(d);
    kputs("'\n");
    kputs("[hv] max hypervisor leaf = ");
    kput_hex((uint64_t)a);
    kputc('\n');

    // ------------------------------------------------------------------------
    // 3) Leaves seguintes: 0x40000001..0x40000005, apenas se max_leaf cobre.
    //    No QEMU TCG sem paravirt, a EAX da leaf 0x40000000 vem 0 entao nada
    //    extra e lido (caminho silencioso e seguro).
    // ------------------------------------------------------------------------
    uint32_t max_leaf = a;

    if (max_leaf >= 0x40000001) {
        HalCpuid(0x40000001, 0, &a, &b, &c, &d);
        s_hv.leaf_4000_0001_eax = a; s_hv.leaf_4000_0001_ebx = b;
        s_hv.leaf_4000_0001_ecx = c; s_hv.leaf_4000_0001_edx = d;
        log_leaf(0x40000001, a, b, c, d);
    }
    if (max_leaf >= 0x40000002) {
        HalCpuid(0x40000002, 0, &a, &b, &c, &d);
        s_hv.leaf_4000_0002_eax = a; s_hv.leaf_4000_0002_ebx = b;
        s_hv.leaf_4000_0002_ecx = c; s_hv.leaf_4000_0002_edx = d;
        log_leaf(0x40000002, a, b, c, d);
    }
    if (max_leaf >= 0x40000003) {
        HalCpuid(0x40000003, 0, &a, &b, &c, &d);
        s_hv.leaf_4000_0003_eax = a; s_hv.leaf_4000_0003_ebx = b;
        s_hv.leaf_4000_0003_ecx = c; s_hv.leaf_4000_0003_edx = d;
        log_leaf(0x40000003, a, b, c, d);
    }
    if (max_leaf >= 0x40000004) {
        HalCpuid(0x40000004, 0, &a, &b, &c, &d);
        s_hv.leaf_4000_0004_eax = a; s_hv.leaf_4000_0004_ebx = b;
        s_hv.leaf_4000_0004_ecx = c; s_hv.leaf_4000_0004_edx = d;
        log_leaf(0x40000004, a, b, c, d);
    }
    if (max_leaf >= 0x40000005) {
        HalCpuid(0x40000005, 0, &a, &b, &c, &d);
        s_hv.leaf_4000_0005_eax = a; s_hv.leaf_4000_0005_ebx = b;
        s_hv.leaf_4000_0005_ecx = c; s_hv.leaf_4000_0005_edx = d;
        log_leaf(0x40000005, a, b, c, d);
    }

    // Resumo final na serial.
    if (s_hv.hypervisor_present || max_leaf >= 0x40000000) {
        kputs("[hv] resumo: rodando dentro de hypervisor (vendor='");
        kputs(s_hv.vendor); kputs("').\n");
    } else {
        kputs("[hv] resumo: sem hypervisor anunciado (HW real OU QEMU TCG silencioso).\n");
    }
}

const hv_info_t* hv_info_get(void) { return &s_hv; }

// ----------------------------------------------------------------------------
//  HalCpuidEx — wrapper exportado via ntoskrnl. Mesma semantica do HalCpuid.
//  Drivers reais (NT 10) podem chamar tanto HalCpuid quanto HalCpuidEx; aqui
//  expomos os dois (HalCpuid ja estava em hal/cpu.c).
// ----------------------------------------------------------------------------
MS_ABI void HalCpuidEx(uint32_t leaf, uint32_t subleaf,
                       uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    HalCpuid(leaf, subleaf, a, b, c, d);
}
