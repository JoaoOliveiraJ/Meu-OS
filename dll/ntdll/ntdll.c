// ntdll.dll  —  a UNICA camada que faz syscalls (int 0x80), igual no Windows.
// Compilada como DLL PE de verdade (export table). kernel32/user32 importam daqui.
unsigned int _tls_index = 0;

enum {
    SYS_EXIT = 2, SYS_MESSAGEBOX = 3,
    SYS_CREATEFILE = 4, SYS_DEVICEIOCONTROL = 5, SYS_CLOSE = 6,
    SYS_CREATEPROCESS = 7, SYS_CREATETHREAD = 8,
    SYS_TERMINATEPROCESS = 9, SYS_WAITFORSINGLEOBJECT = 10,
    SYS_WRITEFILE = 11, SYS_READFILE = 12,
    SYS_OPENKEY = 13, SYS_QUERYVALUEKEY = 14,
    SYS_GETMODULEHANDLE = 15, SYS_GETPROCADDRESS = 16,
    // --- FASE 2: win32k (janelas + mensagens + GDI) ---
    SYS_USERREGISTERCLASS = 17, SYS_USERCREATEWINDOWEX = 18,
    SYS_USERDESTROYWINDOW = 19, SYS_USERSHOWWINDOW = 20,
    SYS_USERGETMESSAGE = 21, SYS_USERDISPATCHMESSAGE = 22,
    SYS_USERPOSTMESSAGE = 23, SYS_USERPOSTQUITMESSAGE = 24,
    SYS_USERGETDC = 25, SYS_USERINVALIDATE = 26,
    SYS_GDIGETSTOCKOBJECT = 27, SYS_GDICREATESOLIDBRUSH = 28,
    SYS_GDITEXTOUT = 29, SYS_GDIFILLRECT = 30,
    // --- FASE 3: Named Pipes (IPC) ---
    SYS_CREATENAMEDPIPE = 31, SYS_CONNECTNAMEDPIPE = 32,
    // --- FASE 4: syscalls de informacao (NtQuery*) ---
    SYS_QUERYSYSTEMINFO = 33, SYS_QUERYINFORMATIONPROCESS = 34,
    SYS_READVIRTUALMEMORY = 35, SYS_WRITEVIRTUALMEMORY = 36,
    // --- FASE 5: shell cmd.exe (enumerar objetos + carregar/descarregar driver) ---
    SYS_ENUMPROCESSES = 37, SYS_ENUMDRIVERS = 38,
    SYS_LOADDRIVER = 39, SYS_UNLOADDRIVER = 40,
    // --- FASE 6: desktop + barra de tarefas + cmd numa janela ---
    SYS_USERSETFOCUS = 41, SYS_USERPOSTKEY = 42, SYS_GDITEXTOUTEX = 43,
    // --- FASE 3 (NTFS): listar diretorio do volume via I/O Manager ---
    SYS_QUERYDIRECTORYFILE = 44,
    // --- FASE 5: info de volume (rotulo/serial/tamanho/fs name) ---
    SYS_QUERYVOLUMEINFORMATION = 45,
    // --- FASE 11: cursor do mouse (le/ajusta posicao) ---
    SYS_USERGETCURSORPOS = 46,
    SYS_USERSETCURSORPOS = 47,
    // --- FASE 3f: LoadLibrary em runtime ---
    SYS_LOADLIBRARY = 48,
    // --- Frente C: tempo do sistema p/ o usermode (explorer real) ---
    SYS_QUERYSYSTEMTIME = 49,
    // --- Frente C: VirtualAlloc (heap do explorer) ---
    SYS_VIRTUALALLOC = 50,
};

// long long = 64-bit no Windows (LLP64), para nao truncar ponteiros.
static long long sc1(long long n, long long a) {
    long long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a) : "memory");
    return r;
}
static long long sc2(long long n, long long a, long long b) {
    long long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "memory");
    return r;
}
static long long sc3(long long n, long long a, long long b, long long c) {
    long long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}
// 4 argumentos: rdi, rsi, rdx, r10 (convencao do nosso int 0x80).
static long long sc4(long long n, long long a1, long long a2, long long a3, long long a4) {
    long long r;
    register long long r10 asm("r10") = a4;
    __asm__ volatile ("int $0x80"
        : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "memory", "rcx", "r11");
    return r;
}
// 5 argumentos: rdi, rsi, rdx, r10, r8.
static long long sc5(long long n, long long a1, long long a2, long long a3,
                     long long a4, long long a5) {
    long long r;
    register long long r10 asm("r10") = a4;
    register long long r8  asm("r8")  = a5;
    __asm__ volatile ("int $0x80"
        : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "memory", "rcx", "r11");
    return r;
}
// 6 argumentos: rdi, rsi, rdx, r10, r8, r9 (convencao do nosso int 0x80).
static long long sc6(long long n, long long a1, long long a2, long long a3,
                     long long a4, long long a5, long long a6) {
    long long r;
    register long long r10 asm("r10") = a4;
    register long long r8  asm("r8")  = a5;
    register long long r9  asm("r9")  = a6;
    __asm__ volatile ("int $0x80"
        : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "memory", "rcx", "r11");
    return r;
}

#define IP(x) ((long long)(__INTPTR_TYPE__)(x))

