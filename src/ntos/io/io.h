#pragma once
#include "ntddk.h"

// Objeto de arquivo: o que um HANDLE de NtCreateFile referencia. Pode apontar
// para um DEVICE_OBJECT (driver) OU, no caso de Named Pipe, para um PIPE_OBJECT
// (nt/pipe.c). 'PipeObject' e void* para evitar dependencia ciclica de headers;
// quando != 0, NtReadFile/NtWriteFile roteiam para o buffer do pipe.
typedef struct _FILE_OBJECT {
    PDEVICE_OBJECT DeviceObject;
    void*          PipeObject;        // PPIPE_OBJECT se for um handle de pipe; senao 0
    uint64_t       FsContext;         // FS-especifico: p/ o NTFS, o numero do registro MFT
    uint64_t       CurrentByteOffset; // posicao de leitura corrente (NtReadFile sequencial)
    uint32_t       IsDirectory;       // 1 se o handle abriu um diretorio (NtQueryDirectoryFile)
} FILE_OBJECT, *PFILE_OBJECT;

// I/O Manager — funcoes "do MeuOS" (nomes internos).
NTSTATUS io_create_device(PDRIVER_OBJECT drv, ULONG ext_size, const char* name,
                          ULONG type, PDEVICE_OBJECT* out);
PIRP     io_build_ioctl(ULONG ioctl, PDEVICE_OBJECT dev,
                        void* in_buf, ULONG in_len, void* out_buf, ULONG out_len);
PIRP     io_build_write(PDEVICE_OBJECT dev, void* buf, ULONG len);   // IRP_MJ_WRITE
PIRP     io_build_read(PDEVICE_OBJECT dev, void* buf, ULONG len);    // IRP_MJ_READ
PIRP     io_build_request(uint8_t major, PDEVICE_OBJECT dev);   // IRP_MJ_CREATE/CLOSE/etc.
NTSTATUS IoCallDriver(PDEVICE_OBJECT dev, PIRP irp);

// FASE FUNDACAO (trilha I/O, Fase 2) — device stacks.
PDEVICE_OBJECT NTAPI IoGetAttachedDevice_k(PDEVICE_OBJECT dev);
PDEVICE_OBJECT NTAPI IoGetAttachedDeviceReference_k(PDEVICE_OBJECT dev);
PDEVICE_OBJECT NTAPI IoAttachDeviceToDeviceStack_k(PDEVICE_OBJECT Source, PDEVICE_OBJECT Target);
NTSTATUS       NTAPI IoAttachDeviceToDeviceStackSafe_k(PDEVICE_OBJECT Source, PDEVICE_OBJECT Target, PDEVICE_OBJECT* AttachedTo);
void           NTAPI IoDetachDevice_k(PDEVICE_OBJECT Target);
PDEVICE_OBJECT NTAPI IoGetLowerDeviceObject_k(PDEVICE_OBJECT dev);
void KiDeviceStackSelfTest(void);
void KiIrpSelfTest(void);
void KiDriverIrpSelfTest(void);
void KiCompletionSelfTest(void);   // INC 4: walk de conclusao em 2 niveis
// Exercita I/O real (WRITE+READ) no device que 'drv' criou (drv->DeviceObject).
struct _DRIVER_OBJECT;
void KiExerciseDriverIO(struct _DRIVER_OBJECT* drv);
// Idem, mas localizando o device por nome no namespace (\Device\...).
void KiExerciseDeviceIO(const char* devname);
void     io_free_irp(PIRP irp);

// =====================================================================
//  FASE 7 — Driver Framework: rotinas de suporte do I/O Manager (NT WDM).
//
//  Drivers chamam IoCompleteRequest no final de cada IRP, e
//  IoGetCurrentIrpStackLocation pra ler o StackLocation atual. Sao
//  declaradas com __attribute__((ms_abi)) p/ casar com a ABI do WDM.
// =====================================================================

// Aloca um IRP com 'StackSize' niveis (no NT a IRP tem array variavel; aqui
// usamos um nivel — basta p/ drivers que nao encadeiam IRPs).
PIRP   NTAPI IoAllocateIrp_k(uint8_t StackSize, BOOLEAN ChargeQuota);
void   NTAPI IoFreeIrp_k(PIRP Irp);
void   NTAPI IoInitializeIrp_k(PIRP Irp, USHORT PacketSize, uint8_t StackSize);
PIO_STACK_LOCATION NTAPI IoGetCurrentIrpStackLocation_k(PIRP Irp);
PIO_STACK_LOCATION NTAPI IoGetNextIrpStackLocation_k(PIRP Irp);
void   NTAPI IoSkipCurrentIrpStackLocation_k(PIRP Irp);
void   NTAPI IoCopyCurrentIrpStackLocationToNext_k(PIRP Irp);
void   NTAPI IoSetCompletionRoutine_k(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context,
                                      BOOLEAN OnSuccess, BOOLEAN OnError, BOOLEAN OnCancel);
NTSTATUS NTAPI IoCallDriver_ms(PDEVICE_OBJECT dev, PIRP irp);
void   NTAPI IoCompleteRequest_k(PIRP Irp, uint8_t PriorityBoost);
BOOLEAN NTAPI IoCancelIrp_k(PIRP Irp);
PIRP   NTAPI IoBuildAsynchronousFsdRequest_k(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject,
                                             PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
                                             PIO_STATUS_BLOCK IoStatusBlock);
PIRP   NTAPI IoBuildDeviceIoControlRequest_k(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject,
                                             PVOID InputBuffer, ULONG InputBufferLength,
                                             PVOID OutputBuffer, ULONG OutputBufferLength,
                                             BOOLEAN InternalDeviceIoControl, PVOID Event,
                                             PIO_STATUS_BLOCK IoStatusBlock);
NTSTATUS NTAPI IoCreateDevice_k(PDRIVER_OBJECT drv, ULONG ext, PUNICODE_STRING name,
                                ULONG type, ULONG chars, BOOLEAN excl, PDEVICE_OBJECT* out);
void   NTAPI IoDeleteDevice_k(PDEVICE_OBJECT dev);
NTSTATUS NTAPI IoCreateSymbolicLink_k(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
NTSTATUS NTAPI IoDeleteSymbolicLink_k(PUNICODE_STRING SymbolicLinkName);
NTSTATUS NTAPI IoGetDeviceObjectPointer_k(PUNICODE_STRING ObjectName, ACCESS_MASK DesiredAccess,
                                          PFILE_OBJECT* FileObject, PDEVICE_OBJECT* DeviceObject);
void   NTAPI IoReleaseRemoveLockAndWait_k(PVOID Lock, PVOID Tag);

// Symbolic link helpers (ASCII) — implementacao interna.
int io_symlink_create_ascii(const char* link, const char* target);
const char* io_symlink_resolve_ascii(const char* link);
