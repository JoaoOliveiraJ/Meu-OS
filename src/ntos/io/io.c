#include <stdint.h>
#include "io/io.h"
#include "ob/object.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);

// Cria um DEVICE_OBJECT como objeto nomeado no namespace (\Device\...).
NTSTATUS io_create_device(PDRIVER_OBJECT drv, ULONG ext_size, const char* name,
                          ULONG type, PDEVICE_OBJECT* out) {
    PDEVICE_OBJECT dev = (PDEVICE_OBJECT)ObCreateObject(
        OB_TYPE_DEVICE, sizeof(DEVICE_OBJECT) + ext_size, name);
    if (!dev) return STATUS_UNSUCCESSFUL;

    dev->Type = 3;
    dev->Size = (SHORT)sizeof(DEVICE_OBJECT);
    dev->DriverObject = drv;
    dev->Flags = type;
    dev->DeviceExtension = ext_size ? (void*)(dev + 1) : 0;
    if (drv) { dev->NextDevice = drv->DeviceObject; drv->DeviceObject = dev; }  // cadeia do driver

    if (out) *out = dev;
    return STATUS_SUCCESS;
}

// Monta um IRP de IOCTL (METHOD_BUFFERED): SystemBuffer recebe a entrada.
PIRP io_build_ioctl(ULONG ioctl, PDEVICE_OBJECT dev,
                    void* in_buf, ULONG in_len, void* out_buf, ULONG out_len) {
    PIRP irp = (PIRP)kmalloc(sizeof(IRP));
    if (!irp) return 0;
    for (unsigned i = 0; i < sizeof(IRP); i++) ((uint8_t*)irp)[i] = 0;

    irp->CurrentStack = &irp->StackLocation;
    irp->StackCount = 1;
    PIO_STACK_LOCATION s = irp->CurrentStack;
    s->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    s->DeviceObject  = dev;
    s->Parameters.DeviceIoControl.IoControlCode     = ioctl;
    s->Parameters.DeviceIoControl.InputBufferLength  = in_len;
    s->Parameters.DeviceIoControl.OutputBufferLength = out_len;

    ULONG buf = in_len > out_len ? in_len : out_len;
    if (buf) {
        irp->SystemBuffer = kmalloc(buf);
        for (ULONG i = 0; i < in_len; i++)
            ((uint8_t*)irp->SystemBuffer)[i] = ((uint8_t*)in_buf)[i];
    }
    irp->UserBuffer = out_buf;
    return irp;
}

// Monta um IRP de WRITE (METHOD_BUFFERED): copia 'len' bytes do buffer do
// usuario para o SystemBuffer e os entrega ao driver. Parameters.Write.Length.
PIRP io_build_write(PDEVICE_OBJECT dev, void* buf, ULONG len) {
    PIRP irp = (PIRP)kmalloc(sizeof(IRP));
    if (!irp) return 0;
    for (unsigned i = 0; i < sizeof(IRP); i++) ((uint8_t*)irp)[i] = 0;

    irp->CurrentStack = &irp->StackLocation;
    irp->StackCount = 1;
    PIO_STACK_LOCATION s = irp->CurrentStack;
    s->MajorFunction = IRP_MJ_WRITE;
    s->DeviceObject  = dev;
    s->Parameters.Write.Length = len;

    if (len) {
        irp->SystemBuffer = kmalloc(len);
        if (irp->SystemBuffer && buf)
            for (ULONG i = 0; i < len; i++)
                ((uint8_t*)irp->SystemBuffer)[i] = ((uint8_t*)buf)[i];
    }
    return irp;
}

