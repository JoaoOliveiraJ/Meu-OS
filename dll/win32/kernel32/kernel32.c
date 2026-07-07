// kernel32.dll  —  reimplementacao. Exporta a API Win32 classica; encaminha
// tudo para o ntdll (que faz a syscall). Igual no Windows.
unsigned int _tls_index = 0;

__declspec(dllimport) void NtTerminateProcess(unsigned code);
__declspec(dllimport) long NtCreateFile(void* out_handle, const char* name);
__declspec(dllimport) long NtDeviceIoControlFile(void* handle, unsigned ioctl,
        void* in_buf, unsigned in_len, void* out_buf, unsigned out_len);
__declspec(dllimport) void NtClose(void* handle);
__declspec(dllimport) long NtCreateProcess(void* out_handle, const char* image_name, const char* cmdline);
__declspec(dllimport) long NtCreateThread(void* out_handle, void* process, void* start);
__declspec(dllimport) long NtWaitForSingleObject(void* handle, unsigned timeout_ms);
__declspec(dllimport) long NtWriteFile(void* handle, const void* buf, unsigned len, unsigned* written);
__declspec(dllimport) long NtReadFile(void* handle, void* buf, unsigned len, unsigned* read);
__declspec(dllimport) long NtQueryDirectoryFile(void* dirHandle, void* outBuf, unsigned outLen, unsigned* retLen);
__declspec(dllimport) long NtQueryVolumeInformation(void* outBuf, unsigned outLen, unsigned* retLen);
__declspec(dllimport) void* LdrGetModuleHandle(const char* name);
__declspec(dllimport) void* LdrGetProcAddress(void* module_base, const char* fn);
__declspec(dllimport) void* LdrLoadDll(const char* name);
typedef unsigned short wchar16;                                   // UTF-16 (def canonica adiante)
static void k32_wtoasc(char* d, const wchar16* s, int cap);       // fwd: usado por LoadLibraryW
__declspec(dllimport) long NtCreateNamedPipeFile(void* out_handle, const char* name);
__declspec(dllimport) long NtConnectNamedPipe(void* pipe_handle);
// FASE 5 — enumeracao de objetos + carregar/descarregar driver (para o cmd.exe).
__declspec(dllimport) long NtEnumProcesses(unsigned index, void* out);
__declspec(dllimport) long NtEnumDrivers(unsigned index, void* out);
__declspec(dllimport) long NtLoadDriver(const char* name);
__declspec(dllimport) long NtUnloadDriver(const char* name);
// Frente C (explorer real): tempo do sistema + info de processo (do ntdll).
__declspec(dllimport) long NtQuerySystemTime(void* out);
__declspec(dllimport) long NtQueryInformationProcess(void* h, unsigned cls, void* buf, unsigned len, unsigned* ret);
__declspec(dllimport) void* NtVirtualAlloc(unsigned long long size);   // Frente C: backing do heap

#define INVALID_HANDLE_VALUE ((void*)(long long)-1)

// Handles padrao (GetStdHandle). Valores do Win32; o kernel os reconhece como
// "console device" e escreve/le pela saida do kernel (VGA+serial).
#define STD_INPUT_HANDLE  ((unsigned)-10)
#define STD_OUTPUT_HANDLE ((unsigned)-11)
#define STD_ERROR_HANDLE  ((unsigned)-12)

__declspec(dllexport) void ExitProcess(unsigned code) {
    NtTerminateProcess(code);
}

__declspec(dllexport) void* CreateFileA(const char* name, unsigned access, unsigned share,
        void* sec, unsigned disposition, unsigned flags, void* templ) {
    (void)access; (void)share; (void)sec; (void)disposition; (void)flags; (void)templ;
    void* handle = 0;
    if (NtCreateFile(&handle, name) != 0) return INVALID_HANDLE_VALUE;
    return handle;
}

// --- Named Pipes (IPC) — FASE 3 ---
// CreateNamedPipeA(name, openMode, pipeMode, maxInstances, outBuf, inBuf,
//                  defaultTimeout, securityAttributes) -> HANDLE do servidor.
// O kernel cria \Pipe\Nome e devolve um handle de pipe. Os parametros de tamanho/
// modo sao aceitos (assinatura compativel com o Win32) mas simplificados.
__declspec(dllexport) void* CreateNamedPipeA(const char* name, unsigned openMode,
        unsigned pipeMode, unsigned maxInstances, unsigned outBufSize,
        unsigned inBufSize, unsigned defaultTimeout, void* sec) {
    (void)openMode; (void)pipeMode; (void)maxInstances;
    (void)outBufSize; (void)inBufSize; (void)defaultTimeout; (void)sec;
    void* handle = 0;
    if (NtCreateNamedPipeFile(&handle, name) != 0) return INVALID_HANDLE_VALUE;
    return handle;
}

// ConnectNamedPipe(hPipe, overlapped) -> BOOL. O servidor aguarda um cliente
// (aqui nao bloqueia: sem escalonador). Retorna TRUE se o pipe ficou utilizavel.
__declspec(dllexport) int ConnectNamedPipe(void* hPipe, void* overlapped) {
    (void)overlapped;
    return NtConnectNamedPipe(hPipe) == 0 ? 1 : 0;   // TRUE/FALSE
}

__declspec(dllexport) int DeviceIoControl(void* hDevice, unsigned ioctl,
        void* in_buf, unsigned in_len, void* out_buf, unsigned out_len,
        unsigned* bytes_returned, void* overlapped) {
    (void)overlapped;
    long st = NtDeviceIoControlFile(hDevice, ioctl, in_buf, in_len, out_buf, out_len);
    if (bytes_returned) *bytes_returned = out_len;
    return st == 0 ? 1 : 0;   // TRUE/FALSE
}

__declspec(dllexport) int CloseHandle(void* handle) {
    // Os handles padrao (GetStdHandle) sao pseudo-handles: nada a fechar.
    long long v = (long long)(__INTPTR_TYPE__)handle;
    if (v >= -12 && v <= -10) return 1;
    NtClose(handle);
    return 1;
}

// GetStdHandle(STD_*_HANDLE) -> pseudo-handle do console (igual no Windows: um
// valor sentinela, nao um handle de verdade). NtWriteFile o reconhece.
__declspec(dllexport) void* GetStdHandle(unsigned which) {
    return (void*)(long long)(int)which;   // sign-extend: -11 -> 0xFFFFFFFFFFFFFFF5
}

// WriteFile(handle, buf, len, &written, overlapped). Escreve via NtWriteFile.
__declspec(dllexport) int WriteFile(void* handle, const void* buf, unsigned len,
        unsigned* written, void* overlapped) {
    (void)overlapped;
    long st = NtWriteFile(handle, buf, len, written);
    return st == 0 ? 1 : 0;   // TRUE/FALSE
}

// ReadFile(handle, buf, len, &read, overlapped). Le via NtReadFile.
__declspec(dllexport) int ReadFile(void* handle, void* buf, unsigned len,
        unsigned* read, void* overlapped) {
    (void)overlapped;
    long st = NtReadFile(handle, buf, len, read);
    return st == 0 ? 1 : 0;
}

// GetModuleHandleA(name): base da imagem ja carregada (via o loader do kernel).
// name==0 -> NULL aqui (simplificado; o NT devolveria a base do .exe corrente).
__declspec(dllexport) void* GetModuleHandleA(const char* name) {
    if (!name) return 0;
    return LdrGetModuleHandle(name);
}

// GetProcAddress(module, name): caminha a export table da imagem (via kernel).
__declspec(dllexport) void* GetProcAddress(void* module, const char* name) {
    if (!module || !name) return 0;
    return LdrGetProcAddress(module, name);
}

// LoadLibraryA(name) — FASE 3f: carrega uma DLL em RUNTIME (via LdrLoadDll -> ldr_load do
// kernel) e devolve o HMODULE (base). GetProcAddress resolve funcoes por cima. Por ora
// carrega DLLs ja registradas como modulos de boot (nao le do disco).
__declspec(dllexport) void* LoadLibraryA(const char* name) {
    if (!name) return 0;
    return LdrLoadDll(name);
}
// LoadLibraryW(name) — versao WIDE. O explorer carrega quase tudo por aqui (LoadLibraryW/
// Ex). Converte UTF-16->ascii e delega ao LdrLoadDll (syscall -> ldr_load_runtime, que
// aplica apiset_redirect + anexa ".dll"). Antes era um stub que devolvia 0 SEMPRE, o que
// abortava silenciosamente TODA carga wide (inclusive de DLLs que ja temos como modulo).
__declspec(dllexport) void* LoadLibraryW(const wchar16* name) {
    if (!name) return 0;
    char nb[128]; k32_wtoasc(nb, name, sizeof(nb));
    return LdrLoadDll(nb);
}

// --- Process Manager (Win32 -> Nt*) ---
// CreateProcessA: cria um EPROCESS + thread inicial (objetos do Object Manager), RODA a
// imagem do filho (o kernel executa via sys_createprocess, sincrono) e devolve os handles.
// lpCommandLine (cmdline) leva o argv do filho ate o CRT dele.
__declspec(dllexport) int CreateProcessA(const char* app, char* cmdline, void* pa, void* ta,
        int inherit, unsigned flags, void* env, const char* cwd, void* si, void* pi) {
    (void)pa; (void)ta; (void)inherit; (void)flags; (void)env; (void)cwd; (void)si;
    void* hproc = 0;
    // Se lpCommandLine for NULL, o Windows usa lpApplicationName como a linha de comando.
    if (NtCreateProcess(&hproc, app, cmdline ? cmdline : (char*)app) != 0) return 0;   // FALSE
    void* hthr = 0;
    NtCreateThread(&hthr, hproc, 0);
    if (pi) { void** out = (void**)pi; out[0] = hproc; out[1] = hthr; }   // hProcess, hThread
    return 1;   // TRUE
}

__declspec(dllexport) unsigned WaitForSingleObject(void* handle, unsigned timeout_ms) {
    NtWaitForSingleObject(handle, timeout_ms);
    return 0;   // WAIT_OBJECT_0
}

