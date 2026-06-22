// Programa WINDOWS de teste (PE32+ x86-64) que imprime no CONSOLE via a API
// classica: GetStdHandle(STD_OUTPUT_HANDLE) + WriteFile. Tambem demonstra
// GetModuleHandleA/GetProcAddress (resolvem contra o loader do MeuOS) e um
// MessageBoxA no fim. Todo o caminho:
//   ring3 -> kernel32 (WriteFile) -> ntdll (NtWriteFile) -> int 0x80 ->
//   SSDT (sys_writefile) -> "console device" (saida do kernel: VGA+serial).

// Sem o CRT do MinGW, o linker ainda referencia este simbolo de TLS; definimos.
unsigned long _tls_index = 0;

#define STD_OUTPUT_HANDLE ((unsigned)-11)

__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len,
                                      unsigned* written, void* overlapped);
__declspec(dllimport) void* GetModuleHandleA(const char* name);
__declspec(dllimport) void* GetProcAddress(void* module, const char* name);
__declspec(dllimport) int   MessageBoxA(void* hWnd, const char* text,
                                        const char* caption, unsigned type);
__declspec(dllimport) void  ExitProcess(unsigned code);

// strlen sem CRT.
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

// Escreve uma string no handle, via WriteFile (que chama NtWriteFile).
static void puts_h(void* h, const char* s) {
    unsigned written = 0;
    WriteFile(h, s, slen(s), &written, 0);
}

void _start(void) {
    void* hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    puts_h(hOut, "  [conhello] Ola do RING 3 via GetStdHandle + WriteFile!\n");
    puts_h(hOut, "  [conhello] Esta linha foi escrita por NtWriteFile (console device).\n");

    // Demonstra GetModuleHandleA + GetProcAddress: resolve MessageBoxA em
    // user32 e o chama pelo ponteiro obtido (igual ao Windows faz com
    // LoadLibrary/GetProcAddress). Apoiado pelo loader real do MeuOS.
    typedef int (*msgbox_t)(void*, const char*, const char*, unsigned);
    void* user32 = GetModuleHandleA("user32.dll");
    msgbox_t pMsgBox = user32 ? (msgbox_t)GetProcAddress(user32, "MessageBoxA") : 0;
    if (pMsgBox) {
        puts_h(hOut, "  [conhello] GetProcAddress(user32, MessageBoxA) OK.\n");
        pMsgBox(0, "Chamado via GetProcAddress!", "MeuOS  -  WriteFile + GetProcAddress", 0);
    } else {
        puts_h(hOut, "  [conhello] GetProcAddress falhou (chamando MessageBoxA direto).\n");
        MessageBoxA(0, "WriteFile no console OK", "MeuOS  -  conhello", 0);
    }

    ExitProcess(0);
}
