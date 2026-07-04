// ============================================================================
//  pnp.c — PnP Manager do MeuOS (FASE 13).
//
//  Implementa o dispatcher IRP_MJ_PNP padrao + APIs ms_abi expostas como
//  exports do ntoskrnl. Tudo no caminho seguro: cada sub-codigo retorna
//  STATUS_SUCCESS e completa o IRP. Sem device stacks reais, sem PDOs
//  filtros, sem QUERY_RELATIONS produzindo arvores.
//
//  Logs em '[pnp] ...'. Cada chamada incrementa contadores internos
//  acessiveis via pnp_*_count() (uteis em smoke test).
// ============================================================================
#include <stdint.h>
#include "ntddk.h"
#include "pnp.h"
#include "io/io.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado interno -------------------------------------------------------
static int               s_initialized       = 0;
static volatile uint64_t s_irp_count         = 0;
static volatile uint64_t s_invalidate_count  = 0;
static int               s_reported_count    = 0;

// Tabela de devices reportados (apenas para contagem; sem operacao real).
#define PNP_MAX_REPORTED 32
static const char*  s_reported_names[PNP_MAX_REPORTED];

static const char* irp_mn_name(uint8_t mn) {
    switch (mn) {
        case IRP_MN_START_DEVICE:                 return "START_DEVICE";
        case IRP_MN_QUERY_REMOVE_DEVICE:          return "QUERY_REMOVE_DEVICE";
        case IRP_MN_REMOVE_DEVICE:                return "REMOVE_DEVICE";
        case IRP_MN_CANCEL_REMOVE_DEVICE:         return "CANCEL_REMOVE_DEVICE";
        case IRP_MN_STOP_DEVICE:                  return "STOP_DEVICE";
        case IRP_MN_QUERY_STOP_DEVICE:            return "QUERY_STOP_DEVICE";
        case IRP_MN_CANCEL_STOP_DEVICE:           return "CANCEL_STOP_DEVICE";
        case IRP_MN_QUERY_DEVICE_RELATIONS:       return "QUERY_DEVICE_RELATIONS";
        case IRP_MN_QUERY_INTERFACE:              return "QUERY_INTERFACE";
        case IRP_MN_QUERY_CAPABILITIES:           return "QUERY_CAPABILITIES";
        case IRP_MN_QUERY_RESOURCES:              return "QUERY_RESOURCES";
        case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:  return "QUERY_RESOURCE_REQUIREMENTS";
        case IRP_MN_QUERY_DEVICE_TEXT:            return "QUERY_DEVICE_TEXT";
        case IRP_MN_FILTER_RESOURCE_REQUIREMENTS: return "FILTER_RESOURCE_REQUIREMENTS";
        case IRP_MN_READ_CONFIG:                  return "READ_CONFIG";
        case IRP_MN_WRITE_CONFIG:                 return "WRITE_CONFIG";
        case IRP_MN_EJECT:                        return "EJECT";
        case IRP_MN_SET_LOCK:                     return "SET_LOCK";
        case IRP_MN_QUERY_ID:                     return "QUERY_ID";
        case IRP_MN_QUERY_PNP_DEVICE_STATE:       return "QUERY_PNP_DEVICE_STATE";
        case IRP_MN_QUERY_BUS_INFORMATION:        return "QUERY_BUS_INFORMATION";
        case IRP_MN_DEVICE_USAGE_NOTIFICATION:    return "DEVICE_USAGE_NOTIFICATION";
        case IRP_MN_SURPRISE_REMOVAL:             return "SURPRISE_REMOVAL";
        default:                                  return "(IRP_MN desconhecido)";
    }
}

int pnp_init(void) {
    if (s_initialized) return 1;
    for (int i = 0; i < PNP_MAX_REPORTED; i++) s_reported_names[i] = 0;
    s_irp_count        = 0;
    s_invalidate_count = 0;
    s_reported_count   = 0;
    s_initialized      = 1;
    kputs("[pnp] IRP_MJ_PNP dispatcher registrado (subcodigos suportados=23)\n");
    return 1;
}

__attribute__((ms_abi))
NTSTATUS pnp_dispatch(void* dev, void* irp_) {
    (void)dev;
    PIRP irp = (PIRP)irp_;
    s_irp_count++;
    uint8_t mn = 0;
    if (irp && IoGetCurrentIrpStackLocation(irp)) {
        mn = IoGetCurrentIrpStackLocation(irp)->MinorFunction;
    }
    // Caminho seguro: completa o IRP com SUCCESS.
    if (irp) {
        irp->IoStatus.Status      = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
    }
    kputs("[pnp] IRP_MJ_PNP recebido: minor=0x");
    kput_hex((uint64_t)mn);
    kputs(" ("); kputs(irp_mn_name(mn)); kputs(")\n");
    return STATUS_SUCCESS;
}

uint64_t pnp_irp_count(void)             { return s_irp_count;        }
int      pnp_reported_device_count(void) { return s_reported_count;   }
uint64_t pnp_invalidate_count(void)      { return s_invalidate_count; }

// IoInvalidateDeviceState: o driver chama quando o estado PnP (resources/
// disabled/removed) muda e o PnP manager precisa re-perguntar QUERY_*.
// No stub apenas conta e loga.
__attribute__((ms_abi))
void pnp_IoInvalidateDeviceState(void* device_object) {
    (void)device_object;
    s_invalidate_count++;
    kputs("[pnp] IoInvalidateDeviceState chamado (call #");
    kput_dec(s_invalidate_count); kputs(")\n");
}

