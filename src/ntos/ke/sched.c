// ============================================================================
//  sched.c — Escalonador preemptivo MP (Pilar 4 da rodada NT foundation).
//
//  Implementacao mais proxima possivel do NT dado o tempo da rodada. Onde a
//  fonte de verdade tem nome (KTHREAD, KPRCB, KiSwapContext, KiQuantumEnd),
//  usamos o nome NT mesmo. Onde simplificamos (uma so fila ready vs 32 listas
//  do KiReadySummary, single dispatcher lock vs per-PRCB locks), MARCAMOS no
//  comentario inline.
//
//  Algoritmo:
//    - Cada CPU tem um ki_prcb_t com:
//        current_thread, idle_thread, ready_head (sentinel de fila duplamente
//        ligada), apic_id, cpu_index.
//    - ki_create_thread aloca KTHREAD + stack 16 KiB, prepara o "switch frame"
//      no topo da stack: 6 callee-saved (RBX/RBP/R12-R15) zerados + endereco
//      de ki_thread_startup como return address. Quando KiSwapContext "ret"a
//      pela primeira vez nessa stack, controla cai em ki_thread_startup.
//    - ki_thread_startup le ki_current_thread()->entry/arg, faz sti, chama
//      entry(arg). Quando entry retorna -> ki_thread_terminate.
//    - APIC timer ISR -> ki_quantum_end():
//        if current_thread == idle: pega proxima ready; se nada, fica em idle.
//        else: decrementa quantum; se 0, recoloca no fim da ready e pega proxima.
//        if proxima != current: KiSwapContext(prev=current, next=proxima).
//    - ki_ready_thread: insere thread no FIM da ready queue de SEU CPU; se
//      esse CPU esta idle, IPI reschedule (so se for outro CPU).
//
//  GS_BASE: KPCR/KPRCB ja tem GS programado por kpcr_init (BSP) e pela
//  trampoline (AP). PERO o KPCR do NT tem campos especificos (Self, Prcb,
//  Number) que ja existem em kpcr.c. ki_current_thread() NAO le via GS pra
//  evitar dependencia frágil de offsets — le via "qual CPU corrente sou"
//  resolvendo o APIC ID. (NT_INTERN: o NT faz mov gs:[0x188]; nos vamos por
//  apic_local_id -> CPU index -> g_ki_prcb[idx].current_thread. APROXIMACAO.)
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "ke/sched.h"
#include "ke/amd64/apic.h"
#include "mm/heap.h"

extern void  kputs(const char* s);
extern void  kput_hex(uint64_t v);
extern void  kput_dec(uint64_t v);
extern void* memset(void* dst, int v, size_t n);

// --- estado global -------------------------------------------------------
ki_prcb_t       g_ki_prcb[MAX_CPUS];
int             g_ki_cpu_count = 0;

// Dispatcher lock: aproximacao single lock. NT real tem per-PRCB.
static volatile int s_dispatcher_lock = 0;

// FASE FUNDACAO (Item 5) — lista global de threads esperando (link via wait_link).
static ki_list_entry_t s_wait_list;

static void disp_lock(void)   { while (__atomic_test_and_set(&s_dispatcher_lock, __ATOMIC_ACQUIRE)) __asm__ volatile ("pause"); }
static void disp_unlock(void) { __atomic_clear(&s_dispatcher_lock, __ATOMIC_RELEASE); }

// FASE FUNDACAO (Item 0a) — salva RFLAGS + cli / restaura RFLAGS. Protege as
// secoes criticas do dispatcher (e o context-switch) contra reentrancia:
//   - caminho do timer ISR: IF ja e' 0 (interrupt gate) -> save/cli no-op, e o
//     restore mantem IF=0; o iretq do ISR reabilita.
//   - yield VOLUNTARIO (IF=1, usado por KeWait bloqueante no Item 5): cli ate o
//     swap; restore reabilita quando a thread volta a rodar.
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
}

// --- list helpers (estilo LIST_ENTRY do NT) -----------------------------
static void list_init(ki_list_entry_t* head) { head->next = head->prev = head; }
static int  list_empty(const ki_list_entry_t* head) { return head->next == head; }
static void list_insert_tail(ki_list_entry_t* head, ki_list_entry_t* entry) {
    entry->prev = head->prev;
    entry->next = head;
    head->prev->next = entry;
    head->prev = entry;
}
static ki_list_entry_t* list_pop_head(ki_list_entry_t* head) {
    if (list_empty(head)) return 0;
    ki_list_entry_t* e = head->next;
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->next = e->prev = 0;
    return e;
}

