// sysinfo.c  —  Programa WINDOWS (PE32+ x64) que demonstra a FASE 4:
//   1) NtQuerySystemInformation: imprime a VERSAO do SO e o NUM de processadores.
//   2) NtQueryInformationProcess: imprime o PID e a ImageBase do processo atual.
//   3) advapi32 (registro): RegOpenKeyExA + RegQueryValueExA + RegCloseKey leem
//      "ProductName" e "CurrentVersion" do registro (apoiado em NtOpenKey/
//      NtQueryValueKey do kernel).
//   4) advapi32 (SCM): OpenSCManagerA + CreateServiceA + CloseServiceHandle —
//      stubs que NAO falham.
//
// Caminho das Nt*: ring3 -> ntdll (NtQuery*) -> int 0x80 -> SSDT (sys_query*).
// Caminho do registro: ring3 -> advapi32 (Reg*) -> ntdll (NtOpenKey/...) ->
//   int 0x80 -> SSDT. Caminho do SCM: ring3 -> advapi32 (stubs, sem syscall).

unsigned long _tls_index = 0;

#define STD_OUTPUT_HANDLE ((unsigned)-11)

// ---- kernel32 (console) ----
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len,
                                      unsigned* written, void* overlapped);
__declspec(dllimport) void  ExitProcess(unsigned code);

// ---- ntdll (Native API de informacao) ----
__declspec(dllimport) long NtQuerySystemInformation(unsigned infoClass, void* buf,
        unsigned buflen, unsigned* retlen);
__declspec(dllimport) long NtQueryInformationProcess(void* hProcess, unsigned infoClass,
        void* buf, unsigned buflen, unsigned* retlen);

// ---- advapi32 (registro + SCM) ----
__declspec(dllimport) long  RegOpenKeyExA(void* hKey, const char* subKey,
        unsigned options, unsigned samDesired, void** phkResult);
__declspec(dllimport) long  RegQueryValueExA(void* hKey, const char* valueName,
        unsigned* reserved, unsigned* type, void* data, unsigned* cbData);
__declspec(dllimport) long  RegCloseKey(void* hKey);
__declspec(dllimport) void* OpenSCManagerA(const char* m, const char* db, unsigned acc);
__declspec(dllimport) void* CreateServiceA(void* scm, const char* name, const char* disp,
        unsigned acc, unsigned type, unsigned start, unsigned err, const char* bin,
        const char* grp, unsigned* tag, const char* deps, const char* user, const char* pwd);
__declspec(dllimport) int   CloseServiceHandle(void* h);

// Classes que pedimos ao kernel (espelham os defines do src/ke/syscall.c).
#define SystemBasicInformation   0
#define MeuOsVersionInformation  0x1000
#define ProcessBasicInformation  0

// Layouts (subset) que o kernel preenche.
typedef struct {
    unsigned          Reserved, TimerResolution, PageSize, NumberOfPhysicalPages;
    unsigned          LowestPhysicalPageNumber, HighestPhysicalPageNumber, AllocationGranularity;
    unsigned long long MinimumUserModeAddress, MaximumUserModeAddress, ActiveProcessorsAffinityMask;
    unsigned char     NumberOfProcessors;
} SYSTEM_BASIC_INFORMATION;

typedef struct {
    unsigned           ExitStatus, Reserved0;
    unsigned long long PebBaseAddress, AffinityMask, BasePriority, UniqueProcessId, ParentPid;
} PROCESS_BASIC_INFORMATION;

