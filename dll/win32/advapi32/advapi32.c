// advapi32.dll  —  reimplementacao (FASE 4). Exporta a API do Service Control
// Manager (SCM) e do registro, no estilo do Windows.
//
// Os stubs do SCM NAO falham (devolvem handles validos / TRUE): um servico
// nativo que use StartServiceCtrlDispatcher / RegisterServiceCtrlHandler /
// SetServiceStatus / OpenSCManager / CreateService roda sem erro, mesmo que o
// MeuOS ainda nao tenha um processo services.exe de verdade.
//
// As funcoes de registro (RegOpenKeyExA / RegQueryValueExA / RegCloseKey)
// encaminham para o ntdll (NtOpenKey / NtQueryValueKey), exatamente como o
// advapi32 real apoia-se na Native API. Assim o registro stub do kernel
// (ProductName="MeuOS", CurrentVersion="0.1") fica acessivel pela API Win32.
//
// Igual ao Windows: advapi32 vive em RING 3; so o ntdll faz syscall (int 0x80).

unsigned int _tls_index = 0;

// ---- imports do ntdll (a unica camada que faz syscall) ----
__declspec(dllimport) long NtOpenKey(void* out_handle, const char* path);
__declspec(dllimport) long NtQueryValueKey(void* key, const char* name,
        void* buf, unsigned buflen, unsigned* outlen);

// ============================================================================
//  Tipos do Win32 (subset) usados pela API do SCM e do registro.
// ============================================================================
typedef unsigned long  DWORD;
typedef int            BOOL;
typedef void*          HANDLE;
typedef HANDLE         SC_HANDLE;
typedef HANDLE         HKEY;
typedef unsigned long  SERVICE_STATUS_HANDLE_T;

#define TRUE  1
#define FALSE 0

#define ERROR_SUCCESS            0L
#define ERROR_FILE_NOT_FOUND     2L

// Estados do servico (SERVICE_STATUS.dwCurrentState).
#define SERVICE_STOPPED          0x00000001
#define SERVICE_START_PENDING    0x00000002
#define SERVICE_RUNNING          0x00000004

// SERVICE_TABLE_ENTRYA: nome do servico + ponteiro para a ServiceMain.
typedef void (*LPSERVICE_MAIN_FUNCTIONA)(DWORD argc, char** argv);
typedef struct _SERVICE_TABLE_ENTRYA {
    char*                    lpServiceName;
    LPSERVICE_MAIN_FUNCTIONA lpServiceProc;
} SERVICE_TABLE_ENTRYA, *LPSERVICE_TABLE_ENTRYA;

// SERVICE_STATUS: estado reportado por SetServiceStatus.
typedef struct _SERVICE_STATUS {
    DWORD dwServiceType;
    DWORD dwCurrentState;
    DWORD dwControlsAccepted;
    DWORD dwWin32ExitCode;
    DWORD dwServiceSpecificExitCode;
    DWORD dwCheckPoint;
    DWORD dwWaitHint;
} SERVICE_STATUS, *LPSERVICE_STATUS;

// Handler de controle do servico (RegisterServiceCtrlHandlerA).
typedef void (*LPHANDLER_FUNCTION)(DWORD control);

// Handles sentinela (pseudo-handles) para o SCM e servicos. Como o MeuOS ainda
// nao tem um services.exe, devolvemos valores nao-nulos e estaveis: o codigo do
// servico os trata como handles opacos, sem nunca falhar.
#define SCM_HANDLE_SENTINEL       ((SC_HANDLE)(unsigned long long)0x53434D00) // 'SCM\0'
#define SERVICE_HANDLE_SENTINEL   ((SC_HANDLE)(unsigned long long)0x53565200) // 'SVR\0'
#define SVC_STATUS_HANDLE_SENTINEL ((SERVICE_STATUS_HANDLE_T)0x53534800)      // 'SSH\0'

// ============================================================================
//  Service Control Manager (SCM) — stubs que NAO falham.
// ============================================================================

// StartServiceCtrlDispatcherA: no Windows, conecta o processo ao SCM e chama a
// ServiceMain de cada entrada. Aqui, sem services.exe, CHAMAMOS a ServiceMain da
// primeira entrada diretamente (single-service), para o ponto de entrada do
// servico rodar; depois retornamos TRUE. Assim a logica do servico e exercitada.
__declspec(dllexport) BOOL StartServiceCtrlDispatcherA(const SERVICE_TABLE_ENTRYA* table) {
    if (table && table[0].lpServiceProc) {
        table[0].lpServiceProc(0, 0);   // executa a ServiceMain (ring 3)
    }
    return TRUE;
}

