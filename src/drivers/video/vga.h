#pragma once

// Driver de video em modo texto VGA (80x25 @ 0xB8000).
void vga_init(void);
void vga_putc(char c);
void vga_set_color(unsigned char fg, unsigned char bg);
