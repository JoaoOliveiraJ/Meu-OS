#pragma once
#include <stdint.h>

// ============================================================================
//  tcpip.h — TCP/IP stack do MeuOS (Fase 12 stub).
//
//  Espelha o tcpip.sys do Windows. Em Windows real, o tcpip.sys e o protocol
//  driver que se registra na NDIS via NdisRegisterProtocolDriver, faz bind a
//  cada miniport (NIC) e expoe a IP/TCP/UDP stack para apps user-mode atraves
//  do Winsock Kernel (WSK) ou do TDI legacy (\Device\Tcp, \Device\Udp).
//
//  Aqui no MeuOS esta fase entrega:
//    1) Inicializacao do "driver" tcpip (no kernel, sem PE separado).
//    2) Registro como protocolo NDIS (ndis_register_protocol("Tcpip")).
//    3) Cria os DEVICE_OBJECTs canonicos:
//         \Device\Tcp     \Device\Udp     \Device\Ip     \Device\RawIp
//       — assim ws2_32.dll pode "abrir" um deles via NtCreateFile (caminho
//       seguro: nenhuma op tenta DMA / SEND real).
//    4) Sem stack real: rotas/binds/sockets vivem em pools estaticos.
//
//  As funcoes ms_abi para TDI/WSK (TdiBuildInternalDeviceControlIrp, etc) sao
//  expostas como stubs em ntoskrnl.c — retornam STATUS_SUCCESS sem efeito real.
// ============================================================================

// Inicializa o "driver" TCP/IP do kernel: cria DRIVER_OBJECT (logico) e os
// 4 devices canonicos no namespace. Idempotente.
int tcpip_init(void);

// Devices criados pelo tcpip_init (sem includes que arrastam ntddk pra cá).
// Retornam ponteiros opacos: PDEVICE_OBJECT cast em void*.
void* tcpip_dev_tcp(void);
void* tcpip_dev_udp(void);
void* tcpip_dev_ip(void);
void* tcpip_dev_rawip(void);

// Contadores informativos (numero de aberturas via NtCreateFile / sockets fake).
int tcpip_socket_count(void);
