// ucrtbase.dll — reimplementacao MINIMA do CRT do Windows (UCRT). O loader do
// MeuOS redireciona os apisets `api-ms-win-crt-*.dll` p/ esta DLL (igual ao Windows,
// onde os apisets encaminham p/ ucrtbase.dll). Objetivo da Frente 3 Fase 3b: fazer o
// STARTUP do mingw chegar em main() e sair limpo, num .exe com CRT REAL. A maioria
// e' stub seguro; malloc/strlen/strncmp sao reais; exit/abort -> ExitProcess.
//
// NAO implementa printf de verdade (o alvo escreve via WriteFile/kernel32). As
// funcoes de stdio existem so p/ resolver os imports e nao quebrar no flush de saida.

typedef unsigned long long size_t_;
typedef unsigned short     wchar16;

unsigned int _tls_index = 0;

__declspec(dllimport) void  ExitProcess(unsigned code);
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* written, void* ov);
__declspec(dllimport) void  RtlRaiseException(void* rec);   // ntdll: dispara o C++ EH (dispatch/unwind)

// ---------------------------------------------------------------------------
//  DIAGNOSTICO — log direto na serial via SYS_WRITE (int 0x80 #1, rdi=string),
//  sem depender de WriteFile/console (o kernel imprime rdi como C-string). Usado
//  p/ instrumentar os caminhos FATAIS do CRT (abort/terminate/_CxxThrowException):
//  quando o worker do explorer REAL "morre", precisamos saber QUAL caminho e de ONDE
//  (endereco de retorno do chamador) — e, no caso de throw C++, QUAL TIPO foi lancado.
// ---------------------------------------------------------------------------
static void uc_puts(const char* s) { long long r; __asm__ volatile("int $0x80":"=a"(r):"a"(1LL),"D"(s):"memory","rcx","r11"); }
static void uc_puthex(unsigned long long v) {
    char b[19]; b[0]='0'; b[1]='x';
    for (int i=0;i<16;i++){ int nib=(int)((v>>((15-i)*4))&0xF); b[2+i]=(char)(nib<10?('0'+nib):('a'+nib-10)); }
    b[18]=0; uc_puts(b);
}
// Acha a base (header 'MZ'/'PE') do modulo que contem 'p', varrendo p/ tras em passos de
// pagina. As RVAs dentro de _ThrowInfo sao relativas a essa base. O('MZ' @ base, imagem
// mapeada contigua) garante que paramos ANTES de tocar pagina nao-mapeada abaixo da base.
static const unsigned char* uc_module_base(const void* p) {
    unsigned long long a = ((unsigned long long)(__INTPTR_TYPE__)p) & ~0xFFFULL;
    for (int i=0; i<8192 && a>=0x10000ULL; i++, a-=0x1000ULL) {
        const unsigned char* m = (const unsigned char*)(__INTPTR_TYPE__)a;
        if (m[0]=='M' && m[1]=='Z') {
            unsigned e = *(const unsigned*)(m+0x3C);
            if (e < 0x10000 && *(const unsigned*)(m+e)==0x00004550u) return m;   // 'PE\0\0'
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  heap — bump allocator sobre um buffer estatico (sem free real; basta p/ o CRT).
// ---------------------------------------------------------------------------
static unsigned char g_heap[0x20000];   // 128 KiB
static size_t_       g_heap_off = 0;

__declspec(dllexport) void* malloc(size_t_ n) {
    n = (n + 15) & ~(size_t_)15;
    if (g_heap_off + n > sizeof(g_heap)) return 0;
    void* p = &g_heap[g_heap_off];
    g_heap_off += n;
    return p;
}
__declspec(dllexport) void  free(void* p) { (void)p; }   // bump: nao libera
__declspec(dllexport) void* calloc(size_t_ a, size_t_ b) {
    size_t_ n = a * b;
    unsigned char* p = (unsigned char*)malloc(n);
    if (p) for (size_t_ i = 0; i < n; i++) p[i] = 0;
    return p;
}
__declspec(dllexport) void* realloc(void* p, size_t_ n) { (void)p; return malloc(n); }
__declspec(dllexport) int   _set_new_mode(int m) { (void)m; return 0; }

// ---------------------------------------------------------------------------
//  string — reais (o CRT usa no arranque).
// ---------------------------------------------------------------------------
__declspec(dllexport) size_t_ strlen(const char* s) {
    size_t_ n = 0; while (s && s[n]) n++; return n;
}
__declspec(dllexport) int strncmp(const char* a, const char* b, size_t_ n) {
    for (size_t_ i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
    return 0;
}

// mem* reais (o explorer os importa do CRT). O `volatile` evita que o clang reconheca
// o loop e emita uma CHAMADA a memcpy/memset DENTRO delas mesmas (recursao infinita).
__declspec(dllexport) void* memcpy(void* d, const void* s, size_t_ n) {
    volatile unsigned char* dp = (volatile unsigned char*)d;
    const volatile unsigned char* sp = (const volatile unsigned char*)s;
    for (size_t_ i = 0; i < n; i++) dp[i] = sp[i]; return d;
}
__declspec(dllexport) void* memmove(void* d, const void* s, size_t_ n) {
    volatile unsigned char* dp = (volatile unsigned char*)d;
    const volatile unsigned char* sp = (const volatile unsigned char*)s;
    if ((void*)dp < (const void*)sp) { for (size_t_ i = 0; i < n; i++) dp[i] = sp[i]; }
    else { for (size_t_ i = n; i > 0; i--) dp[i-1] = sp[i-1]; }
    return d;
}
__declspec(dllexport) void* memset(void* d, int c, size_t_ n) {
    volatile unsigned char* dp = (volatile unsigned char*)d;
    for (size_t_ i = 0; i < n; i++) dp[i] = (unsigned char)c; return d;
}
__declspec(dllexport) int memcmp(const void* a, const void* b, size_t_ n) {
    const unsigned char* pa = (const unsigned char*)a; const unsigned char* pb = (const unsigned char*)b;
    for (size_t_ i = 0; i < n; i++) if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i]; return 0;
}

// ---------------------------------------------------------------------------
//  runtime startup — argv/env, _initterm, config, exit.
// ---------------------------------------------------------------------------
static char     g_arg0[] = "crthello.exe";
static char*    g_argv[2]  = { g_arg0, 0 };      // array default de argv (1 elem + NULL)
static wchar16  g_warg0[1] = { 0 };
static wchar16* g_wargv[1] = { g_warg0 };
// __argc/__argv/__wargv: SIMBOLOS DE DADOS exportados que o startup do mingw-w64 (crtexe.c)
// le DIRETAMENTE (e o __p___argc/argv devolvem o endereco deles). Sem exporta-los, o startup
// lia um import de dado nao resolvido -> argc=0. meuos_parse_cmdline os atualiza.
__declspec(dllexport) int       __argc  = 1;
__declspec(dllexport) char**    __argv  = g_argv;
__declspec(dllexport) wchar16** __wargv = g_wargv;
static char*    g_env[1]   = { 0 };
static char**   g_env_p    = g_env;
static wchar16* g_wenv[1]  = { 0 };
static wchar16** g_wenv_p  = g_wenv;
static int      g_commode  = 0;
static int      g_fmode    = 0;

static void meuos_parse_cmdline(void);   // parseia a cmdline do kernel em argv[] (MeuOS)
__declspec(dllexport) int*       __p___argc(void)  { meuos_parse_cmdline(); return &__argc; }
__declspec(dllexport) char***    __p___argv(void)  { meuos_parse_cmdline(); return &__argv; }
__declspec(dllexport) wchar16*** __p___wargv(void) { return &__wargv; }
__declspec(dllexport) char***    __p__environ(void)  { return &g_env_p; }
__declspec(dllexport) wchar16*** __p__wenviron(void) { return &g_wenv_p; }
__declspec(dllexport) int*        __p__commode(void) { return &g_commode; }
__declspec(dllexport) int*        __p__fmode(void)   { return &g_fmode; }

// __p__acmdln: ponteiro p/ a string bruta da linha de comando (o startup GUI do CRT,
// WinMainCRTStartup, usa p/ montar lpCmdLine). _ismbblead: sem MBCS, nunca e' lead-byte.
static char* g_acmdln = (char*)"";
__declspec(dllexport) char** __p__acmdln(void)    { return &g_acmdln; }
__declspec(dllexport) int    _ismbblead(unsigned c) { (void)c; return 0; }

// MeuOS: o kernel escreve a linha de comando do processo em 0x601800 (PEB+0x800; a
// janela TEB/PEB deste OS fica em 0x600000). String vazia = sem cmdline -> mantem o
// default ("crthello.exe"). Parseamos em argv[] por espacos (v1: sem aspas). Idempotente
// (re-le a mesma fonte), chamado do _configure_narrow_argv e do __p___argc/__p___argv,
// entao funciona qualquer que seja a ordem do startup do mingw. NAO usa guard: cada
// processo (o pai roda com 0x601800 vazio; o filho com a sua cmdline) e' re-parseado.
#define MEUOS_CMDLINE_ADDR 0x601800ULL
static char  g_cmdbuf[256];
static char* g_argv_real[32];
static void meuos_parse_cmdline(void) {
    const char* src = (const char*)(__INTPTR_TYPE__)MEUOS_CMDLINE_ADDR;
    if (!src || src[0] == 0) {                        // sem cmdline: volta ao default.
        __argc = 1; __argv = g_argv;                 // NAO herdar args de um processo anterior
        return;                                        // (ucrtbase e' compartilhado entre processos)
    }
    int n = 0;
    while (src[n] && n < (int)sizeof(g_cmdbuf) - 1) { g_cmdbuf[n] = src[n]; n++; }
    g_cmdbuf[n] = 0;
    int argc = 0; char* p = g_cmdbuf;
    while (*p && argc < 31) {
        while (*p == ' ') p++;                        // pula espacos entre tokens
        if (!*p) break;
        g_argv_real[argc++] = p;                      // inicio do token
        while (*p && *p != ' ') p++;                  // fim do token
        if (*p) *p++ = 0;                             // termina o token in-place
    }
    if (argc > 0) {
        g_argv_real[argc] = 0;
        __argc = argc;
        __argv = g_argv_real;
        g_acmdln = (char*)(__INTPTR_TYPE__)MEUOS_CMDLINE_ADDR;   // cmdline crua (GetCommandLine/WinMain)
    }
}

__declspec(dllexport) int  _configure_narrow_argv(int mode) { (void)mode; meuos_parse_cmdline(); return 0; }
__declspec(dllexport) int  _configure_wide_argv(int mode)   { (void)mode; return 0; }
__declspec(dllexport) int  _initialize_narrow_environment(void) { return 0; }
__declspec(dllexport) int  _initialize_wide_environment(void)   { return 0; }
__declspec(dllexport) void _set_app_type(int t) { (void)t; }
__declspec(dllexport) void* _set_invalid_parameter_handler(void* h) { (void)h; return 0; }
__declspec(dllexport) void  __setusermatherr(void* h) { (void)h; }
__declspec(dllexport) int   _crt_atexit(void* fn)      { (void)fn; return 0; }
__declspec(dllexport) int   _crt_at_quick_exit(void* fn){ (void)fn; return 0; }
__declspec(dllexport) void* signal(int sig, void* handler) { (void)sig; (void)handler; return 0; }

// _initterm: chama cada ponteiro de funcao em [first,last) (inicializadores C do CRT).
// PRECISA rodar a tabela: o startup do mingw (crtexe.c) registra pre_c_init (.CRT$XI) e
// pre_cpp_init (.CRT$XC) nela, e o pre_cpp_init e' quem chama __getmainargs ->
// _configure_narrow_argv + __p___argc/argv (monta o argv REAL). Como no-op, argc/argv
// nunca eram montados (argc=0). pre_c_init/pre_cpp_init so chamam funcs que ja temos
// (_set_app_type, __p__fmode/commode, _setargv estatico, __p___argc/argv), sem cascata.
__declspec(dllexport) void _initterm(void* first, void* last) {
    void (**p)(void) = (void (**)(void))first;
    void (**e)(void) = (void (**)(void))last;
    for (; p < e; p++) if (*p) (*p)();
}
__declspec(dllexport) int _initterm_e(void* first, void* last) {
    int (**p)(void) = (int (**)(void))first;
    int (**e)(void) = (int (**)(void))last;
    for (; p < e; p++) if (*p) { int r = (*p)(); if (r) return r; }
    return 0;
}

__declspec(dllexport) void _cexit(void) { }
// ExitProcess encerra o processo; o for(;;) so satisfaz o 'noreturn' que o compilador
// assume p/ exit/_exit/abort (ele nao sabe que ExitProcess nao retorna).
__declspec(dllexport) void exit(int code)  { ExitProcess((unsigned)code); for (;;) {} }
__declspec(dllexport) void _exit(int code) { ExitProcess((unsigned)code); for (;;) {} }
__declspec(dllexport) void abort(void)     { uc_puts("[ucrtbase] abort() RA="); uc_puthex((unsigned long long)(__INTPTR_TYPE__)__builtin_return_address(0)); uc_puts("\n"); ExitProcess(3); for (;;) {} }

// ---------------------------------------------------------------------------
//  private / SEH — __C_specific_handler. So existe p/ resolver; sem exceptions
//  reais devolve ExceptionContinueSearch (1).
// ---------------------------------------------------------------------------
__declspec(dllexport) int __C_specific_handler(void* rec, void* frame, void* ctx, void* disp) {
    (void)rec; (void)frame; (void)ctx; (void)disp; return 1;
}

// ---------------------------------------------------------------------------
//  stdio — printf REAL (subset): formata fmt+valist e escreve no console via
//  WriteFile (kernel32). Assim um printf() de um .exe com CRT real IMPRIME.
//  va_list no x64 do Windows = char* p/ os varargs (cada slot = 8 bytes).
// ---------------------------------------------------------------------------
// FILE* falso: console (fd 0/1/2, handle=0) OU arquivo (fd=-1 + handle do CreateFileA).
typedef struct { int fd; void* handle; int err; int eof; } UCRT_FILE;
static UCRT_FILE g_iob[3] = { {0,0,0,0}, {1,0,0,0}, {2,0,0,0} };
__declspec(dllexport) void* __acrt_iob_func(unsigned i) {
    return (i < 3) ? (void*)&g_iob[i] : (void*)&g_iob[1];
}
static void ucrt_write(int fd, const char* s, int n) {
    if (n <= 0) return;
    void* h = GetStdHandle(fd == 2 ? (unsigned)-12 : (unsigned)-11);
    unsigned w = 0; WriteFile(h, s, (unsigned)n, &w, 0);
}
static void put_uint(char* out, int* pos, int cap, unsigned long long v,
                     int base, int upper, int width, char pad) {
    char tmp[32]; int t = 0;
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = dig[v % base]; v /= base; }
    while (t < width) tmp[t++] = pad;
    while (t > 0 && *pos < cap) out[(*pos)++] = tmp[--t];
}
static int ucrt_vfmt(char* out, int cap, const char* fmt, char* ap) {
    int pos = 0;
    for (const char* p = fmt; *p && pos < cap; p++) {
        if (*p != '%') { out[pos++] = *p; continue; }
        p++;
        char pad = ' '; int width = 0, lng = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l') { lng++; p++; }
        char c = *p;
        if (c == 's') {
            const char* s = *(char**)ap; ap += 8;
            if (!s) s = "(null)";
            while (*s && pos < cap) out[pos++] = *s++;
        } else if (c == 'c') {
            int ch = (int)*(long long*)ap; ap += 8;
            if (pos < cap) out[pos++] = (char)ch;
        } else if (c == 'd' || c == 'i') {
            long long v = *(long long*)ap; ap += 8;
            if (!lng) v = (int)v;
            if (v < 0) { if (pos < cap) out[pos++] = '-';
                         put_uint(out, &pos, cap, (unsigned long long)(-v), 10, 0, width, pad); }
            else put_uint(out, &pos, cap, (unsigned long long)v, 10, 0, width, pad);
        } else if (c == 'u') {
            unsigned long long v = *(unsigned long long*)ap; ap += 8;
            if (!lng) v = (unsigned)v;
            put_uint(out, &pos, cap, v, 10, 0, width, pad);
        } else if (c == 'x' || c == 'X') {
            unsigned long long v = *(unsigned long long*)ap; ap += 8;
            if (!lng) v = (unsigned)v;
            put_uint(out, &pos, cap, v, 16, c == 'X', width, pad);
        } else if (c == 'p') {
            unsigned long long v = *(unsigned long long*)ap; ap += 8;
            if (pos < cap) out[pos++] = '0'; if (pos < cap) out[pos++] = 'x';
            put_uint(out, &pos, cap, v, 16, 0, 16, '0');
        } else if (c == '%') {
            if (pos < cap) out[pos++] = '%';
        } else {   // desconhecido: emite literal
            if (pos < cap) out[pos++] = '%';
            if (pos < cap) out[pos++] = c;
        }
    }
    return pos;
}
__declspec(dllexport) int __stdio_common_vfprintf(unsigned long long opt, void* stream,
        const char* fmt, void* loc, void* valist) {
    (void)opt; (void)loc;
    char buf[1024];
    int n = ucrt_vfmt(buf, (int)sizeof(buf), fmt, (char*)valist);
    ucrt_write(stream ? ((UCRT_FILE*)stream)->fd : 1, buf, n);
    return n;
}
__declspec(dllexport) int __stdio_common_vfwprintf(unsigned long long opt, void* stream,
        const wchar16* fmt, void* loc, void* valist) {
    (void)opt; (void)stream; (void)fmt; (void)loc; (void)valist; return 0;
}
__declspec(dllexport) size_t_ fwrite(const void* p, size_t_ sz, size_t_ n, void* stream) {
    UCRT_FILE* f = (UCRT_FILE*)stream;
    size_t_ total = sz * n;
    if (f && f->fd < 0) {                    // arquivo: WriteFile no handle
        unsigned wrote = 0;
        WriteFile(f->handle, p, (unsigned)total, &wrote, 0);
        return sz ? (size_t_)(wrote / sz) : 0;
    }
    ucrt_write(f ? f->fd : 1, (const char*)p, (int)total);   // console
    return n;
}

// ---------------------------------------------------------------------------
//  stdio — ENTRADA (getchar/fgetc/fgets/fread/scanf). Espelha a saida: le do
//  console (stdin) via ReadFile (kernel32) -> NtReadFile -> fila do teclado (IRQ1).
//  A leitura de console do kernel NAO bloqueia (devolve 0 se nada foi digitado — p/
//  o cmd.exe poder desistir sozinho em headless). Entao o BLOQUEIO de getchar/scanf
//  e' feito AQUI, em ring 3: giramos chamando ReadFile ate chegar >=1 caractere (a
//  IRQ1 do teclado enfileira em paralelo enquanto giramos; interrupcoes ficam ligadas
//  em ring 3). Assim getchar()/scanf() ESPERAM a digitacao (como no Windows) SEM
//  alterar o kernel — o cmd.exe segue com a leitura nao-bloqueante de antes.
// ---------------------------------------------------------------------------
__declspec(dllimport) int ReadFile(void* h, void* buf, unsigned len, unsigned* read, void* ov);

// le 1 byte do stdin, BLOQUEANTE (gira ate uma tecla chegar). Ring 3 nao pode 'hlt'.
static int ucrt_getc_raw(void) {
    void* h = GetStdHandle((unsigned)-10);   // STD_INPUT_HANDLE
    for (;;) {
        unsigned char c; unsigned got = 0;
        ReadFile(h, &c, 1, &got, 0);
        if (got) return (int)c;
    }
}

// pushback de 1 caractere (scanf precisa "espiar" o proximo p/ saber onde o campo
// termina). Fila unica global (stdin e' single-stream; sem escalonador de usuario).
static int  s_unget = -1;
static int  ucrt_sgetc(void)    { if (s_unget >= 0) { int c = s_unget; s_unget = -1; return c; } return ucrt_getc_raw(); }
static void ucrt_sungetc(int c) { s_unget = c; }
static int  ucrt_is_ws(int c)   { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }

__declspec(dllexport) int getchar(void)           { return ucrt_sgetc(); }
__declspec(dllexport) int fgetc(void* stream) {
    UCRT_FILE* f = (UCRT_FILE*)stream;
    if (f && f->fd < 0) {                          // arquivo: 1 byte via ReadFile (EOF se 0)
        unsigned char c; unsigned got = 0;
        ReadFile(f->handle, &c, 1, &got, 0);
        if (!got) { f->eof = 1; return -1; }
        return (int)c;
    }
    return ucrt_sgetc();                            // console/stdin (bloqueante)
}
__declspec(dllexport) int _fgetc_nolock(void* s)  { return fgetc(s); }
__declspec(dllexport) int getc(void* stream)      { return fgetc(stream); }
__declspec(dllexport) int ungetc(int c, void* st) { (void)st; ucrt_sungetc(c); return c; }

__declspec(dllexport) char* fgets(char* s, int n, void* stream) {
    if (!s || n <= 0) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(stream);
        if (c < 0) { if (i == 0) return 0; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return s;
}

__declspec(dllexport) size_t_ fread(void* ptr, size_t_ sz, size_t_ n, void* stream) {
    UCRT_FILE* f = (UCRT_FILE*)stream;
    size_t_ total = sz * n;
    if (f && f->fd < 0) {                           // arquivo: ReadFile de uma vez
        unsigned got = 0;
        ReadFile(f->handle, ptr, (unsigned)total, &got, 0);
        if (!got) f->eof = 1;
        return sz ? (size_t_)(got / sz) : 0;
    }
    unsigned char* p = (unsigned char*)ptr;         // console/stdin (char a char, bloqueante)
    size_t_ i;
    for (i = 0; i < total; i++) { int c = ucrt_sgetc(); if (c < 0) break; p[i] = (unsigned char)c; }
    return sz ? (size_t_)(i / sz) : 0;
}

// scanf REAL (subset): %d %i %u %x/%X %c %s, com '*' (suprime), largura e 'l'/'h'.
// mingw encaminha scanf() -> __stdio_common_vfscanf (igual printf -> __stdio_common_vfprintf).
// va_list no x64 do Windows = char* p/ os varargs; cada arg de scanf e' um PONTEIRO (8 bytes).
__declspec(dllexport) int __stdio_common_vfscanf(unsigned long long opt, void* stream,
        const char* fmt, void* loc, void* valist) {
    (void)opt; (void)stream; (void)loc;
    char* ap = (char*)valist;
    int assigned = 0;
    for (const char* p = fmt; *p; p++) {
        if (ucrt_is_ws(*p)) {                     // ws no fmt casa com zero+ ws na entrada
            int c; do { c = ucrt_sgetc(); } while (ucrt_is_ws(c));
            ucrt_sungetc(c);
            continue;
        }
        if (*p != '%') {                          // caractere literal: precisa casar
            int c = ucrt_sgetc();
            if (c != (unsigned char)*p) { ucrt_sungetc(c); return assigned; }
            continue;
        }
        p++;
        int width = 0, lng = 0, suppress = 0;
        if (*p == '*') { suppress = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l' || *p == 'h') { if (*p == 'l') lng++; p++; }
        char conv = *p;
        if (conv == 'c') {
            int c = ucrt_sgetc();
            if (c < 0) return assigned;
            if (!suppress) { *(char*)(*(void**)ap) = (char)c; ap += 8; assigned++; }
            continue;
        }
        if (conv == 's') {
            int c; do { c = ucrt_sgetc(); } while (ucrt_is_ws(c));   // pula ws inicial
            char* dst = suppress ? 0 : (char*)(*(void**)ap);
            if (!suppress) ap += 8;
            int k = 0;
            while (c >= 0 && !ucrt_is_ws(c) && (width == 0 || k < width)) {
                if (dst) dst[k] = (char)c;
                k++; c = ucrt_sgetc();
            }
            ucrt_sungetc(c);
            if (dst) dst[k] = 0;
            if (!suppress && k > 0) assigned++;
            continue;
        }
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X') {
            int c; do { c = ucrt_sgetc(); } while (ucrt_is_ws(c));   // pula ws inicial
            int neg = 0;
            if (c == '+' || c == '-') { neg = (c == '-'); c = ucrt_sgetc(); }
            int base = (conv == 'x' || conv == 'X') ? 16 : 10;
            unsigned long long val = 0; int got = 0;
            for (;;) {
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                if (d < 0 || d >= base) break;
                val = val * (unsigned)base + (unsigned)d; got = 1; c = ucrt_sgetc();
            }
            ucrt_sungetc(c);
            if (!got) return assigned;             // nenhum digito casou
            if (!suppress) {
                void* dst = *(void**)ap; ap += 8;
                if (conv == 'd' || conv == 'i') {
                    long long sv = neg ? -(long long)val : (long long)val;
                    if (lng) *(long long*)dst = sv; else *(int*)dst = (int)sv;
                } else {
                    if (lng) *(unsigned long long*)dst = val; else *(unsigned*)dst = (unsigned)val;
                }
                assigned++;
            }
            continue;
        }
        if (conv == '%') {
            int c = ucrt_sgetc();
            if (c != '%') { ucrt_sungetc(c); return assigned; }
            continue;
        }
        return assigned;   // conversao desconhecida: para
    }
    return assigned;
}

// ---------------------------------------------------------------------------
//  stdio — ARQUIVOS. fopen abre via CreateFileA -> NtCreateFile -> volume NTFS
//  (\Device\Harddisk0\Partition1). v1: abre EXISTENTE (o sys_createfile ainda nao
//  cria arquivo novo); "w"/"wb" sobrescreve a partir do offset 0 (ntfs_write_file).
//  Sem seek real (nao ha syscall de seek): fseek/ftell sao stubs; p/ reler, reabra
//  (reabrir cria um FILE_OBJECT novo com offset 0).
// ---------------------------------------------------------------------------
__declspec(dllimport) void* CreateFileA(const char* name, unsigned access, unsigned share,
                                        void* sec, unsigned disp, unsigned flags, void* templ);
__declspec(dllimport) int   CloseHandle(void* h);

__declspec(dllexport) void* fopen(const char* path, const char* mode) {
    if (!path || !mode) return 0;
    int write = (mode[0] == 'w' || mode[0] == 'a' || mode[1] == '+');
    unsigned access = write ? 0x40000000u : 0x80000000u;    // GENERIC_WRITE : GENERIC_READ
    unsigned disp   = (mode[0] == 'w') ? 2u : 3u;           // CREATE_ALWAYS : OPEN_EXISTING
    void* h = CreateFileA(path, access, 3u, 0, disp, 0, 0);  // share RW
    if (!h || h == (void*)(unsigned long long)-1) return 0;
    UCRT_FILE* f = (UCRT_FILE*)malloc(sizeof(UCRT_FILE));
    if (!f) { CloseHandle(h); return 0; }
    f->fd = -1; f->handle = h; f->err = 0; f->eof = 0;
    return f;
}
__declspec(dllexport) void* _wfopen(const wchar16* path, const wchar16* mode) {
    (void)path; (void)mode; return 0;   // wide nao suportado por ora
}
__declspec(dllexport) int fclose(void* stream) {
    UCRT_FILE* f = (UCRT_FILE*)stream;
    if (f && f->fd < 0 && f->handle) { CloseHandle(f->handle); f->handle = 0; }
    return 0;   // bump allocator: nao libera o proprio FILE
}
__declspec(dllexport) int feof(void* stream)   { UCRT_FILE* f = (UCRT_FILE*)stream; return f ? f->eof : 0; }
__declspec(dllexport) int ferror(void* stream) { UCRT_FILE* f = (UCRT_FILE*)stream; return f ? f->err : 0; }
__declspec(dllexport) int fflush(void* stream) { (void)stream; return 0; }
__declspec(dllexport) int fseek(void* stream, long off, int whence) {
    (void)stream; (void)off; (void)whence; return 0;   // sem seek real (v1)
}
__declspec(dllexport) long ftell(void* stream) { (void)stream; return 0; }

// ---------------------------------------------------------------------------
//  time — nao usadas pelo alvo; existem p/ resolver os imports. Exportadas como
//  funcoes que devolvem ponteiros p/ estaticos (o startup so referencia).
// ---------------------------------------------------------------------------
static int      g_daylight = 0;
static long     g_timezone = 0;
static char*    g_tzname[2] = { (char*)"GMT", (char*)"GMT" };
__declspec(dllexport) int*    __daylight(void) { return &g_daylight; }
__declspec(dllexport) long*   __timezone(void) { return &g_timezone; }
__declspec(dllexport) char**  __tzname(void)   { return g_tzname; }
__declspec(dllexport) void    _tzset(void)     { }

// ===========================================================================
// Frente C (explorer.exe REAL): a superficie UCRT que o explorer importa. A maior
// parte sao os wrappers _o_* (downlevel) que ENCAMINHAM p/ a base ja definida
// acima; + string/math wide reais; + errno; + terminate HONESTO (C++ EH ainda nao
// implementado -> um throw = terminate, comportamento correto sem handlers).
// Descoberto pelo bring-up: o explorer parava em _o__set_app_type na init do CRT.
// ===========================================================================
static int uc_errno = 0;
static wchar16 uc_wl(wchar16 c) { return (c >= 'A' && c <= 'Z') ? (wchar16)(c + 32) : c; }

// ---- errno ----
__declspec(dllexport) int* _errno(void)       { return &uc_errno; }
__declspec(dllexport) int  _set_errno(int v)  { uc_errno = v; return 0; }
__declspec(dllexport) int  _get_errno(int* p) { if (p) *p = uc_errno; return 0; }

// ---- config / init / exit (base p/ os _o_) ----
__declspec(dllexport) int   _configthreadlocale(int x) { (void)x; return 0; }
__declspec(dllexport) int   _set_fmode(int m)          { (void)m; return 0; }
__declspec(dllexport) int   _initialize_onexit_table(void* t)        { (void)t; return 0; }
__declspec(dllexport) int   _register_onexit_function(void* t, void* f){ (void)t; (void)f; return 0; }
__declspec(dllexport) void  _register_thread_local_exe_atexit_callback(void* f) { (void)f; }
__declspec(dllexport) int   _set_error_mode(int m)     { (void)m; return 0; }
__declspec(dllexport) void  _c_exit(void)              { }
__declspec(dllexport) void* _recalloc(void* p, size_t_ n, size_t_ s) { (void)p; return calloc(n, s); }
__declspec(dllexport) void  _purecall(void)            { ExitProcess((unsigned)-1); }
__declspec(dllexport) void  _invalid_parameter_noinfo(void) { }
__declspec(dllexport) void  _invalid_parameter_noinfo_noreturn(void) { ExitProcess((unsigned)-1); }
__declspec(dllexport) int   _seh_filter_exe(unsigned code, void* p) { (void)code; (void)p; return 0; } // CONTINUE_SEARCH
__declspec(dllexport) unsigned long long _beginthreadex(void* sec, unsigned st, void* start,
        void* arg, unsigned fl, unsigned* id) { (void)sec;(void)st;(void)start;(void)arg;(void)fl;(void)id; return 0; }
__declspec(dllexport) wchar16* _get_wide_winmain_command_line(void) { static wchar16 e[1]={0}; return e; }
__declspec(dllexport) unsigned ___lc_codepage_func(void) { return 0; }
__declspec(dllexport) int*      __p__commode2(void) { return __p__commode(); }

// ---- terminate / C++ EH (ainda nao implementado -> throw = terminate) ----
__declspec(dllexport) void terminate(void)      { uc_puts("[ucrtbase] terminate() RA="); uc_puthex((unsigned long long)(__INTPTR_TYPE__)__builtin_return_address(0)); uc_puts("\n"); ExitProcess(3); for(;;){} }
__declspec(dllexport) void __std_terminate(void){ uc_puts("[ucrtbase] __std_terminate() RA="); uc_puthex((unsigned long long)(__INTPTR_TYPE__)__builtin_return_address(0)); uc_puts("\n"); ExitProcess(3); for(;;){} }
// _CxxThrowException(obj, _ThrowInfo*) — o compilador MSVC emite ISTO p/ cada `throw`
// C++. Sem dispatch de EH aqui, um throw = fim. INSTRUMENTADO: decodifica o tipo lancado.
//   _ThrowInfo{ u32 attrs; i32 pmfnUnwind; i32 pForwardCompat; i32 pCatchableTypeArray }
//   _CatchableTypeArray{ i32 nTypes; i32 arrayOfTypes[] }  (RVAs, base = modulo do throw)
//   _CatchableType{ u32 props; i32 pType; ... }            pType -> _TypeDescriptor (RVA)
//   _TypeDescriptor{ void* pVFTable; void* spare; char name[] }  name @ +0x10 (mangled)
// O nome (ex.: ".?AV_com_error@@") diz EXATAMENTE o que o worker do explorer lanca.
// Backtrace de bring-up: no ponto do throw a pilha ainda esta' INTACTA (o dispatch de EH
// ainda nao rodou). Varremos a pilha por RETURN ADDRESSES que caem na IMAGEM PRINCIPAL
// (o explorer.exe, base = PEB->ImageBaseAddress) e sao precedidos por um CALL — reconstruindo
// a cadeia de chamada callback -> ... -> funcao que fez o THROW_IF_FAILED. Revela EXATAMENTE
// o caminho do throw (RVA = endereco - base), p/ mapear qual init/objeto COM esta' na origem.
static void uc_throw_backtrace(void) {
    unsigned long long rsp; __asm__ volatile("mov %%rsp,%0":"=r"(rsp));
    unsigned long long base = *(volatile unsigned long long*)(0x601000ULL + 0x10);  // PEB->ImageBaseAddress
    if (!base) return;
    unsigned long long lo = base, hi = base + 0x800000ULL;
    uc_puts("[ucrtbase]   backtrace (explorer RVAs de retorno na pilha):\n");
    unsigned long long* sp = (unsigned long long*)rsp;
    int shown = 0;
    for (int i = 0; i < 0x1400 && shown < 40; i++) {
        unsigned long long v = sp[i];
        if (v <= lo + 0x1000ULL || v >= hi) continue;    // ignora header/base
        const unsigned char* p = (const unsigned char*)(__INTPTR_TYPE__)v;
        int iscall = (p[-5]==0xE8) || (p[-6]==0xFF && p[-5]==0x15) ||
                     (p[-2]==0xFF && (p[-1]&0xF8)==0xD0) || (p[-3]==0xFF && (p[-2]&0x38)==0x10);
        if (!iscall) continue;
        uc_puts("    +0x"); uc_puthex(v - base); uc_puts("\n");
        shown++;
    }
}
__declspec(dllexport) void _CxxThrowException(void* obj, void* info) {
    uc_puts("[ucrtbase] _CxxThrowException RA=");
    uc_puthex((unsigned long long)(__INTPTR_TYPE__)__builtin_return_address(0));
    uc_puts(" obj="); uc_puthex((unsigned long long)(__INTPTR_TYPE__)obj);
    uc_puts(" info="); uc_puthex((unsigned long long)(__INTPTR_TYPE__)info);
    const unsigned char* base = info ? uc_module_base(info) : 0;
    if (base) {
        int cta_rva = *(const int*)((const char*)info + 12);        // pCatchableTypeArray
        if (cta_rva) {
            const char* cta = (const char*)base + (unsigned)cta_rva;
            int n = *(const int*)cta;
            if (n > 0) {
                int ct_rva = *(const int*)(cta + 4);                // 1o _CatchableType
                const char* ct = (const char*)base + (unsigned)ct_rva;
                int td_rva = *(const int*)(ct + 4);                 // pType -> _TypeDescriptor
                const char* td = (const char*)base + (unsigned)td_rva;
                uc_puts(" type="); uc_puts(td + 16);                 // name mangled
            }
        }
    }
    uc_puts("\n");
    uc_throw_backtrace();
    // RAISE DE VERDADE: monta o EXCEPTION_RECORD do C++ EH da MS e chama RtlRaiseException.
    // O __CxxFrameHandler3 (estatico no explorer) recebe o dispatch, acha o catch (ex.: o
    // try/catch da threadproc do taskbar em RVA 0x7A880) e trata — o processo NAO morre.
    // Layout do EXCEPTION_RECORD igual ao da ntdll. 4 params (x64 C++ EH):
    //   [0]=EH_MAGIC(0x19930520) [1]=&obj [2]=&ThrowInfo [3]=imagebase (base das RVAs do ThrowInfo).
    struct { unsigned code, flags; void* rec; void* addr; unsigned nparams, pad; unsigned long long info[15]; } er;
    for (unsigned i = 0; i < sizeof(er); i++) ((unsigned char*)&er)[i] = 0;
    er.code = 0xE06D7363u;            // 'msc' | 0xE0000000 = C++ exception
    er.flags = 1;                     // EXCEPTION_NONCONTINUABLE
    er.nparams = 4;
    er.info[0] = 0x19930520ULL;       // EH_MAGIC_NUMBER1
    er.info[1] = (unsigned long long)(__INTPTR_TYPE__)obj;
    er.info[2] = (unsigned long long)(__INTPTR_TYPE__)info;
    er.info[3] = (unsigned long long)(__INTPTR_TYPE__)base;   // imagebase (do modulo do throw)
    RtlRaiseException(&er);
    // So chega aqui se NINGUEM tratou (unhandled) -> terminate, como o Windows.
    uc_puts("[ucrtbase] throw NAO tratado -> terminate\n");
    ExitProcess(3); for(;;){}
}
__declspec(dllexport) int  __CxxFrameHandler3(void* r, void* f, void* c, void* d) { (void)r;(void)f;(void)c;(void)d; return 1; }
__declspec(dllexport) int  __CxxFrameHandler4(void* r, void* f, void* c, void* d) { (void)r;(void)f;(void)c;(void)d; return 1; }
__declspec(dllexport) void __std_exception_copy(void* from, void* to)   { (void)from; (void)to; }
__declspec(dllexport) void __std_exception_destroy(void* obj)           { (void)obj; }

// ---- tempo (minimo) ----
__declspec(dllexport) long long _time64(long long* t) { if (t) *t = 0; return 0; }
__declspec(dllexport) double    _difftime64(long long a, long long b) { return (double)(a - b); }
__declspec(dllexport) void*     _localtime64(long long* t) { (void)t; static long long z[9]={0}; return z; }
__declspec(dllexport) long long _mktime64(void* tm) { (void)tm; return 0; }

// ---- string wide (reais) ----
__declspec(dllexport) int wcscmp(const wchar16* a, const wchar16* b) {
    while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b;
}
__declspec(dllexport) int wcsncmp(const wchar16* a, const wchar16* b, size_t_ n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (int)*a - (int)*b; if (!*a) break; } return 0;
}
__declspec(dllexport) wchar16* wcsncpy(wchar16* d, const wchar16* s, size_t_ n) {
    size_t_ i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d;
}
__declspec(dllexport) size_t_ wcscspn(const wchar16* s, const wchar16* set) {
    size_t_ i = 0; for (; s[i]; i++) { for (const wchar16* p = set; *p; p++) if (s[i] == *p) return i; } return i;
}
__declspec(dllexport) wchar16* wcsstr(const wchar16* h, const wchar16* n) {
    if (!*n) return (wchar16*)h;
    for (; *h; h++) { const wchar16* a = h; const wchar16* b = n;
        while (*a && *b && *a == *b) { a++; b++; } if (!*b) return (wchar16*)h; }
    return 0;
}
__declspec(dllexport) wchar16* _wcsrev(wchar16* s) {
    size_t_ n = 0; while (s[n]) n++; for (size_t_ i = 0; i < n/2; i++) { wchar16 t = s[i]; s[i] = s[n-1-i]; s[n-1-i] = t; } return s;
}
__declspec(dllexport) int _wcsicmp(const wchar16* a, const wchar16* b) {
    while (*a && uc_wl(*a) == uc_wl(*b)) { a++; b++; } return (int)uc_wl(*a) - (int)uc_wl(*b);
}
__declspec(dllexport) int _wcsnicmp(const wchar16* a, const wchar16* b, size_t_ n) {
    for (; n; n--, a++, b++) { wchar16 ca = uc_wl(*a), cb = uc_wl(*b); if (ca != cb) return (int)ca - (int)cb; if (!ca) break; } return 0;
}
__declspec(dllexport) int _wtoi(const wchar16* s) {
    int sign = 1, v = 0; if (!s) return 0; while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v*10 + (*s - '0'); s++; } return v*sign;
}
__declspec(dllexport) long wcstol(const wchar16* s, wchar16** end, int base) {
    long v = 0; int sign = 1; if (!s) return 0; while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    if (base == 0) base = 10;
    for (;;) { int dgt; wchar16 c = *s;
        if (c >= '0' && c <= '9') dgt = c - '0';
        else if (c >= 'a' && c <= 'z') dgt = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') dgt = c - 'A' + 10;
        else break;
        if (dgt >= base) break; v = v*base + dgt; s++; }
    if (end) *end = (wchar16*)s; return v*sign;
}
__declspec(dllexport) int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
__declspec(dllexport) int towlower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
__declspec(dllexport) int iswalnum(int c){ return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
__declspec(dllexport) int iswspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f'; }
__declspec(dllexport) int wcscpy_s(wchar16* d, size_t_ n, const wchar16* s) {
    size_t_ i = 0; if (!d || !s || !n) return 22; for (; s[i] && i < n-1; i++) d[i] = s[i]; d[i] = 0; return 0;
}
__declspec(dllexport) int wcscat_s(wchar16* d, size_t_ n, const wchar16* s) {
    size_t_ l = 0; while (l < n && d[l]) l++; size_t_ i = 0; for (; s[i] && l+i < n-1; i++) d[l+i] = s[i]; d[l+i] = 0; return 0;
}
__declspec(dllexport) int wcsncpy_s(wchar16* d, size_t_ n, const wchar16* s, size_t_ cnt) {
    size_t_ i = 0; if (!d || !n) return 22; for (; s[i] && i < cnt && i < n-1; i++) d[i] = s[i]; d[i] = 0; return 0;
}
__declspec(dllexport) int memcpy_s(void* d, size_t_ dn, const void* s, size_t_ n) {
    if (!d || (s == 0 && n)) return 22; if (n > dn) return 34;
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    for (size_t_ i = 0; i < n; i++) dp[i] = sp[i]; return 0;
}
__declspec(dllexport) int mbstowcs_s(size_t_* ret, wchar16* wc, size_t_ szw, const char* mb, size_t_ cnt) {
    size_t_ i = 0; if (wc && szw) { for (; mb[i] && i < cnt && i < szw-1; i++) wc[i] = (unsigned char)mb[i]; wc[i] = 0; }
    if (ret) *ret = i + 1; return 0;
}
__declspec(dllexport) int _itow_s(int val, wchar16* buf, size_t_ sz, int radix) {
    (void)radix; if (!buf || !sz) return 22; wchar16 tmp[16]; int n = 0; unsigned u = (val < 0) ? -val : val;
    if (u == 0) tmp[n++] = '0'; while (u) { tmp[n++] = '0' + (u % 10); u /= 10; }
    size_t_ o = 0; if (val < 0 && o < sz-1) buf[o++] = '-';
    while (n > 0 && o < sz-1) buf[o++] = tmp[--n]; buf[o] = 0; return 0;
}
__declspec(dllexport) void* bsearch(const void* key, const void* base, size_t_ num, size_t_ sz,
        int (*cmp)(const void*, const void*)) {
    size_t_ lo = 0, hi = num;
    while (lo < hi) { size_t_ mid = (lo + hi) / 2; const void* e = (const char*)base + mid*sz;
        int c = cmp(key, e); if (c == 0) return (void*)e; if (c < 0) hi = mid; else lo = mid + 1; }
    return 0;
}

// ---- math (basico; a maioria nao e' chamada na init) ----
__declspec(dllexport) double ceil(double x)  { long long i = (long long)x; double d = (double)i; return (d < x) ? d + 1.0 : d; }
__declspec(dllexport) double floor(double x) { long long i = (long long)x; double d = (double)i; return (d > x) ? d - 1.0 : d; }
__declspec(dllexport) float  ceilf(float x)  { long long i = (long long)x; float d = (float)i; return (d < x) ? d + 1.0f : d; }
__declspec(dllexport) float  floorf(float x) { long long i = (long long)x; float d = (float)i; return (d > x) ? d - 1.0f : d; }
__declspec(dllexport) float  roundf(float x) { return (float)(long long)(x + (x >= 0 ? 0.5f : -0.5f)); }
__declspec(dllexport) double sqrt(double x)  { double r; __asm__("sqrtsd %1,%0" : "=x"(r) : "x"(x)); return r; }
__declspec(dllexport) double pow(double b, double e) { // e inteiro (caso comum); senao aprox
    int n = (int)e; double r = 1.0; if (n >= 0) while (n--) r *= b; else while (n++) r /= b; return r;
}

// ===========================================================================
//  Wrappers _o_* (downlevel): encaminham p/ as funcoes acima. O explorer os importa
//  de api-ms-win-crt-private-l1-1-0.dll (redirecionado p/ ucrtbase).
// ===========================================================================
__declspec(dllexport) void  _o__set_app_type(int t)          { _set_app_type(t); }
__declspec(dllexport) int   _o__set_new_mode(int m)          { return _set_new_mode(m); }
__declspec(dllexport) int   _o__set_fmode(int m)             { return _set_fmode(m); }
__declspec(dllexport) int   _o__configthreadlocale(int x)    { return _configthreadlocale(x); }
__declspec(dllexport) int   _o__configure_wide_argv(int m)   { return _configure_wide_argv(m); }
__declspec(dllexport) int   _o__initialize_wide_environment(void) { return _initialize_wide_environment(); }
__declspec(dllexport) void  _o__cexit(void)                  { _cexit(); }
__declspec(dllexport) void  _o__exit(int c)                  { _exit(c); }
__declspec(dllexport) void  _o_exit(int c)                   { exit(c); }
__declspec(dllexport) void  _o_terminate(void)              { terminate(); }
__declspec(dllexport) void* _o_malloc(size_t_ n)             { return malloc(n); }
__declspec(dllexport) void  _o_free(void* p)                 { free(p); }
__declspec(dllexport) void* _o_realloc(void* p, size_t_ n)   { return realloc(p, n); }
__declspec(dllexport) void* _o__recalloc(void* p, size_t_ n, size_t_ s) { return _recalloc(p, n, s); }
__declspec(dllexport) int   _o__crt_atexit(void* f)          { return _crt_atexit(f); }
__declspec(dllexport) int   _o__initialize_onexit_table(void* t)         { return _initialize_onexit_table(t); }
__declspec(dllexport) int   _o__register_onexit_function(void* t, void* f){ return _register_onexit_function(t, f); }
__declspec(dllexport) int   _o__seh_filter_exe(unsigned c, void* p)      { return _seh_filter_exe(c, p); }
__declspec(dllexport) void  _o__purecall(void)              { _purecall(); }
__declspec(dllexport) void  _o__invalid_parameter_noinfo(void)          { _invalid_parameter_noinfo(); }
__declspec(dllexport) void  _o__invalid_parameter_noinfo_noreturn(void) { _invalid_parameter_noinfo_noreturn(); }
__declspec(dllexport) int*  _o__errno(void)                 { return _errno(); }
__declspec(dllexport) int   _o__get_errno(int* p)           { return _get_errno(p); }
__declspec(dllexport) int   _o__set_errno(int v)           { return _set_errno(v); }
__declspec(dllexport) unsigned long long _o__beginthreadex(void* a, unsigned b, void* c, void* d, unsigned e2, unsigned* f) { return _beginthreadex(a,b,c,d,e2,f); }
__declspec(dllexport) wchar16* _o__get_wide_winmain_command_line(void) { return _get_wide_winmain_command_line(); }
__declspec(dllexport) int*   _o___p__commode(void)         { return __p__commode(); }
__declspec(dllexport) unsigned _o____lc_codepage_func(void){ return ___lc_codepage_func(); }
__declspec(dllexport) void  _o___std_exception_copy(void* a, void* b)   { __std_exception_copy(a, b); }
__declspec(dllexport) void  _o___std_exception_destroy(void* a)         { __std_exception_destroy(a); }
__declspec(dllexport) int   _o__wcsicmp(const wchar16* a, const wchar16* b)          { return _wcsicmp(a, b); }
__declspec(dllexport) int   _o__wcsnicmp(const wchar16* a, const wchar16* b, size_t_ n){ return _wcsnicmp(a, b, n); }
__declspec(dllexport) int   _o__wtoi(const wchar16* s)     { return _wtoi(s); }
__declspec(dllexport) long  _o_wcstol(const wchar16* s, wchar16** e, int b) { return wcstol(s, e, b); }
__declspec(dllexport) int   _o_toupper(int c)             { return toupper(c); }
__declspec(dllexport) int   _o_towlower(int c)            { return towlower(c); }
__declspec(dllexport) int   _o_iswalnum(int c)           { return iswalnum(c); }
__declspec(dllexport) int   _o_iswspace(int c)           { return iswspace(c); }
__declspec(dllexport) int   _o_wcscpy_s(wchar16* d, size_t_ n, const wchar16* s)  { return wcscpy_s(d, n, s); }
__declspec(dllexport) int   _o_wcscat_s(wchar16* d, size_t_ n, const wchar16* s)  { return wcscat_s(d, n, s); }
__declspec(dllexport) int   _o_wcsncpy_s(wchar16* d, size_t_ n, const wchar16* s, size_t_ c) { return wcsncpy_s(d, n, s, c); }
__declspec(dllexport) int   _o_memcpy_s(void* d, size_t_ dn, const void* s, size_t_ n) { return memcpy_s(d, dn, s, n); }
__declspec(dllexport) int   _o_mbstowcs_s(size_t_* r, wchar16* w, size_t_ sw, const char* m, size_t_ c) { return mbstowcs_s(r, w, sw, m, c); }
__declspec(dllexport) int   _o__itow_s(int v, wchar16* b, size_t_ s, int radix) { return _itow_s(v, b, s, radix); }
__declspec(dllexport) void* _o_bsearch(const void* k, const void* b, size_t_ n, size_t_ s, int(*c)(const void*,const void*)) { return bsearch(k, b, n, s, c); }
__declspec(dllexport) double _o_ceil(double x)  { return ceil(x); }
__declspec(dllexport) double _o_floor(double x) { return floor(x); }
__declspec(dllexport) float  _o_roundf(float x) { return roundf(x); }
__declspec(dllexport) double _o_sqrt(double x)  { return sqrt(x); }
__declspec(dllexport) double _o_pow(double b, double e) { return pow(b, e); }

// ---------------------------------------------------------------------------
// Frente C (explorer real): variantes de STRING do printf/scanf (formata em BUFFER,
// nao stream) que o explorer importa via crt-private. Reusa o va_list x64 (char*,
// 8 bytes/arg) e o mesmo motor de formatacao; versao WIDE (wchar16) abaixo.
// ---------------------------------------------------------------------------
static void ucrt_putuw(wchar16* out, int* pos, int cap, unsigned long long v, int base, int upper, int width, wchar16 pad) {
    wchar16 tmp[32]; int t = 0; const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (wchar16)dig[v % base]; v /= base; }
    while (t < width) tmp[t++] = pad;
    while (t > 0 && *pos < cap) out[(*pos)++] = tmp[--t];
}
// formatador WIDE -> buffer wide. %s/%ls = wide; %hs = narrow; numericos como o narrow.
static int ucrt_vfmtw(wchar16* out, int cap, const wchar16* fmt, char* ap) {
    int pos = 0;
    for (const wchar16* p = fmt; *p && pos < cap; p++) {
        if (*p != '%') { out[pos++] = *p; continue; }
        p++; wchar16 pad = ' '; int width = 0, lng = 0, sht = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width*10 + (*p - '0'); p++; }
        while (*p == 'l') { lng++; p++; }
        while (*p == 'h') { sht++; p++; }
        wchar16 c = *p;
        if (c == 's') {
            if (sht) { const char* s = *(char**)ap; ap += 8; if (!s) s = ""; while (*s && pos<cap) out[pos++]=(wchar16)(unsigned char)*s++; }
            else { const wchar16* s = *(wchar16**)ap; ap += 8; while (s && *s && pos<cap) out[pos++]=*s++; }
        } else if (c == 'c') { int ch = (int)*(long long*)ap; ap += 8; if (pos<cap) out[pos++]=(wchar16)ch; }
        else if (c == 'd' || c == 'i') { long long v = *(long long*)ap; ap += 8; if (!lng) v=(int)v;
            if (v<0) { if (pos<cap) out[pos++]='-'; ucrt_putuw(out,&pos,cap,(unsigned long long)(-v),10,0,width,pad); }
            else ucrt_putuw(out,&pos,cap,(unsigned long long)v,10,0,width,pad); }
        else if (c == 'u') { unsigned long long v=*(unsigned long long*)ap; ap+=8; if (!lng) v=(unsigned)v; ucrt_putuw(out,&pos,cap,v,10,0,width,pad); }
        else if (c == 'x' || c == 'X') { unsigned long long v=*(unsigned long long*)ap; ap+=8; if (!lng) v=(unsigned)v; ucrt_putuw(out,&pos,cap,v,16,c=='X',width,pad); }
        else if (c == 'p') { unsigned long long v=*(unsigned long long*)ap; ap+=8; if (pos<cap) out[pos++]='0'; if (pos<cap) out[pos++]='x'; ucrt_putuw(out,&pos,cap,v,16,0,16,'0'); }
        else if (c == '%') { if (pos<cap) out[pos++]='%'; }
        else { if (pos<cap) out[pos++]='%'; if (pos<cap) out[pos++]=c; }
    }
    return pos;
}
__declspec(dllexport) int __stdio_common_vswprintf(unsigned long long opt, wchar16* buf, size_t_ count, const wchar16* fmt, void* loc, void* valist) {
    (void)opt;(void)loc; if (!buf || !count) return -1;
    int cap = (int)(count > 0x7fffffff ? 0x7fffffff : count);
    int n = ucrt_vfmtw(buf, cap-1, fmt, (char*)valist); buf[n]=0; return n;
}
__declspec(dllexport) int __stdio_common_vswprintf_s(unsigned long long opt, wchar16* buf, size_t_ count, const wchar16* fmt, void* loc, void* valist) {
    return __stdio_common_vswprintf(opt, buf, count, fmt, loc, valist);
}
__declspec(dllexport) int __stdio_common_vsnwprintf_s(unsigned long long opt, wchar16* buf, size_t_ count, size_t_ maxcount, const wchar16* fmt, void* loc, void* valist) {
    (void)maxcount; return __stdio_common_vswprintf(opt, buf, count, fmt, loc, valist);
}
__declspec(dllexport) int __stdio_common_vsnprintf_s(unsigned long long opt, char* buf, size_t_ count, size_t_ maxcount, const char* fmt, void* loc, void* valist) {
    (void)opt;(void)loc;(void)maxcount; if (!buf || !count) return -1;
    int cap = (int)(count > 0x7fffffff ? 0x7fffffff : count);
    int n = ucrt_vfmt(buf, cap-1, fmt, (char*)valist); buf[n]=0; return n;
}
__declspec(dllexport) int __stdio_common_vswscanf(unsigned long long opt, const wchar16* buf, size_t_ count, const wchar16* fmt, void* loc, void* valist) {
    (void)opt;(void)count;(void)loc; const wchar16* s = buf; char* ap = (char*)valist; int assigned = 0;
    for (const wchar16* p = fmt; *p; p++) {
        if (*p==' '||*p=='\t'||*p=='\n') { while (*s==' '||*s=='\t'||*s=='\n') s++; continue; }
        if (*p != '%') { if (*s==*p) s++; else break; continue; }
        p++; while (*p>='0'&&*p<='9') p++; while (*p=='l'||*p=='h') p++; wchar16 c = *p;
        while (*s==' '||*s=='\t'||*s=='\n') s++;
        if (c=='d'||c=='i'||c=='u') { long long v=0; int neg=0; if (*s=='-'){neg=1;s++;} if (!(*s>='0'&&*s<='9')) break; while (*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} if (neg) v=-v; int* d=*(int**)ap; ap+=8; *d=(int)v; assigned++; }
        else if (c=='x'||c=='X') { unsigned long long v=0; int any=0; for(;;){ wchar16 h=*s; int dv; if(h>='0'&&h<='9')dv=h-'0'; else if(h>='a'&&h<='f')dv=h-'a'+10; else if(h>='A'&&h<='F')dv=h-'A'+10; else break; v=v*16+dv; s++; any=1; } if(!any) break; unsigned* d=*(unsigned**)ap; ap+=8; *d=(unsigned)v; assigned++; }
        else if (c=='s') { wchar16* d=*(wchar16**)ap; ap+=8; while (*s && !(*s==' '||*s=='\t'||*s=='\n')) *d++=*s++; *d=0; assigned++; }
        else break;
    }
    return assigned;
}
// aliases _o_ (mesma impl; o explorer importa a variante _o_ via crt-private) --------
__declspec(dllexport) int _o___stdio_common_vswprintf(unsigned long long o, wchar16* b, size_t_ c, const wchar16* f, void* l, void* v) { return __stdio_common_vswprintf(o,b,c,f,l,v); }
__declspec(dllexport) int _o___stdio_common_vswprintf_s(unsigned long long o, wchar16* b, size_t_ c, const wchar16* f, void* l, void* v) { return __stdio_common_vswprintf_s(o,b,c,f,l,v); }
__declspec(dllexport) int _o___stdio_common_vsnwprintf_s(unsigned long long o, wchar16* b, size_t_ c, size_t_ m, const wchar16* f, void* l, void* v) { return __stdio_common_vsnwprintf_s(o,b,c,m,f,l,v); }
__declspec(dllexport) int _o___stdio_common_vsnprintf_s(unsigned long long o, char* b, size_t_ c, size_t_ m, const char* f, void* l, void* v) { return __stdio_common_vsnprintf_s(o,b,c,m,f,l,v); }
__declspec(dllexport) int _o___stdio_common_vswscanf(unsigned long long o, const wchar16* b, size_t_ c, const wchar16* f, void* l, void* v) { return __stdio_common_vswscanf(o,b,c,f,l,v); }
__declspec(dllexport) double    _o__difftime64(long long a, long long b) { return _difftime64(a,b); }
__declspec(dllexport) void*     _o__localtime64(long long* t) { return _localtime64(t); }
__declspec(dllexport) long long _o__mktime64(void* tm) { return _mktime64(tm); }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
