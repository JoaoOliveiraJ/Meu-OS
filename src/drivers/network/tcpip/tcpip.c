// ============================================================================
//  tcpip.c — TCP/IP stack do MeuOS (Fase 12 stub).
//
//  Cria os 4 devices canonicos do tcpip.sys do Windows:
//
//    \Device\Tcp     — TCP TDI/WSK (stream)
//    \Device\Udp     — UDP TDI/WSK (datagram)
//    \Device\Ip      — IP raw socket (privilegiado)
//    \Device\RawIp   — RAW IP datagram (privilegiado)
//
//  Sem socket real (zero TX/RX), apenas o DEVICE_OBJECT existe no namespace
//  para que ws2_32.dll possa abrir um handle via NtCreateFile sem 404. Logs
//  em '[tcpip] ...'. Registra a si mesmo como protocolo na NDIS.
// ============================================================================
#include "ntddk.h"
#include "tcpip.h"
#include "io/io.h"
#include "ob/object.h"
#include "../ndis/ndis.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado interno -------------------------------------------------------
static int             s_initialized   = 0;
static PDRIVER_OBJECT  s_tcpip_driver  = 0;
static PDEVICE_OBJECT  s_dev_tcp       = 0;
static PDEVICE_OBJECT  s_dev_udp       = 0;
static PDEVICE_OBJECT  s_dev_ip        = 0;
static PDEVICE_OBJECT  s_dev_rawip     = 0;
static int             s_sock_count    = 0;

// IRP MJ_CREATE simples para os devices do TCPIP: cada open vira um "socket"
// fake. Aqui apenas contamos as aberturas (todas devolvem STATUS_SUCCESS).
__attribute__((ms_abi))
static NTSTATUS tcpip_create(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    s_sock_count++;
    if (irp) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
    }
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
static NTSTATUS tcpip_close(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    if (irp) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
    }
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
static NTSTATUS tcpip_devctl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    // TDI/WSK IOCTLs: todos no caminho seguro -> SUCCESS sem efeito real.
    if (irp) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
    }
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
static NTSTATUS tcpip_read(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    // Recv sem dados: devolve 0 bytes (comportamento esperado de rede vazia).
    if (irp) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
    }
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
static NTSTATUS tcpip_write(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    // Send: aceita os bytes sem transmitir (rede vazia).
    if (irp) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        // Reporta TUDO escrito (write n bytes -> n bytes "transmitidos").
        irp->IoStatus.Information = IoGetCurrentIrpStackLocation(irp) ?
            IoGetCurrentIrpStackLocation(irp)->Parameters.Write.Length : 0;
    }
    return STATUS_SUCCESS;
}

static int create_device(const char* name, PDEVICE_OBJECT* out) {
    NTSTATUS st = io_create_device(s_tcpip_driver, 0, name,
                                   FILE_DEVICE_UNKNOWN, out);
    if (st != STATUS_SUCCESS || !*out) {
        kputs("[tcpip] falha ao criar '"); kputs(name); kputs("'\n");
        return 0;
    }
    return 1;
}

int tcpip_init(void) {
    if (s_initialized) return 1;

    // Inicializa NDIS antes (se ainda nao foi). Idempotente.
    ndis_init();

    // 1) Cria o DRIVER_OBJECT logico do tcpip (sem PE separado).
    s_tcpip_driver = (PDRIVER_OBJECT)ObCreateObject(OB_TYPE_DRIVER,
                                                    sizeof(DRIVER_OBJECT),
                                                    "\\Driver\\Tcpip");
    if (!s_tcpip_driver) {
        kputs("[tcpip] sem memoria p/ DRIVER_OBJECT\n");
        return 0;
    }
    s_tcpip_driver->Type = 4;
    s_tcpip_driver->Size = (SHORT)sizeof(DRIVER_OBJECT);
    s_tcpip_driver->MajorFunction[IRP_MJ_CREATE]         = (PVOID)tcpip_create;
    s_tcpip_driver->MajorFunction[IRP_MJ_CLOSE]          = (PVOID)tcpip_close;
    s_tcpip_driver->MajorFunction[IRP_MJ_READ]           = (PVOID)tcpip_read;
    s_tcpip_driver->MajorFunction[IRP_MJ_WRITE]          = (PVOID)tcpip_write;
    s_tcpip_driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = (PVOID)tcpip_devctl;

    // 2) Registra como protocolo na NDIS.
    ndis_register_protocol("Tcpip");

    // 3) Cria os 4 devices canonicos.
    if (!create_device("\\Device\\Tcp",   &s_dev_tcp))   return 0;
    if (!create_device("\\Device\\Udp",   &s_dev_udp))   return 0;
    if (!create_device("\\Device\\Ip",    &s_dev_ip))    return 0;
    if (!create_device("\\Device\\RawIp", &s_dev_rawip)) return 0;

    kputs("[tcpip] init: \\Device\\{Tcp,Udp,Ip,RawIp} criados (TDI/WSK stub)\n");
    s_initialized = 1;
    return 1;
}

void* tcpip_dev_tcp(void)   { return (void*)s_dev_tcp;   }
void* tcpip_dev_udp(void)   { return (void*)s_dev_udp;   }
void* tcpip_dev_ip(void)    { return (void*)s_dev_ip;    }
void* tcpip_dev_rawip(void) { return (void*)s_dev_rawip; }

int tcpip_socket_count(void) { return s_sock_count; }
