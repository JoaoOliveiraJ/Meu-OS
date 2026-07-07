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
__declspec(dllexport) void* LoadLibraryW(const void* name) { (void)name; return 0; }   // wide: nao suportado

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

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