// ---- util sem CRT ----
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}
// imprime um inteiro sem sinal em decimal
static void say_u(unsigned long long v) {
    char buf[21]; int i = 0;
    if (v == 0) { say("0"); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    char out[22]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    say(out);
}
// imprime um inteiro em hex (0x...)
static void say_hex(unsigned long long v) {
    const char* d = "0123456789ABCDEF";
    char out[19]; int j = 0;
    out[j++] = '0'; out[j++] = 'x';
    for (int s = 60; s >= 0; s -= 4) out[j++] = d[(v >> s) & 0xF];
    out[j] = 0;
    say(out);
}

void _start(void) {
    say("  [sysinfo] inicio (ring 3). FASE 4: syscalls de informacao + advapi32.\n");

    // 1) Versao do SO (classe propria) ------------------------------------
    char ver[64];
    for (unsigned i = 0; i < sizeof(ver); i++) ver[i] = 0;
    unsigned vlen = 0;
    if (NtQuerySystemInformation(MeuOsVersionInformation, ver, sizeof(ver) - 1, &vlen) == 0) {
        say("  [sysinfo] versao do SO: \"");
        say(ver);
        say("\"\n");
    } else {
        say("  [sysinfo] NtQuerySystemInformation(versao) FALHOU.\n");
    }

    // 1b) SYSTEM_BASIC_INFORMATION: num de processadores + paginas fisicas --
    SYSTEM_BASIC_INFORMATION sbi;
    for (unsigned i = 0; i < sizeof(sbi); i++) ((char*)&sbi)[i] = 0;
    unsigned slen2 = 0;
    if (NtQuerySystemInformation(SystemBasicInformation, &sbi, sizeof(sbi), &slen2) == 0) {
        say("  [sysinfo] processadores = ");
        say_u(sbi.NumberOfProcessors);
        say(" ; page size = ");
        say_u(sbi.PageSize);
        say(" ; paginas fisicas = ");
        say_u(sbi.NumberOfPhysicalPages);
        say("\n");
    } else {
        say("  [sysinfo] NtQuerySystemInformation(basic) FALHOU.\n");
    }

    // 2) NtQueryInformationProcess do processo atual ----------------------
    PROCESS_BASIC_INFORMATION pbi;
    for (unsigned i = 0; i < sizeof(pbi); i++) ((char*)&pbi)[i] = 0;
    unsigned plen = 0;
    if (NtQueryInformationProcess(0 /*proc atual*/, ProcessBasicInformation,
                                  &pbi, sizeof(pbi), &plen) == 0) {
        say("  [sysinfo] processo atual: PID = ");
        say_u(pbi.UniqueProcessId);
        say(" ; ImageBase = ");
        say_hex(pbi.PebBaseAddress);
        say("\n");
    } else {
        say("  [sysinfo] NtQueryInformationProcess FALHOU.\n");
    }

    // 3) Registro via advapi32 (RegOpenKeyEx/RegQueryValueEx/RegCloseKey) --
    void* hKey = 0;
    if (RegOpenKeyExA((void*)0, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, 0, &hKey) == 0) {
        say("  [sysinfo] RegOpenKeyExA OK.\n");
        char data[64];
        for (unsigned i = 0; i < sizeof(data); i++) data[i] = 0;
        unsigned cb = sizeof(data);
        if (RegQueryValueExA(hKey, "ProductName", 0, 0, data, &cb) == 0) {
            say("  [sysinfo] registro: ProductName = \"");
            say(data);
            say("\"\n");
        }
        for (unsigned i = 0; i < sizeof(data); i++) data[i] = 0;
        cb = sizeof(data);
        if (RegQueryValueExA(hKey, "CurrentVersion", 0, 0, data, &cb) == 0) {
            say("  [sysinfo] registro: CurrentVersion = \"");
            say(data);
            say("\"\n");
        }
        RegCloseKey(hKey);
        say("  [sysinfo] RegCloseKey OK.\n");
    } else {
        say("  [sysinfo] RegOpenKeyExA FALHOU.\n");
    }

    // 4) SCM via advapi32 (stubs que NAO falham) --------------------------
    void* scm = OpenSCManagerA(0, 0, 0);
    if (scm) {
        say("  [sysinfo] OpenSCManagerA OK (handle do SCM nao-nulo).\n");
        void* svc = CreateServiceA(scm, "MeuOSSvc", "MeuOS Demo Service",
                                   0, 0x10 /*WIN32_OWN_PROCESS*/, 0x3 /*DEMAND_START*/,
                                   1 /*ERROR_NORMAL*/, "C:\\meuossvc.exe",
                                   0, 0, 0, 0, 0);
        if (svc) {
            say("  [sysinfo] CreateServiceA OK (servico 'MeuOSSvc' registrado - stub).\n");
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    } else {
        say("  [sysinfo] OpenSCManagerA FALHOU.\n");
    }

    say("  [sysinfo] FASE 4 OK: NtQuery* + advapi32 (SCM + registro) exercitados.\n");
    ExitProcess(0);
}
