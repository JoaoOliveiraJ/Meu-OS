// shcore.dll — SHELL CORE minimo p/ o explorer.exe REAL (DPI/scaling, IStream helpers,
// registro SHReg*, thread refs, AppUserModelID, string). O explorer importa ~53 funcoes
// (SHCORE.dll + apisets api-ms-win-shcore-*). Impl reais onde barato (DPI=96, AppUserModelID,
// string dup/convert, IsOS); stubs ESPECIFICOS onde falta COM/IStream/registro real. Cada
// nome e ordinal aponta explicitamente no shcore.def. Autocontido (caminho PMM+reloc limpo).
typedef unsigned short     WCHAR_;
typedef unsigned long long size_t_;
typedef long               HRESULT_;
unsigned int _tls_index = 0;
#define S_OK_      ((HRESULT_)0)
#define E_FAIL_    ((HRESULT_)0x80004005L)
#define E_NOTIMPL_ ((HRESULT_)0x80004001L)

static unsigned char g_sccheap[0x40000]; static size_t_ g_scoff = 0;
static void* sc_alloc(size_t_ n){ n=(n+15)&~(size_t_)15; if(!n||g_scoff+n>sizeof(g_sccheap)) return 0; void* p=&g_sccheap[g_scoff]; g_scoff+=n; return p; }
static size_t_ sc_wlen(const WCHAR_* s){ size_t_ n=0; if(s) while(s[n]) n++; return n; }

// ---- DPI / scaling: 96 DPI (100%), sem monitor de alta densidade ----
__declspec(dllexport) HRESULT_ GetDpiForMonitor(void* hmon, int type, unsigned* dpiX, unsigned* dpiY) { (void)hmon;(void)type; if (dpiX)*dpiX=96; if (dpiY)*dpiY=96; return S_OK_; }
__declspec(dllexport) HRESULT_ GetScaleFactorForMonitor(void* hmon, int* scale) { (void)hmon; if (scale)*scale=100; return S_OK_; }   // SCALE_100_PERCENT

// ---- AppUserModelID / IsOS / CommandLineToArgvW ----
__declspec(dllexport) HRESULT_ SetCurrentProcessExplicitAppUserModelID(const WCHAR_* id) { (void)id; return S_OK_; }
__declspec(dllexport) HRESULT_ GetCurrentProcessExplicitAppUserModelID(WCHAR_** id) { if (id)*id=0; return E_FAIL_; }
__declspec(dllexport) int IsOS(int os) { (void)os; return 0; }
__declspec(dllexport) void** CommandLineToArgvW(const WCHAR_* cmd, int* argc) { (void)cmd; if (argc)*argc=0; return 0; }

// ---- string: dup/convert reais ----
__declspec(dllexport) HRESULT_ SHStrDupW(const WCHAR_* src, WCHAR_** out) {
    if (!out) return E_FAIL_; *out=0; if (!src) return S_OK_;
    size_t_ n=sc_wlen(src)+1; WCHAR_* d=(WCHAR_*)sc_alloc(n*2); if (!d) return E_FAIL_;
    for (size_t_ i=0;i<n;i++) d[i]=src[i]; *out=d; return S_OK_;
}
__declspec(dllexport) int SHAnsiToUnicode(const char* src, WCHAR_* dst, int cch) { int i=0; if(!dst||cch<=0) return 0; for(; src&&src[i] && i<cch-1; i++) dst[i]=(WCHAR_)(unsigned char)src[i]; dst[i]=0; return i+1; }
__declspec(dllexport) int SHUnicodeToAnsi(const WCHAR_* src, char* dst, int cch) { int i=0; if(!dst||cch<=0) return 0; for(; src&&src[i] && i<cch-1; i++) dst[i]=(char)src[i]; dst[i]=0; return i+1; }

