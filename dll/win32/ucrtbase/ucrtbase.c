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

// ---------------------------------------------------------------------------
//  runtime startup — argv/env, _initterm, config, exit.
// ---------------------------------------------------------------------------
static int      g_argc = 1;
static char     g_arg0[] = "crthello.exe";
static char*    g_argv[2]  = { g_arg0, 0 };
static char**   g_argv_p   = g_argv;
static wchar16  g_warg0[1] = { 0 };
static wchar16* g_wargv[1] = { g_warg0 };
static wchar16** g_wargv_p = g_wargv;
static char*    g_env[1]   = { 0 };
static char**   g_env_p    = g_env;
static wchar16* g_wenv[1]  = { 0 };
static wchar16** g_wenv_p  = g_wenv;
static int      g_commode  = 0;
static int      g_fmode    = 0;

__declspec(dllexport) int*       __p___argc(void)  { return &g_argc; }
__declspec(dllexport) char***    __p___argv(void)  { return &g_argv_p; }
__declspec(dllexport) wchar16*** __p___wargv(void) { return &g_wargv_p; }
__declspec(dllexport) char***    __p__environ(void)  { return &g_env_p; }
__declspec(dllexport) wchar16*** __p__wenviron(void) { return &g_wenv_p; }
__declspec(dllexport) int*        __p__commode(void) { return &g_commode; }
__declspec(dllexport) int*        __p__fmode(void)   { return &g_fmode; }

// __p__acmdln: ponteiro p/ a string bruta da linha de comando (o startup GUI do CRT,
// WinMainCRTStartup, usa p/ montar lpCmdLine). _ismbblead: sem MBCS, nunca e' lead-byte.
static char* g_acmdln = (char*)"";
__declspec(dllexport) char** __p__acmdln(void)    { return &g_acmdln; }
__declspec(dllexport) int    _ismbblead(unsigned c) { (void)c; return 0; }

__declspec(dllexport) int  _configure_narrow_argv(int mode) { (void)mode; return 0; }
__declspec(dllexport) int  _configure_wide_argv(int mode)   { (void)mode; return 0; }
__declspec(dllexport) int  _initialize_narrow_environment(void) { return 0; }
__declspec(dllexport) int  _initialize_wide_environment(void)   { return 0; }
__declspec(dllexport) void _set_app_type(int t) { (void)t; }
__declspec(dllexport) void* _set_invalid_parameter_handler(void* h) { (void)h; return 0; }
__declspec(dllexport) void  __setusermatherr(void* h) { (void)h; }
__declspec(dllexport) int   _crt_atexit(void* fn)      { (void)fn; return 0; }
__declspec(dllexport) int   _crt_at_quick_exit(void* fn){ (void)fn; return 0; }
__declspec(dllexport) void* signal(int sig, void* handler) { (void)sig; (void)handler; return 0; }

// _initterm: no Windows chama cada ponteiro de funcao em [first,last) (inicializadores
// C/C++). Para um hello C sem construtores estaticos, e' seguro NAO chamar a tabela —
// os inicializadores internos do CRT que pularizamos ja estao stubbados aqui. Isso
// evita cascatear em codigo de CRT nao implementado.
__declspec(dllexport) void _initterm(void* first, void* last)   { (void)first; (void)last; }
__declspec(dllexport) int  _initterm_e(void* first, void* last) { (void)first; (void)last; return 0; }

__declspec(dllexport) void _cexit(void) { }
// ExitProcess encerra o processo; o for(;;) so satisfaz o 'noreturn' que o compilador
// assume p/ exit/_exit/abort (ele nao sabe que ExitProcess nao retorna).
__declspec(dllexport) void exit(int code)  { ExitProcess((unsigned)code); for (;;) {} }
__declspec(dllexport) void _exit(int code) { ExitProcess((unsigned)code); for (;;) {} }
__declspec(dllexport) void abort(void)     { ExitProcess(3);              for (;;) {} }

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

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