// Monta um IRP de READ (METHOD_BUFFERED): o driver preenche o SystemBuffer e
// reporta os bytes lidos em IoStatus.Information. Parameters.Read.Length.
PIRP io_build_read(PDEVICE_OBJECT dev, void* buf, ULONG len) {
    PIRP irp = (PIRP)kmalloc(sizeof(IRP));
    if (!irp) return 0;
    for (unsigned i = 0; i < sizeof(IRP); i++) ((uint8_t*)irp)[i] = 0;

    irp->CurrentStack = &irp->StackLocation;
    irp->StackCount = 1;
    PIO_STACK_LOCATION s = irp->CurrentStack;
    s->MajorFunction = IRP_MJ_READ;
    s->DeviceObject  = dev;
    s->Parameters.Read.Length = len;

    if (len) irp->SystemBuffer = kmalloc(len);
    irp->UserBuffer = buf;
    return irp;
}

// IRP simples (sem buffers) para IRP_MJ_CREATE / IRP_MJ_CLOSE / IRP_MJ_CLEANUP / etc.
PIRP io_build_request(uint8_t major, PDEVICE_OBJECT dev) {
    PIRP irp = (PIRP)kmalloc(sizeof(IRP));
    if (!irp) return 0;
    for (unsigned i = 0; i < sizeof(IRP); i++) ((uint8_t*)irp)[i] = 0;
    irp->CurrentStack = &irp->StackLocation;
    irp->StackCount = 1;
    irp->CurrentStack->MajorFunction = major;
    irp->CurrentStack->DeviceObject  = dev;
    return irp;
}

// Chama o dispatch do driver para o MajorFunction do IRP (ABI Microsoft).
typedef NTSTATUS (__attribute__((ms_abi)) *DISPATCH_MS)(PDEVICE_OBJECT, PIRP);

NTSTATUS IoCallDriver(PDEVICE_OBJECT dev, PIRP irp) {
    PDRIVER_OBJECT drv = (PDRIVER_OBJECT)dev->DriverObject;
    uint8_t mj = irp->CurrentStack->MajorFunction;
    DISPATCH_MS fn = (DISPATCH_MS)drv->MajorFunction[mj];
    if (!fn) { irp->IoStatus.Status = STATUS_UNSUCCESSFUL; return STATUS_UNSUCCESSFUL; }
    irp->CurrentStack->DeviceObject = dev;
    return fn(dev, irp);
}

void io_free_irp(PIRP irp) {
    if (!irp) return;
    if (irp->SystemBuffer) kfree(irp->SystemBuffer);
    kfree(irp);
}

// ============================================================================
//  FASE 7 — Rotinas de suporte do I/O Manager para drivers .sys.
// ============================================================================

