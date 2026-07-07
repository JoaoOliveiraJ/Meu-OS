#pragma once
#include <stdint.h>

// Primitivas de PE usadas pelo loader (loader/loader.c).
// Suportam PE32+ (x86-64, magic 0x20B, machine 0x8664) E PE32 (x86, magic
// 0x10B, machine 0x14C): o parsing le os campos independente da bitness.
typedef void* (*pe_resolver_t)(const char* dll, const char* fn);

// Campos do cabecalho PE, lidos de forma independente da bitness.
typedef struct {
    uint16_t machine;       // 0x8664 (x64) | 0x14C (x86)
    uint16_t magic;         // 0x20B (PE32+) | 0x10B (PE32)
    int      is64;          // 1 = PE32+ (64-bit); 0 = PE32 (32-bit)
    uint16_t nsec;          // numero de secoes
    uint32_t entry_rva;     // AddressOfEntryPoint
    uint64_t preferred;     // ImageBase preferido
    uint32_t size_image;    // SizeOfImage
    uint32_t size_hdrs;     // SizeOfHeaders
    uint32_t ndirs;         // NumberOfRvaAndSizes
    uint32_t dir_off;       // offset do array de data directories no optional header
    uint32_t sec_off;       // offset (no arquivo) das section headers
} pe_info_t;

// Le os campos do PE para 'pi'. Retorna 1 se for um PE valido (32 ou 64), 0 senao.
int   pe_parse(const void* image, pe_info_t* pi);

// Subsystem do PE: 1=NATIVE(.sys), 2=GUI, 3=console(.exe). -1 se invalido.
int   pe_subsystem(const void* image);

// Mapeia as secoes do PE no seu ImageBase preferido; *entry_out = entry point.
void* pe_map(const void* image, void** entry_out);

// Mapeia as secoes do PE numa base ARBITRARIA (para relocacoes); *entry_out =
// entry point ja relativo a essa base. Retorna a base usada.
void* pe_map_at(const void* image, uint64_t base, void** entry_out);

// Aplica base relocations (.reloc) se a base usada difere da 'preferred'.
// Trata HIGHLOW (32-bit) e DIR64 (64-bit). Retorna o numero de fixups.
uint32_t pe_relocate(void* base, uint64_t preferred);

// Resolve a tabela de imports (IAT) usando o 'resolve'. Trata thunks de 8 bytes
// (PE32+) e de 4 bytes (PE32).
void  pe_bind_imports(void* base, pe_resolver_t resolve);

// Procura um export por nome na tabela de exports de uma imagem ja mapeada.
void* pe_get_export(void* base, const char* name);
// Procura um export por ORDINAL (imports #N — ex.: shell32 via api-ms-win-shell-*).
void* pe_get_export_by_ordinal(void* base, uint32_t ordinal);
