#pragma once

// Resolve um import (DLL, funcao) -> ponteiro da nossa implementacao.
typedef void* (*pe_resolver_t)(const char* dll, const char* fn);

// Carrega um PE32+ no seu ImageBase, resolve imports via 'resolve' e
// devolve a base; *entry_out recebe o endereco do entry point.
void* pe_load(const void* image, pe_resolver_t resolve, void** entry_out);

// Carrega e executa como um .exe Win32 (entry void(void), ABI ms).
void  pe_run(const void* image);

// Subsystem do PE: 1=NATIVE(driver .sys), 2=GUI, 3=console (.exe). -1 se invalido.
int   pe_subsystem(const void* image);
