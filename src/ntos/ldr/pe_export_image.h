#pragma once
#include <stdint.h>

// Constroi (uma vez) uma imagem PE32+ "ntoskrnl.exe" sintetica com uma EXPORT
// DIRECTORY valida, ordenada e completa, que pintok.sys parseia
// manualmente para resolver ~217 funcoes do ntoskrnl. Ver pe_export_image.c.

// Monta/retorna a imagem e o SizeOfImage. Idempotente. 0 em erro.
void*    ldr_pe_export_image_build(uint32_t* out_size);

// Base (ponteiro) e SizeOfImage prontos p/ o provedor de SystemModuleInformation.
void*    ldr_ntoskrnl_image_base(void);
uint32_t ldr_ntoskrnl_image_size(void);

// Registra a imagem como "ntoskrnl.exe" no loader. Chame no boot antes do pintok.sys.
void     ldr_register_ntoskrnl_export_image(void);
