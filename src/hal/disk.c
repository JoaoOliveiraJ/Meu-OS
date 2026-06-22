#include <stdint.h>
#include "disk.h"
#include "io.h"

// Serial = canal de log do kernel (kputc -> VGA texto + COM1).
extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// ============================================================================
//  Registradores ATA (offsets a partir de ATA_PRIMARY_IO = 0x1F0).
//    +0  Data (16 bits)           leitura/escrita do bloco de 256 words
//    +1  Error (R) / Features (W)
//    +2  Sector Count
//    +3  LBA low   (bits 0..7)
//    +4  LBA mid   (bits 8..15)
//    +5  LBA high  (bits 16..23)
//    +6  Drive/Head (bits 24..27 do LBA + bit LBA + master/slave)
//    +7  Status (R) / Command (W)
//  Controle: 0x3F6 = Alternate Status (R) / Device Control (W).
// ============================================================================
#define ATA_REG_DATA     (ATA_PRIMARY_IO + 0)
#define ATA_REG_ERROR    (ATA_PRIMARY_IO + 1)
#define ATA_REG_FEATURES (ATA_PRIMARY_IO + 1)
#define ATA_REG_SECCOUNT (ATA_PRIMARY_IO + 2)
#define ATA_REG_LBA0     (ATA_PRIMARY_IO + 3)
#define ATA_REG_LBA1     (ATA_PRIMARY_IO + 4)
#define ATA_REG_LBA2     (ATA_PRIMARY_IO + 5)
#define ATA_REG_DRIVE    (ATA_PRIMARY_IO + 6)
#define ATA_REG_STATUS   (ATA_PRIMARY_IO + 7)
#define ATA_REG_COMMAND  (ATA_PRIMARY_IO + 7)

// Bits do registrador de status.
#define ATA_SR_BSY  0x80   // Busy
#define ATA_SR_DRDY 0x40   // Drive ready
#define ATA_SR_DRQ  0x08   // Data request (pronto p/ transferir um word)
#define ATA_SR_ERR  0x01   // Error

// Comandos ATA.
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

// Bit 6 do registrador Drive seleciona modo LBA; bit 4 = master(0)/slave(1).
#define ATA_DRIVE_LBA_MASTER  0xE0   // 1110 0000: LBA + master + bits altos

