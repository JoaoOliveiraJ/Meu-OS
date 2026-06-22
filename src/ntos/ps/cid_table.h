#pragma once
// ============================================================================
//  FASE 7.5 — PspCidTable (Client ID Table) estilo NT.
//
//  No Windows real, ntoskrnl mantem uma HANDLE_TABLE global chamada
//  PspCidTable (variavel exportada como PVOID ou PEX_HANDLE_TABLE). Cada
//  entrada nessa tabela mapeia um Client ID (HANDLE truncado a 32-bit que
//  contem PID ou TID) -> ponteiro p/ EPROCESS ou ETHREAD.
//
//  Drivers reais (anti-cheat, EDR, rootkits) tipicamente fazem UMA das duas:
//   1) Chamam PsLookupProcessByProcessId(pid, &eproc) — varre a tabela.
//   2) "Reach-around": dereferenciam PspCidTable diretamente, andam pela
//      hash table NT (3 niveis), e obtem o EPROCESS sem chamar API. Isso
//      e a tecnica que pintok.sys (e similares VMProtected) usam.
//
//  ESTRATEGIA AQUI (simplificada):
//   - Array linear de 1024 entradas com (cid, ptr) — o suficiente p/ os PIDs
//     que o MeuOS gera (1, 2, 3, ...).
//   - PsLookupProcessByProcessId_k consulta a CID table PRIMEIRO; se nao
//     achar, cai no fallback linear via ob_enum_by_type (ja existente).
//   - Inserts/Removes acontecem em PsCreateProcess/PsCreateThread/Terminate.
//   - Exportamos g_pspcid_table como "PspCidTable" para drivers que fazem
//     reach-around — apontamos para um struct HANDLE_TABLE simples (sem
//     levels reais; a maioria so dereferencia ponteiros, e como tudo e 0
//     nao crasha mas a busca falha silenciosamente).
// ============================================================================
#include <stdint.h>
#include "ntddk.h"          // HANDLE
#include "ob/object.h"
#include "ps/process.h"

// Tipo de entrada na tabela. Mistura processo e thread (como o NT real).
// 'cid' e o Client ID (PID ou TID); 'object' aponta p/ EPROCESS ou ETHREAD.
// 'is_thread' diferencia (0=process, 1=thread).
typedef struct _CID_ENTRY {
    uint32_t  cid;          // PID/TID (0 = entrada livre)
    uint8_t   is_thread;    // 0=EPROCESS, 1=ETHREAD
    uint8_t   _pad[3];
    void*     object;       // ponteiro p/ corpo do objeto
} CID_ENTRY;

// HANDLE_TABLE simplificado, layout MINIMO p/ drivers que tocam em offsets
// fixos. No NT real essa estrutura tem ~ 0x80 bytes (Win10) com TableCode,
// QuotaProcess, etc. Mantemos os campos mais consultados em offsets parecidos
// (sem garantir bit-exatidao, pois a maioria dos drivers apenas faz teste
// de NULL ou anda em ponteiros vivos).
typedef struct _CID_HANDLE_TABLE {
    void*       TableCode;          // +0x00  ponteiro p/ array (cid_table_entries)
    void*       QuotaProcess;       // +0x08  NULL
    HANDLE      UniqueProcessId;    // +0x10  NULL (tabela global)
    uint64_t    HandleLock;         // +0x18  spinlock dummy = 0
    LIST_ENTRY  HandleTableList;    // +0x20  vazia (Flink/Blink = self)
    uint64_t    HandleContentionEvent; // +0x30
    uint64_t    DebugInfo;          // +0x38
    int32_t     ExtraInfoPages;     // +0x40
    uint32_t    Flags;              // +0x44
    uint32_t    FirstFreeHandle;    // +0x48
    uint32_t    LastFreeHandle;     // +0x4C
    uint32_t    NextHandleNeedingPool; // +0x50
    int32_t     HandleCount;        // +0x54  numero de handles em uso
    uint64_t    _reserved[6];       // padding p/ ~0x80 bytes (offsets seguros)
} CID_HANDLE_TABLE;

// Variavel global exportada como "PspCidTable" (drivers fazem reach-around).
extern CID_HANDLE_TABLE g_pspcid_table;

// Inicializa a tabela (chame ANTES de PsCreateProcess). Limpa todas entradas
// e prepara o struct CID_HANDLE_TABLE p/ ser exposto via ntexec.
void cid_init(void);

// Insercao: associa um CID (PID/TID) a um EPROCESS/ETHREAD. Retorna 1 se OK,
// 0 se tabela cheia. Se ja existir, atualiza o ponteiro (raro, mas seguro).
int cid_insert_process(PEPROCESS proc);
int cid_insert_thread(PETHREAD thr);

// Busca: devolve o EPROCESS/ETHREAD para um PID/TID, ou 0 se nao achou.
// O lookup e LINEAR (array pequeno; 1024 entradas no max) — ainda mais rapido
// que ob_enum_by_type, pois nao precisa pular objetos de outros tipos.
PEPROCESS cid_lookup_process(uint32_t pid);
PETHREAD  cid_lookup_thread (uint32_t tid);

// Remocao (chamada por PsTerminateProcess): libera a entrada do CID.
void cid_remove_process(uint32_t pid);
void cid_remove_thread (uint32_t tid);
