// Executiva NT (mini "ntoskrnl.exe"): as APIs de kernel que os drivers .sys
// importam. Tudo em ABI da Microsoft (ms_abi), pois os drivers sao PE Windows.
#include "ntddk.h"
#include "nt/ntexec.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);

// --- Ke / Rtl ---
__attribute__((ms_abi)) static ULONG NT_DbgPrint(const char* Format, ...) {
    // (formatacao com %s/%d ainda nao implementada; imprime o texto literal)
    kputs("  [DbgPrint] ");
    kputs(Format);
    return 0;
}

__attribute__((ms_abi)) static void NT_RtlInitUnicodeString(PUNICODE_STRING dst, const WCHAR* src) {
    USHORT n = 0;
    if (src) while (src[n]) n++;
    if (dst) {
        dst->Buffer = (WCHAR*)src;
        dst->Length = (USHORT)(n * 2);
        dst->MaximumLength = (USHORT)((n + 1) * 2);
    }
}

// --- I/O Manager ---
__attribute__((ms_abi)) static NTSTATUS NT_IoCreateDevice(
        PDRIVER_OBJECT DriverObject, ULONG DevExtSize, PUNICODE_STRING DeviceName,
        ULONG DeviceType, ULONG Characteristics, BOOLEAN Exclusive, PDEVICE_OBJECT* DeviceObject) {
    (void)DeviceName; (void)DeviceType; (void)Characteristics; (void)Exclusive;

    PDEVICE_OBJECT dev = (PDEVICE_OBJECT)kmalloc(sizeof(DEVICE_OBJECT) + DevExtSize);
    if (!dev) return STATUS_UNSUCCESSFUL;

    for (unsigned i = 0; i < sizeof(DEVICE_OBJECT); i++) ((uint8_t*)dev)[i] = 0;
    dev->Type = 3;
    dev->Size = (SHORT)sizeof(DEVICE_OBJECT);
    dev->DriverObject = DriverObject;
    dev->DeviceExtension = DevExtSize ? (void*)(dev + 1) : 0;

    // encadeia no DRIVER_OBJECT (como o I/O manager faz)
    dev->NextDevice = DriverObject->DeviceObject;
    DriverObject->DeviceObject = dev;

    if (DeviceObject) *DeviceObject = dev;
    return STATUS_SUCCESS;
}

__attribute__((ms_abi)) static void NT_IoDeleteDevice(PDEVICE_OBJECT DeviceObject) {
    (void)DeviceObject;   // (sem free no heap simples por enquanto)
}

// --- Tabela de exports do "ntoskrnl.exe" ---
static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static const struct { const char* name; void* fn; } g_ntexports[] = {
    { "DbgPrint",             (void*)NT_DbgPrint },
    { "RtlInitUnicodeString", (void*)NT_RtlInitUnicodeString },
    { "IoCreateDevice",       (void*)NT_IoCreateDevice },
    { "IoDeleteDevice",       (void*)NT_IoDeleteDevice },
    { 0, 0 }
};

void* ntkrnl_resolve(const char* dll, const char* fn) {
    (void)dll;
    for (int i = 0; g_ntexports[i].name; i++)
        if (streq(g_ntexports[i].name, fn)) return g_ntexports[i].fn;
    return 0;
}
