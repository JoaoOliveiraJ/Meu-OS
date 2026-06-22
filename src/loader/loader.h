#pragma once

// Loader estilo NT (LdrLoadDll): registra os modulos do boot por nome e
// resolve imports carregando DLLs recursivamente contra suas export tables.

void  ldr_register(const char* path, const void* image);   // registra bytes de um modulo
void* ldr_load(const char* name);                          // carrega DLL (recursivo) -> base
void  ldr_run(const char* path, const void* image);        // carrega e roda um .exe em ring 3
int   ldr_match_ext(const char* path, const char* ext);    // path termina com 'ext'? (sem case)

// Bytes brutos de um modulo registrado, pelo nome (ex.: "mydriver.sys"). 0 se nao
// houver. Usado pelo I/O Manager para carregar um driver .sys pelo nome (sc start).
const void* ldr_get_module_bytes(const char* name);
