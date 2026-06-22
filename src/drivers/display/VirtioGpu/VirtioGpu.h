#pragma once
#include <stdint.h>

// ============================================================================
//  VirtioGpu.h - Driver virtio-gpu PCI (modern transport, virtio 1.1 spec).
//
//  FASE 10.1: deteccao + capability walk + MMIO map + status protocol
//             (ACK -> DRIVER -> FEATURES_OK).
//
//  FASE 10.2: virtqueue setup (controlq + cursorq) + DRIVER_OK.
//             Aloca contiguamente desc/avail/used via pmm_alloc_contiguous (a
//             identidade de 1 GiB cobre os enderecos retornados, entao phys e
//             virt sao iguais). Inicializa a free list de descriptors, escreve
//             os enderecos fisicos em queue_desc/driver/device, e habilita as
//             duas queues. Termina setando o bit DRIVER_OK em device_status.
//
//  PCI IDs:
//    vendor=0x1AF4 (Red Hat virtio)
//    device=0x1050 (modern virtio-gpu) ou 0x1040..0x107F com subsystem_id=0x10
//    subsystem_id=0x10 = VIRTIO_ID_GPU (16)
//
//  Fallback: se este device nao for achado, gpu.c continua usando Bochs VBE.
// ============================================================================

#define VIRTIO_PCI_VENDOR       0x1AF4
#define VIRTIO_GPU_DEVICE_MODERN 0x1050
#define VIRTIO_GPU_DEVICE_LEGACY 0x1040   // transitional/legacy base
#define VIRTIO_GPU_DEVICE_LEGACY_MAX 0x107F
#define VIRTIO_ID_GPU           0x10      // subsystem ID

// PCI capability ID for "vendor specific"
#define PCI_CAP_ID_VENDOR       0x09

// virtio config types (cfg_type field in capability)
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

// Device status bits
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_NEEDS_RESET   0x40
#define VIRTIO_STATUS_FAILED        0x80

// Feature bit 32 = VIRTIO_F_VERSION_1 (modern virtio).
// Quando reportado via select=1, fica no bit 0 da janela alta.
#define VIRTIO_F_VERSION_1_BIT  (1u << 0)   // dentro do select=1

// ----------------------------------------------------------------------------
//  FASE 10.2: virtqueue (virtio 1.1, secao 2.6 split virtqueues).
//
//  Cada queue tem 3 areas que o device le/escreve via DMA:
//    - desc table:  array de N x 16 bytes (descriptors).
//    - avail ring:  driver -> device. Lista os heads dos chains submetidos.
//    - used ring:   device -> driver. Lista os heads dos chains completados.
//
//  Para uma queue de 64 entradas, os tamanhos sao:
//    desc:  64 * 16  = 1024 B  (cabe em 1 pagina de 4 KiB)
//    avail: 4 + 64*2 + 2 = 134 B  (1 pagina)
//    used:  4 + 64*8 + 2 = 518 B  (1 pagina)
//  Alocamos 1 pagina por area (pmm_alloc_contiguous(1)) — desperdico minusculo,
//  alinhamento natural a 4 KiB (a spec exige alinhamento >= 16/2/4 bytes para
//  desc/avail/used; 4 KiB satisfaz todos).
// ----------------------------------------------------------------------------
#define VIRTIO_VQ_SIZE  64

// Flags de descriptor (struct vq_desc.flags).
#define VIRTQ_DESC_F_NEXT      0x1   // ha desc[next] na cadeia (entradas in/out)
#define VIRTQ_DESC_F_WRITE     0x2   // device escreve neste descriptor (resposta)
#define VIRTQ_DESC_F_INDIRECT  0x4   // o desc aponta para uma tabela indireta

// Estado interno expostito p/ outros TUs do driver (VirtioGpu_cmd.c etc no futuro).
struct vq_desc;
struct vq_avail;
struct vq_used;
typedef struct VIRTIO_QUEUE {
    volatile struct vq_desc*   desc;   // 64 x 16 B  (phys em queue_desc)
    volatile struct vq_avail*  avail;  // ring de heads enviados ao device
    volatile struct vq_used*   used;   // ring de respostas do device
    uint64_t                   desc_phys;
    uint64_t                   avail_phys;
    uint64_t                   used_phys;
    uint16_t                   size;            // efetivo (min(VQ_SIZE, queue_size))
    uint16_t                   last_used_idx;   // ultimo idx do used que processamos
    uint16_t                   notify_off;      // queue_notify_off lido no select
    volatile uint16_t*         notify_reg;      // endereco MMIO p/ notificar este queue
    uint16_t                   next_free_desc;  // head da free list (0xFFFF = vazia)
    uint16_t                   num_free;        // descriptors livres na cadeia
    uint16_t                   queue_idx;       // 0=controlq, 1=cursorq
} VIRTIO_QUEUE;

