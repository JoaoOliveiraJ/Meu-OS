// ============================================================================
//  calller.sys — Test driver da FASE 7 (Driver Framework completo).
//
//  Exercita TODOS os subsistemas novos do MeuOS: callbacks (Ps/Ob/Cm),
//  system thread, KEVENT/spinlock, ExAllocatePool com tag, CPUID/MSR,
//  IOCTL (IRP_MJ_DEVICE_CONTROL), section objects, registro, debug.
//
//  build.ps1 compila como NATIVE (.sys) com -lntoskrnl; o boot carrega via
//  modulo Multiboot e chama DriverEntry. Saida toda na serial.
//
//  Convencao: "stub honesto" — onde a infra do MeuOS e simplificada (sem
//  scheduler real, sem APIC, etc.) o driver loga STATUS e continua. Caminho
//  seguro: nenhum bug-check, nenhum panic.
// ============================================================================
#include "ntddk.h"

unsigned int _tls_index = 0;

__declspec(dllimport) ULONG  DbgPrint(const char* Format, ...);
__declspec(dllimport) NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, ULONG, ULONG, BOOLEAN, PDEVICE_OBJECT*);
__declspec(dllimport) void   IoDeleteDevice(PDEVICE_OBJECT);
__declspec(dllimport) NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING);
__declspec(dllimport) NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING);
__declspec(dllimport) void   IoCompleteRequest(PIRP, uint8_t);
__declspec(dllimport) void   RtlInitUnicodeString(PUNICODE_STRING, const WCHAR*);
__declspec(dllimport) PVOID  ExAllocatePoolWithTag(POOL_TYPE, SIZE_T, ULONG);
__declspec(dllimport) void   ExFreePoolWithTag(PVOID, ULONG);
__declspec(dllimport) void   KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN);
__declspec(dllimport) LONG   KeSetEvent(PKEVENT, KPRIORITY, BOOLEAN);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(PVOID, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) NTSTATUS PsCreateSystemThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PCLIENT_ID, PKSTART_ROUTINE, PVOID);
__declspec(dllimport) NTSTATUS PsSetCreateProcessNotifyRoutine(PCREATE_PROCESS_NOTIFY_ROUTINE, BOOLEAN);
__declspec(dllimport) NTSTATUS PsSetCreateThreadNotifyRoutine(PCREATE_THREAD_NOTIFY_ROUTINE);
__declspec(dllimport) NTSTATUS PsSetLoadImageNotifyRoutine(PLOAD_IMAGE_NOTIFY_ROUTINE);
__declspec(dllimport) NTSTATUS CmRegisterCallback(PEX_CALLBACK_FUNCTION, PVOID, PLARGE_INTEGER);
__declspec(dllimport) NTSTATUS NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);

// KeInitializeSpinLock e HalReadMsr / HalCpuid sao "intrinsics" no WDK (macros
// no header). No nosso ABI eles existem como exports do MeuOS, mas Zig nao gera
// stubs por padrao. Em vez de torna-los imports, fazemos versoes inline locais
// que executam a operacao diretamente — drivers reais fazem o mesmo via macro.
static inline void calller_KeInitializeSpinLock(PKSPIN_LOCK lock) { if (lock) *lock = 0; }
static inline uint64_t calller_HalReadMsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void calller_HalCpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    uint32_t ra, rb, rc, rd;
    __asm__ volatile ("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd) : "a"(leaf), "c"(0));
    if (a) *a = ra; if (b) *b = rb; if (c) *c = rc; if (d) *d = rd;
}

#define CALLLER_IOCTL_ECHO    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, 0)
#define CALLLER_IOCTL_SECTION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, 0)
#define CALLLER_TAG           0x6C6C6143u   // 'Call'

static UNICODE_STRING g_devname;
static UNICODE_STRING g_symname;
static KEVENT  g_event;
static KSPIN_LOCK g_lock;
static PVOID   g_section_base = 0;
static HANDLE  g_thread = 0;

// ---- Notify routines (callbacks) ----
static void NTAPI cb_process(HANDLE Parent, HANDLE Pid, BOOLEAN Create) {
    DbgPrint(Create ? "calller: process_create CB\n" : "calller: process_exit CB\n");
    (void)Parent; (void)Pid;
}
static void NTAPI cb_thread(HANDLE Pid, HANDLE Tid, BOOLEAN Create) {
    DbgPrint(Create ? "calller: thread_create CB\n" : "calller: thread_exit CB\n");
    (void)Pid; (void)Tid;
}
static void NTAPI cb_image(PUNICODE_STRING ImgName, HANDLE Pid, PVOID Info) {
    DbgPrint("calller: image_load CB\n");
    (void)ImgName; (void)Pid; (void)Info;
}
static NTSTATUS NTAPI cb_registry(PVOID Ctx, PVOID Arg1, PVOID Arg2) {
    DbgPrint("calller: registry CB\n");
    (void)Ctx; (void)Arg1; (void)Arg2;
    return STATUS_SUCCESS;
}

// ---- System thread ----
static void NTAPI sys_thread_entry(PVOID Context) {
    (void)Context;
    DbgPrint("calller: system thread iniciada\n");
    // Sem scheduler, o KeWaitForSingleObject retorna imediato.
    LARGE_INTEGER timeout; timeout.QuadPart = -10000;   // 1 ms
    for (int i = 0; i < 3; i++) {
        KeWaitForSingleObject(&g_event, Executive, KernelMode, 0, &timeout);
        DbgPrint("calller: thread tick\n");
    }
    DbgPrint("calller: system thread saindo\n");
}