// Frente C (explorer real): tempo do sistema (o kernel le o KUSER_SHARED_DATA).
// out = { long long SystemTime; unsigned TickCount; }.
__declspec(dllexport) long NtQuerySystemTime(void* out) { return (long)sc1(SYS_QUERYSYSTEMTIME, IP(out)); }

// Frente C (explorer real): VirtualAlloc (backing do HeapAlloc). Devolve o VA (0 = falha).
__declspec(dllexport) void* NtVirtualAlloc(unsigned long long size) { return (void*)(__INTPTR_TYPE__)sc1(SYS_VIRTUALALLOC, (long long)size); }

// "Native API" (Nt*): o ponto onde o modo usuário entra no kernel.
__declspec(dllexport) long NtUserMessageBox(const char* text, const char* caption) {
    return (long)sc2(SYS_MESSAGEBOX, IP(text), IP(caption));
}
__declspec(dllexport) void NtTerminateProcess(unsigned code) {
    sc1(SYS_EXIT, (long long)code);
}
__declspec(dllexport) long NtCreateFile(void* out_handle, const char* name) {
    return (long)sc2(SYS_CREATEFILE, IP(out_handle), IP(name));
}
__declspec(dllexport) long NtDeviceIoControlFile(void* handle, unsigned ioctl,
        void* in_buf, unsigned in_len, void* out_buf, unsigned out_len) {
    return (long)sc6(SYS_DEVICEIOCONTROL, IP(handle), (long long)ioctl,
                     IP(in_buf), (long long)in_len, IP(out_buf), (long long)out_len);
}
__declspec(dllexport) void NtClose(void* handle) {
    sc1(SYS_CLOSE, IP(handle));
}
// NtWriteFile/NtReadFile: para o console device (handle padrao do GetStdHandle)
// escrevem/leem via a saida do kernel; para um handle de arquivo, via IRP.
__declspec(dllexport) long NtWriteFile(void* handle, const void* buf,
                                       unsigned len, unsigned* written) {
    return (long)sc4(SYS_WRITEFILE, IP(handle), IP(buf), (long long)len, IP(written));
}
__declspec(dllexport) long NtReadFile(void* handle, void* buf,
                                      unsigned len, unsigned* read) {
    return (long)sc4(SYS_READFILE, IP(handle), IP(buf), (long long)len, IP(read));
}
// NtQueryDirectoryFile(dirHandle, outBuf, outLen, *retLen) — FASE 3 (NTFS):
// devolve UMA entrada do diretorio aberto no volume (via IRP_MJ_DIRECTORY_CONTROL).
// RAX = STATUS_SUCCESS enquanto houver; 0x80000006 (NO_MORE_FILES) no fim.
__declspec(dllexport) long NtQueryDirectoryFile(void* dirHandle, void* outBuf,
                                                unsigned outLen, unsigned* retLen) {
    return (long)sc4(SYS_QUERYDIRECTORYFILE, IP(dirHandle), IP(outBuf),
                     (long long)outLen, IP(retLen));
}

// NtQueryVolumeInformation(outBuf, outLen, *retLen) — FASE 5: resumo do volume
// NTFS montado como C: (rotulo, serial, tamanho total/livre, fs name "NTFS").
__declspec(dllexport) long NtQueryVolumeInformation(void* outBuf, unsigned outLen,
                                                    unsigned* retLen) {
    return (long)sc3(SYS_QUERYVOLUMEINFORMATION, IP(outBuf), (long long)outLen, IP(retLen));
}

// ---- Named Pipes (IPC) — FASE 3 ----
// NtCreateNamedPipeFile: o servidor cria \Pipe\Nome e recebe um handle de pipe.
__declspec(dllexport) long NtCreateNamedPipeFile(void* out_handle, const char* name) {
    return (long)sc2(SYS_CREATENAMEDPIPE, IP(out_handle), IP(name));
}
// NtConnectNamedPipe: servidor aguarda/aceita um cliente (nao bloqueia aqui).
__declspec(dllexport) long NtConnectNamedPipe(void* pipe_handle) {
    return (long)sc1(SYS_CONNECTNAMEDPIPE, IP(pipe_handle));
}

// ---- Registro (stubs simples) ----
__declspec(dllexport) long NtOpenKey(void* out_handle, const char* path) {
    return (long)sc2(SYS_OPENKEY, IP(out_handle), IP(path));
}
__declspec(dllexport) long NtQueryValueKey(void* key, const char* name,
        void* buf, unsigned buflen, unsigned* outlen) {
    return (long)sc5(SYS_QUERYVALUEKEY, IP(key), IP(name), IP(buf),
                     (long long)buflen, IP(outlen));
}

// ---- FASE 4: syscalls de informacao (NtQuery*) ----
// NtQuerySystemInformation(class, buf, buflen, *retlen): versao do SO, num de CPUs.
__declspec(dllexport) long NtQuerySystemInformation(unsigned infoClass, void* buf,
        unsigned buflen, unsigned* retlen) {
    return (long)sc4(SYS_QUERYSYSTEMINFO, (long long)infoClass, IP(buf),
                     (long long)buflen, IP(retlen));
}
// NtQueryInformationProcess(hProcess, class, buf, buflen, *retlen): pid + base.
__declspec(dllexport) long NtQueryInformationProcess(void* hProcess, unsigned infoClass,
        void* buf, unsigned buflen, unsigned* retlen) {
    return (long)sc5(SYS_QUERYINFORMATIONPROCESS, IP(hProcess), (long long)infoClass,
                     IP(buf), (long long)buflen, IP(retlen));
}
// NtReadVirtualMemory(hProcess, base, buf, len, *read).
__declspec(dllexport) long NtReadVirtualMemory(void* hProcess, void* base,
        void* buf, unsigned len, unsigned* read) {
    return (long)sc5(SYS_READVIRTUALMEMORY, IP(hProcess), IP(base),
                     IP(buf), (long long)len, IP(read));
}
// NtWriteVirtualMemory(hProcess, base, buf, len, *written).
__declspec(dllexport) long NtWriteVirtualMemory(void* hProcess, void* base,
        const void* buf, unsigned len, unsigned* written) {
    return (long)sc5(SYS_WRITEVIRTUALMEMORY, IP(hProcess), IP(base),
                     IP(buf), (long long)len, IP(written));
}

