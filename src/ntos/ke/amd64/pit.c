#include "ke/amd64/pit.h"
#include "io.h"

void pit_init(uint32_t hz) {
    if (hz == 0) hz = 100;
    uint32_t divisor = 1193182u / hz;   // clock base do PIT

    outb(0x43, 0x36);                   // canal 0, lobyte/hibyte, modo 3 (onda quadrada)
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}