PIRP NTAPI IoAllocateIrp_k(uint8_t StackSize, BOOLEAN ChargeQuota) {
    (void)StackSize; (void)ChargeQuota;
    PIRP irp = (PIRP)kmalloc(sizeof(IRP));
    if (!irp) return 0;
    for (unsigned i = 0; i < sizeof(IRP); i++) ((uint8_t*)irp)[i] = 0;
    irp->CurrentStack = &irp->StackLocation;
    irp->StackCount = StackSize ? StackSize : 1;
    return irp;
}
void NTAPI IoFreeIrp_k(PIRP Irp) { io_free_irp(Irp); }
void NTAPI IoInitializeIrp_k(PIRP Irp, USHORT PacketSize, uint8_t StackSize) {
    if (!Irp) return;
    for (unsigned i = 0; i < PacketSize && i < sizeof(IRP); i++) ((uint8_t*)Irp)[i] = 0;
    Irp->CurrentStack = &Irp->StackLocation;
    Irp->StackCount = StackSize ? StackSize : 1;
}
PIO_STACK_LOCATION NTAPI IoGetCurrentIrpStackLocation_k(PIRP Irp) { return Irp ? Irp->CurrentStack : 0; }
PIO_STACK_LOCATION NTAPI IoGetNextIrpStackLocation_k(PIRP Irp) { return Irp ? Irp->CurrentStack : 0; }
void NTAPI IoSkipCurrentIrpStackLocation_k(PIRP Irp) { (void)Irp; }
void NTAPI IoCopyCurrentIrpStackLocationToNext_k(PIRP Irp) { (void)Irp; }
void NTAPI IoSetCompletionRoutine_k(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context,
                                    BOOLEAN OnSuccess, BOOLEAN OnError, BOOLEAN OnCancel) {
    (void)OnSuccess; (void)OnError; (void)OnCancel;
    if (Irp && Irp->CurrentStack) {
        Irp->CurrentStack->CompletionRoutine = (void*)Routine;
        Irp->CurrentStack->Context = Context;
    }
}
NTSTATUS NTAPI IoCallDriver_ms(PDEVICE_OBJECT dev, PIRP irp) { return IoCallDriver(dev, irp); }
void NTAPI IoCompleteRequest_k(PIRP Irp, uint8_t PriorityBoost) {
    (void)PriorityBoost;
    if (!Irp) return;
    // Dispara CompletionRoutine se houver.
    if (Irp->CurrentStack && Irp->CurrentStack->CompletionRoutine) {
        PIO_COMPLETION_ROUTINE r = (PIO_COMPLETION_ROUTINE)Irp->CurrentStack->CompletionRoutine;
        r(Irp->CurrentStack->DeviceObject, Irp, Irp->CurrentStack->Context);
    }
}
BOOLEAN NTAPI IoCancelIrp_k(PIRP Irp) {
    if (!Irp) return 0;
    Irp->Cancel = 1;
    Irp->IoStatus.Status = STATUS_ACCESS_DENIED;
    return 1;
}
PIRP NTAPI IoBuildAsynchronousFsdRequest_k(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject,
                                           PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
                                           PIO_STATUS_BLOCK IoStatusBlock) {
    PIRP irp;
    if (MajorFunction == IRP_MJ_WRITE) irp = io_build_write(DeviceObject, Buffer, Length);
    else if (MajorFunction == IRP_MJ_READ) irp = io_build_read(DeviceObject, Buffer, Length);
    else irp = io_build_request((uint8_t)MajorFunction, DeviceObject);
    if (irp && StartingOffset) {
        if (MajorFunction == IRP_MJ_WRITE) irp->CurrentStack->Parameters.Write.ByteOffset = (uint64_t)StartingOffset->QuadPart;
        else if (MajorFunction == IRP_MJ_READ) irp->CurrentStack->Parameters.Read.ByteOffset = (uint64_t)StartingOffset->QuadPart;
    }
    if (IoStatusBlock) irp->IoStatus = *IoStatusBlock;
    return irp;
}
PIRP NTAPI IoBuildDeviceIoControlRequest_k(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject,
                                           PVOID InputBuffer, ULONG InputBufferLength,
                                           PVOID OutputBuffer, ULONG OutputBufferLength,
                                           BOOLEAN InternalDeviceIoControl, PVOID Event,
                                           PIO_STATUS_BLOCK IoStatusBlock) {
    (void)InternalDeviceIoControl; (void)Event;
    PIRP irp = io_build_ioctl(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
                              OutputBuffer, OutputBufferLength);
    if (irp && IoStatusBlock) irp->IoStatus = *IoStatusBlock;
    return irp;
}

// Encaminhadores ms_abi para io_create_device/io_delete_device.
static void uni_to_ascii(PUNICODE_STRING u, char* out, int max) {
    int n = (u && u->Buffer) ? (u->Length / 2) : 0;
    int i = 0;
    for (; i < n && i < max - 1; i++) out[i] = (char)(u->Buffer[i] & 0xFF);
    out[i] = 0;
}
NTSTATUS NTAPI IoCreateDevice_k(PDRIVER_OBJECT drv, ULONG ext, PUNICODE_STRING name,
                                ULONG type, ULONG chars, BOOLEAN excl, PDEVICE_OBJECT* out) {
    (void)chars; (void)excl;
    char nm[64];
    uni_to_ascii(name, nm, sizeof(nm));
    return io_create_device(drv, ext, nm[0] ? nm : 0, type, out);
}
void NTAPI IoDeleteDevice_k(PDEVICE_OBJECT dev) { (void)dev; }

