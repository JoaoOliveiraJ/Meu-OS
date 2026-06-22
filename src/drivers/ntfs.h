#pragma once
#include <stdint.h>

// ============================================================================
//  ntfs.h  —  Driver NTFS (LEITURA) + camada de File System (FASE 3).
//
//  Monta um volume NTFS lido pela HAL de disco (src/hal/disk.c, IDE ATA PIO) e
//  implementa a LEITURA do sistema de arquivos mais complexo que existe:
//    - parse do boot sector (BPB): bytes/setor, setores/cluster, LCN do $MFT,
//      tamanho do registro MFT;
//    - leitura do $MFT e parse de um registro MFT (header FILE + fixups USA +
//      atributos residentes e nao-residentes com data runs);
//    - $STANDARD_INFORMATION, $FILE_NAME (nomes UTF-16), $DATA (residente e
//      nao-residente via data runs), $INDEX_ROOT/$INDEX_ALLOCATION (diretorios);
//    - resolucao de caminho (\dir\arquivo -> numero de registro MFT -> $DATA).
//
//  Camada de FS: registra um DRIVER_OBJECT + um DEVICE_OBJECT de volume
//  (\Device\Harddisk0\Partition1) no I/O Manager, atendendo IRP_MJ_CREATE/READ/
//  DIRECTORY_CONTROL — exatamente como um File System Driver do NT. As syscalls
//  NtCreateFile/NtReadFile/NtQueryDirectoryFile no volume passam por ele.
//
//  Regra 4: cada operacao (MFT, atributos, arquivo lido, bytes) e logada na
//  serial ([ntfs] ...) para comprovar a leitura em headless.
// ============================================================================

#define NTFS_MAX_PATH    255   // caminho NTFS (em chars)
#define NTFS_MAX_NAME    255   // nome de um componente

// Numeros de registro MFT fixos do NTFS (layout padrao).
#define NTFS_MFT_ROOT    5     // diretorio raiz '.'

// Estado de um volume NTFS montado (BPB + geometria, em bytes/clusters).
typedef struct _NTFS_VOLUME {
    int       mounted;             // 1 se montado com sucesso
    uint32_t  part_lba;            // LBA inicial da particao no disco (offset)
    uint16_t  bytes_per_sector;    // BPB: bytes por setor (512)
    uint8_t   sectors_per_cluster; // BPB: setores por cluster (8 = 4 KiB)
    uint32_t  bytes_per_cluster;   // derivado
    uint64_t  total_sectors;       // BPB: total de setores do volume
    uint64_t  mft_lcn;             // BPB: LCN do inicio do $MFT
    uint64_t  mftmirr_lcn;         // BPB: LCN do $MFTMirr
    uint32_t  mft_record_size;     // tamanho de um registro MFT (1024)
    uint32_t  index_record_size;   // tamanho de um index record ($INDEX_ALLOCATION)
    uint64_t  mft_data_lcn;        // LCN onde o $DATA do $MFT comeca (=mft_lcn)
    uint64_t  serial;              // BPB @0x48: numero de serie do volume
} NTFS_VOLUME;

// Resumo do volume montado para NtQueryVolumeInformation (FASE 5). Tamanhos em
// bytes; o "free" e estimado (nao varremos o $Bitmap — ver nota em ntfs.c).
typedef struct _NTFS_VOLUME_INFO {
    uint64_t serial;            // numero de serie do volume
    uint64_t total_bytes;       // tamanho total do volume (total_setores * bytes/setor)
    uint64_t free_bytes;        // bytes livres (estimativa)
    uint32_t bytes_per_sector;  // BPB
    uint32_t bytes_per_cluster; // derivado
    char     fs_name[8];        // "NTFS"
    char     label[32];         // rotulo do volume (ASCII; do $Volume se houver)
} NTFS_VOLUME_INFO;

