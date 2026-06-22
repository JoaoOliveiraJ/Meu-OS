#pragma once

// Driver da porta serial COM1 (otimo para depurar no QEMU com -serial stdio).
void serial_init(void);
void serial_putc(char c);
int  serial_has_byte(void);
char serial_getc(void);
