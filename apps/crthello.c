// FRENTE 3 (Fase 3b) — ALVO: um .exe de console com o CRT REAL (mingw/msvcrt),
// NAO -nostdlib. Compilado por `zig cc -target x86_64-windows-gnu apps/crthello.c`
// (sem -nostdlib, sem -e _start): o zig linka o startup do mingw (crt2/_initterm/
// __main) + msvcrt. E' o "primeiro binario real" que a Fase 3b precisa rodar: puxa
// PEB/TEB, TLS, __C_specific_handler, GetStartupInfo, Heap*, etc. Este arquivo serve
// primeiro de DIAGNOSTICO (ver que imports faltam) e depois de troféu.
#include <stdio.h>

int main(void) {
    printf("Ola de um .exe com CRT REAL (mingw/msvcrt) rodando no MeuOS!\n");
    return 0;
}