// ---- FASE 5: enumeracao de objetos + drivers (apoiam o cmd.exe) ----
// NtEnumProcesses(index, out): 1 = preencheu *out (o n-esimo EPROCESS), 0 = fim.
__declspec(dllexport) long NtEnumProcesses(unsigned index, void* out) {
    return (long)sc2(SYS_ENUMPROCESSES, (long long)index, IP(out));
}
// NtEnumDrivers(index, out): 1 = preencheu *out (o n-esimo driver), 0 = fim.
__declspec(dllexport) long NtEnumDrivers(unsigned index, void* out) {
    return (long)sc2(SYS_ENUMDRIVERS, (long long)index, IP(out));
}
// NtLoadDriver(name) — sc start: carrega o .sys pelo nome. Retorna NTSTATUS.
__declspec(dllexport) long NtLoadDriver(const char* name) {
    return (long)sc1(SYS_LOADDRIVER, IP(name));
}
// NtUnloadDriver(name) — sc stop: descarrega o driver pelo nome. NTSTATUS.
__declspec(dllexport) long NtUnloadDriver(const char* name) {
    return (long)sc1(SYS_UNLOADDRIVER, IP(name));
}

// ---- Loader (apoiam GetModuleHandle/GetProcAddress do kernel32) ----
// O loader (estilo LdrLoadDll) vive no kernel; estas chamadas o consultam.
__declspec(dllexport) void* LdrGetModuleHandle(const char* name) {
    return (void*)(__INTPTR_TYPE__)sc1(SYS_GETMODULEHANDLE, IP(name));
}
__declspec(dllexport) void* LdrGetProcAddress(void* module_base, const char* fn) {
    return (void*)(__INTPTR_TYPE__)sc2(SYS_GETPROCADDRESS, IP(module_base), IP(fn));
}
// LdrLoadDll(name) — FASE 3f: carrega uma DLL registrada (modulo de boot) sob demanda e
// devolve a base. kernel32!LoadLibraryA chama isto; GetProcAddress resolve por cima.
__declspec(dllexport) void* LdrLoadDll(const char* name) {
    return (void*)(__INTPTR_TYPE__)sc1(SYS_LOADLIBRARY, IP(name));
}

// ---- Process Manager (Ps*) ----
__declspec(dllexport) long NtCreateProcess(void* out_handle, const char* image_name, const char* cmdline) {
    return (long)sc3(SYS_CREATEPROCESS, IP(out_handle), IP(image_name), IP(cmdline));
}
__declspec(dllexport) long NtCreateThread(void* out_handle, void* process, void* start) {
    return (long)sc3(SYS_CREATETHREAD, IP(out_handle), IP(process), IP(start));
}
// process==0 -> encerra o processo corrente (nao retorna).
__declspec(dllexport) long NtTerminateProcessEx(void* process, unsigned status) {
    return (long)sc2(SYS_TERMINATEPROCESS, IP(process), (long long)status);
}
__declspec(dllexport) long NtWaitForSingleObject(void* handle, unsigned timeout_ms) {
    return (long)sc2(SYS_WAITFORSINGLEOBJECT, IP(handle), (long long)timeout_ms);
}

// ---- win32k (USER): janelas + mensagens. Os exports Nt* fazem o int 0x80;
//      o user32 (ring 3) os chama e implementa a API Win32 por cima. ----
__declspec(dllexport) long NtUserRegisterClass(const char* className, void* wndProc) {
    return (long)sc2(SYS_USERREGISTERCLASS, IP(className), IP(wndProc));
}
__declspec(dllexport) void* NtUserCreateWindowEx(void* createStruct) {
    return (void*)(__INTPTR_TYPE__)sc1(SYS_USERCREATEWINDOWEX, IP(createStruct));
}
__declspec(dllexport) long NtUserDestroyWindow(void* hwnd) {
    return (long)sc1(SYS_USERDESTROYWINDOW, IP(hwnd));
}
__declspec(dllexport) long NtUserShowWindow(void* hwnd, int cmdShow) {
    return (long)sc2(SYS_USERSHOWWINDOW, IP(hwnd), (long long)cmdShow);
}
// 1 = msg normal, 0 = WM_QUIT, -1 = erro. Preenche *msg.
__declspec(dllexport) long NtUserGetMessage(void* msg) {
    return (long)sc1(SYS_USERGETMESSAGE, IP(msg));
}
__declspec(dllexport) long NtUserDispatchMessage(void* msg) {
    return (long)sc1(SYS_USERDISPATCHMESSAGE, IP(msg));
}
__declspec(dllexport) long NtUserPostMessage(void* hwnd, unsigned msg,
                                             unsigned long long wParam,
                                             unsigned long long lParam) {
    return (long)sc4(SYS_USERPOSTMESSAGE, IP(hwnd), (long long)msg,
                     (long long)wParam, (long long)lParam);
}
__declspec(dllexport) long NtUserPostQuitMessage(int exitCode) {
    return (long)sc1(SYS_USERPOSTQUITMESSAGE, (long long)exitCode);
}
__declspec(dllexport) void* NtUserGetDC(void* hwnd) {
    return (void*)(__INTPTR_TYPE__)sc1(SYS_USERGETDC, IP(hwnd));
}
__declspec(dllexport) long NtUserInvalidate(void* hwnd) {
    return (long)sc1(SYS_USERINVALIDATE, IP(hwnd));
}