// Informacao de um arquivo/diretorio resolvido (de um registro MFT).
typedef struct _NTFS_FILE_INFO {
    uint64_t mft_record;     // numero do registro MFT
    int      is_dir;         // 1 se diretorio
    uint64_t size;           // tamanho do $DATA (real size), 0 p/ diretorio
    char     name[NTFS_MAX_NAME + 1];  // nome (ASCII; UTF-16 convertido)
} NTFS_FILE_INFO;

// ----------------------------------------------------------------------------
//  Montagem / API de leitura (lado kernel).
// ----------------------------------------------------------------------------

// Monta o volume NTFS cuja particao comeca em 'part_lba' no disco IDE (0 = LBA
// 0, superfloppy). Le o boot sector, valida a assinatura "NTFS    " e preenche
// o BPB. Retorna 1 em sucesso, 0 em falha. Loga cada campo do BPB.
int ntfs_mount(uint32_t part_lba);

// 1 se ha um volume NTFS montado.
int ntfs_mounted(void);

// Preenche *out com o resumo do volume montado (serial, tamanho total/livre, fs
// name "NTFS", rotulo). Retorna 1 se ha volume montado, 0 senao. FASE 5: apoia
// NtQueryVolumeInformation.
int ntfs_volume_info(NTFS_VOLUME_INFO* out);

// Resolve um caminho NTFS ('\hello.txt', '\dir1\file.txt', '\' = raiz) para o
// seu registro MFT, preenchendo *out. Retorna 1 se achou, 0 senao.
int ntfs_resolve_path(const char* path, NTFS_FILE_INFO* out);

// Le ate 'len' bytes do $DATA do arquivo no registro MFT 'mft_record', a partir
// de 'offset', para 'buf'. Retorna o numero de bytes lidos (0 em EOF/erro).
// Trata $DATA residente E nao-residente (data runs). Loga os bytes lidos.
uint32_t ntfs_read_file(uint64_t mft_record, uint64_t offset, void* buf, uint32_t len);

// Lista o diretorio no registro MFT 'dir_record'. Para CADA entrada chama o
// callback (index sobe 0,1,2,...). Retorna o numero de entradas listadas.
// Le $INDEX_ROOT e, se houver, $INDEX_ALLOCATION (data runs).
typedef void (*ntfs_dir_cb)(int index, const NTFS_FILE_INFO* info, void* ctx);
int ntfs_list_dir(uint64_t dir_record, ntfs_dir_cb cb, void* ctx);

// ----------------------------------------------------------------------------
//  FASE 4 — ESCRITA NTFS (subconjunto SEGURO).
//
//  Implementamos a escrita que NAO precisa alocar clusters novos nem mexer no
//  $Bitmap/$LogFile (o caminho de risco). O que e seguro e exercitado:
//
//   1) ntfs_write_file: sobrescreve o $DATA de um arquivo EXISTENTE.
//      - $DATA RESIDENTE: novo conteudo de tamanho <= espaco do registro MFT.
//        Se o novo tamanho couber no slack do registro (1024 bytes), tambem
//        CRESCE/ENCURTA o arquivo, atualizando os campos de tamanho do atributo,
//        do registro (bytes used) e do $FILE_NAME, alem da entrada no $INDEX do
//        diretorio pai. Reescreve o registro MFT no disco com os fixups (USA).
//      - $DATA NAO-RESIDENTE: reescreve bytes nos clusters JA ALOCADOS (tamanho
//        <= tamanho real atual), via HalWriteSector — sem alocar clusters novos.
//      Retorna o numero de bytes escritos (0 em erro). Loga cada operacao.
//
//  O que fica de fora (relatado como blocker): criar/excluir arquivo/diretorio,
//  alocacao de clusters ($Bitmap), crescer um $DATA nao-residente, journaling
//  ($LogFile). Esses exigem o caminho de alocacao, arriscado de fechar com o
//  boot verde; o subconjunto acima e seguro e prova a ESCRITA ponta a ponta.
// ----------------------------------------------------------------------------

