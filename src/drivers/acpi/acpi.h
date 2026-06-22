#pragma once
#include <stdint.h>

// ============================================================================
//  acpi.h — ACPI driver stub do MeuOS (FASE 13).
//
//  Espelha o acpi.sys do Windows. Em um stack real, parseia RSDP -> RSDT ->
//  FADT/DSDT/MADT/HPET e expoe APIs do ACPICA-like interpreter para AML.
//  No MeuOS, fazemos apenas a varredura da BIOS area procurando a
//  assinatura "RSD PTR " (8 bytes alinhados em 16). Sem AML, sem
//  enumeracao de tables.
//
//  Range canonico do RSDP em maquinas BIOS legacy:
//    0x000E0000 .. 0x000FFFFF  (BIOS ROM/EBDA shadow)
//
//  Em UEFI ele vem na config table do firmware (nao aplicavel aqui).
//
//  Logs em '[acpi] ...'.
// ============================================================================

// Inicializa o driver ACPI. Procura RSDP no range BIOS [0xE0000, 0xFFFFF].
// Idempotente. Retorna 1 se achou o RSDP, 0 caso contrario.
int acpi_init(void);

// Estado: 1 se RSDP foi achado.
int acpi_active(void);

// Endereco fisico do RSDP (0 se nao achado).
uint64_t acpi_rsdp_phys(void);

// Endereco fisico do RSDT (lido do RSDP). 0 se nao achado / nao parseado.
uint64_t acpi_rsdt_phys(void);
