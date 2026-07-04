// ============================================================================
//  ntfs_fs.c  —  Camada de File System do NTFS, ligada ao I/O Manager (FASE 3).
//
//  Registra um DRIVER_OBJECT ("ntfs.sys" sintetico) + um DEVICE_OBJECT de
//  volume (\Device\Harddisk0\Partition1) no Object Manager / I/O Manager,
//  exatamente como um File System Driver do NT. As funcoes de dispatch atendem:
//    - IRP_MJ_CREATE: abre o volume (open do device);
//    - IRP_MJ_READ:   le bytes do $DATA do arquivo associado ao FILE_OBJECT;
//    - IRP_MJ_DIRECTORY_CONTROL: lista uma entrada do diretorio (NtQueryDirectoryFile).
//
//  O parse NTFS em si esta em ntfs.c; aqui so fazemos a ponte com os IRPs e o
//  registro do device, para que NtCreateFile/NtReadFile/NtQueryDirectoryFile
//  no volume passem pelo modelo de driver (DRIVER_OBJECT.MajorFunction + IRPs).
// ============================================================================
#include <stdint.h>
#include "ntddk.h"
#include "filesystems/ntfs/ntfs.h"
#include "io/io.h"
#include "ob/object.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// IRP_MJ_DIRECTORY_CONTROL nao estava no ntddk.h; e 0x0C no NT.
#ifndef IRP_MJ_DIRECTORY_CONTROL
#define IRP_MJ_DIRECTORY_CONTROL 0x0C
#endif

// ---------------------------------------------------------------------------
//  Estado da camada de FS: o DRIVER_OBJECT e o DEVICE_OBJECT do volume.
//  A extensao do device guarda o registro MFT do arquivo aberto + o offset de
//  leitura corrente (para IRP_MJ_READ sequencial) e o indice de diretorio
//  corrente (para enumeracao via DIRECTORY_CONTROL). Versao simplificada: um
//  unico contexto de volume (sem multiplos handles concorrentes), suficiente
//  para a demo (sem escalonador) — o NtCreateFile fixa o alvo do volume.
// ---------------------------------------------------------------------------
typedef struct _NTFS_VOLUME_EXT {
    uint64_t cur_file_record;   // MFT do ultimo arquivo "aberto" no volume
    uint64_t cur_read_offset;   // offset de leitura corrente
    int      dir_index;         // proximo indice a devolver em DIRECTORY_CONTROL
    uint64_t dir_record;        // diretorio sendo enumerado (raiz por padrao)
} NTFS_VOLUME_EXT;

static PDRIVER_OBJECT s_ntfs_driver;
static PDEVICE_OBJECT s_ntfs_volume;

static PDEVICE_OBJECT ntfs_volume_device(void) { return s_ntfs_volume; }

// Layout devolvido por DIRECTORY_CONTROL para CADA entrada (compat com o
// NTFS_FILE_INFO do header). O chamador (NtQueryDirectoryFile) recebe isto.
typedef struct _NTFS_DIR_ENTRY_OUT {
    uint64_t mft_record;
    uint32_t is_dir;
    uint32_t pad;
    uint64_t size;
    char     name[256];
} NTFS_DIR_ENTRY_OUT;

// Contexto p/ capturar a n-esima entrada do diretorio via ntfs_list_dir.
typedef struct { int want; int found; NTFS_FILE_INFO hit; } dir_pick_ctx;
static void dir_pick_cb(int index, const NTFS_FILE_INFO* info, void* ctx) {
    dir_pick_ctx* d = (dir_pick_ctx*)ctx;
    if (index == d->want) { d->found = 1; d->hit = *info; }
}