// ---- win32k (GDI) ----
__declspec(dllexport) void* NtGdiGetStockObject(int index) {
    return (void*)(__INTPTR_TYPE__)sc1(SYS_GDIGETSTOCKOBJECT, (long long)index);
}
__declspec(dllexport) void* NtGdiCreateSolidBrush(unsigned color) {
    return (void*)(__INTPTR_TYPE__)sc1(SYS_GDICREATESOLIDBRUSH, (long long)color);
}
__declspec(dllexport) long NtGdiTextOut(void* hdc, int x, int y, const char* str, int len) {
    return (long)sc5(SYS_GDITEXTOUT, IP(hdc), (long long)x, (long long)y, IP(str), (long long)len);
}
__declspec(dllexport) long NtGdiFillRect(void* hdc, int x, int y, int w, int h, void* hbrush) {
    return (long)sc6(SYS_GDIFILLRECT, IP(hdc), (long long)x, (long long)y,
                     (long long)w, (long long)h, IP(hbrush));
}

// ---- FASE 6: desktop + barra de tarefas + cmd numa janela ----
// NtUserSetFocus(HWND): da o foco a janela (clique simulado / Alt+Tab).
__declspec(dllexport) long NtUserSetFocus(void* hwnd) {
    return (long)sc1(SYS_USERSETFOCUS, IP(hwnd));
}
// NtUserPostKey(HWND, ascii, scancode): posta WM_KEYDOWN+WM_CHAR para UMA janela.
__declspec(dllexport) long NtUserPostKey(void* hwnd, int ascii, int scancode) {
    return (long)sc3(SYS_USERPOSTKEY, IP(hwnd), (long long)ascii, (long long)scancode);
}
// NtGdiTextOutEx(HDC, x, y, str, len, fg): TextOut com cor de texto (console).
__declspec(dllexport) long NtGdiTextOutEx(void* hdc, int x, int y, const char* str,
                                          int len, unsigned fg) {
    return (long)sc6(SYS_GDITEXTOUTEX, IP(hdc), (long long)x, (long long)y,
                     IP(str), (long long)len, (long long)fg);
}

// ---- FASE 11: cursor do mouse ----
// NtUserGetCursorPos(out POINT*) -> 1/0. *out recebe x,y do cursor (pixels).
__declspec(dllexport) long NtUserGetCursorPos(void* out_point) {
    return (long)sc1(SYS_USERGETCURSORPOS, IP(out_point));
}
// NtUserSetCursorPos(int x, int y) -> 1. Move o cursor (com clamp ao tamanho).
__declspec(dllexport) long NtUserSetCursorPos(int x, int y) {
    return (long)sc2(SYS_USERSETCURSORPOS, (long long)x, (long long)y);
}

// ============================================================================
// Frente C (explorer real) — ntdll: Rtl*/Zw*/heap/SRW/versao/WNF/telemetria + libc.
// O explorer importa 73 funcoes da ntdll. Implementamos DE VERDADE as que fazem
// trabalho (strings UNICODE, heap, run-once, versao, C runtime); stubs ESPECIFICOS e
// nomeados onde a funcao e' genuinamente no-op aqui (WNF, telemetria Sqm, SRW single-
// threaded, unwind sem SEH ativo). Sem catch-all generico. Tipos LLP64: long=32 bits.
// ============================================================================
typedef unsigned short     WCHAR_;
typedef unsigned short     USHORT_;
typedef long               NTSTATUS_;      // 32-bit (LLP64)
#define STATUS_SUCCESS_         ((NTSTATUS_)0)
#define STATUS_UNSUCCESSFUL_    ((NTSTATUS_)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED_ ((NTSTATUS_)0xC0000002L)
#define STATUS_NOT_FOUND_       ((NTSTATUS_)0xC0000225L)

typedef struct { USHORT_ Length; USHORT_ MaximumLength; WCHAR_* Buffer; } UNICODE_STRING_;
typedef struct { USHORT_ Length; USHORT_ MaximumLength; char*   Buffer; } ANSI_STRING_;

static unsigned long long ntd_wlen(const WCHAR_* s){ unsigned long long n=0; if(s) while(s[n]) n++; return n; }
static unsigned long long ntd_alen(const char* s){ unsigned long long n=0; if(s) while(s[n]) n++; return n; }

