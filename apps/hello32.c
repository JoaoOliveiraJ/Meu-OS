// Programa WINDOWS de teste de 32 BITS (PE32, x86 / IMAGE_NT_HEADERS32).
// Compilado com  zig cc -target x86-windows-gnu -nostdlib -e _start.
//
// Demonstra o suporte a PE32 no loader do MeuOS:
//   - o loader detecta machine=0x14C / magic=0x10B, mapeia as secoes de 32 bits
//     e (se houver .reloc) aplica relocacoes;
//   - executa o codigo em RING 3 de 32 bits (compatibility mode);
//   - a syscall vinda de 32 bits e o mesmo int 0x80, com a convencao
//     EAX=numero; args em EDI, ESI (ver src/ke/syscall.c).
//
// Para ser 100% autocontido (sem depender de DLLs de 32 bits, que o OS ainda
// nao tem), ele NAO importa nada: faz int 0x80 direto via assembly inline.

// Sem o CRT do MinGW, o linker ainda referencia este simbolo de TLS (em 32 bits
// vira __tls_index pelo prefixo cdecl); definimos para satisfazer o link.
unsigned long _tls_index = 0;

// Syscalls do MeuOS (numeros da SSDT em src/ke/syscall.c).
#define SYS_WRITE       1
#define SYS_MESSAGEBOX  3
#define SYS_EXIT        2

// int 0x80 com 0 argumentos (so o numero em EAX).
static inline void sys0(unsigned eax) {
    __asm__ volatile ("int $0x80" : : "a"(eax) : "memory");
}
// int 0x80 com 1 argumento (EDI).
static inline void sys1(unsigned eax, const void* edi) {
    __asm__ volatile ("int $0x80" : : "a"(eax), "D"(edi) : "memory");
}
// int 0x80 com 2 argumentos (EDI, ESI). Convencao 32-bit do MeuOS.
static inline void sys2(unsigned eax, const void* edi, const void* esi) {
    __asm__ volatile ("int $0x80" : : "a"(eax), "D"(edi), "S"(esi) : "memory");
}

void _start(void) {
    // sys_write: imprime na serial/VGA, provando que o codigo de 32 bits roda.
    sys1(SYS_WRITE, "  [PE32] Ola do RING 3 de 32 bits (compatibility mode)!\n");

    // sys_messagebox(text em EDI, caption em ESI).
    sys2(SYS_MESSAGEBOX,
         "Sou um .EXE de 32 bits (PE32) rodando em compat mode no MeuOS!",
         "MeuOS  -  PE32 (loader 32-bit)");

    // sys_exit: volta ao kernel (longjmp), igual ao caminho de 64 bits.
    sys0(SYS_EXIT);

    // nao retorna; mas o linker quer um fim definido.
    for (;;) { }
}
