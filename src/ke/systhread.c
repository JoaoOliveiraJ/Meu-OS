// ============================================================================
//  FASE 7 — implementacao das system threads + Ps* de inspecao.
// ============================================================================
#include "ke/systhread.h"
#include "nt/process.h"
#include "nt/cid_table.h"   // FASE 7.5: PspCidTable (caminho rapido)

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

static uint32_t s_max_iter = 4;   // numero de iteracoes "infinitas" antes de sair
static uint32_t s_iter_now = 0;

void systhread_set_max_iterations(uint32_t n) { s_max_iter = n; }
uint32_t systhread_iteration_count(void) { return s_iter_now; }

NTSTATUS NTAPI PsCreateSystemThread_k(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                                      POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                                      PCLIENT_ID ClientId, PKSTART_ROUTINE StartRoutine, PVOID StartContext) {
    (void)DesiredAccess; (void)ObjectAttributes; (void)ProcessHandle;
    if (!StartRoutine) return STATUS_INVALID_PARAMETER;

    static uint32_t s_tid = 100;
    uint32_t tid = ++s_tid;
    if (ClientId) {
        ClientId->UniqueProcess = (HANDLE)(uintptr_t)1;
        ClientId->UniqueThread  = (HANDLE)(uintptr_t)tid;
    }
    if (ThreadHandle) *ThreadHandle = (HANDLE)(uintptr_t)tid;

    kputs("[sys] PsCreateSystemThread -> tid="); kput_dec(tid);
    kputs(" start="); kput_hex((uint64_t)(uintptr_t)StartRoutine); kputc('\n');

    // EXECUCAO INLINE: chama o entry da thread imediatamente. Sem scheduler.
    // O test driver tipico checa um KEVENT num loop — KeDelayExecutionThread/
    // KeWaitForSingleObject sao "auto-resolve" no nosso kernel, entao a thread
    // roda algumas iteracoes e retorna naturalmente.
    s_iter_now = 0;
    StartRoutine(StartContext);
    kputs("[sys] system thread tid="); kput_dec(tid); kputs(" retornou.\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI PsTerminateSystemThread_k(NTSTATUS ExitStatus) {
    (void)ExitStatus;
    // Sem scheduler, retornar normal volta ao chamador. NAO faz longjmp; o driver
    // espera "esta funcao nunca retorna" — mas como o chamamos inline, ele continua.
    // Drivers reais chamam isso ao FINAL da rotina; entao o retorno daqui = OK.
    return STATUS_SUCCESS;
}

PVOID NTAPI PsGetCurrentThreadId_k(void) {
    PETHREAD t = PsGetCurrentThread();
    return t ? (PVOID)(uintptr_t)t->ThreadId : (PVOID)(uintptr_t)1;
}
PVOID NTAPI PsGetCurrentProcessId_k(void) {
    PEPROCESS p = PsGetCurrentProcess();
    return p ? (PVOID)(uintptr_t)p->ProcessId : (PVOID)(uintptr_t)1;
}
PVOID NTAPI PsGetCurrentThread_k(void)  { return (PVOID)PsGetCurrentThread();  }
PVOID NTAPI PsGetCurrentProcess_k(void) { return (PVOID)PsGetCurrentProcess(); }

PVOID NTAPI PsGetProcessId_k(PVOID Process) {
    PEPROCESS p = (PEPROCESS)Process;
    return p ? (PVOID)(uintptr_t)p->ProcessId : 0;
}
const char* NTAPI PsGetProcessImageFileName_k(PVOID Process) {
    PEPROCESS p = (PEPROCESS)Process;
    return (p && p->ImageName[0]) ? p->ImageName : "System";
}
PVOID NTAPI PsGetProcessPeb_k(PVOID Process) {
    PEPROCESS p = (PEPROCESS)Process;
    return p ? (PVOID)(uintptr_t)p->ImageBase : 0;
}
PVOID NTAPI PsGetProcessWow64Process_k(PVOID Process) { (void)Process; return 0; }
LONGLONG NTAPI PsGetProcessCreateTimeQuadPart_k(PVOID Process) {
    (void)Process;
    // Tempo fixo "do boot" (sem RTC). TODO: se ligar RTC, retornar real.
    return (LONGLONG)132934176000000000ULL;
}
NTSTATUS NTAPI PsGetProcessExitStatus_k(PVOID Process) {
    PEPROCESS p = (PEPROCESS)Process;
    return p ? (NTSTATUS)p->ExitStatus : STATUS_SUCCESS;
}
BOOLEAN NTAPI PsIsProtectedProcess_k(PVOID Process)      { (void)Process; return 0; }
BOOLEAN NTAPI PsIsProtectedProcessLight_k(PVOID Process) { (void)Process; return 0; }

NTSTATUS NTAPI PsLookupProcessByProcessId_k(HANDLE ProcessId, PVOID* Process) {
    uint32_t pid = (uint32_t)(uintptr_t)ProcessId;

    // FASE 7.5: tenta o caminho rapido via PspCidTable (array linear pequeno).
    PEPROCESS hit = cid_lookup_process(pid);
    if (hit) { if (Process) *Process = hit; return STATUS_SUCCESS; }

    // Fallback: varredura linear pelos objetos do Object Manager (compat).
    // Mantemos este caminho como rede de seguranca — se algum processo nao
    // foi inserido na CID table por qualquer razao, ainda achamos aqui.
    extern void* ob_enum_by_type(uint32_t type, int index);
    for (int i = 0; ; i++) {
        PEPROCESS p = (PEPROCESS)ob_enum_by_type(/*OB_TYPE_PROCESS*/5, i);
        if (!p) break;
        if (p->ProcessId == pid) { if (Process) *Process = p; return STATUS_SUCCESS; }
    }
    if (Process) *Process = 0;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
NTSTATUS NTAPI PsLookupThreadByThreadId_k(HANDLE ThreadId, PVOID* Thread) {
    uint32_t tid = (uint32_t)(uintptr_t)ThreadId;

    // FASE 7.5: caminho rapido via PspCidTable.
    PETHREAD hit = cid_lookup_thread(tid);
    if (hit) { if (Thread) *Thread = hit; return STATUS_SUCCESS; }

    // Fallback: varredura linear (compat).
    extern void* ob_enum_by_type(uint32_t type, int index);
    for (int i = 0; ; i++) {
        PETHREAD t = (PETHREAD)ob_enum_by_type(/*OB_TYPE_THREAD*/6, i);
        if (!t) break;
        if (t->ThreadId == tid) { if (Thread) *Thread = t; return STATUS_SUCCESS; }
    }
    if (Thread) *Thread = 0;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
