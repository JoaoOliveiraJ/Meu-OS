#include <stdint.h>
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00); // desliga interrupcoes
    outb(COM1 + 3, 0x80); // DLAB on (configurar baud)
    outb(COM1 + 0, 0x01); // divisor baixo = 1  -> 115200 baud
    outb(COM1 + 1, 0x00); // divisor alto  = 0
    outb(COM1 + 3, 0x03); // 8 bits, sem paridade, 1 stop
    outb(COM1 + 2, 0xC7); // habilita/limpa FIFO
    outb(COM1 + 4, 0x0B); // RTS/DSR ligados
}

static int tx_ready(void) { return inb(COM1 + 5) & 0x20; }

void serial_putc(char c) {
    while (!tx_ready()) { }
    outb(COM1, (uint8_t)c);
}

int serial_has_byte(void) { return inb(COM1 + 5) & 0x01; }

char serial_getc(void) {
    while (!serial_has_byte()) { }
    return (char)inb(COM1);
}
