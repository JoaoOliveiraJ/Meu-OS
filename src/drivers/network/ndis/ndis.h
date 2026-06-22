#pragma once
#include <stdint.h>

// ============================================================================
//  ndis.h — NDIS (Network Driver Interface Specification) framework.
//
//  FASE 12 do MeuOS: stack de rede Windows 10/11. Espelha em miniatura o
//  ndis.sys do NT. Em Windows real, NDIS e a camada entre drivers de NIC
//  (miniport) e os protocolos (tcpip, nbf, etc). Apis chave:
//
//    - NdisInitializeWrapper       : registro inicial de driver (WDM legacy).
//    - NdisMRegisterMiniportDriver : registra um driver miniport (NDIS 6).
//    - NdisRegisterProtocolDriver  : registra protocolos sobre os miniports.
//    - NdisAllocateNetBufferList   : aloca NET_BUFFER_LIST para envio.
//    - NdisMSendNetBufferListsComplete: callback de conclusao de envio.
//
//  Aqui implementamos APENAS o framework stub: o driver de miniport (e1000)
//  chama NdisMRegisterMiniportDriver e ganha um handle. Sem TX/RX real, sem
//  CORB/DMA, sem mecanismo de bind tcpip<->ndis. As DLLs ring 3 (ws2_32)
//  funcionam em paralelo (sockets fakes que devolvem 0 bytes em recv).
//
//  Todo o estado vive em pools estaticos. Logs em '[ndis] ...'.
// ============================================================================

// Status codes NDIS (subset dos canonicos do Windows).
#define NDIS_STATUS_SUCCESS         0x00000000
#define NDIS_STATUS_FAILURE         0xC0000001
#define NDIS_STATUS_RESOURCES       0xC000009A
#define NDIS_STATUS_INVALID_PARAMETER 0xC000000D
#define NDIS_STATUS_NOT_RECOGNIZED  0xC0010002

// Versao da NDIS (NT 10.0+). Equivalente a NDIS_MINIPORT_DRIVER_CHARACTERISTICS.
#define NDIS_MINIPORT_MAJOR_VERSION 6
#define NDIS_MINIPORT_MINOR_VERSION 80

// Inicializa o framework NDIS. Chamada uma vez no boot, antes dos drivers de
// miniport rodarem DriverEntry. Idempotente.
int  ndis_init(void);

// Quantidade de miniport drivers registrados (apos ndis_init + DriverEntry
// dos drivers como e1000.sys).
int  ndis_miniport_count(void);

// Quantidade de protocol drivers registrados (TCP/IP, etc).
int  ndis_protocol_count(void);

// Forward decls para o stub das APIs (assinaturas sem tipos complexos para
// evitar grandes dependencias; usadas APENAS internamente). As entradas que
// os drivers veem vivem em ntoskrnl.c (Nt*).
typedef int NDIS_STATUS;
typedef void* NDIS_HANDLE;
typedef void* PNET_BUFFER_LIST;

// Stubs exposed via ntoskrnl exports table:
//
//   NDIS_STATUS NdisMRegisterMiniportDriver(...);
//   NDIS_STATUS NdisRegisterProtocolDriver(...);
//   PNET_BUFFER_LIST NdisAllocateNetBufferList(...);
//   void NdisFreeNetBufferList(PNET_BUFFER_LIST);
//   void NdisMSendNetBufferListsComplete(NDIS_HANDLE, PNET_BUFFER_LIST, ULONG);
//   void NdisInitializeWrapper(...);
//
//  Implementacoes em ndis.c. Cada chamada incrementa contadores internos.

// Registra um miniport novo (chamado pelo driver de NIC, e.g. e1000.sys).
// Aqui apenas guarda o ponteiro em uma tabela; sem PnP real.
int ndis_register_miniport(const char* name, void* driver_handle);

// Registra um protocolo (chamado pelo driver de protocolo, e.g. tcpip.sys).
int ndis_register_protocol(const char* name);
