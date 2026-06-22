// Process Manager (mini "Ps" do NT). EPROCESS/ETHREAD como objetos do
// Object Manager. Mantem PID/TID, processo corrente e os campos que o
// loader e o usermode precisam (CR3, ImageBase, entry, pilha).
//
// FASE 7.3: agora preenche tambem o ESPELHO do layout NT 10 x64 (offsets
// 0x440 UniqueProcessId, 0x448 ActiveProcessLinks, 0x5A8 ImageFileName, ...).
// Static asserts no fim garantem que os campos estao nos offsets corretos.
#include <stdint.h>
#include <stddef.h>
#include "ps/process.h"
#include "ob/object.h"
#include "ps/cid_table.h"   // FASE 7.5: PspCidTable insercoes/remocoes

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

static uint32_t  s_next_pid;
static uint32_t  s_next_tid;
static PEPROCESS s_current_proc;   // processo rodando em ring 3 agora
static PETHREAD  s_current_thr;

// ----------------------------------------------------------------------------
//  Asserts de compilacao: confirmam os offsets do "espelho NT" no EPROCESS/
//  ETHREAD. Se qualquer um quebrar, o build aborta (e ajustamos o _padX).
//  Usamos _Static_assert do C11 (suportado pelo nosso gcc/clang).
// ----------------------------------------------------------------------------
_Static_assert(offsetof(EPROCESS, ProcessId)                       == 0x000, "EPROCESS.ProcessId@0");
_Static_assert(offsetof(EPROCESS, ImageName)                       == 0x028, "EPROCESS.ImageName@0x28");
_Static_assert(offsetof(EPROCESS, PageFaultsCount)                 == 0x180, "EPROCESS.PageFaultsCount@0x180");
_Static_assert(offsetof(EPROCESS, UniqueProcessId)                 == 0x440, "EPROCESS.UniqueProcessId@0x440");
_Static_assert(offsetof(EPROCESS, ActiveProcessLinks)              == 0x448, "EPROCESS.ActiveProcessLinks@0x448");
_Static_assert(offsetof(EPROCESS, InheritedFromUniqueProcessId)    == 0x4B8, "EPROCESS.Inherited@0x4B8");
_Static_assert(offsetof(EPROCESS, Peb)                             == 0x550, "EPROCESS.Peb@0x550");
_Static_assert(offsetof(EPROCESS, Wow64Process)                    == 0x568, "EPROCESS.Wow64Process@0x568");
_Static_assert(offsetof(EPROCESS, ImageFileName)                   == 0x5A8, "EPROCESS.ImageFileName@0x5A8");
_Static_assert(offsetof(EPROCESS, ActiveThreads)                   == 0x650, "EPROCESS.ActiveThreads@0x650");
_Static_assert(offsetof(EPROCESS, ObjectTable)                     == 0x710, "EPROCESS.ObjectTable@0x710");
_Static_assert(sizeof(EPROCESS) >= 0x800, "EPROCESS deve ter ~2 KiB");

_Static_assert(offsetof(ETHREAD, ThreadId)                         == 0x000, "ETHREAD.ThreadId@0");
_Static_assert(offsetof(ETHREAD, InitialStack)                     == 0x180, "ETHREAD.InitialStack@0x180");
_Static_assert(offsetof(ETHREAD, StackLimit)                       == 0x188, "ETHREAD.StackLimit@0x188");
_Static_assert(offsetof(ETHREAD, KernelStack)                      == 0x190, "ETHREAD.KernelStack@0x190");
_Static_assert(offsetof(ETHREAD, TrapFrame)                        == 0x1F8, "ETHREAD.TrapFrame@0x1F8");
_Static_assert(offsetof(ETHREAD, CreateTime)                       == 0x420, "ETHREAD.CreateTime@0x420");
_Static_assert(offsetof(ETHREAD, Cid_UniqueProcess)                == 0x650, "ETHREAD.Cid@0x650");
_Static_assert(offsetof(ETHREAD, Cid_UniqueThread)                 == 0x658, "ETHREAD.Cid+8");
_Static_assert(offsetof(ETHREAD, ImpersonationInfo)                == 0x6B0, "ETHREAD.Impersonation@0x6B0");
_Static_assert(offsetof(ETHREAD, ThreadName)                       == 0x7C8, "ETHREAD.ThreadName@0x7C8");
_Static_assert(sizeof(ETHREAD) >= 0x800, "ETHREAD deve ter ~2 KiB");

