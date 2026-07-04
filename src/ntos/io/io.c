#include <stdint.h>
#include "io/io.h"
#include "ob/object.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern int  ke_legacy_active(void);   // trilha I/O Fase 2: gate p/ pintok (legado -> stub)

// Cria um DEVICE_OBJECT como objeto nomeado no namespace (\Device\...).
NTSTATUS io_create_device(PDRIVER_OBJECT drv, ULONG ext_size, const char* name,
                          ULONG type, PDEVICE_OBJECT* out) {
    PDEVICE_OBJECT dev = (PDEVICE_OBJECT)ObCreateObject(
        OB_TYPE_DEVICE, sizeof(DEVICE_OBJECT) + ext_size, name);
    if (!dev) return STATUS_UNSUCCESSFUL;

    dev->Type = 3;
    dev->Size = (SHORT)sizeof(DEVICE_OBJECT);
    dev->DriverObject = drv;
    dev->DeviceType = type;            // Fase 1a: era 'dev->Flags = type' (bug: Flags != DeviceType)
    dev->StackSize  = 1;               // dispositivo nao-anexado: 1 nivel
    dev->DeviceExtension = ext_size ? (void*)(dev + 1) : 0;
    if (drv) { dev->NextDevice = drv->DeviceObject; drv->DeviceObject = dev; }  // cadeia do driver

    if (out) *out = dev;
    return STATUS_SUCCESS;
}

// FASE FUNDACAO (trilha I/O, Fase 1b) — aloca um IRP (layout NT) com (sc+1)
// IO_STACK_LOCATIONs no array traseiro. CurrentStackLocation comeca no sentinela;
// o build preenche a "next" (topo) e o 1o IoCallDriver avanca (a next vira current).
static PIRP io_alloc_irp(uint8_t stack_count) {
    uint8_t sc = stack_count ? stack_count : 1;
    uint64_t sz = sizeof(IRP) + (uint64_t)(sc + 1) * sizeof(IO_STACK_LOCATION);
    PIRP irp = (PIRP)kmalloc(sz);
    if (!irp) return 0;
    for (uint64_t i = 0; i < sz; i++) ((uint8_t*)irp)[i] = 0;
    irp->Type = 6;                       // IO_TYPE_IRP
    irp->Size = (USHORT)sz;
    irp->StackCount = (signed char)sc;
    irp->CurrentLocation = (signed char)(sc + 1);
    irp->Tail.CurrentStackLocation = (PIO_STACK_LOCATION)(irp + 1) + sc;   // sentinela
    return irp;
}

// Monta um IRP de IOCTL (METHOD_BUFFERED): AssociatedIrp.SystemBuffer = entrada.
PIRP io_build_ioctl(ULONG ioctl, PDEVICE_OBJECT dev,
                    void* in_buf, ULONG in_len, void* out_buf, ULONG out_len) {
    PIRP irp = io_alloc_irp(1);
    if (!irp) return 0;
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    s->DeviceObject  = dev;
    s->Parameters.DeviceIoControl.IoControlCode      = ioctl;
    s->Parameters.DeviceIoControl.InputBufferLength  = in_len;
    s->Parameters.DeviceIoControl.OutputBufferLength = out_len;

    ULONG buf = in_len > out_len ? in_len : out_len;
    if (buf) {
        irp->AssociatedIrp.SystemBuffer = kmalloc(buf);
        for (ULONG i = 0; i < in_len; i++)
            ((uint8_t*)irp->AssociatedIrp.SystemBuffer)[i] = ((uint8_t*)in_buf)[i];
    }
    irp->UserBuffer = out_buf;
    return irp;
}

// Monta um IRP de WRITE (METHOD_BUFFERED): copia 'len' bytes do buffer do
// usuario para o SystemBuffer e os entrega ao driver. Parameters.Write.Length.
PIRP io_build_write(PDEVICE_OBJECT dev, void* buf, ULONG len) {
    PIRP irp = io_alloc_irp(1);
    if (!irp) return 0;
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = IRP_MJ_WRITE;
    s->DeviceObject  = dev;
    s->Parameters.Write.Length = len;
    if (len) {
        irp->AssociatedIrp.SystemBuffer = kmalloc(len);
        if (irp->AssociatedIrp.SystemBuffer && buf)
            for (ULONG i = 0; i < len; i++)
                ((uint8_t*)irp->AssociatedIrp.SystemBuffer)[i] = ((uint8_t*)buf)[i];
    }
    return irp;
}

