#pragma once
#include <stdint.h>

// Timer programavel (PIT 8254). Gera IRQ0 na frequencia pedida (Hz).
void pit_init(uint32_t hz);
