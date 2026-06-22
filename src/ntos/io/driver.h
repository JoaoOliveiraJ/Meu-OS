#pragma once
#include "ntddk.h"

// Estados de um driver no "registro" do I/O Manager (espelha o SCM do Windows
// para drivers de kernel: SERVICE_STOPPED / SERVICE_RUNNING).
enum {
    DRV_STATE_STOPPED = 1,   // SERVICE_STOPPED
    DRV_STATE_RUNNING = 4,   // SERVICE_RUNNING
};

// Carrega um driver de kernel (.sys) no BOOT: monta o DRIVER_OBJECT, chama
// DriverEntry e (se houver) DriverUnload logo em seguida, e registra o driver
// pelo nome como STOPPED (disponivel para 'sc start' depois). 'name' = nome do
// modulo (ex.: "mydriver.sys"); 'image' = bytes do .sys.
void driver_load(const char* name, const void* image);

// 'sc start <nome>': carrega o .sys pelo nome (via loader), chama DriverEntry e
// deixa o driver RODANDO (nao chama DriverUnload). Devolve STATUS_SUCCESS (0) se
// o DriverEntry retornou sucesso, ou um codigo de erro. Idempotente: se ja estiver
// rodando, retorna sucesso sem recarregar.
NTSTATUS driver_load_by_name(const char* name);

// 'sc stop <nome>': chama o DriverUnload do driver rodando e o marca STOPPED.
NTSTATUS driver_unload_by_name(const char* name);

// Enumeracao para 'sc query'. Devolve 1 e preenche name/state/laststatus do
// n-esimo driver conhecido (0,1,2,...), ou 0 quando 'index' passa do ultimo.
int driver_enum(int index, const char** name, uint32_t* state, NTSTATUS* laststatus);
