#pragma once
#include <stdint.h>

// ============================================================================
//  disk.h  —  Driver de disco IDE (ATA) por PIO, LBA28 (FASE 2 da HAL).
//
//  O QEMU expoe um disco IDE primario master nas portas 0x1F0-0x1F7 (canal
//  primario) + 0x3F6 (controle). Aqui falamos ATA "a moda antiga" (PIO, sem
//  DMA/bus-mastering): IDENTIFY para descobrir o disco e READ/WRITE SECTORS
//  (comandos 0x20/0x30) movendo 256 words (512 bytes) por setor pela porta de
//  dados (0x1F0).
//
//  ABI: as funcoes Hal* sao __attribute__((ms_abi)) como o resto da HAL (imitam
//  a HAL.DLL do NT). Setor = 512 bytes. LBA28 cobre ate 128 GiB (suficiente para
//  a imagem de teste de 64 MiB).
//
//  Regra 4 do projeto: cada setor lido/escrito e logado na serial ([disk] ...).
// ============================================================================

#ifndef MS_ABI
#define MS_ABI __attribute__((ms_abi))
#endif

#define HAL_SECTOR_SIZE 512

// Portas do canal IDE primario (master).
#define ATA_PRIMARY_IO    0x1F0   // base de I/O (registradores 0x1F0..0x1F7)
#define ATA_PRIMARY_CTRL  0x3F6   // registrador de controle/alt-status

// Inicializa/identifica o disco IDE primario master (comando IDENTIFY 0xEC).
// Loga o modelo, o numero de setores LBA28 e a capacidade. Retorna 1 se um
// disco respondeu (presente), 0 caso contrario (sem disco anexado: -kernel sem
// -Disk). Idempotente: chamar mais de uma vez nao quebra nada.
int hal_disk_init(void);

// 1 se hal_disk_init detectou um disco presente.
int hal_disk_present(void);

// Numero de setores LBA28 reportado pelo IDENTIFY (0 se nao houver disco).
uint32_t hal_disk_sector_count(void);

// Le um setor (512 bytes) do LBA dado para 'buf'. Retorna 0 em sucesso, !=0 em
// erro (sem disco, timeout, ou bit ERR no status). Loga a operacao na serial.
MS_ABI int HalReadSector(uint32_t lba, void* buf);

// Escreve um setor (512 bytes) de 'buf' no LBA dado. Retorna 0 em sucesso.
// Faz CACHE FLUSH (0xE7) apos a escrita. Loga a operacao na serial.
MS_ABI int HalWriteSector(uint32_t lba, const void* buf);
