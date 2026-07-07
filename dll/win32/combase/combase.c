// combase.dll — COM base MINIMO p/ rodar o explorer.exe REAL. Descoberto pelo
// bring-up: o explorer chama CoTaskMemAlloc na inicializacao de COM (a fase apos o
// CRT). Por ora: o alocador de tarefa do COM (CoTaskMemAlloc/Free/Realloc) sobre uma
// arena bump propria + stubs Co* de init de apartamento. Objetos/apartamentos/class
// factories reais vem depois (esta e' a entrada na fase COM). Sem stub generico.
typedef unsigned long long size_t_;
unsigned int _tls_index = 0;   // exigido pelo runtime de DLL (igual as outras DLLs)

// Arena de 1 MiB: cabe no mapeamento de usuario de 2 MiB que o loader abre por DLL
// (ldr_load -> mm_map_user(base, 0x200000)); o codigo aqui e' pequeno.
static unsigned char g_comheap[0x100000];
static size_t_ g_comoff = 0;

__declspec(dllexport) void* CoTaskMemAlloc(size_t_ cb) {
    cb = (cb + 15) & ~(size_t_)15;
    if (!cb || g_comoff + cb > sizeof(g_comheap)) return 0;
    void* p = &g_comheap[g_comoff]; g_comoff += cb; return p;
}
__declspec(dllexport) void  CoTaskMemFree(void* p) { (void)p; }   // bump: nao libera (por ora)
__declspec(dllexport) void* CoTaskMemRealloc(void* p, size_t_ cb) { (void)p; return CoTaskMemAlloc(cb); }

// Init de apartamento COM (honesto: S_OK; sem apartamentos/STA/MTA reais ainda).
__declspec(dllexport) long CoInitialize(void* rsv)              { (void)rsv; return 0; }        // S_OK
__declspec(dllexport) long CoInitializeEx(void* rsv, unsigned f){ (void)rsv; (void)f; return 0; }
__declspec(dllexport) void CoUninitialize(void)                { }
__declspec(dllexport) long CoCreateInstance(const void* clsid, void* outer, unsigned ctx,
        const void* iid, void** out) { (void)clsid; (void)outer; (void)ctx; (void)iid;
        if (out) *out = 0; return (long)0x80004001; }   // E_NOTIMPL (sem class factories ainda)
__declspec(dllexport) long CoGetMalloc(unsigned ctx, void** out) { (void)ctx; if (out) *out = 0; return (long)0x80004001; }

int DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }
