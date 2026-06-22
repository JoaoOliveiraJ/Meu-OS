// Carregador de drivers de kernel — o papel do I/O Manager do NT ao dar
// "load" num .sys: monta o DRIVER_OBJECT e chama DriverEntry(Driver, Registry).
#include "ntddk.h"
#include "loader/pe.h"
#include "nt/ntexec.h"
#include "nt/driver.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);

void driver_load(const void* image) {
    void* entry = 0;
    void* base = pe_load(image, ntkrnl_resolve, &entry);
    if (!base || !entry) { kputs("[io] falha ao carregar o .sys\n"); return; }

    static DRIVER_OBJECT drv;
    for (unsigned i = 0; i < sizeof(drv); i++) ((uint8_t*)&drv)[i] = 0;
    drv.Type        = 4;                       // IO_TYPE_DRIVER
    drv.Size        = (SHORT)sizeof(DRIVER_OBJECT);
    drv.DriverStart = base;
    drv.DriverInit  = entry;

    UNICODE_STRING reg = { 0, 0, 0 };

    typedef NTSTATUS (__attribute__((ms_abi)) * driver_entry_t)(PDRIVER_OBJECT, PUNICODE_STRING);
    driver_entry_t DriverEntry = (driver_entry_t)entry;

    kputs("[io] chamando DriverEntry...\n");
    NTSTATUS st = DriverEntry(&drv, &reg);
    kputs("[io] DriverEntry retornou status="); kput_hex((uint32_t)st);
    kputs(st == STATUS_SUCCESS ? "  (STATUS_SUCCESS)\n" : "\n");

    if (drv.DeviceObject)
        kputs("[io] driver registrou device object(s).\n");

    if (drv.DriverUnload) {
        typedef void (__attribute__((ms_abi)) * unload_t)(PDRIVER_OBJECT);
        kputs("[io] chamando DriverUnload...\n");
        ((unload_t)drv.DriverUnload)(&drv);
    }
    kputs("[io] driver finalizado.\n");
}
