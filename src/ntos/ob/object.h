#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void* HANDLE;

// Tipos de objeto (como no NT: Process, Thread, Device, File, Event...).
enum {
    OB_TYPE_NONE = 0,
    OB_TYPE_DIRECTORY,
    OB_TYPE_DEVICE,
    OB_TYPE_DRIVER,
    OB_TYPE_FILE,
    OB_TYPE_PROCESS,    // EPROCESS (nt/process.c)
    OB_TYPE_THREAD,     // ETHREAD  (nt/process.c)
    OB_TYPE_EVENT,
    OB_TYPE_PIPE,       // PIPE_OBJECT (nt/pipe.c): Named Pipe (IPC) no namespace
};

void  ob_init(void);

// Cria um objeto (header + corpo). Se 'name' != 0, registra no namespace.
// Retorna o ponteiro para o CORPO (igual ao NT).
void* ObCreateObject(uint32_t type, size_t body_size, const char* name);
void  ObReferenceObject(void* body);
void  ObDereferenceObject(void* body);
void* ObLookupObject(const char* name);      // procura por nome -> corpo (ou 0)
uint32_t ob_type_of(void* body);

// Handles (tabela global; por-processo virá com o Process Manager).
HANDLE ob_create_handle(void* body);
void*  ob_handle_to_object(HANDLE h, uint32_t type);   // 0 se invalido / tipo errado
void   ob_close_handle(HANDLE h);

// Enumeracao de objetos por tipo (para NtQuerySystemInformation/tasklist/sc query).
// Itera TODOS os objetos vivos do tipo dado (inclusive os sem nome, como EPROCESS
// e DRIVER_OBJECT). 'index' e o n-esimo objeto daquele tipo (0,1,2,...); devolve o
// corpo do objeto ou 0 quando 'index' passa do ultimo. ob_count_by_type devolve
// quantos objetos vivos existem daquele tipo.
void* ob_enum_by_type(uint32_t type, int index);
int   ob_count_by_type(uint32_t type);
