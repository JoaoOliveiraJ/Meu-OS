#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 7 — Kernel Callback Registration (Ps/Ob/Cm/Ex).
//
//  Os drivers do Windows registram callbacks no kernel para serem notificados
//  quando processos/threads/imagens sao criados, quando objetos sao acessados
//  por handle ou quando o registro e tocado. O Windows mantem listas globais
//  e percorre cada uma no momento certo (durante PspInsertProcess, etc.).
//
//  Aqui implementamos as TABELAS de registro (o lado driver). O FIRE delas no
//  momento certo do kernel e wiring incremental (ver IMPLEMENTED.md FASE 7):
//  callbacks_fire_process_create / callbacks_fire_image_load / ... sao chamados
//  pelo loader/process manager quando aquele evento acontece.
//
//  Sem escalonador (varias .exe rodam em sequencia) a maioria dos eventos so
//  fire 1x no ciclo de vida do .exe — suficiente para o driver "ver" o sistema
//  funcionar e nao quebrar. Cada registro/fire e logado na serial (regra 4).
// ============================================================================

void callbacks_init(void);

// ---- Ps* (process / thread / image) ----
NTSTATUS NTAPI PsSetCreateProcessNotifyRoutine_k(PCREATE_PROCESS_NOTIFY_ROUTINE n, BOOLEAN Remove);
NTSTATUS NTAPI PsSetCreateProcessNotifyRoutineEx_k(PCREATE_PROCESS_NOTIFY_ROUTINE_EX n, BOOLEAN Remove);
NTSTATUS NTAPI PsSetCreateThreadNotifyRoutine_k(PCREATE_THREAD_NOTIFY_ROUTINE n);
NTSTATUS NTAPI PsRemoveCreateThreadNotifyRoutine_k(PCREATE_THREAD_NOTIFY_ROUTINE n);
NTSTATUS NTAPI PsSetLoadImageNotifyRoutine_k(PLOAD_IMAGE_NOTIFY_ROUTINE n);
NTSTATUS NTAPI PsRemoveLoadImageNotifyRoutine_k(PLOAD_IMAGE_NOTIFY_ROUTINE n);

// ---- Ob* (object handle filtering) ----
NTSTATUS NTAPI ObRegisterCallbacks_k(POB_CALLBACK_REGISTRATION reg, PVOID* RegistrationHandle);
void     NTAPI ObUnRegisterCallbacks_k(PVOID RegistrationHandle);

// ---- Cm* (registry) + Ex* (generic) ----
NTSTATUS NTAPI CmRegisterCallback_k(PEX_CALLBACK_FUNCTION cb, PVOID Context, PLARGE_INTEGER Cookie);
NTSTATUS NTAPI CmRegisterCallbackEx_k(PEX_CALLBACK_FUNCTION cb, PUNICODE_STRING Altitude,
                                      PVOID Driver, PVOID Context, PLARGE_INTEGER Cookie, PVOID Reserved);
NTSTATUS NTAPI CmUnRegisterCallback_k(LARGE_INTEGER Cookie);
PVOID    NTAPI ExRegisterCallback_k(PVOID CallbackObject, PEX_CALLBACK_FUNCTION cb, PVOID Context);
void     NTAPI ExUnregisterCallback_k(PVOID Registration);

// ---- Disparadores (kernel-side; nao expostos como API ms_abi) ----
void callbacks_fire_process_create(uint32_t parent_pid, uint32_t pid, int create, const char* image_name);
void callbacks_fire_thread_create(uint32_t pid, uint32_t tid, int create);
void callbacks_fire_image_load(const char* image_name, uint32_t pid, void* base, uint32_t size);
void callbacks_fire_registry_op(uint32_t op, const char* key_name);

// Telemetria simples: quantos callbacks ativos por tipo (para os logs do test driver).
int callbacks_count_process(void);
int callbacks_count_thread(void);
int callbacks_count_image(void);
int callbacks_count_ob(void);
int callbacks_count_cm(void);
