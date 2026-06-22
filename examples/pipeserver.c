// pipeserver.c  —  Programa WINDOWS (PE32+ x64) que demonstra o LADO SERVIDOR
// de um Named Pipe (IPC), FASE 3.
//
// Fluxo classico do Win32:
//   CreateNamedPipeA("\\.\pipe\MeuOSPipe") -> ConnectNamedPipe -> WriteFile.
//
// Caminho completo de cada chamada:
//   ring3 (pipeserver) -> kernel32 (CreateNamedPipeA/WriteFile) ->
//   ntdll (NtCreateNamedPipeFile/NtWriteFile) -> int 0x80 -> SSDT ->
//   pipe_create/pipe_write (nt/pipe.c) -> buffer do pipe no Object Manager.
//
// Como NAO ha escalonador, a demo e SEQUENCIAL: este servidor cria o pipe e
// escreve os bytes; em seguida o pipeclient.exe (outro processo, no MESMO boot)
// abre o pipe pelo nome e LE os mesmos bytes. O pipe vive no namespace do
// Object Manager (kernel), entao persiste entre os dois processos.

unsigned long _tls_index = 0;

#define STD_OUTPUT_HANDLE ((unsigned)-11)
#define PIPE_ACCESS_DUPLEX 0x00000003

__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) void* CreateNamedPipeA(const char* name, unsigned openMode,
        unsigned pipeMode, unsigned maxInstances, unsigned outBufSize,
        unsigned inBufSize, unsigned defaultTimeout, void* sec);
__declspec(dllimport) int   ConnectNamedPipe(void* hPipe, void* overlapped);
__declspec(dllimport) int   CloseHandle(void* h);
__declspec(dllimport) void  ExitProcess(unsigned code);

#define INVALID_HANDLE_VALUE ((void*)(long long)-1)

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}

void _start(void) {
    say("  [pipeserver] inicio (ring 3). Criando o Named Pipe...\n");

    void* hPipe = CreateNamedPipeA("\\\\.\\pipe\\MeuOSPipe",
                                   PIPE_ACCESS_DUPLEX, 0, 1, 4096, 4096, 0, 0);
    if (hPipe == INVALID_HANDLE_VALUE || hPipe == 0) {
        say("  [pipeserver] CreateNamedPipeA FALHOU.\n");
        ExitProcess(1);
    }
    say("  [pipeserver] CreateNamedPipeA(\\\\.\\pipe\\MeuOSPipe) OK.\n");

    // Sinaliza que o servidor esta pronto para um cliente.
    ConnectNamedPipe(hPipe, 0);
    say("  [pipeserver] ConnectNamedPipe OK (aguardando cliente).\n");

    // Escreve a mensagem no pipe. O cliente vai ler exatamente estes bytes.
    const char* msg = "Ola do servidor via Named Pipe! (IPC FASE 3)";
    unsigned written = 0;
    WriteFile(hPipe, msg, slen(msg), &written, 0);
    say("  [pipeserver] WriteFile -> pipe; escrevi a mensagem:\n");
    say("    \"");
    say(msg);
    say("\"\n");

    say("  [pipeserver] feito. O pipeclient.exe vai ler estes bytes.\n");
    // Nao fechamos o handle do pipe de proposito: assim o objeto do pipe
    // continua no namespace com os bytes no buffer para o cliente ler em
    // seguida (sem escalonador, a demo e sequencial no mesmo boot).
    ExitProcess(0);
}