// ---------------------------------------------------------------------------
//  IRP_MJ_CREATE — "abre" o volume. Sem path por-handle ainda: o open marca o
//  device como pronto e zera o offset/enum. NtCreateFile no \Device\... entra aqui.
// ---------------------------------------------------------------------------
static NTSTATUS __attribute__((ms_abi)) ntfs_create(PDEVICE_OBJECT dev, PIRP irp) {
    NTFS_VOLUME_EXT* ext = (NTFS_VOLUME_EXT*)dev->DeviceExtension;
    if (ext) {
        ext->cur_read_offset = 0;
        ext->dir_index = 0;
        if (ext->dir_record == 0) ext->dir_record = NTFS_MFT_ROOT;
    }
    kputs("[ntfs.sys] IRP_MJ_CREATE: volume aberto.\n");
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS __attribute__((ms_abi)) ntfs_close(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
//  IRP_MJ_READ — le do $DATA. O numero do registro MFT alvo vem em
//  Parameters.Read.Key (reaproveitado como "file id") OU, se 0, usa o
//  cur_file_record da extensao. O offset vem de Parameters.Read.ByteOffset.
//  Preenche o SystemBuffer (METHOD_BUFFERED) com os bytes lidos.
// ---------------------------------------------------------------------------
static NTSTATUS __attribute__((ms_abi)) ntfs_read(PDEVICE_OBJECT dev, PIRP irp) {
    NTFS_VOLUME_EXT* ext = (NTFS_VOLUME_EXT*)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG len = s->Parameters.Read.Length;
    uint64_t off = s->Parameters.Read.ByteOffset;
    uint64_t rec = s->Parameters.Read.Key;
    if (rec == 0 && ext) rec = ext->cur_file_record;

    if (rec == 0 || !irp->AssociatedIrp.SystemBuffer || len == 0) {
        irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
        irp->IoStatus.Information = 0;
        return STATUS_UNSUCCESSFUL;
    }
    kputs("[ntfs.sys] IRP_MJ_READ: MFT #"); kput_dec(rec);
    kputs(" offset="); kput_dec(off); kputs(" len="); kput_dec(len); kputc('\n');

    uint32_t n = ntfs_read_file(rec, off, irp->AssociatedIrp.SystemBuffer, len);
    if (ext) ext->cur_read_offset = off + n;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = n;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
//  IRP_MJ_WRITE — sobrescreve o $DATA do arquivo (FASE 4, escrita segura).
//  O numero do registro MFT alvo vem em Parameters.Write.Key (ou cur_file_record
//  da extensao); o offset em Parameters.Write.ByteOffset; os bytes no SystemBuffer
//  (METHOD_BUFFERED, ja copiados do usuario por io_build_write). Encaminha p/
//  ntfs_write_file (sobrescreve/estende $DATA residente; nao-residente in-place).
//  Information = bytes escritos.
// ---------------------------------------------------------------------------
static NTSTATUS __attribute__((ms_abi)) ntfs_write(PDEVICE_OBJECT dev, PIRP irp) {
    NTFS_VOLUME_EXT* ext = (NTFS_VOLUME_EXT*)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG len = s->Parameters.Write.Length;
    uint64_t off = s->Parameters.Write.ByteOffset;
    uint64_t rec = s->Parameters.Write.Key;
    if (rec == 0 && ext) rec = ext->cur_file_record;

    if (rec == 0 || !irp->AssociatedIrp.SystemBuffer || len == 0) {
        irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
        irp->IoStatus.Information = 0;
        return STATUS_UNSUCCESSFUL;
    }
    kputs("[ntfs.sys] IRP_MJ_WRITE: MFT #"); kput_dec(rec);
    kputs(" offset="); kput_dec(off); kputs(" len="); kput_dec(len); kputc('\n');

    // set_eof=0 (sobrescrita/extensao automatica se passar do fim); parent=0
    // (NtWriteFile nao sabe o diretorio pai — atualiza so o $DATA do arquivo).
    uint32_t n = ntfs_write_file(rec, off, irp->AssociatedIrp.SystemBuffer, len, /*set_eof*/0, /*parent*/0);
    if (ext) ext->cur_read_offset = off + n;
    irp->IoStatus.Status = (n > 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    irp->IoStatus.Information = n;
    return irp->IoStatus.Status;
}

// ---------------------------------------------------------------------------
//  IRP_MJ_DIRECTORY_CONTROL — devolve UMA entrada do diretorio por chamada
//  (estilo NtQueryDirectoryFile com ReturnSingleEntry). O indice corrente vem
//  da extensao (avanca a cada chamada). Escreve um NTFS_DIR_ENTRY_OUT no
//  SystemBuffer; Information>0 = entrega; Information==0 = fim (sem mais).
// ---------------------------------------------------------------------------
static NTSTATUS __attribute__((ms_abi)) ntfs_directory_control(PDEVICE_OBJECT dev, PIRP irp) {
    NTFS_VOLUME_EXT* ext = (NTFS_VOLUME_EXT*)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG outlen = s->Parameters.Read.Length;   // tamanho do buffer de saida
    uint64_t dirrec = ext ? ext->dir_record : NTFS_MFT_ROOT;
    if (dirrec == 0) dirrec = NTFS_MFT_ROOT;

    if (!irp->AssociatedIrp.SystemBuffer || outlen < sizeof(NTFS_DIR_ENTRY_OUT)) {
        irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
        irp->IoStatus.Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    int want = ext ? ext->dir_index : 0;
    dir_pick_ctx d; d.want = want; d.found = 0;
    ntfs_list_dir(dirrec, dir_pick_cb, &d);

    if (!d.found) {
        irp->IoStatus.Status = (NTSTATUS)0x80000006;   // STATUS_NO_MORE_FILES
        irp->IoStatus.Information = 0;
        return irp->IoStatus.Status;
    }

    NTFS_DIR_ENTRY_OUT* o = (NTFS_DIR_ENTRY_OUT*)irp->AssociatedIrp.SystemBuffer;
    o->mft_record = d.hit.mft_record;
    o->is_dir = (uint32_t)d.hit.is_dir;
    o->pad = 0;
    o->size = d.hit.size;
    int i = 0;
    for (; d.hit.name[i] && i < (int)sizeof(o->name) - 1; i++) o->name[i] = d.hit.name[i];
    o->name[i] = 0;

    if (ext) ext->dir_index = want + 1;
    kputs("[ntfs.sys] IRP_MJ_DIRECTORY_CONTROL: entrada ["); kput_dec(want);
    kputs("] = "); kputs(o->name); kputc('\n');
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = sizeof(NTFS_DIR_ENTRY_OUT);
    return STATUS_SUCCESS;
}

// Aponta o contexto do volume para um arquivo/diretorio (usado pelo NtCreateFile
// quando o caminho inclui um arquivo dentro do volume). Exposto via ntfs.h-less
// helper para o syscall layer chamar apos resolver o caminho.
void ntfs_fs_set_target(uint64_t mft_record, int is_dir) {
    if (!s_ntfs_volume) return;
    NTFS_VOLUME_EXT* ext = (NTFS_VOLUME_EXT*)s_ntfs_volume->DeviceExtension;
    if (!ext) return;
    if (is_dir) {
        // So REINICIA o cursor de enumeracao quando o diretorio MUDA (abrir um
        // novo dir). Apontar repetidamente para o MESMO dir (cada chamada de
        // NtQueryDirectoryFile) NAO zera o indice — senao a enumeracao trava
        // sempre na entrada [0].
        if (ext->dir_record != mft_record) { ext->dir_record = mft_record; ext->dir_index = 0; }
    } else {
        ext->cur_file_record = mft_record; ext->cur_read_offset = 0;
    }
}

// Devolve o device de volume (para o syscall layer montar o caminho do volume).
PDEVICE_OBJECT ntfs_fs_volume_device(void) { return ntfs_volume_device(); }

// 1 se o device de volume ja foi registrado no I/O Manager.
int ntfs_fs_registered(void) { return s_ntfs_volume != 0; }

// ---------------------------------------------------------------------------
//  FsMountVolume — monta o NTFS (ntfs_mount) e registra DRIVER_OBJECT +
//  DEVICE_OBJECT do volume no I/O Manager. Idempotente.
// ---------------------------------------------------------------------------
int FsMountVolume(uint32_t part_lba) {
    if (s_ntfs_volume) return 1;   // ja montado/registrado

    if (!ntfs_mount(part_lba)) return 0;

    // Cria o DRIVER_OBJECT do file system driver (estilo ntfs.sys).
    s_ntfs_driver = (PDRIVER_OBJECT)ObCreateObject(OB_TYPE_DRIVER, sizeof(DRIVER_OBJECT),
                                                   "\\FileSystem\\Ntfs");
    if (!s_ntfs_driver) { kputs("[ntfs.sys] sem memoria p/ o DRIVER_OBJECT.\n"); return 0; }
    s_ntfs_driver->Type = 4;
    s_ntfs_driver->Size = (SHORT)sizeof(DRIVER_OBJECT);
    s_ntfs_driver->MajorFunction[IRP_MJ_CREATE]            = (PVOID)ntfs_create;
    s_ntfs_driver->MajorFunction[IRP_MJ_CLOSE]             = (PVOID)ntfs_close;
    s_ntfs_driver->MajorFunction[IRP_MJ_READ]              = (PVOID)ntfs_read;
    s_ntfs_driver->MajorFunction[IRP_MJ_WRITE]             = (PVOID)ntfs_write;
    s_ntfs_driver->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = (PVOID)ntfs_directory_control;

    // Cria o DEVICE_OBJECT do volume com a extensao de contexto.
    PDEVICE_OBJECT dev = 0;
    NTSTATUS st = io_create_device(s_ntfs_driver, sizeof(NTFS_VOLUME_EXT),
                                   NTFS_VOLUME_DEVICE, FILE_DEVICE_UNKNOWN, &dev);
    if (st != STATUS_SUCCESS || !dev) {
        kputs("[ntfs.sys] falha ao criar o device de volume.\n");
        return 0;
    }
    NTFS_VOLUME_EXT* ext = (NTFS_VOLUME_EXT*)dev->DeviceExtension;
    if (ext) { ext->cur_file_record = 0; ext->cur_read_offset = 0;
               ext->dir_index = 0; ext->dir_record = NTFS_MFT_ROOT; }
    s_ntfs_volume = dev;

    kputs("[ntfs.sys] File System Driver registrado: device '"
          NTFS_VOLUME_DEVICE "' no I/O Manager.\n");
    kputs("[ntfs.sys]   MajorFunction: CREATE/CLOSE/READ/DIRECTORY_CONTROL atendidos via IRP.\n");
    return 1;
}
