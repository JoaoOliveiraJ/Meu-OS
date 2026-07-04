#pragma once
#include <stdint.h>

// ============================================================================
//  sched.h — Escalonador preemptivo MULTI-PROCESSADOR (Pilar 4).
//
//  Espelho minimo do scheduler do NT:
//    - KTHREAD: estado de uma thread. Contem o RSP salvo + ponteiros de
//      ready queue + estado + prioridade.
//    - KPRCB (estendida em sched_prcb_t): per-CPU. Contem a ready queue
//      do CPU + IdleThread + CurrentThread.
//    - KiDispatcherLock: spinlock global cobrindo as ready queues. Aproximacao:
//      o NT real usa per-PRCB queue locks + dispatcher lock. Para esta rodada
//      e' uma trava unica — funciona, e' marcado como APROXIMACAO no relatorio.
//    - KiSwapContext (asm): salva RBX/RBP/R12-R15 + RSP da thread atual; troca
//      RSP para a proxima thread; pop dos callee-saved; ret. Mesmo padrao do
//      Linux/BSD context switch.
//    - APIC timer ISR -> KiQuantumEnd -> KiSwapThread -> KiSwapContext.
//
//  Referencias:
//    Windows Internals 7th ed. cap 4 ("Thread Scheduling")
//    Russinovich/Solomon/Ionescu — 32 prio queues, KiReadySummary, quantum.
//    ReactOS kernel/ke/thrdschd.c — reimplementacao clean-room do mesmo fluxo.
//
//  APROXIMACOES (declaradas explicitamente no relatorio):
//    - 1 unico nivel de prioridade efetivo (sem 32 filas, sem boost/decay).
//    - Round-robin dentro do nivel.
//    - Quantum = 1 tick do APIC timer (10 ms) — sem KTHREAD.QuantumReset.
//    - Sem priority inheritance, sem affinity mask (so int affinity CPU).
//    - Sem KiIdleSummary/KiReadySummary (sao bits do bitmap; com 1 prio nao
//      precisa).
// ============================================================================

// Estados de KTHREAD (subset do NT).
typedef enum {
    KI_THREAD_NEW       = 0,
    KI_THREAD_READY     = 1,
    KI_THREAD_RUNNING   = 2,
    KI_THREAD_WAITING   = 3,
    KI_THREAD_TERMINATED= 4,
} ki_thread_state_t;

// Lista duplamente ligada (estilo NT LIST_ENTRY).
typedef struct ki_list_entry {
    struct ki_list_entry* next;
    struct ki_list_entry* prev;
} ki_list_entry_t;

// KTHREAD.
typedef struct ki_thread {
    // OFFSET 0: RSP salvo. KiSwapContext (asm) le/grava AQUI.
    uint64_t          saved_rsp;
    // Estado e identidade.
    int               state;          // ki_thread_state_t
    int               priority;       // 0..31; usamos uma so prioridade efetiva
    int               cpu_affinity;   // -1 = qualquer; 0..N-1 = CPU especifico
    int               cpu_current;    // CPU onde rodou por ultimo (-1 = nenhum)
    uint32_t          tid;
    // Stack alocada por kmalloc; stack_base..stack_base+stack_size.
    void*             stack_base;
    uint64_t          stack_size;
    // Entry.
    void           (*entry)(void*);
    void*             arg;
    // Links nas filas (ready ou wait).
    ki_list_entry_t   ready_link;
    ki_list_entry_t   wait_link;
    // Wait object (NULL se nao esperando).
    void*             wait_object;
    // Conta quantum atual (decrementado pelo APIC timer).
    int               quantum;
} ki_thread_t;

// Per-CPU PRCB do scheduler.
typedef struct {
    ki_thread_t*      current_thread;
    ki_thread_t*      idle_thread;
    ki_list_entry_t   ready_head;          // sentinel da fila ready do CPU
    uint32_t          apic_id;
    uint32_t          cpu_index;
    int               online;
} ki_prcb_t;

#define MAX_CPUS 8

// Estado global.
extern ki_prcb_t       g_ki_prcb[MAX_CPUS];
extern int             g_ki_cpu_count;     // numero de CPUs realmente online

// -------------------------------- API ------------------------------------

// Inicializa scheduler do CPU corrente (chamado pelo BSP no boot e por cada
// AP em ap_entry apos setup minimo). Aloca a idle thread, instala em PRCB,
// marca online. Lock externo nao necessario — cada CPU configura seu PRCB.
void ki_init_processor(uint32_t cpu_index, uint32_t apic_id);

// Cria uma nova thread no scheduler. Aloca stack, KTHREAD, prepara o
// initial-switch-frame que sera "restaurado" pelo KiSwapContext no primeiro
// dispatch. Retorna ponteiro KTHREAD (nunca NULL — halts se sem RAM).
// 'cpu_affinity' = -1: round-robin entre os CPUs; >=0: fixa.
ki_thread_t* ki_create_thread(void (*entry)(void*), void* arg,
                              int priority, int cpu_affinity);

// Insere a thread na ready queue do seu CPU (afinidade ja decidida em create).
// Se aquele CPU esta idle, manda IPI reschedule (vetor 0xE1).
void ki_ready_thread(ki_thread_t* t);

// Chamada pelo APIC timer ISR (em isr.c). Decrementa quantum; se atingiu 0
// (ou se a thread atual e' idle), faz swap para a proxima ready do CPU.
void ki_quantum_end(void);

// Chamada de codigo que quer ceder CPU voluntariamente. Coloca thread atual
// no fim da ready queue do seu CPU e troca para a proxima ready (ou idle).
void ki_yield_processor(void);

// Idle loop: sti+hlt forever. Eh o entry da idle thread de cada CPU.
void ki_idle_loop(void);

// Acessa a thread atual desta CPU (le pelo KPCR do scheduler ou via GS).
ki_thread_t* ki_current_thread(void);

// Indice da CPU corrente (le do KPCR ou via APIC ID -> tabela).
uint32_t     ki_current_cpu_index(void);

// IPI reschedule handler (chamado em isr.c quando vetor 0xE1 chega).
void ki_ipi_reschedule(void);

// FASE FUNDACAO (Item 5) — waits bloqueantes (infinito) p/ threads worker reais.
int  ki_can_block(void);               // true se a thread corrente pode bloquear
void ki_block_current(void* obj);      // marca WAITING + insere na lista de espera
int  ki_wake(void* obj, int wake_all); // acorda waiters de 'obj' (-> ready)