// ---- C runtime que o explorer importa DA ntdll (impl reais) ----
__declspec(dllexport) char* strchr(const char* s, int c) {
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) return (char*)s;
    return (c == 0) ? (char*)s : 0;
}
__declspec(dllexport) WCHAR_* wcschr(const WCHAR_* s, WCHAR_ c) {
    for (; *s; s++) if (*s == c) return (WCHAR_*)s;
    return (c == 0) ? (WCHAR_*)s : 0;
}
__declspec(dllexport) WCHAR_* wcsrchr(const WCHAR_* s, WCHAR_ c) {
    const WCHAR_* last = 0;
    for (; *s; s++) if (*s == c) last = s;
    if (c == 0) return (WCHAR_*)s;
    return (WCHAR_*)last;
}
__declspec(dllexport) unsigned long long wcsspn(const WCHAR_* s, const WCHAR_* set) {
    const WCHAR_* p = s;
    for (; *p; p++) { const WCHAR_* q = set; while (*q && *q != *p) q++; if (!*q) break; }
    return (unsigned long long)(p - s);
}

// ---- Heap: bump com header de 8 bytes (size), backing NtVirtualAlloc. Free no-op. ----
static unsigned char*     g_heap_arena = 0;
static unsigned long long g_heap_off = 0, g_heap_cap = 0;
static void* ntd_heap_bump(unsigned long long size) {
    unsigned long long need = (size + 8 + 15) & ~15ULL;
    if (!g_heap_arena || g_heap_off + need > g_heap_cap) {
        unsigned long long chunk = need > 0x200000ULL ? ((need + 0xFFFFFULL) & ~0xFFFFFULL) : 0x200000ULL;
        unsigned char* a = (unsigned char*)NtVirtualAlloc(chunk);
        if (!a) return 0;
        g_heap_arena = a; g_heap_off = 0; g_heap_cap = chunk;
    }
    unsigned char* p = g_heap_arena + g_heap_off;
    *(unsigned long long*)p = size;
    g_heap_off += need;
    return p + 8;
}
__declspec(dllexport) void* RtlAllocateHeap(void* heap, unsigned long flags, unsigned long long size) {
    (void)heap; void* p = ntd_heap_bump(size);
    if (p && (flags & 0x8)) { unsigned char* b = (unsigned char*)p; for (unsigned long long i=0;i<size;i++) b[i]=0; }  // HEAP_ZERO_MEMORY
    return p;
}
__declspec(dllexport) int RtlFreeHeap(void* heap, unsigned long flags, void* p) { (void)heap;(void)flags;(void)p; return 1; }
__declspec(dllexport) void* RtlReAllocateHeap(void* heap, unsigned long flags, void* p, unsigned long long size) {
    if (!p) return RtlAllocateHeap(heap, flags, size);
    unsigned long long old = *((unsigned long long*)p - 1);
    void* np = ntd_heap_bump(size); if (!np) return 0;
    unsigned long long n = old < size ? old : size;
    unsigned char* d = (unsigned char*)np; const unsigned char* s = (const unsigned char*)p;
    for (unsigned long long i=0;i<n;i++) d[i]=s[i];
    if ((flags & 0x8) && size > old) for (unsigned long long i=old;i<size;i++) d[i]=0;
    return np;
}
__declspec(dllexport) long RtlFlushHeaps(void) { return 0; }

