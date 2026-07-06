// FRENTE 3 (Fase 3b) — ALVO: um .exe com o CRT REAL do mingw (NAO -nostdlib), mas
// cujo main() escreve via WriteFile (kernel32) em vez de printf. Assim exercitamos o
// STARTUP do CRT (argv/env/_initterm/_cexit/exit + __C_specific_handler) — a prova de
// que rodamos um binario de CRT real — sem puxar o stdio pesado do CRT. Alvo: imprimir
// e sair com codigo 0. Compilado por `zig cc -target x86_64-windows-gnu apps/crthello.c`.
#include <windows.h>

int main(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    static const char s[] =
        "  [crthello] Ola de um .exe com CRT REAL (startup mingw) rodando no MeuOS!\n";
    DWORD written = 0;
    WriteFile(h, s, (DWORD)(sizeof(s) - 1), &written, NULL);
    return 0;
}
