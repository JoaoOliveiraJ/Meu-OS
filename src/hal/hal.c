#include <stdint.h>
#include "hal.h"
#include "io.h"
#include "mm/paging.h"   // Pilar 1: mm_map_phys_range / mm_unmap_range / arena

// A serial e o canal de log do kernel (kputc -> VGA texto + COM1). Declaradas
// em kernel.c; aqui as usamos para comprovar cada operacao da HAL em headless.
extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// ============================================================================
//  1) Portas de I/O — wrappers da HAL (ms_abi) sobre as primitivas in/out.
//     O x86 le/escreve 8/16/32 bits em portas; usamos inline asm igual ao
//     resto do kernel (src/include/io.h so tem 8 bits, entao definimos 16/32).
// ============================================================================

static inline uint16_t inw(uint16_t port) {
    uint16_t r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

MS_ABI uint8_t  HalReadPortUchar(uint16_t port)  { return inb(port); }
MS_ABI uint16_t HalReadPortUshort(uint16_t port) { return inw(port); }
MS_ABI uint32_t HalReadPortUlong(uint16_t port)  { return inl(port); }
MS_ABI void HalWritePortUchar(uint16_t port, uint8_t value)   { outb(port, value); }
MS_ABI void HalWritePortUshort(uint16_t port, uint16_t value) { outw(port, value); }
MS_ABI void HalWritePortUlong(uint16_t port, uint32_t value)  { outl(port, value); }

// ============================================================================
//  2) MMIO — acesso a memoria mapeada de dispositivo (Pilar 1 da rodada NT
//     foundation: paginacao dinamica).
//
//  Identidade de 1 GiB cobre [0, 0x40000000): faixas dentro dela viram acesso
//  direto (zero PTE novo, custo zero). Faixas ACIMA de 1 GiB sao mapeadas em
//  VA do arena MMIO (PML4[450]) com flags PCD+PWT (cache-disable e write-thru
//  — semantica do MmMapIoSpace(MmNonCached) do NT, obrigatorio para registros
//  do APIC / IO-APIC e qualquer BAR de dispositivo: cache nao pode reordenar
//  ou bufferizar as escritas).
// ============================================================================

#define HAL_IDENTITY_LIMIT 0x40000000ULL   // 1 GiB de identidade montada no boot

volatile void* hal_map_mmio(uint64_t phys, uint64_t size) {
    if (size == 0) return 0;

    // Faixa toda dentro da identidade: acesso direto, sem PTE novo.
    if (phys + size <= HAL_IDENTITY_LIMIT) {
        return (volatile void*)(uintptr_t)phys;
    }

    // Faixa acima da identidade: aloca VA no arena e mapeia phys -> VA
    // com PCD|PWT (cache disable / write-through) — mandatorio em MMIO.
    // Preserva o offset intra-pagina (BARs costumam comecar alinhadas mas
    // o registro lido pode estar em meio de pagina).
    uint64_t aligned_phys = phys & ~0xFFFULL;
    uint64_t intra        = phys & 0xFFFULL;
    uint64_t span         = (intra + size + 0xFFFULL) & ~0xFFFULL;

    uint64_t va = mm_mmio_reserve(span);
    if (!va) {
        kputs("[hal] MMIO map: arena cheio (phys="); kput_hex(phys); kputs(")\n");
        return 0;
    }
    uint64_t flags = MM_FLAG_PRESENT | MM_FLAG_RW | MM_FLAG_PCD | MM_FLAG_PWT;
    if (!mm_map_phys_range(va, aligned_phys, span, flags)) {
        kputs("[hal] MMIO map: mm_map_phys_range FALHOU phys="); kput_hex(phys); kputs("\n");
        return 0;
    }

    volatile void* ptr = (volatile void*)(uintptr_t)(va + intra);
    kputs("[hal] MMIO map phys="); kput_hex(phys);
    kputs(" -> virt="); kput_hex((uint64_t)(uintptr_t)ptr);
    kputs(" size="); kput_hex(size); kputs(" (PCD|PWT)\n");
    return ptr;
}

void hal_unmap_mmio(volatile void* va, uint64_t size) {
    if (!va || size == 0) return;
    uint64_t v = (uint64_t)(uintptr_t)va;
    // Identity range nao tem PTE proprio para desfazer: NOP.
    if (v < HAL_IDENTITY_LIMIT) return;

    uint64_t intra = v & 0xFFFULL;
    uint64_t span  = (intra + size + 0xFFFULL) & ~0xFFFULL;
    uint64_t base  = v & ~0xFFFULL;
    if (!mm_unmap_range(base, span)) {
        kputs("[hal] MMIO unmap FALHOU va="); kput_hex(v); kputs("\n");
        return;
    }
    kputs("[hal] MMIO unmap va="); kput_hex(v);
    kputs(" size="); kput_hex(size); kputs("\n");
}

MS_ABI uint8_t  HalReadMmioUchar(volatile void* a)  { return *(volatile uint8_t*)a; }
MS_ABI uint16_t HalReadMmioUshort(volatile void* a) { return *(volatile uint16_t*)a; }
MS_ABI uint32_t HalReadMmioUlong(volatile void* a)  { return *(volatile uint32_t*)a; }
MS_ABI void HalWriteMmioUchar(volatile void* a, uint8_t v)  { *(volatile uint8_t*)a = v; }
MS_ABI void HalWriteMmioUshort(volatile void* a, uint16_t v){ *(volatile uint16_t*)a = v; }
MS_ABI void HalWriteMmioUlong(volatile void* a, uint32_t v) { *(volatile uint32_t*)a = v; }

// ============================================================================
//  3) PCI — enumeracao via mecanismo de configuracao #1 (0xCF8/0xCFC).
//
//  Endereco de configuracao (escrito em 0xCF8):
//    bit 31    = enable
//    bits 30-24= reservado (0)
//    bits 23-16= bus
//    bits 15-11= device (slot)
//    bits 10-8 = function
//    bits 7-2  = offset do registrador (alinhado a dword)
//    bits 1-0  = 0
//  Depois le/escreve o dword em 0xCFC.
// ============================================================================

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (uint32_t)((1u << 31)
                    | ((uint32_t)bus  << 16)
                    | ((uint32_t)(dev  & 0x1F) << 11)
                    | ((uint32_t)(func & 0x07) << 8)
                    | ((uint32_t)offset & 0xFC));
}

