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

__declspec(dllimport) void ExitProcess(unsigned code);

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
__declspec(dllexport) void exit(int code)  { ExitProcess((unsigned)code); }
__declspec(dllexport) void _exit(int code) { ExitProcess((unsigned)code); }
__declspec(dllexport) void abort(void)     { ExitProcess(3); }

// ---------------------------------------------------------------------------
//  private / SEH — __C_specific_handler. So existe p/ resolver; sem exceptions
//  reais devolve ExceptionContinueSearch (1).
// ---------------------------------------------------------------------------
__declspec(dllexport) int __C_specific_handler(void* rec, void* frame, void* ctx, void* disp) {
    (void)rec; (void)frame; (void)ctx; (void)disp; return 1;
}

// ---------------------------------------------------------------------------
//  stdio — stubs. O alvo (crthello) escreve via WriteFile/kernel32, entao estas
//  so precisam existir e nao quebrar num flush de saida na saida do processo.
// ---------------------------------------------------------------------------
static unsigned char g_iob[3][64];   // FILE* falsos p/ stdin/stdout/stderr
__declspec(dllexport) void* __acrt_iob_func(unsigned i) {
    return (i < 3) ? (void*)&g_iob[i][0] : (void*)&g_iob[1][0];
}
__declspec(dllexport) size_t_ fwrite(const void* p, size_t_ sz, size_t_ n, void* stream) {
    (void)p; (void)sz; (void)stream; return n;   // finge ter escrito 'n' elementos
}
__declspec(dllexport) int __stdio_common_vfprintf(unsigned long long opt, void* stream,
        const char* fmt, void* loc, void* valist) {
    (void)opt; (void)stream; (void)fmt; (void)loc; (void)valist; return 0;
}
__declspec(dllexport) int __stdio_common_vfwprintf(unsigned long long opt, void* stream,
        const wchar16* fmt, void* loc, void* valist) {
    (void)opt; (void)stream; (void)fmt; (void)loc; (void)valist; return 0;
}

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
