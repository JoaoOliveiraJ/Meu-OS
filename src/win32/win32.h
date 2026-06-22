#pragma once
#include <stdint.h>

// Resolve um import (por nome) de um .exe Windows para a nossa implementacao.
void* win32_resolve(const char* dll, const char* fn);
void* win32_resolve_ordinal(const char* dll, uint16_t ordinal);

// Buffer de retorno usado por ExitProcess para voltar ao kernel (setjmp).
extern void* g_pe_exit[5];
