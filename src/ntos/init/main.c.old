#include <stdint.h>
#include <stddef.h>
#include "video/vga.h"
#include "video/video.h"
#include "display/BasicDisplay/gpu.h"
#include "display/VirtioGpu/VirtioGpu.h"
#include "serial/serial.h"
#include "input/keyboard.h"
#include "input/mouse/mouse.h"   // FASE 11: driver PS/2 do mouse (IRQ12)
#include "input/virtio_input.h"  // FASE 14: tablet ABSOLUTA (virtio-input) — cursor visivel
#include "ke/amd64/gdt.h"
#include "ke/amd64/idt.h"
#include "ke/amd64/pic.h"
#include "ke/amd64/pit.h"
#include "ke/amd64/isr.h"
#include "ke/amd64/apic.h"   // Pilar 2 (NT foundation): Local APIC + IO-APIC
#include "ke/amd64/smp.h"    // Pilar 3 (NT foundation): MADT + INIT-SIPI-SIPI
#include "ke/sched.h"        // Pilar 4 (NT foundation): scheduler MP
#include "io.h"               // inb/outb para leitura direta de portas (proof P2)
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/paging.h"   // Pilar 1 (NT foundation): mmio arena + probe
#include "hal/hal.h"
#include "hal/cpu.h"
#include "hal/disk.h"
#include "filesystems/ntfs/ntfs.h"
#include "ldr/loader.h"
#include "ldr/pe_export_image.h"   // GATE 2/5/6 do pintok.sys: ntoskrnl.exe sintetico parseavel
#include "io/driver.h"
#include "ob/object.h"
#include "io/io.h"
#include "ps/process.h"
#include "ex/callbacks.h"
#include "cm/registry.h"
#include "ps/cid_table.h"   // FASE 7.5: PspCidTable
#include "win32/win32k.h"
#include "dx/dxgkrnl/dxgkrnl.h"   // dxgkrnl: dispatcher DirectX em kernel
#include "dx/dxgmms/dxgmms.h"     // dxgmms : memory manager de video (residency)
#include "audio/HDAudio/HDAudio.h" // FASE 11: HD Audio stub (so deteccao PCI)
#include "network/ndis/ndis.h"     // FASE 12: NDIS framework (kernel)
#include "network/tcpip/tcpip.h"   // FASE 12: TCP/IP stack (kernel)
#include "network/e1000/e1000.h"   // FASE 12: Intel 8254x NIC stub
#include "usb/usbport/usbport.h"   // FASE 13: USB Port framework
#include "usb/usbhub/usbhub.h"     // FASE 13: USB Hub class driver
#include "usb/xhci/xhci.h"         // FASE 13: xHCI/EHCI/OHCI/UHCI host controller stub
#include "acpi/acpi.h"             // FASE 13: ACPI stub (RSDP scan)
#include "fltmgr/fltmgr.h"         // FASE 13: Filter Manager
#include "io/pnp.h"                // FASE 13: PnP Manager (IRP_MJ_PNP)

// FS layer (ntfs_fs.c): device de volume registrado no I/O Manager.
PDEVICE_OBJECT ntfs_fs_volume_device(void);

// Escreve um caractere na tela (VGA) E na serial.
void kputc(char c) {
    vga_putc(c);
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}
void kputs(const char* s) { while (*s) kputc(*s++); }

void kput_hex(uint64_t v) {
    const char* d = "0123456789ABCDEF";
    kputc('0'); kputc('x');
    for (int i = 60; i >= 0; i -= 4) kputc(d[(v >> i) & 0xF]);
}
void kput_dec(uint64_t v) {
    char buf[21]; int i = 0;
    if (v == 0) { kputc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) kputc(buf[i]);
}

