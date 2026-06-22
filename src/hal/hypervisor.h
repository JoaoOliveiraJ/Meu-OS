#pragma once
#include <stdint.h>

#ifndef MS_ABI
#define MS_ABI __attribute__((ms_abi))
#endif

// ============================================================================
//  FASE 7.8 — Deteccao de HYPERVISOR via CPUID (leaves 0x40000000..0x40000005).
//
//  Convencao da industria (Intel/AMD/Microsoft/VMware/KVM/Xen):
//    - CPUID.1:ECX bit 31 = "Hypervisor Present" — 0 em HW real, 1 quando
//      o CPU esta dentro de um hypervisor que escolheu expor isso.
//    - CPUID leaves 0x40000000..0x4000FFFF sao a "vendor hypervisor leaves":
//        0x40000000 EAX = max leaf desse range; EBX/ECX/EDX = vendor string
//        (12 bytes ASCII). Exemplos:
//           "KVMKVMKVM\0\0\0"   -> KVM
//           "Microsoft Hv"     -> Hyper-V
//           "VMwareVMware"     -> VMware
//           "XenVMMXenVMM"     -> Xen
//           "TCGTCGTCGTCG"     -> QEMU TCG (alguns builds; nem sempre exposto)
//           "QEMUQEMUQEMU"     -> QEMU (alguns builds)
//
//  REALIDADE TECNICA NO QEMU TCG (nosso ambiente):
//    Sem KVM/Hyper-V/VMX/SVM, o CPUID e emulado pelo TCG. Normalmente:
//      - bit 31 de CPUID.1.ECX = 0 (TCG nao se anuncia como hypervisor)
//      - leaf 0x40000000 retorna zeros (ou lixo da ultima leaf valida,
//        dependendo da versao do QEMU).
//    Drivers que checam "HypervisorPresent" interpretam isso como HW real:
//    caminho seguro (sem hooks de paravirt).
//
//  NAO TENTAR PATCHAR CPUID VIA SIGILL: a instrucao CPUID nao gera #UD nem
//  #GP por leaf invalido; ela apenas retorna zeros/lixo. Nao temos VMX/SVM
//  para INTERCEPTAR. Logo, somos consumidores passivos do que o HW expoe.
// ============================================================================

typedef struct hv_info {
    // bit 31 de CPUID.1.ECX:
    uint8_t  hypervisor_present;          // 1 se CPUID.1.ECX[31]=1
    // Conteudo cru das 4 primeiras leaves do range 0x40000000:
    uint32_t leaf_4000_0000_eax;          // max leaf no range (0 se ausente)
    uint32_t leaf_4000_0000_ebx;          // vendor[0..3]
    uint32_t leaf_4000_0000_ecx;          // vendor[4..7]
    uint32_t leaf_4000_0000_edx;          // vendor[8..11]
    char     vendor[13];                  // string ASCII (EBX+ECX+EDX), NUL-term

    // Leaves seguintes (so leem se max_leaf >= delas). Cada leaf da uma janela
    // de interface — drivers KVM/Hyper-V usam essas para versao/features/etc.
    uint32_t leaf_4000_0001_eax, leaf_4000_0001_ebx, leaf_4000_0001_ecx, leaf_4000_0001_edx;
    uint32_t leaf_4000_0002_eax, leaf_4000_0002_ebx, leaf_4000_0002_ecx, leaf_4000_0002_edx;
    uint32_t leaf_4000_0003_eax, leaf_4000_0003_ebx, leaf_4000_0003_ecx, leaf_4000_0003_edx;
    uint32_t leaf_4000_0004_eax, leaf_4000_0004_ebx, leaf_4000_0004_ecx, leaf_4000_0004_edx;
    uint32_t leaf_4000_0005_eax, leaf_4000_0005_ebx, leaf_4000_0005_ecx, leaf_4000_0005_edx;
} hv_info_t;

// Le e LOGA na serial todas as informacoes de hypervisor (CPUID.1 bit 31 +
// leaves 0x40000000..0x40000005). Chame APOS cpu_features_init().
void hv_detect_init(void);

// Snapshot do que foi lido.
const hv_info_t* hv_info_get(void);

// Wrapper exportavel via ntoskrnl: igual ao HalCpuid, mas explicitamente para
// drivers que querem ler leaves de hypervisor sem se preocupar com sub-leaf.
// (Drivers reais ja chamam HalCpuid; expomos HalCpuidEx p/ consistencia com a
// API documentada da Microsoft de algumas versoes do HAL.)
MS_ABI void HalCpuidEx(uint32_t leaf, uint32_t subleaf,
                       uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d);
