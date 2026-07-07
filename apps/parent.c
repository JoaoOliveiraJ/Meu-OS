// FRENTE 4 (Fase 4a) — ALVO: um .exe PAI que LANCA outro .exe (child.exe) via
// CreateProcess e ESPERA ele terminar (WaitForSingleObject), depois CONTINUA.
// Caminho: CreateProcessA -> kernel32 -> ntdll (NtCreateProcess) -> int 0x80 ->
//   sys_createprocess (que passa a REALMENTE rodar a imagem do filho, de forma sincrona).
// Ate a Fase 4a, sys_createprocess so criava o objeto EPROCESS e NAO rodava a imagem;
// entao este PAI imprimia "lancando" e "terminou" mas o filho nunca aparecia.
// Compilado por: zig cc -target x86_64-windows-gnu apps/parent.c (com o CRT).
#include <stdio.h>

// PROCESS_INFORMATION: o kernel32 preenche hProcess (slot 0) e hThread (slot 1).
typedef struct { void* hProcess; void* hThread; unsigned pid; unsigned tid; } PROCINFO;

__declspec(dllimport) int      CreateProcessA(const char* app, char* cmdline, void* pa, void* ta,
        int inherit, unsigned flags, void* env, const char* cwd, void* si, void* pi);
__declspec(dllimport) unsigned WaitForSingleObject(void* handle, unsigned timeout_ms);
__declspec(dllimport) int      CloseHandle(void* handle);

int main(void) {
    // Linha de comando REAL passada ao filho; o CRT do filho a parseia em argv[]
    // (argv[0] por convencao = nome do programa). Buffer mutavel (o kernel copia).
    char cmd[] = "child.exe alpha beta 42";
    printf("[parent] lancando o filho com cmdline \"%s\"...\n", cmd);

    // STARTUPINFOA zerado com cb=104 (o kernel32 ignora, mas passamos como no Windows).
    char si[104];
    for (int i = 0; i < 104; i++) si[i] = 0;
    *(unsigned*)si = 104;

    PROCINFO pi = { 0, 0, 0, 0 };
    int ok = CreateProcessA("child.exe", cmd, 0, 0, 0, 0, 0, 0, si, &pi);
    if (!ok) { printf("[parent] ERRO: CreateProcess falhou\n"); return 1; }

    // Espera o filho terminar. Sem escalonador, o filho ja rodou ate o fim dentro do
    // CreateProcess, entao isto retorna imediatamente (WAIT_OBJECT_0).
    WaitForSingleObject(pi.hProcess, 0xFFFFFFFFu);   // INFINITE

    printf("[parent] filho terminou, continuo executando.\n");

    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);
    return 0;
}
