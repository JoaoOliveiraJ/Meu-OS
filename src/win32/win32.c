// Lado KERNEL (ring 0) do subsistema Win32. Estas funcoes sao chamadas pelo
// despacho de syscalls (ke/syscall.c) quando o stub de ring 3 faz int 0x80.
#include "win32/win32.h"

extern void kputs(const char* s);
extern void kputc(char c);

void win32k_messagebox(const char* text, const char* caption) {
    kputs("\n  +------------------------- MessageBox -------------------------+\n");
    kputs("  | Titulo: "); kputs(caption ? caption : "(null)"); kputc('\n');
    kputs("  | Texto : "); kputs(text    ? text    : "(null)"); kputc('\n');
    kputs("  +--------------------------------------------------------------+\n");
}