// Monta um IRP de READ (METHOD_BUFFERED): o driver preenche o SystemBuffer e
// reporta os bytes lidos em IoStatus.Information. Parameters.Read.Length.
PIRP io_build_read(PDEVICE_OBJECT dev, void* buf, ULONG len) {
    PIRP irp = io_alloc_irp(1);
    if (!irp) return 0;
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = IRP_MJ_READ;
    s->DeviceObject  = dev;
    s->Parameters.Read.Length = len;
    if (len) irp->AssociatedIrp.SystemBuffer = kmalloc(len);
    irp->UserBuffer = buf;
    return irp;
}

// IRP simples (sem buffers) para IRP_MJ_CREATE / IRP_MJ_CLOSE / IRP_MJ_CLEANUP / etc.
PIRP io_build_request(uint8_t major, PDEVICE_OBJECT dev) {
    PIRP irp = io_alloc_irp(1);
    if (!irp) return 0;
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = major;
    s->DeviceObject  = dev;
    return irp;
}

// Chama o dispatch do driver para o MajorFunction do IRP (ABI Microsoft).
typedef NTSTATUS (__attribute__((ms_abi)) *DISPATCH_MS)(PDEVICE_OBJECT, PIRP);

NTSTATUS IoCallDriver(PDEVICE_OBJECT dev, PIRP irp) {
    PDRIVER_OBJECT drv = (PDRIVER_OBJECT)dev->DriverObject;
    IoSetNextIrpStackLocation(irp);              // avanca: a "next" preenchida vira "current"
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    s->DeviceObject = dev;
    uint8_t mj = s->MajorFunction;
    DISPATCH_MS fn = (DISPATCH_MS)drv->MajorFunction[mj];
    if (!fn) { irp->IoStatus.Status = STATUS_UNSUCCESSFUL; return STATUS_UNSUCCESSFUL; }
    return fn(dev, irp);
}

void io_free_irp(PIRP irp) {
    if (!irp) return;
    if (irp->AssociatedIrp.SystemBuffer) kfree(irp->AssociatedIrp.SystemBuffer);
    kfree(irp);
}

// ============================================================================
//  FASE 7 — Rotinas de suporte do I/O Manager para drivers .sys.
// ============================================================================