MS_ABI uint32_t HalPciReadConfigUlong(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

MS_ABI void HalPciWriteConfigUlong(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

MS_ABI uint16_t HalPciReadConfigUshort(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t v = HalPciReadConfigUlong(bus, dev, func, offset);
    return (uint16_t)(v >> ((offset & 2) * 8));
}

MS_ABI uint8_t HalPciReadConfigUchar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t v = HalPciReadConfigUlong(bus, dev, func, offset);
    return (uint8_t)(v >> ((offset & 3) * 8));
}

// --- tabela de dispositivos achados ---------------------------------------
static hal_pci_device_t s_pci[HAL_PCI_MAX_DEVICES];
static int s_pci_count = 0;

int                     hal_pci_count(void)        { return s_pci_count; }
const hal_pci_device_t* hal_pci_get(int index) {
    if (index < 0 || index >= s_pci_count) return 0;
    return &s_pci[index];
}

const hal_pci_device_t* hal_pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < s_pci_count; i++) {
        if (s_pci[i].class_code == class_code &&
            (subclass == 0xFF || s_pci[i].subclass == subclass)) {
            return &s_pci[i];
        }
    }
    return 0;
}

// Nome legivel da classe base do PCI (apenas as que interessam ao boot).
static const char* pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00: return "Dispositivo legado (pre-classe)";
        case 0x01:
            switch (subclass) {
                case 0x01: return "Mass Storage / IDE (ATA)";
                case 0x06: return "Mass Storage / SATA (AHCI)";
                default:   return "Mass Storage";
            }
        case 0x02: return "Controlador de rede";
        case 0x03: return "Controlador de video (Display)";
        case 0x04: return "Multimidia";
        case 0x05: return "Controlador de memoria";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Bridge / Host (CPU->PCI)";
                case 0x01: return "Bridge / ISA";
                case 0x04: return "Bridge / PCI-to-PCI";
                default:   return "Bridge";
            }
        case 0x07: return "Comunicacao (serial/paralela)";
        case 0x08: return "Periferico de sistema (PIC/DMA/timer)";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "Serial Bus / USB";
                default:   return "Serial Bus";
            }
        default:   return "Outro";
    }
}

