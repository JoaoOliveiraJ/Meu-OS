// Aplicativo Windows (.exe) que abre um dispositivo e faz DeviceIoControl.
// Fluxo: CreateFileA -> DeviceIoControl(IOCTL) -> MessageBoxA(resultado) -> CloseHandle.
#include "ntddk.h"   // CTL_CODE / FILE_DEVICE_UNKNOWN / METHOD_BUFFERED

unsigned int _tls_index = 0;

#define IOCTL_GET_MAGIC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, 0)

__declspec(dllimport) int   MessageBoxA(void* hWnd, const char* text, const char* caption, unsigned type);
__declspec(dllimport) void* CreateFileA(const char* name, unsigned access, unsigned share,
                                        void* sec, unsigned disp, unsigned flags, void* templ);
__declspec(dllimport) int   DeviceIoControl(void* h, unsigned ioctl, void* inB, unsigned inL,
                                            void* outB, unsigned outL, unsigned* ret, void* ov);
__declspec(dllimport) int   CloseHandle(void* h);
__declspec(dllimport) void  ExitProcess(unsigned code);

static void to_hex(unsigned v, char* b) {
    const char* d = "0123456789ABCDEF";
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = d[(v >> ((7 - i) * 4)) & 0xF];
    b[10] = 0;
}

void _start(void) {
    void* h = CreateFileA("\\Device\\MeuDispositivo", 0, 0, 0, 0, 0, 0);
    if (h == 0 || h == (void*)(long long)-1) {
        MessageBoxA(0, "Falha ao abrir \\Device\\MeuDispositivo", "IoctlApp", 0);
        ExitProcess(1);
    }
    unsigned result = 0, returned = 0;
    DeviceIoControl(h, IOCTL_GET_MAGIC, 0, 0, &result, sizeof(result), &returned, 0);

    char buf[16];
    to_hex(result, buf);
    MessageBoxA(0, buf, "IOCTL respondeu (esperado 0xCAFEBABE)", 0);
    CloseHandle(h);
    ExitProcess(0);
}
