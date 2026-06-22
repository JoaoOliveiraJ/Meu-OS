// ============================================================================
//  FASE 7.5 — PspCidTable (Client ID Table) estilo NT.
//
//  Implementacao simples: array linear de 1024 entradas com (cid, ptr). As
//  funcoes PsLookupProcessByProcessId_k / PsLookupThreadByThreadId_k em
//  ke/systhread.c agora consultam ESTA tabela primeiro; fallback linear via
//  ob_enum_by_type continua funcionando (sem regressao).
//
//  A estrutura CID_HANDLE_TABLE em g_pspcid_table e exposta via ntexec.c como
//  "PspCidTable" — drivers que fazem reach-around (acessam o ponteiro direto e
//  andam a hash table) encontram um struct nao-NULL com layout suficiente p/
//  nao crashar em probes. Sem hash multi-level real, eles caem em ramo de erro
//  silencioso (NULL retornado) — mesmo comportamento de "process nao existe".
// ============================================================================
#include <stdint.h>
#include "nt/cid_table.h"

extern void kputs(const char* s);
extern void kput_dec(uint64_t v);
extern void kputc(char c);

// Tamanho da tabela: 1024 entradas suficientes p/ nossas dezenas de PIDs/TIDs
// (PsCreateProcess so e chamada algumas vezes durante o boot).
#define CID_MAX_ENTRIES 1024

// Array linear estatico (zerado por BSS). cid==0 marca entrada livre.
static CID_ENTRY s_cid_entries[CID_MAX_ENTRIES];

// Variavel global exportada. Apontamos TableCode p/ o array de entradas; assim
// um driver que faca `MOV RAX, [PspCidTable]; MOV RAX, [RAX]` consegue ler
// memoria valida (e nao crashar). O array em si e zerado fora do range de uso,
// entao a busca por reach-around devolve NULL na maioria dos casos — caminho
// seguro (igual a "PID nao existe").
CID_HANDLE_TABLE g_pspcid_table;

void cid_init(void) {
    // Zera todas as entradas (defensivo; BSS ja faz isso, mas garantimos
    // reinicio limpo caso cid_init seja chamada mais de uma vez no futuro).
    for (int i = 0; i < CID_MAX_ENTRIES; i++) {
        s_cid_entries[i].cid       = 0;
        s_cid_entries[i].is_thread = 0;
        s_cid_entries[i].object    = 0;
    }

    // Inicializa o cabecalho da tabela exposto via "PspCidTable".
    g_pspcid_table.TableCode             = (void*)s_cid_entries;
    g_pspcid_table.QuotaProcess          = 0;
    g_pspcid_table.UniqueProcessId       = 0;
    g_pspcid_table.HandleLock            = 0;
    g_pspcid_table.HandleTableList.Flink = &g_pspcid_table.HandleTableList;  // lista circular vazia
    g_pspcid_table.HandleTableList.Blink = &g_pspcid_table.HandleTableList;
    g_pspcid_table.HandleContentionEvent = 0;
    g_pspcid_table.DebugInfo             = 0;
    g_pspcid_table.ExtraInfoPages        = 0;
    g_pspcid_table.Flags                 = 0;
    g_pspcid_table.FirstFreeHandle       = 0;
    g_pspcid_table.LastFreeHandle        = 0;
    g_pspcid_table.NextHandleNeedingPool = 0;
    g_pspcid_table.HandleCount           = 0;
    for (int i = 0; i < 6; i++) g_pspcid_table._reserved[i] = 0;

    kputs("[cid] PspCidTable inicializada (");
    kput_dec(CID_MAX_ENTRIES);
    kputs(" entradas, exposta como variavel ntoskrnl)\n");
}

// Procura a primeira entrada livre (cid==0) e devolve seu indice; -1 se cheia.
static int cid_find_free(void) {
    for (int i = 0; i < CID_MAX_ENTRIES; i++)
        if (s_cid_entries[i].cid == 0) return i;
    return -1;
}

// Procura uma entrada por (cid, is_thread). Retorna indice ou -1.
static int cid_find_entry(uint32_t cid, uint8_t is_thread) {
    if (cid == 0) return -1;   // 0 = livre, nunca casa
    for (int i = 0; i < CID_MAX_ENTRIES; i++) {
        if (s_cid_entries[i].cid == cid &&
            s_cid_entries[i].is_thread == is_thread) return i;
    }
    return -1;
}

int cid_insert_process(PEPROCESS proc) {
    if (!proc || proc->ProcessId == 0) return 0;

    // Se ja existir, atualiza o ponteiro (defensivo — nao deve ocorrer).
    int idx = cid_find_entry(proc->ProcessId, /*is_thread*/0);
    if (idx < 0) idx = cid_find_free();
    if (idx < 0) {
        kputs("[cid] AVISO: PspCidTable cheia (process)\n");
        return 0;
    }
    s_cid_entries[idx].cid       = proc->ProcessId;
    s_cid_entries[idx].is_thread = 0;
    s_cid_entries[idx].object    = (void*)proc;
    g_pspcid_table.HandleCount++;
    return 1;
}

int cid_insert_thread(PETHREAD thr) {
    if (!thr || thr->ThreadId == 0) return 0;

    int idx = cid_find_entry(thr->ThreadId, /*is_thread*/1);
    if (idx < 0) idx = cid_find_free();
    if (idx < 0) {
        kputs("[cid] AVISO: PspCidTable cheia (thread)\n");
        return 0;
    }
    s_cid_entries[idx].cid       = thr->ThreadId;
    s_cid_entries[idx].is_thread = 1;
    s_cid_entries[idx].object    = (void*)thr;
    g_pspcid_table.HandleCount++;
    return 1;
}

PEPROCESS cid_lookup_process(uint32_t pid) {
    int idx = cid_find_entry(pid, /*is_thread*/0);
    if (idx < 0) return 0;
    return (PEPROCESS)s_cid_entries[idx].object;
}

PETHREAD cid_lookup_thread(uint32_t tid) {
    int idx = cid_find_entry(tid, /*is_thread*/1);
    if (idx < 0) return 0;
    return (PETHREAD)s_cid_entries[idx].object;
}

void cid_remove_process(uint32_t pid) {
    int idx = cid_find_entry(pid, /*is_thread*/0);
    if (idx < 0) return;
    s_cid_entries[idx].cid       = 0;
    s_cid_entries[idx].is_thread = 0;
    s_cid_entries[idx].object    = 0;
    if (g_pspcid_table.HandleCount > 0) g_pspcid_table.HandleCount--;
}

void cid_remove_thread(uint32_t tid) {
    int idx = cid_find_entry(tid, /*is_thread*/1);
    if (idx < 0) return;
    s_cid_entries[idx].cid       = 0;
    s_cid_entries[idx].is_thread = 0;
    s_cid_entries[idx].object    = 0;
    if (g_pspcid_table.HandleCount > 0) g_pspcid_table.HandleCount--;
}
