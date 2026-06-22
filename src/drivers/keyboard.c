#include <stdint.h>
#include "keyboard.h"
#include "io.h"

extern void kputc(char c);

// Tabela scancode (set 1) -> ASCII, layout US.
static const char map[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ',
};

void keyboard_irq(void) {
    uint8_t sc = inb(0x60);          // le o scancode
    if (sc & 0x80) return;           // ignora "key release"
    char c = (sc < 128) ? map[sc] : 0;
    if (c) kputc(c);
}