// IoReportDeviceObject: driver de bus enumerator informa que existe um novo
// PDO. Sem device stack real, apenas adiciona a tabela e loga.
__attribute__((ms_abi))
NTSTATUS pnp_IoReportDeviceObject(void* device_object, const char* name) {
    (void)device_object;
    if (!s_initialized) pnp_init();
    if (s_reported_count < PNP_MAX_REPORTED) {
        s_reported_names[s_reported_count] = name ? name : "(anon)";
        s_reported_count++;
    }
    kputs("[pnp] IoReportDeviceObject: '");
    kputs(name ? name : "(anon)");
    kputs("' total="); kput_dec((uint64_t)s_reported_count); kputc('\n');
    return STATUS_SUCCESS;
}

// ============================================================================
//  FASE FUNDACAO (trilha I/O, Fase 4) — PnP minimo: AddDevice + START_DEVICE.
//
//  Le DriverExtension.AddDevice; se setado, cria um PDO sintetico, chama
//  AddDevice (o driver cria a FDO e a anexa sobre o PDO — device stack da Fase
//  2), e envia IRP_MJ_PNP/IRP_MN_START_DEVICE ao topo da pilha. Drivers legados
//  (AddDevice==NULL, incluindo o pintok) sao no-op -> seguro. NAO e' chamado do
//  caminho de load do driver.c (esse fica intocado); por ora, exercitado pelo
//  self-test. Wire no load = passo futuro (quando houver function driver real).
// ============================================================================
#define IRP_MN_START_DEVICE_ 0
void pnp_start_function_driver(PDRIVER_OBJECT drv, const char* name) {
    PDRIVER_EXTENSION ext = drv ? (PDRIVER_EXTENSION)drv->DriverExtension : 0;
    if (!ext || !ext->AddDevice) {
        kputs("[pnp] driver '"); kputs(name ? name : "(anon)");
        kputs("' sem AddDevice — modelo legado, START_DEVICE nao enviado\n");
        return;
    }
    static DRIVER_OBJECT s_pnp_root;              // dono dos PDOs sinteticos
    PDEVICE_OBJECT pdo = 0;
    io_create_device(&s_pnp_root, 0, 0, 0x22 /*FILE_DEVICE_UNKNOWN*/, &pdo);
    if (!pdo) return;
    kputs("[pnp] AddDevice detectado p/ '"); kputs(name ? name : "(anon)"); kputs("' -> criando FDO\n");
    ext->AddDevice(drv, pdo);                     // o driver cria a FDO + anexa sobre o PDO
    PDEVICE_OBJECT top = IoGetAttachedDevice_k(pdo);
    if (top && top != pdo) {
        PIRP irp = io_build_request(IRP_MJ_PNP, top);
        if (irp) {
            IoGetNextIrpStackLocation(irp)->MinorFunction = IRP_MN_START_DEVICE_;
            IoCallDriver(top, irp);
            io_free_irp(irp);
            kputs("[pnp] START_DEVICE enviado a FDO\n");
        }
    }
}

// --- Prova de boot: um function driver com AddDevice inicia e recebe START. ---
static PDEVICE_OBJECT s_pnp_test_fdo = 0;
static volatile int   s_pnp_test_started = 0;
static NTSTATUS __attribute__((ms_abi)) pnp_test_add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo) {
    PDEVICE_OBJECT fdo = 0;
    io_create_device(drv, 0, "\\Device\\ProbeFdo", 0x22, &fdo);
    if (fdo) { IoAttachDeviceToDeviceStack_k(fdo, pdo); s_pnp_test_fdo = fdo; }
    return STATUS_SUCCESS;
}
static NTSTATUS __attribute__((ms_abi)) pnp_test_dispatch(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (s && s->MajorFunction == IRP_MJ_PNP && s->MinorFunction == IRP_MN_START_DEVICE_)
        s_pnp_test_started = 1;
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest_k(irp, 0);
    return STATUS_SUCCESS;
}
void KiPnpSelfTest(void) {
    static DRIVER_OBJECT   drv;
    static DRIVER_EXTENSION ext;
    for (unsigned i = 0; i < sizeof(drv); i++) ((uint8_t*)&drv)[i] = 0;
    for (unsigned i = 0; i < sizeof(ext); i++) ((uint8_t*)&ext)[i] = 0;
    ext.DriverObject = &drv;
    ext.AddDevice    = pnp_test_add_device;
    drv.DriverExtension = &ext;
    drv.MajorFunction[IRP_MJ_PNP] = (void*)pnp_test_dispatch;
    s_pnp_test_fdo = 0; s_pnp_test_started = 0;
    pnp_start_function_driver(&drv, "probe_pnp");
    if (s_pnp_test_fdo && s_pnp_test_started)
        kputs("[pnp-test] AddDevice + IRP_MJ_PNP START_DEVICE -> FDO OK\n");
    else {
        kputs("[pnp-test] FALHOU fdo="); kput_hex((uint64_t)(uintptr_t)s_pnp_test_fdo);
        kputs(" started="); kput_hex((uint64_t)(uint32_t)s_pnp_test_started); kputs("\n");
    }
}
