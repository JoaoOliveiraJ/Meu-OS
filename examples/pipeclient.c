// pipeclient.c  —  Programa WINDOWS (PE32+ x64) que demonstra o LADO CLIENTE
// de um Named Pipe (IPC), FASE 3.
//
// Fluxo classico do Win32:
//   CreateFileA("\\.\pipe\MeuOSPipe") -> ReadFile -> imprime os bytes lidos.
//
// Caminho completo de cada chamada:
//   ring3 (pipeclient) -> kernel32 (CreateFileA/ReadFile) ->
//   ntdll (NtCreateFile/NtReadFile) -> int 0x80 -> SSDT ->
//   pipe_open/pipe_read (nt/pipe.c) -> buffer do pipe no Object Manager.
//
// O pipeserver.exe (rodado ANTES, no mesmo boot) ja criou o pipe e escreveu a
// mensagem. Como o pipe vive no namespace do Object Manager (kernel), o cliente
// o encontra pelo nome e LE os mesmos bytes — provando o IPC ponta a ponta.

unsigned long _tls_index = 0;

#define STD_OUTPUT_HANDLE ((unsigned)-11)
#define GENERIC_READ  0x80000000
#define OPEN_EXISTING 3

__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) int   ReadFile(void* h, void* buf, unsigned len, unsigned* read, void* ov);
__declspec(dllimport) void* CreateFileA(const char* name, unsigned access, unsigned share,
                                        void* sec, unsigned disp, unsigned flags, void* templ);
__declspec(dllimport) int   CloseHandle(void* h);
__declspec(dllimport) void  ExitProcess(unsigned code);

#define INVALID_HANDLE_VALUE ((void*)(long long)-1)

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}

void _start(void) {
    say("  [pipeclient] inicio (ring 3). Abrindo o Named Pipe pelo nome...\n");

    void* hPipe = CreateFileA("\\\\.\\pipe\\MeuOSPipe",
                              GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (hPipe == INVALID_HANDLE_VALUE || hPipe == 0) {
        say("  [pipeclient] CreateFileA(\\\\.\\pipe\\MeuOSPipe) FALHOU.\n");
        ExitProcess(1);
    }
    say("  [pipeclient] CreateFileA(\\\\.\\pipe\\MeuOSPipe) OK (conectado).\n");

    // Le os bytes que o servidor escreveu no pipe.
    char buf[128];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = 0;
    unsigned got = 0;
    ReadFile(hPipe, buf, sizeof(buf) - 1, &got, 0);
    buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;

    say("  [pipeclient] ReadFile <- pipe; recebi do servidor:\n");
    say("    \"");
    say(buf);
    say("\"\n");
    say("  [pipeclient] IPC por Named Pipe OK: os bytes atravessaram servidor->cliente.\n");

    CloseHandle(hPipe);
    ExitProcess(0);
}
