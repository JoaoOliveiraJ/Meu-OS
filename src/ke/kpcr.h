// ============================================================================
//  FASE 7.2 — KPCR (Kernel Processor Control Region) do NT x64.
//
//  Estrutura unica POR CPU mapeada em GS_BASE (MSR 0xC0000101). Drivers reais
//  do Windows leem campos relativos a gs:[..] sem qualquer chamada de API:
//      mov rax, gs:[0x188]   ; KPRCB.CurrentThread (offset 0x180+0x08)
//      mov eax, gs:[0x1B8]   ; KPRCB.Number        (offset 0x180+0x80 == 0x1B8)
//      mov  cl, gs:[0x60]    ; KPCR.Irql
//      ...
//
//  Sem KPCR mapeado, qualquer driver real cai em Page Fault na primeira leitura
//  via gs (em pintok.sys foi cr2=0x58B948C6 — leitura via gs:[xxx] espalhada).
//
//  LAYOUT do KPCR x64 (do WRK/ReactOS docs — Windows 10 build 19041+):
//    +0x000 GdtBase          (PVOID)
//    +0x008 TssBase          (PVOID)
//    +0x010 UserRsp          (uint64)
//    +0x018 Self             (KPCR*)        <-- ponteiro p/ si mesmo
//    +0x020 CurrentPrcb      (KPRCB*)       <-- aponta p/ &this->Prcb
//    +0x028 LockArray        (PVOID)
//    +0x030 Used_Self        (PVOID)        <-- KPCR*
//    +0x038 IdtBase          (PVOID)
//    +0x040..+0x05F          (reserved/unused)
//    +0x060 Irql             (UCHAR)        <-- KeGetCurrentIrql gs-relative
//    +0x064 SecondLevelCacheAssociativity (ULONG)
//    +0x0F4 MajorVersion     (USHORT) = 1
//    +0x0F6 MinorVersion     (USHORT) = 1
//    +0x100 StallScaleFactor (ULONG)  = 100
//    +0x180 Prcb             (KPRCB inline; ~3712 bytes)
//
//  KPRCB (offsets relativos ao inicio da Prcb = 0x180 do KPCR):
//    +0x000 MxCsr            (ULONG)
//    +0x008 LegacyNumber     (UCHAR)
//    +0x010 CurrentThread    (PKTHREAD)    <-- gs:[0x190]
//    +0x018 NextThread       (PKTHREAD)
//    +0x020 IdleThread       (PKTHREAD)
//    +0x080 Number           (ULONG)       ProcessorNumber  <-- gs:[0x200]
//
//  Obs: muitos drivers leem por offsets que MUDAM entre builds. Garantimos os
//  mais comuns (Self/Prcb/CurrentThread/Number). Para um driver de "media casa"
//  como pintok.sys isso ja resolve.
// ============================================================================
#pragma once
#include <stdint.h>
#include "ntddk.h"

// MSRs
#define MSR_IA32_GS_BASE        0xC0000101u   // GS_BASE
#define MSR_IA32_KERNEL_GS_BASE 0xC0000102u   // KERNEL_GS_BASE (swapgs target)

// KPRCB (subset). Total ~3712 bytes (4 KiB - 0x180).
typedef struct __attribute__((packed)) _KPRCB {
    uint32_t MxCsr;              // +0x000
    uint32_t _pad_004;           // +0x004
    uint8_t  LegacyNumber;       // +0x008
    uint8_t  _pad_009[7];        // +0x009..+0x00F
    void*    CurrentThread;      // +0x010 (PKTHREAD)
    void*    NextThread;         // +0x018
    void*    IdleThread;         // +0x020
    uint8_t  _pad_028[0x80 - 0x28]; // +0x028..+0x07F
    uint32_t Number;             // +0x080 (ProcessorNumber)
    uint8_t  _pad_084[3712 - 0x84]; // o resto do PRCB ate caber em 4 KiB total
} KPRCB;

// KPCR (4 KiB).
typedef struct __attribute__((packed)) _KPCR {
    void*    GdtBase;            // +0x000
    void*    TssBase;            // +0x008
    uint64_t UserRsp;            // +0x010
    void*    Self;               // +0x018  <-- self-pointer
    KPRCB*   CurrentPrcb;        // +0x020  <-- &this->Prcb
    void*    LockArray;          // +0x028
    void*    Used_Self;          // +0x030
    void*    IdtBase;            // +0x038
    uint8_t  _pad_040[0x60 - 0x40]; // +0x040..+0x05F
    uint8_t  Irql;               // +0x060
    uint8_t  _pad_061[3];        // +0x061..+0x063
    uint32_t SecondLevelCacheAssociativity; // +0x064
    uint8_t  _pad_068[0xF4 - 0x68]; // +0x068..+0x0F3
    uint16_t MajorVersion;       // +0x0F4
    uint16_t MinorVersion;       // +0x0F6
    uint8_t  _pad_0F8[0x100 - 0xF8]; // +0x0F8..+0x0FF
    uint32_t StallScaleFactor;   // +0x100
    uint8_t  _pad_104[0x180 - 0x104]; // +0x104..+0x17F
    KPRCB    Prcb;               // +0x180 (inline)
} KPCR;

// Inicializa KPCR e o programa em GS_BASE / KERNEL_GS_BASE.
// Loga "[kpcr] ..." na serial. Depois disso drivers podem ler gs:[..].
void kpcr_init(void);

// Acesso ao KPCR atual (apenas para uso interno do kernel; em UP basta 1 ptr).
KPCR* kpcr_get(void);

// Pequenos helpers para drivers (e p/ ntexec exports).
uint32_t kpcr_processor_number(void);
void*    kpcr_current_thread(void);
