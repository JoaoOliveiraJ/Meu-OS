// Driver de kernel (.sys) que cria um dispositivo e responde a um IOCTL.
// Registra dispatch routines (IRP_MJ_CREATE / CLOSE / DEVICE_CONTROL).
#include "ntddk.h"

unsigned int _tls_index = 0;

#define IOCTL_GET_MAGIC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, 0)

__declspec(dllimport) ULONG    DbgPrint(const char* Format, ...);
__declspec(dllimport) NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING,
                                              ULONG, ULONG, BOOLEAN, PDEVICE_OBJECT*);
__declspec(dllimport) void     RtlInitUnicodeString(PUNICODE_STRING, const WCHAR*);

static NTSTATUS DispatchCreate(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    DbgPrint("IoctlDrv: IRP_MJ_CREATE (open)\n");
    irp->IoStatus.Status = STATUS_SUCCESS; irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}
static NTSTATUS DispatchClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    DbgPrint("IoctlDrv: IRP_MJ_CLOSE\n");
    irp->IoStatus.Status = STATUS_SUCCESS; irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}
static NTSTATUS DispatchIoctl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION s = irp->CurrentStack;
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    DbgPrint("IoctlDrv: IRP_MJ_DEVICE_CONTROL recebido\n");
    if (code == IOCTL_GET_MAGIC) {
        unsigned* out = (unsigned*)irp->SystemBuffer;   // METHOD_BUFFERED
        if (out) out[0] = 0xCAFEBABE;
        irp->IoStatus.Information = 4;
        irp->IoStatus.Status = STATUS_SUCCESS;
        DbgPrint("IoctlDrv: respondeu 0xCAFEBABE\n");
        return STATUS_SUCCESS;
    }
    irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    irp->IoStatus.Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg) {
    (void)reg;
    DbgPrint("IoctlDrv: DriverEntry\n");
    drv->MajorFunction[IRP_MJ_CREATE]         = (PVOID)DispatchCreate;
    drv->MajorFunction[IRP_MJ_CLOSE]          = (PVOID)DispatchClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = (PVOID)DispatchIoctl;

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\Device\\MeuDispositivo");
    PDEVICE_OBJECT dev = 0;
    NTSTATUS st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN, 0, 0, &dev);
    DbgPrint(st == STATUS_SUCCESS
        ? "IoctlDrv: \\Device\\MeuDispositivo criado\n"
        : "IoctlDrv: falha ao criar dispositivo\n");
    return STATUS_SUCCESS;
}