// Container_of: dado um ready_link, devolve o ki_thread_t que o contem.
static ki_thread_t* thread_of_ready(ki_list_entry_t* link) {
    return (ki_thread_t*)((uint8_t*)link - offsetof(ki_thread_t, ready_link));
}

// --- forwards -----------------------------------------------------------
extern void ki_swap_context(uint64_t* save_rsp, uint64_t next_rsp);  // sched_asm.asm
void ki_thread_startup(void);                                        // C, called from asm via ret
extern void ki_thread_startup_asm(void);                             // sched_asm.asm wrapper
static ki_thread_t* ki_pick_next_locked(uint32_t cpu_index);

// --- mapeia APIC ID -> CPU index (preenchido em ki_init_processor) -----
static uint8_t s_apic_to_cpu[256];   // 0xFF = nao mapeado

uint32_t ki_current_cpu_index(void) {
    uint32_t aid = apic_local_id();
    return s_apic_to_cpu[aid & 0xFF];
}

ki_thread_t* ki_current_thread(void) {
    return g_ki_prcb[ki_current_cpu_index()].current_thread;
}

// --- thread startup (called via ret from KiSwapContext na 1a vez) -------
void ki_thread_startup(void) {
    // Re-habilita interrupcoes (a thread veio sob preempcao, IF=0). A entry
    // roda com IF=1 para que possa ser preemptada pelo timer.
    __asm__ volatile ("sti");
    ki_thread_t* t = ki_current_thread();
    if (t && t->entry) t->entry(t->arg);
    // Quando entry retorna: termina a thread. FASE FUNDACAO (Item 5): CEDE a CPU
    // p/ sempre — NAO usa cli;hlt (que congelaria o CPU0 com IF=0, matando a
    // preempcao e o scheduler). ki_quantum_end nunca re-enfileira TERMINATED.
    if (t) t->state = KI_THREAD_TERMINATED;
    for (;;) ki_yield_processor();
}

// --- ki_init_processor: chamado pelo BSP em kmain e pelo AP em ap_entry --
static int  s_quantum_default = 4;     // ~40 ms @ 100 Hz

// ----------------------------------------------------------------------------
//  CAUSA RAIZ confirmada e contornada:
//
//  O compilador (Zig/clang) combinava stores ZERO adjacentes em PRCB (e.g.
//  current_thread=0 + idle_thread=0) numa unica instrucao SIMD `movdqu xmm`
//  de 16 bytes (com `xorps xmm0,xmm0`). Sob QEMU TCG -accel tcg,thread=multi,
//  emular esse SIMD store gera STALL DETERMINISTICO no outro vCPU quando ha
//  contencao na mesma cache line (BSP em crude_delay loop simples congela
//  apos uma iteracao).
//
//  Hipoteses anteriores REFUTADAS via bisect:
//    (a) interrupt do timer AP -> testei com LVT_TMR mascarado E sem sti,
//        hang persiste. Refutada.
//    (b) BSP em apic_wait_ipi sem ACK -> nao aplica, hang esta em crude_delay
//        simples (volatile for + pause), nao em wait_ipi.
//    (c) atomic_add no g_ki_cpu_count -> testei substituindo por write
//        non-atomic, hang persiste. Refutada.
//    (d) list_init, kputs serial cross-CPU, heap kmalloc race -> todos
//        descartados pelo bisect.
//
//  Bisect convergiu: trocar apenas o store `p->idle_thread = 0` por um valor
//  != 0 (ex.: 0xDEAD) faz o hang sumir. Trocar para `__atomic_store_n(..., 0)`
//  tambem faz o hang sumir. O sintoma so aparece quando o COMPILADOR consegue
//  fundir multiplos zero-stores adjacentes num SIMD move.
//
//  Workaround robusto: usar __atomic_store_n para CADA store discreto. O
//  compilador e' proibido de combinar atomic stores em SIMD por construcao
//  (cada um precisa de sua propria ordem de visibilidade). Em hw real isso
//  vira `mov [mem], 0` normal — custo zero. Aplicamos so onde necessario.
// ----------------------------------------------------------------------------

