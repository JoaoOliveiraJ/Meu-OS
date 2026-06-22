#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  Named Pipes (IPC) — FASE 3
//
//  Um Named Pipe e um objeto do Object Manager (OB_TYPE_PIPE) com um buffer
//  circular de bytes e um namespace \Pipe\Nome. O servidor cria o pipe
//  (NtCreateNamedPipeFile -> pipe_create), o cliente o abre pelo nome
//  (NtCreateFile com "\\.\pipe\Nome" ou "\Pipe\Nome" -> pipe_open). Os dados
//  trafegam por NtWriteFile/NtReadFile, que num handle de pipe vao para o
//  buffer (pipe_write/pipe_read) — exatamente como no Windows, em que um pipe
//  e um FILE_OBJECT cujas leituras/escritas o file system de pipe (npfs.sys)
//  roteia para o buffer interno.
// ============================================================================

#define PIPE_BUF_SIZE 4096   // buffer circular do pipe (bytes)

// Estados do pipe (subconjunto do Win32: o NT tem mais).
enum {
    PIPE_STATE_DISCONNECTED = 0,   // criado, sem cliente
    PIPE_STATE_LISTENING    = 1,   // ConnectNamedPipe chamado, aguardando
    PIPE_STATE_CONNECTED    = 2,   // cliente conectado (abriu pelo nome)
};

typedef struct _PIPE_OBJECT {
    char     Name[64];             // nome completo no namespace (\Pipe\Nome)
    uint32_t State;                // PIPE_STATE_*
    int32_t  ServerConnected;      // o lado servidor ja chamou ConnectNamedPipe?
    int32_t  ClientConnected;      // algum cliente ja abriu pelo nome?
    // Buffer circular (uma direcao: servidor -> cliente e vice-versa partilham
    // o mesmo buffer FIFO; basta para a demo sequencial sem escalonador).
    uint8_t  Buffer[PIPE_BUF_SIZE];
    uint32_t Head;                 // proxima posicao de leitura
    uint32_t Tail;                 // proxima posicao de escrita
    uint32_t Count;                // bytes atualmente no buffer
} PIPE_OBJECT, *PPIPE_OBJECT;

// Cria um Named Pipe nomeado no namespace (\Pipe\Nome). Aceita "\\.\pipe\Nome",
// "\Pipe\Nome" ou so "Nome": normaliza tudo para "\Pipe\Nome". Devolve o objeto
// (corpo) ja registrado, ou 0 se ja existir / faltar memoria.
PPIPE_OBJECT pipe_create(const char* name);

// Abre um Named Pipe existente pelo nome (lado cliente). Marca como conectado.
// Devolve o objeto, ou 0 se nao existir.
PPIPE_OBJECT pipe_open(const char* name);

// Marca o lado servidor como "conectado" (ConnectNamedPipe). Como nao ha
// escalonador, nao bloqueia: se o cliente ja abriu, retorna STATUS_SUCCESS;
// caso contrario o estado vira LISTENING (a demo conecta o cliente em seguida).
NTSTATUS pipe_connect(PPIPE_OBJECT p);

// Escreve/le no buffer circular do pipe. Retornam bytes efetivamente movidos.
uint32_t pipe_write(PPIPE_OBJECT p, const void* buf, uint32_t len);
uint32_t pipe_read (PPIPE_OBJECT p, void* buf, uint32_t len);

// Normaliza um nome de pipe (qualquer das formas) para "\Pipe\Nome" em 'out'.
void pipe_normalize_name(const char* in, char* out, int outcap);