// ============================================================================
//  FASE 5 — apoio ao shell (cmd.exe): enumerar processos/drivers e
//  carregar/descarregar drivers de kernel. Encaminha para o ntdll (Nt*).
//  As structs abaixo TEM o mesmo layout que as do kernel (ke/syscall.c).
// ============================================================================
// Estado de um driver no registro do I/O Manager (espelha SERVICE_* do SCM).
#define MEUOS_SERVICE_STOPPED  1
#define MEUOS_SERVICE_RUNNING  4

typedef struct _MEUOS_PROCESS_ENTRY {
    unsigned       ProcessId;
    unsigned       Terminated;
    unsigned long long ImageBase;
    unsigned       ThreadCount;
    char           ImageName[32];
} MEUOS_PROCESS_ENTRY;

typedef struct _MEUOS_DRIVER_ENTRY {
    unsigned State;
    unsigned LastStatus;
    char     Name[32];
} MEUOS_DRIVER_ENTRY;

// EnumProcessesEx(index, *out): preenche *out com o n-esimo EPROCESS. Retorna
// TRUE (1) enquanto houver; FALSE (0) no fim. (tasklist itera index=0,1,2,...)
__declspec(dllexport) int EnumProcessesEx(unsigned index, MEUOS_PROCESS_ENTRY* out) {
    return NtEnumProcesses(index, out) ? 1 : 0;
}

// EnumDriversEx(index, *out): preenche *out com o n-esimo driver conhecido
// (sc query). Retorna TRUE enquanto houver; FALSE no fim.
__declspec(dllexport) int EnumDriversEx(unsigned index, MEUOS_DRIVER_ENTRY* out) {
    return NtEnumDrivers(index, out) ? 1 : 0;
}

// StartDriverServiceA(name): 'sc start' de um driver de kernel. TRUE se subiu.
__declspec(dllexport) int StartDriverServiceA(const char* name) {
    return NtLoadDriver(name) == 0 ? 1 : 0;
}

// StopDriverServiceA(name): 'sc stop' de um driver de kernel. TRUE se parou.
__declspec(dllexport) int StopDriverServiceA(const char* name) {
    return NtUnloadDriver(name) == 0 ? 1 : 0;
}

// ============================================================================
//  FASE 3 (NTFS) — apoio ao 'dir' do cmd.exe: enumerar um diretorio do volume.
//  Layout casa com NTFS_DIR_ENTRY_OUT (src/drivers/ntfs_fs.c).
// ============================================================================
typedef struct _MEUOS_DIR_ENTRY {
    unsigned long long MftRecord;
    unsigned           IsDir;
    unsigned           Pad;
    unsigned long long Size;
    char               Name[256];
} MEUOS_DIR_ENTRY;

// QueryDirectoryFileEx(hDir, *out): devolve a PROXIMA entrada do diretorio
// aberto (CreateFileA no \Device\Harddisk0\Partition1). TRUE enquanto houver;
// FALSE no fim (STATUS_NO_MORE_FILES). O 'dir' itera ate FALSE.
__declspec(dllexport) int QueryDirectoryFileEx(void* hDir, MEUOS_DIR_ENTRY* out) {
    unsigned ret = 0;
    long st = NtQueryDirectoryFile(hDir, out, (unsigned)sizeof(MEUOS_DIR_ENTRY), &ret);
    return (st == 0 && ret > 0) ? 1 : 0;
}

// FASE 5 — resumo do volume (vol / dir): rotulo, serial, fs name, tamanho. Layout
// casa com MEUOS_VOLUME_INFO do kernel (ke/syscall.c).
typedef struct _MEUOS_VOLUME_INFO {
    unsigned long long Serial;
    unsigned long long TotalBytes;
    unsigned long long FreeBytes;
    unsigned           BytesPerSector;
    unsigned           BytesPerCluster;
    char               FsName[8];
    char               Label[32];
} MEUOS_VOLUME_INFO;

// QueryVolumeInfoEx(*out): preenche *out com a info do volume NTFS montado (C:).
// TRUE se ha volume e a info coube; FALSE senao. Apoia o comando 'vol' do cmd.
__declspec(dllexport) int QueryVolumeInfoEx(MEUOS_VOLUME_INFO* out) {
    unsigned ret = 0;
    long st = NtQueryVolumeInformation(out, (unsigned)sizeof(MEUOS_VOLUME_INFO), &ret);
    return (st == 0) ? 1 : 0;
}

// ============================================================================
//  FASE 3b (Frente 3) — apoio ao CRT REAL (mingw/UCRT). O startup do CRT importa
//  estas do kernel32. Em UP single-app, CriticalSection sao no-ops; Tls/Virtual*
//  devolvem algo plausivel; GetLastError=0; SetUnhandledExceptionFilter guarda/0.
// ============================================================================
__declspec(dllexport) void InitializeCriticalSection(void* cs) { (void)cs; }
__declspec(dllexport) void DeleteCriticalSection(void* cs)     { (void)cs; }
__declspec(dllexport) void EnterCriticalSection(void* cs)      { (void)cs; }
__declspec(dllexport) void LeaveCriticalSection(void* cs)      { (void)cs; }
__declspec(dllexport) unsigned GetLastError(void)             { return 0; }
__declspec(dllexport) void     SetLastError(unsigned e)       { (void)e; }
__declspec(dllexport) void*    SetUnhandledExceptionFilter(void* filter) { (void)filter; return 0; }
// __C_specific_handler: no Windows tambem e' exportado pelo kernel32 (alem do ucrtbase).
// Quando o .exe linka -lkernel32, o CRT o resolve AQUI. Sem SEH real -> ContinueSearch(1).
__declspec(dllexport) int __C_specific_handler(void* rec, void* frame, void* ctx, void* disp) {
    (void)rec; (void)frame; (void)ctx; (void)disp; return 1;
}
__declspec(dllexport) void     Sleep(unsigned ms)            { (void)ms; }
__declspec(dllexport) void*    TlsGetValue(unsigned idx)     { (void)idx; return 0; }
__declspec(dllexport) int      TlsSetValue(unsigned idx, void* v) { (void)idx; (void)v; return 1; }
// GetStartupInfoA: o startup GUI do CRT (WinMainCRTStartup) chama p/ obter nCmdShow.
// Zeramos a STARTUPINFOA (104 bytes no x64) -> dwFlags=0, entao o CRT cai em
// SW_SHOWDEFAULT (a janela aparece). cb fica no offset 0.
__declspec(dllexport) void GetStartupInfoA(void* si) {
    if (!si) return;
    char* p = (char*)si;
    for (int i = 0; i < 104; i++) p[i] = 0;
    *(unsigned*)si = 104;   // cb
}
__declspec(dllexport) int VirtualProtect(void* addr, unsigned long long size,
                                         unsigned newprot, unsigned* oldprot) {
    (void)addr; (void)size; (void)newprot;
    if (oldprot) *oldprot = 0x40;   // PAGE_EXECUTE_READWRITE
    return 1;                        // TRUE (paginas de usuario ja sao RWX aqui)
}
// MEMORY_BASIC_INFORMATION (x64): BaseAddress, AllocationBase, AllocationProtect,
// __alignment, RegionSize, State, Protect, Type. Devolve algo committed+RWX.
typedef struct _MEUOS_MBI {
    void*              BaseAddress;
    void*              AllocationBase;
    unsigned           AllocationProtect;
    unsigned           __align0;
    unsigned long long RegionSize;
    unsigned           State;
    unsigned           Protect;
    unsigned           Type;
    unsigned           __align1;
} MEUOS_MBI;
__declspec(dllexport) unsigned long long VirtualQuery(void* addr, MEUOS_MBI* mbi,
                                                      unsigned long long len) {
    if (!mbi || len < sizeof(MEUOS_MBI)) return 0;
    mbi->BaseAddress       = (void*)((unsigned long long)addr & ~0xFFFULL);
    mbi->AllocationBase    = mbi->BaseAddress;
    mbi->AllocationProtect = 0x40; mbi->__align0 = 0;
    mbi->RegionSize        = 0x1000;
    mbi->State             = 0x1000;   // MEM_COMMIT
    mbi->Protect           = 0x40;     // PAGE_EXECUTE_READWRITE
    mbi->Type              = 0x20000;  // MEM_PRIVATE
    mbi->__align1          = 0;
    return sizeof(MEUOS_MBI);
}

// ===========================================================================
// kernelbase (via api-ms-win-core-* -> kernel32): 1o lote p/ o explorer.exe REAL.
// Funcoes PURAS DE USUARIO, implementadas DE VERDADE (sem syscall): strings largas,
// Str*/Path* (shlwapi via core), comparacao ordinal, MulDiv, conversao MB<->WC. Os
// SRW locks sao corretos p/ o modelo single-threaded atual (sem contencao), igual as
// critical sections ja existentes acima. Backadas por kernel (heap/VirtualAlloc/
// registro/threads) vem em lotes seguintes. Ver RECON-EXPLORER.md.
// ===========================================================================
typedef unsigned short K32_WCHAR;
static K32_WCHAR k32_wlower(K32_WCHAR c) { return (c >= 'A' && c <= 'Z') ? (K32_WCHAR)(c + 32) : c; }

__declspec(dllexport) int lstrlenW(const K32_WCHAR* s) { int n = 0; if (s) while (s[n]) n++; return n; }
__declspec(dllexport) int lstrcmpW(const K32_WCHAR* a, const K32_WCHAR* b) {
    while (*a && (*a == *b)) { a++; b++; } return (int)*a - (int)*b;
}
__declspec(dllexport) int lstrcmpiW(const K32_WCHAR* a, const K32_WCHAR* b) {
    while (*a && (k32_wlower(*a) == k32_wlower(*b))) { a++; b++; }
    return (int)k32_wlower(*a) - (int)k32_wlower(*b);
}

__declspec(dllexport) K32_WCHAR* CharNextW(const K32_WCHAR* s) { return (K32_WCHAR*)(s && *s ? s + 1 : s); }
__declspec(dllexport) char*      CharNextA(const char* s)      { return (char*)(s && *s ? s + 1 : s); }
__declspec(dllexport) K32_WCHAR* CharLowerBuffW(K32_WCHAR* s, unsigned len) {
    if (s) for (unsigned i = 0; i < len; i++) s[i] = k32_wlower(s[i]);
    return s;
}