// ---- Rtl string (UNICODE_STRING/ANSI_STRING) — impl reais ----
__declspec(dllexport) void RtlInitUnicodeString(UNICODE_STRING_* d, const WCHAR_* s) {
    if (!d) return;
    unsigned long long n = ntd_wlen(s);
    d->Length = (USHORT_)(n*2); d->MaximumLength = (USHORT_)(n*2+2); d->Buffer = (WCHAR_*)s;
}
__declspec(dllexport) long RtlInitUnicodeStringEx(UNICODE_STRING_* d, const WCHAR_* s) { RtlInitUnicodeString(d, s); return 0; }
__declspec(dllexport) void RtlInitString(ANSI_STRING_* d, const char* s) {
    if (!d) return;
    unsigned long long n = ntd_alen(s);
    d->Length = (USHORT_)n; d->MaximumLength = (USHORT_)(n+1); d->Buffer = (char*)s;
}
__declspec(dllexport) WCHAR_ RtlUpcaseUnicodeChar(WCHAR_ c) { return (c>='a'&&c<='z') ? (WCHAR_)(c-32) : c; }
__declspec(dllexport) long RtlCompareUnicodeString(const UNICODE_STRING_* a, const UNICODE_STRING_* b, unsigned char ci) {
    unsigned long long la = a?a->Length/2:0, lb = b?b->Length/2:0, n = la<lb?la:lb;
    for (unsigned long long i=0;i<n;i++){ WCHAR_ ca=a->Buffer[i], cb=b->Buffer[i];
        if (ci){ ca=RtlUpcaseUnicodeChar(ca); cb=RtlUpcaseUnicodeChar(cb);} if (ca!=cb) return (long)ca-(long)cb; }
    return (long)la-(long)lb;
}
__declspec(dllexport) long RtlCopyUnicodeString(UNICODE_STRING_* d, const UNICODE_STRING_* s) {
    if (!d) return 0;
    unsigned long long n = s?s->Length:0; if (n > d->MaximumLength) n = d->MaximumLength;
    for (unsigned long long i=0;i<n/2;i++) d->Buffer[i]=s->Buffer[i];
    d->Length = (USHORT_)n; return 0;
}
__declspec(dllexport) long RtlUpcaseUnicodeString(UNICODE_STRING_* d, const UNICODE_STRING_* s, unsigned char alloc) {
    (void)alloc; if (!d||!s) return STATUS_SUCCESS_;
    unsigned long long n = s->Length; if (n > d->MaximumLength) n = d->MaximumLength;
    for (unsigned long long i=0;i<n/2;i++) d->Buffer[i]=RtlUpcaseUnicodeChar(s->Buffer[i]);
    d->Length=(USHORT_)n; return STATUS_SUCCESS_;
}
__declspec(dllexport) long RtlAppendUnicodeToString(UNICODE_STRING_* d, const WCHAR_* s) {
    if (!d) return STATUS_SUCCESS_;
    unsigned long long sl = ntd_wlen(s), cur = d->Length/2;
    for (unsigned long long i=0;i<sl && (cur+i)*2+2 <= d->MaximumLength;i++) d->Buffer[cur+i]=s[i];
    unsigned long long room = d->MaximumLength/2 - cur; if (sl>room) sl=room;
    d->Length=(USHORT_)((cur+sl)*2); return STATUS_SUCCESS_;
}
__declspec(dllexport) long RtlAppendUnicodeStringToString(UNICODE_STRING_* d, const UNICODE_STRING_* s) {
    if (!d||!s) return STATUS_SUCCESS_;
    unsigned long long sl=s->Length/2, cur=d->Length/2, room=d->MaximumLength/2-cur; if (sl>room) sl=room;
    for (unsigned long long i=0;i<sl;i++) d->Buffer[cur+i]=s->Buffer[i];
    d->Length=(USHORT_)((cur+sl)*2); return STATUS_SUCCESS_;
}
__declspec(dllexport) void RtlFreeUnicodeString(UNICODE_STRING_* s) { if (s){ s->Length=0; s->MaximumLength=0; s->Buffer=0; } }
__declspec(dllexport) long RtlAnsiStringToUnicodeString(UNICODE_STRING_* d, const ANSI_STRING_* s, unsigned char alloc) {
    (void)alloc; if (!d||!s) return STATUS_UNSUCCESSFUL_;
    unsigned long long n=s->Length; if (n*2+2 > d->MaximumLength) return STATUS_UNSUCCESSFUL_;
    for (unsigned long long i=0;i<n;i++) d->Buffer[i]=(WCHAR_)(unsigned char)s->Buffer[i];
    d->Buffer[n]=0; d->Length=(USHORT_)(n*2); return STATUS_SUCCESS_;
}
__declspec(dllexport) unsigned long RtlxAnsiStringToUnicodeSize(const ANSI_STRING_* s) { return s ? (unsigned long)(s->Length*2+2) : 0; }

// ---- run-once (RtlRunOnceExecuteOnce): executa o callback UMA vez de verdade ----
__declspec(dllexport) long RtlRunOnceExecuteOnce(void** once, int (*fn)(void**, void*, void**), void* param, void** ctx) {
    if (once && *once) return STATUS_SUCCESS_;
    void* lctx = 0; int ok = fn ? fn(once, param, &lctx) : 1;
    if (ok && once) *once = (void*)(unsigned long long)1;
    if (ctx) *ctx = lctx;
    return ok ? STATUS_SUCCESS_ : STATUS_UNSUCCESSFUL_;
}
// ---- SRW locks (single-threaded: no-op correto) ----
__declspec(dllexport) void RtlAcquireSRWLockExclusive(void** l){ (void)l; }
__declspec(dllexport) void RtlAcquireSRWLockShared(void** l){ (void)l; }
__declspec(dllexport) void RtlReleaseSRWLockExclusive(void** l){ (void)l; }
__declspec(dllexport) void RtlReleaseSRWLockShared(void** l){ (void)l; }

// ---- versao do SO (Windows 10 build 19045) ----
typedef struct { unsigned long dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; WCHAR_ szCSDVersion[128]; } RTL_OSVERSIONINFOW_;
__declspec(dllexport) long RtlGetVersion(RTL_OSVERSIONINFOW_* p) {
    if (!p) return STATUS_UNSUCCESSFUL_;
    p->dwMajorVersion=10; p->dwMinorVersion=0; p->dwBuildNumber=19045; p->dwPlatformId=2; p->szCSDVersion[0]=0;
    return STATUS_SUCCESS_;
}
__declspec(dllexport) long RtlGetNativeSystemInformation(unsigned cls, void* buf, unsigned long len, unsigned long* ret) {
    return (long)sc4(SYS_QUERYSYSTEMINFO, (long long)cls, IP(buf), (long long)len, IP(ret));
}
__declspec(dllexport) long RtlVerifyVersionInfo(void* vi, unsigned long typeMask, unsigned long long condMask) { (void)vi;(void)typeMask;(void)condMask; return STATUS_SUCCESS_; }
__declspec(dllexport) unsigned long long VerSetConditionMask(unsigned long long m, unsigned long t, unsigned char c) {
    unsigned char op = c & 0x07; if (!t) return m;
    unsigned sh; if (t&0x1) sh=0; else if (t&0x2) sh=3; else if (t&0x4) sh=6; else if (t&0x8) sh=9;
    else if (t&0x10) sh=12; else if (t&0x20) sh=15; else if (t&0x40) sh=18; else if (t&0x80) sh=21; else return m;
    m &= ~((unsigned long long)0x07 << sh); m |= ((unsigned long long)op << sh); return m;
}