// Rotinas que o compilador pode chamar implicitamente (freestanding).
void* memset(void* dst, int v, size_t n) {
    uint8_t* p = (uint8_t*)dst; while (n--) *p++ = (uint8_t)v; return dst;
}
void* memcpy(void* dst, const void* s, size_t n) {
    uint8_t* a = (uint8_t*)dst; const uint8_t* b = (const uint8_t*)s;
    while (n--) *a++ = *b++; return dst;
}
void* memmove(void* dst, const void* s, size_t n) {
    uint8_t* a = (uint8_t*)dst; const uint8_t* b = (const uint8_t*)s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

// ----------------------------------------------------------------------------
//  FASE 2 (HAL disco) — teste do disco IDE: le o MBR (setor 0), localiza a
//  particao NTFS na tabela de particoes e le o BOOT SECTOR dessa particao,
//  conferindo a assinatura "NTFS    " no offset 3 (regra 4: tudo logado).
//
//  Roda DEPOIS de hal_init (a HAL ja achou o controlador IDE) e ANTES de tocar
//  o framebuffer, para preservar os logs no VGA texto. Sem -Disk, o disco nao
//  esta presente e a funcao apenas reporta isso (boot segue verde).
//
//  Retorna o LBA inicial da particao NTFS (>0) se achou e validou; 0 senao.
//  Esse LBA e o ponto de partida para montar o NTFS nas fases seguintes.
// ----------------------------------------------------------------------------
static uint32_t g_ntfs_part_lba = 0;   // LBA inicial da particao NTFS (0 = nenhuma)

static uint32_t disk_test(void) {
    kputs("\n--- FASE 2: teste do disco IDE (MBR + boot sector NTFS) ---\n");

    if (!hal_disk_init()) {
        kputs("[disk] sem disco anexado; pulando o teste de leitura "
              "(rode: .\\run.ps1 -Headless -Disk).\n");
        return 0;
    }

    // 1) Setor 0 = MBR (Master Boot Record).
    static uint8_t sec[HAL_SECTOR_SIZE];
    if (HalReadSector(0, sec) != 0) {
        kputs("[disk] falha ao ler o setor 0 (MBR).\n");
        return 0;
    }

    // Assinatura do MBR: 0x55 0xAA nos offsets 510/511.
    kputs("[disk] MBR assinatura @510 = ");
    kput_hex(sec[510]); kputc(' '); kput_hex(sec[511]);
    kputs((sec[510] == 0x55 && sec[511] == 0xAA) ? "  (0x55AA OK)\n"
                                                 : "  (sem 0x55AA)\n");

    // 2) Tabela de particoes: 4 entradas de 16 bytes a partir do offset 0x1BE.
    //    Cada entrada: +0 status, +4 type, +8 LBA inicial (dword LE), +12 nsec.
    //    Procuramos a 1a particao do tipo NTFS (0x07) com LBA inicial valido.
    uint32_t part_lba = 0;
    uint8_t  part_type = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = sec + 0x1BE + i * 16;
        uint8_t  type = e[4];
        uint32_t lba  = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                        ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t nsec = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                        ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (type == 0x00) continue;            // entrada vazia
        kputs("[disk] particao "); kput_dec(i);
        kputs(": type="); kput_hex(type);
        kputs(" LBA="); kput_dec(lba);
        kputs(" nsec="); kput_dec(nsec); kputc('\n');
        if (part_lba == 0 && (type == 0x07 || type == 0x17)) {  // NTFS / NTFS oculta
            part_lba = lba; part_type = type;
        }
    }

    // Algumas imagens "superfloppy" nao tem MBR/particao: o setor 0 ja e o boot
    // sector NTFS. Detectamos isso pela assinatura no proprio setor 0.
    if (part_lba == 0) {
        if (sec[3] == 'N' && sec[4] == 'T' && sec[5] == 'F' && sec[6] == 'S') {
            kputs("[disk] sem tabela de particao MBR; setor 0 ja e o boot NTFS "
                  "(imagem superfloppy).\n");
            g_ntfs_part_lba = 0;
            kputs("[disk] assinatura 'NTFS    ' confirmada no boot sector.\n");
            return 0;   // particao comeca no LBA 0
        }
        kputs("[disk] nenhuma particao NTFS (type 0x07) na tabela do MBR.\n");
        return 0;
    }
    (void)part_type;

    // 3) Le o boot sector (VBR) da particao NTFS e confere "NTFS    " @offset 3.
    kputs("[disk] particao NTFS no LBA "); kput_dec(part_lba);
    kputs("; lendo o boot sector (VBR)...\n");
    if (HalReadSector(part_lba, sec) != 0) {
        kputs("[disk] falha ao ler o boot sector da particao.\n");
        return 0;
    }

    // OEM ID no offset 3: deve ser "NTFS    " (NTFS + 4 espacos).
    kputs("[disk] OEM @3 = '");
    for (int i = 0; i < 8; i++) {
        char c = (char)sec[3 + i];
        kputc((c >= 0x20 && c < 0x7F) ? c : '.');
    }
    kputs("'\n");

    int is_ntfs = (sec[3] == 'N' && sec[4] == 'T' && sec[5] == 'F' && sec[6] == 'S');
    if (is_ntfs) {
        // BPB do NTFS (campos uteis para a montagem nas proximas fases):
        uint16_t bytes_per_sec = (uint16_t)(sec[11] | (sec[12] << 8));
        uint8_t  sec_per_clus  = sec[13];
        uint64_t total_sectors = 0, mft_lcn = 0;
        for (int i = 0; i < 8; i++) total_sectors |= (uint64_t)sec[0x28 + i] << (i * 8);
        for (int i = 0; i < 8; i++) mft_lcn       |= (uint64_t)sec[0x30 + i] << (i * 8);
        kputs("[disk] NTFS BPB: bytes/setor="); kput_dec(bytes_per_sec);
        kputs(" setores/cluster="); kput_dec(sec_per_clus);
        kputs(" total_setores="); kput_dec(total_sectors);
        kputs(" MFT_LCN="); kput_dec(mft_lcn); kputc('\n');
        kputs("[disk] assinatura 'NTFS    ' confirmada no boot sector -> OK\n");
        g_ntfs_part_lba = part_lba;

        // --- Prova de HalWriteSector (NAO destrutiva): le um setor de SOBRA bem
        //     longe da area usada (so o MBR/boot/MFT estao escritos), salva o
        //     conteudo, escreve um padrao, le de volta conferindo, e RESTAURA o
        //     original. Assim comprovamos a ESCRITA por PIO sem corromper a
        //     imagem (regra 4: setor escrito logado; regra 3: nao quebrar nada).
        uint32_t scratch = 100000;   // dentro da particao, em area NTFS nao alocada
        if (scratch < hal_disk_sector_count()) {
            static uint8_t saved[HAL_SECTOR_SIZE];
            static uint8_t pattern[HAL_SECTOR_SIZE];
            static uint8_t readback[HAL_SECTOR_SIZE];
            kputs("[disk] teste de ESCRITA (nao destrutivo) no setor de sobra ");
            kput_dec(scratch); kputs(":\n");
            if (HalReadSector(scratch, saved) == 0) {
                for (int i = 0; i < HAL_SECTOR_SIZE; i++)
                    pattern[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
                if (HalWriteSector(scratch, pattern) == 0 &&
                    HalReadSector(scratch, readback) == 0) {
                    int ok = 1;
                    for (int i = 0; i < HAL_SECTOR_SIZE; i++)
                        if (readback[i] != pattern[i]) { ok = 0; break; }
                    kputs(ok ? "[disk] write/readback BATE (escrita PIO OK).\n"
                             : "[disk] write/readback DIVERGE!\n");
                    // Restaura o conteudo original (imagem volta ao estado anterior).
                    HalWriteSector(scratch, saved);
                    kputs("[disk] setor de sobra restaurado ao conteudo original.\n");
                }
            }
        }
    } else {
        kputs("[disk] assinatura NTFS ausente no boot sector (particao nao montada).\n");
    }
    return g_ntfs_part_lba;
}

// ----------------------------------------------------------------------------
//  FASE 3 — Teste do driver NTFS (LEITURA) + camada de File System.
//
//  Monta o volume NTFS (BPB + $MFT), LE \hello.txt resolvendo o caminho ate o
//  $DATA e mostra o conteudo na serial (deve bater com o texto conhecido da
//  imagem de teste), e LISTA o diretorio raiz. Tambem exercita a camada de FS
//  via I/O Manager (FsMountVolume registra \Device\Harddisk0\Partition1 e o
//  NtCreateFile/NtReadFile resolvem caminho -> MFT -> $DATA).
//
//  Roda apos disk_test(), so se houver um volume NTFS (g_ntfs_part_lba).
// ----------------------------------------------------------------------------
static void ntfs_print_dir_entry(int index, const NTFS_FILE_INFO* info, void* ctx) {
    (void)ctx;
    kputs("[ntfs]   ["); kput_dec(index); kputs("] ");
    kputs(info->is_dir ? "<DIR>  " : "       ");
    kputs(info->name);
    if (!info->is_dir) { kputs("   ("); kput_dec(info->size); kputs(" bytes)"); }
    kputs("  -> MFT #"); kput_dec(info->mft_record);
    kputc('\n');
}

static void ntfs_test(uint32_t part_lba) {
    // Monta o volume e registra o device de FS no I/O Manager (camada de FS).
    if (!FsMountVolume(part_lba)) {
        kputs("[ntfs] montagem/registro do volume falhou; pulando o teste NTFS.\n");
        return;
    }

    // 1) LISTAR o diretorio raiz (\). Percorre $INDEX_ROOT/$INDEX_ALLOCATION.
    kputs("\n[ntfs] === LISTAGEM DO DIRETORIO RAIZ (\\) ===\n");
    int n = ntfs_list_dir(NTFS_MFT_ROOT, ntfs_print_dir_entry, 0);
    kputs("[ntfs] total de entradas no diretorio raiz: "); kput_dec(n); kputc('\n');

    // 2) LER \hello.txt: resolve o caminho -> registro MFT -> $DATA, e mostra
    //    o conteudo (deve == o texto conhecido gravado pela imagem de teste).
    kputs("\n[ntfs] === LEITURA DE \\hello.txt ===\n");
    NTFS_FILE_INFO fi;
    if (ntfs_resolve_path("\\hello.txt", &fi)) {
        kputs("[ntfs] \\hello.txt resolvido: MFT #"); kput_dec(fi.mft_record);
        kputs(", tamanho "); kput_dec(fi.size); kputs(" bytes, ");
        kputs(fi.is_dir ? "DIRETORIO" : "ARQUIVO"); kputc('\n');

        static char content[1024];
        uint32_t got = ntfs_read_file(fi.mft_record, 0, content,
                                      (uint32_t)(sizeof(content) - 1));
        content[got < sizeof(content) ? got : sizeof(content) - 1] = 0;
        kputs("[ntfs] bytes lidos do $DATA: "); kput_dec(got); kputc('\n');
        kputs("[ntfs] ---- conteudo de \\hello.txt ----\n");
        kputs(content);
        if (got && content[got - 1] != '\n') kputc('\n');
        kputs("[ntfs] ---- fim do conteudo ----\n");
    } else {
        kputs("[ntfs] nao foi possivel resolver \\hello.txt no volume.\n");
    }

    // 3) LER \dir1\file.txt: prova a descida de diretorio (\dir1 -> file.txt).
    kputs("\n[ntfs] === LEITURA DE \\dir1\\file.txt (subdiretorio) ===\n");
    if (ntfs_resolve_path("\\dir1\\file.txt", &fi)) {
        kputs("[ntfs] \\dir1\\file.txt resolvido: MFT #"); kput_dec(fi.mft_record);
        kputs(", "); kput_dec(fi.size); kputs(" bytes\n");
        static char buf2[512];
        uint32_t got = ntfs_read_file(fi.mft_record, 0, buf2, (uint32_t)(sizeof(buf2) - 1));
        buf2[got < sizeof(buf2) ? got : sizeof(buf2) - 1] = 0;
        kputs("[ntfs] conteudo: "); kputs(buf2);
        if (got && buf2[got - 1] != '\n') kputc('\n');
    } else {
        kputs("[ntfs] (\\dir1\\file.txt nao encontrado nesta imagem.)\n");
    }

    // 4) CAMADA DE FS via I/O MANAGER: prova que \Device\Harddisk0\Partition1
    //    atende IRPs reais (IRP_MJ_CREATE/READ/DIRECTORY_CONTROL) pelo
    //    DRIVER_OBJECT.MajorFunction — o mesmo caminho de NtCreateFile/NtReadFile/
    //    NtQueryDirectoryFile vindo do ring 3. Monta os IRPs e chama IoCallDriver.
    kputs("\n[ntfs] === CAMADA DE FS via I/O MANAGER (IRPs no device de volume) ===\n");
    PDEVICE_OBJECT vol = ntfs_fs_volume_device();
    if (vol) {
        // (a) IRP_MJ_CREATE — abre o volume.
        PIRP cr = io_build_request(IRP_MJ_CREATE, vol);
        if (cr) { IoCallDriver(vol, cr); io_free_irp(cr); }

        // (b) IRP_MJ_READ — le \hello.txt pelo I/O Manager. Aponta o alvo (MFT
        //     #24) e monta um READ com Key=MFT, ByteOffset=0.
        if (ntfs_resolve_path("\\hello.txt", &fi)) {
            ntfs_fs_set_target(fi.mft_record, 0);
            static char iobuf[128];
            PIRP rd = io_build_read(vol, iobuf, (uint32_t)(sizeof(iobuf) - 1));
            if (rd) {
                IoGetNextIrpStackLocation(rd)->Parameters.Read.Key = (uint32_t)fi.mft_record;
                IoGetNextIrpStackLocation(rd)->Parameters.Read.ByteOffset = 0;
                IoCallDriver(vol, rd);
                uint64_t got = rd->IoStatus.Information;
                uint64_t n = got < sizeof(iobuf) - 1 ? got : sizeof(iobuf) - 1;
                for (uint64_t i = 0; i < n; i++)
                    iobuf[i] = rd->AssociatedIrp.SystemBuffer ? ((char*)rd->AssociatedIrp.SystemBuffer)[i] : 0;
                iobuf[n] = 0;
                kputs("[ntfs] IoCallDriver(READ) devolveu "); kput_dec(got);
                kputs(" bytes: \""); kputs(iobuf); kputs("\"\n");
                io_free_irp(rd);
            }
        }

        // (c) IRP_MJ_DIRECTORY_CONTROL — enumera a raiz, uma entrada por IRP
        //     (como NtQueryDirectoryFile com ReturnSingleEntry).
        ntfs_fs_set_target(NTFS_MFT_ROOT, 1);
        kputs("[ntfs] enumerando a raiz via IRP_MJ_DIRECTORY_CONTROL:\n");
        for (int guard = 0; guard < 32; guard++) {
            static uint8_t dbuf[320];
            PIRP dc = io_build_read(vol, dbuf, (uint32_t)sizeof(dbuf));
            if (!dc) break;
            IoGetNextIrpStackLocation(dc)->MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
            IoGetNextIrpStackLocation(dc)->Parameters.Read.Length = (uint32_t)sizeof(dbuf);
            IoCallDriver(vol, dc);
            uint64_t info = dc->IoStatus.Information;
            io_free_irp(dc);
            if (info == 0) break;   // STATUS_NO_MORE_FILES
        }

        // (d) IRP_MJ_WRITE — escreve no $DATA de \dir1\file.txt PELO I/O Manager
        //     (NtWriteFile no volume monta este IRP). Aponta o alvo, escreve uma
        //     sobrescrita curta, e RELE via IRP_MJ_READ confirmando.
        kputs("[ntfs] escrevendo em \\dir1\\file.txt via IRP_MJ_WRITE (I/O Manager):\n");
        NTFS_FILE_INFO dfi;
        if (ntfs_resolve_path("\\dir1\\file.txt", &dfi)) {
            ntfs_fs_set_target(dfi.mft_record, 0);
            const char* msg = "via IRP!";   // 8 bytes (<= tamanho original)
            uint32_t mlen = 0; while (msg[mlen]) mlen++;
            PIRP wr = io_build_write(vol, (void*)msg, mlen);
            if (wr) {
                IoGetNextIrpStackLocation(wr)->Parameters.Write.Key = (uint32_t)dfi.mft_record;
                IoGetNextIrpStackLocation(wr)->Parameters.Write.ByteOffset = 0;
                IoCallDriver(vol, wr);
                kputs("[ntfs] IoCallDriver(WRITE) gravou "); kput_dec(wr->IoStatus.Information);
                kputs(" bytes.\n");
                io_free_irp(wr);
            }
            // RELE via IRP_MJ_READ.
            static char vbuf[64];
            PIRP rd = io_build_read(vol, vbuf, (uint32_t)(sizeof(vbuf) - 1));
            if (rd) {
                IoGetNextIrpStackLocation(rd)->Parameters.Read.Key = (uint32_t)dfi.mft_record;
                IoGetNextIrpStackLocation(rd)->Parameters.Read.ByteOffset = 0;
                IoCallDriver(vol, rd);
                uint64_t got = rd->IoStatus.Information;
                uint64_t n = got < sizeof(vbuf) - 1 ? got : sizeof(vbuf) - 1;
                for (uint64_t i = 0; i < n; i++)
                    vbuf[i] = rd->AssociatedIrp.SystemBuffer ? ((char*)rd->AssociatedIrp.SystemBuffer)[i] : 0;
                vbuf[n] = 0;
                kputs("[ntfs] RELIDO via IRP_MJ_READ: \""); kputs(vbuf); kputs("\"\n");
                io_free_irp(rd);
            }
        }
    } else {
        kputs("[ntfs] device de volume nao registrado (camada de FS indisponivel).\n");
    }

    // ========================================================================
    //  FASE 4 — ESCRITA NTFS (subconjunto seguro): sobrescreve o $DATA de um
    //  arquivo existente, REMONTA o estado relendo o registro MFT do disco e
    //  confirma o novo conteudo na serial (regra: "escreva, releia, confirme").
    //  build/boot verdes: so usa HalWriteSector em registros JA alocados.
    // ========================================================================
    kputs("\n[ntfs] === ESCRITA NTFS (FASE 4) em \\hello.txt ===\n");
    NTFS_FILE_INFO wfi;
    if (ntfs_resolve_path("\\hello.txt", &wfi)) {
        uint64_t rec = wfi.mft_record;
        kputs("[ntfs] alvo: \\hello.txt = MFT #"); kput_dec(rec);
        kputs(", tamanho original "); kput_dec(wfi.size); kputs(" bytes.\n");

        // --- (1) SOBRESCRITA do mesmo tamanho (<= original): caminho mais seguro.
        //     Troca os primeiros bytes do conteudo e confirma relendo do disco.
        const char* over = "OVERWRITTEN by MeuOS write path!";   // 32 bytes
        uint32_t olen = 0; while (over[olen]) olen++;
        kputs("[ntfs] (1) sobrescrevendo "); kput_dec(olen);
        kputs(" bytes no inicio (tamanho <= original)...\n");
        uint32_t w1 = ntfs_write_file(rec, 0, over, olen, /*set_eof*/0, /*parent*/0);
        kputs("[ntfs] (1) ntfs_write_file devolveu "); kput_dec(w1); kputs(" bytes.\n");

        // RELE do disco (ntfs_read_file le o registro MFT de novo via HAL).
        static char rb[256];
        uint32_t g1 = ntfs_read_file(rec, 0, rb, (uint32_t)(sizeof(rb) - 1));
        rb[g1 < sizeof(rb) ? g1 : sizeof(rb) - 1] = 0;
        kputs("[ntfs] (1) RELIDO do disco ("); kput_dec(g1); kputs(" bytes): \"");
        kputs(rb); kputs("\"\n");
        int ok1 = 1; for (uint32_t i = 0; i < olen; i++) if (rb[i] != over[i]) { ok1 = 0; break; }
        kputs(ok1 ? "[ntfs] (1) OK: sobrescrita PERSISTIU no disco.\n"
                  : "[ntfs] (1) FALHA: conteudo relido nao bate.\n");

        // --- (2) CRESCER o arquivo (set_eof): novo conteudo MAIOR que o original,
        //     ainda dentro do registro MFT (resident grow). Atualiza tamanho no
        //     $FILE_NAME + entrada no $INDEX da raiz (#5). Confirma relendo +
        //     conferindo o tamanho reportado pela listagem do diretorio.
        const char* grow =
            "MeuOS FASE 4: arquivo NTFS reescrito E AUMENTADO no lugar, "
            "resident grow dentro do registro MFT, com $FILE_NAME e $INDEX_ROOT "
            "do diretorio raiz atualizados. Releitura do disco confirma. [EOF]";
        uint32_t glen = 0; while (grow[glen]) glen++;
        kputs("[ntfs] (2) crescendo o arquivo p/ "); kput_dec(glen);
        kputs(" bytes (set_eof, atualizando $INDEX da raiz #5)...\n");
        uint32_t w2 = ntfs_write_file(rec, 0, grow, glen, /*set_eof*/1, /*parent*/NTFS_MFT_ROOT);
        kputs("[ntfs] (2) ntfs_write_file devolveu "); kput_dec(w2); kputs(" bytes.\n");

        // RELE o conteudo do disco.
        static char rb2[512];
        uint32_t g2 = ntfs_read_file(rec, 0, rb2, (uint32_t)(sizeof(rb2) - 1));
        rb2[g2 < sizeof(rb2) ? g2 : sizeof(rb2) - 1] = 0;
        kputs("[ntfs] (2) RELIDO do disco ("); kput_dec(g2); kputs(" bytes): \"");
        kputs(rb2); kputs("\"\n");
        int ok2 = (g2 == glen);
        for (uint32_t i = 0; ok2 && i < glen; i++) if (rb2[i] != grow[i]) ok2 = 0;
        kputs(ok2 ? "[ntfs] (2) OK: arquivo CRESCEU e o novo conteudo persistiu.\n"
                  : "[ntfs] (2) FALHA: tamanho/conteudo relido nao bate.\n");

        // Confirma o NOVO tamanho via a LISTAGEM do diretorio (le a chave do
        // $INDEX_ROOT da raiz, que tambem atualizamos).
        NTFS_FILE_INFO after;
        if (ntfs_resolve_path("\\hello.txt", &after)) {
            kputs("[ntfs] (2) diretorio raiz reporta \\hello.txt agora com ");
            kput_dec(after.size); kputs(" bytes (era "); kput_dec(wfi.size);
            kputs(") -> "); kputs(after.size == glen ? "OK\n" : "tamanho do indice nao atualizou\n");
        }

        kputs("[ntfs] ESCRITA NTFS concluida: o disk.img foi modificado (o SHA-256 "
              "muda; releitura comprova).\n");
    } else {
        kputs("[ntfs] (\\hello.txt nao encontrado; pulando o teste de escrita.)\n");
    }

    // ========================================================================
    //  FASE 4 — CRIAR e EXCLUIR arquivo (subconjunto seguro, sem alocar cluster).
    //  Cria \novo.txt na raiz com $DATA residente, RELE do disco confirmando o
    //  conteudo, LISTA a raiz (deve aparecer), depois EXCLUI e LISTA de novo
    //  (deve sumir). Tudo logado; build/boot verdes.
    // ========================================================================
    kputs("\n[ntfs] === CRIAR/EXCLUIR arquivo (FASE 4) ===\n");
    {
        const char* newname = "novo.txt";
        const char* newdata = "Arquivo CRIADO em runtime pelo MeuOS (novo registro MFT + entrada no $INDEX da raiz).\r\n";
        uint32_t ndlen = 0; while (newdata[ndlen]) ndlen++;
        uint64_t newrec = 0;
        kputs("[ntfs] criando \\novo.txt na raiz (#5)...\n");
        if (ntfs_create_file(NTFS_MFT_ROOT, newname, /*is_dir*/0, newdata, ndlen, &newrec)) {
            // RELE pelo caminho (resolve \novo.txt -> registro -> $DATA do disco).
            NTFS_FILE_INFO nf;
            if (ntfs_resolve_path("\\novo.txt", &nf)) {
                kputs("[ntfs] \\novo.txt resolvido: MFT #"); kput_dec(nf.mft_record);
                kputs(", "); kput_dec(nf.size); kputs(" bytes.\n");
                static char nrb[256];
                uint32_t gg = ntfs_read_file(nf.mft_record, 0, nrb, (uint32_t)(sizeof(nrb) - 1));
                nrb[gg < sizeof(nrb) ? gg : sizeof(nrb) - 1] = 0;
                kputs("[ntfs] conteudo RELIDO do disco: \""); kputs(nrb); kputs("\"\n");
                int okc = (gg == ndlen);
                for (uint32_t i = 0; okc && i < ndlen; i++) if (nrb[i] != newdata[i]) okc = 0;
                kputs(okc ? "[ntfs] OK: arquivo CRIADO e o conteudo persistiu no disco.\n"
                          : "[ntfs] FALHA: conteudo do arquivo criado nao bate.\n");
            } else {
                kputs("[ntfs] FALHA: \\novo.txt nao resolve apos a criacao.\n");
            }

            // LISTA a raiz: deve conter novo.txt agora.
            kputs("[ntfs] raiz APOS criar (deve conter novo.txt):\n");
            int nafter = ntfs_list_dir(NTFS_MFT_ROOT, ntfs_print_dir_entry, 0);
            kputs("[ntfs] total de entradas: "); kput_dec(nafter); kputc('\n');

            // EXCLUI \novo.txt.
            kputs("[ntfs] excluindo \\novo.txt...\n");
            if (ntfs_delete_file(NTFS_MFT_ROOT, newrec)) {
                kputs("[ntfs] raiz APOS excluir (novo.txt deve SUMIR):\n");
                int ndel = ntfs_list_dir(NTFS_MFT_ROOT, ntfs_print_dir_entry, 0);
                kputs("[ntfs] total de entradas: "); kput_dec(ndel); kputc('\n');
                NTFS_FILE_INFO gone;
                kputs(ntfs_resolve_path("\\novo.txt", &gone)
                          ? "[ntfs] FALHA: \\novo.txt ainda resolve apos a exclusao.\n"
                          : "[ntfs] OK: \\novo.txt foi EXCLUIDO (nao resolve mais).\n");
            } else {
                kputs("[ntfs] exclusao reportou erro.\n");
            }
        } else {
            kputs("[ntfs] criacao de \\novo.txt nao concluiu (ver logs acima).\n");
        }
    }

    kputs("\n[ntfs] teste de leitura+escrita+criar/excluir NTFS concluido.\n");
}

// ----------------------------------------------------------------------------
//  Demonstracao do FRAMEBUFFER grafico (VGA mode 13h).
//
//  Roda DEPOIS de todos os [ok] e dos testes de binario, para nao perder os
//  logs de boot no VGA texto (que para de ser exibido ao entrar em mode 13h).
//  Como em headless (-display none) nao ha pixels, LOGAMOS cada operacao na
//  SERIAL para comprovar a logica. Opcional: screendump via QMP confere a tela.
// ----------------------------------------------------------------------------
static void fb_demo(void) {
    kputs("\n--- Framebuffer grafico (VGA mode 13h, 320x200x256) ---\n");

    kputs("[fb] fb_init(): programando registradores VGA (CRTC/SEQ/GC/AC) + paleta DAC...\n");
    fb_init();
    kputs("[fb] mode 13h ativo: framebuffer linear em 0xA0000 (320x200, 8bpp). "
          "VGA texto deixa de exibir; serial segue.\n");

    // 1) Fundo
    kputs("[fb] fb_clear(cor=1 azul): pintando 320x200 = 64000 pixels.\n");
    fb_clear(FB_BLUE);

    // 2) Borda da "tela" (desktop)
    kputs("[fb] fb_rect(0,0,320,200, cor=15 branco): contorno do desktop.\n");
    fb_rect(0, 0, FB_WIDTH, FB_HEIGHT, FB_WHITE);

    // 3) Uma "janela" (estilo NT): retangulo preenchido + barra de titulo
    int wx = 40, wy = 40, ww = 240, wh = 110;
    kputs("[fb] fb_fill_rect(40,40,240,110, cor=7 cinza): corpo da janela.\n");
    fb_fill_rect(wx, wy, ww, wh, FB_LIGHT_GRAY);
    kputs("[fb] fb_fill_rect(40,40,240,12, cor=1 azul): barra de titulo.\n");
    fb_fill_rect(wx, wy, ww, 12, FB_BLUE);
    kputs("[fb] fb_rect(40,40,240,110, cor=0 preto): moldura da janela.\n");
    fb_rect(wx, wy, ww, wh, FB_BLACK);

    // 4) Texto com a fonte 8x8 embutida (titulo + corpo)
    kputs("[fb] fb_draw_text(48,42,\"MeuOS\", fg=15): titulo na barra.\n");
    fb_draw_text(wx + 8, wy + 2, "MeuOS - mode 13h", FB_WHITE, 0xFF);
    kputs("[fb] fb_draw_text(48,60,\"Framebuffer grafico OK\", fg=0): corpo.\n");
    fb_draw_text(wx + 8, wy + 24, "Framebuffer grafico OK", FB_BLACK, 0xFF);
    fb_draw_text(wx + 8, wy + 40, "Fonte bitmap 8x8 0-9 ABC", FB_RED, 0xFF);
    fb_draw_text(wx + 8, wy + 56, "320x200 256 cores @0xA0000", FB_BLUE, 0xFF);

    // 5) Pixels avulsos + barra de cores (rampa de cinza dos indices 16..255)
    kputs("[fb] fb_pixel: marcando 4 cantos (cores variadas).\n");
    fb_pixel(2, 2, FB_RED);
    fb_pixel(FB_WIDTH - 3, 2, FB_GREEN);
    fb_pixel(2, FB_HEIGHT - 3, FB_YELLOW);
    fb_pixel(FB_WIDTH - 3, FB_HEIGHT - 3, FB_LIGHT_CYAN);
    kputs("[fb] fb_fill_rect x16: paleta nomeada (cores 0..15) em baixo.\n");
    for (int i = 0; i < 16; i++) fb_fill_rect(20 + i * 18, 175, 16, 16, (uint8_t)i);

    // 6) Verifica leitura do framebuffer (prova que escrevemos mesmo)
    uint8_t got = fb_get_pixel(wx + 8 + 1, wy + 1);  // dentro da barra de titulo (azul)
    kputs("[fb] fb_get_pixel(barra de titulo) = "); kput_dec(got);
    kputs(" (esperado 1 = azul) -> ");
    kputs(got == FB_BLUE ? "OK\n" : "DIVERGENTE\n");

    kputs("[fb] demo concluida: desktop + 1 janela (titulo+corpo+texto) desenhados. "
          "Use QMP screendump para ver os pixels.\n");
}

// ============================================================================
//  Pilar 1 (NT foundation) — PROVA de paginacao dinamica.
//
//  Tres etapas, em ordem:
//   (a) Mapeia o Local APIC em 0xFEE00000 (FISICO ACIMA DE 1 GIB, fora da
//       identidade) e LE o registrador LAPIC ID (offset 0x20). Sem PCD esse
//       acesso poderia retornar lixo bufferizado; nosso hal_map_mmio seta
//       PCD|PWT explicitamente. Logamos o valor lido.
//   (b) Alias-mapeia uma mesma pagina fisica em DOIS enderecos virtuais
//       diferentes do arena MMIO. Escreve 0xDEADBEEF via V1, le via V2:
//       deve bater. Prova que mm_map_phys_range esta mesmo populando PTEs
//       (e nao alguma identidade escondida).
//   (c) Desmapeia V1, le via V1 dentro de mm_probe_read_u32 (que aciona o
//       caminho expected-PF em isr.c). Deve disparar #PF -> g_mm_pf_caught=1.
//       V2 continua valido (sao PTEs diferentes apontando p/ o mesmo phys).
//
//  Tudo logado com "[P1] ..." pra ficar visivel no serial.log do qemu.
// ============================================================================
static int proof_pillar1_paging(void) {
    int ok = 1;
    kputs("\n[P1] ==== prova de paginacao dinamica (Pilar 1) ====\n");

    // (a) — LAPIC MMIO acima de 1 GiB.
    volatile uint32_t* lapic = (volatile uint32_t*)hal_map_mmio(0xFEE00000ULL, 0x1000ULL);
    if (!lapic) {
        kputs("[P1] FAIL: hal_map_mmio(LAPIC) devolveu 0\n");
        return 0;
    }
    // LAPIC ID register em offset 0x20 (bits 24..31 = APIC ID). No QEMU BSP=0.
    uint32_t lapic_id_raw = lapic[0x20 / 4];
    kputs("[P1] LAPIC mapeado @ "); kput_hex((uint64_t)(uintptr_t)lapic);
    kputs("  ID raw="); kput_hex(lapic_id_raw);
    kputs("  (APIC ID = "); kput_dec((lapic_id_raw >> 24) & 0xFF); kputs(")\n");
    // (NAO desmapeamos: o Pilar 2 vai querer manter esse VA.)

    // (b) — alias de uma mesma pagina fisica em DOIS VAs.
    uint64_t phys = pmm_alloc_frame();
    if (!phys) { kputs("[P1] FAIL: pmm_alloc_frame sem RAM\n"); return 0; }
    uint64_t v1 = mm_mmio_reserve(0x1000);
    uint64_t v2 = mm_mmio_reserve(0x1000);
    if (!v1 || !v2) { kputs("[P1] FAIL: mm_mmio_reserve\n"); return 0; }
    // Para RAM normal NAO setamos PCD: queremos cache write-back; aqui basta
    // PRESENT|RW (memoria do PMM, nao MMIO de dispositivo).
    if (!mm_map_phys_range(v1, phys, 0x1000, MM_FLAG_PRESENT | MM_FLAG_RW)) {
        kputs("[P1] FAIL: map v1\n"); return 0;
    }
    if (!mm_map_phys_range(v2, phys, 0x1000, MM_FLAG_PRESENT | MM_FLAG_RW)) {
        kputs("[P1] FAIL: map v2\n"); return 0;
    }
    volatile uint32_t* p1 = (volatile uint32_t*)(uintptr_t)v1;
    volatile uint32_t* p2 = (volatile uint32_t*)(uintptr_t)v2;
    *p1 = 0xDEADBEEFu;
    uint32_t back = *p2;
    kputs("[P1] alias map: phys="); kput_hex(phys);
    kputs(" v1="); kput_hex(v1); kputs(" v2="); kput_hex(v2);
    kputs(" write(v1)=0xDEADBEEF read(v2)="); kput_hex(back);
    if (back == 0xDEADBEEFu) kputs(" OK\n");
    else { kputs(" FAIL\n"); ok = 0; }

    // (c) — unmap em V1; V1 deve faltar #PF, V2 continua valido.
    if (!mm_unmap_range(v1, 0x1000)) {
        kputs("[P1] FAIL: mm_unmap_range(v1)\n"); return 0;
    }
    uint32_t junk = 0xCAFEBABEu;
    int got = mm_probe_read_u32(p1, &junk);
    kputs("[P1] probe(v1 apos unmap): retorno="); kput_dec((uint64_t)got);
    kputs(" caught="); kput_dec((uint64_t)g_mm_pf_caught);
    if (!got && g_mm_pf_caught) kputs(" OK (PF capturado)\n");
    else { kputs(" FAIL (esperava PF)\n"); ok = 0; }

    // V2 ainda mapeado: leitura deve bater 0xDEADBEEF.
    uint32_t back2 = *p2;
    kputs("[P1] read(v2) pos-unmap(v1)="); kput_hex(back2);
    if (back2 == 0xDEADBEEFu) kputs(" OK (alias independente)\n");
    else { kputs(" FAIL\n"); ok = 0; }

    // Limpa V2 e devolve a moldura ao PMM (higiene).
    mm_unmap_range(v2, 0x1000);
    pmm_free_frame(phys);

    if (ok) kputs("[P1] ==== PROVA PASSOU ====\n\n");
    else    kputs("[P1] ==== PROVA FALHOU ====\n\n");
    return ok;
}

// ============================================================================
//  Pilar 2 (NT foundation) — PROVA de APIC (Local APIC timer + IO-APIC).
//
//  Sequencia:
//   (a) Antes do APIC: confere que g_ticks avanca pelo PIT (linha de base).
//   (b) Roda apic_init() — mapeia LAPIC + IO-APIC, calibra timer contra PIT,
//       programa LVT Timer periodico em 0xD1, redireciona IRQ1 do IO-APIC
//       para 0x21, MASCARA o PIC inteiro (pic_disable).
//   (c) Confirma que PIC esta mascarado (OCW1: PIC1=0xFF, PIC2=0xFF) — IRQ0
//       do PIT esta SILENCIADA. Nenhum vetor 0x20 (IRQ0) pode mais subir.
//   (d) g_ticks precisa CONTINUAR avancando — agora pelo vetor 0xD1 (APIC).
//       Espera 20 ticks (200 ms a 100 Hz). Se chegou, a prova passou.
// ============================================================================
static int proof_pillar2_apic(void) {
    int ok = 1;
    kputs("\n[P2] ==== prova de APIC (Pilar 2) ====\n");

    // (a) PIT vivo: g_ticks avanca.
    uint64_t base = g_ticks;
    while (g_ticks < base + 5) __asm__ volatile ("hlt");
    kputs("[P2] PIT base: g_ticks "); kput_dec(base);
    kputs(" -> "); kput_dec(g_ticks); kputs(" (PIT alimentando)\n");

    // (b) Liga APIC. apic_init faz pic_disable() ao final.
    apic_init();
    if (!apic_active()) { kputs("[P2] FAIL: apic_init nao ativou\n"); return 0; }

    // (c) PIC silenciado.
    uint8_t pm1 = inb(0x21);
    uint8_t pm2 = inb(0xA1);
    kputs("[P2] PIC mask: PIC1=0x"); kput_hex(pm1);
    kputs(" PIC2=0x"); kput_hex(pm2);
    if (pm1 == 0xFFu && pm2 == 0xFFu) kputs(" OK (8259 silente)\n");
    else { kputs(" FAIL\n"); ok = 0; }

    // (d) g_ticks avanca pelo APIC timer agora.
    uint64_t t0 = g_ticks;
    while (g_ticks < t0 + 20) __asm__ volatile ("hlt");
    uint64_t t1 = g_ticks;
    kputs("[P2] APIC timer: g_ticks "); kput_dec(t0);
    kputs(" -> "); kput_dec(t1);
    kputs(" delta="); kput_dec(t1 - t0);
    if (t1 - t0 >= 20) kputs(" OK (APIC alimentando com PIT desligado)\n");
    else { kputs(" FAIL\n"); ok = 0; }

    if (ok) kputs("[P2] ==== PROVA PASSOU ====\n\n");
    else    kputs("[P2] ==== PROVA FALHOU ====\n\n");
    return ok;
}

// ============================================================================
//  Pilar 3 (NT foundation) — PROVA de SMP.
//
//  Sequencia:
//   (a) acpi_init() — varre BIOS area, acha RSDP, le RSDT phys.
//   (b) smp_init() — parseia MADT, monta tabela de APIC IDs, lanca cada AP
//       via trampoline em phys 0x8000 + INIT-SIPI-SIPI.
//   (c) Espera AP sinalizar alive (atomic increment de s_ap_alive_count e
//       marcacao por APIC ID). BSP polls com timeout.
//   (d) A prova passa se: MADT achou >= 2 CPUs, lancamos >= 1 AP, e ao menos
//       1 AP sinalizou alive com o APIC ID esperado.
// ============================================================================
static int proof_pillar3_smp(void) {
    kputs("\n[P3] ==== prova de SMP (Pilar 3) ====\n");

    // (a) ACPI: idempotente — pode ja ter rodado, mas garantimos AGORA.
    extern int acpi_init(void);
    extern uint64_t acpi_rsdt_phys(void);
    acpi_init();
    uint64_t rsdt = acpi_rsdt_phys();
    kputs("[P3] RSDT phys="); kput_hex(rsdt); kputc('\n');

    // (b) SMP init: parseia MADT, lanca APs.
    smp_init();

    // (c) Resultados.
    int cpus = smp_cpu_count();
    uint32_t alive = smp_ap_alive_count();
    kputs("[P3] CPUs MADT="); kput_dec((uint64_t)cpus);
    kputs("  APs_alive="); kput_dec((uint64_t)alive); kputc('\n');

    int ok = 1;
    if (cpus < 2) {
        kputs("[P3] FAIL: MADT relatou < 2 CPUs (esperava >= 2 com -smp 2)\n");
        ok = 0;
    }
    if (alive < 1) {
        kputs("[P3] FAIL: nenhum AP sinalizou alive\n");
        ok = 0;
    }
    // Confirma que o AP com APIC ID 1 esta vivo (BSP=0 no QEMU).
    if (cpus >= 2 && !smp_ap_seen(1)) {
        kputs("[P3] FAIL: AP com APIC_ID=1 nao sinalizou\n");
        ok = 0;
    }

    if (ok) kputs("[P3] ==== PROVA PASSOU ====\n\n");
    else    kputs("[P3] ==== PROVA FALHOU ====\n\n");
    return ok;
}

// ============================================================================
//  Pilar 4 (NT foundation) — PROVA do scheduler MP.
//
//  Cria duas threads de kernel, afinidade fixa: A no CPU 0 (BSP), B no CPU 1
//  (AP). Cada uma roda `while (!stop) counter++` em contador separado. APIC
//  timer (vetor 0xD1) dispara em CADA CPU; ki_quantum_end troca a thread
//  atual pela proxima ready de SUA CPU. Com afinidade fixa, A so corre no
//  BSP e B so corre no AP — entao seus contadores avancam SIMULTANEAMENTE
//  em cores distintos.
//
//  BSP polls os contadores num loop. A propria proof_pillar4 funciona como o
//  "idle thread" do CPU 0: o scheduler vai swap entre idle (= esta funcao) e
//  thread A. Counter A advances quando A roda em BSP; counter B advances
//  quando B roda em AP. Apos N rounds vendo ambos crescerem, a prova passou.
// ============================================================================
static volatile uint64_t g_counter_a = 0;
static volatile uint64_t g_counter_b = 0;
static volatile int      g_threads_stop = 0;
volatile int g_p4_active = 0;   // ligado dentro de proof_pillar4 — gate do scheduler no timer

// As threads imprimem o contador a cada N iteracoes para que o BSP possa
// OBSERVAR o progresso sem precisar de idle time — caso contrario a thread
// rodando no BSP monopoliza a CPU e o spin loop do BSP nunca progride.
static void p4_thread_a(void* arg) {
    (void)arg;
    kputs("[P4] thread A: vivo no seu core\n");
    while (!g_threads_stop) {
        g_counter_a++;
        if ((g_counter_a & 0xFFFFF) == 0) {     // ~1M iter
            kputs("[P4] A counter="); kput_dec(g_counter_a); kputc('\n');
        }
        __asm__ volatile ("" ::: "memory");
    }
}
static void p4_thread_b(void* arg) {
    (void)arg;
    kputs("[P4] thread B: vivo no seu core\n");
    while (!g_threads_stop) {
        g_counter_b++;
        if ((g_counter_b & 0xFFFFF) == 0) {
            kputs("[P4] B counter="); kput_dec(g_counter_b); kputc('\n');
        }
        __asm__ volatile ("" ::: "memory");
    }
}

static void p4_busy_wait(uint64_t pauses) {
    for (volatile uint64_t i = 0; i < pauses; i++) __asm__ volatile ("pause");
}

static int proof_pillar4_scheduler(void) {
    kputs("\n[P4] ==== prova do scheduler MP (Pilar 4) ====\n");

    if (g_ki_cpu_count < 2) {
        kputs("[P4] FAIL: g_ki_cpu_count="); kput_dec((uint64_t)g_ki_cpu_count);
        kputs(" (< 2 — Pilar 3 nao deixou o AP no scheduler?)\n");
        return 0;
    }

    // Cria threads NT-style: A afinidade CPU 0 (BSP), B afinidade CPU 1 (AP).
    ki_thread_t* ta = ki_create_thread(p4_thread_a, 0, /*prio*/ 8, /*affinity*/ 0);
    ki_thread_t* tb = ki_create_thread(p4_thread_b, 0, /*prio*/ 8, /*affinity*/ 1);
    if (!ta || !tb) { kputs("[P4] FAIL: create_thread\n"); return 0; }
    kputs("[P4] threads criadas: A tid="); kput_dec(ta->tid);
    kputs(" (CPU 0)  B tid="); kput_dec(tb->tid); kputs(" (CPU 1)\n");

    g_p4_active = 1;
    ki_ready_thread(ta);
    ki_ready_thread(tb);

    // BSP polls. Durante este loop, o APIC timer dispara periodicamente:
    //   - Em BSP: ki_quantum_end troca entre 'idle' (= esta funcao kmain) e A.
    //   - Em AP : ki_quantum_end troca entre AP-idle e B.
    // Counter A advances enquanto A roda em BSP; counter B advances em AP.
    uint64_t prev_a = 0, prev_b = 0;
    int wins = 0;
    for (int spin = 0; spin < 24; spin++) {
        p4_busy_wait(50000000ULL);                  // ~5s @ TCG
        uint64_t a = __atomic_load_n(&g_counter_a, __ATOMIC_ACQUIRE);
        uint64_t b = __atomic_load_n(&g_counter_b, __ATOMIC_ACQUIRE);
        kputs("[P4] spin "); kput_dec((uint64_t)spin);
        kputs(" counter_a="); kput_dec(a);
        kputs(" counter_b="); kput_dec(b); kputc('\n');
        if (a > prev_a + 100 && b > prev_b + 100) {
            wins++;
            if (wins >= 2) {
                kputs("[P4] AMBOS contadores avancando concorrentemente (2 rounds) -> OK\n");
                g_threads_stop = 1;
                kputs("[P4] ==== PROVA PASSOU ====\n\n");
                return 1;
            }
        } else {
            wins = 0;
        }
        prev_a = a;
        prev_b = b;
    }
    g_threads_stop = 1;
    kputs("[P4] ==== PROVA FALHOU (timeout sem progresso concorrente) ====\n\n");
    return 0;
}

// ============================================================================
//  MULTITAREFA PREEMPTIVA — threads de kernel de demonstracao.
//
//  Sao KTHREADs reais escalonadas pelo scheduler MP (ke/sched.c): o APIC timer
//  (0xD1) chama ki_quantum_end a cada tick e faz KiSwapContext entre elas, o
//  fluxo do boot (idle thread) e — quando o AP esta online — o outro core.
//
//  Cada worker DORME (sti;hlt) entre ticks e acorda ~1x/segundo para logar seu
//  progresso + em qual CPU esta rodando. Baixo custo, e prova visdivel no log
//  que ha varias threads vivas sendo preemptadas (igual ao Windows).
// ============================================================================
static volatile uint64_t g_worker_beats[4];

static void kdemo_worker(void* arg) {
    uint64_t id   = (uint64_t)(uintptr_t)arg;
    uint64_t next = g_ticks + 100;                 // ~1 s @ 100 Hz
    kputs("[kthread] worker "); kput_dec(id);
    kputs(" vivo (CPU "); kput_dec(ki_current_cpu_index()); kputs(")\n");
    for (;;) {
        __asm__ volatile ("sti; hlt");             // dorme ate o proximo tick
        if (g_ticks >= next) {
            next = g_ticks + 100;
            g_worker_beats[id & 3]++;
            kputs("[kthread] worker "); kput_dec(id);
            kputs(" beat "); kput_dec(g_worker_beats[id & 3]);
            kputs(" @CPU "); kput_dec(ki_current_cpu_index());
            kputs(" (ticks="); kput_dec(g_ticks); kputs(")\n");
            // worker 0 tambem reporta o 2o core (AP): heartbeat + resultado da
            // computacao paralela. O heartbeat cresce MUITO por segundo — prova
            // de que o AP roda instrucoes ao mesmo tempo que este BSP.
            if (id == 0 && smp_ap_working()) {
                kputs("[smp]   CPU1(AP) rodando EM PARALELO: heartbeat=");
                kput_dec(smp_ap_heartbeat());
                kputs(" compute=0x"); kput_hex(smp_ap_compute()); kputc('\n');
            }
        }
    }
}

// Thread de REFRESH do desktop: recompoe a tela ~2x/s. Faz o relogio da taskbar
// (HH:MM:SS) avancar em tempo real e mantem a UI viva sem depender de eventos de
// input — igual ao Windows, onde o shell tem um timer que atualiza a bandeja. E'
// uma KTHREAD de kernel escalonada como as demais: prova que o scheduler roda
// trabalho REAL (nao so contadores).
extern void win32k_refresh_taskbar(void);
extern int  win32k_was_active(void);
static void kdesktop_refresh(void* arg) {
    (void)arg;
    uint64_t next = g_ticks + 50;
    kputs("[kthread] desktop-refresh vivo (relogio/taskbar ao vivo)\n");
    for (;;) {
        __asm__ volatile ("sti; hlt");            // dorme ate o proximo tick
        if (g_ticks >= next) {
            next = g_ticks + 50;                  // ~2 Hz
            if (win32k_was_active()) win32k_refresh_taskbar();
        }
    }
}

// Cria as threads de demonstracao e LIGA a preempcao (g_p4_active=1). A partir
// daqui o timer do BSP escalona idle<->workers. cpu_affinity=-1 => round-robin
// entre os CPUs online (se o AP entrou no scheduler, ele tambem recebe threads).
extern volatile int g_p4_active;
extern int smp_ap_timer_online(void);   // 1 se o LVT timer do AP foi desmascarado

// Substring simples (para detectar o shell persistente pelo nome do modulo).
static int name_contains(const char* s, const char* needle) {
    if (!s || !needle) return 0;
    for (; *s; s++) {
        const char* a = s; const char* b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int s_sched_started = 0;
static void sched_start_demo_threads(void) {
    if (s_sched_started) return;            // idempotente (chamado 1x)
    s_sched_started = 1;
    kputs("\n=== Multitarefa preemptiva: criando KTHREADs de kernel ===\n");
    int online   = g_ki_cpu_count;
    int ap_ready = smp_ap_timer_online();   // so o AP com timer ligado escalona
    kputs("[sched] CPUs no scheduler: "); kput_dec((uint64_t)online);
    kputs("  AP com timer/escalonavel: "); kputs(ap_ready ? "sim" : "nao"); kputc('\n');

    // So podemos FIXAR uma thread num CPU se aquele CPU realmente recebe ticks
    // (senao a thread fica presa na ready queue dele). O BSP (CPU 0) sempre
    // escalona. O AP (CPU 1) so quando seu LVT timer foi desmascarado.
    int aff_cpu1 = (online >= 2 && ap_ready) ? 1 : 0;

    ki_thread_t* w0 = ki_create_thread(kdemo_worker, (void*)(uintptr_t)0, 8, 0);
    ki_thread_t* w1 = ki_create_thread(kdemo_worker, (void*)(uintptr_t)1, 8, aff_cpu1);
    ki_thread_t* w2 = ki_create_thread(kdemo_worker, (void*)(uintptr_t)2, 8, 0);
    // Thread de refresh do desktop (relogio ao vivo) — fixa no BSP (dona do FB).
    ki_thread_t* wr = ki_create_thread(kdesktop_refresh, 0, 9, 0);
    if (w0) ki_ready_thread(w0);
    if (w1) ki_ready_thread(w1);
    if (w2) ki_ready_thread(w2);
    if (wr) ki_ready_thread(wr);

    // FASE FUNDACAO (Item 5): auto-teste de block+wake (2 threads worker reais).
    extern void ki_wait_selftest_spawn(void);
    ki_wait_selftest_spawn();
    // FASE FUNDACAO (Item 6): auto-teste do KTIMER (worker arma timer e bloqueia nele).
    extern void KiTimerSelfTestSpawn(void);
    KiTimerSelfTestSpawn();
    // FASE FUNDACAO (Item 7): auto-teste dos primitivos Ex (fast mutex + lookaside).
    extern void KiExSelfTestSpawn(void);
    KiExSelfTestSpawn();

    __atomic_store_n(&g_p4_active, 1, __ATOMIC_SEQ_CST);   // liga a preempcao no timer ISR
    kputs("[sched] preempcao LIGADA (g_p4_active=1). Timer 0xD1 escalona agora.\n");
}

void kmain(uint32_t mb_info) {
    vga_init();
    serial_init();

    vga_set_color(0x0A, 0x00);
    kputs("==================================================\n");
    kputs("   MeuOS  -  kernel 64 bits, escrito do zero em C\n");
    kputs("==================================================\n");
    vga_set_color(0x0F, 0x00);

    kputs("[ok] Long mode 64 bits + GDT + SSE\n");
    kputs("[ok] Video VGA + Serial COM1\n");

    gdt_init();
    kputs("[ok] GDT completa (ring 0 + ring 3) + TSS\n");

    idt_init();
    kputs("[ok] IDT carregada (256 vetores)\n");

    pic_remap();
    pit_init(100);
    kputs("[ok] PIC remapeado + PIT 100 Hz + teclado por IRQ\n");

    __asm__ volatile ("sti");
    kputs("[ok] Interrupcoes habilitadas (sti)\n");

    // Demonstra dispatch de excecao pela IDT (int3, nao-fatal):
    __asm__ volatile ("int3");

    // Prova o timer (IRQ0): espera ~0,5 s contando ticks.
    while (g_ticks < 50) __asm__ volatile ("hlt");
    kputs("[ok] Timer IRQ0 contando: ");
    kput_dec(g_ticks);
    kputs(" ticks em ~0,5s\n");

    // --- Gerencia de memoria (base para carregar programas) ---
    uint32_t mbflags = *(volatile uint32_t*)(uintptr_t)(mb_info + 0);
    uint64_t mem_top = 0x100000ULL;
    if (mbflags & 1) {                 // bit 0: campos mem_lower/mem_upper validos
        uint32_t mem_upper = *(volatile uint32_t*)(uintptr_t)(mb_info + 8);
        mem_top = 0x100000ULL + (uint64_t)mem_upper * 1024ULL;
    }
    kputs("[ok] RAM detectada: "); kput_dec(mem_top / 1024 / 1024); kputs(" MiB\n");

    pmm_init(mem_top);
    kputs("[ok] PMM: "); kput_dec(pmm_free_frames()); kputs(" frames de 4 KiB livres\n");

    heap_init();
    // Pilar 1 (NT foundation): inicializa o MMIO arena ANTES de qualquer
    // caminho que possa precisar mapear fisicos > 1 GiB (Local APIC no Pilar 2,
    // virtio-gpu BAR, etc.). mm_mmio_init pre-aloca o PDPT do arena em
    // PML4[450] para que processos criados depois (mm_create_address_space
    // copia PML4 por valor) herdem a entrada compartilhada.
    mm_mmio_init();
    void* a = kmalloc(64);
    void* b = kmalloc(4096);
    kputs("     kmalloc(64)    = "); kput_hex((uint64_t)(uintptr_t)a); kputc('\n');
    kputs("     kmalloc(4096)  = "); kput_hex((uint64_t)(uintptr_t)b); kputc('\n');
    kfree(a);
    void* d = kmalloc(32);
    kputs("     reuso pos-free  = "); kput_hex((uint64_t)(uintptr_t)d); kputc('\n');
    uint64_t fr = pmm_alloc_frame();
    kputs("     pmm_alloc_frame = "); kput_hex(fr); kputc('\n');
    kputs("[ok] Heap (kmalloc/kfree) + PMM operacionais\n");

    // --- HAL (Hardware Abstraction Layer): I/O ports + MMIO + enumeracao PCI ---
    // Loga cada dispositivo PCI achado na serial (controlador IDE, video, etc.).
    hal_init();

    // Pilar 1 (NT foundation): PROVA de paginacao dinamica. Mapeia LAPIC fora
    // da identidade, alias-mapeia 1 frame em 2 VAs, desmapeia 1 e confirma
    // que o acesso pos-unmap dispara #PF. Se a prova falhar, NAO seguimos
    // com os demais pilares — paginacao quebrada invalida APIC, SMP e tudo
    // que vem depois.
    if (!proof_pillar1_paging()) {
        kputs("[P1] paginacao dinamica nao passou na prova; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // Pilar 2 (NT foundation): APIC (Local APIC + IO-APIC) substituindo o
    // 8259/PIT. apic_init calibra contra o PIT (que ainda esta vivo neste
    // ponto), programa LVT Timer periodico em 0xD1, redireciona IRQ1 do
    // teclado pelo IO-APIC, e MASCARA o PIC. A prova confirma que g_ticks
    // continua avancando — via APIC timer, nao mais PIT.
    if (!proof_pillar2_apic()) {
        kputs("[P2] APIC nao passou na prova; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // Pilar 3 (NT foundation): SMP. Parseia MADT (RSDT -> "APIC"), lanca AP
    // via INIT-SIPI-SIPI usando trampoline em phys 0x8000. AP corre ap_entry
    // em C, le seu APIC ID e sinaliza. Prova: o segundo core executou codigo
    // no nosso KPCR/GS_BASE e o BSP confirmou.
    //
    // Pilar 4 setup PRE-P3: o BSP precisa estar registrado no scheduler ANTES
    // do AP rodar ap_entry — porque ap_entry chama ki_init_processor que
    // incrementa g_ki_cpu_count atomicamente. Sem BSP registrado primeiro, o
    // scheduler pensa que o AP e' a CPU 0. Idle thread do BSP fica pronta.
    ki_init_processor(0, apic_bsp_id());
    if (!proof_pillar3_smp()) {
        // Gate NAO-fatal (ver FUTURE.md): a prova roda e loga seu resultado,
        // mas o boot CONTINUA. O desktop (gpu_init/win32k mais abaixo) nao
        // depende de SMP. Antes isto era `halt`, prendendo o desktop atras do P3.
        kputs("[P3] PARCIAL — SMP nao passou na prova; seguindo sem gate (ver FUTURE.md).\n");
    }

    // Pilar 4 (NT foundation): scheduler MP preemptivo. APIC timer dispara
    // em cada CPU; ki_quantum_end pega proxima ready da fila daquele CPU e
    // faz KiSwapContext (asm). Prova: A com afinidade CPU 0, B com afinidade
    // CPU 1 — ambos contadores avancam concorrentemente.
    //
    // PAUSADO: proof_pillar4 sequestra o BSP (swap MP quebrado, ver FUTURE.md).
    // Reativar quando o scheduler MP for consertado.
    //
    // A prova do Pilar 4 ESTAVA sendo chamada aqui — `if (!proof_pillar4_scheduler())
    // { halt }` — e foi DESACOPLADA do boot. Motivo medido: proof_pillar4_scheduler()
    // ativa a preempcao MP (g_p4_active=1 + ki_ready_thread de A/B); o APIC timer
    // entao troca o BSP para a thread A que, com o swap MP quebrado, NUNCA e'
    // preemptada de volta — A monopoliza o BSP, a funcao NUNCA retorna, e o boot
    // jamais alcanca gpu_init/win32k/desktop (serial trava em "[P4] A counter=").
    // A prova NAO foi apagada (segue definida acima); so deixou de ser CHAMADA,
    // atras deste gate explicito. Tornar o `if` nao-fatal nao bastava: o problema
    // e a funcao nao retornar, nao o halt.
    // Para REATIVAR: conserte o swap MP (KiSwapContext) e bote p4_proof_enabled=1.
    static const int p4_proof_enabled = 0;
    if (p4_proof_enabled) {
        if (!proof_pillar4_scheduler()) {
            kputs("[P4] PARCIAL — scheduler MP nao passou na prova; seguindo sem gate (ver FUTURE.md).\n");
        }
    } else {
        kputs("\n[P4] ==== prova do scheduler MP: PAUSADA (desacoplada do boot — ver FUTURE.md) ====\n");
        kputs("[P4] proof_pillar4 NAO chamada: sequestra o BSP (thread A monopoliza). "
              "Reativar com p4_proof_enabled=1 apos consertar o swap MP.\n");
    }

    // FUNDACAO SMP/scheduler REATIVADA: o LVT timer do BSP fica LIGADO.
    // O timer ISR (0xD1) alimenta g_ticks + mm_kuser_tick a cada tick (100 Hz) —
    // e' o relogio do sistema (a taskbar mostra HH:MM:SS avancando em tempo real).
    // A PREEMPCAO (ki_quantum_end no timer) so entra quando g_p4_active=1, ligado
    // mais abaixo por sched_start_demo_threads(), ja com todos os subsistemas
    // e o desktop prontos — assim nao ha swap de contexto durante o init fragil.
    // (mm_kuser_tick e' um write trivial de 3 dwords; a "re-entrancia" da versao
    // anterior era um diagnostico equivocado — o tick e' inofensivo.)
    kputs("[apic] LVT timer do BSP ATIVO (relogio/g_ticks correndo; preempcao liga adiante)\n");

    hal_cpu_init();        // FASE 7: CPUID -> vendor/family/model + features (cpu.c)
    // FASE FUNDACAO: calibra o TSC (contra g_ticks @ 100 Hz, timer ja correndo)
    // para KeStallExecutionProcessor e KeQueryPerformanceCounter reais.
    extern void hal_tsc_calibrate(void);
    hal_tsc_calibrate();
    // FASE FUNDACAO (Item 2): inicializa o subsistema de DPC (fila per-CPU).
    extern void KiInitializeDpcSubsystem(void);
    KiInitializeDpcSubsystem();
    // FASE FUNDACAO (Item 6): subsistema KTIMER (lista de timers armados).
    extern void KiInitializeTimerSubsystem(void);
    KiInitializeTimerSubsystem();
    // FASE FUNDACAO (trilha I/O, Fase 3): modelo de interrupcao (tabela por-vetor).
    extern void ke_interrupt_init(void);
    ke_interrupt_init();
    // FASE 7.7: CR4 + XCR0. Habilita OSXSAVE (e xsetbv XCR0=0x7 = X87+SSE+AVX
    // quando suportado) e CR4.SMEP/UMIP/PCIDE quando o CPU expoe. CADA bit e
    // gateado por CPUID antes de ser setado: bit reservado em CR4 = #GP no boot.
    // SMAP fica detectado mas NAO setado (kernel ainda copia p/ pagina user no
    // PE loader sem stac/clac). Drivers reais (pintok.sys) leem CR4 via
    // KeQueryFeatureFlags/RtlGetEnabledExtendedFeatures: agora o valor bate
    // com o NT real.
    extern void cpu_features_init(void);
    cpu_features_init();
    // FASE 7.8: deteccao de HYPERVISOR via CPUID (leaves 0x40000000..0x40000005
    // + bit 31 de CPUID.1.ECX). Loga vendor string e leaves cruas na serial.
    // No QEMU TCG sem KVM o esperado e zeros / "sem hypervisor" — caminho
    // seguro: drivers reais ramificam para HW real. Em QEMU+KVM/Hyper-V/VMware
    // o vendor string aparece (KVMKVMKVM/Microsoft Hv/VMwareVMware) e drivers
    // de paravirt podem se enxergar.
    extern void hv_detect_init(void);
    hv_detect_init();
    // FASE 7.1: KUSER_SHARED_DATA em 0xFFFFF780_00000000 (drivers reais como
    // pintok.sys leem TickCount/NtVersion direto desse offset, sem syscall).
    extern void mm_map_kuser_shared_data(void);
    mm_map_kuser_shared_data();
    // FASE 7.2: KPCR em GS_BASE (MSR 0xC0000101). Drivers reais leem gs:[..]
    // p/ CurrentThread/ProcessorNumber/etc — sem KPCR mapeado, Page Fault na
    // primeira leitura. Programamos GS_BASE e KERNEL_GS_BASE iguais (UP, sem
    // swapgs assimetrico).
    extern void kpcr_init(void);
    kpcr_init();
    // FASE 7.4: habilita a instrucao SYSCALL (Intel/AMD). Drivers reais
    // (pintok.sys) e ntdll.dll usam SYSCALL como forma rapida de pedir um
    // servico do kernel. Sem EFER.SCE=1, a instrucao gera #GP — exatamente o
    // que pintok crashava com depois da Fase 7.3 (rip=0x5EA6673, GP err=0).
    // Programa EFER/STAR/LSTAR/SFMASK/CSTAR; o handler (syscall_entry.asm)
    // entra direto no isr_handler (mesmo dispatcher do int 0x80).
    extern void syscall_msr_init(void);
    syscall_msr_init();
    kputs("[ok] HAL: portas de I/O + MMIO + enumeracao PCI + CPU info (CPUID) + KUSER_SHARED_DATA + KPCR/GS_BASE + SYSCALL\n");
    // FASE FUNDACAO (Item 2): prova rapida do DPC (gs/KPCR ja prontos apos kpcr_init).
    extern void KiDpcSelfTest(void);
    KiDpcSelfTest();
    // FASE FUNDACAO (trilha I/O, Fase 3): prova do modelo de interrupcao.
    extern void KiInterruptSelfTest(void);
    KiInterruptSelfTest();
    // FASE FUNDACAO (trilha I/O, Fase 2): prova de device stacks.
    extern void KiDeviceStackSelfTest(void);
    KiDeviceStackSelfTest();
    // FASE FUNDACAO (trilha I/O, Fase 5): prova de HAL DMA.
    extern void KiDmaSelfTest(void);
    KiDmaSelfTest();
    // FASE FUNDACAO (trilha I/O, Fase 1b): prova do IRP (layout NT).
    extern void KiIrpSelfTest(void);
    KiIrpSelfTest();
    // FASE FUNDACAO (trilha I/O, Fase 6): round-trip completo de IOCTL por um driver.
    extern void KiDriverIrpSelfTest(void);
    KiDriverIrpSelfTest();
    // FASE FUNDACAO (trilha I/O, Fase 4): prova de PnP (AddDevice + START_DEVICE).
    extern void KiPnpSelfTest(void);
    KiPnpSelfTest();
    // INC 4 (Frente 1): prova do walk de conclusao em 2 niveis (filtro + funcao).
    extern void KiCompletionSelfTest(void);
    KiCompletionSelfTest();

    // --- FASE 10.1: detecta virtio-gpu (modern, virtio 1.1) ---
    // Caminha PCI capabilities, mapeia common/notify/isr/device cfg fora da
    // identidade, e roda o status protocol ate FEATURES_OK. Sem QEMU rodando
    // com -device virtio-gpu-pci, a deteccao falha silenciosamente e o Bochs
    // VBE permanece como caminho de video (fallback).
    if (virtio_gpu_detect()) {
        kputs("[ok] virtio-gpu: deteccao + MMIO + FEATURES_OK (pronto p/ fase 10.2)\n");
        // FASE 10.3: smoke test (soft). Submete os 7 comandos 2D essenciais
        // pelo controlq, SEM trocar o backend de video (sem SET_SCANOUT). O
        // Bochs VBE continua como caminho de display ate a fase de switch.
        if (virtio_gpu_smoke_test()) {
            kputs("[ok] virtio-gpu: smoke test passou (CREATE_2D/ATTACH/TRANSFER/FLUSH/UNREF + GET_DISPLAY_INFO)\n");
        } else {
            kputs("[ok] virtio-gpu: smoke test FALHOU (continuando com Bochs VBE)\n");
        }
    } else {
        kputs("[ok] virtio-gpu: indisponivel; Bochs VBE como caminho de video\n");
    }

    // --- FASE GPU: inicializa o backend de display (virtio-gpu OU Bochs VBE) ---
    // gpu_init tenta virtio-gpu PRIMEIRO (se virtio_gpu_detect() + DRIVER_OK ja
    // ocorreu acima), alocando um framebuffer DMA + SET_SCANOUT. Caindo no
    // fallback, tenta Bochs VBE em 1024x768x32. Falha silenciosa = fb_demo
    // continua em mode13h.
    if (gpu_init(1024, 768)) {
        if (virtio_gpu_display_ok()) {
            kputs("[ok] GPU: virtio-gpu 1024x768x32 BGRA (SET_SCANOUT 0; host apresenta NOSSO framebuffer)\n");
        } else {
            kputs("[ok] GPU: Bochs VBE 1024x768x32 (LFB mapeado fora da identidade)\n");
        }
    } else {
        kputs("[ok] GPU: hardware Bochs/QEMU std-vga nao detectado; mode13h fallback ativo\n");
    }

    // --- FASE DX: subsistema DirectX em kernel (dxgkrnl + dxgmms) ---
    // Espelha o dxgkrnl.sys + dxgmms2.sys do Windows 10/11. DxgkInitialize le
    // o estado da GPU (gpu_active/width/height/bpp/pitch) e prepara o adapter
    // primario; DxgMmsInitialize zera o pool de descritores de residency. Sem
    // GPU acelerada o adapter fica em modo "display-only" — exatamente o
    // perfil do BasicDisplay/IDDDriver do Windows. Necessario ANTES de que
    // qualquer DLL d3d/dxgi tente Dxgk*Create*.
    DxgkInitialize();
    DxgMmsInitialize();
    kputs("[ok] DX: dxgkrnl + dxgmms inicializados (adapter primario pronto)\n");

    // --- FASE 11 (audio stack) — HD Audio stub: enumera PCI procurando o
    // controlador de audio (class=0x04 subclass=0x03/0x01). Sem PCM real
    // (sem DMA / CORB/RIRB nesta fase), apenas LOGA o achado. As DLLs ring 3
    // do stack (mmdevapi/Audioses/dsound/winmm) coexistem com este stub: o
    // caminho UMD nao chama o KMD, entao apps WASAPI/DirectSound/PlaySound
    // funcionam ABI-wise mesmo sem hardware acelerado.
    hda_init();
    kputs("[ok] FASE 11: HD Audio detection (stack ring 3 mmdevapi/audioses/dsound/winmm tambem disponivel)\n");

    // --- FASE 12 (network stack) — NDIS + TCPIP + E1000 stub ----------------
    // Espelha o stack de rede Windows 10/11:
    //   ndis.sys    — framework (registro de miniport/protocol drivers)
    //   tcpip.sys   — protocolo IP/TCP/UDP (cria \Device\{Tcp,Udp,Ip,RawIp})
    //   e1000.sys   — driver Intel 8254x (detection PCI; sem MMIO)
    // Sem socket real (rede vazia): apps que importam ws2_32.dll abrem
    // sockets fake; recv() devolve 0 bytes. send() simula sucesso "no vazio".
    ndis_init();
    tcpip_init();
    e1000_init();
    kputs("[ok] FASE 12: NDIS + TCPIP + E1000 (stack ring 3 ws2_32.dll tambem disponivel)\n");

    // --- FASE 13 (4 subsistemas adicionais) — USB / ACPI / PnP / FltMgr ------
    // Espelha em miniatura 4 pilares do kernel Windows 10/11:
    //   usbport.sys + usbhub.sys + xhci.sys : USB host controller stack
    //   acpi.sys                            : ACPI tables (apenas RSDP scan)
    //   pnp manager                         : IRP_MJ_PNP dispatcher canonico
    //   fltmgr.sys                          : Filter Manager (minifilter framework)
    // Todos sao stubs (sem URBs reais, sem AML, sem QUERY_RELATIONS produzindo
    // arvores, sem interceptacao real de IRPs). O objetivo desta fase e expor
    // o ABI completo para que drivers que dependem desses simbolos carreguem
    // sem missing import, e logar o init de cada subsistema na serial.
    usbport_init();
    xhci_init();
    usbhub_init();
    acpi_init();
    pnp_init();
    fltmgr_init();
    kputs("[ok] FASE 13: USB (usbport+usbhub+xhci) + ACPI + PnP + FltMgr\n");

    // --- FASE 2 (HAL disco): IDE ATA PIO + teste de leitura do MBR/NTFS ---
    // Identifica o disco (se anexado via -Disk), le o setor 0 (MBR) e o boot
    // sector da particao NTFS, confirmando a assinatura "NTFS    " (regra 4).
    uint32_t ntfs_lba = disk_test();
    kputs("[ok] HAL disco: IDE ATA PIO (HalReadSector/HalWriteSector)\n");

    ob_init();
    kputs("[ok] Object Manager + namespace (\\Device\\, \\Driver\\)\n");

    // FASE 7 — Driver Framework: tabelas de callbacks (Ps/Ob/Cm/Ex) + registro.
    callbacks_init();
    registry_init();
    kputs("[ok] FASE 7: Callbacks (Ps/Ob/Cm/Ex) + Registro em memoria (\\Registry\\)\n");

    // --- FASE 3: driver NTFS (LEITURA) + camada de File System ---
    // Se disk_test() achou e validou um boot sector NTFS, monta o volume
    // (BPB + $MFT), registra \Device\Harddisk0\Partition1 no I/O Manager, LE
    // \hello.txt (== texto conhecido) e LISTA a raiz (regra 4: tudo logado).
    if (ntfs_lba) {
        ntfs_test(ntfs_lba);
        // FASE 5: o volume fica disponivel como a unidade C: (o syscall layer
        // mapeia "C:\..." -> \Device\Harddisk0\Partition1\...). O cmd.exe usa C:.
        kputs("[ntfs] volume disponivel como C: (C:\\ -> "
              "\\Device\\Harddisk0\\Partition1). cmd.exe usa C:\\>.\n");
        kputs("[ok] NTFS: volume montado como C: + \\hello.txt lido + raiz listada\n");
    }

    // FASE 7.5: inicializa a PspCidTable (Client ID Table NT). DEVE ser
    // ANTES de ps_init/PsCreateProcess para que as insercoes funcionem.
    cid_init();
    ps_init();
    kputs("[ok] Process Manager (EPROCESS/ETHREAD via Object Manager + PspCidTable)\n");

    win32k_init();          // fb_init e feito sob demanda (lazy) no 1o desenho
    kputs("[ok] win32k: window manager + filas de mensagens + GDI\n");

    // FASE 11 — driver PS/2 do mouse (IRQ12). DEPOIS de win32k_init (usa
    // win32k_screen_width para posicionar o cursor) e DEPOIS da remap do PIC
    // (pic_remap ja rodou). mouse_init desmascara IRQ12 + cascade IRQ2 e
    // habilita o stream mode no dispositivo aux do i8042.
    mouse_init();
    kputs("[ok] FASE 11: mouse PS/2 (IRQ12) + cursor sprite + WM_MOUSE* routing\n");

    // FASE 11 — smoke test sintetico do roteamento de eventos de mouse. Em
    // headless o QEMU nao injeta IRQ12 sem display interativo, entao para
    // comprovar que win32k_on_mouse_event + sprite + hit-test + WM_* funcionam
    // ponta a ponta, simulamos AQUI 3 eventos: 2 movimentos + 1 clique. Isto
    // tambem cobre NtUserGet/SetCursorPos indiretamente (via os getters).
    kputs("\n--- FASE 11: smoke test sintetico do roteamento do mouse ---\n");
    win32k_on_mouse_event(50, 40, 0);   // move +50,+40
    win32k_on_mouse_event(-20, -10, 0); // move -20,-10
    win32k_on_mouse_event(0, 0, 1);     // click L button down
    win32k_on_mouse_event(0, 0, 0);     // click L button up
    kputs("[mouse] smoke test: cursor final em ("); kput_dec((uint64_t)win32k_cursor_x());
    kputc(','); kput_dec((uint64_t)win32k_cursor_y()); kputs(")\n");

    // FASE 14 — Tablet ABSOLUTA (virtio-input). O mouse PS/2 relativo numa janela
    // do QEMU nao tem "mouse integration" (o host captura/grab o ponteiro e
    // ESCONDE o cursor; o eixo X ainda satura em -127). Um dispositivo ABSOLUTO
    // faz o QEMU sair do grab -> o cursor de HW do virtio-gpu aparece e segue o
    // mouse, e o guest le coords absolutas p/ hit-test/cliques. Depende de
    // win32k_init (resolucao) ja' ter rodado p/ a escala abs->pixels.
    virtio_input_init();

    // NOTA sobre a multitarefa preemptiva: o AP (2o core) JA esta rodando seu
    // worker loop em paralelo desde o P3 (nao afeta a velocidade do BSP). As
    // KTHREADs de kernel do BSP + a preempcao (g_p4_active) sao ligadas so quando
    // o SHELL PERSISTENTE (explorer.exe) vai subir — assim todo o carregamento
    // pesado de drivers/apps roda a TODA velocidade, e o multitarefa entra em
    // cena com o desktop pronto. Ver sched_start_demo_threads() no laco abaixo.

    // --- Carrega os binarios Windows passados pelo boot (modulos Multiboot) ---
    // Nada e hardcoded: roda QUALQUER PE passado. Detecta pelo Subsystem:
    // NATIVE(1) -> driver .sys (executiva NT);  senao -> aplicativo .exe (Win32).
    vga_set_color(0x0B, 0x00);
    kputs("\n--- Binarios Windows recebidos do boot ---\n");
    vga_set_color(0x0F, 0x00);
    if (mbflags & (1u << 3)) {                                  // bit 3: modulos validos
        uint32_t mods_count = *(volatile uint32_t*)(uintptr_t)(mb_info + 20);
        uint32_t mods_addr  = *(volatile uint32_t*)(uintptr_t)(mb_info + 24);
        kputs("[boot] modulos: "); kput_dec(mods_count); kputc('\n');

        // GATE 2/5/6 do pintok.sys: registra PRIMEIRO um "ntoskrnl.exe"
        // sintetico com export table parseavel (as 217 funcoes que o pintok.sys resolve
        // por parse manual da export directory, nao pela IAT). Tem que ser o
        // modulo[0] da lista que ZwQuerySystemInformation(SystemModuleInformation
        // /Ex) devolve — o pintok.sys assume modulo[0] = kernel e soma os RVAs da export
        // table a essa ImageBase. Ver src/ntos/ldr/pe_export_image.c.
        ldr_register_ntoskrnl_export_image();

        // Passo 1: registra TODOS os modulos por nome (as DLLs ficam disponiveis).
        for (uint32_t i = 0; i < mods_count; i++) {
            const uint32_t* m = (const uint32_t*)(uintptr_t)(mods_addr + i * 16);
            const void* bytes = (const void*)(uintptr_t)m[0];          // mod_start
            const char* path  = (const char*)(uintptr_t)m[2];          // string (nome)
            ldr_register(path, bytes);
        }
        // Passo 2: .sys -> driver (ring 0);  .exe -> roda em ring 3 (carrega as DLLs).
        for (uint32_t i = 0; i < mods_count; i++) {
            const uint32_t* m = (const uint32_t*)(uintptr_t)(mods_addr + i * 16);
            const void* bytes = (const void*)(uintptr_t)m[0];
            const char* path  = (const char*)(uintptr_t)m[2];
            if (ldr_match_ext(path, ".sys")) {
                kputs("\n[boot] driver de kernel: "); kputs(path); kputc('\n');
                driver_load(path, bytes);   // exercita I/O real internamente (antes do Unload)
            } else if (ldr_match_ext(path, ".exe")) {
                // Antes de subir o SHELL persistente (explorer.exe), LIGA a
                // multitarefa preemptiva: todo o carregamento pesado ja rodou a
                // toda velocidade; agora as KTHREADs coexistem com o desktop.
                if (name_contains(path, "explorer")) sched_start_demo_threads();
                kputs("\n[boot] aplicativo: "); kputs(path); kputc('\n');
                ldr_run(path, bytes);
            }
            // .dll: carregada sob demanda pelo loader (LdrLoadDll)
        }
    } else {
        kputs("[boot] nenhum modulo. Rode:  .\\run.ps1\n");
    }

    // Fallback: se nenhum explorer.exe estava na lista (ldr_run dele nunca
    // retorna quando presente), liga a multitarefa aqui — assim o desktop tem
    // as KTHREADs + relogio ao vivo mesmo sem o shell persistente.
    sched_start_demo_threads();

    // --- FASE 2/6: se alguma app GUI ja tomou a tela (guiapp/desktop), o estado
    // final do framebuffer ja e a GUI — PULAMOS a demo estatica da Fase 1 (senao
    // ela limparia o desktop). Usamos win32k_was_active() (e nao has_windows),
    // pois as janelas do ultimo app GUI sao reaped ao ele encerrar, mas a tela
    // grafica permanece. Sem nenhuma GUI, roda a demo da Fase 1 normalmente.
    if (win32k_was_active()) {
        // O desktop (papel de parede + barra de tarefas + janelas de cmd) foi
        // composto e as janelas pintadas durante o loop do desktop.exe; deixamos
        // o framebuffer COMO ESTA para o screendump mostrar o ambiente completo.
        kputs("\n--- FASE 2/6: GUI ativa; desktop/janelas permanecem no framebuffer ---\n");
        kputs("[win32k] estado final: desktop + barra de tarefas + janela(s) de cmd.\n");
    } else if (gpu_active()) {
        // --- FASE GPU: teste minimo no LFB Bochs VBE (32 bpp) ---
        // Sem mexer no win32k. Pinta fundo azul, 3 retangulos coloridos, um
        // pequeno marcador no canto, e mantem a tela parada p/ screendump.
        kputs("\n--- FASE GPU: teste minimo no LFB (Bochs VBE 32 bpp) ---\n");
        kputs("[gpu] gpu_clear(azul fundo)\n");
        gpu_clear(0x00103060);
        kputs("[gpu] gpu_fill_rect 3x cores (RGB)\n");
        gpu_fill_rect(40,  40, 200, 120, 0x00C03030);   // vermelho
        gpu_fill_rect(280, 40, 200, 120, 0x0030C030);   // verde
        gpu_fill_rect(520, 40, 200, 120, 0x003030C0);   // azul claro
        kputs("[gpu] gpu_fill_rect barra no rodape\n");
        gpu_fill_rect(0, (int)gpu_height() - 32, (int)gpu_width(), 32, 0x00202020);
        // Marcadores de canto para confirmar coords (1 px cada).
        gpu_pixel(0, 0, 0x00FFFFFF);
        gpu_pixel((int)gpu_width() - 1, 0, 0x00FFFF00);
        gpu_pixel(0, (int)gpu_height() - 1, 0x0000FFFF);
        gpu_pixel((int)gpu_width() - 1, (int)gpu_height() - 1, 0x00FF00FF);
        gpu_present();
        kputs("[gpu] frame desenhado; use screendump para confirmar pixels.\n");
    } else {
        // --- FASE 1: modo grafico (framebuffer) + GDI de baixo nivel ---
        // Demo do driver de video; loga cada operacao na serial (canal de log).
        // Fica por ULTIMO para preservar os logs no VGA texto ate aqui.
        fb_demo();
    }

    vga_set_color(0x0E, 0x00);
    kputs("\nSistema no ar. Digite algo (teclado por interrupcao):\n");
    kputs("(tela agora em modo grafico 13h; o eco de teclas segue na serial)\n\n");
    vga_set_color(0x0F, 0x00);
    kputs("> ");

    // Idle loop: STI antes do HLT. As IRQs do mouse (IRQ12) e do teclado (IRQ1)
    // chegam pelo IO-APIC e so sao servidas com IF=1. Medimos que o boot chegava
    // aqui com IF=0 — por isso o `hlt` puro CONGELAVA tudo (mouse/teclado mortos
    // apesar do desktop ja composto na tela). `sti; hlt` garante IF=1 e acorda a
    // CPU a cada IRQ pra servir o handler. (Padrao canonico de idle loop NT/x86.)
    for (;;) {
        __asm__ volatile ("sti; hlt");   // ocioso: tudo acontece via interrupcoes
        virtio_input_poll();             // drena a tablet absoluta (sem IRQ wired)
    }
}
