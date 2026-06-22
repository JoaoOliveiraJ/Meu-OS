// Programa WINDOWS de teste (PE32+ x86-64).
// Compilado com  zig cc -target x86_64-windows-gnu -nostdlib -e _start.
// Nao tem CRT: a entrada e _start. Ele importa funcoes do "Windows"
// (user32/kernel32) que o MeuOS resolve para a sua propria implementacao.

// Sem o CRT do MinGW, o linker ainda referencia este simbolo de TLS; definimos.
unsigned long _tls_index = 0;

__declspec(dllimport) int  MessageBoxA(void* hWnd, const char* text,
                                       const char* caption, unsigned int type);
__declspec(dllimport) void ExitProcess(unsigned int code);

void _start(void) {
    MessageBoxA(0,
        "Sou um .EXE do Windows rodando no MeuOS, chamando a Win32 nativa!",
        "MeuOS  -  Pinball (teste do loader PE)", 0);
    ExitProcess(123);
}