// ---- Zw* -> mesmos syscalls dos Nt* (os que temos); resto = stub NOT_IMPLEMENTED ----
__declspec(dllexport) void ZwClose(void* h){ sc1(SYS_CLOSE, IP(h)); }
__declspec(dllexport) long ZwQuerySystemInformation(unsigned cls, void* buf, unsigned long len, unsigned long* ret) {
    return (long)sc4(SYS_QUERYSYSTEMINFO, (long long)cls, IP(buf), (long long)len, IP(ret));
}
__declspec(dllexport) long ZwQueryInformationProcess(void* h, unsigned cls, void* buf, unsigned long len, unsigned long* ret) {
    return (long)sc5(SYS_QUERYINFORMATIONPROCESS, IP(h), (long long)cls, IP(buf), (long long)len, IP(ret));
}
__declspec(dllexport) long ZwOpenKey(void* out, unsigned acc, void* attr) { (void)acc; return (long)sc2(SYS_OPENKEY, IP(out), IP(attr)); }
__declspec(dllexport) long ZwQueryValueKey(void* h, void* name, unsigned cls, void* buf, unsigned long len, unsigned long* ret) {
    return (long)sc6(SYS_QUERYVALUEKEY, IP(h), IP(name), (long long)cls, IP(buf), (long long)len, IP(ret));
}
__declspec(dllexport) long ZwCreateFile(void* out, unsigned acc, void* attr) { (void)acc; return (long)sc2(SYS_CREATEFILE, IP(out), IP(attr)); }
__declspec(dllexport) long ZwOpenFile(void* out, unsigned acc, void* attr) { (void)acc; return (long)sc2(SYS_CREATEFILE, IP(out), IP(attr)); }
__declspec(dllexport) long ZwQueryDirectoryFile(void* h, void* a1, void* a2, void* a3, void* iosb, void* buf, unsigned long len, unsigned c, unsigned char s, void* mask, unsigned char rs) {
    (void)a1;(void)a2;(void)a3;(void)c;(void)s;(void)mask;(void)rs; return (long)sc4(SYS_QUERYDIRECTORYFILE, IP(h), IP(buf), (long long)len, IP(iosb)); }
__declspec(dllexport) long ZwEnumerateKey(void* h, unsigned idx, unsigned cls, void* buf, unsigned long len, unsigned long* ret) { (void)h;(void)idx;(void)cls;(void)buf;(void)len; if (ret)*ret=0; return STATUS_NOT_FOUND_; }
__declspec(dllexport) long ZwQueryInformationFile(void* h, void* iosb, void* buf, unsigned long len, unsigned cls) { (void)h;(void)iosb;(void)buf;(void)len;(void)cls; return STATUS_NOT_IMPLEMENTED_; }
__declspec(dllexport) long ZwCreateSection(void* out, unsigned acc, void* attr, void* sz, unsigned prot, unsigned alloc, void* fh) { (void)acc;(void)attr;(void)sz;(void)prot;(void)alloc;(void)fh; if (out)*(void**)out=0; return STATUS_NOT_IMPLEMENTED_; }
__declspec(dllexport) long ZwMapViewOfSection(void* sec, void* proc, void** base, unsigned long long z, unsigned long long cs, void* off, unsigned long long* vs, unsigned it, unsigned alloc, unsigned prot) { (void)sec;(void)proc;(void)z;(void)cs;(void)off;(void)vs;(void)it;(void)alloc;(void)prot; if (base)*base=0; return STATUS_NOT_IMPLEMENTED_; }
__declspec(dllexport) long ZwUnmapViewOfSection(void* proc, void* base) { (void)proc;(void)base; return STATUS_SUCCESS_; }
__declspec(dllexport) long ZwSetInformationProcess(void* h, unsigned cls, void* buf, unsigned long len) { (void)h;(void)cls;(void)buf;(void)len; return STATUS_SUCCESS_; }

// ---- tokens (sem seguranca real ainda): stubs especificos ----
__declspec(dllexport) long NtOpenProcessToken(void* proc, unsigned acc, void** tok) { (void)proc;(void)acc; if (tok)*tok=(void*)(unsigned long long)1; return STATUS_SUCCESS_; }
__declspec(dllexport) long NtOpenThreadToken(void* thr, unsigned acc, unsigned char self, void** tok) { (void)thr;(void)acc;(void)self; if (tok)*tok=0; return STATUS_NOT_FOUND_; }
__declspec(dllexport) long NtQueryInformationToken(void* tok, unsigned cls, void* buf, unsigned long len, unsigned long* ret) { (void)tok;(void)cls;(void)buf;(void)len; if (ret)*ret=0; return STATUS_NOT_IMPLEMENTED_; }
__declspec(dllexport) long NtSetInformationProcess(void* h, unsigned cls, void* buf, unsigned long len) { (void)h;(void)cls;(void)buf;(void)len; return STATUS_SUCCESS_; }
__declspec(dllexport) long NtSetSystemInformation(unsigned cls, void* buf, unsigned long len) { (void)cls;(void)buf;(void)len; return STATUS_SUCCESS_; }
__declspec(dllexport) long NtSetThreadExecutionState(unsigned esFlags, unsigned* prev) { if (prev)*prev=esFlags; return STATUS_SUCCESS_; }

