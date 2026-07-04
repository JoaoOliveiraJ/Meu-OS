#pragma once
#include <stdint.h>

// ============================================================================
//  win32k.h  —  Lado KERNEL (ring 0) do subsistema grafico Win32 (estilo
//  win32k.sys do NT). Implementa o WINDOW MANAGER (arvore de janelas/HWND,
//  retangulos, z-order, foco), as FILAS DE MENSAGENS por thread e objetos GDI
//  (HDC/HBRUSH/HPEN). Estas funcoes sao chamadas pelo despacho de syscalls
//  (ke/syscall.c) quando o user32/gdi32 (ring 3) fazem int 0x80.
//
//  O desenho vai para o framebuffer (drivers/video.c, fb_*). Em headless cada
//  operacao e logada na serial ([win32k]/[gdi]) para comprovar a logica.
// ============================================================================

// ---- Mensagens (subset do Windows) ----
#define WM_NULL        0x0000
#define WM_CREATE      0x0001
#define WM_DESTROY     0x0002
#define WM_PAINT       0x000F
#define WM_CLOSE       0x0010
#define WM_QUIT        0x0012
#define WM_KEYDOWN     0x0100
#define WM_KEYUP       0x0101
#define WM_CHAR        0x0102
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP   0x0202
#define WM_MOUSEMOVE   0x0200
// FASE 11 — botoes direito/meio (postados pelo win32k_on_mouse_event).
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP   0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP   0x0208

// Stock objects (GetStockObject).
#define WHITE_BRUSH    0
#define LTGRAY_BRUSH   1
#define GRAY_BRUSH     2
#define DKGRAY_BRUSH   3
#define BLACK_BRUSH    4
#define NULL_BRUSH     5

// Estrutura MSG (layout compativel com o que o user32/ntdll passam).
typedef struct _W32_POINT { int32_t x, y; } W32_POINT;
typedef struct _W32_MSG {
    void*     hwnd;        // HWND alvo (id da janela)
    uint32_t  message;     // WM_*
    uint64_t  wParam;
    uint64_t  lParam;
    uint32_t  time;
    W32_POINT pt;
} W32_MSG;

// ---- Flags de janela (FASE 6) — campo 'flags' de W32_CREATE. ----
#define WNDF_DESKTOP   0x0001   // janela de fundo (papel de parede): sem chrome,
                                //   preenche a tela, fica no fundo do z-order.
#define WNDF_CONSOLE   0x0002   // janela de console (cmd): area cliente "terminal".

// Sentinela de cor de fundo "use o padrao" (cinza claro da area cliente).
#define WND_BG_DEFAULT 0xFF

// Estrutura passada por NtUserCreateWindowEx (preenchida pelo user32 em ring 3).
// FASE 6: ganhou 'bgColor' (cor da area cliente; WND_BG_DEFAULT=cinza padrao) e
// 'flags' (WNDF_*). Campos no FIM para o layout antigo continuar valido; o
// CreateWindowExA do user32 preenche bgColor=WND_BG_DEFAULT e flags=0.
typedef struct _W32_CREATE {
    const char* className;
    const char* windowName;
    uint32_t    style;
    int32_t     x, y, w, h;
    void*       hwndParent;
    void*       wndProc;    // ponteiro para o WNDPROC (ring 3) — guardado p/ debug
    uint32_t    bgColor;    // cor da area cliente (indice de paleta) ou WND_BG_DEFAULT
    uint32_t    flags;      // WNDF_* (desktop / console)
} W32_CREATE;

void win32k_init(void);

// ---- Servicos chamados pelas syscalls (cada um devolve um valor em RAX) ----
// Registro de classe: so registra o nome (o wndproc fica no user32, ring 3).
uintptr_t NtUserRegisterClass_k(const char* className, void* wndProc);
// Cria uma janela; devolve o HWND (id) ou 0. Posta WM_CREATE + WM_PAINT.
uintptr_t NtUserCreateWindowEx_k(W32_CREATE* c);
// Destroi a janela: posta WM_DESTROY, remove da arvore, redesenha o desktop.
uintptr_t NtUserDestroyWindow_k(void* hwnd);
// Mostra/oculta (ShowWindow): marca visivel e redesenha. cmdShow!=0 => visivel.
uintptr_t NtUserShowWindow_k(void* hwnd, int cmdShow);
// Tira a proxima mensagem da fila da thread corrente -> *out. Retorna:
//   1 = mensagem normal, 0 = WM_QUIT, -1 = sem janelas/erro. BLOQUEIA (hlt) ate
//   haver mensagem (o teclado/timer por IRQ enchem a fila).
int       NtUserGetMessage_k(W32_MSG* out);
// Posta uma mensagem para a janela (entra na fila da thread dona).
uintptr_t NtUserPostMessage_k(void* hwnd, uint32_t msg, uint64_t wParam, uint64_t lParam);
// Posta WM_QUIT (PostQuitMessage).
uintptr_t NtUserPostQuitMessage_k(int exitCode);
// "Despacho" do lado kernel: apenas loga (o user32 chama o wndproc em ring 3).
uintptr_t NtUserDispatchMessage_k(W32_MSG* msg);
// Invalida a area cliente -> posta WM_PAINT (InvalidateRect/UpdateWindow).
uintptr_t NtUserInvalidate_k(void* hwnd);
// GetDC: devolve um HDC (objeto GDI) ligado a janela.
uintptr_t NtUserGetDC_k(void* hwnd);

