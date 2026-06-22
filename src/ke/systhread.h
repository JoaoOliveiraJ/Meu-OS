#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 7 — System threads (PsCreateSystemThread / PsTerminateSystemThread).
//
//  Sem escalonador, a thread "do sistema" e EXECUTADA INLINE no momento da
//  criacao (mesma pilha do chamador). A maioria das system threads em drivers
//  faz "loop infinito esperando evento" — esses ficariam pendurados; entao
//  oferecemos um *modo de execucao limitado* via systhread_run_once_then_return:
//  setamos uma flag que faz KeDelayExecutionThread / KeWaitForSingleObject
//  pegarem um "timeout pequeno" e a thread sair sozinha apos N iteracoes.
//
//  TODO real: escalonador cooperativo (multi-thread) — esta API esta pronta
//  pra plugar quando ele existir.
// ============================================================================

NTSTATUS NTAPI PsCreateSystemThread_k(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                                      POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                                      PCLIENT_ID ClientId, PKSTART_ROUTINE StartRoutine, PVOID StartContext);
NTSTATUS NTAPI PsTerminateSystemThread_k(NTSTATUS ExitStatus);

PVOID NTAPI PsGetCurrentThreadId_k(void);
PVOID NTAPI PsGetCurrentProcessId_k(void);
PVOID NTAPI PsGetCurrentThread_k(void);
PVOID NTAPI PsGetCurrentProcess_k(void);

PVOID    NTAPI PsGetProcessId_k(PVOID Process);
const char* NTAPI PsGetProcessImageFileName_k(PVOID Process);
PVOID    NTAPI PsGetProcessPeb_k(PVOID Process);
PVOID    NTAPI PsGetProcessWow64Process_k(PVOID Process);
LONGLONG NTAPI PsGetProcessCreateTimeQuadPart_k(PVOID Process);
NTSTATUS NTAPI PsGetProcessExitStatus_k(PVOID Process);
BOOLEAN  NTAPI PsIsProtectedProcess_k(PVOID Process);
BOOLEAN  NTAPI PsIsProtectedProcessLight_k(PVOID Process);

NTSTATUS NTAPI PsLookupProcessByProcessId_k(HANDLE ProcessId, PVOID* Process);
NTSTATUS NTAPI PsLookupThreadByThreadId_k(HANDLE ThreadId, PVOID* Thread);

// Limita quantas iteracoes "infinite-loop" uma system thread roda no headless.
// Drivers que checam um KeEvent num while(1) acabam saindo apos N iteracoes.
void systhread_set_max_iterations(uint32_t n);
uint32_t systhread_iteration_count(void);