// --- acesso a portas de 16 bits para o bloco de dados (PIO) -----------------
static inline uint16_t inw16(uint16_t port) {
    uint16_t r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outw16(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

// --- estado do driver -------------------------------------------------------
static int      s_present = 0;
static uint32_t s_sectors = 0;     // total de setores LBA28 (do IDENTIFY)

int      hal_disk_present(void)      { return s_present; }
uint32_t hal_disk_sector_count(void) { return s_sectors; }

// Le o Alternate Status 4x (~400 ns) para dar tempo ao drive trocar de estado,
// como manda o protocolo ATA (a leitura de 0x3F6 nao afeta o estado/IRQ).
static void ata_delay400ns(void) {
    for (int i = 0; i < 4; i++) (void)inb(ATA_PRIMARY_CTRL);
}

// Espera BSY cair (com timeout). Retorna 0 se BSY caiu, -1 se estourou.
static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < 4000000u; i++) {
        uint8_t st = inb(ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -1;
}

// Espera BSY=0 e DRQ=1 (pronto p/ transferir). Retorna 0 ok; -1 timeout; -2 ERR.
static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 4000000u; i++) {
        uint8_t st = inb(ATA_REG_STATUS);
        if (st & ATA_SR_ERR) return -2;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

// Programa o canal para um acesso LBA28 de 1 setor (seleciona drive + LBA +
// sector count=1). Usado por READ e WRITE.
static void ata_setup_lba(uint32_t lba) {
    outb(ATA_REG_DRIVE, (uint8_t)(ATA_DRIVE_LBA_MASTER | ((lba >> 24) & 0x0F)));
    outb(ATA_REG_FEATURES, 0x00);
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
}

// ============================================================================
//  IDENTIFY DEVICE (0xEC): descobre se ha disco e quantos setores LBA28 tem.
// ============================================================================
int hal_disk_init(void) {
    if (s_present) return 1;   // idempotente

    kputs("\n--- HAL disco (IDE ATA PIO, canal primario master 0x1F0) ---\n");

    // Seleciona o master e zera os registradores de endereco (protocolo IDENTIFY).
    outb(ATA_REG_DRIVE, ATA_DRIVE_LBA_MASTER);
    ata_delay400ns();
    outb(ATA_REG_SECCOUNT, 0);
    outb(ATA_REG_LBA0, 0);
    outb(ATA_REG_LBA1, 0);
    outb(ATA_REG_LBA2, 0);

    // Status 0xFF = barramento flutuante (sem disco): nada respondendo.
    uint8_t st = inb(ATA_REG_STATUS);
    if (st == 0xFF) {
        kputs("[disk] nenhum disco no canal primario (status=0xFF). "
              "Rode com -Disk para anexar build\\disk.img.\n");
        return 0;
    }

    outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay400ns();

    st = inb(ATA_REG_STATUS);
    if (st == 0) {
        kputs("[disk] IDENTIFY: status=0 -> sem disco anexado.\n");
        return 0;
    }

    if (ata_wait_not_busy() != 0) {
        kputs("[disk] IDENTIFY: timeout esperando BSY cair.\n");
        return 0;
    }

    // Se LBA1/LBA2 != 0 apos o IDENTIFY, e um dispositivo ATAPI/SATA (nao ATA):
    // nao tratamos aqui.
    uint8_t mid = inb(ATA_REG_LBA1), hi = inb(ATA_REG_LBA2);
    if (mid != 0 || hi != 0) {
        kputs("[disk] dispositivo nao-ATA no canal (ATAPI/SATA): mid=");
        kput_hex(mid); kputs(" hi="); kput_hex(hi); kputs(" -> ignorado.\n");
        return 0;
    }

    if (ata_wait_drq() != 0) {
        kputs("[disk] IDENTIFY: erro/timeout esperando DRQ.\n");
        return 0;
    }

    // Le os 256 words do bloco IDENTIFY.
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw16(ATA_REG_DATA);

    // Modelo: words 27..46, ASCII com bytes trocados dentro de cada word.
    char model[41];
    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        model[i * 2]     = (char)(w >> 8);
        model[i * 2 + 1] = (char)(w & 0xFF);
    }
    model[40] = 0;
    // Apara espacos a direita.
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    // Total de setores LBA28: words 60..61 (dword little-endian, low em 60).
    s_sectors = ((uint32_t)id[61] << 16) | id[60];
    s_present = 1;

    kputs("[disk] IDENTIFY OK. modelo='"); kputs(model); kputs("'\n");
    kputs("[disk] setores LBA28 = "); kput_dec(s_sectors);
    kputs(" ("); kput_dec((uint64_t)s_sectors * HAL_SECTOR_SIZE / (1024 * 1024));
    kputs(" MiB, setor de "); kput_dec(HAL_SECTOR_SIZE); kputs(" bytes)\n");
    return 1;
}

// ============================================================================
//  HalReadSector — le 1 setor (512 bytes) por PIO.
// ============================================================================
MS_ABI int HalReadSector(uint32_t lba, void* buf) {
    if (!s_present) {
        kputs("[disk] HalReadSector: sem disco presente.\n");
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        kputs("[disk] HalReadSector: timeout (BSY) lba="); kput_dec(lba); kputc('\n');
        return -1;
    }

    ata_setup_lba(lba);
    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);
    ata_delay400ns();

    int w = ata_wait_drq();
    if (w != 0) {
        kputs("[disk] HalReadSector: ");
        kputs(w == -2 ? "bit ERR no status" : "timeout esperando DRQ");
        kputs(" (lba="); kput_dec(lba); kputs(")\n");
        return -1;
    }

    uint16_t* p = (uint16_t*)buf;
    for (int i = 0; i < HAL_SECTOR_SIZE / 2; i++) p[i] = inw16(ATA_REG_DATA);

    const uint8_t* b = (const uint8_t*)buf;
    kputs("[disk] READ  lba="); kput_dec(lba);
    kputs(" (512 bytes) primeiros: ");
    for (int i = 0; i < 8; i++) { kput_hex(b[i]); kputc(' '); }
    kputc('\n');
    return 0;
}

// ============================================================================
//  HalWriteSector — escreve 1 setor (512 bytes) por PIO + cache flush.
// ============================================================================
MS_ABI int HalWriteSector(uint32_t lba, const void* buf) {
    if (!s_present) {
        kputs("[disk] HalWriteSector: sem disco presente.\n");
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        kputs("[disk] HalWriteSector: timeout (BSY) lba="); kput_dec(lba); kputc('\n');
        return -1;
    }

    ata_setup_lba(lba);
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);
    ata_delay400ns();

    int w = ata_wait_drq();
    if (w != 0) {
        kputs("[disk] HalWriteSector: ");
        kputs(w == -2 ? "bit ERR no status" : "timeout esperando DRQ");
        kputs(" (lba="); kput_dec(lba); kputs(")\n");
        return -1;
    }

    const uint16_t* p = (const uint16_t*)buf;
    for (int i = 0; i < HAL_SECTOR_SIZE / 2; i++) {
        outw16(ATA_REG_DATA, p[i]);
    }

    // Cache flush: garante que os dados foram para o meio (nao so o cache).
    outb(ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_not_busy();

    const uint8_t* b = (const uint8_t*)buf;
    kputs("[disk] WRITE lba="); kput_dec(lba);
    kputs(" (512 bytes) primeiros: ");
    for (int i = 0; i < 8; i++) { kput_hex(b[i]); kputc(' '); }
    kputc('\n');
    return 0;
}
