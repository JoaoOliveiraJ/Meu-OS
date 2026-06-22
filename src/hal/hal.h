#pragma once
#include <stdint.h>

// ============================================================================
//  hal.h  —  Hardware Abstraction Layer (estilo HAL.DLL do Windows NT).
//
//  Camada que isola o resto do kernel do acesso direto ao hardware:
//    - Portas de I/O (HalReadPort*/HalWritePort*) — wrappers ms_abi sobre in/out.
//    - Acesso MMIO (mapear uma faixa fisica e ler/escrever 8/16/32 bits).
//    - Enumeracao PCI: percorre bus/device/function lendo o config space pelo
//      mecanismo #1 (porta 0xCF8 = address, 0xCFC = data), lista vendor/device/
//      class de cada dispositivo e le as BARs.
//
//  Convencao de ABI: as funcoes Hal* sao __attribute__((ms_abi)) porque imitam
//  a HAL do NT (chamada por codigo que segue a ABI Microsoft). As funcoes
//  internas/de boot (hal_init, hal_pci_enumerate) usam a ABI do kernel (SysV).
//
//  Regra 4 do projeto: cada dispositivo PCI achado e logado na serial
//  ([hal] PCI: ...) para comprovar a enumeracao em modo headless.
// ============================================================================

#ifndef MS_ABI
#define MS_ABI __attribute__((ms_abi))
#endif

// ---------------------------------------------------------------------------
//  Acesso a portas de I/O (nomes no estilo NT HAL). Sao ms_abi para casar com
//  a HAL do Windows; internamente fazem in/out (uso interno: include "io.h").
// ---------------------------------------------------------------------------
MS_ABI uint8_t  HalReadPortUchar(uint16_t port);
MS_ABI uint16_t HalReadPortUshort(uint16_t port);
MS_ABI uint32_t HalReadPortUlong(uint16_t port);
MS_ABI void     HalWritePortUchar(uint16_t port, uint8_t value);
MS_ABI void     HalWritePortUshort(uint16_t port, uint16_t value);
MS_ABI void     HalWritePortUlong(uint16_t port, uint32_t value);

// ---------------------------------------------------------------------------
//  Acesso MMIO. A identidade de 1 GiB ja cobre [0, 0x40000000); para faixas
//  fisicas dentro dela, hal_map_mmio devolve o proprio ponteiro identidade
//  (acesso direto). Para faixas acima de 1 GiB nao mapeadas, devolve 0 (o
//  chamador deve tratar — nao mexemos nas page tables aqui, caminho seguro).
// ---------------------------------------------------------------------------
volatile void* hal_map_mmio(uint64_t phys, uint64_t size);

MS_ABI uint8_t  HalReadMmioUchar(volatile void* addr);
MS_ABI uint16_t HalReadMmioUshort(volatile void* addr);
MS_ABI uint32_t HalReadMmioUlong(volatile void* addr);
MS_ABI void     HalWriteMmioUchar(volatile void* addr, uint8_t value);
MS_ABI void     HalWriteMmioUshort(volatile void* addr, uint16_t value);
MS_ABI void     HalWriteMmioUlong(volatile void* addr, uint32_t value);

// ---------------------------------------------------------------------------
//  PCI — mecanismo de configuracao #1 (portas 0xCF8/0xCFC).
// ---------------------------------------------------------------------------
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_MAX_BUS        256
#define PCI_MAX_DEVICE     32
#define PCI_MAX_FUNCTION   8

// Le/escreve um dword (32 bits) do config space de um (bus,dev,func) no offset.
MS_ABI uint32_t HalPciReadConfigUlong(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
MS_ABI void     HalPciWriteConfigUlong(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);
MS_ABI uint16_t HalPciReadConfigUshort(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
MS_ABI uint8_t  HalPciReadConfigUchar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

// Descritor de um dispositivo PCI achado na enumeracao.
typedef struct hal_pci_device {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;       // classe base (0x01 = mass storage, 0x03 = display, ...)
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint32_t bar[6];           // Base Address Registers (raw)
} hal_pci_device_t;

// Numero de dispositivos guardados na tabela apos hal_pci_enumerate.
#define HAL_PCI_MAX_DEVICES 64

// Percorre todo o espaco PCI, preenche a tabela interna e loga cada dispositivo.
// Retorna a quantidade de dispositivos encontrados.
int hal_pci_enumerate(void);

// Acesso a tabela de dispositivos achados (apos hal_pci_enumerate).
int                     hal_pci_count(void);
const hal_pci_device_t* hal_pci_get(int index);

// Procura o 1o dispositivo de uma dada classe/subclasse (subclass=0xFF = qualquer).
// Retorna NULL se nao houver. Util para localizar o controlador IDE, a video, etc.
const hal_pci_device_t* hal_pci_find_class(uint8_t class_code, uint8_t subclass);

// Inicializa a HAL no boot: identifica a HAL e dispara a enumeracao PCI,
// logando os dispositivos achados na serial. Retorna 1 em sucesso.
int hal_init(void);
