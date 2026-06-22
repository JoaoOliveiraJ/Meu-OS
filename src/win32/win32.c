#include <stdint.h>
#include "win32/win32.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_dec(uint64_t v);

// Buffer de "processo terminou" (ver pe.c, __builtin_setjmp).
void* g_pe_exit[5];

static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ===== Implementacoes da Win32 (ABI da Microsoft x64 = ms_abi) =====
// O .exe foi compilado para Windows, entao chama estas funcoes na ABI MS.

__attribute__((ms_abi)) static int WIN_MessageBoxA(
        void* hWnd, const char* text, const char* caption, unsigned int type) {
    (void)hWnd; (void)type;
    kputs("\n  +------------------------- MessageBox -------------------------+\n");
    kputs("  | Titulo: "); kputs(caption ? caption : "(null)"); kputc('\n');
    kputs("  | Texto : "); kputs(text    ? text    : "(null)"); kputc('\n');
    kputs("  +--------------------------------------------------------------+\n");
    return 1; // IDOK
}

__attribute__((ms_abi)) static void WIN_ExitProcess(unsigned int code) {
    kputs("  [kernel] ExitProcess chamado com codigo=");
    kput_dec(code); kputc('\n');
    __builtin_longjmp(g_pe_exit, 1);   // volta ao carregador no kernel
}

// Tabela de exports da "Win32". Crescemos isto conforme o .exe alvo precisar.
static const struct { const char* name; void* fn; } g_exports[] = {
    { "MessageBoxA", (void*)WIN_MessageBoxA },
    { "ExitProcess", (void*)WIN_ExitProcess },
    { 0, 0 }
};

void* win32_resolve(const char* dll, const char* fn) {
    (void)dll;  // resolvemos por nome de funcao, independente da DLL
    for (int i = 0; g_exports[i].name; i++)
        if (streq(g_exports[i].name, fn)) return g_exports[i].fn;
    return 0;
}

void* win32_resolve_ordinal(const char* dll, uint16_t ordinal) {
    (void)dll; (void)ordinal;
    return 0;   // imports por ordinal ainda nao suportados
}