// RegisterServiceCtrlHandlerA: registra o handler de controle e devolve um
// SERVICE_STATUS_HANDLE valido (sentinela). Nunca falha.
__declspec(dllexport) SERVICE_STATUS_HANDLE_T RegisterServiceCtrlHandlerA(
        const char* serviceName, LPHANDLER_FUNCTION handler) {
    (void)serviceName; (void)handler;
    return SVC_STATUS_HANDLE_SENTINEL;
}

// SetServiceStatus: o servico reporta seu estado ao SCM. Stub: aceita e
// devolve TRUE (o estado e ignorado, mas a chamada nunca falha).
__declspec(dllexport) BOOL SetServiceStatus(SERVICE_STATUS_HANDLE_T h,
                                            LPSERVICE_STATUS status) {
    (void)h; (void)status;
    return TRUE;
}

// OpenSCManagerA(machine, database, access) -> SC_HANDLE. Stub: devolve um
// handle do SCM (sentinela) nao-nulo. Nunca falha.
__declspec(dllexport) SC_HANDLE OpenSCManagerA(const char* machineName,
        const char* databaseName, DWORD desiredAccess) {
    (void)machineName; (void)databaseName; (void)desiredAccess;
    return SCM_HANDLE_SENTINEL;
}

// CreateServiceA(...): no Windows, registra um novo servico no SCM. Stub:
// devolve um SC_HANDLE de servico (sentinela) nao-nulo, sem persistir nada.
__declspec(dllexport) SC_HANDLE CreateServiceA(SC_HANDLE scm, const char* serviceName,
        const char* displayName, DWORD desiredAccess, DWORD serviceType,
        DWORD startType, DWORD errorControl, const char* binaryPath,
        const char* loadOrderGroup, DWORD* tagId, const char* dependencies,
        const char* serviceStartName, const char* password) {
    (void)scm; (void)serviceName; (void)displayName; (void)desiredAccess;
    (void)serviceType; (void)startType; (void)errorControl; (void)binaryPath;
    (void)loadOrderGroup; (void)dependencies; (void)serviceStartName; (void)password;
    if (tagId) *tagId = 0;
    return SERVICE_HANDLE_SENTINEL;
}

// OpenServiceA: devolve um SC_HANDLE de servico nao-nulo. Nunca falha.
__declspec(dllexport) SC_HANDLE OpenServiceA(SC_HANDLE scm, const char* serviceName,
                                             DWORD desiredAccess) {
    (void)scm; (void)serviceName; (void)desiredAccess;
    return SERVICE_HANDLE_SENTINEL;
}

// StartServiceA: stub que aceita e devolve TRUE.
__declspec(dllexport) BOOL StartServiceA(SC_HANDLE service, DWORD numArgs,
                                         const char** args) {
    (void)service; (void)numArgs; (void)args;
    return TRUE;
}

// CloseServiceHandle: os handles do SCM/servico sao sentinelas; nada a fechar.
__declspec(dllexport) BOOL CloseServiceHandle(SC_HANDLE h) {
    (void)h;
    return TRUE;
}

// ============================================================================
//  Registro — apoiado no ntdll (NtOpenKey / NtQueryValueKey), como no Windows.
// ============================================================================

// RegOpenKeyExA(hKey, subKey, options, samDesired, *phkResult) -> LONG.
// Encaminha para NtOpenKey (o kernel devolve a raiz fixa do registro stub).
// Retorna ERROR_SUCCESS (0) em caso de sucesso, igual ao Win32.
__declspec(dllexport) long RegOpenKeyExA(HKEY hKey, const char* subKey,
        DWORD options, DWORD samDesired, HKEY* phkResult) {
    (void)hKey; (void)options; (void)samDesired;
    HKEY result = 0;
    long st = NtOpenKey(&result, subKey ? subKey : "");
    if (st != 0) { if (phkResult) *phkResult = 0; return ERROR_FILE_NOT_FOUND; }
    if (phkResult) *phkResult = result;
    return ERROR_SUCCESS;
}

// RegQueryValueExA(hKey, valueName, reserved, *type, data, *cbData) -> LONG.
// Encaminha para NtQueryValueKey. *cbData entra com o tamanho do buffer e sai
// com os bytes copiados (semantica do Win32). type, se != 0, recebe REG_SZ(1).
__declspec(dllexport) long RegQueryValueExA(HKEY hKey, const char* valueName,
        DWORD* reserved, DWORD* type, void* data, DWORD* cbData) {
    (void)reserved;
    unsigned buflen = (cbData ? (unsigned)*cbData : 0);
    unsigned outlen = 0;
    long st = NtQueryValueKey(hKey, valueName ? valueName : "", data, buflen, &outlen);
    if (st != 0) { if (cbData) *cbData = 0; return ERROR_FILE_NOT_FOUND; }
    if (type)   *type   = 1;          // REG_SZ
    if (cbData) *cbData = outlen;     // bytes do valor (inclui terminador)
    return ERROR_SUCCESS;
}