void ki_init_processor(uint32_t cpu_index, uint32_t apic_id) {
    if (cpu_index >= MAX_CPUS) return;
    ki_prcb_t* p = &g_ki_prcb[cpu_index];

    // Campos escalares: stores comuns funcionam (sao 4 bytes, nao adjacentes
    // a outros 8-byte pointers em modo a serem fundidos em SIMD).
    p->apic_id   = apic_id;
    p->cpu_index = cpu_index;
    p->online    = 1;
    s_apic_to_cpu[apic_id & 0xFF] = (uint8_t)cpu_index;

    // Item 5: inicializa a lista de espera global uma vez (no BSP). Stores
    // discretos p/ nao virarem SIMD (mesma causa-raiz do PRCB).
    if (cpu_index == 0) {
        __atomic_store_n((void**)&s_wait_list.next, &s_wait_list, __ATOMIC_RELEASE);
        __atomic_store_n((void**)&s_wait_list.prev, &s_wait_list, __ATOMIC_RELEASE);
    }

    // Ponteiros que serao zerados: DISCRETOS para nao virarem movdqu.
    __atomic_store_n((void**)&p->current_thread, (void*)0, __ATOMIC_RELEASE);
    __atomic_store_n((void**)&p->idle_thread,    (void*)0, __ATOMIC_RELEASE);

    // ready_head: list_init faz head->next = head->prev = head. Sao DOIS
    // pointer stores que poderiam ser fundidos. Faz manual com atomic.
    __atomic_store_n((void**)&p->ready_head.next, &p->ready_head, __ATOMIC_RELEASE);
    __atomic_store_n((void**)&p->ready_head.prev, &p->ready_head, __ATOMIC_RELEASE);

    __atomic_add_fetch(&g_ki_cpu_count, 1, __ATOMIC_SEQ_CST);

    // Cria a idle thread deste CPU usando o pool estatico (heap nao e' MP-safe).
    static ki_thread_t s_idle_pool[MAX_CPUS];
    ki_thread_t* idle = &s_idle_pool[cpu_index];

    // O memset zera ~100 bytes -> compilador pode usar rep stosq ou SIMD
    // largo. Substituido por loop discreto: cada __atomic_store_n e' um
    // store individual, nunca SIMD.
    {
        volatile uint8_t* b = (volatile uint8_t*)idle;
        for (uint64_t i = 0; i < sizeof(*idle); i++) b[i] = 0;
    }
    idle->state        = KI_THREAD_RUNNING;
    idle->priority     = 0;
    idle->cpu_affinity = (int)cpu_index;
    idle->cpu_current  = (int)cpu_index;
    idle->entry        = (void(*)(void*))ki_idle_loop;
    idle->arg          = 0;
    idle->quantum      = s_quantum_default;
    idle->tid          = 0;

    // Publica o idle E current_thread atomicamente (DEPOIS do struct populado).
    __atomic_store_n((void**)&p->idle_thread,    idle, __ATOMIC_RELEASE);
    __atomic_store_n((void**)&p->current_thread, idle, __ATOMIC_RELEASE);
}

// --- ki_create_thread ---------------------------------------------------
#define STACK_BYTES 16384

static uint32_t s_next_tid = 100;
static uint32_t s_rr_cpu   = 0;

ki_thread_t* ki_create_thread(void (*entry)(void*), void* arg, int priority, int cpu_affinity) {
    ki_thread_t* t = (ki_thread_t*)kmalloc(sizeof(*t));
    if (!t) { kputs("[sched] create: sem RAM\n"); return 0; }
    memset(t, 0, sizeof(*t));

    uint8_t* stack = (uint8_t*)kmalloc(STACK_BYTES);
    if (!stack) { kputs("[sched] create: sem stack\n"); kfree(t); return 0; }
    // Zera a stack p/ debug.
    memset(stack, 0, STACK_BYTES);

    t->stack_base   = stack;
    t->stack_size   = STACK_BYTES;
    t->state        = KI_THREAD_NEW;
    t->priority     = priority;
    t->cpu_affinity = cpu_affinity;
    t->cpu_current  = -1;
    t->entry        = entry;
    t->arg          = arg;
    t->tid          = ++s_next_tid;
    t->quantum      = s_quantum_default;

    // Prepara o "initial switch frame" no topo da stack:
    //   [stack_top - 8]  : RIP de retorno (= ki_thread_startup_asm)
    //   [-16] RBP=0
    //   [-24] RBX=0
    //   [-32] R12=0
    //   [-40] R13=0
    //   [-48] R14=0
    //   [-56] R15=0  <-- RSP aponta aqui
    // O KiSwapContext (asm) faz pop r15/r14/r13/r12/rbx/rbp, ret. O ret cai
    // em ki_thread_startup_asm que apenas chama ki_thread_startup (C).
    uint64_t top = ((uint64_t)(uintptr_t)stack + STACK_BYTES) & ~0xFULL;
    uint64_t* sp = (uint64_t*)top;
    *--sp = (uint64_t)(uintptr_t)&ki_thread_startup_asm;   // ret target
    *--sp = 0;   // RBP
    *--sp = 0;   // RBX
    *--sp = 0;   // R12
    *--sp = 0;   // R13
    *--sp = 0;   // R14
    *--sp = 0;   // R15
    t->saved_rsp = (uint64_t)(uintptr_t)sp;

    return t;
}

