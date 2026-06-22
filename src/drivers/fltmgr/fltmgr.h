#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  fltmgr.h — Filter Manager do MeuOS (FASE 13).
//
//  Espelha o fltmgr.sys do Windows: arquiteta minifilters (drivers de filtro
//  de filesystem). Antivirus, encryption, virtualization (e.g. ProcMon)
//  usam esta interface ao inves do legacy filter driver model.
//
//  APIs canonicas (ms_abi, exportadas via ntoskrnl + fltmgr.lib):
//    FltRegisterFilter         — minifilter registra suas callbacks por IRP_MJ_*
//    FltStartFiltering         — minifilter sai do estado bound e comeca a interceptar
//    FltUnregisterFilter       — desfaz o registro
//    FltCreateCommunicationPort — abre port \FltMgr p/ comm minifilter<->user-mode
//    FltSendMessage            — minifilter envia msg para o servico de user-mode
//    FltSetCallbackDataDirty   — marca o callback como tendo modificado FLT_CALLBACK_DATA
//
//  Sem callbacks reais sendo invocadas — somos o stub do Filter Manager
//  apenas para que drivers de minifilter (signtool/sentinel/etc) carreguem
//  o DriverEntry sem missing import.
//
//  Logs em '[fltmgr] ...'.
// ============================================================================

// Tipo opaco do handle de filtro (passado de volta para Unregister/Start).
typedef void* PFLT_FILTER;
typedef void* PFLT_PORT;
typedef void* PFLT_CALLBACK_DATA;

// Inicializa o Filter Manager. Idempotente. Loga '[fltmgr] init'.
int fltmgr_init(void);

// Contadores informativos (smoke test).
int      fltmgr_filter_count(void);
int      fltmgr_port_count(void);
uint64_t fltmgr_message_count(void);

// --- APIs ms_abi expostas via ntoskrnl exports ---
__attribute__((ms_abi))
NTSTATUS fltmgr_FltRegisterFilter(void* driver_object, void* registration,
                                  PFLT_FILTER* out_filter);

__attribute__((ms_abi))
NTSTATUS fltmgr_FltStartFiltering(PFLT_FILTER filter);

__attribute__((ms_abi))
void fltmgr_FltUnregisterFilter(PFLT_FILTER filter);

__attribute__((ms_abi))
NTSTATUS fltmgr_FltCreateCommunicationPort(PFLT_FILTER filter, PFLT_PORT* server_port,
                                           void* object_attributes, void* server_port_cookie,
                                           void* connect_notify, void* disconnect_notify,
                                           void* message_notify, int max_connections);

__attribute__((ms_abi))
NTSTATUS fltmgr_FltSendMessage(PFLT_FILTER filter, PFLT_PORT* client_port,
                               void* sender_buffer, uint32_t sender_buffer_length,
                               void* reply_buffer, uint32_t* reply_length,
                               void* timeout);

__attribute__((ms_abi))
void fltmgr_FltSetCallbackDataDirty(PFLT_CALLBACK_DATA data);
