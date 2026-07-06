// FRENTE 3 (Fase 3a) — prova do TEB/PEB do processo ring-3.
// Le, em ring 3, gs:[0x30] (TEB.Self), gs:[0x60] (PEB) e PEB->ImageBaseAddress
// (PEB+0x10), e imprime os 3 em hex. Se o kernel montou o TEB/PEB e apontou o
// gs-base do ring 3 p/ o TEB, os valores saem corretos:
//   gs:[0x30] = 0x600000 (TEB no fundo da janela de pilha)
//   gs:[0x60] = 0x601000 (PEB)
//   ImageBase = 0x1900000 (a base de carga real deste tebtest)
// E' exatamente o acesso que o CRT de um .exe REAL do Windows faz no arranque.

unsigned long _tls_index = 0;

#define STD_OUTPUT_HANDLE ((unsigned)-11)
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* b, unsigned n, unsigned* w, void* o);
__declspec(dllimport) void  ExitProcess(unsigned code);

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void wr(void* h, const char* s) { unsigned w = 0; WriteFile(h, s, slen(s), &w, 0); }
static void hex64(char* out, unsigned long long v) {
    static const char d[] = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) out[2 + i] = d[(v >> ((15 - i) * 4)) & 0xF];
    out[18] = '\n'; out[19] = 0;
}

void _start(void) {
    void* h = GetStdHandle(STD_OUTPUT_HANDLE);
    unsigned long long teb = 0, peb = 0, imgbase = 0;
    __asm__ volatile ("movq %%gs:0x30, %0" : "=r"(teb));
    __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(peb));
    if (peb) imgbase = *(volatile unsigned long long*)(unsigned long long)(peb + 0x10);
    char buf[24];
    wr(h, "  [tebtest] gs:[0x30] TEB.Self    = "); hex64(buf, teb);     wr(h, buf);
    wr(h, "  [tebtest] gs:[0x60] PEB         = "); hex64(buf, peb);     wr(h, buf);
    wr(h, "  [tebtest] PEB->ImageBaseAddress = "); hex64(buf, imgbase); wr(h, buf);
    ExitProcess(0);
}