// Str* (shlwapi-legacy; chegam via api-ms-win-core-shlwapi-*)
__declspec(dllexport) K32_WCHAR* StrChrW(const K32_WCHAR* s, K32_WCHAR c) {
    if (s) for (; *s; s++) if (*s == c) return (K32_WCHAR*)s; return 0;
}
__declspec(dllexport) K32_WCHAR* StrChrIW(const K32_WCHAR* s, K32_WCHAR c) {
    c = k32_wlower(c); if (s) for (; *s; s++) if (k32_wlower(*s) == c) return (K32_WCHAR*)s; return 0;
}
__declspec(dllexport) K32_WCHAR* StrRChrW(const K32_WCHAR* s, const K32_WCHAR* end, K32_WCHAR c) {
    const K32_WCHAR* r = 0; if (!s) return 0; if (!end) { end = s; while (*end) end++; }
    for (; s < end; s++) if (*s == c) r = s; return (K32_WCHAR*)r;
}
__declspec(dllexport) K32_WCHAR* StrStrIW(const K32_WCHAR* h, const K32_WCHAR* n) {
    if (!h || !n || !*n) return (K32_WCHAR*)h;
    for (; *h; h++) { const K32_WCHAR* a = h; const K32_WCHAR* b = n;
        while (*a && *b && k32_wlower(*a) == k32_wlower(*b)) { a++; b++; }
        if (!*b) return (K32_WCHAR*)h; }
    return 0;
}
__declspec(dllexport) int StrCmpW(const K32_WCHAR* a, const K32_WCHAR* b)  { return lstrcmpW(a, b); }
__declspec(dllexport) int StrCmpIW(const K32_WCHAR* a, const K32_WCHAR* b) { return lstrcmpiW(a, b); }
__declspec(dllexport) int StrCmpNIW(const K32_WCHAR* a, const K32_WCHAR* b, int n) {
    for (; n > 0; n--, a++, b++) { K32_WCHAR ca = k32_wlower(*a), cb = k32_wlower(*b);
        if (ca != cb) return (int)ca - (int)cb; if (!ca) break; }
    return 0;
}
__declspec(dllexport) int StrToIntW(const K32_WCHAR* s) {
    int sign = 1, v = 0; if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (int)(*s - '0'); s++; }
    return v * sign;
}

// Path* (via api-ms-win-core-shlwapi/path)
__declspec(dllexport) K32_WCHAR* PathFindFileNameW(const K32_WCHAR* p) {
    const K32_WCHAR* r = p; if (!p) return 0;
    for (; *p; p++) if (*p == '\\' || *p == '/') r = p + 1;
    return (K32_WCHAR*)r;
}
__declspec(dllexport) K32_WCHAR* PathFindExtensionW(const K32_WCHAR* p) {
    const K32_WCHAR* dot = 0; const K32_WCHAR* q = p; if (!p) return 0;
    for (; *q; q++) { if (*q == '\\' || *q == '/') dot = 0; else if (*q == '.') dot = q; }
    return (K32_WCHAR*)(dot ? dot : q);
}

// CompareStringOrdinal -> CSTR_LESS_THAN(1)/EQUAL(2)/GREATER_THAN(3)
__declspec(dllexport) int CompareStringOrdinal(const K32_WCHAR* a, int la, const K32_WCHAR* b, int lb, int ignoreCase) {
    if (la < 0) la = lstrlenW(a); if (lb < 0) lb = lstrlenW(b);
    int n = la < lb ? la : lb;
    for (int i = 0; i < n; i++) { K32_WCHAR ca = a[i], cb = b[i];
        if (ignoreCase) { ca = k32_wlower(ca); cb = k32_wlower(cb); }
        if (ca != cb) return ca < cb ? 1 : 3; }
    return la == lb ? 2 : (la < lb ? 1 : 3);
}

__declspec(dllexport) int MulDiv(int a, int b, int c) {
    if (c == 0) return -1;
    long long r = (long long)a * (long long)b;
    r += (r >= 0 ? c / 2 : -(c / 2));   // arredonda p/ o mais proximo
    return (int)(r / c);
}

// Conversao MB<->WC (por ora: ASCII/Latin-1, byte<->WCHAR; sem tabelas de codepage).
// cbMultiByte/cchWideChar == -1 => string terminada em NUL. Tamanho de saida 0 =>
// devolve o tamanho necessario (contrato do Win32).
__declspec(dllexport) int MultiByteToWideChar(unsigned cp, unsigned flags, const char* mb,
        int cbMB, K32_WCHAR* wc, int cchWC) {
    (void)cp; (void)flags; if (!mb) return 0;
    int n = cbMB; if (n < 0) { n = 0; while (mb[n]) n++; n++; }   // inclui o NUL
    if (cchWC == 0) return n;
    int w = n < cchWC ? n : cchWC;
    for (int i = 0; i < w; i++) wc[i] = (K32_WCHAR)(unsigned char)mb[i];
    return w;
}
__declspec(dllexport) int WideCharToMultiByte(unsigned cp, unsigned flags, const K32_WCHAR* wc,
        int cchWC, char* mb, int cbMB, const char* defChar, int* usedDef) {
    (void)cp; (void)flags; (void)defChar; if (usedDef) *usedDef = 0; if (!wc) return 0;
    int n = cchWC; if (n < 0) { n = 0; while (wc[n]) n++; n++; }   // inclui o NUL
    if (cbMB == 0) return n;
    int w = n < cbMB ? n : cbMB;
    for (int i = 0; i < w; i++) mb[i] = (wc[i] < 256) ? (char)wc[i] : '?';
    return w;
}

// Critical sections (complementos): as basicas (Init/Enter/Leave/Delete) ja existem
// acima como no-op (correto sem contencao, single-threaded).
__declspec(dllexport) int  InitializeCriticalSectionAndSpinCount(void* cs, unsigned spin) { (void)cs; (void)spin; return 1; }
__declspec(dllexport) int  InitializeCriticalSectionEx(void* cs, unsigned spin, unsigned flags) { (void)cs; (void)spin; (void)flags; return 1; }
__declspec(dllexport) int  TryEnterCriticalSection(void* cs) { (void)cs; return 1; }

// SRW locks: no modelo single-threaded atual nao ha contencao, entao a aquisicao e'
// sempre imediata (mesma logica das critical sections). Vira real quando o escalonador
// de ring-3 estiver ligado (thread por thread).
__declspec(dllexport) void InitializeSRWLock(void* l)          { (void)l; }
__declspec(dllexport) void AcquireSRWLockExclusive(void* l)    { (void)l; }
__declspec(dllexport) void ReleaseSRWLockExclusive(void* l)    { (void)l; }
__declspec(dllexport) void AcquireSRWLockShared(void* l)       { (void)l; }
__declspec(dllexport) void ReleaseSRWLockShared(void* l)       { (void)l; }
__declspec(dllexport) int  TryAcquireSRWLockExclusive(void* l) { (void)l; return 1; }

// Condition variables — mesma logica single-threaded (sem contencao, sem outra thread p/
// sinalizar). Initialize/Wake sao no-ops; o Sleep "acorda" imediatamente (TRUE) p/ o
// chamador reavaliar o predicado, sem bloquear (bloquear travaria — nao ha quem sinalize).
// Vira real com o escalonador de ring-3 (thread por thread).
__declspec(dllexport) void InitializeConditionVariable(void* cv)                          { (void)cv; }
__declspec(dllexport) void WakeConditionVariable(void* cv)                                { (void)cv; }
__declspec(dllexport) void WakeAllConditionVariable(void* cv)                             { (void)cv; }
__declspec(dllexport) int  SleepConditionVariableCS(void* cv, void* cs, unsigned ms)      { (void)cv;(void)cs;(void)ms; return 1; }
__declspec(dllexport) int  SleepConditionVariableSRW(void* cv, void* l, unsigned ms, unsigned fl) { (void)cv;(void)l;(void)ms;(void)fl; return 1; }

// ===========================================================================
// kernelbase lote 2: TEMPO + IDs + perf counter. O CRT do explorer usa isto no
// __security_init_cookie (o 1o ponto onde ele parava). Implementado DE VERDADE:
// tempo via syscall novo (kernel le o KUSER_SHARED_DATA), PID via o syscall de
// processo que ja existe (EPROCESS real), perf counter via RDTSC (instrucao de
// usuario; CR4.TSD=0). Ver RECON-EXPLORER.md.
// ===========================================================================
typedef struct { unsigned dwLowDateTime, dwHighDateTime; } K32_FILETIME;
struct k32_timeinfo { long long SystemTime; unsigned TickCount; };
// subset do PROCESS_BASIC_INFORMATION (offsets casam com o kernel): UniqueProcessId @ 0x20.
struct k32_pbi { unsigned ExitStatus, Reserved0;
                 unsigned long long PebBaseAddress, AffinityMask, BasePriority,
                                    UniqueProcessId, InheritedFrom; };

