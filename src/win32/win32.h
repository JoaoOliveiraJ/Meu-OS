#pragma once

// Servicos Win32 do lado KERNEL (ring 0), chamados pelo despacho de syscalls.
// No NT seria a parte de kernel do subsistema Win32 (win32k.sys).
void win32k_messagebox(const char* text, const char* caption);
