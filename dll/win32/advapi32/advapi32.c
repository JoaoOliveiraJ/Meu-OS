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

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