__declspec(dllexport) void GetSystemTimeAsFileTime(K32_FILETIME* ft) {
    struct k32_timeinfo ti; ti.SystemTime = 0; ti.TickCount = 0;
    NtQuerySystemTime(&ti);
    if (ft) { ft->dwLowDateTime = (unsigned)ti.SystemTime;
              ft->dwHighDateTime = (unsigned)((unsigned long long)ti.SystemTime >> 32); }
}
__declspec(dllexport) void GetSystemTimePreciseAsFileTime(K32_FILETIME* ft) { GetSystemTimeAsFileTime(ft); }
__declspec(dllexport) unsigned GetTickCount(void) {
    struct k32_timeinfo ti; ti.SystemTime = 0; ti.TickCount = 0; NtQuerySystemTime(&ti); return ti.TickCount;
}
__declspec(dllexport) unsigned long long GetTickCount64(void) {
    struct k32_timeinfo ti; ti.SystemTime = 0; ti.TickCount = 0; NtQuerySystemTime(&ti);
    return (unsigned long long)ti.TickCount;
}
__declspec(dllexport) unsigned GetCurrentProcessId(void) {
    struct k32_pbi pbi; for (unsigned i = 0; i < sizeof(pbi); i++) ((char*)&pbi)[i] = 0;
    NtQueryInformationProcess(0, 0 /*ProcessBasicInformation*/, &pbi, (unsigned)sizeof(pbi), 0);
    return (unsigned)pbi.UniqueProcessId;
}
__declspec(dllexport) unsigned GetCurrentThreadId(void) {
    // Modelo single-threaded (sem escalonador de ring-3 ligado): id distinto do PID
    // so p/ dar entropia ao cookie. Vira real quando houver threads de usuario.
    return GetCurrentProcessId() ^ 0x1000u;
}
__declspec(dllexport) void* GetCurrentProcess(void) { return (void*)(long long)-1; }  // pseudo-handle NT
__declspec(dllexport) void* GetCurrentThread(void)  { return (void*)(long long)-2; }  // pseudo-handle NT
__declspec(dllexport) int QueryPerformanceCounter(long long* c) {
    unsigned lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    if (c) *c = ((long long)hi << 32) | lo;
    return 1;
}
__declspec(dllexport) int QueryPerformanceFrequency(long long* f) {
    if (f) *f = 3000000000LL;   // TSC ~3 GHz (i7-9700K); consistente com QPC=rdtsc
    return 1;
}

// --- Entry-point / SEH setup: o __scrt_common_main_seh do explorer chama ISTO
//     primeiro (ANTES do cookie). Descoberto desmontando o entry real. ---
__declspec(dllexport) int IsDebuggerPresent(void) { return 0; }   // nao estamos sob depurador
__declspec(dllexport) int IsProcessorFeaturePresent(unsigned f) {
    // PF_XMMI_INSTRUCTIONS_AVAILABLE(6)/PF_XMMI64(10) = SSE/SSE2: temos (long mode c/
    // SSE ligado). PF_NX(12): temos. Demais features: conservador (0).
    return (f == 6 || f == 10 || f == 12) ? 1 : 0;
}
__declspec(dllexport) long UnhandledExceptionFilter(void* info) { (void)info; return 0; } // EXCEPTION_CONTINUE_SEARCH
__declspec(dllexport) int TerminateProcess(void* hproc, unsigned code) {
    (void)hproc; NtTerminateProcess(code); return 1;   // (so o processo corrente por ora)
}

// --- SList (api-ms-win-core-interlocked): o CRT do explorer inicializa um SList head
//     na startup. Impl correta p/ o modelo single-threaded (head@[0], depth@[8]). ---
__declspec(dllexport) void InitializeSListHead(void* h) {
    if (h) { ((void**)h)[0] = 0; ((unsigned long long*)h)[1] = 0; }
}
__declspec(dllexport) void* InterlockedPushEntrySList(void* h, void* entry) {
    if (!h || !entry) return 0;
    void* old = ((void**)h)[0]; ((void**)entry)[0] = old; ((void**)h)[0] = entry;
    ((unsigned long long*)h)[1]++; return old;
}
__declspec(dllexport) void* InterlockedPopEntrySList(void* h) {
    if (!h) return 0; void* top = ((void**)h)[0];
    if (top) { ((void**)h)[0] = ((void**)top)[0]; ((unsigned long long*)h)[1]--; } return top;
}
__declspec(dllexport) void* InterlockedFlushSList(void* h) {
    if (!h) return 0; void* top = ((void**)h)[0];
    ((void**)h)[0] = 0; ((unsigned long long*)h)[1] = 0; return top;
}
__declspec(dllexport) unsigned short QueryDepthSList(void* h) {
    return h ? (unsigned short)((unsigned long long*)h)[1] : 0;
}

// --- Startup / modulo / cmdline (o CRT do explorer usa na init, logo apos o SList) ---
__declspec(dllexport) void* GetModuleHandleW(const K32_WCHAR* name) {
    if (!name) {   // NULL = o proprio .exe: PEB->ImageBaseAddress (gs:[0x60] -> PEB -> +0x10)
        void* peb; __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(peb));
        return *(void**)((char*)peb + 0x10);
    }
    char n[260]; int i = 0; while (name[i] && i < 259) { n[i] = (char)name[i]; i++; } n[i] = 0;
    return GetModuleHandleA(n);
}
__declspec(dllexport) int GetModuleHandleExW(unsigned flags, const K32_WCHAR* name, void** out) {
    (void)flags; if (out) *out = GetModuleHandleW(name); return out ? 1 : 0;
}
static K32_WCHAR g_cmdlineW[] = { 'e','x','p','l','o','r','e','r','.','e','x','e', 0 };
__declspec(dllexport) K32_WCHAR* GetCommandLineW(void) { return g_cmdlineW; }
__declspec(dllexport) char*      GetCommandLineA(void) { static char c[] = "explorer.exe"; return c; }
__declspec(dllexport) void GetStartupInfoW(void* si) {
    if (!si) return; char* p = (char*)si; for (int i = 0; i < 104; i++) p[i] = 0;
    *(unsigned*)p = 104;   // cb = sizeof(STARTUPINFOW)
}

// --- Objetos de sincronizacao (api-ms-win-core-synch). Ainda nao ha objetos de kernel
//     expostos ao usermode; no modelo single-threaded (sem threads de ring-3) NAO ha
//     contencao, entao handles pseudo (nao-nulos, na faixa alta) + waits imediatos sao
//     CORRETOS p/ a init do explorer. Viram objetos reais qdo houver threads/eventos. ---
static void* k32_pseudo(void) { static unsigned long long h = 0x40001000ULL; h += 0x10; return (void*)h; }
__declspec(dllexport) void* CreateEventW(void* a, int mr, int is, const K32_WCHAR* n)  { (void)a;(void)mr;(void)is;(void)n; return k32_pseudo(); }
__declspec(dllexport) void* CreateEventExW(void* a, const K32_WCHAR* n, unsigned f, unsigned acc) { (void)a;(void)n;(void)f;(void)acc; return k32_pseudo(); }
__declspec(dllexport) void* OpenEventW(unsigned acc, int inh, const K32_WCHAR* n) { (void)acc;(void)inh;(void)n; return k32_pseudo(); }
__declspec(dllexport) int   SetEvent(void* h)   { (void)h; return 1; }
__declspec(dllexport) int   ResetEvent(void* h) { (void)h; return 1; }
__declspec(dllexport) void* CreateMutexW(void* a, int owner, const K32_WCHAR* n) { (void)a;(void)owner;(void)n; return k32_pseudo(); }
__declspec(dllexport) void* CreateMutexExW(void* a, const K32_WCHAR* n, unsigned f, unsigned acc) { (void)a;(void)n;(void)f;(void)acc; return k32_pseudo(); }
__declspec(dllexport) int   ReleaseMutex(void* h) { (void)h; return 1; }
__declspec(dllexport) void* CreateSemaphoreExW(void* a, long ini, long mx, const K32_WCHAR* n, unsigned f, unsigned acc) { (void)a;(void)ini;(void)mx;(void)n;(void)f;(void)acc; return k32_pseudo(); }
__declspec(dllexport) int   ReleaseSemaphore(void* h, long c, long* prev) { (void)h;(void)c; if (prev) *prev = 0; return 1; }
__declspec(dllexport) unsigned WaitForSingleObjectEx(void* h, unsigned ms, int al) { (void)h;(void)ms;(void)al; return 0; } // WAIT_OBJECT_0
__declspec(dllexport) unsigned WaitForMultipleObjectsEx(unsigned n, void* h, int all, unsigned ms, int al) { (void)n;(void)h;(void)all;(void)ms;(void)al; return 0; }

// --- VirtualAlloc + HEAP real (api-ms-win-core-memory/heap). Backing por VirtualAlloc
//     (syscall -> frames contiguos no kernel). O heap e' bump com header de tamanho por
//     bloco; HeapFree e' no-op por ora (free-list real depois). O explorer aloca muito
//     na init do shell -> precisa de heap de verdade, nao o bump de 128 KiB do ucrtbase. ---
__declspec(dllexport) void* VirtualAlloc(void* addr, unsigned long long size, unsigned type, unsigned prot) {
    (void)addr; (void)type; (void)prot; return NtVirtualAlloc(size);
}
__declspec(dllexport) int VirtualFree(void* addr, unsigned long long size, unsigned type) { (void)addr;(void)size;(void)type; return 1; }

static unsigned char*      k32_heap_cur  = 0;
static unsigned long long  k32_heap_left = 0;
__declspec(dllexport) void* GetProcessHeap(void) { return (void*)(long long)0x00420000; }  // handle sentinela
__declspec(dllexport) void* HeapAlloc(void* heap, unsigned flags, unsigned long long size) {
    (void)heap;
    unsigned long long need = (size + 16 + 15) & ~15ULL;               // +16 header, alinhado a 16
    if (need > k32_heap_left) {
        unsigned long long chunk = need > 0x1000000ULL ? need : 0x1000000ULL;  // 16 MiB ou o pedido
        k32_heap_cur  = (unsigned char*)NtVirtualAlloc(chunk);
        k32_heap_left = k32_heap_cur ? chunk : 0;
        if (!k32_heap_cur) return 0;
    }
    unsigned char* base = k32_heap_cur; k32_heap_cur += need; k32_heap_left -= need;
    *(unsigned long long*)base = size;                                 // header: tamanho pedido
    void* p = base + 16;
    if (flags & 0x8u) { for (unsigned long long i = 0; i < size; i++) ((unsigned char*)p)[i] = 0; } // HEAP_ZERO_MEMORY
    return p;
}
__declspec(dllexport) int HeapFree(void* heap, unsigned flags, void* p) { (void)heap;(void)flags;(void)p; return 1; } // bump: no-op
__declspec(dllexport) unsigned long long HeapSize(void* heap, unsigned flags, const void* p) {
    (void)heap;(void)flags; return p ? *(const unsigned long long*)((const unsigned char*)p - 16) : 0;
}
__declspec(dllexport) void* HeapReAlloc(void* heap, unsigned flags, void* p, unsigned long long size) {
    if (!p) return HeapAlloc(heap, flags, size);
    unsigned long long old = *(unsigned long long*)((unsigned char*)p - 16);
    void* np = HeapAlloc(heap, flags & ~0x8u, size);
    if (np) { unsigned long long c = old < size ? old : size; for (unsigned long long i = 0; i < c; i++) ((unsigned char*)np)[i] = ((unsigned char*)p)[i]; }
    return np;
}
__declspec(dllexport) void* HeapCreate(unsigned opts, unsigned long long init, unsigned long long mx) { (void)opts;(void)init;(void)mx; return (void*)(long long)0x00430000; }
__declspec(dllexport) int HeapDestroy(void* h) { (void)h; return 1; }
__declspec(dllexport) int HeapValidate(void* h, unsigned f, const void* p) { (void)h;(void)f;(void)p; return 1; }
__declspec(dllexport) int HeapSetInformation(void* h, int cls, void* info, unsigned long long len) { (void)h;(void)cls;(void)info;(void)len; return 1; }

