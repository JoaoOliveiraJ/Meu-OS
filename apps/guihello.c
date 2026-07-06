// FRENTE 3 (Fase 3e) — ALVO: uma aplicacao GRAFICA (GUI) de "terceiro" ESTRUTURA
// PADRAO do Windows: WinMain + RegisterClassA + CreateWindowExA + loop de mensagens
// + WM_PAINT desenhando, compilada com o CRT REAL do mingw (subsistema WINDOWS, o
// startup WinMainCRTStartup chama WinMain). Diferente do guiapp.c (que usa _start
// -nostdlib e declara tudo a mao), este e um programa Win32 "de livro": inclui
// <windows.h> e usa a API padrao. Roda no win32k PROPRIO do MeuOS.
//   Caminho: WinMainCRTStartup (ucrtbase/kernel32) -> WinMain -> user32/gdi32 ->
//            ntdll -> int 0x80 -> win32k -> framebuffer (virtio-gpu). Prova: screendump.
// Compilado por: zig cc -target x86_64-windows-gnu -Wl,--subsystem,windows -luser32 -lgdi32
#include <windows.h>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r = { 8, 26, 200, 50 };
        HBRUSH b = CreateSolidBrush(RGB(200, 30, 30));
        FillRect(hdc, &r, b);
        TextOutA(hdc, 8, 6,  "Janela de TERCEIRO (CRT real)", -1);
        TextOutA(hdc, 12, 30, "WinMain + WM_PAINT OK", -1);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrev; (void)lpCmdLine;

    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = "GuiHelloClass";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, "GuiHelloClass", "GuiHello (terceiro, CRT)",
                                WS_OVERLAPPEDWINDOW, 50, 45, 240, 130,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
