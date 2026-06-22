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
#include "nt/ntexec.h"
#include "nt/io.h"
#include "nt/object.h"
#include "nt/callbacks.h"
#include "nt/section.h"
#include "nt/registry.h"
#include "ke/sync.h"
#include "ke/pool.h"
#include "ke/systhread.h"
#include "ke/kpcr.h"          // FASE 7.2: KPCR/GS_BASE
#include "hal/cpu.h"
#include "hal/hal.h"
#include "hal/hypervisor.h"   // FASE 7.8: HalCpuidEx + hv_info_get
#include "nt/process.h"
#include "nt/cid_table.h"     // FASE 7.5: PspCidTable exposta como variavel
#include "mm/mdl.h"           // FASE 7.6: MDL real (IoAllocateMdl/MmProbeAndLockPages/...)
#include "mm/virtual_memory.h"// FASE 5: Mm*/Nt*VirtualMemory (alloc/free/protect)

// Forwards das funcoes implementadas em ke/kpcr.c (NTAPI / ms_abi).
__attribute__((ms_abi)) ULONG KeGetCurrentProcessorNumber_k(void);
__attribute__((ms_abi)) ULONG KeGetCurrentProcessorNumberEx_k(void* ProcNumber);

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);

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

// ZwQuerySystemInformation: drivers usam pra checar VBS/HVCI/SecureBoot/etc.
__attribute__((ms_abi)) static NTSTATUS NT_ZwQuerySystemInformation(ULONG cls, PVOID buf, ULONG buflen, PULONG ret) {
    if (g_pintok_trace) {
        kputs("  [trace] ZwQuerySystemInformation(class=0x"); kput_hex(cls);
        kputs(" buflen="); kput_hex(buflen); kputs(")\n");
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
__attribute__((ms_abi)) static int NT_snprintf_s_stub(char* buf, SIZE_T sz, SIZE_T cnt, const char* fmt, ...) {
    (void)cnt; (void)fmt;
    if (buf && sz) buf[0] = 0;
    return 0;
}

// ---- Ke* faltantes (no-ops seguros) ----
__attribute__((ms_abi)) static void NT_KeInitializeDpc(PVOID Dpc, PVOID Routine, PVOID Ctx) {
    (void)Routine; (void)Ctx;
    if (Dpc) { for (int i = 0; i < 64; i++) ((uint8_t*)Dpc)[i] = 0; }
}
__attribute__((ms_abi)) static void NT_KeInitializeTimer(PVOID Timer) {
    if (Timer) { for (int i = 0; i < 64; i++) ((uint8_t*)Timer)[i] = 0; }
}
__attribute__((ms_abi)) static BOOLEAN NT_KeSetTimer(PVOID Timer, LARGE_INTEGER Due, PVOID Dpc) {
    (void)Timer; (void)Due; (void)Dpc; return 0;
}
__attribute__((ms_abi)) static BOOLEAN NT_KeCancelTimer(PVOID Timer) { (void)Timer; return 1; }
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
__attribute__((ms_abi)) static void NT_KeIpiGenericCall(PVOID Routine, uintptr_t Ctx) { (void)Routine; (void)Ctx; }
__attribute__((ms_abi)) static KIRQL NT_KfRaiseIrql(KIRQL NewIrql) { (void)NewIrql; return PASSIVE_LEVEL; }
__attribute__((ms_abi)) static NTSTATUS NT_KeRegisterBugCheckReasonCallback(PVOID Record, PVOID Cb, ULONG Reason, PVOID Component) {
    (void)Record; (void)Cb; (void)Reason; (void)Component; return STATUS_SUCCESS;
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
__attribute__((ms_abi)) static NTSTATUS NT_IoCreateFileEx(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io, PLARGE_INTEGER sz, ULONG fa, ULONG sh, ULONG d, ULONG co, PVOID ea, ULONG ealen, ULONG ck, PVOID ctx) {
    (void)a; (void)oa; (void)io; (void)sz; (void)fa; (void)sh; (void)d; (void)co; (void)ea; (void)ealen; (void)ck; (void)ctx;
    if (h) *h = 0;
    return STATUS_NOT_IMPLEMENTED;
}
__attribute__((ms_abi)) static NTSTATUS NT_ZwReadFile_stub(HANDLE h, HANDLE e, PVOID rt, PVOID c, PIO_STATUS_BLOCK io, PVOID buf, ULONG len, PLARGE_INTEGER off, PULONG key) {
    (void)h; (void)e; (void)rt; (void)c; (void)off; (void)key;
    if (buf && len) for (ULONG i = 0; i < len; i++) ((uint8_t*)buf)[i] = 0;
    if (io) { io->Status = STATUS_END_OF_FILE; io->Information = 0; }
    return STATUS_END_OF_FILE;
}
__attribute__((ms_abi)) static NTSTATUS NT_ZwWriteFile_stub(HANDLE h, HANDLE e, PVOID rt, PVOID c, PIO_STATUS_BLOCK io, PVOID buf, ULONG len, PLARGE_INTEGER off, PULONG key) {
    (void)h; (void)e; (void)rt; (void)c; (void)buf; (void)off; (void)key;
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
__attribute__((ms_abi)) static void NT_RtlTimeToTimeFields(PLARGE_INTEGER t, PVOID f) { (void)t; (void)f; }
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
    EX("_snprintf_s",                  NT_snprintf_s_stub),
    EX("vsprintf_s",                   NT_snprintf_s_stub),
    EX("swprintf_s",                   NT_snprintf_s_stub),
    EX("vswprintf_s",                  NT_snprintf_s_stub),
    EX("_vsnwprintf",                  NT_snprintf_s_stub),
    // Ke* extras
    EX("KeInitializeDpc",              NT_KeInitializeDpc),
    EX("KeInitializeTimer",            NT_KeInitializeTimer),
    EX("KeSetTimer",                   NT_KeSetTimer),
    EX("KeCancelTimer",                NT_KeCancelTimer),
    EX("KeInitializeApc",              NT_KeInitializeApc),
    EX("KeInsertQueueApc",             NT_KeInsertQueueApc),
    EX("KeInitializeMutant",           NT_KeInitializeMutant),
    EX("KeAreAllApcsDisabled",         NT_KeAreAllApcsDisabled),
    EX("KeIpiGenericCall",             NT_KeIpiGenericCall),
    EX("KfRaiseIrql",                  NT_KfRaiseIrql),
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

// Reporte unico por nome (nao polui a serial).
#define MISS_MAX 32
static const char* s_missed[MISS_MAX];
static int s_nmissed = 0;
static int already_missed(const char* fn) {
    for (int i = 0; i < s_nmissed; i++) if (streq(s_missed[i], fn)) return 1;
    if (s_nmissed < MISS_MAX) s_missed[s_nmissed++] = fn;
    return 0;
}

void* ntkrnl_resolve(const char* dll, const char* fn) {
    (void)dll;
    for (int i = 0; g_ntexports[i].name; i++)
        if (streq(g_ntexports[i].name, fn)) return g_ntexports[i].fn;

    // export desconhecido: loga 1x e devolve o stub (NAO devolve 0, p/ que o
    // pe_bind_imports nao loge "import nao resolvido" e o driver carregue).
    if (!already_missed(fn)) {
        kputs("[ntex] stub generico p/ '"); kputs(fn); kputs("' (no-op, retorna 0)\n");
    }
    return (void*)generic_zero_stub;
}
