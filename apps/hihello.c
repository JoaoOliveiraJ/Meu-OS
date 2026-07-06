// FRENTE 3 — teste da RELOCACAO no caminho de USUARIO (ldr_run).
// Compilado com ImageBase ALTO (0x140000000 = 5 GiB, o default de .exe reais do
// Windows) e com .reloc (--dynamicbase). O ponteiro global absoluto 'g_msgptr'
// forca uma relocacao DIR64: quando o loader mapeia a imagem numa base baixa
// (RAM < 1 GiB) e aplica o .reloc, esse ponteiro precisa ser corrigido. Se a
// relocacao estiver certa, o WriteFile abaixo imprime a mensagem correta.

unsigned long _tls_index = 0;   // referenciado pelo runtime mesmo sem CRT

#define STD_OUTPUT_HANDLE ((unsigned)-11)

__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len,
                                      unsigned* written, void* overlapped);
__declspec(dllimport) void  ExitProcess(unsigned code);

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

static const char msg[] =
    "  [hihello] RING 3 a partir de ImageBase ALTO (0x140000000) realocado p/ RAM baixa!\n";

// Ponteiro absoluto em .data: o inicializador guarda o VA absoluto de 'msg'
// (ImageBase + rva). Isso OBRIGA uma relocacao DIR64 na imagem — exatamente o
// que exercita o pe_relocate no caminho de usuario. 'volatile' impede o
// compilador de dobrar o acesso em RIP-relative.
static const char* volatile g_msgptr = msg;

void _start(void) {
    void* hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const char* m = g_msgptr;            // lido via o ponteiro absoluto (relocado)
    unsigned written = 0;
    WriteFile(hOut, m, slen(m), &written, 0);
    ExitProcess(0);
}
