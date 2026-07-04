#pragma once

// Resolve imports de drivers (.sys) contra a "ntoskrnl.exe" do MeuOS.
void* ntkrnl_resolve(const char* dll, const char* fn);

// Resolvedor multi-DLL (ciente do nome da DLL de origem: ntoskrnl.exe, hal.dll,
// CI.dll, cng.sys, ksecdd.sys, FLTMGR.SYS, ...). ntkrnl_resolve encaminha aqui.
void* driver_import_resolver(const char* dll, const char* fn);