// ---- GDI ----
// GetStockObject: devolve um HBRUSH stock (objeto GDI).
uintptr_t NtGdiGetStockObject_k(int index);
// CreateSolidBrush: cria um HBRUSH com a cor (indice de paleta) dada.
uintptr_t NtGdiCreateSolidBrush_k(uint32_t color);
// TextOut(hdc, x, y, str, len): desenha texto na area cliente da janela do HDC.
uintptr_t NtGdiTextOut_k(void* hdc, int x, int y, const char* str, int len);
// FASE 6: TextOut com cor de texto (fg) e fundo (bg, 0xFF=transparente).
uintptr_t NtGdiTextOutEx_k(void* hdc, int x, int y, const char* str, int len,
                           uint32_t fg, uint32_t bg);
// FillRect(hdc, x, y, w, h, hbrush): preenche um retangulo na area cliente.
uintptr_t NtGdiFillRect_k(void* hdc, int x, int y, int w, int h, void* hbrush);
// FASE 9.2: DIB minimo. Aloca um buffer de width*height*4 bytes (XRGB888) e
// devolve um HBITMAP (ponteiro p/ um objeto W32_DIB). Sem CreateDC/BitBlt;
// existe p/ apps que usam DIBs como back-buffer fora da tela.
uintptr_t NtGdiCreateDIBSection_k(int width, int height, void** ppBits);

// ---- FASE 6: foco / multiplas janelas / desktop ----
// Da o foco a uma janela (clique simulado / Alt+Tab). Recompoe (a barra de
// titulo da nova ativa fica azul; a anterior, cinza). Retorna 1 se mudou.
uintptr_t NtUserSetFocus_k(void* hwnd);
// Posta WM_KEYDOWN+WM_CHAR para UMA janela especifica (independe do foco). Usado
// para injetar entrada deterministica em janelas distintas no modo demo headless.
uintptr_t NtUserPostKey_k(void* hwnd, char ascii, uint8_t scancode);

// ---- Entrada (chamada pelo kernel, fora das syscalls) ----
// Roteia uma tecla (do IRQ1) para a janela com foco: posta WM_KEYDOWN + WM_CHAR.
void      win32k_on_key(char ascii, uint8_t scancode);
// Injeta teclas sinteticas + WM_QUIT na janela com foco (teste headless).
void      win32k_inject_demo_input(const char* s);
// FASE 11 — Roteia um evento de mouse (vindo da IRQ12 do PS/2): aplica o delta
// ao cursor (com clamp a tela), faz hit-test e posta WM_MOUSEMOVE +
// WM_LBUTTONDOWN/UP / WM_RBUTTONDOWN/UP para a janela que esta debaixo do
// cursor. 'buttons' = bitmask MOUSE_BTN_LEFT/RIGHT/MIDDLE (ver mouse.h).
void      win32k_on_mouse_event(int32_t dx, int32_t dy, uint32_t buttons);
// FASE 14 — Roteia um evento de mouse ABSOLUTO (vindo do virtio-tablet): a
// posicao ja vem em pixels da tela (nao delta). Atualiza o cursor (com clamp),
// faz hit-test e posta WM_MOUSE*/botoes. NAO mexe no cursor de HW (em modo
// absoluto o HOST o desenha na posicao do ponteiro). 'buttons' = bit0=L bit1=R
// bit2=M (mesma convencao do PS/2).
void      win32k_on_mouse_abs(int32_t x, int32_t y, uint32_t buttons);
// FASE 11 — Posicao atual do cursor (em pixels da tela), atualizada pelo
// roteamento de eventos do mouse. Usados pelas syscalls NtUserGet/SetCursorPos.
int32_t   win32k_cursor_x(void);
int32_t   win32k_cursor_y(void);
// FASE 11 — Move o cursor para (x,y), com clamp a tela. Recompoe pra atualizar
// o sprite no lugar novo. Usado por NtUserSetCursorPos.
void      win32k_set_cursor(int32_t x, int32_t y);
// FASE 11 — Resolucao do backend grafico ativo (LFB 32 bpp ou mode13h 8 bpp).
int       win32k_screen_width(void);
int       win32k_screen_height(void);
// Pinta o desktop + todas as janelas (z-order) + a barra de tarefas no
// framebuffer. Loga na serial cada janela e a barra de tarefas.
void      win32k_compose(void);
// Verdadeiro se ha pelo menos uma janela viva (a GUI esta "no ar").
int       win32k_has_windows(void);
// FASE 6: verdadeiro se a GUI ja compos a tela alguma vez (mantem o framebuffer
// no estado grafico da GUI mesmo apos as janelas serem liberadas).
int       win32k_was_active(void);
// FASE 6: libera todas as janelas de um EPROCESS quando ele encerra (estilo NT).
void      win32k_reap_process_windows(void* eproc);

// ============================================================================
//  FASE 12 — SHELL desktop estilo Windows 10 (win32kshell.c).
//
//  A shell senta em cima do win32kbase: o BASE pinta o framebuffer (wallpaper,
//  chrome simples, taskbar pequena) e a SHELL repinta por cima com a aparencia
//  do Win10 (taskbar 40px, start button, app tray, clock, popup do start menu,
//  window chrome com [_][[]][X] + drag de janelas).
// ============================================================================
void      win32k_shell_init(void);
// Chamado pelo BASE no fim de win32k_compose: redesenha a UI Win10 por cima.
void      win32k_shell_compose(void);
// Chamado pelo BASE no inicio de win32k_on_mouse_event. Retorna 1 se a shell
// consumiu o evento (e o BASE pula a postagem normal de WM_MOUSE*).
int       win32k_shell_on_mouse(int x, int y, uint32_t buttons, uint32_t prev_buttons);
