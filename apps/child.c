// FRENTE 4 (Fase 4a) — ALVO: um .exe FILHO simples, com o CRT REAL do mingw. Serve para
// ser LANCADO por outro processo (parent.exe) via CreateProcess. Prova que o filho
// REALMENTE executa (imprime na tela), e nao apenas vira um objeto EPROCESS inerte.
// Compilado por: zig cc -target x86_64-windows-gnu apps/child.c (com o CRT).
#include <stdio.h>

int main(void) {
    printf("  [child] estou vivo, rodando como processo filho!\n");
    printf("  [child] (2+2=%d) — terminando com codigo 0.\n", 2 + 2);
    return 0;
}