// --- Versao do SO + info do sistema (api-ms-win-core-sysinfo). O explorer checa se
//     e' Windows 10 (major=10). Reportamos Win10 build 19045 (o real que rodamos). ---
__declspec(dllexport) int GetVersionExW(void* info) {
    if (!info) return 0;
    unsigned char* c = (unsigned char*)info; unsigned* p = (unsigned*)info; // p[0]=size (do caller)
    p[1] = 10; p[2] = 0; p[3] = 19045; p[4] = 2;    // Major=10 Minor=0 Build=19045 Platform=NT
    for (int i = 0x14; i < 0x114; i++) c[i] = 0;    // szCSDVersion[128] (wide) = ""
    if (p[0] >= 0x11C) {                            // OSVERSIONINFOEXW
        for (int i = 0x114; i < 0x11C; i++) c[i] = 0;
        c[0x11A] = 1;                               // wProductType = VER_NT_WORKSTATION
    }
    return 1;
}
__declspec(dllexport) int GetVersionExA(void* info) {
    if (!info) return 0;
    unsigned char* c = (unsigned char*)info; unsigned* p = (unsigned*)info;
    p[1] = 10; p[2] = 0; p[3] = 19045; p[4] = 2;
    for (int i = 0x14; i < 0x94; i++) c[i] = 0;     // szCSDVersion[128] (narrow) = ""
    return 1;
}
__declspec(dllexport) unsigned GetVersion(void) { return 0x0000000A; }   // major 10, minor 0, NT
__declspec(dllexport) void GetSystemInfo(void* si) {
    if (!si) return; unsigned char* c = (unsigned char*)si; for (int i = 0; i < 0x30; i++) c[i] = 0;
    *(unsigned short*)(c + 0)      = 9;        // wProcessorArchitecture = AMD64
    *(unsigned*)(c + 4)            = 4096;     // dwPageSize
    *(unsigned long long*)(c+0x18) = 1;        // dwActiveProcessorMask
    *(unsigned*)(c + 0x20)        = 2;         // dwNumberOfProcessors
    *(unsigned*)(c + 0x28)        = 0x10000;   // dwAllocationGranularity
}
__declspec(dllexport) void GetNativeSystemInfo(void* si) { GetSystemInfo(si); }

// ============================================================================
// Frente C (explorer real) — kernel32 lote A: threadpool + InitOnce + processo/thread
// + excecao + debug + tempo. O explorer importa ~248 funcoes (via api-ms-win-core-*);
// implementamos por fases. Impl reais onde da (threadpool Work sincrono, InitOnce);
// stubs ESPECIFICOS e nomeados onde a semantica exige subsistema ainda ausente
// (threads de ring-3 com param, criacao de processo). Sem catch-all.
// ============================================================================
__declspec(dllimport) long NtCreateThread(void* out_handle, void* process, void* start);
static long long g_fake_h = 0x2000;
static void* k32_fake_handle(void) { g_fake_h += 8; return (void*)g_fake_h; }

// ---- Threadpool: Work/Simple rodam SINCRONO no submit (sem escalonador de callbacks
//      assincronos em ring-3); Timer/Wait sao no-op (nunca disparam). ----
typedef struct { void* cb; void* ctx; } k32_tp_t;
static k32_tp_t* k32_tp_new(void* cb, void* ctx) { k32_tp_t* o = (k32_tp_t*)HeapAlloc(0, 0, sizeof(k32_tp_t)); if (o){ o->cb=cb; o->ctx=ctx; } return o; }
__declspec(dllexport) void* CreateThreadpoolWork(void* cb, void* ctx, void* env) { (void)env; return k32_tp_new(cb, ctx); }
__declspec(dllexport) void SubmitThreadpoolWork(void* work) { k32_tp_t* o=(k32_tp_t*)work; if (o && o->cb) ((void(*)(void*,void*,void*))o->cb)(0, o->ctx, work); }
__declspec(dllexport) void WaitForThreadpoolWorkCallbacks(void* work, int cancel) { (void)work;(void)cancel; }
__declspec(dllexport) void CloseThreadpoolWork(void* work) { (void)work; }
__declspec(dllexport) int TrySubmitThreadpoolCallback(void* cb, void* ctx, void* env) { (void)env; if (cb) ((void(*)(void*,void*))cb)(0, ctx); return 1; }
__declspec(dllexport) void* CreateThreadpoolTimer(void* cb, void* ctx, void* env) { (void)env; return k32_tp_new(cb, ctx); }
__declspec(dllexport) void SetThreadpoolTimer(void* timer, void* due, unsigned period, unsigned window) { (void)timer;(void)due;(void)period;(void)window; }
__declspec(dllexport) void WaitForThreadpoolTimerCallbacks(void* timer, int cancel) { (void)timer;(void)cancel; }
__declspec(dllexport) void CloseThreadpoolTimer(void* timer) { (void)timer; }
__declspec(dllexport) void* CreateThreadpoolWait(void* cb, void* ctx, void* env) { (void)env; return k32_tp_new(cb, ctx); }
__declspec(dllexport) void SetThreadpoolWait(void* wait, void* handle, void* timeout) { (void)wait;(void)handle;(void)timeout; }
__declspec(dllexport) void WaitForThreadpoolWaitCallbacks(void* wait, int cancel) { (void)wait;(void)cancel; }
__declspec(dllexport) void CloseThreadpoolWait(void* wait) { (void)wait; }

// ---- InitOnce: INIT_ONCE = { void* Ptr }. Estado nos 2 bits baixos (contexto alinhado a
//      4): 0=nao-iniciado, 2=concluido (Ptr&~3 = contexto guardado por Complete). Guardar
//      e devolver o CONTEXTO e' essencial — o caller (padrao begin/complete) usa o objeto
//      criado; devolver 0 fazia o explorer derefar NULL. Single-threaded. ----
#define K32_IO_DONE 2ULL
__declspec(dllexport) int InitOnceExecuteOnce(void** once, void* fn, void* param, void** ctx) {
    unsigned long long v = once ? (unsigned long long)*once : 0;
    if ((v & 3) == K32_IO_DONE) { if (ctx)*ctx=(void*)(v & ~3ULL); return 1; }
    void* lctx = 0; int ok = fn ? ((int(*)(void**,void*,void**))fn)(once, param, &lctx) : 1;
    if (ok && once) *once = (void*)(((unsigned long long)lctx & ~3ULL) | K32_IO_DONE);
    if (ctx)*ctx=lctx; return ok;
}
__declspec(dllexport) int InitOnceBeginInitialize(void** once, unsigned flags, int* pending, void** ctx) {
    (void)flags; unsigned long long v = once ? (unsigned long long)*once : 0;
    if ((v & 3) == K32_IO_DONE) { if (pending)*pending=0; if (ctx)*ctx=(void*)(v & ~3ULL); return 1; }
    if (pending)*pending=1; if (ctx)*ctx=0; return 1;   // caller inicializa e chama Complete
}
__declspec(dllexport) int InitOnceComplete(void** once, unsigned flags, void* ctx) {
    if (flags & 0x4u) { if (once)*once=0; return 1; }   // INIT_ONCE_INIT_FAILED: reabre
    if (once) *once=(void*)(((unsigned long long)ctx & ~3ULL) | K32_IO_DONE); return 1;
}

// ---- processo/thread: threads reais de ring-3 (com param/stack proprios) ainda nao
//      wired -> handles-sentinela. O thread principal do explorer segue; workers nao. ----
__declspec(dllexport) void* CreateThread(void* sec, unsigned long long stack, void* start, void* param, unsigned flags, unsigned* tid) {
    (void)sec;(void)stack;(void)start;(void)param;(void)flags; if (tid)*tid=(unsigned)(unsigned long long)k32_fake_handle(); return k32_fake_handle();
}
__declspec(dllexport) void* OpenProcess(unsigned acc, int inh, unsigned pid) { (void)acc;(void)inh;(void)pid; return k32_fake_handle(); }
__declspec(dllexport) void* OpenThread(unsigned acc, int inh, unsigned tid) { (void)acc;(void)inh;(void)tid; return k32_fake_handle(); }
__declspec(dllexport) unsigned GetProcessId(void* h) { (void)h; return 1; }
__declspec(dllexport) int ProcessIdToSessionId(unsigned pid, unsigned* sid) { (void)pid; if (sid)*sid=1; return 1; }
__declspec(dllexport) int GetExitCodeProcess(void* h, unsigned* code) { (void)h; if (code)*code=259; return 1; }   // STILL_ACTIVE
__declspec(dllexport) unsigned ResumeThread(void* h) { (void)h; return 0; }
__declspec(dllexport) int SetThreadPriority(void* h, int pri) { (void)h;(void)pri; return 1; }
__declspec(dllexport) int GetThreadPriority(void* h) { (void)h; return 0; }
__declspec(dllexport) int SetThreadPriorityBoost(void* h, int dis) { (void)h;(void)dis; return 1; }
__declspec(dllexport) long SetThreadDescription(void* h, const void* d) { (void)h;(void)d; return 0; }   // S_OK
__declspec(dllexport) unsigned GetThreadUILanguage(void) { return 0x0409; }
__declspec(dllexport) int QueueUserAPC(void* fn, void* thread, unsigned long long data) { (void)fn;(void)thread;(void)data; return 1; }
__declspec(dllexport) int CreateProcessW(const void* app, void* cmd, void* pa, void* ta, int inh, unsigned flags, void* env, const void* cwd, void* si, void* pi) {
    (void)app;(void)cmd;(void)pa;(void)ta;(void)inh;(void)flags;(void)env;(void)cwd;(void)si;(void)pi; return 0;   // sem spawn (falha graciosa)
}
__declspec(dllexport) int QueryFullProcessImageNameW(void* h, unsigned flags, void* buf, unsigned* size) { (void)h;(void)flags;(void)buf; if (size)*size=0; return 0; }
__declspec(dllexport) int GetProcessMitigationPolicy(void* h, int pol, void* buf, unsigned long long len) { (void)h;(void)pol; if (buf){ unsigned char* b=(unsigned char*)buf; for (unsigned long long i=0;i<len;i++) b[i]=0; } return 1; }
__declspec(dllexport) int SetProcessInformation(void* h, int cls, void* info, unsigned long len) { (void)h;(void)cls;(void)info;(void)len; return 1; }
__declspec(dllexport) int SetPriorityClass(void* h, unsigned cls) { (void)h;(void)cls; return 1; }
__declspec(dllexport) unsigned GetPriorityClass(void* h) { (void)h; return 0x20; }   // NORMAL_PRIORITY_CLASS

