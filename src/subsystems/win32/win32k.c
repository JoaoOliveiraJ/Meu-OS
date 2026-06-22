// ============================================================================
//  win32k.c  —  SHIM do subsistema Win32 kernel (modelo Windows 8.1+ / NT 6.4+).
//
//  No NT moderno, o monolito win32k.sys foi DIVIDIDO em tres modulos:
//      - win32kbase.sys  (servicos essenciais: HWND, message queue, compose,
//                         focus, mouse/keyboard routing, GDI minimo)
//      - win32kfull.sys  (GDI complexo: TextOut com cor, FillRect, DIB,
//                         printing, formatos pesados)
//      - win32k.sys      (shim que ANCORA o nome historico e amarra os dois)
//
//  Este arquivo segue o MESMO modelo do Microsoft Build Lab para preservar o
//  contrato de nomenclatura com o kernel (que ainda chama win32k_init/...):
//  o shim faz #include "win32kbase.c" + #include "win32kfull.c" — entao tudo
//  acaba em UM unico translation unit. As funcoes static dos parciais ficam
//  visiveis dentro deste TU; as exportadas (NtUser*/NtGdi*) sao publicadas no
//  win32k.h e linkadas pelo dispatcher de syscalls.
//
//  Mesmo headless: cada operacao importante e logada na serial (regra 3).
// ============================================================================
#include "win32/win32k.h"
#include "video/video.h"
#include "display/BasicDisplay/gpu.h"
#include "ob/object.h"
#include "ps/process.h"
#include "mm/heap.h"

// Fonte 8x8 embutida (drivers/video/font8x8.c) — usada pelo renderizador de
// texto no BASE quando o backend e o GPU (LFB 32 bpp), pois o helper
// fb_draw_text so escreve no framebuffer de 8 bpp do mode 13h.
extern const uint8_t g_font8x8[96][8];

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern volatile uint64_t g_ticks;   // timer (IRQ0) — usado como "hora" da MSG

// ============================================================================
//  Inclusao dos parciais. PRIMEIRO o BASE (tipos / s_wins / drawing helpers),
//  DEPOIS o FULL (que usa esses tipos e helpers).
// ============================================================================
#include "win32kbase.c"   // FASE 9.10 — parte BASE (HWND + msg queue + GDI minimo)
#include "win32kfull.c"   // FASE 9.10 — parte FULL (TextOut/FillRect/DIB)

// ============================================================================
//  Inicializacao do subsistema. Apenas amarra o init do BASE e adiciona o log
//  "split" para comprovar nos boots que o modelo NT 6.4+ esta vigente.
// ============================================================================
void win32k_init(void) {
    win32kbase_init();
    kputs("[win32k] base + full carregados (Windows 8.1+ split model)\n");
}
