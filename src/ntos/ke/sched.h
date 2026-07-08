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

    // ------------------------------------------------------------------
    // FRENTE THREADS RING-3 PREEMPTIVAS
    // ------------------------------------------------------------------
    // Uma thread ring-3 e' um KTHREAD normal cujo entry de KERNEL (ring3_trampoline)
    // faz IRETQ p/ ring 3 e roda codigo de usuario (a threadproc do CreateThread do
    // explorer). Enquanto ela roda em ring 3, o timer a preempta como qualquer thread;
    // o trap usa a pilha de KERNEL DESTA thread (via TSS.rsp0) e o gs.base DELA (TEB).
    // O escalonador (ki_quantum_end) restaura esses dois por-thread a cada troca.
    uint64_t          is_ring3;      // 1 = roda em ring 3 (tem user_teb/kstack_top validos)
    uint64_t          user_teb;      // gs.base a programar ao escalonar esta thread (TEB de ring 3)
    uint64_t          kstack_top;    // topo da pilha de KERNEL desta thread -> TSS.rsp0 ao escalonar
    uint64_t          user_start;    // RIP inicial de ring 3 (threadproc do CreateThread)
    uint64_t          user_param;    // 1o arg de ring 3 (lpParameter), em rcx
    uint64_t          user_stack_top;// topo da pilha de USUARIO (ring 3) desta thread
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

// ---------------------------------------------------------------------------
// FRENTE THREADS RING-3 PREEMPTIVAS — API
// ---------------------------------------------------------------------------
// g_ring3_active: liga a logica de troca de TSS.rsp0 + gs.base por-thread no
// ki_quantum_end. Fica 0 ate a 1a thread ring-3 nascer. No cenario do pintok
// (sem threads ring-3) NUNCA liga -> ki_quantum_end roda identico ao baseline
// (regra de ouro: nao mexer no caminho do pintok). So o CPU 0 roda ring 3.
extern volatile int g_ring3_active;

// Marca a thread CORRENTE como "roda em ring 3" com este TEB. Chamado por
// usermode_enter p/ a thread principal do explorer (que roda sobre a idle/boot):
// assim, ao voltar a escalona-la, o ki_quantum_end restaura gs.base=TEB e
// TSS.rsp0=pilha de kernel dela. kstack_top = TSS.rsp0 atual (pilha do boot).
void ki_mark_current_ring3(uint64_t user_teb);

// Cria e enfileira uma thread RING-3 preemptiva de verdade: aloca pilha de
// usuario + TEB + stub de retorno, cria um KTHREAD (pilha de kernel propria)
// cujo entry (ring3_trampoline, em usermode.c) faz IRETQ p/ user_start(param)
// em ring 3. Fixada no CPU 0 (TSS unico). Retorna o KTHREAD (NULL se sem RAM).
ki_thread_t* ki_launch_ring3_thread(uint64_t user_start, uint64_t user_param);

// Termina a thread CORRENTE (chamado pelo stub de retorno via sys_thread_exit
// quando a threadproc de ring 3 retorna): marca TERMINATED, acorda quem espera
// no handle dela, e cede a CPU p/ sempre (nunca mais escalonada). NAO retorna.
void ki_ring3_thread_exit(void);

// true se a thread (por ponteiro KTHREAD) ja terminou. Usado pelo
// WaitForSingleObject cooperativo da thread principal (idle nao pode bloquear).
int  ki_thread_is_terminated(ki_thread_t* t);
