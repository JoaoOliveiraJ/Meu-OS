// Driver de KERNEL do Windows (PE32+, subsystem NATIVE).
// Importa do "ntoskrnl.exe" — resolvido pela executiva NT do MeuOS.
//   zig cc -target x86_64-windows-gnu -nostdlib -e DriverEntry
//          -Wl,--subsystem,native -o mydriver.sys ...  -lntoskrnl
#include "ntddk.h"

// Sem CRT: o linker ainda referencia este simbolo de TLS.
unsigned int _tls_index = 0;

__declspec(dllimport) ULONG    DbgPrint(const char* Format, ...);
__declspec(dllimport) void     RtlInitUnicodeString(PUNICODE_STRING Dst, const WCHAR* Src);
__declspec(dllimport) NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DevExtSize,
                                              PUNICODE_STRING DeviceName, ULONG DeviceType,
                                              ULONG DeviceCharacteristics, BOOLEAN Exclusive,
                                              PDEVICE_OBJECT* DeviceObject);

static void DriverUnload(PDRIVER_OBJECT DriverObject) {
    (void)DriverObject;
    DbgPrint("MeuDriver: DriverUnload chamado — driver descarregado.\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    (void)RegistryPath;
    DbgPrint("MeuDriver: DriverEntry — driver de kernel rodando no MeuOS!\n");

    PDEVICE_OBJECT dev = 0;
    NTSTATUS st = IoCreateDevice(DriverObject, 0, 0, FILE_DEVICE_UNKNOWN, 0, 0, &dev);
    if (st == STATUS_SUCCESS)
        DbgPrint("MeuDriver: IoCreateDevice OK — device object criado.\n");
    else
        DbgPrint("MeuDriver: IoCreateDevice FALHOU.\n");

    DriverObject->DriverUnload = (PVOID)DriverUnload;
    return STATUS_SUCCESS;
}
