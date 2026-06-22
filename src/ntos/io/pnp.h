#pragma once
#include "ntddk.h"

// ============================================================================
//  pnp.h — PnP Manager do MeuOS (FASE 13).
//
//  Parte do I/O Manager. Lida com IRP_MJ_PNP (major function 0x1B). Os
//  sub-codigos (IRP_MN_*) seguem a numeracao do WDM. Em um stack PnP real,
//  o I/O Manager emite estes IRPs para a Device Stack (FDO + filtros + PDO)
//  em resposta a eventos do barramento. No MeuOS, a maquina e quase toda
//  estatica (nada e plug-and-play em runtime), entao o dispatcher recebe
//  IRPs apenas em smoke tests; o objetivo aqui e *prover a API* e logar.
//
//  IRP_MN canonicos (subset usado):
//    0x00 START_DEVICE
//    0x01 QUERY_REMOVE_DEVICE
//    0x02 REMOVE_DEVICE
//    0x07 QUERY_DEVICE_RELATIONS
//    0x09 QUERY_CAPABILITIES
//    0x14 SURPRISE_REMOVAL
//
//  Logs em '[pnp] ...'.
// ============================================================================

// IRP_MN_* — sub-codigos para IRP_MJ_PNP (espelho do WDM).
#define IRP_MN_START_DEVICE                 0x00
#define IRP_MN_QUERY_REMOVE_DEVICE          0x01
#define IRP_MN_REMOVE_DEVICE                0x02
#define IRP_MN_CANCEL_REMOVE_DEVICE         0x03
#define IRP_MN_STOP_DEVICE                  0x04
#define IRP_MN_QUERY_STOP_DEVICE            0x05
#define IRP_MN_CANCEL_STOP_DEVICE           0x06
#define IRP_MN_QUERY_DEVICE_RELATIONS       0x07
#define IRP_MN_QUERY_INTERFACE              0x08
#define IRP_MN_QUERY_CAPABILITIES           0x09
#define IRP_MN_QUERY_RESOURCES              0x0A
#define IRP_MN_QUERY_RESOURCE_REQUIREMENTS  0x0B
#define IRP_MN_QUERY_DEVICE_TEXT            0x0C
#define IRP_MN_FILTER_RESOURCE_REQUIREMENTS 0x0D
#define IRP_MN_READ_CONFIG                  0x0F
#define IRP_MN_WRITE_CONFIG                 0x10
#define IRP_MN_EJECT                        0x11
#define IRP_MN_SET_LOCK                     0x12
#define IRP_MN_QUERY_ID                     0x13
#define IRP_MN_QUERY_PNP_DEVICE_STATE       0x14
#define IRP_MN_QUERY_BUS_INFORMATION        0x15
#define IRP_MN_DEVICE_USAGE_NOTIFICATION    0x16
#define IRP_MN_SURPRISE_REMOVAL             0x17

// DeviceState bits (para IoInvalidateDeviceState e PnP_DEVICE_STATE).
#define PNP_DEVICE_DISABLED       0x00000001
#define PNP_DEVICE_REMOVED        0x00000002
#define PNP_DEVICE_FAILED         0x00000004
#define PNP_DEVICE_RESOURCE_REQ_CHANGED 0x00000008

// Inicializa o PnP manager. Loga '[pnp] init'. Idempotente.
int pnp_init(void);

// Dispatcher canonico para IRP_MJ_PNP. Drivers chamam (ou usam como default).
// Devolve STATUS_SUCCESS para todos os sub-codigos no caminho seguro,
// loga o sub-codigo recebido. Aceita PIRP/PDEVICE_OBJECT como void* para
// evitar dependencias.
__attribute__((ms_abi))
NTSTATUS pnp_dispatch(void* dev, void* irp);

// Numero de IRPs PNP processados (smoke test counter).
uint64_t pnp_irp_count(void);

// Numero de devices reportados via IoReportDeviceObject.
int pnp_reported_device_count(void);

// Numero de IoInvalidateDeviceState recebidos.
uint64_t pnp_invalidate_count(void);

// APIs publicas ms_abi expostas via ntoskrnl exports.
__attribute__((ms_abi))
void pnp_IoInvalidateDeviceState(void* device_object);

__attribute__((ms_abi))
NTSTATUS pnp_IoReportDeviceObject(void* device_object, const char* name);
