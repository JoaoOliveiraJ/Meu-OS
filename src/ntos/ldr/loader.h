#pragma once

// Loader estilo NT (LdrLoadDll): registra os modulos do boot por nome e
// resolve imports carregando DLLs recursivamente contra suas export tables.

void  ldr_register(const char* path, const void* image);   // registra bytes de um modulo
void* ldr_load(const char* name);                          // carrega DLL (recursivo) -> base
void* ldr_load_runtime(const char* name);                  // LoadLibrary em runtime: apiset_redirect + anexa ".dll"
void* ldr_find_runtime(const char* name);                  // GetModuleHandle: base ja carregada (nao carrega) + redirect
void  ldr_run(const char* path, const void* image);        // carrega e roda um .exe em ring 3
int   ldr_match_ext(const char* path, const char* ext);    // path termina com 'ext'? (sem case)

// Bytes brutos de um modulo registrado, pelo nome (ex.: "mydriver.sys"). 0 se nao
// houver. Usado pelo I/O Manager para carregar um driver .sys pelo nome (sc start).
const void* ldr_get_module_bytes(const char* name);

// ----------------------------------------------------------------------------
//  Enumeracao de modulos carregados (GATE 2 do pintok.sys / pintok.sys).
//
//  ZwQuerySystemInformation(SystemModuleInformation/Ex) precisa devolver a lista
//  de modulos do kernel com base/tamanho/nome REAIS. Estes helpers expoem o
//  s_mods[] do loader de forma read-only para o ntoskrnl.c montar o blob
//  RTL_PROCESS_MODULE_INFORMATION_EX[].
//
//  IMPORTANTE: ldr_module_set_base() deve ser chamado pelo I/O Manager logo
//  apos pe_map/pe_map_at de um .sys (o driver_load nao passa pelo ldr_load, que
//  e quem normalmente preenche s_mods[].base). Sem base != 0, o modulo e
//  reportado mas com ImageBase = ponteiro dos bytes brutos (fallback).
// ----------------------------------------------------------------------------

// Numero de modulos registrados (0..MAX_MODULES).
int ldr_get_module_count(void);

// Informacoes do modulo de indice 'i' (0-based). Retorna 1 se valido, 0 senao.
//   *out_base  = base de carga (VA) — 0 se ainda nao mapeado;
//   *out_size  = SizeOfImage (lido do PE header), 0 se nao parseavel;
//   *out_name  = nome curto (basename, ex.: "pintok.sys") — ponteiro interno estavel.
int ldr_get_module_info(int i, uint64_t* out_base, uint32_t* out_size, const char** out_name);

// Registra a base de carga (VA) de um modulo ja mapeado, pelo nome. Usado pelo
// I/O Manager apos carregar um .sys (que nao passa pelo ldr_load).
void ldr_module_set_base(const char* name, void* base);