// Indices fixos por spec do virtio-gpu (secao 5.7).
#define VIRTIO_GPU_CONTROLQ_IDX  0
#define VIRTIO_GPU_CURSORQ_IDX   1

// Sentinel da free list (cabeca aponta aqui = sem descriptors).
#define VIRTQ_DESC_FREE_END  0xFFFF

// ============================================================================
//  FASE 10.3 — virtio-gpu 2D command set (spec sec. 5.7.6).
//
//  Codigos sao little-endian dentro do payload; o device retorna RESP_OK_NODATA
//  (0x1100) ou RESP_OK_DISPLAY_INFO (0x1101) em caso de sucesso; qualquer
//  ERR_* na faixa 0x1200..0x12FF e erro.
// ============================================================================
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC             0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY      0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203

#define VIRTIO_GPU_MAX_SCANOUTS                16

// Formatos pixel (spec sec. 5.7.6.2). Usamos BGRA8 = 1 (compatibilidade com o
// pitch padrao do Bochs VBE existente).
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM       1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM       3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM       4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM       67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM       68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM       121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM       134

// ----------------------------------------------------------------------------
//  API publica
// ----------------------------------------------------------------------------
int      virtio_gpu_detect(void);
int      virtio_gpu_active(void);
uint32_t virtio_gpu_features_lo(void);
uint32_t virtio_gpu_features_hi(void);

// FASE 10.2: acessores para diagnostico (e p/ submeter comandos nas proximas
// fases). Retornam NULL se a fase ainda nao concluiu.
VIRTIO_QUEUE* virtio_gpu_controlq(void);
VIRTIO_QUEUE* virtio_gpu_cursorq(void);

// FASE 10.3: comandos 2D individuais. Cada um devolve o response.type lido do
// device (RESP_OK_NODATA = 0x1100 em sucesso). Retornam 0 se driver inativo.
uint32_t virtio_gpu_cmd_get_display_info(uint32_t* out_w, uint32_t* out_h,
                                         uint32_t* out_enabled);
uint32_t virtio_gpu_cmd_resource_create_2d(uint32_t resource_id, uint32_t format,
                                           uint32_t width, uint32_t height);
uint32_t virtio_gpu_cmd_resource_unref(uint32_t resource_id);
uint32_t virtio_gpu_cmd_resource_attach_backing(uint32_t resource_id,
                                                uint64_t phys_addr, uint32_t length);
uint32_t virtio_gpu_cmd_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height);
uint32_t virtio_gpu_cmd_transfer_to_host_2d(uint32_t resource_id,
                                            uint64_t offset,
                                            uint32_t x, uint32_t y,
                                            uint32_t width, uint32_t height);
uint32_t virtio_gpu_cmd_resource_flush(uint32_t resource_id,
                                       uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height);

// Smoke test ("soft"): GET_DISPLAY_INFO + CREATE_2D + ATTACH_BACKING +
// TRANSFER + FLUSH. Nao chama SET_SCANOUT (nao trocamos o backend ainda).
// Devolve 1 se todos os comandos voltaram RESP_OK_NODATA/OK_DISPLAY_INFO.
int      virtio_gpu_smoke_test(void);

// ============================================================================
//  FASE 10.4 — Display init (SET_SCANOUT) e present.
//
//  virtio_gpu_init_display(w, h):
//    Aloca um framebuffer DMA contiguo (w*h*4 bytes, BGRA), cria o recurso 2D
//    no host (resource_id=1), anexa o backing fisico, e faz SET_SCANOUT no
//    scanout 0. Em sucesso, o host passa a apresentar este FB na tela: o
//    driver escreve nos pixels diretamente; chamar virtio_gpu_present() faz
//    o re-flush (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH).
//    Devolve 1 em sucesso, 0 se algum comando falhou (caller faz fallback).
//
//  virtio_gpu_present():
//    Re-publica o FB completo: TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
//
//  virtio_gpu_display_ok():
//    1 se SET_SCANOUT concluiu — usado pelo gpu.c para escolher o backend.
//
//  virtio_gpu_framebuffer():
//    Ponteiro VIRTUAL para o framebuffer (identidade-mapeado, virt == phys).
//    O caller escreve aqui em formato BGRA8 little-endian (0x00RRGGBB para
//    cores opacas; alpha=0 e suficiente pois nao componizamos com host).
// ============================================================================
int      virtio_gpu_init_display(uint32_t width, uint32_t height);
void     virtio_gpu_present(void);
int      virtio_gpu_display_ok(void);
uint32_t virtio_gpu_fb_width(void);
uint32_t virtio_gpu_fb_height(void);
uint32_t virtio_gpu_fb_pitch(void);
volatile uint32_t* virtio_gpu_framebuffer(void);