// ===== Symbolic links =====
#define SYMLINK_MAX 32
typedef struct { char link[64]; char target[64]; int used; } SYMLINK_ENT;
static SYMLINK_ENT s_links[SYMLINK_MAX];

int io_symlink_create_ascii(const char* link, const char* target) {
    if (!link || !target) return 0;
    // Substitui se ja existe.
    for (int i = 0; i < SYMLINK_MAX; i++) {
        if (s_links[i].used) {
            int eq = 1, j = 0;
            while (link[j] && s_links[i].link[j] && eq) { if (link[j] != s_links[i].link[j]) eq = 0; j++; }
            if (eq && link[j] == 0 && s_links[i].link[j] == 0) {
                int k = 0; while (target[k] && k < 63) { s_links[i].target[k] = target[k]; k++; }
                s_links[i].target[k] = 0;
                return 1;
            }
        }
    }
    for (int i = 0; i < SYMLINK_MAX; i++) if (!s_links[i].used) {
        int j = 0; while (link[j] && j < 63) { s_links[i].link[j] = link[j]; j++; } s_links[i].link[j] = 0;
        int k = 0; while (target[k] && k < 63) { s_links[i].target[k] = target[k]; k++; } s_links[i].target[k] = 0;
        s_links[i].used = 1;
        kputs("[io ] symlink: '"); kputs(link); kputs("' -> '"); kputs(target); kputs("'\n");
        return 1;
    }
    return 0;
}
const char* io_symlink_resolve_ascii(const char* link) {
    if (!link) return 0;
    for (int i = 0; i < SYMLINK_MAX; i++) if (s_links[i].used) {
        int eq = 1, j = 0;
        while (link[j] && s_links[i].link[j] && eq) { if (link[j] != s_links[i].link[j]) eq = 0; j++; }
        if (eq && link[j] == 0 && s_links[i].link[j] == 0) return s_links[i].target;
    }
    return 0;
}
NTSTATUS NTAPI IoCreateSymbolicLink_k(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName) {
    char sl[64], dn[64];
    uni_to_ascii(SymbolicLinkName, sl, sizeof(sl));
    uni_to_ascii(DeviceName,       dn, sizeof(dn));
    return io_symlink_create_ascii(sl, dn) ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
NTSTATUS NTAPI IoDeleteSymbolicLink_k(PUNICODE_STRING SymbolicLinkName) {
    char sl[64];
    uni_to_ascii(SymbolicLinkName, sl, sizeof(sl));
    for (int i = 0; i < SYMLINK_MAX; i++) if (s_links[i].used) {
        int eq = 1, j = 0;
        while (sl[j] && s_links[i].link[j] && eq) { if (sl[j] != s_links[i].link[j]) eq = 0; j++; }
        if (eq && sl[j] == 0 && s_links[i].link[j] == 0) { s_links[i].used = 0; return STATUS_SUCCESS; }
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
NTSTATUS NTAPI IoGetDeviceObjectPointer_k(PUNICODE_STRING ObjectName, ACCESS_MASK DesiredAccess,
                                          PFILE_OBJECT* FileObject, PDEVICE_OBJECT* DeviceObject) {
    (void)DesiredAccess;
    char nm[64]; uni_to_ascii(ObjectName, nm, sizeof(nm));
    // Resolve symlink se aplicavel.
    const char* target = io_symlink_resolve_ascii(nm);
    void* body = ObLookupObject(target ? target : nm);
    if (!body) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (DeviceObject) *DeviceObject = (PDEVICE_OBJECT)body;
    if (FileObject)   *FileObject   = 0;
    return STATUS_SUCCESS;
}
void NTAPI IoReleaseRemoveLockAndWait_k(PVOID Lock, PVOID Tag) { (void)Lock; (void)Tag; }