// ---- WNF (Windows Notification Facility): genuinamente no-op aqui ----
__declspec(dllexport) long NtQueryWnfStateData(void* n, void* ti, void* es, unsigned* cs, void* buf, unsigned long* len) { (void)n;(void)ti;(void)es;(void)buf; if (cs)*cs=0; if (len)*len=0; return STATUS_SUCCESS_; }
__declspec(dllexport) long RtlQueryWnfStateData(void* a, void* b, void* c, void* d, void* e) { (void)a;(void)b;(void)c;(void)d;(void)e; return STATUS_SUCCESS_; }
__declspec(dllexport) long RtlPublishWnfStateData(unsigned long long name, void* ti, void* buf, unsigned long len, void* es) { (void)name;(void)ti;(void)buf;(void)len;(void)es; return STATUS_SUCCESS_; }
__declspec(dllexport) long RtlSubscribeWnfStateChangeNotification(void** sub, unsigned long long name, unsigned cs, void* cb, void* ctx, void* td, unsigned sn, unsigned us) { (void)name;(void)cs;(void)cb;(void)ctx;(void)td;(void)sn;(void)us; if (sub)*sub=0; return STATUS_SUCCESS_; }
__declspec(dllexport) long RtlUnsubscribeWnfNotificationWaitForCompletion(void* sub) { (void)sub; return STATUS_SUCCESS_; }

// ---- telemetria (SQM/WinSqm): opted-out, no-op ----
__declspec(dllexport) int  WinSqmIsOptedIn(void) { return 0; }
__declspec(dllexport) void WinSqmAddToStreamEx(void* a, unsigned b, void* c, unsigned d) { (void)a;(void)b;(void)c;(void)d; }
__declspec(dllexport) void WinSqmSetDWORD(void* a, unsigned b, unsigned c) { (void)a;(void)b;(void)c; }

// ---- SEH/unwind: sem excecoes ativas -> sem entrada de funcao / handler ----
__declspec(dllexport) void* RtlLookupFunctionEntry(unsigned long long pc, unsigned long long* imgbase, void* hist) { (void)pc;(void)hist; if (imgbase)*imgbase=0; return 0; }
__declspec(dllexport) void* RtlVirtualUnwind(unsigned type, unsigned long long base, unsigned long long pc, void* fn, void* ctx, void** hdata, unsigned long long* establisher, void* ctxptrs) { (void)type;(void)base;(void)pc;(void)fn;(void)ctx;(void)establisher;(void)ctxptrs; if (hdata)*hdata=0; return 0; }
__declspec(dllexport) void RtlCaptureContext(void* ctx) { (void)ctx; }

// ---- diversos: mapeamento de status, imagem, path, policy — stubs especificos ----
__declspec(dllexport) unsigned char RtlIsStateSeparationEnabled(void) { return 0; }
__declspec(dllexport) unsigned long RtlNtStatusToDosError(long st) { return st==0 ? 0u : (unsigned long)(st & 0xFFFF); }
__declspec(dllexport) unsigned long RtlNtStatusToDosErrorNoTeb(long st) { return st==0 ? 0u : (unsigned long)(st & 0xFFFF); }
__declspec(dllexport) void* RtlImageDirectoryEntryToData(void* base, unsigned char mapped, unsigned short dir, unsigned long* size) {
    (void)mapped; if (!base) { if (size)*size=0; return 0; }
    unsigned char* b = (unsigned char*)base;
    unsigned e = *(unsigned*)(b+0x3C); unsigned char* opt = b+e+4+20;
    unsigned short magic = *(unsigned short*)opt; unsigned ddoff = (magic==0x20B)?112:96;
    unsigned* dd = (unsigned*)(opt+ddoff);
    unsigned rva = dd[dir*2], sz = dd[dir*2+1];
    if (size)*size=sz; return rva ? (void*)(b+rva) : 0;
}
__declspec(dllexport) long RtlQueryResourcePolicy(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return STATUS_NOT_FOUND_; }
__declspec(dllexport) long RtlGetDeviceFamilyInfoEnum(unsigned long long* ver, unsigned* fam, unsigned* form) { if (ver)*ver=0; if (fam)*fam=0; if (form)*form=0; return STATUS_SUCCESS_; }
__declspec(dllexport) long RtlFormatCurrentUserKeyPath(UNICODE_STRING_* out) { if (out){ out->Length=0; out->MaximumLength=0; out->Buffer=0; } return STATUS_UNSUCCESSFUL_; }
__declspec(dllexport) long RtlDosPathNameToNtPathName_U_WithStatus(const WCHAR_* dos, UNICODE_STRING_* nt, WCHAR_** filepart, void* rel) {
    (void)dos;(void)filepart;(void)rel; if (nt){ nt->Length=0; nt->MaximumLength=0; nt->Buffer=0; } return STATUS_NOT_IMPLEMENTED_; }
__declspec(dllexport) unsigned char RtlNtPathNameToDosPathName(unsigned flags, void* inpath, unsigned* outflags, void* rel) { (void)flags;(void)inpath;(void)rel; if (outflags)*outflags=0; return 0; }
__declspec(dllexport) long RtlpEnsureBufferSize(unsigned flags, void* buf, unsigned long long size) { (void)flags;(void)buf;(void)size; return STATUS_SUCCESS_; }
__declspec(dllexport) long LdrResSearchResource(void* base, void* info, unsigned long level, unsigned long flags, void** addr, unsigned long* size, void* a, void* b) {
    (void)base;(void)info;(void)level;(void)flags;(void)a;(void)b; if (addr)*addr=0; if (size)*size=0; return STATUS_NOT_FOUND_; }

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
