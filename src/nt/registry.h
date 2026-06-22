#pragma once
#include "ntddk.h"

// ============================================================================
//  FASE 7 — Registro (hive em memoria) estilo \Registry\Machine\...
//
//  Implementacao minimalista: arvore de "chaves" com lista de "valores"
//  (nome + tipo + bytes), em memoria. Persistencia opcional p/ NTFS no futuro
//  (TODO). As syscalls NtOpenKey/NtCreateKey/NtSetValueKey/NtQueryValueKey/
//  NtEnumerateKey aceitam caminhos absolutos (\Registry\Machine\Software\Foo)
//  ou relativos a uma chave (RootDirectory != 0).
// ============================================================================

#define REG_NONE        0
#define REG_SZ          1
#define REG_EXPAND_SZ   2
#define REG_BINARY      3
#define REG_DWORD       4
#define REG_DWORD_BE    5
#define REG_LINK        6
#define REG_MULTI_SZ    7
#define REG_QWORD       11

void registry_init(void);

// FASE 7.11: pre-cria \Registry\Machine\System\CurrentControlSet\Services\<name>
// com Type/Start/ErrorControl/ImagePath + subchave Parameters. Hipotese: drivers
// reais (nao packados) que abrem essa chave no DriverEntry encontram o caminho
// valido e nao bailam com STATUS_NOT_FOUND.
void registry_create_driver_service(const char* drv_basename);

NTSTATUS NTAPI NtCreateKey_k(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                             ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition);
NTSTATUS NTAPI NtOpenKey_k  (PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS NTAPI NtCloseKey_k (HANDLE KeyHandle);
NTSTATUS NTAPI NtDeleteKey_k(HANDLE KeyHandle);
NTSTATUS NTAPI NtEnumerateKey_k(HANDLE KeyHandle, ULONG Index, ULONG KeyInformationClass,
                                PVOID KeyInformation, ULONG Length, PULONG ResultLength);
NTSTATUS NTAPI NtEnumerateValueKey_k(HANDLE KeyHandle, ULONG Index, ULONG KeyValueInfoClass,
                                     PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
NTSTATUS NTAPI NtSetValueKey_k(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex,
                               ULONG Type, PVOID Data, ULONG DataSize);
NTSTATUS NTAPI NtQueryValueKey_k(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG KeyValueInfoClass,
                                 PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
NTSTATUS NTAPI NtDeleteValueKey_k(HANDLE KeyHandle, PUNICODE_STRING ValueName);
NTSTATUS NTAPI NtFlushKey_k(HANDLE KeyHandle);

// Helpers ASCII (uso interno + cmd.exe).
int  registry_set_value_ascii(const char* keypath, const char* valuename, uint32_t type, const void* data, uint32_t size);
int  registry_get_value_ascii(const char* keypath, const char* valuename, uint32_t* outType, void* outData, uint32_t maxSize, uint32_t* outSize);
void registry_dump(void);   // imprime a arvore na serial p/ debug
