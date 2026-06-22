#pragma once

// Resolve imports de drivers (.sys) contra a "ntoskrnl.exe" do MeuOS.
void* ntkrnl_resolve(const char* dll, const char* fn);
