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

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
