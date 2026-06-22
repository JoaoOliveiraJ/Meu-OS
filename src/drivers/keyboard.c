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