static void copy_name(char* dst, const char* src, int max) {
    int i = 0;
    if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// Copia o nome curto da imagem para ImageFileName[15] (sem terminador
// obrigatorio se preencher os 15 bytes; preenchemos com '\0' se sobrar).
static void copy_image_file_name(uint8_t* dst15, const char* src) {
    int i = 0;
    if (src) while (src[i] && i < 15) { dst15[i] = (uint8_t)src[i]; i++; }
    while (i < 15) { dst15[i] = 0; i++; }
}

void ps_init(void) {
    s_next_pid = 1;
    s_next_tid = 1;
    s_current_proc = 0;
    s_current_thr  = 0;
}

PEPROCESS PsCreateProcess(const char* image_name, uint64_t image_base, uint64_t dir_base) {
    // O objeto nao entra no namespace por nome (varios processos podem ter o
    // mesmo nome de imagem); ficamos so com o corpo + refcount.
    // ObCreateObject zera todo o corpo — entao todos os _padX/espelho ja sao 0.
    PEPROCESS p = (PEPROCESS)ObCreateObject(OB_TYPE_PROCESS, sizeof(EPROCESS), 0);
    if (!p) return 0;

    uint32_t pid = s_next_pid++;

    // ---- Campos legacy (o que o resto do kernel le hoje) ----
    p->ProcessId          = pid;
    p->ThreadCount        = 0;
    p->DirectoryTableBase = dir_base;     // 0 = espaco compartilhado (CR3 atual)
    p->ImageBase          = image_base;
    p->PML4               = 0;
    p->ExitStatus         = 0;
    p->Terminated         = 0;
    p->Thread             = 0;
    copy_name(p->ImageName, image_name, (int)sizeof(p->ImageName));

    // ---- Espelho NT 10 x64 (drivers reais andam por aqui) ----
    p->PageFaultsCount             = 0;
    p->UniqueProcessId             = (HANDLE)(uintptr_t)pid;
    // ActiveProcessLinks como lista circular vazia (Flink/Blink apontam p/ si).
    p->ActiveProcessLinks.Flink    = &p->ActiveProcessLinks;
    p->ActiveProcessLinks.Blink    = &p->ActiveProcessLinks;
    p->InheritedFromUniqueProcessId = (HANDLE)(uintptr_t)0;   // sem parent (system)
    p->Peb                         = 0;                       // sem PEB ring-3 ainda
    p->Wow64Process                = 0;                       // nativo x64
    copy_image_file_name(p->ImageFileName, image_name);
    p->ActiveThreads               = 0;
    p->ObjectTable                 = 0;                       // tabela global compartilhada

    // FASE 7.5: insere na PspCidTable (CID -> EPROCESS). Drivers que chamem
    // PsLookupProcessByProcessId agora encontram em O(1)~ ao inves de varrer
    // toda a lista de objetos.
    cid_insert_process(p);

    kputs("[ps] EPROCESS criado: pid="); kput_dec(p->ProcessId);
    kputs(" img="); kputs(p->ImageName[0] ? p->ImageName : "(sem nome)");
    kputs(" base="); kput_hex(p->ImageBase);
    kputs(" cr3="); kput_hex(p->DirectoryTableBase);
    kputs(" UniqueId@0x440="); kput_hex((uint64_t)(uintptr_t)p->UniqueProcessId);
    kputc('\n');
    return p;
}

PETHREAD PsCreateThread(PEPROCESS proc, uint64_t start, uint64_t stack_top, uint64_t stack_base) {
    PETHREAD t = (PETHREAD)ObCreateObject(OB_TYPE_THREAD, sizeof(ETHREAD), 0);
    if (!t) return 0;

    uint32_t tid = s_next_tid++;

    // ---- Legacy ----
    t->ThreadId     = tid;
    t->Process      = proc;
    t->StartAddress = start;
    t->StackBase    = stack_base;
    t->StackTop     = stack_top;
    t->ExitStatus   = 0;
    t->Terminated   = 0;

    // ---- Espelho NT ----
    // KTHREAD.InitialStack/StackLimit/KernelStack apontam para a pilha de USUARIO
    // (sem scheduler proprio; basta ser nao-nulo p/ drivers que validam).
    t->InitialStack = (void*)(uintptr_t)stack_top;
    t->StackLimit   = (void*)(uintptr_t)stack_base;
    t->KernelStack  = (void*)(uintptr_t)stack_top;
    t->TrapFrame    = 0;
    t->CreateTime   = (int64_t)132934176000000000LL;   // mesmo "boot time" do Ps*
    t->Cid_UniqueProcess = (HANDLE)(uintptr_t)(proc ? proc->ProcessId : 0);
    t->Cid_UniqueThread  = (HANDLE)(uintptr_t)tid;
    t->ImpersonationInfo = 0;
    t->ThreadName        = 0;

    if (proc) {
        proc->ThreadCount++;
        proc->ActiveThreads++;                 // espelho NT (offset 0x650)
        if (!proc->Thread) proc->Thread = t;   // primeira thread = principal
    }

    // FASE 7.5: insere na PspCidTable (CID -> ETHREAD). PsLookupThreadByThreadId
    // tambem usa esta tabela como caminho rapido.
    cid_insert_thread(t);

    kputs("[ps] ETHREAD criado: tid="); kput_dec(t->ThreadId);
    kputs(" pid="); kput_dec(proc ? proc->ProcessId : 0);
    kputs(" entry="); kput_hex(t->StartAddress);
    kputc('\n');
    return t;
}

void PsTerminateProcess(PEPROCESS proc, uint32_t status) {
    if (!proc) return;
    proc->Terminated = 1;
    proc->ExitStatus = status;
    if (proc->Thread) {
        proc->Thread->Terminated = 1;
        proc->Thread->ExitStatus = status;
        // FASE 7.5: remove a thread principal da PspCidTable.
        cid_remove_thread(proc->Thread->ThreadId);
    }
    if (proc->ActiveThreads) proc->ActiveThreads--;
    // FASE 7.5: remove o processo da PspCidTable (libera entrada).
    cid_remove_process(proc->ProcessId);
    kputs("[ps] processo pid="); kput_dec(proc->ProcessId);
    kputs(" encerrou (status="); kput_hex(status); kputs(")\n");
}

PEPROCESS PsGetCurrentProcess(void) { return s_current_proc; }
PETHREAD  PsGetCurrentThread(void)  { return s_current_thr; }

void ps_set_current(PEPROCESS proc, PETHREAD thr) {
    s_current_proc = proc;
    s_current_thr  = thr;
}