// ---- excecao (sem SEH ring-3 ativo): RaiseException nao propaga; FailFast encerra. ----
__declspec(dllexport) void RaiseException(unsigned code, unsigned flags, unsigned n, const void* args) { (void)code;(void)flags;(void)n;(void)args; }
__declspec(dllexport) void RaiseFailFastException(void* rec, void* ctx, unsigned flags) { (void)rec;(void)ctx;(void)flags; ExitProcess(0xC0000409u); }
__declspec(dllexport) unsigned RemoveVectoredExceptionHandler(void* h) { (void)h; return 1; }

// ---- debug ----
__declspec(dllexport) void OutputDebugStringW(const void* s) { (void)s; }
__declspec(dllexport) void DebugBreak(void) { }
__declspec(dllexport) void* DelayLoadFailureHook(const char* dll, const char* fn) { (void)dll;(void)fn; return 0; }
__declspec(dllexport) unsigned SetErrorMode(unsigned mode) { (void)mode; return 0; }

// ---- tempo: data/hora FIXA plausivel (sem RTC ring-3 aqui); conversoes basicas. ----
typedef struct { unsigned short wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds; } SYSTEMTIME_;
__declspec(dllexport) void GetSystemTime(void* st) { if (!st) return; SYSTEMTIME_* s=(SYSTEMTIME_*)st;
    s->wYear=2026; s->wMonth=7; s->wDayOfWeek=2; s->wDay=7; s->wHour=12; s->wMinute=0; s->wSecond=0; s->wMilliseconds=0; }
__declspec(dllexport) void GetLocalTime(void* st) { GetSystemTime(st); }
__declspec(dllexport) int FileTimeToSystemTime(const void* ft, void* st) { (void)ft; GetSystemTime(st); return 1; }
__declspec(dllexport) int SystemTimeToFileTime(const void* st, void* ft) { (void)st; if (ft) *(unsigned long long*)ft = 0x01DCADE6C0000000ULL; return 1; }
__declspec(dllexport) int SystemTimeToTzSpecificLocalTime(const void* tz, const void* st, void* local) {
    (void)tz; if (st && local) { const unsigned char* a=(const unsigned char*)st; unsigned char* b=(unsigned char*)local; for (int i=0;i<16;i++) b[i]=a[i]; } return 1;
}
__declspec(dllexport) long CompareFileTime(const void* a, const void* b) {
    unsigned long long x = a?*(const unsigned long long*)a:0, y = b?*(const unsigned long long*)b:0;
    return x<y ? -1 : (x>y ? 1 : 0);
}

// ============================================================================
// Frente C (explorer real) — kernel32 lote B: synch/modulo/arquivo/NLS/path/recurso/
// mapeamento/global-heap/job/misc. Impl reais onde barato (Global/LocalAlloc no heap,
// path/string, GetModuleFileName, CompareStringW); stubs ESPECIFICOS onde o subsistema
// falta (arquivo/recurso -> "nao encontrado"; mapeamento -> RAM anonima). Sem catch-all.
// ============================================================================
__declspec(dllimport) void* LdrLoadDll(const char* name);
typedef unsigned short wchar16;   // UTF-16 (wide) do Win32
static unsigned k32_wlen(const wchar16* s){ unsigned n=0; if(s) while(s[n]) n++; return n; }
static void k32_asctow(wchar16* d, const char* s, int cap){ int i=0; if(!d||cap<=0) return; for(; s[i] && i<cap-1; i++) d[i]=(wchar16)(unsigned char)s[i]; d[i]=0; }
static void k32_wtoasc(char* d, const wchar16* s, int cap){ int i=0; if(!d||cap<=0) return; for(; s && s[i] && i<cap-1; i++) d[i]=(char)s[i]; d[i]=0; }
typedef struct { unsigned long long size; } k32_map_t;
static void* k32_mapnew(unsigned long long size){ k32_map_t* m=(k32_map_t*)HeapAlloc(0,0,sizeof(k32_map_t)); if(m) m->size=size?size:0x1000; return m; }

// ---- synch: objetos por handle-sentinela; SleepEx dorme; waits de pool no-op ----
__declspec(dllexport) void* OpenSemaphoreW(unsigned acc, int inh, const wchar16* n) { (void)acc;(void)inh;(void)n; return k32_fake_handle(); }
__declspec(dllexport) void* OpenMutexW(unsigned acc, int inh, const wchar16* n) { (void)acc;(void)inh;(void)n; return k32_fake_handle(); }
__declspec(dllexport) unsigned SleepEx(unsigned ms, int alertable) { (void)alertable; Sleep(ms); return 0; }
__declspec(dllexport) int RegisterWaitForSingleObject(void** wait, void* obj, void* cb, void* ctx, unsigned ms, unsigned flags) { (void)obj;(void)cb;(void)ctx;(void)ms;(void)flags; if (wait)*wait=k32_fake_handle(); return 1; }
__declspec(dllexport) int UnregisterWaitEx(void* wait, void* ev) { (void)wait;(void)ev; return 1; }
__declspec(dllexport) void* CreateTimerQueueTimer(void** timer, void* queue, void* cb, void* param, unsigned due, unsigned period, unsigned flags) { (void)queue;(void)cb;(void)param;(void)due;(void)period;(void)flags; if (timer)*timer=k32_fake_handle(); return (void*)(long long)1; }
__declspec(dllexport) int ChangeTimerQueueTimer(void* queue, void* timer, unsigned due, unsigned period) { (void)queue;(void)timer;(void)due;(void)period; return 1; }
__declspec(dllexport) int DeleteTimerQueueTimer(void* queue, void* timer, void* ev) { (void)queue;(void)timer;(void)ev; return 1; }

// ---- modulo ----
__declspec(dllexport) int FreeLibrary(void* h) { (void)h; return 1; }
__declspec(dllexport) void* LoadLibraryExW(const wchar16* name, void* file, unsigned flags) {
    (void)file;(void)flags; char nb[128]; k32_wtoasc(nb, name, sizeof(nb)); return LdrLoadDll(nb);
}
__declspec(dllexport) unsigned GetModuleFileNameW(void* mod, wchar16* buf, unsigned size) {
    (void)mod; k32_asctow(buf, "C:\\Windows\\explorer.exe", (int)size); return k32_wlen(buf);
}
__declspec(dllexport) unsigned GetModuleFileNameA(void* mod, char* buf, unsigned size) {
    (void)mod; const char* s="C:\\Windows\\explorer.exe"; unsigned i=0; if(buf) for(; s[i] && i<size-1; i++) buf[i]=s[i]; if(buf&&size) buf[i]=0; return i;
}
__declspec(dllexport) void* ResolveDelayLoadedAPI(void* pa, const void* dd, void* dh, void* fh, void* thunk, unsigned flags) { (void)pa;(void)dd;(void)dh;(void)fh;(void)thunk;(void)flags; return 0; }

// ---- arquivo: sem volume montado p/ o explorer -> "nao encontrado"/falha graciosa ----
#define K32_INVALID_FILE_ATTR 0xFFFFFFFFu
__declspec(dllexport) void* CreateFileW(const wchar16* n, unsigned a, unsigned s, void* sec, unsigned d, unsigned f, void* t) { (void)n;(void)a;(void)s;(void)sec;(void)d;(void)f;(void)t; return INVALID_HANDLE_VALUE; }
__declspec(dllexport) int DeleteFileW(const wchar16* n) { (void)n; return 0; }
__declspec(dllexport) int CopyFileW(const wchar16* a, const wchar16* b, int fail) { (void)a;(void)b;(void)fail; return 0; }
__declspec(dllexport) unsigned GetFileAttributesW(const wchar16* n) { (void)n; return K32_INVALID_FILE_ATTR; }
__declspec(dllexport) int GetFileAttributesExW(const wchar16* n, int lvl, void* info) { (void)n;(void)lvl;(void)info; return 0; }
__declspec(dllexport) int GetFileInformationByHandleEx(void* h, int cls, void* buf, unsigned len) { (void)h;(void)cls;(void)buf;(void)len; return 0; }
__declspec(dllexport) void* FindFirstFileW(const wchar16* n, void* data) { (void)n;(void)data; return INVALID_HANDLE_VALUE; }
__declspec(dllexport) int FindNextFileW(void* h, void* data) { (void)h;(void)data; return 0; }
__declspec(dllexport) int FindClose(void* h) { (void)h; return 1; }
__declspec(dllexport) unsigned SearchPathW(const wchar16* path, const wchar16* file, const wchar16* ext, unsigned len, wchar16* buf, wchar16** part) { (void)path;(void)file;(void)ext;(void)len;(void)buf;(void)part; return 0; }
__declspec(dllexport) unsigned GetTempPathW(unsigned len, wchar16* buf) { k32_asctow(buf, "C:\\Windows\\Temp\\", (int)len); return k32_wlen(buf); }
__declspec(dllexport) unsigned GetCurrentDirectoryW(unsigned len, wchar16* buf) { k32_asctow(buf, "C:\\Windows", (int)len); return k32_wlen(buf); }
__declspec(dllexport) unsigned GetSystemDirectoryW(wchar16* buf, unsigned len) { k32_asctow(buf, "C:\\Windows\\System32", (int)len); return k32_wlen(buf); }
__declspec(dllexport) unsigned GetWindowsDirectoryW(wchar16* buf, unsigned len) { k32_asctow(buf, "C:\\Windows", (int)len); return k32_wlen(buf); }
__declspec(dllexport) unsigned GetLongPathNameW(const wchar16* s, wchar16* buf, unsigned len) { unsigned i=0; if(buf&&s) for(; s[i] && i<len-1; i++) buf[i]=s[i]; if(buf&&len) buf[i]=0; return i; }