// Reescreve o registro MFT 'record_no' (buf tem mft_record_size bytes ja com o
// header FILE) no disco, (re)aplicando os fixups (USA) ANTES da escrita.
// Retorna 0 em sucesso. Usado internamente pela escrita; exposto p/ depuracao.
int ntfs_write_mft_record(uint64_t record_no, uint8_t* buf);

// Sobrescreve o $DATA do arquivo no registro MFT 'mft_record' com 'len' bytes de
// 'buf', a partir de 'offset'. Para $DATA residente, se 'set_eof' for 1 e o novo
// fim (offset+len) couber no registro, ATUALIZA o tamanho do arquivo (cresce ou
// encurta) — incluindo o $FILE_NAME e a entrada no $INDEX do diretorio pai
// 'parent_record' (passe 0 para nao atualizar o indice). Retorna bytes escritos.
uint32_t ntfs_write_file(uint64_t mft_record, uint64_t offset,
                         const void* buf, uint32_t len,
                         int set_eof, uint64_t parent_record);

// 1 se o volume montado e somente-leitura (montagem nao confirmou escrita). Por
// ora sempre 0 (a HAL tem HalWriteSector). Reservado p/ evoluir.
int ntfs_readonly(void);

// ----------------------------------------------------------------------------
//  CRIAR / EXCLUIR arquivo e diretorio (subconjunto SEGURO, SEM alocacao de
//  clusters): usa um registro MFT livre (nao-em-uso) e mantem o $DATA do novo
//  arquivo RESIDENTE; a entrada do diretorio pai vive no $INDEX_ROOT residente
//  (que tem folga no registro de 1024 B). Cada operacao loga na serial.
//
//  Limitacao honesta: NAO atualizamos o $BITMAP do $MFT nem o tamanho logico do
//  $DATA do $MFT (o Windows real exige isso p/ "alocar" o registro). Para o NOSSO
//  leitor (ntfs_read_mft_record le por offset linear + valida 'FILE'), o registro
//  passa a existir e a entrada no $INDEX o torna visivel — round-trip comprovado.
// ----------------------------------------------------------------------------

// Cria um arquivo (is_dir=0) ou diretorio (is_dir=1) chamado 'name' dentro do
// diretorio 'parent_record', com 'content' (len bytes) como $DATA residente (use
// len=0 p/ diretorio). Escolhe um registro MFT livre. Em sucesso, preenche
// *out_record com o numero do registro novo e retorna 1. Loga cada etapa.
int ntfs_create_file(uint64_t parent_record, const char* name, int is_dir,
                     const void* content, uint32_t len, uint64_t* out_record);

// Exclui (marca como nao-em-uso + remove do $INDEX do pai) o arquivo/diretorio
// no registro 'child_record', cujo nome no pai 'parent_record'. Retorna 1 em
// sucesso. Nao libera clusters (arquivos da imagem de teste sao residentes).
int ntfs_delete_file(uint64_t parent_record, uint64_t child_record);

// ----------------------------------------------------------------------------
//  Camada de File System: registra o DRIVER_OBJECT + DEVICE_OBJECT de volume
//  (\Device\Harddisk0\Partition1) no I/O Manager. Chamar APOS ob_init e
//  ntfs_mount. Depois disso, NtCreateFile/NtReadFile/NtQueryDirectoryFile
//  resolvem no volume via IRP. Retorna 1 em sucesso.
// ----------------------------------------------------------------------------
int FsMountVolume(uint32_t part_lba);

// Nome do device de volume registrado (para NtCreateFile montar o caminho).
#define NTFS_VOLUME_DEVICE "\\Device\\Harddisk0\\Partition1"

// 1 se o device de volume NTFS ja foi registrado no I/O Manager (FsMountVolume).
int ntfs_fs_registered(void);

// Aponta o contexto do volume para um arquivo (is_dir=0) ou diretorio (is_dir=1)
// resolvido por caminho. Usado pelo syscall layer (NtCreateFile no volume) para
// que IRP_MJ_READ/DIRECTORY_CONTROL subsequentes mirem o alvo certo.
void ntfs_fs_set_target(uint64_t mft_record, int is_dir);
