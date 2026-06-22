// ============================================================================
//  bochsvbe.c — Driver Bochs/QEMU std-vga (BGA - Bochs Graphics Adapter).
//
//  Etapas:
//   1) Procura via PCI o dispositivo vendor=0x1234 device=0x1111 (classe 0x03).
//   2) Le BAR0 (offset 0x10 do config space) — base FISICA do Linear FrameBuffer.
//   3) Calcula tamanho do LFB pelo modo desejado (W * H * bytes_per_pixel) e
//      mapeia em virt via mm_map_phys_range. Como BAR0 (~0xFD000000 no QEMU)
//      esta FORA da identidade de 1 GiB, usamos um virt acima dela e o mapeamos
//      sob demanda. Estrategia simples: virt = 0xFFFF800000000000ULL + phys_low
//      (faixa canonica alta, padronizada para MMIO em todos os drivers).
//   4) Habilita o BAR0 no command register (bit 1 = Memory Space).
//   5) Valida via porta de I/O: outw(INDEX, ID); inw(DATA) == 0xB0C5.
//   6) Sequencia oficial de programacao do modo:
//        disable -> set XRES/YRES/BPP -> enable | LFB_ENABLED
//   7) Loga tudo na serial ([bvbe] ...) p/ headless.
//
//  Sem assumir BIOS/VBE int10 (estamos em long mode). Tudo via porta + MMIO.
// ============================================================================
#include <stdint.h>
#include "BasicDisplay.h"
#include "hal/hal.h"
#include "mm/paging.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- inline I/O 16-bit (o io.h global so tem 8 bits; o resto da HAL tambem
//     usa static inline locais). Mantemos a mesma convencao aqui.
static inline void out16(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t in16(uint16_t port) {
    uint16_t r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

// --- estado do driver ----------------------------------------------------
static int      s_active   = 0;
static uint8_t* s_lfb_virt = 0;   // ponteiro para a VRAM mapeada
static uint64_t s_lfb_phys = 0;
static uint64_t s_lfb_size = 0;   // bytes mapeados
static uint32_t s_width    = 0;
static uint32_t s_height   = 0;
static uint32_t s_bpp      = 0;   // 32, 24, 16, 8
static uint32_t s_pitch    = 0;

// --- helpers VBE ---------------------------------------------------------
static void vbe_write(uint16_t index, uint16_t value) {
    out16(VBE_DISPI_IOPORT_INDEX, index);
    out16(VBE_DISPI_IOPORT_DATA, value);
}
static uint16_t vbe_read(uint16_t index) {
    out16(VBE_DISPI_IOPORT_INDEX, index);
    return in16(VBE_DISPI_IOPORT_DATA);
}

// Procura o PCI 1234:1111 entre os dispositivos enumerados.
static const hal_pci_device_t* find_bochs_vga(void) {
    int n = hal_pci_count();
    for (int i = 0; i < n; i++) {
        const hal_pci_device_t* d = hal_pci_get(i);
        if (d && d->vendor_id == VBE_PCI_VENDOR &&
                 d->device_id == VBE_PCI_DEVICE) {
            return d;
        }
    }
    return 0;
}

// Habilita Memory Space + Bus Master no command register (offset 0x04 do PCI
// config space, low 16 bits). Bochs-VGA do QEMU as vezes vem com Memory off.
static void enable_bar(const hal_pci_device_t* d) {
    uint32_t cmd = HalPciReadConfigUlong(d->bus, d->device, d->function, 0x04);
    uint32_t want = cmd | 0x6;   // bit1=Memory Space, bit2=Bus Master
    if (want != cmd) {
        HalPciWriteConfigUlong(d->bus, d->device, d->function, 0x04, want);
        kputs("[bvbe] PCI Command 0x04: 0x");
        kput_hex(cmd); kputs(" -> 0x"); kput_hex(want); kputs(" (Memory+BM)\n");
    }
}

int bochsvbe_init(uint32_t width, uint32_t height, uint32_t bpp) {
    if (s_active) return 1;

    // 1) PCI scan ja foi feito por hal_init; pegamos o registro.
    const hal_pci_device_t* d = find_bochs_vga();
    if (!d) {
        kputs("[bvbe] PCI 1234:1111 (Bochs VGA) NAO encontrado; abortando.\n");
        return 0;
    }
    kputs("[bvbe] PCI achado: bus=");
    kput_dec(d->bus); kputc(':'); kput_dec(d->device); kputc('.');
    kput_dec(d->function);
    kputs("  vendor="); kput_hex(d->vendor_id);
    kputs(" device="); kput_hex(d->device_id);
    kputs(" class="); kput_hex(d->class_code);
    kputc('\n');

    // 2) BAR0 = base fisica do LFB. Bit 0 deve ser 0 (MMIO, nao I/O). Mascara
    //    os 4 bits baixos (type/prefetch) p/ obter o endereco.
    uint32_t bar0 = d->bar[0];
    if (bar0 & 0x1) {
        kputs("[bvbe] BAR0 e espaco de I/O (inesperado); abortando.\n");
        return 0;
    }
    uint64_t phys = (uint64_t)(bar0 & 0xFFFFFFF0u);
    if (!phys) {
        kputs("[bvbe] BAR0 == 0; firmware nao alocou. Abortando.\n");
        return 0;
    }

    // 3) Habilita memoria no command register (caminho safe — ja vem ligado no
    //    QEMU, mas e a regra defensiva pra outros emuladores/HW).
    enable_bar(d);

    // 4) Calcula bytes_per_pixel e tamanho da VRAM (alinha pra cima em 4 KiB).
    uint32_t bytes_per_pixel = (bpp + 7) / 8;
    uint64_t size = (uint64_t)width * (uint64_t)height * (uint64_t)bytes_per_pixel;
    if (size < 0x100000) size = 0x100000;   // 1 MiB minimo (alguns drivers checam)
    size = (size + 0xFFFULL) & ~0xFFFULL;   // alinha em 4 KiB

    // 5) Escolhe o virt: faixa canonica alta padrao do kernel para MMIO.
    //    Usamos 0xFFFFC00000000000ULL + (phys & 0xFFFFFFFF) — entrada PML4=384.
    //    Bem longe de 0xFFFFF780_... (KUSER_SHARED_DATA) e da identidade baixa.
    uint64_t virt = 0xFFFFC00000000000ULL | (phys & 0xFFFFFFFFULL);

    kputs("[bvbe] BAR0 phys=0x"); kput_hex(phys);
    kputs("  mapeando "); kput_dec(size / 1024); kputs(" KiB em virt=0x");
    kput_hex(virt); kputs("\n");

    // Mapeia com PCD (cache disable) — VRAM e write-combining, mas sem PAT
    // o seguro e desligar cache. Funcionalmente OK p/ scan-out simples.
    uint64_t mflags = MM_FLAG_PRESENT | MM_FLAG_RW | MM_FLAG_PCD;
    if (!mm_map_phys_range(virt, phys, size, mflags)) {
        kputs("[bvbe] mm_map_phys_range FALHOU (sem RAM p/ page tables?). Abortando.\n");
        return 0;
    }

    s_lfb_phys = phys;
    s_lfb_virt = (uint8_t*)virt;
    s_lfb_size = size;

    // 6) Le o ID via porta — confirma a BGA presente.
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    kputs("[bvbe] VBE_DISPI_INDEX_ID = 0x"); kput_hex(id); kputs("\n");
    if (id < VBE_DISPI_ID0 || id > 0xB0CF) {
        kputs("[bvbe] ID fora do range BGA (esperado 0xB0C0..0xB0C5+). Abortando.\n");
        return 0;
    }

    // 7) Programacao do modo: disable, set, enable+LFB.
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vbe_write(VBE_DISPI_INDEX_XRES,   (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_YRES,   (uint16_t)height);
    vbe_write(VBE_DISPI_INDEX_BPP,    (uint16_t)bpp);
    vbe_write(VBE_DISPI_INDEX_BANK,   0);
    vbe_write(VBE_DISPI_INDEX_VIRT_W, (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_VIRT_H, (uint16_t)height);
    vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_8BIT_DAC);

    // Releitura defensiva — confirma que o modo "pegou".
    uint16_t rxres = vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t ryres = vbe_read(VBE_DISPI_INDEX_YRES);
    uint16_t rbpp  = vbe_read(VBE_DISPI_INDEX_BPP);
    uint16_t ren   = vbe_read(VBE_DISPI_INDEX_ENABLE);

    s_width  = rxres;
    s_height = ryres;
    s_bpp    = rbpp;
    s_pitch  = rxres * ((rbpp + 7) / 8);

    kputs("[bvbe] modo programado: ");
    kput_dec(s_width); kputc('x'); kput_dec(s_height);
    kputc('x'); kput_dec(s_bpp);
    kputs("  pitch="); kput_dec(s_pitch);
    kputs("  ENABLE=0x"); kput_hex(ren); kputs("\n");

    if (s_width != width || s_height != height || s_bpp != bpp) {
        kputs("[bvbe] AVISO: modo retornado nao bate com o pedido; seguindo assim mesmo.\n");
    }

    s_active = 1;
    kputs("[bvbe] driver ATIVO. LFB virt=0x"); kput_hex(virt);
    kputs(" phys=0x"); kput_hex(phys); kputs("\n");
    return 1;
}

uint8_t* bochsvbe_lfb(void)    { return s_lfb_virt; }
uint32_t bochsvbe_width(void)  { return s_width; }
uint32_t bochsvbe_height(void) { return s_height; }
uint32_t bochsvbe_pitch(void)  { return s_pitch; }
uint32_t bochsvbe_bpp(void)    { return s_bpp; }
int      bochsvbe_active(void) { return s_active; }
