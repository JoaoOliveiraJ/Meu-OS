// FRENTE 4 (Fase 4b) — ALVO: um .exe PAI que LANCA um filho a partir de um ARQUIVO no
// DISCO NTFS (C:\child.exe), nao de um modulo de boot. Prova CreateProcess DO DISCO: o
// kernel resolve o caminho no volume NTFS, le os bytes do $DATA (data run nao-residente),
// mapeia/reloca o PE e roda; o PAI espera e continua.
// Caminho: CreateProcessA -> kernel32 -> ntdll (NtCreateProcess) -> int 0x80 ->
//   sys_createprocess (detecta C:\ -> ntfs_resolve_path + ntfs_read_file -> run_child_image).
// Exige -Disk (build\disk.img com \child.exe, embutido por make-ntfs-disk.ps1).
// Compilado por: zig cc -target x86_64-windows-gnu apps/parentdisk.c (com o CRT).
#include <stdio.h>

typedef struct { void* hProcess; void* hThread; unsigned pid; unsigned tid; } PROCINFO;

__declspec(dllimport) int      CreateProcessA(const char* app, char* cmdline, void* pa, void* ta,
        int inherit, unsigned flags, void* env, const char* cwd, void* si, void* pi);
__declspec(dllimport) unsigned WaitForSingleObject(void* handle, unsigned timeout_ms);
__declspec(dllimport) int      CloseHandle(void* handle);

int main(void) {
    // Linha de comando REAL para o filho do disco (o CRT do filho parseia em argv[]).
    char cmd[] = "child.exe do-disco 7";
    printf("[parent-disk] lancando C:\\child.exe do DISCO com cmdline \"%s\"...\n", cmd);

    char si[104];
    for (int i = 0; i < 104; i++) si[i] = 0;
    *(unsigned*)si = 104;

    PROCINFO pi = { 0, 0, 0, 0 };
    int ok = CreateProcessA("C:\\child.exe", cmd, 0, 0, 0, 0, 0, 0, si, &pi);
    if (!ok) { printf("[parent-disk] ERRO: CreateProcess falhou\n"); return 1; }

    WaitForSingleObject(pi.hProcess, 0xFFFFFFFFu);   // INFINITE
    printf("[parent-disk] filho (do disco) terminou, continuo executando.\n");

    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);
    return 0;
}
