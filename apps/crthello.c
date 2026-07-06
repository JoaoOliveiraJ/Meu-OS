// FRENTE 3 (Fase 3b) — ALVO: um .exe com o CRT REAL do mingw (NAO -nostdlib) que
// imprime via printf() DE VERDADE. O caminho completo:
//   main -> printf -> __stdio_common_vfprintf (na nossa ucrtbase.dll) -> formata os
//   argumentos (va_list) -> WriteFile (kernel32) -> NtWriteFile -> console do kernel.
// O startup do mingw (argv/env/_initterm/exit/__C_specific_handler) roda via ucrtbase.
// Compilado por `zig cc -target x86_64-windows-gnu apps/crthello.c` (com o CRT).
#include <stdio.h>

int main(void) {
    printf("  [crthello] Ola de um .exe com CRT REAL (mingw) rodando no MeuOS!\n");
    printf("  [crthello] printf de verdade: 2+2=%d, hex=0x%x, str=%s, char=%c\n",
           4, 255, "MeuOS", '!');
    return 0;
}
