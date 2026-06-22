// kernel32.dll  —  reimplementacao. Exporta a API Win32 classica; encaminha
// tudo para o ntdll (que faz a syscall). Igual no Windows.
unsigned int _tls_index = 0;

__declspec(dllimport) void NtTerminateProcess(unsigned code);
__declspec(dllimport) long NtCreateFile(void* out_handle, const char* name);
__declspec(dllimport) long NtDeviceIoControlFile(void* handle, unsigned ioctl,
        void* in_buf, unsigned in_len, void* out_buf, unsigned out_len);
__declspec(dllimport) void NtClose(void* handle);
__declspec(dllimport) long NtCreateProcess(void* out_handle, const char* image_name);
__declspec(dllimport) long NtCreateThread(void* out_handle, void* process, void* start);
__declspec(dllimport) long NtWaitForSingleObject(void* handle, unsigned timeout_ms);
__declspec(dllimport) long NtWriteFile(void* handle, const void* buf, unsigned len, unsigned* written);
__declspec(dllimport) long NtReadFile(void* handle, void* buf, unsigned len, unsigned* read);
__declspec(dllimport) long NtQueryDirectoryFile(void* dirHandle, void* outBuf, unsigned outLen, unsigned* retLen);
__declspec(dllimport) long NtQueryVolumeInformation(void* outBuf, unsigned outLen, unsigned* retLen);
__declspec(dllimport) void* LdrGetModuleHandle(const char* name);
__declspec(dllimport) void* LdrGetProcAddress(void* module_base, const char* fn);
__declspec(dllimport) long NtCreateNamedPipeFile(void* out_handle, const char* name);
__declspec(dllimport) long NtConnectNamedPipe(void* pipe_handle);
// FASE 5 — enumeracao de objetos + carregar/descarregar driver (para o cmd.exe).
__declspec(dllimport) long NtEnumProcesses(unsigned index, void* out);
__declspec(dllimport) long NtEnumDrivers(unsigned index, void* out);
__declspec(dllimport) long NtLoadDriver(const char* name);
__declspec(dllimport) long NtUnloadDriver(const char* name);

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

// --- Process Manager (Win32 -> Nt*) ---
// Versao simplificada de CreateProcessA: cria um EPROCESS + uma thread inicial
// (objetos do Object Manager) e devolve seus handles. Nao executa a imagem.
__declspec(dllexport) int CreateProcessA(const char* app, char* cmdline, void* pa, void* ta,
        int inherit, unsigned flags, void* env, const char* cwd, void* si, void* pi) {
    (void)cmdline; (void)pa; (void)ta; (void)inherit; (void)flags; (void)env; (void)cwd; (void)si;
    void* hproc = 0;
    if (NtCreateProcess(&hproc, app) != 0) return 0;       // FALSE
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

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