// RegCloseKey: o handle do registro stub e um pseudo-handle (raiz fixa); nada
// a liberar de fato. Retorna ERROR_SUCCESS.
__declspec(dllexport) long RegCloseKey(HKEY hKey) {
    (void)hKey;
    return ERROR_SUCCESS;
}

// ============================================================================
//  Registro — variantes WIDE (W) que o explorer.exe REAL importa (via
//  api-ms-win-core-registry -> advapi32). Convertem nome wide->narrow e encaminham
//  p/ os mesmos Nt* (registro stub do kernel). Chave/valor ausente -> nao encontrado
//  / enumeracao vazia (HONESTO: o registro stub esta vazio; app bem-feito usa default).
// ============================================================================
typedef unsigned short REG_WCHAR;
static void reg_w2n(const REG_WCHAR* w, char* n, int max) {
    int i = 0; if (w) while (w[i] && i < max-1) { n[i] = (char)w[i]; i++; } n[i] = 0;
}
#define ERROR_NO_MORE_ITEMS 259

__declspec(dllexport) long RegOpenKeyExW(HKEY hKey, const REG_WCHAR* subKey, DWORD opt, DWORD sam, HKEY* phk) {
    char n[512]; reg_w2n(subKey, n, sizeof(n)); return RegOpenKeyExA(hKey, n, opt, sam, phk);
}
__declspec(dllexport) long RegQueryValueExW(HKEY hKey, const REG_WCHAR* val, DWORD* res, DWORD* type, void* data, DWORD* cb) {
    char n[256]; reg_w2n(val, n, sizeof(n)); return RegQueryValueExA(hKey, n, res, type, data, cb);
}
__declspec(dllexport) long RegGetValueW(HKEY hKey, const REG_WCHAR* sub, const REG_WCHAR* val, DWORD flags, DWORD* type, void* data, DWORD* cb) {
    (void)sub; (void)flags; return RegQueryValueExW(hKey, val, 0, type, data, cb);
}
__declspec(dllexport) long RegCreateKeyExW(HKEY hKey, const REG_WCHAR* sub, DWORD res, REG_WCHAR* cls, DWORD opt, DWORD sam, void* sa, HKEY* phk, DWORD* disp) {
    (void)res; (void)cls; (void)opt; (void)sa; char n[512]; reg_w2n(sub, n, sizeof(n));
    long st = RegOpenKeyExA(hKey, n, 0, sam, phk);
    if (st != ERROR_SUCCESS && phk) *phk = hKey;    // devolve a raiz p/ nao travar a criacao
    if (disp) *disp = 2;                            // REG_OPENED_EXISTING_KEY
    return ERROR_SUCCESS;
}
__declspec(dllexport) long RegEnumKeyExW(HKEY h, DWORD i, REG_WCHAR* nm, DWORD* cn, DWORD* r, REG_WCHAR* cl, DWORD* ccl, void* ft) {
    (void)h;(void)i;(void)nm;(void)cn;(void)r;(void)cl;(void)ccl;(void)ft; return ERROR_NO_MORE_ITEMS;
}
__declspec(dllexport) long RegEnumValueW(HKEY h, DWORD i, REG_WCHAR* nm, DWORD* cn, DWORD* r, DWORD* type, void* data, DWORD* cb) {
    (void)h;(void)i;(void)nm;(void)cn;(void)r;(void)type;(void)data;(void)cb; return ERROR_NO_MORE_ITEMS;
}
__declspec(dllexport) long RegQueryInfoKeyW(HKEY h, REG_WCHAR* cl, DWORD* ccl, DWORD* r, DWORD* nsub, DWORD* msub, DWORD* mcl, DWORD* nval, DWORD* mval, DWORD* mdata, DWORD* sec, void* ft) {
    (void)h;(void)cl;(void)ccl;(void)r;(void)msub;(void)mcl;(void)mval;(void)mdata;(void)sec;(void)ft;
    if (nsub) *nsub = 0; if (nval) *nval = 0; return ERROR_SUCCESS;   // chave vazia
}
__declspec(dllexport) long RegSetValueExW(HKEY h, const REG_WCHAR* v, DWORD r, DWORD type, const void* data, DWORD cb) {
    (void)h;(void)v;(void)r;(void)type;(void)data;(void)cb; return ERROR_SUCCESS;
}
__declspec(dllexport) long RegSetKeyValueW(HKEY h, const REG_WCHAR* sub, const REG_WCHAR* v, DWORD type, const void* data, DWORD cb) {
    (void)h;(void)sub;(void)v;(void)type;(void)data;(void)cb; return ERROR_SUCCESS;
}
__declspec(dllexport) long RegDeleteValueW(HKEY h, const REG_WCHAR* v)  { (void)h;(void)v; return ERROR_SUCCESS; }
__declspec(dllexport) long RegDeleteKeyExW(HKEY h, const REG_WCHAR* sub, DWORD sam, DWORD r) { (void)h;(void)sub;(void)sam;(void)r; return ERROR_SUCCESS; }
__declspec(dllexport) long RegDeleteKeyValueW(HKEY h, const REG_WCHAR* sub, const REG_WCHAR* v) { (void)h;(void)sub;(void)v; return ERROR_SUCCESS; }
__declspec(dllexport) long RegDeleteTreeW(HKEY h, const REG_WCHAR* sub) { (void)h;(void)sub; return ERROR_SUCCESS; }
__declspec(dllexport) long RegNotifyChangeKeyValue(HKEY h, int w, DWORD f, HKEY ev, int a) { (void)h;(void)w;(void)f;(void)ev;(void)a; return ERROR_SUCCESS; }
__declspec(dllexport) long RegGetKeySecurity(HKEY h, DWORD si, void* sd, DWORD* cb) { (void)h;(void)si;(void)sd; if (cb) *cb = 0; return ERROR_SUCCESS; }
__declspec(dllexport) long RegSetKeySecurity(HKEY h, DWORD si, void* sd) { (void)h;(void)si;(void)sd; return ERROR_SUCCESS; }
__declspec(dllexport) long RegOpenCurrentUser(DWORD sam, HKEY* phk) { (void)sam; if (phk) *phk = (HKEY)(unsigned long long)0x80000001ULL; return ERROR_SUCCESS; }
__declspec(dllexport) long RegisterApplicationRestart(const REG_WCHAR* cmd, DWORD flags) { (void)cmd; (void)flags; return 0; }

