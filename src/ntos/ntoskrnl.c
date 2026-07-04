// ============================================================================
//  ntexec.c — Executiva NT (mini ntoskrnl.exe) do MeuOS.
//
//  Esta e a tabela de exports do "ntoskrnl.exe" que o pe_bind_imports usa
//  para resolver os imports dos drivers .sys do Windows. FASE 7 (Driver
//  Framework Completo) expandiu massivamente:
//
//  - IO Manager: rotinas de suporte (IoCompleteRequest, IoAllocateIrp, ...).
//  - Callbacks (Ps/Ob/Cm/Ex): registrar/remover + tipos publicos.
//  - Sincronizacao (Ke*): KEVENT, KSPIN_LOCK, KMUTEX, KSEMAPHORE, KeWait*.
//  - Pool (Ex*): ExAllocatePool com tag tracking.
//  - System threads: PsCreateSystemThread + Ps* de inspecao.
//  - Memoria (Mm*): MmGetPhysicalAddress, MmMapIoSpace, MmAllocateContiguous,
//    MmAllocateNonCachedMemory, MmIsAddressValid.
//  - Section objects: NtCreateSection / NtMapViewOfSection.
//  - Registro (Cm/Nt*Key): hive em memoria.
//  - Crypto stubs (Bcrypt/Ci/Hash): retornam STATUS_SUCCESS (caminho seguro).
//  - Debug (KdDebuggerEnabled, DbgPrompt, DbgPrint, KeBugCheck).
//  - Rtl* / Cc* / Se* — stubs/no-ops.
//
//  Tudo em __attribute__((ms_abi)) (ABI Microsoft, como o WDM do Windows).
//  Exports nao encontrados na tabela caem num stub generico que retorna 0 e
//  loga "[ntex] export desconhecido: nome" — caminho seguro.
// ============================================================================
#include "ntddk.h"
#include "ntoskrnl.h"
#include "ldr/loader.h"      // GATE 2: ldr_get_module_count/info p/ SystemModuleInformation
#include "io/io.h"
#include "ob/object.h"
#include "ex/callbacks.h"
#include "mm/section.h"
#include "cm/registry.h"
#include "ke/sync.h"
#include "ke/dpc.h"                 // FASE FUNDACAO (Item 2): DPC real
#include "ke/timer.h"               // FASE FUNDACAO (Item 6): KTIMER real
#include "ex/ex_sync.h"             // FASE FUNDACAO (Item 7): primitivos Ex reais
#include "ke/amd64/kinterrupt.h"    // trilha I/O Fase 3: modelo de interrupcao
#include "hal/haldma.h"             // trilha I/O Fase 5: HAL DMA
#include "ex/pool.h"
#include "ps/systhread.h"
#include "ke/amd64/kpcr.h"          // FASE 7.2: KPCR/GS_BASE
#include "hal/cpu.h"
#include "hal/hal.h"
#include "hal/hypervisor.h"   // FASE 7.8: HalCpuidEx + hv_info_get
#include "ps/process.h"
#include "ps/cid_table.h"     // FASE 7.5: PspCidTable exposta como variavel
#include "mm/mdl.h"           // FASE 7.6: MDL real (IoAllocateMdl/MmProbeAndLockPages/...)
#include "mm/virtmem.h"// FASE 5: Mm*/Nt*VirtualMemory (alloc/free/protect)
// FASE DX: dispatcher DirectX em kernel (dxgkrnl) + memory manager video (dxgmms).
#include "dx/dxgkrnl/dxgkrnl.h"
#include "dx/dxgmms/dxgmms.h"

// FASE 12 (network stack) — NDIS APIs ms_abi expostas como exports do ntoskrnl.
// Os simbolos vivem em src/drivers/network/ndis/ndis.c.
typedef int NDIS_STATUS_t;
typedef void* NDIS_HANDLE_t;
typedef void* PNET_BUFFER_LIST_t;
__attribute__((ms_abi)) void          ndis_NdisInitializeWrapper(void**, void*, void*, void*);
__attribute__((ms_abi)) NDIS_STATUS_t ndis_NdisMRegisterMiniportDriver(void*, void*, void*, void*, void**);
__attribute__((ms_abi)) NDIS_STATUS_t ndis_NdisRegisterProtocolDriver(void*, void*, void**);
__attribute__((ms_abi)) PNET_BUFFER_LIST_t ndis_NdisAllocateNetBufferList(void*, uint16_t, uint16_t);
__attribute__((ms_abi)) void          ndis_NdisFreeNetBufferList(PNET_BUFFER_LIST_t);
__attribute__((ms_abi)) void          ndis_NdisMSendNetBufferListsComplete(void*, PNET_BUFFER_LIST_t, uint32_t);
__attribute__((ms_abi)) void          ndis_NdisFreeMemory(void*, uint32_t, uint32_t);
__attribute__((ms_abi)) NDIS_STATUS_t ndis_NdisAllocateMemoryWithTag(void**, uint32_t, uint32_t);
__attribute__((ms_abi)) void          ndis_NdisMResetComplete(void*, NDIS_STATUS_t, int);
__attribute__((ms_abi)) uint32_t      ndis_NdisGetVersion(void);

// FASE 13 — PnP Manager + Filter Manager APIs ms_abi (exports).
// Sao implementadas em src/ntos/io/pnp.c e src/drivers/fltmgr/fltmgr.c. Forwards
// aqui evitam puxar os headers (que arrastariam ntddk -> ciclos de include).
typedef void* PFLT_FILTER_t;
typedef void* PFLT_PORT_t;
typedef void* PFLT_CALLBACK_DATA_t;
__attribute__((ms_abi)) void     pnp_IoInvalidateDeviceState(void*);
__attribute__((ms_abi)) NTSTATUS pnp_IoReportDeviceObject(void*, const char*);
__attribute__((ms_abi)) NTSTATUS fltmgr_FltRegisterFilter(void*, void*, PFLT_FILTER_t*);
__attribute__((ms_abi)) NTSTATUS fltmgr_FltStartFiltering(PFLT_FILTER_t);
__attribute__((ms_abi)) void     fltmgr_FltUnregisterFilter(PFLT_FILTER_t);
__attribute__((ms_abi)) NTSTATUS fltmgr_FltCreateCommunicationPort(PFLT_FILTER_t, PFLT_PORT_t*,
                                                                   void*, void*, void*, void*, void*, int);
__attribute__((ms_abi)) NTSTATUS fltmgr_FltSendMessage(PFLT_FILTER_t, PFLT_PORT_t*,
                                                       void*, uint32_t, void*, uint32_t*, void*);
__attribute__((ms_abi)) void     fltmgr_FltSetCallbackDataDirty(PFLT_CALLBACK_DATA_t);

// Forwards das funcoes implementadas em ke/kpcr.c (NTAPI / ms_abi).
__attribute__((ms_abi)) ULONG KeGetCurrentProcessorNumber_k(void);
__attribute__((ms_abi)) ULONG KeGetCurrentProcessorNumberEx_k(void* ProcNumber);

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// FASE 7.10: tracer global declarado cedo p/ todas as funcoes usarem.
volatile int g_pintok_trace = 0;