// Le os campos basicos de uma funcao PCI e a guarda na tabela; loga na serial.
static void pci_record_function(uint8_t bus, uint8_t dev, uint8_t func, uint32_t vend_dev) {
    uint16_t vendor = (uint16_t)(vend_dev & 0xFFFF);
    uint16_t device = (uint16_t)(vend_dev >> 16);

    // offset 0x08: revision(7..0), prog-if(15..8), subclass(23..16), class(31..24)
    uint32_t cls = HalPciReadConfigUlong(bus, dev, func, 0x08);
    uint8_t revision = (uint8_t)(cls & 0xFF);
    uint8_t prog_if  = (uint8_t)((cls >> 8) & 0xFF);
    uint8_t subclass = (uint8_t)((cls >> 16) & 0xFF);
    uint8_t classc   = (uint8_t)((cls >> 24) & 0xFF);

    // offset 0x0C bits 23..16: header type (bit 7 = multifuncao)
    uint8_t header_type = HalPciReadConfigUchar(bus, dev, func, 0x0E);

    hal_pci_device_t* d = 0;
    if (s_pci_count < HAL_PCI_MAX_DEVICES) d = &s_pci[s_pci_count++];

    if (d) {
        d->bus = bus; d->device = dev; d->function = func;
        d->vendor_id = vendor; d->device_id = device;
        d->class_code = classc; d->subclass = subclass;
        d->prog_if = prog_if; d->revision = revision;
        d->header_type = header_type;
    }

    // BARs: so existem no header type 0 (dispositivo) — 6 BARs em 0x10..0x24.
    // (header type 1 = PCI-PCI bridge tem 2 BARs; aqui lemos so se for tipo 0.)
    int nbars = ((header_type & 0x7F) == 0x00) ? 6 : 0;
    for (int i = 0; i < 6; i++) {
        uint32_t bar = 0;
        if (i < nbars) bar = HalPciReadConfigUlong(bus, dev, func, (uint8_t)(0x10 + i * 4));
        if (d) d->bar[i] = bar;
    }

    // --- log (regra 4) ---
    kputs("[hal] PCI: ");
    kput_dec(bus); kputc(':'); kput_dec(dev); kputc('.'); kput_dec(func);
    kputs("  vendor="); kput_hex(vendor);
    kputs(" device="); kput_hex(device);
    kputs(" class="); kput_hex(classc);
    kputs(" sub="); kput_hex(subclass);
    kputs("  ("); kputs(pci_class_name(classc, subclass)); kputs(")\n");

    // Loga as BARs nao-nulas (mostra se e I/O ou MMIO e a base).
    for (int i = 0; i < nbars; i++) {
        uint32_t bar = d ? d->bar[i] : 0;
        if (bar == 0) continue;
        kputs("[hal]   BAR"); kput_dec(i); kputs("=");
        if (bar & 1) {                                  // bit0=1 -> espaco de I/O
            kputs("I/O  porta="); kput_hex(bar & 0xFFFFFFFCu);
        } else {                                        // bit0=0 -> espaco de memoria (MMIO)
            kputs("MMIO base="); kput_hex(bar & 0xFFFFFFF0u);
            uint8_t type = (uint8_t)((bar >> 1) & 3);
            if (type == 2) kputs(" (64-bit)");
            if (bar & 8)   kputs(" prefetch");
        }
        kputc('\n');
    }
}

int hal_pci_enumerate(void) {
    s_pci_count = 0;
    kputs("[hal] enumerando PCI (mecanismo #1, portas 0xCF8/0xCFC)...\n");

    for (int bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (int dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            // Funcao 0: se vendor==0xFFFF, o slot esta vazio (pula).
            uint32_t vd0 = HalPciReadConfigUlong((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((vd0 & 0xFFFF) == 0xFFFF) continue;

            pci_record_function((uint8_t)bus, (uint8_t)dev, 0, vd0);

            // Se for multifuncao (header type bit 7), varre func 1..7.
            uint8_t htype = HalPciReadConfigUchar((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
            if (htype & 0x80) {
                for (int func = 1; func < PCI_MAX_FUNCTION; func++) {
                    uint32_t vd = HalPciReadConfigUlong((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x00);
                    if ((vd & 0xFFFF) == 0xFFFF) continue;
                    pci_record_function((uint8_t)bus, (uint8_t)dev, (uint8_t)func, vd);
                }
            }

            if (s_pci_count >= HAL_PCI_MAX_DEVICES) {
                kputs("[hal] tabela PCI cheia; parando a enumeracao.\n");
                return s_pci_count;
            }
        }
    }
    return s_pci_count;
}

// ============================================================================
//  4) Inicializacao da HAL (chamada no boot por kmain).
// ============================================================================
int hal_init(void) {
    kputs("\n--- HAL (Hardware Abstraction Layer) ---\n");
    kputs("[hal] hal_init(): I/O ports + MMIO + enumeracao PCI.\n");

    int n = hal_pci_enumerate();

    kputs("[hal] PCI: "); kput_dec(n); kputs(" dispositivo(s) encontrado(s).\n");

    // Destaca os dispositivos-chave para o resto do projeto (disco, video).
    const hal_pci_device_t* ide = hal_pci_find_class(0x01, 0xFF);   // mass storage
    if (ide) {
        kputs("[hal] controlador de armazenamento: vendor=");
        kput_hex(ide->vendor_id); kputs(" device="); kput_hex(ide->device_id);
        kputs(" subclass="); kput_hex(ide->subclass);
        kputs(ide->subclass == 0x01 ? " (IDE/ATA — portas 0x1F0-0x1F7)\n" : "\n");
    } else {
        kputs("[hal] nenhum controlador de armazenamento PCI (IDE legado em 0x1F0 mesmo assim).\n");
    }

    const hal_pci_device_t* vga = hal_pci_find_class(0x03, 0xFF);   // display
    if (vga) {
        kputs("[hal] controlador de video: vendor=");
        kput_hex(vga->vendor_id); kputs(" device="); kput_hex(vga->device_id);
        kputs("\n");
    }

    const hal_pci_device_t* host = hal_pci_find_class(0x06, 0x00);  // host bridge
    if (host) {
        kputs("[hal] host bridge (CPU->PCI): vendor=");
        kput_hex(host->vendor_id); kputs(" device="); kput_hex(host->device_id);
        kputs("\n");
    }

    kputs("[hal] HAL pronta.\n");
    return 1;
}