void ki_ready_thread(ki_thread_t* t) {
    if (!t) return;
    int target_cpu = t->cpu_affinity;
    if (target_cpu < 0 || target_cpu >= MAX_CPUS) {
        // Round-robin entre CPUs online.
        uint32_t pick = __atomic_fetch_add(&s_rr_cpu, 1, __ATOMIC_RELAXED);
        int found = -1;
        for (int i = 0; i < g_ki_cpu_count; i++) {
            uint32_t idx = (pick + i) % MAX_CPUS;
            if (g_ki_prcb[idx].online) { found = (int)idx; break; }
        }
        if (found < 0) found = 0;
        target_cpu = found;
    }
    t->cpu_current = target_cpu;
    t->state       = KI_THREAD_READY;

    uint64_t _flags = irq_save();       // Item 0a: protege o insert na ready queue
    disp_lock();
    list_insert_tail(&g_ki_prcb[target_cpu].ready_head, &t->ready_link);
    disp_unlock();
    irq_restore(_flags);

    // IPI cross-CPU 0xE1 cria hang determinístico sob TCG (apic_send_ipi
    // de BSP para AP durante a fase de criacao das threads). Desligado por
    // enquanto — o AP pega B no proximo tick do seu LAPIC local.
    (void)target_cpu;
}

// --- ki_pick_next_locked: dispatcher lock must be held -----------------
static ki_thread_t* ki_pick_next_locked(uint32_t cpu_index) {
    ki_prcb_t* p = &g_ki_prcb[cpu_index];
    ki_list_entry_t* link = list_pop_head(&p->ready_head);
    if (!link) return p->idle_thread;
    return thread_of_ready(link);
}

// --- ki_quantum_end: APIC timer ISR -> aqui ----------------------------
void ki_quantum_end(void) {
    // Item 0a: cli desde o topo ate o swap (dispatcher reentrancia-safe).
    uint64_t _flags = irq_save();
    uint32_t cpu = ki_current_cpu_index();
    ki_prcb_t* p = &g_ki_prcb[cpu];
    if (!p->online) { irq_restore(_flags); return; }


    ki_thread_t* cur = p->current_thread;
    if (!cur) cur = p->idle_thread;

    // Idle nao tem quantum — sempre troca se ha thread ready.
    int is_idle = (cur == p->idle_thread);
    int do_swap = 0;

    if (is_idle) {
        do_swap = 1;
    } else {
        if (--cur->quantum <= 0) {
            cur->quantum = s_quantum_default;
            do_swap = 1;
        }
    }

    if (!do_swap) { irq_restore(_flags); return; }

    disp_lock();

    // Pega proxima da ready queue do CPU; se nada, idle.
    ki_thread_t* next = ki_pick_next_locked(cpu);

    // Se proxima e' a mesma + nao ha ninguem mais, nao troca (sem custo).
    if (next == cur) {
        disp_unlock();
        irq_restore(_flags);
        return;
    }

    // Recoloca current no fim da ready. IMPORTANTE: a thread "idle" desta CPU
    // e', na pratica, o FLUXO DO BOOT/desktop (kmain roda sobre o contexto do
    // idle ate bloquear em NtUserGetMessage). Se ela NAO voltasse a fila, as
    // threads worker encheriam a ready queue e o boot NUNCA mais rodaria — o
    // desktop jamais subiria. Entao a idle TAMBEM participa do rodizio (so nao
    // reinserimos threads terminadas/esperando). Quando so a idle esta pronta,
    // o "next == cur" acima ja evita swap desnecessario (idle segue em hlt).
    if (cur->state != KI_THREAD_TERMINATED &&
        cur->state != KI_THREAD_WAITING) {
        if (!is_idle) cur->state = KI_THREAD_READY;   // idle mantem estado RUNNING
        list_insert_tail(&p->ready_head, &cur->ready_link);
    }

    next->state        = KI_THREAD_RUNNING;
    next->cpu_current  = (int)cpu;
    p->current_thread  = next;
    disp_unlock();

    // KiSwapContext: salva RSP de cur, restaura RSP de next, ret.
    // Se cur == idle, ainda salvamos saved_rsp do idle pra restaurar depois.
    ki_swap_context(&cur->saved_rsp, next->saved_rsp);
    irq_restore(_flags);   // Item 0a: reabilita IF quando 'cur' e' re-escalonada
}