PIRP NTAPI IoAllocateIrp_k(uint8_t StackSize, BOOLEAN ChargeQuota) {
    (void)ChargeQuota;
    return io_alloc_irp(StackSize);
}
void NTAPI IoFreeIrp_k(PIRP Irp) { io_free_irp(Irp); }
void NTAPI IoInitializeIrp_k(PIRP Irp, USHORT PacketSize, uint8_t StackSize) {
    if (!Irp) return;
    for (unsigned i = 0; i < PacketSize; i++) ((uint8_t*)Irp)[i] = 0;
    uint8_t sc = StackSize ? StackSize : 1;
    Irp->Type = 6;
    Irp->Size = PacketSize;
    Irp->StackCount = (signed char)sc;
    Irp->CurrentLocation = (signed char)(sc + 1);
    Irp->Tail.CurrentStackLocation = (PIO_STACK_LOCATION)(Irp + 1) + sc;
}
PIO_STACK_LOCATION NTAPI IoGetCurrentIrpStackLocation_k(PIRP Irp) { return Irp ? IoGetCurrentIrpStackLocation(Irp) : 0; }
PIO_STACK_LOCATION NTAPI IoGetNextIrpStackLocation_k(PIRP Irp) { return Irp ? IoGetNextIrpStackLocation(Irp) : 0; }
void NTAPI IoSkipCurrentIrpStackLocation_k(PIRP Irp) { if (Irp) IoSkipCurrentIrpStackLocation(Irp); }
void NTAPI IoCopyCurrentIrpStackLocationToNext_k(PIRP Irp) { if (Irp) IoCopyCurrentIrpStackLocationToNext(Irp); }
void NTAPI IoSetCompletionRoutine_k(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context,
                                    BOOLEAN OnSuccess, BOOLEAN OnError, BOOLEAN OnCancel) {
    (void)OnSuccess; (void)OnError; (void)OnCancel;
    PIO_STACK_LOCATION s = Irp ? IoGetCurrentIrpStackLocation(Irp) : 0;
    if (s) { s->CompletionRoutine = (void*)Routine; s->Context = Context; }
}
NTSTATUS NTAPI IoCallDriver_ms(PDEVICE_OBJECT dev, PIRP irp) { return IoCallDriver(dev, irp); }
void NTAPI IoCompleteRequest_k(PIRP Irp, uint8_t PriorityBoost) {
    (void)PriorityBoost;
    if (!Irp) return;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    if (s && s->CompletionRoutine) {
        PIO_COMPLETION_ROUTINE r = (PIO_COMPLETION_ROUTINE)s->CompletionRoutine;
        r(s->DeviceObject, Irp, s->Context);
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
        PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
        if (MajorFunction == IRP_MJ_WRITE) s->Parameters.Write.ByteOffset = (uint64_t)StartingOffset->QuadPart;
        else if (MajorFunction == IRP_MJ_READ) s->Parameters.Read.ByteOffset = (uint64_t)StartingOffset->QuadPart;
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

// ============================================================================
//  FASE FUNDACAO (trilha I/O, Fase 2) — device stacks.
//
//  AttachedDevice/StackSize sao campos reais (Fase 1a). O "lower device" (que um
//  driver le via IoGetLowerDeviceObject) fica numa side-table (o NT esconde no
//  DEVOBJ_EXTENSION; aqui array paralelo, mesmo padrao dos symlinks). Flag-gated:
//  no modo legado (ke_legacy_active: pintok) voltam ao ANTIGO (retorna 0/no-op,
//  como os stubs) -> trajetoria do pintok preservada. Ref-counting simplificado
//  (nao chamamos ObReferenceObject: os devices de teste sao estaticos, nao-Ob).
// ============================================================================
#define IOSTACK_MAX 64
static struct { PDEVICE_OBJECT dev; PDEVICE_OBJECT lower; int used; } s_lower[IOSTACK_MAX];
static void iostack_set_lower(PDEVICE_OBJECT dev, PDEVICE_OBJECT lower) {
    for (int i = 0; i < IOSTACK_MAX; i++) if (s_lower[i].used && s_lower[i].dev == dev) { s_lower[i].lower = lower; return; }
    for (int i = 0; i < IOSTACK_MAX; i++) if (!s_lower[i].used) { s_lower[i].dev = dev; s_lower[i].lower = lower; s_lower[i].used = 1; return; }
}
static PDEVICE_OBJECT iostack_get_lower(PDEVICE_OBJECT dev) {
    for (int i = 0; i < IOSTACK_MAX; i++) if (s_lower[i].used && s_lower[i].dev == dev) return s_lower[i].lower;
    return 0;
}
static void iostack_clear_lower(PDEVICE_OBJECT dev) {
    for (int i = 0; i < IOSTACK_MAX; i++) if (s_lower[i].used && s_lower[i].dev == dev) { s_lower[i].used = 0; return; }
}
// Topo da pilha (raw, sem gate) — usado internamente.
static PDEVICE_OBJECT io_top(PDEVICE_OBJECT dev) {
    if (!dev) return 0;
    while (dev->AttachedDevice) dev = (PDEVICE_OBJECT)dev->AttachedDevice;
    return dev;
}
PDEVICE_OBJECT NTAPI IoGetAttachedDevice_k(PDEVICE_OBJECT dev) {
    return ke_legacy_active() ? 0 : io_top(dev);
}
PDEVICE_OBJECT NTAPI IoGetAttachedDeviceReference_k(PDEVICE_OBJECT dev) {
    return ke_legacy_active() ? 0 : io_top(dev);   // ref simplificado (ver acima)
}
PDEVICE_OBJECT NTAPI IoAttachDeviceToDeviceStack_k(PDEVICE_OBJECT Source, PDEVICE_OBJECT Target) {
    if (ke_legacy_active() || !Source || !Target) return 0;   // ANTIGO: stub retornava 0
    PDEVICE_OBJECT top = io_top(Target);
    top->AttachedDevice = Source;
    Source->StackSize = (signed char)(top->StackSize + 1);
    iostack_set_lower(Source, top);
    return top;
}
NTSTATUS NTAPI IoAttachDeviceToDeviceStackSafe_k(PDEVICE_OBJECT Source, PDEVICE_OBJECT Target, PDEVICE_OBJECT* AttachedTo) {
    if (ke_legacy_active()) { if (AttachedTo) *AttachedTo = 0; return STATUS_UNSUCCESSFUL; }
    PDEVICE_OBJECT t = IoAttachDeviceToDeviceStack_k(Source, Target);
    if (AttachedTo) *AttachedTo = t;
    return t ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
void NTAPI IoDetachDevice_k(PDEVICE_OBJECT Target) {
    if (ke_legacy_active() || !Target) return;   // ANTIGO: no-op
    PDEVICE_OBJECT top = (PDEVICE_OBJECT)Target->AttachedDevice;
    Target->AttachedDevice = 0;
    if (top) iostack_clear_lower(top);
}
PDEVICE_OBJECT NTAPI IoGetLowerDeviceObject_k(PDEVICE_OBJECT dev) {
    return ke_legacy_active() ? 0 : iostack_get_lower(dev);
}

// Prova de boot: attach de 2 niveis (upper sobre lower) + checagens.
void KiDeviceStackSelfTest(void) {
    static DEVICE_OBJECT lower, upper;
    for (unsigned i = 0; i < sizeof(DEVICE_OBJECT); i++) { ((uint8_t*)&lower)[i] = 0; ((uint8_t*)&upper)[i] = 0; }
    lower.StackSize = 1;
    PDEVICE_OBJECT top      = IoAttachDeviceToDeviceStack_k(&upper, &lower);
    PDEVICE_OBJECT attached = IoGetAttachedDevice_k(&lower);
    PDEVICE_OBJECT low      = IoGetLowerDeviceObject_k(&upper);
    if (top == &lower && attached == &upper && upper.StackSize == 2 && low == &lower)
        kputs("[devstack-test] IoAttachDeviceToDeviceStack (2 niveis) OK\n");
    else
        kputs("[devstack-test] FALHOU\n");
    IoDetachDevice_k(&lower);
}

// Prova de boot (Fase 1b): build IOCTL IRP -> avanca (como IoCallDriver) ->
// le a "current" e confere os campos (layout NT: SystemBuffer em AssociatedIrp,
// stack location traseira via Tail.CurrentStackLocation).
void KiIrpSelfTest(void) {
    static int in = 0x1234, out = 0;
    PIRP irp = io_build_ioctl(0xDEAD, 0, &in, sizeof(in), &out, sizeof(out));
    if (!irp) { kputs("[irp-test] FALHOU (sem IRP)\n"); return; }
    IoSetNextIrpStackLocation(irp);                              // como o IoCallDriver faz
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    int ok = s && s->MajorFunction == IRP_MJ_DEVICE_CONTROL
             && s->Parameters.DeviceIoControl.IoControlCode == 0xDEAD
             && s->Parameters.DeviceIoControl.InputBufferLength == sizeof(in)
             && irp->AssociatedIrp.SystemBuffer
             && *(int*)irp->AssociatedIrp.SystemBuffer == 0x1234;
    if (ok) kputs("[irp-test] IRP build+advance+read (layout NT: Tail.CurrentStackLocation@0xB8) OK\n");
    else    kputs("[irp-test] FALHOU\n");
    io_free_irp(irp);
}

// Prova de boot (Fase 6): round-trip COMPLETO de IOCTL por um driver — build IRP
// -> IoCallDriver (avanca) -> dispatch le a current (MajorFunction/IoControlCode/
// SystemBuffer) e escreve a resposta -> IoCompleteRequest -> caller le o buffer.
// E' exatamente o que um driver WDM real faz ao servir um IOCTL.
static NTSTATUS __attribute__((ms_abi)) test_ioctl_dispatch(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (s && s->MajorFunction == IRP_MJ_DEVICE_CONTROL
          && s->Parameters.DeviceIoControl.IoControlCode == 0xCAFE
          && irp->AssociatedIrp.SystemBuffer) {
        *(uint32_t*)irp->AssociatedIrp.SystemBuffer = 0xF00DBEEF;   // resposta "magic"
        irp->IoStatus.Information = 4;
        irp->IoStatus.Status = STATUS_SUCCESS;
    } else {
        irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    }
    IoCompleteRequest_k(irp, 0);
    return irp->IoStatus.Status;
}
// Núcleo do teste: manda um IRP de WRITE e um de READ REAIS pro device 'dev' e
// reporta o que o driver respondeu. Prova que um driver PROCESSA IRP (roda o
// dispatch, completa o IRP com status/Information), nao so carrega.
static void exercise_one_device(PDEVICE_OBJECT dev) {
    if (!dev || !dev->DriverObject) return;
    PDRIVER_OBJECT drv = dev->DriverObject;
    int did_any = 0;
    // WRITE — so se o driver implementa IRP_MJ_WRITE (senao pularia no dispatch nulo).
    if (drv->MajorFunction[IRP_MJ_WRITE]) {
        static char wbuf[8] = { 'M','e','u','O','S','!','!', 0 };
        PIRP wirp = io_build_write(dev, wbuf, 8);
        NTSTATUS wst = (NTSTATUS)0xC0000001; uint64_t winfo = 0;
        if (wirp) { IoCallDriver(dev, wirp); wst = wirp->IoStatus.Status; winfo = wirp->IoStatus.Information; io_free_irp(wirp); }
        kputs("[real-test] WRITE 8 bytes -> status="); kput_hex((uint64_t)(uint32_t)wst);
        kputs(" info(bytes)="); kput_dec(winfo); kputs("\n");
        did_any = 1;
    }
    // READ — so se o driver implementa IRP_MJ_READ.
    if (drv->MajorFunction[IRP_MJ_READ]) {
        static char rbuf[8];
        PIRP rirp = io_build_read(dev, rbuf, 8);
        NTSTATUS rst = (NTSTATUS)0xC0000001; uint64_t rinfo = 0;
        if (rirp) { IoCallDriver(dev, rirp); rst = rirp->IoStatus.Status; rinfo = rirp->IoStatus.Information; io_free_irp(rirp); }
        kputs("[real-test] READ  8 bytes -> status="); kput_hex((uint64_t)(uint32_t)rst);
        kputs(" info(bytes)="); kput_dec(rinfo); kputs("\n");
        did_any = 1;
    }
    if (did_any)
        kputs("[real-test] >>> o driver REAL processou os IRPs (rodou o dispatch e completou) <<<\n");
    else
        kputs("[real-test] (driver nao implementa READ/WRITE — nada a exercitar por aqui)\n");
}

// Exercita I/O real no primeiro device que 'drv' criou (drv->DeviceObject = cabeça
// da lista de devices do driver, igual ao NT). GENERICO: vale p/ qualquer driver
// que criou um device (null.sys, wdmdemo, etc.). No-op se o driver nao criou device.
// Chamado logo apos o DriverEntry ter sucesso e ANTES do DriverUnload (device vivo).
void KiExerciseDriverIO(PDRIVER_OBJECT drv) {
    if (!drv || !drv->DeviceObject) return;   // driver nao criou device -> nada a testar
    kputs("\n[real-test] === exercitando I/O real no device criado por este driver ===\n");
    exercise_one_device(drv->DeviceObject);
}

// Variante por nome (\Device\...): util para exercitar um device ja existente no
// namespace. No-op se nao existe.
void KiExerciseDeviceIO(const char* devname) {
    void* body = ObLookupObject(devname);
    if (!body) return;                      // device nao existe -> nada a testar
    kputs("\n[real-test] === exercitando I/O real em '"); kputs(devname); kputs("' ===\n");
    exercise_one_device((PDEVICE_OBJECT)body);
}

void KiDriverIrpSelfTest(void) {
    static DRIVER_OBJECT drv;
    for (unsigned i = 0; i < sizeof(drv); i++) ((uint8_t*)&drv)[i] = 0;
    drv.MajorFunction[IRP_MJ_DEVICE_CONTROL] = (void*)test_ioctl_dispatch;
    PDEVICE_OBJECT dev = 0;
    io_create_device(&drv, 0, "\\Device\\ProbeIoctl", 0x22 /*FILE_DEVICE_UNKNOWN*/, &dev);
    if (!dev) { kputs("[drv-irp-test] FALHOU (sem device)\n"); return; }
    uint32_t inbuf = 0, outbuf = 0;
    PIRP irp = io_build_ioctl(0xCAFE, dev, &inbuf, 0, &outbuf, 4);
    if (!irp) { kputs("[drv-irp-test] FALHOU (sem IRP)\n"); return; }
    IoCallDriver(dev, irp);
    uint32_t result = irp->AssociatedIrp.SystemBuffer ? *(uint32_t*)irp->AssociatedIrp.SystemBuffer : 0;
    NTSTATUS st = irp->IoStatus.Status;
    if (result == 0xF00DBEEF && NT_SUCCESS(st))
        kputs("[drv-irp-test] driver dispatch de IOCTL (IoCallDriver->IoCompleteRequest) OK\n");
    else { kputs("[drv-irp-test] FALHOU result=0x"); kput_hex(result); kputs("\n"); }
    io_free_irp(irp);
}

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
