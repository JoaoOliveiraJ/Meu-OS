#include <stdint.h>
#include "keyboard.h"
#include "io.h"
#include "win32/win32k.h"

extern void kputc(char c);

// Tabela scancode (set 1) -> ASCII, layout US.
static const char map[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ',
};

// ---- Fila de stdin do console (entrada interativa, sem GUI) ----
#define KBD_BUF 256
static volatile char     s_kbuf[KBD_BUF];
static volatile uint32_t s_head, s_tail;   // produtor = IRQ; consumidor = NtReadFile

static void kbd_stdin_push(char c) {
    uint32_t next = (s_head + 1) % KBD_BUF;
    if (next == s_tail) return;             // fila cheia: descarta
    s_kbuf[s_head] = c;
    s_head = next;
}

uint32_t kbd_stdin_read(char* dst, uint32_t max) {
    uint32_t n = 0;
    while (n < max && s_tail != s_head) {
        dst[n++] = s_kbuf[s_tail];
        s_tail = (s_tail + 1) % KBD_BUF;
    }
    return n;
}

void keyboard_irq(void) {
    // i8042 compartilha o data port 0x60 entre teclado (IRQ1) e mouse (IRQ12).
    // O bit 5 (AUX) do status 0x64 diz de quem e o byte em 0x60. Se for do mouse,
    // NAO consumimos aqui — deixamos o mouse_irq (IRQ12) ler. Sem este check, com
    // a IRQ12 ligada, o handler do teclado comeria bytes do mouse (cursor travado/
    // erratico + scancodes-fantasma). Espelha o mouse_irq, que ja checa o AUX no
    // sentido oposto (so consome se AUX=1). i8042 real distingue os dois assim.
    if (inb(0x64) & 0x20) return;    // bit5 AUX=1 -> byte e do mouse; cede p/ IRQ12
    uint8_t sc = inb(0x60);          // le o scancode
    if (sc & 0x80) return;           // ignora "key release"
    char c = (sc < 128) ? map[sc] : 0;

    // Se a GUI esta no ar (ha janelas), roteia a tecla para a janela com foco
    // (WM_KEYDOWN + WM_CHAR), igual ao NT. Senao, ecoa no console E enfileira no
    // stdin para o NtReadFile do console device (entrada interativa do cmd.exe).
    if (win32k_has_windows()) {
        win32k_on_key(c, sc);
        return;
    }
    if (c) { kputc(c); kbd_stdin_push(c); }
}
