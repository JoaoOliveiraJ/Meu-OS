// ============================================================================
//  wdmdemo.sys — driver WDM REAL que exercita a stack do MeuOS: device + symlink,
//  IOCTL (IRP com layout NT), system thread, KTIMER + wait BLOQUEANTE real,
//  fast mutex, DPC. (DMA e KeStall ja sao provados pelos kernel self-tests e
//  nao estao na import lib do MinGW, entao ficam fora deste driver.)
//
//  O DriverEntry roda sob o tracer do kernel (modo legado); o trabalho "real"
//  roda numa SYSTEM THREAD, DEPOIS do DriverEntry retornar (kernel em modo real).
//  Carregue SOZINHO: run.ps1 -Modules build\wdmdemo.sys
// ============================================================================
#include "ntddk.h"

unsigned int _tls_index = 0;

#define IOCTL_DEMO_MAGIC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, 0)

__declspec(dllimport) ULONG    DbgPrint(const char* Format, ...);
__declspec(dllimport) NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, ULONG, ULONG, BOOLEAN, PDEVICE_OBJECT*);
__declspec(dllimport) NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING);
__declspec(dllimport) void     RtlInitUnicodeString(PUNICODE_STRING, const WCHAR*);
__declspec(dllimport) void     KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN);
__declspec(dllimport) void     KeInitializeTimer(PKTIMER);
__declspec(dllimport) BOOLEAN  KeSetTimer(PKTIMER, LARGE_INTEGER, PKDPC);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(PVOID, int, char, BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) void     ExAcquireFastMutex(PFAST_MUTEX);
__declspec(dllimport) void     ExReleaseFastMutex(PFAST_MUTEX);
__declspec(dllimport) void     KeInitializeDpc(PKDPC, PVOID, PVOID);
__declspec(dllimport) BOOLEAN  KeInsertQueueDpc(PKDPC, PVOID, PVOID);
__declspec(dllimport) NTSTATUS PsCreateSystemThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PCLIENT_ID, PKSTART_ROUTINE, PVOID);
__declspec(dllimport) NTSTATUS PsTerminateSystemThread(NTSTATUS);

static volatile int g_dpc_flag = 0;
static void demo_dpc(PKDPC d, PVOID ctx, PVOID a1, PVOID a2) { (void)d;(void)ctx;(void)a1;(void)a2; g_dpc_flag = 0xD9C; }

static void demo_worker(PVOID ctx) {
    (void)ctx;
    DbgPrint("wdmdemo: worker rodando (modo REAL, pos-DriverEntry)\n");

    // 1) KTIMER + wait BLOQUEANTE real (bloqueia ate o timer expirar).
    static KTIMER tmr;
    KeInitializeTimer(&tmr);
    LARGE_INTEGER due; due.QuadPart = -10000000LL;   // 1 s relativo (100ns)
    KeSetTimer(&tmr, due, 0);
    DbgPrint("wdmdemo: KeWaitForSingleObject num timer 1s (bloqueando)...\n");
    KeWaitForSingleObject(&tmr, 0, 0, 0, 0);
    DbgPrint("wdmdemo: [OK] timer expirou + acordou (KTIMER + wait bloqueante REAIS)\n");

    // 2) Fast mutex (init inline — ExInitializeFastMutex e' macro no WDK).
    static FAST_MUTEX fm;
    fm.Count = 1; fm.Owner = 0; fm.Contention = 0;
    KeInitializeEvent(&fm.Event, SynchronizationEvent, 0);
    ExAcquireFastMutex(&fm);
    ExReleaseFastMutex(&fm);
    DbgPrint("wdmdemo: [OK] fast mutex acquire/release\n");

    // 3) DPC.
    static KDPC dpc;
    g_dpc_flag = 0;
    KeInitializeDpc(&dpc, (PVOID)demo_dpc, 0);
    KeInsertQueueDpc(&dpc, 0, 0);
    DbgPrint(g_dpc_flag == 0xD9C ? "wdmdemo: [OK] DPC disparou\n" : "wdmdemo: [X] DPC falhou\n");

    DbgPrint("wdmdemo: ===== driver WDM real exercitou timer/wait/mutex/DPC do MeuOS =====\n");
    PsTerminateSystemThread(0);
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS; irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}
static NTSTATUS DispatchIoctl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);   // macro inline (layout NT)
    if (s->Parameters.DeviceIoControl.IoControlCode == IOCTL_DEMO_MAGIC) {
        unsigned* out = (unsigned*)irp->AssociatedIrp.SystemBuffer;
        if (out) out[0] = 0x1337C0DE;
        irp->IoStatus.Information = 4;
        irp->IoStatus.Status = STATUS_SUCCESS;
        DbgPrint("wdmdemo: IOCTL respondeu 0x1337C0DE\n");
        return STATUS_SUCCESS;
    }
    irp->IoStatus.Status = STATUS_UNSUCCESSFUL; irp->IoStatus.Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg) {
    (void)reg;
    DbgPrint("wdmdemo: DriverEntry — driver WDM real rodando no MeuOS\n");
    drv->MajorFunction[IRP_MJ_CREATE]         = (PVOID)DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = (PVOID)DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = (PVOID)DispatchIoctl;

    UNICODE_STRING dn, sl;
    RtlInitUnicodeString(&dn, L"\\Device\\WdmDemo");
    RtlInitUnicodeString(&sl, L"\\DosDevices\\WdmDemo");
    PDEVICE_OBJECT dev = 0;
    IoCreateDevice(drv, 0, &dn, FILE_DEVICE_UNKNOWN, 0, 0, &dev);
    IoCreateSymbolicLink(&sl, &dn);
    DbgPrint("wdmdemo: device + symlink criados; disparando system thread (trabalho real)\n");

    HANDLE h = 0;
    PsCreateSystemThread(&h, 0, 0, 0, 0, (PKSTART_ROUTINE)demo_worker, 0);
    return STATUS_SUCCESS;
}
