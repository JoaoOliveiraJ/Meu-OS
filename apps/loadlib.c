// FRENTE 3 (Fase 3f) — ALVO: um .exe com CRT REAL que carrega uma DLL EM RUNTIME via
// LoadLibraryA + GetProcAddress e chama a funcao POR PONTEIRO (nao por import estatico).
// Caminho: LoadLibraryA -> kernel32 -> ntdll (LdrLoadDll) -> int 0x80 -> sys_loadlibrary
//   -> ldr_load (mapeia a testlib.dll no espaco do usuario) -> base; depois
//   GetProcAddress(base, "testlib_add") -> ponteiro -> chamada direta.
// A testlib.dll e' passada como modulo de boot mas NAO e' importada estaticamente aqui.
// Compilado por: zig cc -target x86_64-windows-gnu apps/loadlib.c (com o CRT).
#include <stdio.h>

__declspec(dllimport) void* LoadLibraryA(const char* name);
__declspec(dllimport) void* GetProcAddress(void* mod, const char* fn);

typedef int         (*add_fn)(int, int);
typedef const char* (*name_fn)(void);

int main(void) {
    printf("  [loadlib] LoadLibraryA(\"testlib.dll\") em runtime...\n");
    void* h = LoadLibraryA("testlib.dll");
    if (!h) { printf("  [loadlib] ERRO: LoadLibrary falhou\n"); return 1; }
    printf("  [loadlib] carregada; base=%p. GetProcAddress...\n", h);

    add_fn  add = (add_fn) GetProcAddress(h, "testlib_add");
    name_fn nm  = (name_fn)GetProcAddress(h, "testlib_name");
    if (!add || !nm) { printf("  [loadlib] ERRO: GetProcAddress falhou\n"); return 1; }

    int s = add(2, 3);
    printf("  [loadlib] chamei POR PONTEIRO: testlib_add(2,3)=%d ; testlib_name()=\"%s\"\n",
           s, nm());
    return 0;
}
