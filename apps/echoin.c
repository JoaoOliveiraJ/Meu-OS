// FRENTE 3 (Fase 3c) — ALVO: um .exe com o CRT REAL do mingw que LE do teclado
// via scanf() DE VERDADE. O caminho completo da ENTRADA:
//   main -> scanf -> __stdio_common_vfscanf (na nossa ucrtbase.dll) -> ucrt_getc_raw
//   -> ReadFile (kernel32) -> NtReadFile -> fila de stdin do teclado (IRQ1, keyboard.c).
// O bloqueio ("espera voce digitar") e' feito em ring 3 dentro da ucrtbase: ela gira
// chamando ReadFile ate a tecla chegar (o kernel segue nao-bloqueante p/ o cmd.exe).
// A saida continua pelo printf real (Fase 3b). Compilado por
//   `zig cc -target x86_64-windows-gnu apps/echoin.c` (com o CRT).
#include <stdio.h>

int main(void) {
    int  n = 0;
    char word[64] = { 0 };

    printf("  [echoin] digite: <numero> <palavra> e Enter\n");
    int got = scanf("%d %s", &n, word);
    printf("  [echoin] scanf casou %d campos\n", got);
    printf("  [echoin] numero=%d (dobro=%d), palavra=%s\n", n, n * 2, word);
    return 0;
}
