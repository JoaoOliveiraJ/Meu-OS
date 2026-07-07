// FRENTE 4 (Fase 4a) — ALVO: um .exe FILHO simples, com o CRT REAL do mingw. Serve para
// ser LANCADO por outro processo (parent.exe) via CreateProcess. Prova que o filho
// REALMENTE executa (imprime na tela), e nao apenas vira um objeto EPROCESS inerte.
// IMPORTANTE: e' compilado com ImageBase ALTO (0x140000000) + .reloc. O pai fica na
// base padrao de EXE (0x400000); como a faixa baixa de 1 GiB e' identity-mapped e
// COMPARTILHADA entre os CR3s por-processo, se o filho tambem usasse 0x400000 ele
// sobrescreveria FISICAMENTE a imagem do pai. Com base alta, o ldr_run reloca o filho
// para uma regiao PMM distinta (>= 64 MiB) e os dois coexistem.
// Compilado por: zig cc -target x86_64-windows-gnu -Wl,--image-base=0x140000000
//                -Wl,--dynamicbase apps/child.c (com o CRT).
#include <stdio.h>

int main(void) {
    printf("  [child] estou vivo, rodando como processo filho!\n");
    printf("  [child] (2+2=%d) — terminando com codigo 0.\n", 2 + 2);
    return 0;
}
