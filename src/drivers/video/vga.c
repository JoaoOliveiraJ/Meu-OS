#include <stdint.h>
#include "vga.h"
#include "io.h"

#define VGA  ((volatile uint16_t*)0xB8000)
#define COLS 80
#define ROWS 25

static int     s_row, s_col;
static uint8_t s_color = 0x0F;   // branco sobre preto

static void put_at(char c, int x, int y) {
    VGA[y * COLS + x] = (uint16_t)(uint8_t)c | ((uint16_t)s_color << 8);
}

static void move_cursor(void) {
    uint16_t pos = (uint16_t)(s_row * COLS + s_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_color(unsigned char fg, unsigned char bg) {
    s_color = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void vga_init(void) {
    s_color = 0x0F;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            put_at(' ', x, y);
    s_row = s_col = 0;
    move_cursor();
}

static void scroll(void) {
    for (int y = 1; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            VGA[(y - 1) * COLS + x] = VGA[y * COLS + x];
    for (int x = 0; x < COLS; x++)
        put_at(' ', x, ROWS - 1);
    s_row = ROWS - 1;
}

void vga_putc(char c) {
    if (c == '\n') { s_col = 0; if (++s_row >= ROWS) scroll(); move_cursor(); return; }
    if (c == '\r') { s_col = 0; move_cursor(); return; }
    if (c == '\b') { if (s_col > 0) { s_col--; put_at(' ', s_col, s_row); } move_cursor(); return; }
    put_at(c, s_col, s_row);
    if (++s_col >= COLS) { s_col = 0; if (++s_row >= ROWS) scroll(); }
    move_cursor();
}
