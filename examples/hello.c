// Programa WINDOWS de teste (PE32+ x86-64).
// Compilado com  zig cc -target x86_64-windows-gnu -nostdlib -e _start.
// Nao tem CRT: a entrada e _start. Ele importa funcoes do "Windows"
// (user32/kernel32) que o MeuOS resolve para a sua propria implementacao.

// Sem o CRT do MinGW, o linker ainda referencia este simbolo de TLS; definimos.
unsigned long _tls_index = 0;

__declspec(dllimport) int  MessageBoxA(void* hWnd, const char* text,
                                       const char* caption, unsigned int type);
__declspec(dllimport) void ExitProcess(unsigned int code);

// Process Manager (Win32). Demonstra NtCreateProcess/NtCreateThread/Wait via
// CreateProcessA + WaitForSingleObject, partindo de ring 3.
__declspec(dllimport) int  CreateProcessA(const char* app, char* cmdline, void* pa, void* ta,
        int inherit, unsigned flags, void* env, const char* cwd, void* si, void* pi);
__declspec(dllimport) unsigned WaitForSingleObject(void* handle, unsigned timeout_ms);

void _start(void) {
    // Cria um processo "filho" (objeto EPROCESS) e espera por ele. Mostra o
    // caminho ring3 -> kernel32 -> ntdll -> int 0x80 -> Process Manager.
    void* pi[4] = { 0, 0, 0, 0 };   // [0]=hProcess [1]=hThread
    if (CreateProcessA("worker.exe", 0, 0, 0, 0, 0, 0, 0, 0, pi))
        WaitForSingleObject(pi[0], 0xFFFFFFFFu);

    MessageBoxA(0,
        "Sou um .EXE do Windows rodando no MeuOS, chamando a Win32 nativa!",
        "MeuOS  -  Pinball (teste do loader PE)", 0);
    ExitProcess(123);
}