// ---- NLS/locale: defaults en-US; CompareStringW real ----
__declspec(dllexport) int CompareStringW(unsigned loc, unsigned flags, const wchar16* a, int la, const wchar16* b, int lb) {
    (void)loc;(void)flags; int ia=0, ib=0;
    for (;;) { if ((la>=0 && ia>=la) || !a[ia]) break; if ((lb>=0 && ib>=lb) || !b[ib]) break;
        wchar16 ca=a[ia], cb=b[ib]; if (ca>='a'&&ca<='z') ca-=32; if (cb>='a'&&cb<='z') cb-=32;
        if (ca!=cb) return ca<cb ? 1 : 3; ia++; ib++; }
    int ra = (la>=0 && ia>=la) || !a[ia], rb = (lb>=0 && ib>=lb) || !b[ib];
    if (ra && rb) return 2; return ra ? 1 : 3;   // CSTR_LESS/EQUAL/GREATER = 1/2/3
}
__declspec(dllexport) int GetLocaleInfoW(unsigned loc, unsigned lctype, wchar16* buf, int len) { (void)loc;(void)lctype; if (buf&&len>0){ buf[0]='0'; if(len>1) buf[1]=0; } return 2; }
__declspec(dllexport) int GetLocaleInfoEx(const wchar16* loc, unsigned lctype, wchar16* buf, int len) { (void)loc;(void)lctype; if (buf&&len>0){ buf[0]='0'; if(len>1) buf[1]=0; } return 2; }
__declspec(dllexport) unsigned short GetUserDefaultLangID(void) { return 0x0409; }
__declspec(dllexport) unsigned short GetUserDefaultUILanguage(void) { return 0x0409; }
__declspec(dllexport) int GetUserDefaultLocaleName(wchar16* buf, int len) { k32_asctow(buf, "en-US", len); return (int)k32_wlen(buf)+1; }
__declspec(dllexport) int GetUserDefaultGeoName(wchar16* buf, int len) { k32_asctow(buf, "US", len); return (int)k32_wlen(buf)+1; }
__declspec(dllexport) int GetDateFormatW(unsigned loc, unsigned flags, const void* st, const wchar16* fmt, wchar16* buf, int len) { (void)loc;(void)flags;(void)st;(void)fmt; k32_asctow(buf, "07/07/2026", len); return (int)k32_wlen(buf)+1; }
__declspec(dllexport) int GetDateFormatEx(const wchar16* loc, unsigned flags, const void* st, const wchar16* fmt, wchar16* buf, int len, const wchar16* cal) { (void)loc;(void)flags;(void)st;(void)fmt;(void)cal; k32_asctow(buf, "07/07/2026", len); return (int)k32_wlen(buf)+1; }
__declspec(dllexport) int GetTimeFormatEx(const wchar16* loc, unsigned flags, const void* st, const wchar16* fmt, wchar16* buf, int len) { (void)loc;(void)flags;(void)st;(void)fmt; k32_asctow(buf, "12:00:00", len); return (int)k32_wlen(buf)+1; }
__declspec(dllexport) int GetCalendarInfoW(unsigned loc, unsigned cal, unsigned type, wchar16* buf, int len, unsigned* val) { (void)loc;(void)cal;(void)type;(void)buf;(void)len; if (val)*val=0; return 0; }
__declspec(dllexport) unsigned GetTimeZoneInformation(void* tz) { (void)tz; return 0; }   // TIME_ZONE_ID_UNKNOWN
__declspec(dllexport) unsigned GetDynamicTimeZoneInformation(void* tz) { (void)tz; return 0; }
__declspec(dllexport) int FindStringOrdinal(unsigned flags, const wchar16* src, int sl, const wchar16* val, int vl, int ci) { (void)flags;(void)src;(void)sl;(void)val;(void)vl;(void)ci; return -1; }

// ---- path (shlwapi/pathcch via core-shlwapi/core-path redirect): string ops reais ----
__declspec(dllexport) int PathFileExistsW(const wchar16* p) { (void)p; return 0; }
__declspec(dllexport) int PathIsURLW(const wchar16* p) { (void)p; return 0; }
__declspec(dllexport) int PathIsFileSpecW(const wchar16* p) { if(!p) return 1; for(; *p; p++) if(*p=='\\'||*p==':') return 0; return 1; }
__declspec(dllexport) int PathGetDriveNumberW(const wchar16* p) { if (p && p[0] && p[1]==':'){ wchar16 c=p[0]; if(c>='a'&&c<='z') c-=32; if(c>='A'&&c<='Z') return c-'A'; } return -1; }
__declspec(dllexport) wchar16* PathRemoveFileSpecW(wchar16* p) { if(!p) return p; wchar16* last=0; for(wchar16* q=p; *q; q++) if(*q=='\\') last=q; if(last) *last=0; else p[0]=0; return p; }
__declspec(dllexport) void PathRemoveBlanksW(wchar16* p) { if(!p) return; wchar16* s=p; while(*s==' ') s++; wchar16* d=p; while(*s) *d++=*s++; while(d>p && d[-1]==' ') d--; *d=0; }
__declspec(dllexport) int PathQuoteSpacesW(wchar16* p) { (void)p; return 0; }
__declspec(dllexport) wchar16* PathGetArgsW(const wchar16* p) { if(!p) return 0; int q=0; for(; *p; p++){ if(*p=='"') q=!q; else if(*p==' '&&!q) return (wchar16*)(p+1); } return (wchar16*)p; }
__declspec(dllexport) wchar16* PathCombineW(wchar16* dst, const wchar16* dir, const wchar16* file) {
    if(!dst) return 0; int i=0; if(dir){ while(dir[i]){ dst[i]=dir[i]; i++; } if(i && dst[i-1]!='\\'){ dst[i++]='\\'; } }
    if(file){ int j=0; if(file[0]=='\\') j=1; while(file[j]){ dst[i++]=file[j++]; } } dst[i]=0; return dst;
}
__declspec(dllexport) wchar16* PathCommonPrefixW(const wchar16* a, const wchar16* b, wchar16* out) { (void)a;(void)b; if(out) out[0]=0; return 0; }
__declspec(dllexport) int PathParseIconLocationW(wchar16* p) { (void)p; return 0; }
__declspec(dllexport) long PathAllocCombine(const wchar16* a, const wchar16* b, unsigned flags, wchar16** out) { (void)a;(void)b;(void)flags; if(out)*out=0; return (long)0x80070057; }   // E_INVALIDARG
__declspec(dllexport) long PathCchCombine(wchar16* dst, unsigned long long cch, const wchar16* dir, const wchar16* file) { (void)cch; return PathCombineW(dst, dir, file) ? 0 : (long)0x80070057; }
__declspec(dllexport) long PathCchAppend(wchar16* p, unsigned long long cch, const wchar16* more) { (void)cch; if(!p) return (long)0x80070057; unsigned n=k32_wlen(p); if(n && p[n-1]!='\\') p[n++]='\\'; int j=0; if(more){ if(more[0]=='\\') j=1; while(more[j]) p[n++]=more[j++]; } p[n]=0; return 0; }
__declspec(dllexport) long PathCchAddExtension(wchar16* p, unsigned long long cch, const wchar16* ext) { (void)cch; if(!p||!ext) return (long)0x80070057; unsigned n=k32_wlen(p); int j=0; while(ext[j]) p[n++]=ext[j++]; p[n]=0; return 0; }
__declspec(dllexport) long PathCchRemoveFileSpec(wchar16* p, unsigned long long cch) { (void)cch; PathRemoveFileSpecW(p); return 0; }
__declspec(dllexport) int StrCmpICA(const char* a, const char* b) { for(;*a&&*b;a++,b++){ char x=*a,y=*b; if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32; if(x!=y) return (unsigned char)x-(unsigned char)y; } return (unsigned char)*a-(unsigned char)*b; }
__declspec(dllexport) int StrCmpICW(const wchar16* a, const wchar16* b) { for(;*a&&*b;a++,b++){ wchar16 x=*a,y=*b; if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32; if(x!=y) return (int)x-(int)y; } return (int)*a-(int)*b; }
__declspec(dllexport) int StrCmpNICW(const wchar16* a, const wchar16* b, int n) { for(int i=0;i<n&&(*a||*b);i++,a++,b++){ wchar16 x=*a,y=*b; if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32; if(x!=y) return (int)x-(int)y; } return 0; }
__declspec(dllexport) int UrlUnescapeW(wchar16* url, wchar16* out, unsigned* len, unsigned flags) { (void)out;(void)len;(void)flags;(void)url; return 0; }   // S_OK (no-op)
__declspec(dllexport) long SHLoadIndirectString(const wchar16* src, wchar16* out, unsigned outlen, void* r) { (void)r; if(out&&outlen){ int i=0; if(src) for(; src[i] && (unsigned)i<outlen-1; i++) out[i]=src[i]; out[i]=0; } return 0; }
__declspec(dllexport) unsigned SHExpandEnvironmentStringsW(const wchar16* src, wchar16* out, unsigned outlen) { unsigned i=0; if(out&&src) for(; src[i] && i<outlen-1; i++) out[i]=src[i]; if(out&&outlen) out[i]=0; return i+1; }
__declspec(dllexport) unsigned ExpandEnvironmentStringsW(const wchar16* src, wchar16* out, unsigned outlen) { return SHExpandEnvironmentStringsW(src, out, outlen); }
__declspec(dllexport) long QISearch(void* that, const void* map, const void* iid, void** ppv) { (void)that;(void)map;(void)iid; if(ppv)*ppv=0; return (long)0x80004002; }   // E_NOINTERFACE
__declspec(dllexport) long HashData(const void* data, unsigned len, unsigned char* out, unsigned outlen) { (void)data; for(unsigned i=0;i<outlen;i++) out[i]=0; (void)len; return 0; }