// ============================================================================
//  ETW (Event Tracing for Windows) — api-ms-win-eventing-* -> advapi32. NAO fazemos
//  tracing; todos no-op HONESTOS (ERROR_SUCCESS / provider nao-habilitado). O explorer
//  registra providers de trace na init do shell; ignoramos sem afetar a logica dele.
// ============================================================================
__declspec(dllexport) long EventRegister(const void* guid, void* cb, void* ctx, unsigned long long* handle) {
    (void)guid;(void)cb;(void)ctx; if (handle) *handle = 0; return 0;
}
__declspec(dllexport) long EventUnregister(unsigned long long h) { (void)h; return 0; }
__declspec(dllexport) long EventWrite(unsigned long long h, const void* d, unsigned c, void* data) { (void)h;(void)d;(void)c;(void)data; return 0; }
__declspec(dllexport) long EventWriteTransfer(unsigned long long h, const void* d, const void* a, const void* r, unsigned c, void* data) { (void)h;(void)d;(void)a;(void)r;(void)c;(void)data; return 0; }
__declspec(dllexport) long EventWriteEx(unsigned long long h, const void* d, unsigned long long f, unsigned long long fl, const void* a, const void* r, unsigned c, void* data) { (void)h;(void)d;(void)f;(void)fl;(void)a;(void)r;(void)c;(void)data; return 0; }
__declspec(dllexport) int  EventEnabled(unsigned long long h, const void* d) { (void)h;(void)d; return 0; }
__declspec(dllexport) int  EventProviderEnabled(unsigned long long h, unsigned char lvl, unsigned long long kw) { (void)h;(void)lvl;(void)kw; return 0; }
__declspec(dllexport) long EventSetInformation(unsigned long long h, int cls, void* info, unsigned len) { (void)h;(void)cls;(void)info;(void)len; return 0; }
__declspec(dllexport) long EventActivityIdControl(unsigned code, void* id) { (void)code;(void)id; return 0; }
__declspec(dllexport) long RegisterTraceGuidsW(void* cb, void* ctx, const void* ctrl, unsigned cnt, void* reg, const unsigned short* mof, const unsigned short* res, unsigned long long* handle) {
    (void)cb;(void)ctx;(void)ctrl;(void)cnt;(void)reg;(void)mof;(void)res; if (handle) *handle = 0; return 0;
}
__declspec(dllexport) long UnregisterTraceGuids(unsigned long long h) { (void)h; return 0; }
__declspec(dllexport) unsigned long long GetTraceLoggerHandle(void* buf) { (void)buf; return 0; }
__declspec(dllexport) unsigned char GetTraceEnableLevel(unsigned long long h) { (void)h; return 0; }
__declspec(dllexport) unsigned long long GetTraceEnableFlags(unsigned long long h) { (void)h; return 0; }
__declspec(dllexport) unsigned long TraceEventInstance(unsigned long long h, void* ev, void* inst) { (void)h;(void)ev;(void)inst; return 0; }
__declspec(dllexport) unsigned long TraceEvent(unsigned long long h, void* ev) { (void)h;(void)ev; return 0; }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