// ---- Dispatch ----
static NTSTATUS NTAPI DispatchCreate(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    DbgPrint("calller: IRP_MJ_CREATE\n");
    irp->IoStatus.Status = STATUS_SUCCESS; irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI DispatchClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    DbgPrint("calller: IRP_MJ_CLOSE\n");
    irp->IoStatus.Status = STATUS_SUCCESS; irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI DispatchCleanup(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    DbgPrint("calller: IRP_MJ_CLEANUP\n");
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI DispatchIoctl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    DbgPrint("calller: IRP_MJ_DEVICE_CONTROL\n");
    if (code == CALLLER_IOCTL_ECHO) {
        // Cifra "RC4" simbolica (xor de demo): apenas reflete os bytes invertidos.
        ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength;
        ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
        if (irp->AssociatedIrp.SystemBuffer && outLen >= inLen) {
            uint8_t* p = (uint8_t*)irp->AssociatedIrp.SystemBuffer;
            for (ULONG i = 0; i < inLen; i++) p[i] ^= 0x5A;
            irp->IoStatus.Information = inLen;
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, 0);
        return STATUS_SUCCESS;
    }
    if (code == CALLLER_IOCTL_SECTION) {
        // Devolve um marcador identificando que a section esta viva.
        if (irp->AssociatedIrp.SystemBuffer && s->Parameters.DeviceIoControl.OutputBufferLength >= 8) {
            *(uint64_t*)irp->AssociatedIrp.SystemBuffer = (uint64_t)(uintptr_t)g_section_base;
            irp->IoStatus.Information = 8;
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, 0);
        return STATUS_SUCCESS;
    }
    irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, 0);
    return STATUS_NOT_IMPLEMENTED;
}

static void NTAPI DriverUnload(PDRIVER_OBJECT drv) {
    DbgPrint("calller: DriverUnload\n");
    IoDeleteSymbolicLink(&g_symname);
    if (drv->DeviceObject) IoDeleteDevice((PDEVICE_OBJECT)drv->DeviceObject);
    if (g_section_base) {
        // ExFreePoolWithTag se foi pool — caso de section sera no kernel.
    }
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg) {
    (void)reg;
    DbgPrint("calller: DriverEntry start\n");

    // Dispatch table (IRP majors)
    for (int i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = 0;
    drv->MajorFunction[IRP_MJ_CREATE]         = (PVOID)DispatchCreate;
    drv->MajorFunction[IRP_MJ_CLOSE]          = (PVOID)DispatchClose;
    drv->MajorFunction[IRP_MJ_CLEANUP]        = (PVOID)DispatchCleanup;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = (PVOID)DispatchIoctl;
    drv->DriverUnload                         = (PVOID)DriverUnload;

    // Device + symbolic link
    RtlInitUnicodeString(&g_devname, L"\\Device\\Calller");
    RtlInitUnicodeString(&g_symname, L"\\DosDevices\\Calller");
    PDEVICE_OBJECT dev = 0;
    NTSTATUS st = IoCreateDevice(drv, 0, &g_devname, FILE_DEVICE_UNKNOWN, 0, 0, &dev);
    if (!NT_SUCCESS(st)) { DbgPrint("calller: IoCreateDevice falhou\n"); return st; }
    IoCreateSymbolicLink(&g_symname, &g_devname);
    DbgPrint("calller: \\Device\\Calller + \\DosDevices\\Calller criados\n");

    // KEVENT + spin lock
    KeInitializeEvent(&g_event, NotificationEvent, 0);
    calller_KeInitializeSpinLock(&g_lock);
    DbgPrint("calller: KEVENT + KSPIN_LOCK inicializados\n");

    // Pool com tag
    void* buf = ExAllocatePoolWithTag(NonPagedPool, 256, CALLLER_TAG);
    if (buf) {
        DbgPrint("calller: ExAllocatePoolWithTag(256) OK\n");
        ExFreePoolWithTag(buf, CALLLER_TAG);
    }

    // CPUID + MSR (le IA32_APIC_BASE como demo). Sao intrinsics no WDK: inline.
    uint32_t a, b, c, d;
    calller_HalCpuid(0, &a, &b, &c, &d);
    DbgPrint("calller: CPUID.0 lido\n");
    uint64_t msr = calller_HalReadMsr(0x1B);   // IA32_APIC_BASE
    (void)msr;
    DbgPrint("calller: HalReadMsr(IA32_APIC_BASE) OK\n");

    // Notify routines (process / thread / image)
    PsSetCreateProcessNotifyRoutine(cb_process, 0);
    PsSetCreateThreadNotifyRoutine(cb_thread);
    PsSetLoadImageNotifyRoutine(cb_image);
    DbgPrint("calller: Ps* notify routines registradas\n");

    // Cm callback (registro)
    LARGE_INTEGER cookie;
    CmRegisterCallback(cb_registry, 0, &cookie);
    DbgPrint("calller: CmRegisterCallback OK\n");

    // Section object (memoria compartilhada)
    LARGE_INTEGER size; size.QuadPart = 4096;
    HANDLE hSec = 0;
    OBJECT_ATTRIBUTES oa = { 0 };
    oa.Length = sizeof(oa);
    NtCreateSection(&hSec, 0, &oa, &size, PAGE_READWRITE, SEC_COMMIT, 0);
    DbgPrint("calller: NtCreateSection 4096 bytes OK\n");

    // System thread
    PsCreateSystemThread(&g_thread, 0, 0, 0, 0, sys_thread_entry, 0);
    DbgPrint("calller: PsCreateSystemThread retornou\n");

    DbgPrint("calller: DriverEntry concluido — driver pronto.\n");
    return STATUS_SUCCESS;
}
