// ============================================================================
//  csrss.exe — Client/Server Runtime Subsystem (RODADA FINAL).
//
//  No Windows real, csrss.exe e um processo SISTEMA que comeca ANTES de
//  qualquer aplicacao Win32. Ele:
//    1. carrega win32k.sys como driver de kernel (no MeuOS, win32k ja foi
//       carregado diretamente no init do main.c — entao aqui so logamos),
//    2. cria o Window Station e o desktop padrao,
//    3. atende ALPC requests dos processos Win32 para criacao de processos
//       (CreateProcess delega para csrss via NtCreateProcess + DupeHandle).
//
//  Aqui no MeuOS csrss.exe e um stub que so loga "[csrss] subsystem started"
//  na serial e sai. Ele e "carregado" antes de winlogon e do app de teste
//  para que a ordem de boot espelhe o Windows real (csrss -> winlogon -> apps).
//
//  Caminho de cada linha:
//    ring3 (csrss) -> kernel32 (WriteFile) -> ntdll (NtWriteFile) ->
//    int 0x80 -> console device (VGA+serial).
//
//  IMAGE BASE: 0x5200000 — zona livre apos winlogon (0x5100000).
// ============================================================================

unsigned long _tls_index = 0;

#define STD_OUTPUT_HANDLE  ((unsigned)-11)

__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len,
                                      unsigned* written, void* overlapped);
__declspec(dllimport) void  ExitProcess(unsigned code);

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void puts_h(void* h, const char* s) {
    unsigned w = 0; WriteFile(h, s, slen(s), &w, 0);
}

void _start(void) {
    void* hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    puts_h(hOut, "\n[csrss] ============================================\n");
    puts_h(hOut, "[csrss] Client/Server Runtime Subsystem (subsystem stub)\n");
    puts_h(hOut, "[csrss] ============================================\n");
    puts_h(hOut, "[csrss] win32k.sys ja carregado pelo kernel no init\n");
    puts_h(hOut, "[csrss] Window Station: WinSta0 (default)\n");
    puts_h(hOut, "[csrss] desktop: Default + Winlogon + Screen-saver\n");
    puts_h(hOut, "[csrss] ALPC port: \\Sessions\\1\\Windows\\ApiPort\n");
    puts_h(hOut, "[csrss] subsystem started (PID virtual = csrss process)\n");
    puts_h(hOut, "[csrss] ready to dispatch CreateProcess requests\n");
    puts_h(hOut, "[csrss] subsystem exit (stub run completo).\n");

    ExitProcess(0);
}