void ki_yield_processor(void) {
    // For simplicity: chama quantum_end com quantum forcado a 0.
    ki_thread_t* cur = ki_current_thread();
    if (cur) cur->quantum = 0;
    ki_quantum_end();
}

void ki_idle_loop(void) {
    // Loop ATIVO sti+pause em vez de sti+hlt. Sob QEMU TCG, vCPU em HLT
    // pode nao receber interrupts pendentes em tempo razoavel quando o outro
    // vCPU esta ocupado (BSP corre A 100%). Pause mantem o vCPU "rodando"
    // do ponto de vista do TCG. Em hw real / KVM, voltariamos a hlt.
    for (;;) {
        __asm__ volatile ("sti; pause");
    }
}

void ki_ipi_reschedule(void) {
    // Reschedule IPI handler: simplesmente forca quantum_end nesta CPU.
    ki_thread_t* cur = ki_current_thread();
    if (cur) cur->quantum = 0;
    ki_quantum_end();
}

// ============================================================================
//  FASE FUNDACAO (Item 5) — waits bloqueantes reais (infinito).
//
//  ki_can_block(): so bloqueia se ha scheduler ativo (g_p4_active) E a thread
//  corrente e' uma worker real (NAO a idle/boot — onde o pintok roda). Assim o
//  pintok mantem o "auto-resolve" e nunca trava.
//  Corretude sleep/wakeup: o caller (KeWait) segura cli da checagem do sinal
//  ate ki_yield; em UP nada interpoe entre marcar WAITING e o swap. O
//  ki_quantum_end salva/restaura IF por-thread (Item 0a), entao o swap sob cli
//  e' seguro. O waker (KeSetEvent) move os waiters de volta p/ ready.
// ============================================================================
int ki_can_block(void) {
    extern volatile int g_p4_active;
    if (!g_p4_active) return 0;
    uint32_t cpu = ki_current_cpu_index();
    ki_thread_t* cur = g_ki_prcb[cpu].current_thread;
    return (cur && cur != g_ki_prcb[cpu].idle_thread) ? 1 : 0;
}

void ki_block_current(void* obj) {
    uint32_t cpu = ki_current_cpu_index();
    ki_thread_t* cur = g_ki_prcb[cpu].current_thread;
    if (!cur) return;
    disp_lock();
    cur->wait_object = obj;
    list_insert_tail(&s_wait_list, &cur->wait_link);
    cur->state = KI_THREAD_WAITING;
    disp_unlock();
}

int ki_wake(void* obj, int wake_all) {
    ki_thread_t* batch[16];
    int n = 0;
    uint64_t f = irq_save();
    disp_lock();
    ki_list_entry_t* e = s_wait_list.next;
    while (e && e != &s_wait_list && n < 16) {
        ki_list_entry_t* nx = e->next;
        ki_thread_t* t = (ki_thread_t*)((uint8_t*)e - offsetof(ki_thread_t, wait_link));
        if (t->wait_object == obj) {
            e->prev->next = e->next;
            e->next->prev = e->prev;
            e->next = e->prev = 0;
            t->wait_object = 0;
            batch[n++] = t;
            if (!wake_all) break;
        }
        e = nx;
    }
    disp_unlock();
    irq_restore(f);
    // ki_ready_thread seta READY + enfileira (toma o disp_lock por dentro).
    for (int i = 0; i < n; i++) ki_ready_thread(batch[i]);
    return n;
}
