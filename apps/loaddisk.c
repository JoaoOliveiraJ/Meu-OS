// FRENTE 3 (Fase 3g) — ALVO: carrega uma DLL a partir de um ARQUIVO no disco NTFS
// (C:\testlib.dll), nao de um modulo de boot. Prova LoadLibrary DO DISCO: o kernel
// resolve o caminho no volume NTFS, le os bytes do $DATA (data run NAO-residente),
// mapeia o PE e binda; GetProcAddress + chamada por ponteiro por cima.
// Caminho: LoadLibraryA -> kernel32 -> ntdll (LdrLoadDll) -> int 0x80 -> sys_loadlibrary
//   -> ntfs_resolve_path + ntfs_read_file (le a DLL do disco) -> ldr_load_image (mapeia).
// Exige -Disk (build\disk.img com \testlib.dll, embutido por make-ntfs-disk.ps1).
// Compilado por: zig cc -target x86_64-windows-gnu apps/loaddisk.c (com o CRT).
#include <stdio.h>

__declspec(dllimport) void* LoadLibraryA(const char* name);
__declspec(dllimport) void* GetProcAddress(void* mod, const char* fn);

typedef int         (*add_fn)(int, int);
typedef const char* (*name_fn)(void);

int main(void) {
    printf("  [loaddisk] LoadLibraryA(\"C:\\\\testlib.dll\") a partir do DISCO NTFS...\n");
    void* h = LoadLibraryA("C:\\testlib.dll");
    if (!h) { printf("  [loaddisk] ERRO: LoadLibrary do disco falhou\n"); return 1; }

    add_fn  add = (add_fn) GetProcAddress(h, "testlib_add");
    name_fn nm  = (name_fn)GetProcAddress(h, "testlib_name");
    if (!add || !nm) { printf("  [loaddisk] ERRO: GetProcAddress falhou\n"); return 1; }

    printf("  [loaddisk] carregada DO DISCO; chamei por ponteiro: testlib_add(7,8)=%d ; \"%s\"\n",
           add(7, 8), nm());
    return 0;
}