// --- Rtl ---
__attribute__((ms_abi)) static ULONG NT_DbgPrint(const char* Format, ...) {
    kputs("  [DbgPrint] ");
    kputs(Format);
    // o formato real do printf nao e implementado; so imprime a string base.
    return 0;
}
__attribute__((ms_abi)) static ULONG NT_DbgPrintEx(ULONG ComponentId, ULONG Level, const char* Format, ...) {
    (void)ComponentId; (void)Level;
    kputs("  [DbgPrintEx] "); kputs(Format);
    return 0;
}
__attribute__((ms_abi)) static NTSTATUS NT_DbgPrompt(const char* Prompt, char* Response, ULONG Length) {
    (void)Prompt; (void)Response; (void)Length;
    return STATUS_DEBUGGER_INACTIVE;
}
__attribute__((ms_abi)) static NTSTATUS NT_DbgQueryDebugFilterState(ULONG ComponentId, ULONG Level) {
    (void)ComponentId; (void)Level; return 0;   // FALSE -> filtros ativos
}
__attribute__((ms_abi)) static void NT_KeBugCheckEx(ULONG Code, uint64_t A, uint64_t B, uint64_t C, uint64_t D) {
    (void)A; (void)B; (void)C; (void)D;
    kputs("\n[**] KeBugCheckEx code="); kput_hex((uint64_t)Code); kputs(" — halting\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
__attribute__((ms_abi)) static void NT_KeBugCheck(ULONG Code) { NT_KeBugCheckEx(Code, 0, 0, 0, 0); }

__attribute__((ms_abi)) static void NT_RtlInitUnicodeString(PUNICODE_STRING dst, const WCHAR* src) {
    if (g_pintok_trace) {
        kputs("  [trace] RtlInitUnicodeString src='");
        if (src) for (int i = 0; i < 64 && src[i]; i++) kputc((char)src[i]);
        kputs("'\n");
    }
    USHORT n = 0;
    if (src) while (src[n]) n++;
    if (dst) {
        dst->Buffer = (WCHAR*)src;
        dst->Length = (USHORT)(n * 2);
        dst->MaximumLength = (USHORT)((n + 1) * 2);
    }
}
__attribute__((ms_abi)) static void NT_RtlInitAnsiString(PANSI_STRING dst, const char* src) {
    USHORT n = 0;
    if (src) while (src[n]) n++;
    if (dst) {
        dst->Buffer = (char*)src;
        dst->Length = (USHORT)n;
        dst->MaximumLength = (USHORT)(n + 1);
    }
}
__attribute__((ms_abi)) static NTSTATUS NT_RtlUnicodeStringToAnsiString(PANSI_STRING dst, PUNICODE_STRING src, BOOLEAN Alloc) {
    (void)Alloc;
    if (!dst || !src) return STATUS_INVALID_PARAMETER;
    USHORT n = (USHORT)(src->Length / 2);
    USHORT cap = dst->MaximumLength ? dst->MaximumLength : 0;
    if (cap < n) return STATUS_BUFFER_TOO_SMALL;
    for (USHORT i = 0; i < n; i++) dst->Buffer[i] = (char)(src->Buffer[i] & 0xFF);
    dst->Length = n;
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_RtlAnsiStringToUnicodeString(PUNICODE_STRING dst, PANSI_STRING src, BOOLEAN Alloc) {
    (void)Alloc;
    if (!dst || !src) return STATUS_INVALID_PARAMETER;
    USHORT need = (USHORT)(src->Length * 2);
    if (dst->MaximumLength < need) return STATUS_BUFFER_TOO_SMALL;
    for (USHORT i = 0; i < src->Length; i++) dst->Buffer[i] = (WCHAR)(uint8_t)src->Buffer[i];
    dst->Length = need;
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static LONG NT_RtlCompareUnicodeString(PUNICODE_STRING a, PUNICODE_STRING b, BOOLEAN CaseInSensitive) {
    USHORT na = a ? a->Length / 2 : 0;
    USHORT nb = b ? b->Length / 2 : 0;
    USHORT n = na < nb ? na : nb;
    for (USHORT i = 0; i < n; i++) {
        WCHAR x = a->Buffer[i], y = b->Buffer[i];
        if (CaseInSensitive) {
            if (x >= 'A' && x <= 'Z') x = (WCHAR)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (WCHAR)(y + 32);
        }
        if (x != y) return (LONG)x - (LONG)y;
    }
    return (LONG)na - (LONG)nb;
}
__attribute__((ms_abi)) static BOOLEAN NT_RtlEqualUnicodeString(PUNICODE_STRING a, PUNICODE_STRING b, BOOLEAN CI) {
    return NT_RtlCompareUnicodeString(a, b, CI) == 0 ? 1 : 0;
}
__attribute__((ms_abi)) static void NT_RtlCopyUnicodeString(PUNICODE_STRING dst, PUNICODE_STRING src) {
    if (!dst || !src) return;
    USHORT n = src->Length;
    if (n > dst->MaximumLength) n = dst->MaximumLength;
    for (USHORT i = 0; i < n / 2; i++) dst->Buffer[i] = src->Buffer[i];
    dst->Length = n;
}
__attribute__((ms_abi)) static void NT_RtlFreeUnicodeString(PUNICODE_STRING s) { (void)s; }
__attribute__((ms_abi)) static void NT_RtlFreeAnsiString(PANSI_STRING s) { (void)s; }
__attribute__((ms_abi)) static SIZE_T NT_RtlCompareMemory(const void* s1, const void* s2, SIZE_T len) {
    const uint8_t* a = (const uint8_t*)s1; const uint8_t* b = (const uint8_t*)s2;
    SIZE_T i; for (i = 0; i < len; i++) if (a[i] != b[i]) return i;
    return len;
}
__attribute__((ms_abi)) static void NT_RtlCopyMemory(void* d, const void* s, SIZE_T n) {
    uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s;
    for (SIZE_T i = 0; i < n; i++) a[i] = b[i];
}
__attribute__((ms_abi)) static void NT_RtlMoveMemory(void* d, const void* s, SIZE_T n) {
    uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s;
    if (a < b) { for (SIZE_T i = 0; i < n; i++) a[i] = b[i]; }
    else { for (SIZE_T i = n; i-- > 0;) a[i] = b[i]; }
}
__attribute__((ms_abi)) static void NT_RtlZeroMemory(void* d, SIZE_T n) {
    uint8_t* p = (uint8_t*)d; for (SIZE_T i = 0; i < n; i++) p[i] = 0;
}
__attribute__((ms_abi)) static void NT_RtlFillMemory(void* d, SIZE_T n, UCHAR v) {
    uint8_t* p = (uint8_t*)d; for (SIZE_T i = 0; i < n; i++) p[i] = v;
}
__attribute__((ms_abi)) static NTSTATUS NT_RtlGetVersion(PVOID lpVersionInformation) {
    if (g_pintok_trace) kputs("  [trace] RtlGetVersion\n");
    if (!lpVersionInformation) return STATUS_INVALID_PARAMETER;
    // RTL_OSVERSIONINFOEXW (subset): dwOSVersionInfoSize, dwMajor, dwMinor, dwBuild, dwPlatformId, ...
    uint32_t* p = (uint32_t*)lpVersionInformation;
    p[1] = 10;       // Major
    p[2] = 0;        // Minor
    p[3] = 26100;    // Build
    p[4] = 2;        // PlatformId = VER_PLATFORM_WIN32_NT
    return STATUS_SUCCESS;
}

// --- Io ---
__attribute__((ms_abi)) static NTSTATUS NT_IoCreateDevice(
        PDRIVER_OBJECT drv, ULONG ext, PUNICODE_STRING name, ULONG type,
        ULONG chars, BOOLEAN excl, PDEVICE_OBJECT* out) {
    return IoCreateDevice_k(drv, ext, name, type, chars, excl, out);
}
__attribute__((ms_abi)) static void NT_IoDeleteDevice(PDEVICE_OBJECT dev) { IoDeleteDevice_k(dev); }

// PsLookupProcessByProcessId via systhread.c (forward).
extern NTSTATUS NTAPI PsLookupProcessByProcessId_k(HANDLE ProcessId, PVOID* Process);

// ===== Stubs genericos para APIs que so precisam compilar/nao falhar =====
__attribute__((ms_abi)) static NTSTATUS stub_success(void) { return STATUS_SUCCESS; }
__attribute__((ms_abi)) static void     stub_void(void)    { }
__attribute__((ms_abi)) static NTSTATUS stub_not_impl(void){ return STATUS_NOT_IMPLEMENTED; }
__attribute__((ms_abi)) static void*    stub_zero(void)    { return 0; }
__attribute__((ms_abi)) static BOOLEAN  stub_false(void)   { return 0; }

// CcMapData, CcUnpinData, etc. — stubs.
__attribute__((ms_abi)) static BOOLEAN NT_CcMapData(PVOID FileObj, PLARGE_INTEGER Off, ULONG Len, ULONG Flags,
                                                   PVOID* Bcb, PVOID* Buf) {
    (void)FileObj; (void)Off; (void)Len; (void)Flags; (void)Bcb; (void)Buf;
    return 0;
}

// Bcrypt / Ci stubs (sempre sucesso — caminho seguro p/ testes).
__attribute__((ms_abi)) static NTSTATUS NT_BCryptOpenAlgorithmProvider(PVOID* hAlg, const WCHAR* AlgId, const WCHAR* Impl, ULONG Flags) {
    (void)AlgId; (void)Impl; (void)Flags;
    if (hAlg) *hAlg = (PVOID)(uintptr_t)0xBCAA0001;
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_BCryptCreateHash(PVOID hAlg, PVOID* hHash, PVOID Buf, ULONG BufLen,
                                                            PVOID Secret, ULONG SecretLen, ULONG Flags) {
    (void)hAlg; (void)Buf; (void)BufLen; (void)Secret; (void)SecretLen; (void)Flags;
    if (hHash) *hHash = (PVOID)(uintptr_t)0xBCAA0002;
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_BCryptHashData(PVOID hHash, PVOID Data, ULONG Len, ULONG Flags) {
    (void)hHash; (void)Data; (void)Len; (void)Flags; return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_BCryptFinishHash(PVOID hHash, PVOID Out, ULONG Len, ULONG Flags) {
    (void)hHash; (void)Flags;
    if (Out && Len) { uint8_t* o = (uint8_t*)Out; for (ULONG i = 0; i < Len; i++) o[i] = (uint8_t)i; }
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_BCryptGetProperty(PVOID hObj, const WCHAR* Prop, PVOID Buf, ULONG BufLen, PULONG Result, ULONG Flags) {
    (void)hObj; (void)Prop; (void)Buf; (void)BufLen; (void)Flags;
    if (Result) *Result = 0;
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_BCryptCloseAlgorithmProvider(PVOID hAlg, ULONG Flags) { (void)hAlg; (void)Flags; return STATUS_SUCCESS; }
__attribute__((ms_abi)) static NTSTATUS NT_BCryptDestroyHash(PVOID hHash) { (void)hHash; return STATUS_SUCCESS; }
__attribute__((ms_abi)) static NTSTATUS NT_CiCheckSignedFile(PVOID a, PVOID b, ULONG c, PVOID d, PVOID e, PVOID f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return STATUS_SUCCESS;   // (stub: trata como bem assinado)
}
__attribute__((ms_abi)) static NTSTATUS NT_CiValidateImageHeader(PVOID a, PVOID b, PVOID c, ULONG d, PVOID e, PVOID f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return STATUS_SUCCESS;
}

// MmGetSystemRoutineAddress: drivers consultam pela API do ntoskrnl.
__attribute__((ms_abi)) static PVOID NT_MmGetSystemRoutineAddress(PUNICODE_STRING SystemRoutineName) {
    // Implementacao simples: converte p/ ASCII e consulta o resolver.
    if (!SystemRoutineName || !SystemRoutineName->Buffer) return 0;
    char buf[64]; int n = SystemRoutineName->Length / 2;
    if (n > 63) n = 63;
    for (int i = 0; i < n; i++) buf[i] = (char)SystemRoutineName->Buffer[i];
    buf[n] = 0;
    PVOID r = ntkrnl_resolve("ntoskrnl.exe", buf);
    if (g_pintok_trace) {
        kputs("  [trace] MmGetSystemRoutineAddress('");
        kputs(buf); kputs(r ? "') -> OK\n" : "') -> NULL (nao existe)\n");
    }
    return r;
}

// ============================================================================
//  GATE 2 do pintok.sys (pintok.sys) — SystemModuleInformation / ...Ex.
//
//  Layout de RTL_PROCESS_MODULE_INFORMATION_EX (uma entrada = 0x140 bytes), tal
//  como o pintok.sys consome (confirmado no emulador unicorn, build_module_list_ex):
//
//    +0x00  USHORT NextOffset            (0x140 p/ todas menos a ultima = 0)
//    +0x08  RTL_PROCESS_MODULE_INFORMATION BaseInfo {
//    +0x08    PVOID  Section
//    +0x10    PVOID  MappedBase
//    +0x18    PVOID  ImageBase           <-- base de carga (VA)
//    +0x20    ULONG  ImageSize           <-- SizeOfImage
//    +0x24    ULONG  Flags
//    +0x28    USHORT LoadOrderIndex
//    +0x2A    USHORT InitOrderIndex
//    +0x2C    USHORT LoadCount
//    +0x2E    USHORT OffsetToFileName    <-- byte offset do basename dentro de FullPathName
//    +0x30    UCHAR  FullPathName[256]   <-- "\\SystemRoot\\system32\\<...>" ANSI
//    }
//    +0x138 ULONG ImageChecksum / +0x13C ULONG TimeDateStamp (zeramos)
//
//  O cabecalho do buffer (RTL_PROCESS_MODULES / ...Ex) e:
//    +0x00 ULONG NumberOfModules; depois o array de entradas.
//  Confirmado pelo padrao de 2 chamadas: buffer NULL -> STATUS_INFO_LENGTH_MISMATCH
//  + *ReturnLength = tamanho total; depois buffer dimensionado -> preenche.
// ============================================================================
#define VG_MODENTRY_SIZE   0x140u
#define VG_MOD_HDR_SIZE    0x10u     /* ULONG NumberOfModules + padding p/ alinhar entradas */

// Prefixo de caminho NT que o pintok.sys espera ver ("\SystemRoot\system32\..."). O
// basename real do modulo (ex.: "pintok.sys") e anexado; OffsetToFileName aponta
// pra ele. Drivers .sys ficam em ...\drivers\, mas o pintok.sys so usa o basename
// (OffsetToFileName) para comparar nomes, entao um prefixo unico basta.
static void vg_build_module_path(char* dst /*>=256*/, const char* basename, USHORT* off_out) {
    static const char* pfx = "\\SystemRoot\\system32\\";
    int i = 0;
    while (pfx[i] && i < 200) { dst[i] = pfx[i]; i++; }
    USHORT ofn = (USHORT)i;
    int j = 0;
    while (basename && basename[j] && i < 255) { dst[i++] = basename[j++]; }
    dst[i] = 0;
    if (off_out) *off_out = ofn;
}

// Monta o blob completo (header + N entradas) em 'out' (cap = outcap bytes).
// Retorna o tamanho total necessario (independe de outcap; se outcap for menor,
// nao escreve — usado p/ a 1a chamada de dimensionamento). 'ntoskrnl_first'
// garante que a entrada 0 seja o ntoskrnl (o pintok.sys assume modulo[0] = kernel).
static ULONG vg_build_module_list(uint8_t* out, ULONG outcap) {
    int n = ldr_get_module_count();
    if (n <= 0) n = 0;
    ULONG total = VG_MOD_HDR_SIZE + (ULONG)n * VG_MODENTRY_SIZE;
    if (!out || outcap < total) return total;

    // zera tudo primeiro (campos nao usados ficam 0).
    for (ULONG z = 0; z < total; z++) out[z] = 0;
    *(ULONG*)(out + 0) = (ULONG)n;                       // NumberOfModules

    uint8_t* e = out + VG_MOD_HDR_SIZE;
    for (int i = 0; i < n; i++, e += VG_MODENTRY_SIZE) {
        uint64_t base = 0; uint32_t size = 0; const char* name = 0;
        ldr_get_module_info(i, &base, &size, &name);
        // tamanho realista: o pintok.sys valida que as funcoes resolvidas caem DENTRO
        // de [ImageBase, ImageBase+ImageSize). Se o PE nao deu SizeOfImage,
        // usa um default generoso.
        if (size == 0) size = 0x300000u;

        *(uint16_t*)(e + 0x00) = (uint16_t)((i < n - 1) ? VG_MODENTRY_SIZE : 0);  // NextOffset
        *(uint64_t*)(e + 0x18) = base;                   // ImageBase
        *(uint32_t*)(e + 0x20) = size;                   // ImageSize
        *(uint16_t*)(e + 0x28) = (uint16_t)i;            // LoadOrderIndex
        *(uint16_t*)(e + 0x2C) = 1;                      // LoadCount
        USHORT ofn = 0;
        vg_build_module_path((char*)(e + 0x30), name ? name : "unknown.sys", &ofn);
        *(uint16_t*)(e + 0x2E) = ofn;                    // OffsetToFileName
    }
    return total;
}

// ZwQuerySystemInformation: drivers usam pra checar VBS/HVCI/SecureBoot/etc.
__attribute__((ms_abi)) static NTSTATUS NT_ZwQuerySystemInformation(ULONG cls, PVOID buf, ULONG buflen, PULONG ret) {
    if (g_pintok_trace) {
        kputs("  [trace] ZwQuerySystemInformation(class=0x"); kput_hex(cls);
        kputs(" buflen="); kput_hex(buflen); kputs(")\n");
    }
    // GATE 2 do pintok.sys: enumeracao de modulos carregados (class 11 e 0x4D=77).
    // Padrao de 2 chamadas: buffer NULL/pequeno -> STATUS_INFO_LENGTH_MISMATCH +
    // *ReturnLength = tamanho; depois buffer dimensionado -> preenche o array.
    if (cls == SystemModuleInformation || cls == SystemModuleInformationEx) {
        ULONG need = vg_build_module_list(0, 0);         // so dimensiona
        if (ret) *ret = need;
        if (!buf || buflen < need) {
            if (g_pintok_trace) { kputs("    [QSI modlist] need=0x"); kput_hex(need); kputs(" (INFO_LENGTH_MISMATCH)\n"); }
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        ULONG wrote = vg_build_module_list((uint8_t*)buf, buflen);
        if (ret) *ret = wrote;
        if (g_pintok_trace) {
            kputs("    [QSI modlist] mods="); kput_dec(ldr_get_module_count());
            kputs(" wrote=0x"); kput_hex(wrote); kputs("\n");
        }
        return STATUS_SUCCESS;
    }
    // Implementacao MINIMA p/ classes mais consultadas:
    if (cls == SystemBasicInformation) {
        ULONG need = 48;
        if (ret) *ret = need;
        if (!buf || buflen < need) return STATUS_INFO_LENGTH_MISMATCH;
        uint32_t* p = (uint32_t*)buf;
        for (ULONG i = 0; i < need / 4; i++) p[i] = 0;
        p[2] = 4096;   // PageSize
        p[6] = 4096;   // AllocationGranularity
        p[10] = 1;     // NumberOfProcessors
        return STATUS_SUCCESS;
    }
    if (cls == SystemCodeIntegrityInformation) {
        // 8 bytes: Length(4) + CodeIntegrityOptions(4). HVCI=0 (caminho seguro).
        ULONG need = 8;
        if (ret) *ret = need;
        if (!buf || buflen < need) return STATUS_INFO_LENGTH_MISMATCH;
        ((uint32_t*)buf)[0] = need; ((uint32_t*)buf)[1] = 0;
        return STATUS_SUCCESS;
    }
    if (cls == SystemSecureBootPolicyInformation || cls == SystemVsmProtectionInformation) {
        if (ret) *ret = 16;
        if (!buf || buflen < 16) return STATUS_INFO_LENGTH_MISMATCH;
        for (int i = 0; i < 16; i++) ((uint8_t*)buf)[i] = 0;
        return STATUS_SUCCESS;
    }
    if (cls == SystemFirmwareTableInformation) {
        if (ret) *ret = 16;
        if (!buf || buflen < 16) return STATUS_INFO_LENGTH_MISMATCH;
        for (int i = 0; i < 16; i++) ((uint8_t*)buf)[i] = 0;
        return STATUS_SUCCESS;
    }
    if (ret) *ret = 0;
    return STATUS_INVALID_INFO_CLASS;
}
__attribute__((ms_abi)) static NTSTATUS NT_NtQuerySystemInformation_k(ULONG cls, PVOID buf, ULONG buflen, PULONG ret) {
    return NT_ZwQuerySystemInformation(cls, buf, buflen, ret);
}

// Process/thread inspection — encaminham para systhread.c.
extern PVOID NTAPI PsGetCurrentThreadId_k(void);
extern PVOID NTAPI PsGetCurrentProcessId_k(void);
extern PVOID NTAPI PsGetCurrentThread_k(void);
extern PVOID NTAPI PsGetCurrentProcess_k(void);
extern PVOID NTAPI PsGetProcessId_k(PVOID Process);
extern const char* NTAPI PsGetProcessImageFileName_k(PVOID Process);
extern PVOID NTAPI PsGetProcessPeb_k(PVOID Process);
extern BOOLEAN NTAPI PsIsProtectedProcess_k(PVOID Process);
extern BOOLEAN NTAPI PsIsProtectedProcessLight_k(PVOID Process);

// Exemplo: ExSystemTimeToLocalTime (no-op identidade).
__attribute__((ms_abi)) static void NT_ExSystemTimeToLocalTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER LocalTime) {
    if (LocalTime && SystemTime) LocalTime->QuadPart = SystemTime->QuadPart;
}
__attribute__((ms_abi)) static void NT_KeBugCheckExWrapper(ULONG code) { NT_KeBugCheckEx(code, 0, 0, 0, 0); }

// PsCreateSystemThread reference (signature ja em systhread.h).
extern NTSTATUS NTAPI PsCreateSystemThread_k(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PCLIENT_ID, PKSTART_ROUTINE, PVOID);
extern NTSTATUS NTAPI PsTerminateSystemThread_k(NTSTATUS);

// ZwClose -> NtClose (sem distincao kernel/user no MeuOS).
__attribute__((ms_abi)) static NTSTATUS NT_ZwClose(HANDLE h) { ob_close_handle(h); return STATUS_SUCCESS; }

// ============================================================================
//  FASE 7.1 — APIs/variaveis extras que drivers reais como pintok.sys importam.
//  Identificamos via "[ntex] stub generico p/" — esta passada adiciona as 50+
//  mais comuns. Variaveis exportadas precisam ser EXPORTADAS como ENDERECO, nao
//  como funcao stub (sob pena de Page Fault quando o driver dereferencia).
// ============================================================================

// ---- Variaveis globais exportadas ----
static ULONG s_KeNumberProcessors = 1;
static PVOID s_MmHighestUserAddress = (PVOID)0x000007FFFFFEFFFFULL;
static PVOID s_MmSystemRangeStart   = (PVOID)0xFFFF080000000000ULL;
static ULONG s_NtBuildNumber = 26100;
static ULONG s_InitSafeBootMode = 0;

// ---- C runtime: implementacoes reais ----
__attribute__((ms_abi)) static SIZE_T NT_strlen(const char* s) {
    SIZE_T n = 0; if (s) while (s[n]) n++; return n;
}
__attribute__((ms_abi)) static int NT_strcmp(const char* a, const char* b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
__attribute__((ms_abi)) static int NT_strncmp(const char* a, const char* b, SIZE_T n) {
    if (!a || !b || n == 0) return 0;
    while (n-- && *a && *a == *b) { a++; b++; }
    return n == (SIZE_T)-1 ? 0 : (int)(uint8_t)*a - (int)(uint8_t)*b;
}
__attribute__((ms_abi)) static char* NT_strchr(const char* s, int c) {
    if (!s) return 0;
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (char)c == 0 ? (char*)s : 0;
}
__attribute__((ms_abi)) static SIZE_T NT_wcslen(const WCHAR* s) {
    SIZE_T n = 0; if (s) while (s[n]) n++; return n;
}
__attribute__((ms_abi)) static int NT_wcscmp(const WCHAR* a, const WCHAR* b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}
__attribute__((ms_abi)) static SIZE_T NT_strnlen(const char* s, SIZE_T m) {
    SIZE_T n = 0; if (!s) return 0;
    while (n < m && s[n]) n++;
    return n;
}
__attribute__((ms_abi)) static int NT_strncpy_s(char* dst, SIZE_T dstSize, const char* src, SIZE_T count) {
    if (!dst || dstSize == 0) return STATUS_INVALID_PARAMETER;
    SIZE_T i = 0;
    if (src) while (i < count && i < dstSize - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return STATUS_SUCCESS;
}
// ---- printf REAL (wide + narrow) -----------------------------------------
// O stub vazio antigo quebrava TODA string que o driver monta com swprintf_s
// (nome de arquivo de log, mensagens, paths) -> divergencia que fazia o pintok.sys
// bailar cedo. Implementacao real: %d %i %u %x %X %p %s %S %c %% com width,
// zero-pad, flag '-' e modificadores h/l/ll. [fidelidade — implementacao real]
#include <stdarg.h>
typedef __builtin_ms_va_list ms_va_list;   // va_list estilo Win64 (driver chama com ms_abi)
__attribute__((ms_abi)) static int vfmt_core(void* out, SIZE_T cap, const void* fmt, ms_va_list ap, int wide) {
    SIZE_T oi = 0, fi = 0;
#define VF_PUT(ch) do { if (cap && oi + 1 < cap) { if (wide) ((uint16_t*)out)[oi]=(uint16_t)(ch); else ((char*)out)[oi]=(char)(ch); } oi++; } while(0)
    for (;;) {
        uint32_t c = wide ? ((const uint16_t*)fmt)[fi] : (uint8_t)((const char*)fmt)[fi];
        if (!c) break;
        fi++;
        if (c != '%') { VF_PUT(c); continue; }
        int zero = 0, left = 0, width = 0, lenmod = 0;   // lenmod: 1=h 2=l 3=ll
        for (;;) { uint32_t f = wide ? ((const uint16_t*)fmt)[fi] : (uint8_t)((const char*)fmt)[fi];
                   if (f=='0'){zero=1;fi++;} else if (f=='-'){left=1;fi++;} else break; }
        for (;;) { uint32_t d = wide ? ((const uint16_t*)fmt)[fi] : (uint8_t)((const char*)fmt)[fi];
                   if (d>='0'&&d<='9'){width=width*10+(int)(d-'0');fi++;} else break; }
        for (;;) { uint32_t m = wide ? ((const uint16_t*)fmt)[fi] : (uint8_t)((const char*)fmt)[fi];
                   if (m=='h'){lenmod=1;fi++;} else if (m=='l'){lenmod=(lenmod==2)?3:2;fi++;}
                   else if (m=='w'){fi++;} else break; }
        uint32_t cv = wide ? ((const uint16_t*)fmt)[fi] : (uint8_t)((const char*)fmt)[fi];
        if (cv) fi++;
        int isneg = 0, base = 10, upper = 0; uint64_t uv = 0;
        switch (cv) {
            case 'd': case 'i': { int64_t v = (lenmod>=3)?va_arg(ap,int64_t):(int64_t)va_arg(ap,int32_t);
                if (lenmod==1) v=(int16_t)v; if (v<0){isneg=1;uv=(uint64_t)(-v);} else uv=(uint64_t)v; break; }
            case 'u': { uint64_t v=(lenmod>=3)?va_arg(ap,uint64_t):(uint64_t)va_arg(ap,uint32_t);
                if (lenmod==1) v=(uint16_t)v; uv=v; break; }
            case 'x': case 'X': { uint64_t v=(lenmod>=3)?va_arg(ap,uint64_t):(uint64_t)va_arg(ap,uint32_t);
                if (lenmod==1) v=(uint16_t)v; uv=v; base=16; upper=(cv=='X'); break; }
            case 'p': { uv=(uint64_t)va_arg(ap,void*); base=16; width=16; zero=1; break; }
            case 'c': { VF_PUT(va_arg(ap,uint32_t)); continue; }
            case 's': case 'S': {
                int argwide = (cv=='S') ? !wide : wide;
                const void* sp = va_arg(ap, void*);
                if (!sp) { const char* nn="(null)"; for (int k=0;nn[k];k++) VF_PUT(nn[k]); continue; }
                for (SIZE_T si=0;;si++) { uint32_t sc = argwide ? ((const uint16_t*)sp)[si] : (uint8_t)((const char*)sp)[si];
                    if (!sc) break; VF_PUT(sc); } continue; }
            case '%': VF_PUT('%'); continue;
            case 0: goto vf_done;
            default: VF_PUT('%'); VF_PUT(cv); continue;
        }
        const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        char rev[24]; int rn = 0;
        if (!uv) rev[rn++]='0'; else while (uv){ rev[rn++]=digs[uv%base]; uv/=base; }
        int pad = width - rn - (isneg?1:0); if (pad<0) pad=0;
        if (!left && !zero) while (pad-->0) VF_PUT(' ');
        if (isneg) VF_PUT('-');
        if (!left && zero) while (pad-->0) VF_PUT('0');
        while (rn>0) VF_PUT(rev[--rn]);
        if (left) while (pad-->0) VF_PUT(' ');
    }
vf_done:
    if (cap) { SIZE_T t = oi < cap ? oi : cap-1; if (wide) ((uint16_t*)out)[t]=0; else ((char*)out)[t]=0; }
#undef VF_PUT
    // Dump do PLAINTEXT formatado (antes do pintok.sys cifrar p/ o ZwWriteFile) — revela os
    // checkpoints/IDs numericos e a versao que o pintok.sys loga. O ULTIMO antes do bail = estagio da falha.
    if (g_pintok_trace && out) {
        kputs("  [pintok.syslog] '");
        SIZE_T m = oi < 180 ? oi : 180;
        for (SIZE_T k = 0; k < m; k++) { char c = wide ? (char)((uint16_t*)out)[k] : ((char*)out)[k];
                                         kputc((c >= 32 && c < 127) ? c : '.'); }
        kputs("'\n");
    }
    return (int)oi;
}
// Wide (swprintf_s / _snwprintf): buf, count, fmt, ...
__attribute__((ms_abi)) static int NT_swprintf_s(uint16_t* b, SIZE_T cnt, const uint16_t* fmt, ...) {
    ms_va_list ap; __builtin_ms_va_start(ap, fmt); int r = vfmt_core(b, cnt, fmt, ap, 1); __builtin_ms_va_end(ap); return r;
}
__attribute__((ms_abi)) static int NT_vswprintf_s(uint16_t* b, SIZE_T cnt, const uint16_t* fmt, ms_va_list ap) {
    return vfmt_core(b, cnt, fmt, ap, 1);
}
// Narrow (_snprintf_s): buf, size, count, fmt, ...   (count ignorado; size limita)
__attribute__((ms_abi)) static int NT_snprintf_s(char* buf, SIZE_T sz, SIZE_T cnt, const char* fmt, ...) {
    (void)cnt; ms_va_list ap; __builtin_ms_va_start(ap, fmt); int r = vfmt_core(buf, sz, fmt, ap, 0); __builtin_ms_va_end(ap); return r;
}
__attribute__((ms_abi)) static int NT_vsprintf_s(char* buf, SIZE_T sz, const char* fmt, ms_va_list ap) {
    return vfmt_core(buf, sz, fmt, ap, 0);
}

// ---- Ke* faltantes (no-ops seguros) ----
// NT_KeInitializeDpc removido — KeInitializeDpc agora e real (KeInitializeDpc_k em ke/dpc.c, Item 2).
// NT_KeInitializeTimer/NT_KeSetTimer/NT_KeCancelTimer removidos — KTIMER agora
// e real (ke/timer.c, Item 6, flag-gated: no modo legado volta ao antigo).
__attribute__((ms_abi)) static void NT_KeInitializeApc(PVOID Apc, PVOID Thread, KPROCESSOR_MODE Env, PVOID K, PVOID R, PVOID N, KPROCESSOR_MODE M, PVOID Ctx) {
    (void)Thread; (void)Env; (void)K; (void)R; (void)N; (void)M; (void)Ctx;
    if (Apc) { for (int i = 0; i < 80; i++) ((uint8_t*)Apc)[i] = 0; }
}
__attribute__((ms_abi)) static BOOLEAN NT_KeInsertQueueApc(PVOID Apc, PVOID Arg1, PVOID Arg2, KPRIORITY Inc) {
    (void)Apc; (void)Arg1; (void)Arg2; (void)Inc; return 1;
}
__attribute__((ms_abi)) static void NT_KeInitializeMutant(PVOID Mutant, BOOLEAN Init) {
    (void)Init;
    if (Mutant) { for (int i = 0; i < 64; i++) ((uint8_t*)Mutant)[i] = 0; }
}
__attribute__((ms_abi)) static BOOLEAN NT_KeAreAllApcsDisabled(void) { return 0; }
// REAL KeIpiGenericCall(BroadcastFunction, Context) roda o worker em TODOS os cores e
// retorna o valor dele. Single-CPU: roda sincronamente 1x. pintok.sys passa o worker do setup/
// timing por-core (Context=0) e espera que ele rode + retorne. No-op deixava o buffer
// por-CPU sem montar -> bail downstream. [GATE 3 da init do pintok.sys]
__attribute__((ms_abi)) static uintptr_t NT_KeIpiGenericCall(PVOID Routine, uintptr_t Ctx) {
    if (g_pintok_trace) { kputs("  [trace] KeIpiGenericCall Routine=0x"); kput_hex((uint64_t)Routine); kputs(" Ctx=0x"); kput_hex(Ctx); kputs("\n"); }
    uintptr_t rv = Routine ? ((__attribute__((ms_abi)) uintptr_t(*)(uintptr_t))Routine)(Ctx) : 0;
    if (g_pintok_trace) { kputs("  [trace] KeIpiGenericCall worker retornou=0x"); kput_hex(rv); kputs("\n"); }
    return rv;
}
// NT_KfRaiseIrql removido — KfRaiseIrql agora e real (KfRaiseIrql_k em ke/sync.c, Item 1).
// REAL KeRegisterBugCheckReasonCallback retorna BOOLEAN (TRUE se registrou), nao NTSTATUS.
// pintok.sys le esse BOOLEAN e bail com STATUS_NOT_FOUND se for FALSE — STATUS_SUCCESS
// (=0) era lido como FALSE. Retornar TRUE (1). [GATE 1 da init do pintok.sys; ver pintok.sys-INIT-NOTES.md]
__attribute__((ms_abi)) static BOOLEAN NT_KeRegisterBugCheckReasonCallback(PVOID Record, PVOID Cb, ULONG Reason, PVOID Component) {
    (void)Record; (void)Cb; (void)Reason; (void)Component; return 1;
}
__attribute__((ms_abi)) static BOOLEAN NT_KeDeregisterBugCheckReasonCallback(PVOID Record) { (void)Record; return 1; }
__attribute__((ms_abi)) static void NT_KeQuerySystemTimePrecise(PLARGE_INTEGER t) {
    KeQuerySystemTime_k(t);
}

// ---- Mm* / Io* / Ob* faltantes ----
// FASE 7.9 (SEH minimo) — ProbeForRead/Write REAIS: validacao basica de
// alinhamento + range, com log para invalidos. Implementadas em src/nt/seh.c
// para nao inflar este arquivo.
__attribute__((ms_abi)) extern void NT_ProbeForRead_real(const void* Addr, SIZE_T Len, ULONG Align);
__attribute__((ms_abi)) extern void NT_ProbeForWrite_real(void* Addr, SIZE_T Len, ULONG Align);
__attribute__((ms_abi)) static PVOID NT_IoGetCurrentProcess(void) {
    return (PVOID)PsGetCurrentProcess();
}
__attribute__((ms_abi)) static NTSTATUS NT_ObReferenceObjectByHandle(HANDLE h, ACCESS_MASK acc, PVOID Type, KPROCESSOR_MODE Mode, PVOID* Out, PVOID InfoOut) {
    (void)acc; (void)Type; (void)Mode; (void)InfoOut;
    if (Out) *Out = ob_handle_to_object(h, /*OB_TYPE_FILE*/4);
    NTSTATUS s = Out && *Out ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
    if (g_pintok_trace) {
        kputs("  [trace] ObReferenceObjectByHandle h=");
        kput_hex((uint64_t)(uintptr_t)h);
        kputs(s == STATUS_SUCCESS ? " -> OK\n" : " -> INVALID_HANDLE\n");
    }
    return s;
}
__attribute__((ms_abi)) static void NT_ObfDereferenceObject(PVOID Obj) {
    if (Obj) ObDereferenceObject(Obj);
}
// ============================================================================
//  FASE 7.6 — MDL real. As implementacoes ficam em src/mm/mdl.c.
//  Aqui apenas redirecionamos com a ABI ms_abi exigida pelos drivers.
//
//  MmAllocatePagesForMdl: aloca um buffer de pool e descreve via IoAllocateMdl,
//  ja com MDL_SOURCE_IS_NONPAGED_POOL/MDL_MAPPED_TO_SYSTEM_VA (graças a
//  MmBuildMdlForNonPagedPool). Drivers depois usam MmGetSystemAddressForMdlSafe
//  ou MmMapLockedPages para obter o endereco.
// ============================================================================
__attribute__((ms_abi)) static PMDL NT_MmAllocatePagesForMdl(LARGE_INTEGER Low, LARGE_INTEGER High, LARGE_INTEGER Skip, SIZE_T Bytes) {
    (void)Low; (void)High; (void)Skip;
    if (Bytes == 0) return 0;
    void* buf = ExAllocatePoolWithTag_k(NonPagedPool, Bytes, 0x4C444D50u);  // 'PMDL'
    if (!buf) return 0;
    PMDL Mdl = IoAllocateMdl_k(buf, (ULONG)Bytes, 0, 0, 0);
    if (!Mdl) { ExFreePoolWithTag_k(buf, 0x4C444D50u); return 0; }
    MmBuildMdlForNonPagedPool_k(Mdl);
    return Mdl;
}
__attribute__((ms_abi)) static void NT_MmFreePagesFromMdl(PMDL Mdl) {
    if (!Mdl) return;
    // O buffer apontado por StartVa+ByteOffset foi alocado por NT_MmAllocatePagesForMdl;
    // soltamos esse pool primeiro, depois a propria MDL via IoFreeMdl_k.
    PVOID buf = (PVOID)((uintptr_t)Mdl->StartVa + Mdl->ByteOffset);
    if (buf) ExFreePoolWithTag_k(buf, 0x4C444D50u);
    IoFreeMdl_k(Mdl);
}
__attribute__((ms_abi)) static PVOID NT_IoAllocateWorkItem(PDEVICE_OBJECT dev) {
    (void)dev;
    return ExAllocatePoolWithTag_k(NonPagedPool, 64, 0x49574B49u);   // 'IWKI'
}
__attribute__((ms_abi)) static void NT_IoFreeWorkItem(PVOID WI) { if (WI) ExFreePoolWithTag_k(WI, 0); }
__attribute__((ms_abi)) static void NT_IoInitializeWorkItem(PDEVICE_OBJECT dev, PVOID WI) { (void)dev; (void)WI; }
__attribute__((ms_abi)) static void NT_IoQueueWorkItem(PVOID WI, PVOID Routine, ULONG Type, PVOID Ctx) {
    (void)WI; (void)Type;
    if (Routine) ((void(NTAPI*)(PVOID,PVOID))Routine)(0, Ctx);    // executa inline (sem scheduler)
}
// pintok.sys abre um arquivo de LOG via IoCreateFileEx no inicio da DriverEntry e bail
// (0xC0000365 STATUS_FAILED_DRIVER_ENTRY) se nao conseguir. ZwWriteFile ja descarta com
// SUCCESS, entao basta um handle fake + IOSB de sucesso pra o logging "funcionar". [GATE 1.5 do pintok.sys]
__attribute__((ms_abi)) static NTSTATUS NT_IoCreateFileEx(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io, PLARGE_INTEGER sz, ULONG fa, ULONG sh, ULONG d, ULONG co, PVOID ea, ULONG ealen, ULONG ck, PVOID ctx) {
    (void)a; (void)sz; (void)fa; (void)sh; (void)d; (void)co; (void)ea; (void)ealen; (void)ck; (void)ctx;
    static uintptr_t s_iocf = 0x4000;
    uintptr_t hh = (s_iocf += 4);                                // handle distinto por open (evita colisao)
    if (g_pintok_trace) {
        kputs("  [trace] IoCreateFileEx");
        if (oa) { PUNICODE_STRING nm = *(PUNICODE_STRING*)((uint8_t*)oa + 0x10);
                  if (nm && nm->Buffer && nm->Length) { kputs(" '");
                      for (int k = 0; k < nm->Length/2 && k < 160; k++) kputc((char)nm->Buffer[k]); kputc('\''); } }
        kputs(" -> SUCCESS h=0x"); kput_hex(hh); kputs("\n");
    }
    if (h) *h = (HANDLE)hh;
    if (io) { io->Status = STATUS_SUCCESS; io->Information = 1; } // FILE_OPENED
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_ZwReadFile_stub(HANDLE h, HANDLE e, PVOID rt, PVOID c, PIO_STATUS_BLOCK io, PVOID buf, ULONG len, PLARGE_INTEGER off, PULONG key) {
    (void)e; (void)rt; (void)c; (void)off; (void)key;
    // Retorna 'len' bytes ZERADOS com SUCESSO (em vez de EOF). pintok.sys le 1 byte do
    // pintok.sysbootstatus.dat (o status do boot anterior); EOF era lido como erro -> bail
    // 0xC0000365. Byte 0 = status "limpo" (boot anterior OK) -> pintok.sys prossegue. [fidelidade]
    if (g_pintok_trace) { kputs("  [trace] ZwReadFile h=0x"); kput_hex((uint64_t)h); kputs(" len="); kput_dec(len); kputs(" -> SUCCESS(zeros)\n"); }
    if (buf && len) for (ULONG i = 0; i < len; i++) ((uint8_t*)buf)[i] = 0;
    if (io) { io->Status = STATUS_SUCCESS; io->Information = len; }
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_ZwWriteFile_stub(HANDLE h, HANDLE e, PVOID rt, PVOID c, PIO_STATUS_BLOCK io, PVOID buf, ULONG len, PLARGE_INTEGER off, PULONG key) {
    (void)e; (void)rt; (void)c; (void)off; (void)key;
    // Dump do CONTEUDO do log do pintok.sys — e o proprio diagnostico dele (porque vai falhar).
    if (g_pintok_trace) {
        kputs("  [LOG] '");
        if (buf && len) {
            ULONG n = len < 240 ? len : 240;
            int wide = (len >= 2 && ((uint8_t*)buf)[1] == 0 && ((uint8_t*)buf)[0] != 0);
            if (wide) { for (ULONG k = 0; k < n/2; k++) { char ch = (char)((uint16_t*)buf)[k]; kputc((ch>=32&&ch<127)?ch:'.'); } }
            else      { for (ULONG k = 0; k < n;   k++) { char ch = ((char*)buf)[k];            kputc((ch>=32&&ch<127)?ch:'.'); } }
        }
        kputs("'\n");
    }
    if (io) { io->Status = STATUS_SUCCESS; io->Information = len; }
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_ZwTerminateProcess(HANDLE h, NTSTATUS exit) { (void)h; (void)exit; return STATUS_SUCCESS; }
__attribute__((ms_abi)) static NTSTATUS NT_ZwQueryDirectoryObject(HANDLE d, PVOID b, ULONG l, BOOLEAN s, BOOLEAN r, PULONG ctx, PULONG ret) {
    (void)d; (void)b; (void)l; (void)s; (void)r; (void)ctx;
    if (ret) *ret = 0;
    return STATUS_NO_MORE_ENTRIES;
}
__attribute__((ms_abi)) static NTSTATUS NT_ZwOpenDirectoryObject(PHANDLE h, ACCESS_MASK acc, POBJECT_ATTRIBUTES oa) {
    (void)acc; (void)oa;
    if (h) *h = (HANDLE)0xD180D170;
    return STATUS_SUCCESS;
}
// RtlTimeToTimeFields REAL: 100ns desde 1601-01-01 -> TIME_FIELDS{Year,Month,Day,Hour,
// Minute,Second,Milliseconds,Weekday} (SHORT cada). O no-op deixava lixo no timestamp
// do nome de arquivo de log do pintok.sys. [fidelidade — implementacao real]
__attribute__((ms_abi)) static void NT_RtlTimeToTimeFields(PLARGE_INTEGER t, PVOID f) {
    if (!t || !f) return;
    int64_t time100 = t->QuadPart; if (time100 < 0) time100 = 0;
    int64_t totalSec = time100 / 10000000LL;
    int msec = (int)((time100 / 10000LL) % 1000);
    int64_t days = totalSec / 86400LL;
    int sod = (int)(totalSec % 86400LL);
    int16_t* tf = (int16_t*)f;
    int weekday = (int)((days + 1) % 7); if (weekday < 0) weekday += 7;   // 1601-01-01 = segunda
    int year = 1601;
    for (;;) { int leap = ((year%4==0 && year%100!=0) || year%400==0); int diy = leap ? 366 : 365;
               if (days >= diy) { days -= diy; year++; } else break; }
    int leap = ((year%4==0 && year%100!=0) || year%400==0);
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int mon = 0;
    for (; mon < 12; mon++) { int dim = md[mon] + ((mon==1 && leap) ? 1 : 0);
                              if (days >= dim) days -= dim; else break; }
    tf[0] = (int16_t)year;     tf[1] = (int16_t)(mon + 1);  tf[2] = (int16_t)(days + 1);
    tf[3] = (int16_t)(sod/3600); tf[4] = (int16_t)((sod%3600)/60); tf[5] = (int16_t)(sod%60);
    tf[6] = (int16_t)msec;     tf[7] = (int16_t)weekday;
}
__attribute__((ms_abi)) static NTSTATUS NT_RtlDuplicateUnicodeString(ULONG flag, PUNICODE_STRING src, PUNICODE_STRING dst) {
    if (g_pintok_trace) {
        kputs("  [trace] RtlDuplicateUnicodeString flag="); kput_hex(flag);
        kputs(" src.Length="); kput_hex(src ? src->Length : 0); kputs(" src='");
        if (src && src->Buffer) for (int i = 0; i < (src->Length/2) && i < 80; i++) kputc((char)src->Buffer[i]);
        kputs("'\n");
    }
    (void)flag;
    if (!src || !dst) return STATUS_INVALID_PARAMETER;
    USHORT n = src->Length;
    void* buf = ExAllocatePoolWithTag_k(NonPagedPool, n + 2, 0x554D5452u);    // 'RTMU'
    if (!buf) return STATUS_NO_MEMORY;
    for (USHORT i = 0; i < n; i++) ((uint8_t*)buf)[i] = ((uint8_t*)src->Buffer)[i];
    ((uint8_t*)buf)[n] = 0; ((uint8_t*)buf)[n+1] = 0;
    dst->Buffer = (WCHAR*)buf; dst->Length = n; dst->MaximumLength = (USHORT)(n + 2);
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static BOOLEAN NT_RtlPrefixUnicodeString(PUNICODE_STRING p, PUNICODE_STRING s, BOOLEAN CI) {
    (void)CI;
    if (!p || !s || p->Length > s->Length) return 0;
    for (USHORT i = 0; i < p->Length / 2; i++) if (p->Buffer[i] != s->Buffer[i]) return 0;
    return 1;
}
__attribute__((ms_abi)) static NTSTATUS NT_RtlMultiByteToUnicodeN(WCHAR* dst, ULONG dstSize, PULONG bytes, const char* src, ULONG srcLen) {
    ULONG n = srcLen < dstSize / 2 ? srcLen : dstSize / 2;
    for (ULONG i = 0; i < n; i++) dst[i] = (WCHAR)(uint8_t)src[i];
    if (bytes) *bytes = n * 2;
    return STATUS_SUCCESS;
}

// BCrypt extras
__attribute__((ms_abi)) static NTSTATUS NT_BCryptImportKeyPair(PVOID hAlg, PVOID hKeyToImport, const WCHAR* BlobType, PVOID* hKey, PVOID Blob, ULONG BlobLen, ULONG Flags) {
    (void)hAlg; (void)hKeyToImport; (void)BlobType; (void)Blob; (void)BlobLen; (void)Flags;
    if (hKey) *hKey = (PVOID)(uintptr_t)0xBCAA0003;
    return STATUS_SUCCESS;
}
__attribute__((ms_abi)) static NTSTATUS NT_BCryptVerifySignature(PVOID hKey, PVOID Pad, PVOID Hash, ULONG HashLen, PVOID Sig, ULONG SigLen, ULONG Flags) {
    (void)hKey; (void)Pad; (void)Hash; (void)HashLen; (void)Sig; (void)SigLen; (void)Flags;
    return STATUS_SUCCESS;
}

// FASE 7.9 — __C_specific_handler REAL em src/nt/seh.c.
//   Comportamento: detecta unwind/non-unwind, loga 1x e devolve
//   ExceptionContinueSearch (1) — propaga ao frame externo. O caminho de
//   recuperacao concreto fica no isr.c (map-zero em PF de driver).
__attribute__((ms_abi)) extern int __C_specific_handler(
        void* ExceptionRecord, void* EstablisherFrame,
        void* ContextRecord,   void* DispatcherContext);

// ----------------------------------------------------------------------------
//  Tabela de exports do "ntoskrnl.exe"
// ----------------------------------------------------------------------------
static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// Macros pra reduzir boilerplate.
#define EX(name, fn)        { name, (void*)(fn) }
#define EX_K(name, fn)      { name, (void*)(fn ## _k) }
#define EX_VAR(name, var)   { name, (void*)&var }

// >>> NAO REORDENAR sem testar — alguns drivers fazem busca linear sensivel <<<
static const struct { const char* name; void* fn; } g_ntexports[] = {
    // Debug / Logging
    EX("DbgPrint",                     NT_DbgPrint),
    EX("DbgPrintEx",                   NT_DbgPrintEx),
    EX("DbgPrompt",                    NT_DbgPrompt),
    EX("DbgQueryDebugFilterState",     NT_DbgQueryDebugFilterState),
    EX("KdDebuggerEnabled",            &KdDebuggerEnabled),
    EX("KdDebuggerNotPresent",         &KdDebuggerNotPresent),
    EX("KeBugCheck",                   NT_KeBugCheck),
    EX("KeBugCheckEx",                 NT_KeBugCheckEx),

    // Rtl
    EX("RtlInitUnicodeString",         NT_RtlInitUnicodeString),
    EX("RtlInitAnsiString",            NT_RtlInitAnsiString),
    EX("RtlUnicodeStringToAnsiString", NT_RtlUnicodeStringToAnsiString),
    EX("RtlAnsiStringToUnicodeString", NT_RtlAnsiStringToUnicodeString),
    EX("RtlCompareUnicodeString",      NT_RtlCompareUnicodeString),
    EX("RtlEqualUnicodeString",        NT_RtlEqualUnicodeString),
    EX("RtlCopyUnicodeString",         NT_RtlCopyUnicodeString),
    EX("RtlFreeUnicodeString",         NT_RtlFreeUnicodeString),
    EX("RtlFreeAnsiString",            NT_RtlFreeAnsiString),
    EX("RtlCompareMemory",             NT_RtlCompareMemory),
    EX("RtlCopyMemory",                NT_RtlCopyMemory),
    EX("RtlMoveMemory",                NT_RtlMoveMemory),
    EX("RtlZeroMemory",                NT_RtlZeroMemory),
    EX("RtlFillMemory",                NT_RtlFillMemory),
    EX("RtlGetVersion",                NT_RtlGetVersion),
    EX("memcpy",                       NT_RtlCopyMemory),   // mingw ctype
    EX("memset",                       NT_RtlFillMemory),
    EX("memmove",                      NT_RtlMoveMemory),

    // I/O Manager
    EX("IoCreateDevice",               NT_IoCreateDevice),
    EX("IoDeleteDevice",               NT_IoDeleteDevice),
    EX("IoAllocateIrp",                IoAllocateIrp_k),
    EX("IoFreeIrp",                    IoFreeIrp_k),
    EX("IoInitializeIrp",              IoInitializeIrp_k),
    EX("IoGetCurrentIrpStackLocation", IoGetCurrentIrpStackLocation_k),
    EX("IoGetNextIrpStackLocation",    IoGetNextIrpStackLocation_k),
    EX("IoSkipCurrentIrpStackLocation",IoSkipCurrentIrpStackLocation_k),
    EX("IoCopyCurrentIrpStackLocationToNext", IoCopyCurrentIrpStackLocationToNext_k),
    EX("IoSetCompletionRoutine",       IoSetCompletionRoutine_k),
    EX("IoCallDriver",                 IoCallDriver_ms),
    EX("IoCompleteRequest",            IoCompleteRequest_k),
    EX("IoCancelIrp",                  IoCancelIrp_k),
    EX("IoBuildAsynchronousFsdRequest",IoBuildAsynchronousFsdRequest_k),
    EX("IoBuildDeviceIoControlRequest",IoBuildDeviceIoControlRequest_k),
    EX("IoCreateSymbolicLink",         IoCreateSymbolicLink_k),
    EX("IoDeleteSymbolicLink",         IoDeleteSymbolicLink_k),
    EX("IoGetDeviceObjectPointer",     IoGetDeviceObjectPointer_k),
    EX("IoReleaseRemoveLockAndWait",   IoReleaseRemoveLockAndWait_k),
    EX("IoFileObjectType",             &IoFileObjectType),

    // Callbacks (Ps / Ob / Cm / Ex)
    EX("PsSetCreateProcessNotifyRoutine",     PsSetCreateProcessNotifyRoutine_k),
    EX("PsSetCreateProcessNotifyRoutineEx",   PsSetCreateProcessNotifyRoutineEx_k),
    EX("PsSetCreateThreadNotifyRoutine",      PsSetCreateThreadNotifyRoutine_k),
    EX("PsRemoveCreateThreadNotifyRoutine",   PsRemoveCreateThreadNotifyRoutine_k),
    EX("PsSetLoadImageNotifyRoutine",         PsSetLoadImageNotifyRoutine_k),
    EX("PsRemoveLoadImageNotifyRoutine",      PsRemoveLoadImageNotifyRoutine_k),
    EX("ObRegisterCallbacks",                 ObRegisterCallbacks_k),
    EX("ObUnRegisterCallbacks",               ObUnRegisterCallbacks_k),
    EX("CmRegisterCallback",                  CmRegisterCallback_k),
    EX("CmRegisterCallbackEx",                CmRegisterCallbackEx_k),
    EX("CmUnRegisterCallback",                CmUnRegisterCallback_k),
    EX("ExRegisterCallback",                  ExRegisterCallback_k),
    EX("ExUnregisterCallback",                ExUnregisterCallback_k),
    EX("PsProcessType",                       &PsProcessType),
    EX("PsThreadType",                        &PsThreadType),

    // Ps* inspecao + system threads
    EX("PsCreateSystemThread",         PsCreateSystemThread_k),
    EX("PsTerminateSystemThread",      PsTerminateSystemThread_k),
    EX("PsGetCurrentThreadId",         PsGetCurrentThreadId_k),
    EX("PsGetCurrentProcessId",        PsGetCurrentProcessId_k),
    EX("PsGetCurrentThread",           PsGetCurrentThread_k),
    EX("PsGetCurrentProcess",          PsGetCurrentProcess_k),
    EX("PsGetProcessId",               PsGetProcessId_k),
    EX("PsGetProcessImageFileName",    PsGetProcessImageFileName_k),
    EX("PsGetProcessPeb",              PsGetProcessPeb_k),
    EX("PsGetProcessWow64Process",     PsGetProcessWow64Process_k),
    EX("PsGetProcessCreateTimeQuadPart", PsGetProcessCreateTimeQuadPart_k),
    EX("PsGetProcessExitStatus",       PsGetProcessExitStatus_k),
    EX("PsIsProtectedProcess",         PsIsProtectedProcess_k),
    EX("PsIsProtectedProcessLight",    PsIsProtectedProcessLight_k),
    EX("PsLookupProcessByProcessId",   PsLookupProcessByProcessId_k),
    EX("PsLookupThreadByThreadId",     PsLookupThreadByThreadId_k),

    // Sincronizacao (Ke*)
    EX("KeInitializeEvent",            KeInitializeEvent_k),
    EX("KeSetEvent",                   KeSetEvent_k),
    EX("KeResetEvent",                 KeResetEvent_k),
    EX("KeClearEvent",                 KeClearEvent_k),
    EX("KeReadStateEvent",             KeReadStateEvent_k),
    EX("KeWaitForSingleObject",        KeWaitForSingleObject_k),
    EX("KeWaitForMultipleObjects",     KeWaitForMultipleObjects_k),
    EX("KeDelayExecutionThread",       KeDelayExecutionThread_k),
    EX("KeInitializeSpinLock",         KeInitializeSpinLock_k),
    EX("KeAcquireSpinLockRaiseToDpc",  KeAcquireSpinLockRaiseToDpc_k),
    EX("KeReleaseSpinLock",            KeReleaseSpinLock_k),
    EX("KeAcquireSpinLockAtDpcLevel",  KeAcquireSpinLockAtDpcLevel_k),
    EX("KeReleaseSpinLockFromDpcLevel",KeReleaseSpinLockFromDpcLevel_k),
    EX("KeGetCurrentIrql",             KeGetCurrentIrql_k),
    EX("KeRaiseIrql",                  KeRaiseIrql_k),
    EX("KeLowerIrql",                  KeLowerIrql_k),
    EX("KeInitializeMutex",            KeInitializeMutex_k),
    EX("KeReleaseMutex",               KeReleaseMutex_k),
    EX("KeInitializeSemaphore",        KeInitializeSemaphore_k),
    EX("KeReleaseSemaphore",           KeReleaseSemaphore_k),
    EX("KeQuerySystemTime",            KeQuerySystemTime_k),
    EX("KeQueryInterruptTime",         KeQueryInterruptTime_k),
    EX("KeQueryTimeIncrement",         KeQueryTimeIncrement_k),
    EX("KeQueryActiveProcessorCount",  KeQueryActiveProcessorCount_k),
    EX("KeQueryActiveProcessors",      KeQueryActiveProcessors_k),
    EX("KeQueryPerformanceCounter",    KeQueryPerformanceCounter_k),
    // FASE 7.2: KPCR/GS_BASE -> processador atual via gs:[0x200].
    EX("KeGetCurrentProcessorNumber",  KeGetCurrentProcessorNumber_k),
    EX("KeGetCurrentProcessorNumberEx",KeGetCurrentProcessorNumberEx_k),

    // Pool (Ex*)
    EX("ExAllocatePool",               ExAllocatePool_k),
    EX("ExAllocatePoolWithTag",        ExAllocatePoolWithTag_k),
    EX("ExAllocatePool2",              ExAllocatePool2_k),
    EX("ExAllocatePool3",              ExAllocatePool3_k),
    EX("ExAllocatePoolUninitialized",  ExAllocatePoolUninitialized_k),
    EX("ExFreePool",                   ExFreePool_k),
    EX("ExFreePoolWithTag",            ExFreePoolWithTag_k),
    EX("ExSystemTimeToLocalTime",      NT_ExSystemTimeToLocalTime),

    // Memoria (Mm*)
    EX("MmGetPhysicalAddress",         MmGetPhysicalAddress_k),
    EX("MmMapIoSpace",                 MmMapIoSpace_k),
    EX("MmUnmapIoSpace",               MmUnmapIoSpace_k),
    EX("MmAllocateContiguousMemory",   MmAllocateContiguousMemory_k),
    EX("MmFreeContiguousMemory",       MmFreeContiguousMemory_k),
    EX("MmAllocateNonCachedMemory",    MmAllocateNonCachedMemory_k),
    EX("MmFreeNonCachedMemory",        MmFreeNonCachedMemory_k),
    EX("MmIsAddressValid",             MmIsAddressValid_k),
    EX("MmProtectMdlSystemAddress",    MmProtectMdlSystemAddress_k),
    EX("MmGetSystemRoutineAddress",    NT_MmGetSystemRoutineAddress),
    // FASE 5 — Mm*/Nt*/Zw*VirtualMemory: aloca/libera/protege paginas via PMM.
    //   Drivers reais usam ZwAllocateVirtualMemory para reservar buffers grandes
    //   fora do pool (ex.: areas DMA contiguas, mapeamentos de teste). No NT,
    //   o caminho NtAllocate*/ZwAllocate* aterra na mesma rotina interna Mm*.
    //   Aqui devolvemos endereco identidade (kernel mapeia 1 GiB 1:1) e em caso
    //   de falta de RAM retornamos STATUS_INSUFFICIENT_RESOURCES (sem crash).
    EX("MmAllocateVirtualMemory",      MmAllocateVirtualMemory_k),
    EX("MmFreeVirtualMemory",          MmFreeVirtualMemory_k),
    EX("MmProtectVirtualMemory",       MmProtectVirtualMemory_k),
    EX("NtAllocateVirtualMemory",      MmAllocateVirtualMemory_k),
    EX("ZwAllocateVirtualMemory",      MmAllocateVirtualMemory_k),
    EX("NtFreeVirtualMemory",          MmFreeVirtualMemory_k),
    EX("ZwFreeVirtualMemory",          MmFreeVirtualMemory_k),
    EX("NtProtectVirtualMemory",       MmProtectVirtualMemory_k),
    EX("ZwProtectVirtualMemory",       MmProtectVirtualMemory_k),

    // Section objects
    EX("NtCreateSection",              NtCreateSection_k),
    EX("ZwCreateSection",              NtCreateSection_k),
    EX("NtOpenSection",                NtOpenSection_k),
    EX("ZwOpenSection",                NtOpenSection_k),
    EX("NtMapViewOfSection",           NtMapViewOfSection_k),
    EX("ZwMapViewOfSection",           NtMapViewOfSection_k),
    EX("NtUnmapViewOfSection",         NtUnmapViewOfSection_k),
    EX("ZwUnmapViewOfSection",         NtUnmapViewOfSection_k),

    // Registry
    EX("NtOpenKey",                    NtOpenKey_k),
    EX("ZwOpenKey",                    NtOpenKey_k),
    EX("NtCreateKey",                  NtCreateKey_k),
    EX("ZwCreateKey",                  NtCreateKey_k),
    EX("NtClose",                      NT_ZwClose),
    EX("ZwClose",                      NT_ZwClose),
    EX("NtCloseKey",                   NtCloseKey_k),
    EX("ZwCloseKey",                   NtCloseKey_k),
    EX("NtDeleteKey",                  NtDeleteKey_k),
    EX("ZwDeleteKey",                  NtDeleteKey_k),
    EX("NtEnumerateKey",               NtEnumerateKey_k),
    EX("ZwEnumerateKey",               NtEnumerateKey_k),
    EX("NtEnumerateValueKey",          NtEnumerateValueKey_k),
    EX("ZwEnumerateValueKey",          NtEnumerateValueKey_k),
    EX("NtSetValueKey",                NtSetValueKey_k),
    EX("ZwSetValueKey",                NtSetValueKey_k),
    EX("NtQueryValueKey",              NtQueryValueKey_k),
    EX("ZwQueryValueKey",              NtQueryValueKey_k),
    EX("NtDeleteValueKey",             NtDeleteValueKey_k),
    EX("ZwDeleteValueKey",             NtDeleteValueKey_k),
    EX("NtFlushKey",                   NtFlushKey_k),
    EX("ZwFlushKey",                   NtFlushKey_k),

    // NtQuerySystemInformation (Zw + Nt)
    EX("NtQuerySystemInformation",     NT_NtQuerySystemInformation_k),
    EX("ZwQuerySystemInformation",     NT_ZwQuerySystemInformation),

    // HAL (CPU/MSR/CPUID)
    EX("HalReadPortUchar",             HalReadPortUchar),
    EX("HalReadPortUshort",            HalReadPortUshort),
    EX("HalReadPortUlong",             HalReadPortUlong),
    EX("HalWritePortUchar",            HalWritePortUchar),
    EX("HalWritePortUshort",           HalWritePortUshort),
    EX("HalWritePortUlong",            HalWritePortUlong),
    // FASE 7 + 7.8 — CPUID/MSR via HAL (drivers reais usam para detectar HW e
    // hypervisor). HalCpuid e a forma generica; HalCpuidEx (este OS) e o mesmo
    // wrapper exposto explicitamente para consistencia com algumas versoes do
    // HAL da Microsoft.
    EX("HalReadMsr",                   HalReadMsr),
    EX("HalWriteMsr",                  HalWriteMsr),
    EX("HalCpuid",                     HalCpuid),
    EX("HalCpuidEx",                   HalCpuidEx),

    // Cache Manager / Security / WMI — STUBS (most drivers tolerate).
    EX("CcMapData",                    NT_CcMapData),
    EX("CcUnpinData",                  stub_void),
    EX("SeAccessCheck",                stub_false),
    EX("SeSinglePrivilegeCheck",       stub_false),
    EX("WmiTraceMessage",              stub_success),
    EX("IoWMIRegistrationControl",     stub_success),
    EX("ExSetTimerResolution",         stub_success),
    EX("ExNotifyCallback",             stub_void),

    // Bcrypt / CodeIntegrity
    EX("BCryptOpenAlgorithmProvider",  NT_BCryptOpenAlgorithmProvider),
    EX("BCryptCloseAlgorithmProvider", NT_BCryptCloseAlgorithmProvider),
    EX("BCryptCreateHash",             NT_BCryptCreateHash),
    EX("BCryptDestroyHash",            NT_BCryptDestroyHash),
    EX("BCryptHashData",               NT_BCryptHashData),
    EX("BCryptFinishHash",             NT_BCryptFinishHash),
    EX("BCryptGetProperty",            NT_BCryptGetProperty),
    EX("CiCheckSignedFile",            NT_CiCheckSignedFile),
    EX("CiValidateImageHeader",        NT_CiValidateImageHeader),

    // ======== FASE 7.1 — pintok.sys + outros drivers reais ========
    // Variaveis globais (exportadas como ENDERECO):
    EX("KeNumberProcessors",           &s_KeNumberProcessors),
    EX("MmHighestUserAddress",         &s_MmHighestUserAddress),
    EX("MmSystemRangeStart",           &s_MmSystemRangeStart),
    EX("NtBuildNumber",                &s_NtBuildNumber),
    EX("InitSafeBootMode",             &s_InitSafeBootMode),
    // FASE 7.5: PspCidTable — drivers reach-around acessam direto a tabela
    // global. Apontamos para o nosso struct CID_HANDLE_TABLE (com layout
    // suficiente para nao crashar em probes; busca por reach-around devolve
    // NULL fora das entradas vivas, mesmo comportamento de "PID inexistente").
    EX("PspCidTable",                  &g_pspcid_table),
    // C runtime
    EX("strlen",                       NT_strlen),
    EX("strcmp",                       NT_strcmp),
    EX("strncmp",                      NT_strncmp),
    EX("strchr",                       NT_strchr),
    EX("strnlen",                      NT_strnlen),
    EX("strncpy_s",                    NT_strncpy_s),
    EX("wcslen",                       NT_wcslen),
    EX("wcscmp",                       NT_wcscmp),
    EX("wcscpy_s",                     NT_strncpy_s),    // fallback p/ versao wide
    EX("wcscat_s",                     NT_strncpy_s),
    EX("_snprintf_s",                  NT_snprintf_s),
    EX("vsprintf_s",                   NT_vsprintf_s),
    EX("swprintf_s",                   NT_swprintf_s),
    EX("vswprintf_s",                  NT_vswprintf_s),
    EX("_vsnwprintf",                  NT_vswprintf_s),
    EX("_snwprintf",                   NT_swprintf_s),
    EX("_snwprintf_s",                 NT_swprintf_s),
    EX("swprintf",                     NT_swprintf_s),
    // Ke* extras
    EX("KeInitializeDpc",              KeInitializeDpc_k),
    EX("KeInitializeTimer",            KeInitializeTimer_k),
    EX("KeSetTimer",                   KeSetTimer_k),
    EX("KeCancelTimer",                KeCancelTimer_k),
    EX("KeInitializeApc",              NT_KeInitializeApc),
    EX("KeInsertQueueApc",             NT_KeInsertQueueApc),
    EX("KeInitializeMutant",           NT_KeInitializeMutant),
    EX("KeAreAllApcsDisabled",         NT_KeAreAllApcsDisabled),
    EX("KeIpiGenericCall",             NT_KeIpiGenericCall),
    EX("KfRaiseIrql",                  KfRaiseIrql_k),
    EX("KeRegisterBugCheckReasonCallback", NT_KeRegisterBugCheckReasonCallback),
    EX("KeDeregisterBugCheckReasonCallback", NT_KeDeregisterBugCheckReasonCallback),
    EX("KeQuerySystemTimePrecise",     NT_KeQuerySystemTimePrecise),
    // Mm* / Io* / Ob* extras
    EX("ProbeForRead",                 NT_ProbeForRead_real),
    EX("ProbeForWrite",                NT_ProbeForWrite_real),
    EX("IoGetCurrentProcess",          NT_IoGetCurrentProcess),
    EX("ObReferenceObjectByHandle",    NT_ObReferenceObjectByHandle),
    EX("ObfDereferenceObject",         NT_ObfDereferenceObject),
    // FASE 7.6 — MDL real (src/mm/mdl.c). Layout NT x64 com array de PFNs.
    EX("IoAllocateMdl",                IoAllocateMdl_k),
    EX("IoFreeMdl",                    IoFreeMdl_k),
    EX("MmProbeAndLockPages",          MmProbeAndLockPages_k),
    EX("MmMapLockedPagesSpecifyCache", MmMapLockedPagesSpecifyCache_k),
    EX("MmMapLockedPages",             MmMapLockedPages_k),
    EX("MmUnlockPages",                MmUnlockPages_k),
    EX("MmUnmapLockedPages",           MmUnmapLockedPages_k),
    EX("MmBuildMdlForNonPagedPool",    MmBuildMdlForNonPagedPool_k),
    EX("MmGetSystemAddressForMdlSafe", MmGetSystemAddressForMdlSafe_k),
    EX("MmGetMdlVirtualAddress",       MmGetMdlVirtualAddress_k),
    EX("MmGetMdlByteCount",            MmGetMdlByteCount_k),
    EX("MmGetMdlByteOffset",           MmGetMdlByteOffset_k),
    EX("MmSizeOfMdl",                  MmSizeOfMdl_k),
    EX("MmAllocatePagesForMdl",        NT_MmAllocatePagesForMdl),
    EX("MmFreePagesFromMdl",           NT_MmFreePagesFromMdl),
    EX("IoAllocateWorkItem",           NT_IoAllocateWorkItem),
    EX("IoFreeWorkItem",               NT_IoFreeWorkItem),
    EX("IoInitializeWorkItem",         NT_IoInitializeWorkItem),
    EX("IoQueueWorkItem",              NT_IoQueueWorkItem),
    EX("IoCreateFileEx",               NT_IoCreateFileEx),
    EX("IoQueryFileInformation",       stub_success),
    EX("ZwReadFile",                   NT_ZwReadFile_stub),
    EX("ZwWriteFile",                  NT_ZwWriteFile_stub),
    EX("ZwTerminateProcess",           NT_ZwTerminateProcess),
    EX("ZwFlushBuffersFile",           stub_success),
    EX("ZwOpenDirectoryObject",        NT_ZwOpenDirectoryObject),
    EX("ZwQueryDirectoryObject",       NT_ZwQueryDirectoryObject),
    EX("RtlTimeToTimeFields",          NT_RtlTimeToTimeFields),
    EX("RtlDuplicateUnicodeString",    NT_RtlDuplicateUnicodeString),
    EX("RtlPrefixUnicodeString",       NT_RtlPrefixUnicodeString),
    EX("RtlMultiByteToUnicodeN",       NT_RtlMultiByteToUnicodeN),
    EX("BCryptVerifySignature",        NT_BCryptVerifySignature),
    EX("BCryptImportKeyPair",          NT_BCryptImportKeyPair),
    EX("__C_specific_handler",         __C_specific_handler),

    // ======== FASE DX — dxgkrnl + dxgmms ========
    // Estes exports aparecem em dxgkrnl.sys / dxgmms2.sys reais do Windows.
    // Drivers de display (BasicDisplay, indddi, KMDs de fornecedores) importam
    // estes simbolos por nome — entao mantemos os mesmos nomes do NT (sem
    // sufixo _k) para que pe_bind_imports resolva sem traducao.
    EX("DxgkInitialize",               DxgkInitialize),
    EX("DxgkShutdown",                 DxgkShutdown),
    EX("DxgkOpenAdapter",              DxgkOpenAdapter),
    EX("DxgkCreateDevice",             DxgkCreateDevice),
    EX("DxgkCreateContext",            DxgkCreateContext),
    EX("DxgkCreateAllocation",         DxgkCreateAllocation),
    EX("DxgkPresent",                  DxgkPresent),
    EX("DxgkPresentDisplayOnly",       DxgkPresentDisplayOnly),
    EX("DxgkSubmitCommand",            DxgkSubmitCommand),
    EX("DxgMmsInitialize",             DxgMmsInitialize),
    EX("DxgMmsShutdown",               DxgMmsShutdown),
    EX("DxgMmsAllocate",               DxgMmsAllocate),
    EX("DxgMmsFree",                   DxgMmsFree),
    EX("DxgMmsLock",                   DxgMmsLock),
    EX("DxgMmsUnlock",                 DxgMmsUnlock),

    // ======== FASE 12 — NDIS framework (network stack) ========
    // Exports do ndis.sys do Windows. Drivers de NIC (miniport) e protocolo
    // (tcpip, nbf) importam por nome — pe_bind_imports resolve aqui. Sem
    // sufixo "_k" para casar com o NDK do Windows. Implementacoes em
    // src/drivers/network/ndis/ndis.c.
    EX("NdisInitializeWrapper",            ndis_NdisInitializeWrapper),
    EX("NdisMRegisterMiniportDriver",      ndis_NdisMRegisterMiniportDriver),
    EX("NdisRegisterProtocolDriver",       ndis_NdisRegisterProtocolDriver),
    EX("NdisAllocateNetBufferList",        ndis_NdisAllocateNetBufferList),
    EX("NdisFreeNetBufferList",            ndis_NdisFreeNetBufferList),
    EX("NdisMSendNetBufferListsComplete",  ndis_NdisMSendNetBufferListsComplete),
    EX("NdisFreeMemory",                   ndis_NdisFreeMemory),
    EX("NdisAllocateMemoryWithTag",        ndis_NdisAllocateMemoryWithTag),
    EX("NdisMResetComplete",               ndis_NdisMResetComplete),
    EX("NdisGetVersion",                   ndis_NdisGetVersion),

    // ======== FASE 13 — PnP Manager (I/O Manager) =====================
    // Drivers de bus enumerator (e.g. PCI/USB) chamam IoReportDeviceObject
    // para anunciar PDOs novos; IoInvalidateDeviceState pede ao PnP que
    // re-pergunte QUERY_* depois de mudanca de estado/resources.
    EX("IoInvalidateDeviceState",          pnp_IoInvalidateDeviceState),
    EX("IoReportDeviceObject",             pnp_IoReportDeviceObject),

    // ======== FASE 13 — Filter Manager (fltmgr.sys) ===================
    // Exports do fltmgr.sys consumidos por minifilters (antivirus, encrypt,
    // ProcMon-style). Sem callbacks reais sendo invocadas — apenas ABI.
    EX("FltRegisterFilter",                fltmgr_FltRegisterFilter),
    EX("FltStartFiltering",                fltmgr_FltStartFiltering),
    EX("FltUnregisterFilter",              fltmgr_FltUnregisterFilter),
    EX("FltCreateCommunicationPort",       fltmgr_FltCreateCommunicationPort),
    EX("FltSendMessage",                   fltmgr_FltSendMessage),
    EX("FltSetCallbackDataDirty",          fltmgr_FltSetCallbackDataDirty),

    // FASE FUNDACAO (Item 4): stall real por TSC. Append-only (nao reordenar).
    // KeQueryPerformanceCounter ja esta acima (linha ~960); so o corpo mudou.
    EX("KeStallExecutionProcessor",        KeStallExecutionProcessor_k),
    // FASE FUNDACAO (Item 1): IRQL real. KfRaiseIrql foi repontado no lugar
    // (~1118); KfLowerIrql/KeRaiseIrqlToDpcLevel adicionados aqui (append-only).
    EX("KfLowerIrql",                      KfLowerIrql_k),
    EX("KeRaiseIrqlToDpcLevel",            KeRaiseIrqlToDpcLevel_k),
    // FASE FUNDACAO (Item 3): spinlocks reais (variantes extras, append-only).
    EX("KeAcquireSpinLock",                KeAcquireSpinLock_k),
    EX("KfAcquireSpinLock",                KfAcquireSpinLock_k),
    EX("KfReleaseSpinLock",                KfReleaseSpinLock_k),
    EX("KeAcquireInStackQueuedSpinLock",   KeAcquireInStackQueuedSpinLock_k),
    EX("KeReleaseInStackQueuedSpinLock",   KeReleaseInStackQueuedSpinLock_k),
    // FASE FUNDACAO (Item 2): DPC real. KeInitializeDpc repontado no lugar
    // (~1109); Insert/Remove/SetImportance adicionados aqui (append-only).
    EX("KeInsertQueueDpc",                 KeInsertQueueDpc_k),
    EX("KeRemoveQueueDpc",                 KeRemoveQueueDpc_k),
    EX("KeSetImportanceDpc",               KeSetImportanceDpc_k),
    // FASE FUNDACAO (Item 6): KTIMER real. Init/Set/Cancel repontados no lugar;
    // Ex/ReadState append-only.
    EX("KeInitializeTimerEx",              KeInitializeTimerEx_k),
    EX("KeSetTimerEx",                     KeSetTimerEx_k),
    EX("KeReadStateTimer",                 KeReadStateTimer_k),
    // FASE FUNDACAO (Item 7): primitivos Ex reais (flag-gated). Append-only.
    EX("ExInitializeFastMutex",            ExInitializeFastMutex_k),
    EX("ExAcquireFastMutex",               ExAcquireFastMutex_k),
    EX("ExReleaseFastMutex",               ExReleaseFastMutex_k),
    EX("ExTryToAcquireFastMutex",          ExTryToAcquireFastMutex_k),
    EX("ExAcquireFastMutexUnsafe",         ExAcquireFastMutexUnsafe_k),
    EX("ExReleaseFastMutexUnsafe",         ExReleaseFastMutexUnsafe_k),
    EX("ExInitializeResourceLite",         ExInitializeResourceLite_k),
    EX("ExAcquireResourceExclusiveLite",   ExAcquireResourceExclusiveLite_k),
    EX("ExAcquireResourceSharedLite",      ExAcquireResourceSharedLite_k),
    EX("ExReleaseResourceLite",            ExReleaseResourceLite_k),
    EX("ExDeleteResourceLite",             ExDeleteResourceLite_k),
    EX("ExInitializeNPagedLookasideList",  ExInitializeNPagedLookasideList_k),
    EX("ExAllocateFromNPagedLookasideList",ExAllocateFromNPagedLookasideList_k),
    EX("ExFreeToNPagedLookasideList",      ExFreeToNPagedLookasideList_k),
    EX("ExDeleteNPagedLookasideList",      ExDeleteNPagedLookasideList_k),
    EX("ExInterlockedInsertHeadList",      ExInterlockedInsertHeadList_k),
    EX("ExInterlockedInsertTailList",      ExInterlockedInsertTailList_k),
    EX("ExInterlockedRemoveHeadList",      ExInterlockedRemoveHeadList_k),
    EX("ExRaiseStatus",                    ExRaiseStatus_k),
    // trilha I/O Fase 3: modelo de interrupcao (append-only). Nenhum destes
    // nomes esta na lista do pintok -> efeito zero p/ ele.
    EX("IoConnectInterrupt",               IoConnectInterrupt_k),
    EX("IoDisconnectInterrupt",            IoDisconnectInterrupt_k),
    EX("KeInitializeInterrupt",            KeInitializeInterrupt_k),
    EX("KeConnectInterrupt",               KeConnectInterrupt_k),
    EX("KeDisconnectInterrupt",            KeDisconnectInterrupt_k),
    EX("KeSynchronizeExecution",           KeSynchronizeExecution_k),
    EX("HalGetInterruptVector",            HalGetInterruptVector),
    // trilha I/O Fase 2: device stacks (append-only, flag-gated p/ pintok).
    EX("IoAttachDeviceToDeviceStack",      IoAttachDeviceToDeviceStack_k),
    EX("IoAttachDeviceToDeviceStackSafe",  IoAttachDeviceToDeviceStackSafe_k),
    EX("IoDetachDevice",                   IoDetachDevice_k),
    EX("IoGetAttachedDevice",              IoGetAttachedDevice_k),
    EX("IoGetAttachedDeviceReference",     IoGetAttachedDeviceReference_k),
    EX("IoGetLowerDeviceObject",           IoGetLowerDeviceObject_k),
    // trilha I/O Fase 5: HAL DMA (append-only). Common-buffer flag-gated p/ pintok.
    EX("HalGetDmaAdapter",                 HalGetDmaAdapter),
    EX("HalAllocateCommonBuffer",          HalAllocateCommonBuffer),
    EX("HalFreeCommonBuffer",              HalFreeCommonBuffer),

    { 0, 0 }
};

// Generic stub para imports nao listados. Caminho seguro: retorna 0 (NULL).
// Como nao sabemos a assinatura, NUNCA passamos parametros — o codigo do driver
// que recebe o ponteiro e chamar com ABI errada SEM CRASHAR o kernel ja eh
// quebra de contrato dele. Mas: 0 retornado pra qualquer chamada normal de
// kernel API e tratado pela maioria como "API nao disponivel" -> ramo de erro.
static volatile uint64_t g_generic_stub_calls = 0;
__attribute__((ms_abi)) static uint64_t generic_zero_stub(void) {
    g_generic_stub_calls++;
    if (g_pintok_trace) {
        kputs("  [trace] generic_zero_stub call #"); kput_hex(g_generic_stub_calls); kputs("\n");
    }
    return 0;
}
uint64_t ntex_generic_stub_calls(void) { return g_generic_stub_calls; }

// ============================================================================
//  FASE FUNDACAO — FLAG DE MODO LEGADO (compat pintok). [pedido do usuario]
//
//    g_ke_legacy_mode == 0  (DEFAULT): faz o CORRETO — IRQL/DPC/waits/timers/Ex
//                                      REAIS (comportamento NT de verdade).
//    g_ke_legacy_mode == 1  : VOLTA PRO ANTIGO — stubs / auto-resolve (o
//                                      comportamento pre-fundacao). Uma flag
//                                      reverte tudo que e' sensivel.
//
//  ke_legacy_active() liga o modo antigo se a flag manual estiver setada OU
//  AUTOMATICAMENTE enquanto o pintok roda (g_pintok_trace=1 no DriverEntry dele).
//  Assim a trajetoria do pintok NUNCA muda — sem precisar setar nada a mao — e
//  os primitivos reais ficam ativos para todo o resto (nossos testes / drivers
//  genericos).
// ============================================================================
volatile int g_ke_legacy_mode = 0;
int ke_legacy_active(void) { return g_ke_legacy_mode || g_pintok_trace; }

// Reporte unico por nome (nao polui a serial).
#define MISS_MAX 32
static const char* s_missed[MISS_MAX];
static int s_nmissed = 0;
static int already_missed(const char* fn) {
    for (int i = 0; i < s_nmissed; i++) if (streq(s_missed[i], fn)) return 1;
    if (s_nmissed < MISS_MAX) s_missed[s_nmissed++] = fn;
    return 0;
}

// ----------------------------------------------------------------------------
//  Resolvedor multi-DLL (driver_import_resolver).
//
//  pe_bind_imports passa o NOME da DLL de cada import descriptor (ex.:
//  "ntoskrnl.exe", "hal.dll", "CI.dll", "cng.sys", "ksecdd.sys", "FLTMGR.SYS").
//  Antes, ntkrnl_resolve ignorava 'dll' e resolvia tudo da mesma tabela plana —
//  funciona porque no modelo do MeuOS o "kernel" e uma unica imagem e g_ntexports
//  agrega ntoskrnl+hal+CI+cng+fltmgr. Agora dispatchamos por DLL para:
//    (a) logar a origem correta (diagnostico do GATE 6: hal/CI/cng);
//    (b) permitir tabelas especificas por DLL no futuro sem tocar no resto;
//    (c) tratar nomes "decorados" do HAL (Hal*) e variaveis de dispatch table.
//
//  pintok.sys (pintok.sys) importa majoritariamente do ntoskrnl, mas o GATE 6 mostra
//  que ele tambem valida hal.dll, CI.dll e cng.sys — todos resolvem aqui.
// ----------------------------------------------------------------------------

// Igualdade case-insensitive de nomes de DLL (o PE pode trazer "CI.dll",
// "ci.dll", "FLTMGR.SYS", etc. — a comparacao do loader e por bytes).
static int dll_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

// Busca um export na tabela plana g_ntexports[] (ntoskrnl+hal+CI+cng+fltmgr+...).
static void* nt_lookup(const char* fn) {
    for (int i = 0; g_ntexports[i].name; i++)
        if (streq(g_ntexports[i].name, fn)) return g_ntexports[i].fn;
    return 0;
}

// Categoria da DLL — usada so p/ log/diagnostico (todas caem na mesma tabela).
static const char* dll_category(const char* dll) {
    if (!dll)                              return "ntoskrnl";
    if (dll_ieq(dll, "ntoskrnl.exe"))      return "ntoskrnl";
    if (dll_ieq(dll, "ntkrnlpa.exe"))      return "ntoskrnl";
    if (dll_ieq(dll, "hal.dll"))           return "hal";
    if (dll_ieq(dll, "CI.dll"))            return "ci";
    if (dll_ieq(dll, "cng.sys"))           return "cng";
    if (dll_ieq(dll, "ksecdd.sys"))        return "ksecdd";
    if (dll_ieq(dll, "FLTMGR.SYS"))        return "fltmgr";
    if (dll_ieq(dll, "ndis.sys"))          return "ndis";
    if (dll_ieq(dll, "netio.sys"))         return "netio";
    if (dll_ieq(dll, "WDFLDR.SYS"))        return "wdf";
    return "other";
}

// Resolvedor unificado, ciente da DLL de origem. Despacha por nome de DLL e
// resolve da tabela agregada; loga a origem e cai num stub seguro se faltar.
void* driver_import_resolver(const char* dll, const char* fn) {
    void* r = nt_lookup(fn);
    if (r) return r;

    // Nao achou pelo nome exato. Tenta variantes comuns de decoracao:
    //  - HAL: algumas versoes exportam "HalpXxx"/"x86BiosXxx"; ignoramos prefixo.
    //  - alias C-runtime (memcpy/memset ja na tabela). Aqui so o fallback.
    // (Mantido simples: a tabela plana ja cobre os 217 nomes do pintok.sys.)

    if (!already_missed(fn)) {
        kputs("[ntex] ("); kputs(dll_category(dll)); kputs(") stub generico p/ '");
        kputs(fn); kputs("' (no-op, retorna 0)\n");
    }
    return (void*)generic_zero_stub;
}

// Compat: ntkrnl_resolve continua sendo o ponto de entrada usado por
// driver.c/loader.c. Agora apenas encaminha pro resolvedor multi-DLL.
void* ntkrnl_resolve(const char* dll, const char* fn) {
    return driver_import_resolver(dll, fn);
}