// ---- IStream helpers / mem stream: sem COM stream real -> NULL/E_FAIL ----
__declspec(dllexport) void* SHCreateMemStream(const void* init, unsigned cb) { (void)init;(void)cb; return 0; }
__declspec(dllexport) HRESULT_ SHCreateStreamOnFileEx(const WCHAR_* file, unsigned mode, unsigned attr, int create, void* templ, void** stream) { (void)file;(void)mode;(void)attr;(void)create;(void)templ; if (stream)*stream=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHCreateStreamOnFileW(const WCHAR_* file, unsigned mode, void** stream) { (void)file;(void)mode; if (stream)*stream=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ CreateStreamOverRandomAccessStream(void* ras, const void* riid, void** ppv) { (void)ras;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IStream_Read(void* s, void* buf, unsigned cb) { (void)s;(void)buf;(void)cb; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IStream_Write(void* s, const void* buf, unsigned cb) { (void)s;(void)buf;(void)cb; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IStream_Reset(void* s) { (void)s; return E_FAIL_; }
__declspec(dllexport) void* SHOpenRegStream2W(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned mode) { (void)hkey;(void)subkey;(void)value;(void)mode; return 0; }

// ---- registro SHReg* (helpers): "nao encontrado" ----
#define ERR_FILE_NOT_FOUND 2L
__declspec(dllexport) long SHRegGetValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned flags, unsigned* type, void* data, unsigned* cb) { (void)hkey;(void)subkey;(void)value;(void)flags; if (type)*type=0; if (cb)*cb=0; (void)data; return ERR_FILE_NOT_FOUND; }
__declspec(dllexport) long SHGetValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned* type, void* data, unsigned* cb) { (void)hkey;(void)subkey;(void)value; if (type)*type=0; if (cb)*cb=0; (void)data; return ERR_FILE_NOT_FOUND; }
__declspec(dllexport) long SHSetValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value, unsigned type, const void* data, unsigned cb) { (void)hkey;(void)subkey;(void)value;(void)type;(void)data;(void)cb; return 0; }
__declspec(dllexport) long SHDeleteKeyW(void* hkey, const WCHAR_* subkey) { (void)hkey;(void)subkey; return 0; }
__declspec(dllexport) long SHDeleteValueW(void* hkey, const WCHAR_* subkey, const WCHAR_* value) { (void)hkey;(void)subkey;(void)value; return 0; }
__declspec(dllexport) long SHEnumKeyExW(void* hkey, unsigned idx, WCHAR_* name, unsigned* cch) { (void)hkey;(void)idx;(void)name; if (cch)*cch=0; return 259L; }   // ERROR_NO_MORE_ITEMS
__declspec(dllexport) long SHQueryInfoKeyW(void* hkey, unsigned* subkeys, unsigned* maxsub, unsigned* values, unsigned* maxval) { (void)hkey; if (subkeys)*subkeys=0; if (maxsub)*maxsub=0; if (values)*values=0; if (maxval)*maxval=0; return 0; }
__declspec(dllexport) long SHRegGetValueFromHKCUHKLM(const WCHAR_* subkey, const WCHAR_* value, unsigned flags, unsigned* type, void* data, unsigned* cb) { (void)subkey;(void)value;(void)flags; if (type)*type=0; if (cb)*cb=0; (void)data; return ERR_FILE_NOT_FOUND; }

// ---- thread refs / taskpool: single-threaded -> refs nulos, task no-op ----
__declspec(dllexport) HRESULT_ SHCreateThreadRef(long* cnt, void** ppunk) { (void)cnt; if (ppunk)*ppunk=0; return E_NOTIMPL_; }
__declspec(dllexport) HRESULT_ SHGetThreadRef(void** ppunk) { if (ppunk)*ppunk=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ SHSetThreadRef(void* punk) { (void)punk; return S_OK_; }
__declspec(dllexport) int SHCreateThread(void* fn, void* param, unsigned flags, void* initfn) { (void)fn;(void)param;(void)flags;(void)initfn; return 1; }
__declspec(dllexport) HRESULT_ SHTaskPoolQueueTask(void* pool, void* task) { (void)pool;(void)task; return E_NOTIMPL_; }
__declspec(dllexport) void* SHTaskPoolGetUniqueContext(void) { return 0; }
__declspec(dllexport) HRESULT_ SetProcessReference(void* punk) { (void)punk; return S_OK_; }

// ---- IUnknown site/service helpers (COM): sem objeto real -> E_FAIL/S_OK ----
__declspec(dllexport) HRESULT_ IUnknown_SetSite(void* unk, void* site) { (void)unk;(void)site; return S_OK_; }
__declspec(dllexport) HRESULT_ IUnknown_GetSite(void* unk, const void* riid, void** site) { (void)unk;(void)riid; if (site)*site=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IUnknown_QueryService(void* unk, const void* guid, const void* riid, void** ppv) { (void)unk;(void)guid;(void)riid; if (ppv)*ppv=0; return E_FAIL_; }
__declspec(dllexport) HRESULT_ IUnknown_Set(void** dst, void* punk) { (void)punk; if (dst)*dst=0; return S_OK_; }

// ---- stub generico ESPECIFICO p/ ordinais internos ainda nao mapeados: devolve 0 ----
__declspec(dllexport) long long shcore_ord_stub(void) { return 0; }

int DllMain(void* h, unsigned reason, void* rsv) { (void)h; (void)reason; (void)rsv; return 1; }