// ---- recurso: sem tabela de recursos carregada -> "nao encontrado" ----
__declspec(dllexport) void* FindResourceW(void* mod, const wchar16* name, const wchar16* type) { (void)mod;(void)name;(void)type; return 0; }
__declspec(dllexport) void* FindResourceExW(void* mod, const wchar16* type, const wchar16* name, unsigned short lang) { (void)mod;(void)type;(void)name;(void)lang; return 0; }
__declspec(dllexport) void* LoadResource(void* mod, void* res) { (void)mod;(void)res; return 0; }
__declspec(dllexport) void* LockResource(void* res) { (void)res; return 0; }
__declspec(dllexport) unsigned SizeofResource(void* mod, void* res) { (void)mod;(void)res; return 0; }
__declspec(dllexport) int LoadStringW(void* mod, unsigned id, wchar16* buf, int len) { (void)mod;(void)id; if (buf&&len>0) buf[0]=0; return 0; }

// ---- mapeamento de arquivo: anonimo -> RAM zerada (NtVirtualAlloc) ----
__declspec(dllexport) void* CreateFileMappingW(void* file, void* sec, unsigned prot, unsigned hi, unsigned lo, const wchar16* name) { (void)file;(void)sec;(void)prot;(void)hi;(void)name; return k32_mapnew(lo); }
__declspec(dllexport) void* OpenFileMappingW(unsigned acc, int inh, const wchar16* name) { (void)acc;(void)inh;(void)name; return 0; }
__declspec(dllexport) void* MapViewOfFile(void* mapping, unsigned acc, unsigned hi, unsigned lo, unsigned long long bytes) { (void)acc;(void)hi;(void)lo; k32_map_t* m=(k32_map_t*)mapping; unsigned long long n = bytes ? bytes : (m?m->size:0x1000); if(!n) n=0x1000; return NtVirtualAlloc(n); }
__declspec(dllexport) int UnmapViewOfFile(const void* addr) { (void)addr; return 1; }

// ---- global/local heap: real, sobre o HeapAlloc do kernel32 ----
__declspec(dllexport) void* GlobalAlloc(unsigned flags, unsigned long long size) { void* p=HeapAlloc(0, (flags&0x40)?0x8:0, size?size:1); return p; }   // GMEM_ZEROINIT=0x40
__declspec(dllexport) void* GlobalFree(void* p) { (void)p; return 0; }
__declspec(dllexport) void* GlobalLock(void* p) { return p; }
__declspec(dllexport) int GlobalUnlock(void* p) { (void)p; return 1; }
__declspec(dllexport) unsigned GlobalGetAtomNameW(unsigned short atom, wchar16* buf, int len) { (void)atom; if(buf&&len>0) buf[0]=0; return 0; }
__declspec(dllexport) void* LocalAlloc(unsigned flags, unsigned long long size) { return HeapAlloc(0, (flags&0x40)?0x8:0, size?size:1); }   // LMEM_ZEROINIT=0x40
__declspec(dllexport) void* LocalFree(void* p) { (void)p; return 0; }
__declspec(dllexport) void* LocalReAlloc(void* p, unsigned long long size, unsigned flags) { return HeapReAlloc(0, (flags&0x40)?0x8:0, p, size); }

// ---- job objects / snapshot / actctx / power / misc: stubs especificos ----
__declspec(dllexport) void* CreateJobObjectW(void* sec, const wchar16* name) { (void)sec;(void)name; return k32_fake_handle(); }
__declspec(dllexport) int AssignProcessToJobObject(void* job, void* proc) { (void)job;(void)proc; return 1; }
__declspec(dllexport) int SetInformationJobObject(void* job, int cls, void* info, unsigned len) { (void)job;(void)cls;(void)info;(void)len; return 1; }
__declspec(dllexport) int QueryInformationJobObject(void* job, int cls, void* info, unsigned len, unsigned* ret) { (void)job;(void)cls;(void)info;(void)len; if(ret)*ret=0; return 0; }
__declspec(dllexport) void* CreateToolhelp32Snapshot(unsigned flags, unsigned pid) { (void)flags;(void)pid; return INVALID_HANDLE_VALUE; }
__declspec(dllexport) int Process32FirstW(void* snap, void* entry) { (void)snap;(void)entry; return 0; }
__declspec(dllexport) int Process32NextW(void* snap, void* entry) { (void)snap;(void)entry; return 0; }
__declspec(dllexport) void* CreateActCtxW(void* actctx) { (void)actctx; return INVALID_HANDLE_VALUE; }
__declspec(dllexport) int ActivateActCtx(void* actctx, unsigned long long* cookie) { (void)actctx; if(cookie)*cookie=0; return 1; }
__declspec(dllexport) int DeactivateActCtx(unsigned flags, unsigned long long cookie) { (void)flags;(void)cookie; return 1; }
__declspec(dllexport) void ReleaseActCtx(void* actctx) { (void)actctx; }
__declspec(dllexport) void* PowerCreateRequest(void* ctx) { (void)ctx; return k32_fake_handle(); }
__declspec(dllexport) int PowerSetRequest(void* req, int type) { (void)req;(void)type; return 1; }
__declspec(dllexport) int GetSystemPowerStatus(void* st) { if(st){ unsigned char* b=(unsigned char*)st; for(int i=0;i<12;i++) b[i]=0; b[0]=1; b[1]=255; } return 1; }   // AC online
__declspec(dllexport) int DuplicateHandle(void* sp, void* sh, void* tp, void** th, unsigned acc, int inh, unsigned opt) { (void)sp;(void)tp;(void)acc;(void)inh;(void)opt; if(th)*th=sh; return 1; }
__declspec(dllexport) void* CreateIoCompletionPort(void* file, void* existing, unsigned long long key, unsigned threads) { (void)file;(void)existing;(void)key;(void)threads; return k32_fake_handle(); }
__declspec(dllexport) int GetQueuedCompletionStatus(void* port, unsigned* bytes, unsigned long long* key, void** ov, unsigned ms) { (void)port;(void)key;(void)ms; if(bytes)*bytes=0; if(ov)*ov=0; return 0; }
__declspec(dllexport) int OpenProcessToken(void* proc, unsigned acc, void** tok) { (void)proc;(void)acc; if(tok)*tok=k32_fake_handle(); return 1; }
__declspec(dllexport) int OpenThreadToken(void* thr, unsigned acc, int self, void** tok) { (void)thr;(void)acc;(void)self; if(tok)*tok=0; return 0; }
__declspec(dllexport) unsigned FormatMessageW(unsigned flags, const void* src, unsigned msgid, unsigned lang, wchar16* buf, unsigned size, void* args) { (void)src;(void)msgid;(void)lang;(void)args;(void)flags; if(buf&&size>0) buf[0]=0; return 0; }
__declspec(dllexport) int GetComputerNameW(wchar16* buf, unsigned* size) { const char* n="MEUOS"; if(buf&&size){ k32_asctow(buf, n, (int)*size); *size=k32_wlen(buf); } return 1; }
__declspec(dllexport) int GetProductInfo(unsigned a, unsigned b, unsigned c, unsigned d, unsigned* type) { (void)a;(void)b;(void)c;(void)d; if(type)*type=0x30; return 1; }   // PRODUCT_PROFESSIONAL
__declspec(dllexport) int GetLogicalProcessorInformation(void* buf, unsigned* len) { (void)buf; if(len)*len=0; return 0; }
__declspec(dllexport) int GetPhysicallyInstalledSystemMemory(unsigned long long* kb) { if(kb)*kb=256*1024; return 1; }   // 256 MiB
__declspec(dllexport) int GetOsSafeBootMode(unsigned* mode) { if(mode)*mode=0; return 1; }
__declspec(dllexport) long CheckElevation(const wchar16* path, unsigned* flags, void* a, void* b, void* c) { (void)path;(void)a;(void)b;(void)c; if(flags)*flags=0; return 0; }
__declspec(dllexport) long CheckElevationEnabled(int* enabled) { if(enabled)*enabled=0; return 0; }
__declspec(dllexport) int ApiSetQueryApiSetPresence(const void* ns, unsigned char* present) { (void)ns; if(present)*present=0; return 1; }
__declspec(dllexport) int VerifyVersionInfoW(void* vi, unsigned type, unsigned long long cond) { (void)vi;(void)type;(void)cond; return 1; }
__declspec(dllexport) long RegisterApplicationRestart(const wchar16* cmd, unsigned flags) { (void)cmd;(void)flags; return 0; }
__declspec(dllexport) int SetProcessShutdownParameters(unsigned level, unsigned flags) { (void)level;(void)flags; return 1; }
__declspec(dllexport) int SetTermsrvAppInstallMode(int enable) { (void)enable; return 0; }
__declspec(dllexport) int IsBadWritePtr(void* p, unsigned long long len) { (void)p;(void)len; return 0; }   // assume gravavel
// BiPt* (background intelligent-transfer broker): genuinamente no-op aqui
__declspec(dllexport) long BiPtAssociateApplicationEntryPoint(void* a, void* b, void* c) { (void)a;(void)b;(void)c; return 0; }
__declspec(dllexport) long BiPtEnumerateWorkItemsForPackageName(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return 0; }
__declspec(dllexport) long BiPtQueryWorkItem(void* a, void* b, void* c, void* d, void* e) { (void)a;(void)b;(void)c;(void)d;(void)e; return 0; }
__declspec(dllexport) void BiPtFreeMemory(void* p) { (void)p; }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
