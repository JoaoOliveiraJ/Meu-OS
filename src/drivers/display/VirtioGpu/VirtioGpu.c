// ============================================================================
//  VirtioGpu.c - Driver virtio-gpu PCI (modern transport, virtio 1.1 spec).
//
//  FASE 10.1: deteccao + capability walk + MMIO map + status protocol.
//  FASE 10.2: virtqueue setup (controlq + cursorq) + DRIVER_OK.
//
//  Sequencia (virtio 1.1 spec, secao 3.1):
//    1) Reset:    device_status = 0
//    2) ACK:      device_status |= ACKNOWLEDGE
//    3) DRIVER:   device_status |= DRIVER
//    4) Features negociadas (VIRTIO_F_VERSION_1).
//    5) FEATURES_OK setado e re-lido (FASE 10.1 ate aqui).
//    6) Setup das virtqueues: para cada queue idx,
//         a) common->queue_select = idx
//         b) le common->queue_size, queue_notify_off
//         c) aloca desc/avail/used (pmm_alloc_contiguous(1) cada)
//         d) zera as 3 areas, monta a free list dos descriptors
//         e) escreve phys em queue_desc/driver/device
//         f) common->queue_enable = 1
//    7) device_status |= DRIVER_OK  -> device entra em operacao.
//
//  Como a identidade de 1 GiB (do mm/paging.c) ja cobre todo o range usado pelo
//  PMM (PMM_BASE = 64 MiB, MAX_PHYS = 1 GiB), os enderecos FISICOS retornados
//  por pmm_alloc_contiguous valem TAMBEM como virtuais — escrever em
//  (void*)phys e ler do device em queue_desc=phys batem.
//
//  Sem WebFetch: tudo do conhecimento publico do virtio 1.1 spec.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "display/VirtioGpu/VirtioGpu.h"
#include "hal/hal.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// memset minimo p/ zerar areas DMA (evita pull do libc; main.c tem um global,
// mas nao podemos depender de ordem de link aqui).
static void vq_bzero(volatile void* p, uint64_t n) {
    volatile uint8_t* q = (volatile uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) q[i] = 0;
}

// --- struct layouts (virtio 1.1) ------------------------------------------
typedef struct __attribute__((packed)) VIRTIO_PCI_CAP {
    uint8_t  cap_vndr;       // 0x09 vendor-specific
    uint8_t  cap_next;       // next cap offset (0 = end)
    uint8_t  cap_len;        // length of this cap (>=16)
    uint8_t  cfg_type;       // VIRTIO_PCI_CAP_*
    uint8_t  bar;            // BAR index (0..5)
    uint8_t  padding[3];
    uint32_t offset;         // offset within BAR
    uint32_t length;         // length of region
} VIRTIO_PCI_CAP;

typedef struct __attribute__((packed)) VIRTIO_COMMON_CFG {
    uint32_t device_feature_select;   // 0x00
    uint32_t device_feature;          // 0x04 RO
    uint32_t driver_feature_select;   // 0x08
    uint32_t driver_feature;          // 0x0C
    uint16_t msix_config;             // 0x10
    uint16_t num_queues;              // 0x12 RO
    uint8_t  device_status;           // 0x14
    uint8_t  config_generation;       // 0x15 RO
    uint16_t queue_select;            // 0x16
    uint16_t queue_size;              // 0x18
    uint16_t queue_msix_vector;       // 0x1A
    uint16_t queue_enable;            // 0x1C
    uint16_t queue_notify_off;        // 0x1E RO
    uint64_t queue_desc;              // 0x20
    uint64_t queue_driver;            // 0x28
    uint64_t queue_device;            // 0x30
} VIRTIO_COMMON_CFG;
// Sanity: ABI esperada para o common config block.
_Static_assert(sizeof(VIRTIO_COMMON_CFG) == 0x38, "VIRTIO_COMMON_CFG layout");

// --- virtqueue split layout (virtio 1.1, sec 2.6) -------------------------
struct __attribute__((packed)) vq_desc {
    uint64_t addr;     // endereco fisico do buffer
    uint32_t len;
    uint16_t flags;    // VIRTQ_DESC_F_*
    uint16_t next;     // proximo na cadeia (se flags & NEXT)
};
_Static_assert(sizeof(struct vq_desc) == 16, "vq_desc layout (16B)");

struct __attribute__((packed)) vq_avail {
    uint16_t flags;
    uint16_t idx;                          // sempre crescente; modulo size na leitura
    uint16_t ring[VIRTIO_VQ_SIZE];
    uint16_t used_event;                   // event index (avail_event no spec)
};
_Static_assert(sizeof(struct vq_avail) == 2 + 2 + VIRTIO_VQ_SIZE * 2 + 2,
               "vq_avail layout");

struct __attribute__((packed)) vq_used_elem {
    uint32_t id;       // head_idx do chain completado
    uint32_t len;      // bytes escritos pelo device
};
_Static_assert(sizeof(struct vq_used_elem) == 8, "vq_used_elem layout (8B)");

struct __attribute__((packed)) vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[VIRTIO_VQ_SIZE];
    uint16_t avail_event;
};
_Static_assert(sizeof(struct vq_used) == 2 + 2 + VIRTIO_VQ_SIZE * 8 + 2,
               "vq_used layout");

// --- virtio-gpu 2D command payload layouts (spec sec. 5.7.6) ---------------
struct __attribute__((packed)) virtio_gpu_ctrl_hdr {
    uint32_t type;        // VIRTIO_GPU_CMD_* (req) ou VIRTIO_GPU_RESP_* (resp)
    uint32_t flags;       // bit 0 = FENCE
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};
_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24, "ctrl_hdr 24B");

struct __attribute__((packed)) virtio_gpu_rect {
    uint32_t x, y, width, height;
};
_Static_assert(sizeof(struct virtio_gpu_rect) == 16, "rect 16B");

struct __attribute__((packed)) virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct __attribute__((packed)) virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    // seguido por virtio_gpu_mem_entry[nr_entries]
};

struct __attribute__((packed)) virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};
_Static_assert(sizeof(struct virtio_gpu_mem_entry) == 16, "mem_entry 16B");

struct __attribute__((packed)) virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct __attribute__((packed)) virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

// --- estado interno -------------------------------------------------------
typedef struct VIRTIO_GPU_DEV {
    int                       present;
    int                       active;     // FEATURES_OK
    int                       queues_ok;  // DRIVER_OK setado, queues prontas
    const hal_pci_device_t*   pci;

    volatile VIRTIO_COMMON_CFG* common;
    volatile uint8_t*         notify;
    volatile uint8_t*         isr;
    volatile uint8_t*         device_cfg;
    uint32_t                  notify_off_multiplier;

    // Para diagnostico/fases futuras.
    uint64_t                  common_phys, common_size;
    uint64_t                  notify_phys, notify_size;
    uint64_t                  isr_phys,    isr_size;
    uint64_t                  device_phys, device_size;

    uint32_t                  features_lo;
    uint32_t                  features_hi;

    VIRTIO_QUEUE              controlq;
    VIRTIO_QUEUE              cursorq;

    // FASE 10.4: framebuffer real (apos SET_SCANOUT). Quando display_ok = 1,
    // o gpu.c usa este FB como backend primario; senao Bochs VBE continua.
    int                       display_ok;        // SET_SCANOUT concluido
    uint32_t                  fb_width;
    uint32_t                  fb_height;
    uint32_t                  fb_pitch;          // bytes por linha (= w * 4)
    uint32_t                  fb_size;           // bytes totais (= h * pitch)
    uint32_t                  fb_resource_id;    // resource_id usado no SET_SCANOUT
    uint64_t                  fb_phys;           // base fisica do FB DMA
    volatile uint32_t*        framebuffer;       // ponteiro virtual (== phys: identidade)
} VIRTIO_GPU_DEV;

static VIRTIO_GPU_DEV g_vgpu = {0};

// Memory barrier conservador para MMIO em sequencia critica.
static inline void mmio_mb(void) { __sync_synchronize(); }

// Acessores volatile-corretos para campos individuais.
static inline uint8_t  vrd8 (volatile uint8_t*  p) { return *p; }
static inline uint16_t vrd16(volatile uint16_t* p) { return *p; }
static inline uint32_t vrd32(volatile uint32_t* p) { return *p; }
static inline void     vwr8 (volatile uint8_t*  p, uint8_t  v) { *p = v; }
static inline void     vwr16(volatile uint16_t* p, uint16_t v) { *p = v; }
static inline void     vwr32(volatile uint32_t* p, uint32_t v) { *p = v; }
static inline void     vwr64(volatile uint64_t* p, uint64_t v) { *p = v; }

// --- PCI helpers ----------------------------------------------------------
static const hal_pci_device_t* find_virtio_gpu_device(void) {
    int n = hal_pci_count();
    for (int i = 0; i < n; i++) {
        const hal_pci_device_t* d = hal_pci_get(i);
        if (!d) continue;
        if (d->vendor_id != VIRTIO_PCI_VENDOR) continue;

        // Modern (transport version 1) virtio-gpu: device_id 0x1050.
        if (d->device_id == VIRTIO_GPU_DEVICE_MODERN) return d;

        // Transitional/legacy: device_id 0x1040..0x107F com subsystem_id=0x10.
        if (d->device_id >= VIRTIO_GPU_DEVICE_LEGACY &&
            d->device_id <= VIRTIO_GPU_DEVICE_LEGACY_MAX) {
            uint16_t sub = HalPciReadConfigUshort(d->bus, d->device, d->function, 0x2E);
            if (sub == VIRTIO_ID_GPU) return d;
        }
    }
    return 0;
}

// Le um BAR como endereco fisico 64 bits. Suporta 64-bit BAR (tipo 2 nos bits
// 2..1 do BAR; usa o BAR seguinte como dword alto). Devolve 0 se o BAR for
// I/O (bit 0 = 1) ou se phys nao for valido.
static uint64_t read_bar_phys(const hal_pci_device_t* d, int bar_idx, int* is_64bit) {
    if (bar_idx < 0 || bar_idx >= 6) return 0;
    uint32_t lo = HalPciReadConfigUlong(d->bus, d->device, d->function,
                                        (uint8_t)(0x10 + bar_idx * 4));
    if (lo & 1) return 0;                       // I/O space — nao usavel p/ MMIO map
    uint8_t type = (uint8_t)((lo >> 1) & 3);
    uint64_t base = (uint64_t)(lo & 0xFFFFFFF0u);
    if (type == 2 && bar_idx < 5) {             // 64-bit
        uint32_t hi = HalPciReadConfigUlong(d->bus, d->device, d->function,
                                            (uint8_t)(0x10 + (bar_idx + 1) * 4));
        base |= ((uint64_t)hi) << 32;
        if (is_64bit) *is_64bit = 1;
    } else {
        if (is_64bit) *is_64bit = 0;
    }
    return base;
}

// Habilita Memory Space + Bus Master no command register (offset 0x04).
static void enable_pci_memory(const hal_pci_device_t* d) {
    uint32_t cmd = HalPciReadConfigUlong(d->bus, d->device, d->function, 0x04);
    uint32_t want = cmd | 0x6;     // bit1 Memory Space, bit2 Bus Master
    if (want != cmd) {
        HalPciWriteConfigUlong(d->bus, d->device, d->function, 0x04, want);
        kputs("[vgpu] PCI Command 0x04: 0x"); kput_hex(cmd);
        kputs(" -> 0x"); kput_hex(want); kputs(" (Memory+BM)\n");
    }
}

// Mapeia [phys, phys+size) em uma faixa virt acima de 1 GiB (fora da
// identidade). Estrategia: 0xFFFFC100_00000000 + slot * 0x100000 — separa
// regioes em slots de 1 MiB para nao colidir.
static volatile void* map_region(uint64_t phys, uint64_t size, int slot) {
    if (size == 0) return 0;
    uint64_t aligned_phys = phys & ~0xFFFULL;
    uint64_t skew = phys - aligned_phys;
    uint64_t aligned_size = (size + skew + 0xFFFULL) & ~0xFFFULL;

    uint64_t virt_base = 0xFFFFC10000000000ULL + (uint64_t)slot * 0x100000ULL;
    // PCD (cache disable) — MMIO de dispositivo nao pode ser cacheado.
    uint64_t mflags = MM_FLAG_PRESENT | MM_FLAG_RW | MM_FLAG_PCD;
    if (!mm_map_phys_range(virt_base, aligned_phys, aligned_size, mflags)) {
        kputs("[vgpu] mm_map_phys_range FALHOU para phys=0x");
        kput_hex(phys); kputs(" size=0x"); kput_hex(size); kputs("\n");
        return 0;
    }
    return (volatile void*)(uintptr_t)(virt_base + skew);
}

// Loga 1 capability detectada.
static void log_cap(const char* tag, uint8_t bar, uint32_t off, uint32_t len) {
    kputs("[vgpu] caps: "); kputs(tag);
    kputs(" bar="); kput_dec(bar);
    kputs(" off=0x"); kput_hex(off);
    kputs(" len=0x"); kput_hex(len);
    kputc('\n');
}

// Le N bytes do PCI config space em offset arbitrario (resolvido em dwords).
static uint32_t pci_read_cfg32(const hal_pci_device_t* d, uint8_t off) {
    return HalPciReadConfigUlong(d->bus, d->device, d->function, off);
}
static uint8_t pci_read_cfg8(const hal_pci_device_t* d, uint8_t off) {
    return HalPciReadConfigUchar(d->bus, d->device, d->function, off);
}

// ============================================================================
//  FASE 10.2 — virtqueue init.
//
//  Para cada queue:
//    1) common->queue_select = idx
//    2) le common->queue_size (capacidade maxima reportada pelo device) e
//       reduz para min(VIRTIO_VQ_SIZE, queue_size); escreve de volta se necess.
//    3) le common->queue_notify_off; calcula notify_reg =
//         (uint16_t*)(notify + queue_notify_off * notify_off_multiplier).
//    4) aloca 1 pagina contigua para CADA area (desc/avail/used). Como o PMM
//       cobre [64 MiB, 1 GiB) e a identidade tambem, phys == virt.
//    5) zera as 3 areas, monta a free list (desc[i].next = i+1; ultimo = 0).
//    6) escreve enderecos FISICOS em queue_desc/driver/device.
//    7) common->queue_enable = 1
// ============================================================================
static int virtio_queue_init(int queue_idx, VIRTIO_QUEUE* q) {
    if (!g_vgpu.common) return 0;

    q->queue_idx = (uint16_t)queue_idx;
    q->last_used_idx = 0;

    // 1) Seleciona a queue.
    vwr16(&g_vgpu.common->queue_select, (uint16_t)queue_idx);
    mmio_mb();

    // 2) Capacidade reportada pelo device; truncamos para VIRTIO_VQ_SIZE.
    uint16_t dev_qsize = vrd16(&g_vgpu.common->queue_size);
    if (dev_qsize == 0) {
        kputs("[vgpu] queue "); kput_dec(queue_idx);
        kputs(": queue_size=0 (device sem suporte a este queue)\n");
        return 0;
    }
    uint16_t qsize = dev_qsize;
    if (qsize > VIRTIO_VQ_SIZE) qsize = VIRTIO_VQ_SIZE;
    q->size = qsize;
    if (qsize != dev_qsize) {
        // Pede ao device para reduzir a capacidade real.
        vwr16(&g_vgpu.common->queue_size, qsize);
        mmio_mb();
    }

    // 3) Notify offset (em unidades de notify_off_multiplier).
    q->notify_off = vrd16(&g_vgpu.common->queue_notify_off);
    uint64_t notify_addr = (uint64_t)(uintptr_t)g_vgpu.notify +
                           (uint64_t)q->notify_off *
                           (uint64_t)g_vgpu.notify_off_multiplier;
    q->notify_reg = (volatile uint16_t*)(uintptr_t)notify_addr;

    // 4) Aloca 1 pagina por area. pmm_alloc_contiguous(1) ja pega 1 frame.
    uint64_t desc_phys  = pmm_alloc_contiguous(1);
    uint64_t avail_phys = pmm_alloc_contiguous(1);
    uint64_t used_phys  = pmm_alloc_contiguous(1);
    if (!desc_phys || !avail_phys || !used_phys) {
        kputs("[vgpu] queue "); kput_dec(queue_idx);
        kputs(": pmm_alloc_contiguous falhou (sem RAM)\n");
        return 0;
    }
    q->desc_phys  = desc_phys;
    q->avail_phys = avail_phys;
    q->used_phys  = used_phys;
    q->desc  = (volatile struct vq_desc*) (uintptr_t)desc_phys;
    q->avail = (volatile struct vq_avail*)(uintptr_t)avail_phys;
    q->used  = (volatile struct vq_used*) (uintptr_t)used_phys;

    // 5) Zera tudo, depois monta a free list nos descriptors.
    vq_bzero(q->desc,  4096);
    vq_bzero(q->avail, 4096);
    vq_bzero(q->used,  4096);
    for (uint16_t i = 0; i < (uint16_t)(qsize - 1); i++) {
        q->desc[i].next = (uint16_t)(i + 1);
    }
    q->desc[qsize - 1].next = VIRTQ_DESC_FREE_END;
    q->next_free_desc = 0;
    q->num_free = qsize;

    // 6) Escreve enderecos FISICOS no device. queue_desc=desc, queue_driver=
    //    avail, queue_device=used (nomes do spec moderno).
    vwr64(&g_vgpu.common->queue_desc,   desc_phys);
    vwr64(&g_vgpu.common->queue_driver, avail_phys);
    vwr64(&g_vgpu.common->queue_device, used_phys);
    mmio_mb();

    // 7) Habilita a queue.
    vwr16(&g_vgpu.common->queue_enable, 1);
    mmio_mb();

    kputs("[vgpu] queue "); kput_dec(queue_idx);
    kputs(" init: size="); kput_dec(qsize);
    kputs(" desc@phys=0x"); kput_hex(desc_phys);
    kputs(" avail@phys=0x"); kput_hex(avail_phys);
    kputs(" used@phys=0x"); kput_hex(used_phys);
    kputs(" notify_off="); kput_dec(q->notify_off);
    kputs(" notify_reg=0x"); kput_hex((uint64_t)(uintptr_t)q->notify_reg);
    kputc('\n');
    return 1;
}

// Aloca 1 descriptor da free list. Retorna VIRTQ_DESC_FREE_END (0xFFFF) se vazia.
uint16_t virtio_queue_alloc_desc(VIRTIO_QUEUE* q) {
    if (!q || q->num_free == 0) return VIRTQ_DESC_FREE_END;
    uint16_t head = q->next_free_desc;
    if (head == VIRTQ_DESC_FREE_END) return VIRTQ_DESC_FREE_END;
    q->next_free_desc = q->desc[head].next;
    q->num_free--;
    return head;
}

// Devolve uma cadeia inteira (desc[head], desc[next], ...) para a free list.
void virtio_queue_free_desc_chain(VIRTIO_QUEUE* q, uint16_t head_idx) {
    if (!q || head_idx >= q->size) return;
    uint16_t cur = head_idx;
    for (;;) {
        uint16_t next = q->desc[cur].next;
        uint16_t fl   = q->desc[cur].flags;
        // Devolve este descriptor para a cabeca da free list.
        q->desc[cur].next  = q->next_free_desc;
        q->desc[cur].flags = 0;
        q->next_free_desc  = cur;
        q->num_free++;
        if (!(fl & VIRTQ_DESC_F_NEXT)) break;
        cur = next;
        if (cur >= q->size) break;   // guard
    }
}

// Submete um chain (head_idx) para o device. Coloca no avail ring, incrementa
// avail->idx e notifica via MMIO.
void virtio_queue_submit(VIRTIO_QUEUE* q, uint16_t head_idx) {
    if (!q) return;
    uint16_t i = q->avail->idx % q->size;
    q->avail->ring[i] = head_idx;
    mmio_mb();
    q->avail->idx = (uint16_t)(q->avail->idx + 1);
    mmio_mb();
    // Notifica: modern virtio escreve o queue_idx (16 bits) no notify_reg.
    if (q->notify_reg) {
        *q->notify_reg = q->queue_idx;
    }
}

// Espera o device completar um chain (polling). Retorna o len escrito pelo
// device. Libera o chain associado.
uint32_t virtio_queue_wait_response(VIRTIO_QUEUE* q) {
    if (!q) return 0;
    // Loop ate o device incrementar used->idx.
    while (q->last_used_idx == q->used->idx) {
        __asm__ volatile ("pause");
    }
    uint16_t i = (uint16_t)(q->last_used_idx % q->size);
    uint32_t id  = q->used->ring[i].id;
    uint32_t len = q->used->ring[i].len;
    q->last_used_idx = (uint16_t)(q->last_used_idx + 1);
    // Libera o chain submetido.
    if (id < q->size) virtio_queue_free_desc_chain(q, (uint16_t)id);
    return len;
}

// ============================================================================
//  FASE 10.3 — comandos 2D do virtio-gpu.
//
//  Cada comando segue o mesmo padrao:
//    1) Aloca 2 descriptors no controlq (1 p/ request, 1 p/ response).
//    2) Linka head -> next: head.flags = NEXT, next.flags = WRITE.
//    3) Submete o head, espera resposta (polling em used->idx).
//    4) Le resp.hdr.type. Sucesso = RESP_OK_NODATA / OK_DISPLAY_INFO.
//
//  As estruturas req/resp moram em buffers do heap do kernel (32 MiB +). Como
//  esse range esta dentro da identidade de 1 GiB, o endereco VIRTUAL retornado
//  por kmalloc tambem e o FISICO que o device precisa em desc.addr.
// ============================================================================

// Pula um header de comando p/ inicializa-lo: type=tag, demais campos zerados.
static void virtio_gpu_hdr_init(struct virtio_gpu_ctrl_hdr* h, uint32_t type) {
    h->type     = type;
    h->flags    = 0;
    h->fence_id = 0;
    h->ctx_id   = 0;
    h->padding  = 0;
}

// Envia (req_buf, req_len) e recebe (resp_buf, resp_len) num chain de 2 desc.
// Retorna resp_buf->hdr.type (tipo da resposta).
static uint32_t virtio_gpu_send_cmd(const void* req_buf, uint32_t req_len,
                                    void* resp_buf, uint32_t resp_len) {
    if (!g_vgpu.queues_ok) return 0;
    VIRTIO_QUEUE* q = &g_vgpu.controlq;

    // Aloca 2 descriptors.
    uint16_t h = virtio_queue_alloc_desc(q);
    uint16_t r = virtio_queue_alloc_desc(q);
    if (h == VIRTQ_DESC_FREE_END || r == VIRTQ_DESC_FREE_END) {
        kputs("[vgpu] send_cmd: sem descriptors livres no controlq\n");
        if (h != VIRTQ_DESC_FREE_END) virtio_queue_free_desc_chain(q, h);
        if (r != VIRTQ_DESC_FREE_END) virtio_queue_free_desc_chain(q, r);
        return 0;
    }

    // Zera o response para o device escrever do zero.
    vq_bzero(resp_buf, resp_len);

    // desc[h] -> request (driver escreve, device le)
    q->desc[h].addr  = (uint64_t)(uintptr_t)req_buf;
    q->desc[h].len   = req_len;
    q->desc[h].flags = VIRTQ_DESC_F_NEXT;
    q->desc[h].next  = r;
    // desc[r] -> response (device escreve)
    q->desc[r].addr  = (uint64_t)(uintptr_t)resp_buf;
    q->desc[r].len   = resp_len;
    q->desc[r].flags = VIRTQ_DESC_F_WRITE;
    q->desc[r].next  = 0;
    mmio_mb();

    virtio_queue_submit(q, h);
    (void)virtio_queue_wait_response(q);

    // Le o type da resposta.
    const struct virtio_gpu_ctrl_hdr* rh = (const struct virtio_gpu_ctrl_hdr*)resp_buf;
    return rh->type;
}

// Helper de log: traduz codigo de resposta em texto curto.
static const char* virtio_gpu_resp_name(uint32_t t) {
    switch (t) {
        case VIRTIO_GPU_RESP_OK_NODATA:         return "OK_NODATA";
        case VIRTIO_GPU_RESP_OK_DISPLAY_INFO:   return "OK_DISPLAY_INFO";
        case VIRTIO_GPU_RESP_ERR_UNSPEC:        return "ERR_UNSPEC";
        case VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY: return "ERR_OUT_OF_MEMORY";
        case VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID:  return "ERR_INVALID_SCANOUT_ID";
        case VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID: return "ERR_INVALID_RESOURCE_ID";
        default: return "?";
    }
}

// 1) GET_DISPLAY_INFO — pergunta ao host as resolucoes dos scanouts.
//    Devolve o type (RESP_OK_DISPLAY_INFO em sucesso) e preenche os outs.
uint32_t virtio_gpu_cmd_get_display_info(uint32_t* out_w, uint32_t* out_h,
                                         uint32_t* out_enabled) {
    if (!g_vgpu.queues_ok) return 0;
    struct virtio_gpu_ctrl_hdr* req =
        (struct virtio_gpu_ctrl_hdr*)kmalloc(sizeof(*req));
    struct virtio_gpu_resp_display_info* resp =
        (struct virtio_gpu_resp_display_info*)kmalloc(sizeof(*resp));
    if (!req || !resp) {
        kputs("[vgpu] GET_DISPLAY_INFO: kmalloc falhou\n");
        return 0;
    }
    virtio_gpu_hdr_init(req, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

    uint32_t t = virtio_gpu_send_cmd(req, sizeof(*req), resp, sizeof(*resp));
    if (t == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        if (out_w)       *out_w       = resp->pmodes[0].r.width;
        if (out_h)       *out_h       = resp->pmodes[0].r.height;
        if (out_enabled) *out_enabled = resp->pmodes[0].enabled;
        kputs("[vgpu] GET_DISPLAY_INFO OK, scanout 0: ");
        kput_dec(resp->pmodes[0].r.width); kputc('x');
        kput_dec(resp->pmodes[0].r.height);
        kputs(" enabled="); kput_dec(resp->pmodes[0].enabled); kputc('\n');
    } else {
        kputs("[vgpu] GET_DISPLAY_INFO falhou: resp=");
        kputs(virtio_gpu_resp_name(t)); kputc('\n');
    }
    kfree(req);
    kfree(resp);
    return t;
}

// 2) RESOURCE_CREATE_2D — cria um recurso 2D (textura) no host.
uint32_t virtio_gpu_cmd_resource_create_2d(uint32_t resource_id, uint32_t format,
                                           uint32_t width, uint32_t height) {
    if (!g_vgpu.queues_ok) return 0;
    struct virtio_gpu_resource_create_2d* req =
        (struct virtio_gpu_resource_create_2d*)kmalloc(sizeof(*req));
    struct virtio_gpu_ctrl_hdr* resp =
        (struct virtio_gpu_ctrl_hdr*)kmalloc(sizeof(*resp));
    if (!req || !resp) return 0;

    virtio_gpu_hdr_init(&req->hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    req->resource_id = resource_id;
    req->format      = format;
    req->width       = width;
    req->height      = height;

    uint32_t t = virtio_gpu_send_cmd(req, sizeof(*req), resp, sizeof(*resp));
    kputs("[vgpu] CREATE_2D resource_id="); kput_dec(resource_id);
    kputs(" "); kput_dec(width); kputc('x'); kput_dec(height);
    kputs(" fmt="); kput_dec(format);
    kputs(" -> "); kputs(virtio_gpu_resp_name(t)); kputc('\n');
    kfree(req);
    kfree(resp);
    return t;
}

// 3) RESOURCE_ATTACH_BACKING — anexa pagina(s) de memoria como backing store.
//    Aqui anexamos UM unico mem_entry (range fisico contiguo).
uint32_t virtio_gpu_cmd_resource_attach_backing(uint32_t resource_id,
                                                uint64_t phys_addr,
                                                uint32_t length) {
    if (!g_vgpu.queues_ok) return 0;
    // Layout: header + 1 mem_entry, contiguo.
    uint32_t req_len = (uint32_t)(sizeof(struct virtio_gpu_resource_attach_backing) +
                                  sizeof(struct virtio_gpu_mem_entry));
    uint8_t* buf = (uint8_t*)kmalloc(req_len);
    struct virtio_gpu_ctrl_hdr* resp =
        (struct virtio_gpu_ctrl_hdr*)kmalloc(sizeof(*resp));
    if (!buf || !resp) return 0;

    struct virtio_gpu_resource_attach_backing* req =
        (struct virtio_gpu_resource_attach_backing*)buf;
    struct virtio_gpu_mem_entry* ent =
        (struct virtio_gpu_mem_entry*)(buf + sizeof(*req));

    virtio_gpu_hdr_init(&req->hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    req->resource_id = resource_id;
    req->nr_entries  = 1;
    ent->addr        = phys_addr;
    ent->length      = length;
    ent->padding     = 0;

    uint32_t t = virtio_gpu_send_cmd(buf, req_len, resp, sizeof(*resp));
    kputs("[vgpu] ATTACH_BACKING resource_id="); kput_dec(resource_id);
    kputs(" phys=0x"); kput_hex(phys_addr);
    kputs(" len="); kput_dec(length);
    kputs(" -> "); kputs(virtio_gpu_resp_name(t)); kputc('\n');
    kfree(buf);
    kfree(resp);
    return t;
}

// 4) SET_SCANOUT — conecta um recurso ao scanout (output do display).
uint32_t virtio_gpu_cmd_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height) {
    if (!g_vgpu.queues_ok) return 0;
    struct virtio_gpu_set_scanout* req =
        (struct virtio_gpu_set_scanout*)kmalloc(sizeof(*req));
    struct virtio_gpu_ctrl_hdr* resp =
        (struct virtio_gpu_ctrl_hdr*)kmalloc(sizeof(*resp));
    if (!req || !resp) return 0;

    virtio_gpu_hdr_init(&req->hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    req->r.x = x; req->r.y = y; req->r.width = width; req->r.height = height;
    req->scanout_id  = scanout_id;
    req->resource_id = resource_id;

    uint32_t t = virtio_gpu_send_cmd(req, sizeof(*req), resp, sizeof(*resp));
    kputs("[vgpu] SET_SCANOUT scanout="); kput_dec(scanout_id);
    kputs(" resource_id="); kput_dec(resource_id);
    kputs(" "); kput_dec(width); kputc('x'); kput_dec(height);
    kputs(" -> "); kputs(virtio_gpu_resp_name(t)); kputc('\n');
    kfree(req);
    kfree(resp);
    return t;
}

// 5) TRANSFER_TO_HOST_2D — copia bytes do backing para o recurso do host.
uint32_t virtio_gpu_cmd_transfer_to_host_2d(uint32_t resource_id,
                                            uint64_t offset,
                                            uint32_t x, uint32_t y,
                                            uint32_t width, uint32_t height) {
    if (!g_vgpu.queues_ok) return 0;
    struct virtio_gpu_transfer_to_host_2d* req =
        (struct virtio_gpu_transfer_to_host_2d*)kmalloc(sizeof(*req));
    struct virtio_gpu_ctrl_hdr* resp =
        (struct virtio_gpu_ctrl_hdr*)kmalloc(sizeof(*resp));
    if (!req || !resp) return 0;

    virtio_gpu_hdr_init(&req->hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    req->r.x = x; req->r.y = y; req->r.width = width; req->r.height = height;
    req->offset      = offset;
    req->resource_id = resource_id;
    req->padding     = 0;

    uint32_t t = virtio_gpu_send_cmd(req, sizeof(*req), resp, sizeof(*resp));
    kputs("[vgpu] TRANSFER_TO_HOST_2D resource_id="); kput_dec(resource_id);
    kputs(" "); kput_dec(width); kputc('x'); kput_dec(height);
    kputs(" offset="); kput_dec(offset);
    kputs(" -> "); kputs(virtio_gpu_resp_name(t)); kputc('\n');
    kfree(req);
    kfree(resp);
    return t;
}

// 6) RESOURCE_FLUSH — pede ao host p/ apresentar a regiao no scanout.
uint32_t virtio_gpu_cmd_resource_flush(uint32_t resource_id,
                                       uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height) {
    if (!g_vgpu.queues_ok) return 0;
    struct virtio_gpu_resource_flush* req =
        (struct virtio_gpu_resource_flush*)kmalloc(sizeof(*req));
    struct virtio_gpu_ctrl_hdr* resp =
        (struct virtio_gpu_ctrl_hdr*)kmalloc(sizeof(*resp));
    if (!req || !resp) return 0;

    virtio_gpu_hdr_init(&req->hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    req->r.x = x; req->r.y = y; req->r.width = width; req->r.height = height;
    req->resource_id = resource_id;
    req->padding     = 0;

    uint32_t t = virtio_gpu_send_cmd(req, sizeof(*req), resp, sizeof(*resp));
    kputs("[vgpu] RESOURCE_FLUSH resource_id="); kput_dec(resource_id);
    kputs(" "); kput_dec(width); kputc('x'); kput_dec(height);
    kputs(" -> "); kputs(virtio_gpu_resp_name(t)); kputc('\n');
    kfree(req);
    kfree(resp);
    return t;
}

// 7) RESOURCE_UNREF — libera o recurso do host.
uint32_t virtio_gpu_cmd_resource_unref(uint32_t resource_id) {
    if (!g_vgpu.queues_ok) return 0;
    struct virtio_gpu_resource_unref* req =
        (struct virtio_gpu_resource_unref*)kmalloc(sizeof(*req));
    struct virtio_gpu_ctrl_hdr* resp =
        (struct virtio_gpu_ctrl_hdr*)kmalloc(sizeof(*resp));
    if (!req || !resp) return 0;

    virtio_gpu_hdr_init(&req->hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    req->resource_id = resource_id;
    req->padding     = 0;

    uint32_t t = virtio_gpu_send_cmd(req, sizeof(*req), resp, sizeof(*resp));
    kputs("[vgpu] RESOURCE_UNREF resource_id="); kput_dec(resource_id);
    kputs(" -> "); kputs(virtio_gpu_resp_name(t)); kputc('\n');
    kfree(req);
    kfree(resp);
    return t;
}

// ----------------------------------------------------------------------------
//  Smoke test (soft): nao chama SET_SCANOUT (nao trocamos o backend ainda).
//
//  Plano:
//    1) GET_DISPLAY_INFO (espera RESP_OK_DISPLAY_INFO)
//    2) Aloca 16 KiB (64x64 BGRA = 64*64*4 = 16384) via kmalloc; preenche c/
//       cor solida (vermelho — pattern 0xFFFF0000 em 32-bit native).
//    3) RESOURCE_CREATE_2D resource_id=1, BGRA, 64x64
//    4) RESOURCE_ATTACH_BACKING resource_id=1 phys=virt(buf) len=16384
//    5) TRANSFER_TO_HOST_2D resource_id=1 64x64 offset=0
//    6) RESOURCE_FLUSH resource_id=1 64x64 (sem scanout vinculado: cmd valido,
//       host so nao apresenta; resposta deve ser OK_NODATA mesmo assim)
//    7) RESOURCE_UNREF resource_id=1
//
//  Sucesso = todos os comandos voltaram OK_*.
// ----------------------------------------------------------------------------
int virtio_gpu_smoke_test(void) {
    if (!g_vgpu.queues_ok) {
        kputs("[vgpu] smoke test: queues nao prontas (DRIVER_OK nao setado)\n");
        return 0;
    }

    kputs("[vgpu] === smoke test (soft, sem SET_SCANOUT) ===\n");

    // 1) GET_DISPLAY_INFO
    uint32_t dw = 0, dh = 0, denab = 0;
    uint32_t t = virtio_gpu_cmd_get_display_info(&dw, &dh, &denab);
    if (t != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        kputs("[vgpu] smoke test FAIL @ GET_DISPLAY_INFO\n");
        return 0;
    }

    // 2) buffer 64x64 BGRA
    const uint32_t W = 64, H = 64, BPP = 4;
    const uint32_t BUFLEN = W * H * BPP;
    uint8_t* fbuf = (uint8_t*)kmalloc(BUFLEN);
    if (!fbuf) {
        kputs("[vgpu] smoke test FAIL @ kmalloc(16K)\n");
        return 0;
    }
    // Pinta com 0xFFFF0000 (BGRA: B=0x00 G=0x00 R=0xFF A=0xFF -> vermelho).
    uint32_t* px = (uint32_t*)fbuf;
    for (uint32_t i = 0; i < W * H; i++) px[i] = 0xFFFF0000u;
    uint64_t fbuf_phys = (uint64_t)(uintptr_t)fbuf;   // identidade: virt==phys
    kputs("[vgpu] smoke buffer @0x"); kput_hex(fbuf_phys);
    kputs(" len="); kput_dec(BUFLEN); kputs(" (64x64 BGRA, vermelho)\n");

    // 3) CREATE_2D
    t = virtio_gpu_cmd_resource_create_2d(1, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM, W, H);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] smoke test FAIL @ CREATE_2D\n");
        kfree(fbuf);
        return 0;
    }

    // 4) ATTACH_BACKING
    t = virtio_gpu_cmd_resource_attach_backing(1, fbuf_phys, BUFLEN);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] smoke test FAIL @ ATTACH_BACKING\n");
        (void)virtio_gpu_cmd_resource_unref(1);
        kfree(fbuf);
        return 0;
    }

    // 5) TRANSFER_TO_HOST_2D
    t = virtio_gpu_cmd_transfer_to_host_2d(1, 0, 0, 0, W, H);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] smoke test FAIL @ TRANSFER_TO_HOST_2D\n");
        (void)virtio_gpu_cmd_resource_unref(1);
        kfree(fbuf);
        return 0;
    }

    // 6) RESOURCE_FLUSH (sem SET_SCANOUT prevenia este flush de pintar tela —
    //    a spec aceita o flush mesmo sem scanout vinculado).
    t = virtio_gpu_cmd_resource_flush(1, 0, 0, W, H);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] smoke test FAIL @ RESOURCE_FLUSH\n");
        (void)virtio_gpu_cmd_resource_unref(1);
        kfree(fbuf);
        return 0;
    }

    // 7) RESOURCE_UNREF — libera o recurso do host. O kmalloc do buffer
    //    pode ficar (sem unbacking explicito; UNREF ja invalida o resource_id).
    t = virtio_gpu_cmd_resource_unref(1);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] smoke test FAIL @ RESOURCE_UNREF\n");
        kfree(fbuf);
        return 0;
    }

    kfree(fbuf);
    kputs("[vgpu] smoke test PASS, todos os comandos retornaram OK\n");
    return 1;
}

// ============================================================================
//  virtio_gpu_detect - Entry point publico chamado apos hal_init().
// ============================================================================
int virtio_gpu_detect(void) {
    if (g_vgpu.present) return g_vgpu.active;

    const hal_pci_device_t* d = find_virtio_gpu_device();
    if (!d) {
        kputs("[vgpu] nao encontrado, usando Bochs VBE como fallback "
              "(esperado quando QEMU rodou sem -device virtio-gpu-pci).\n");
        return 0;
    }

    g_vgpu.present = 1;
    g_vgpu.pci = d;

    kputs("[vgpu] PCI ");
    kput_hex(d->vendor_id); kputc(':'); kput_hex(d->device_id);
    kputs(" (modern virtio-gpu) achado em bus=");
    kput_dec(d->bus); kputc(' '); kputs("dev="); kput_dec(d->device);
    kputs(" func="); kput_dec(d->function); kputc('\n');

    // Habilita Memory Space + Bus Master antes de tocar nas BARs.
    enable_pci_memory(d);

    // 1) Caminhar lista de capabilities a partir de offset 0x34.
    uint8_t cap_off = pci_read_cfg8(d, 0x34);
    int safety = 0;

    // Salva info de cada cfg_type detectado.
    struct { uint8_t bar; uint32_t off; uint32_t len; int found; } regions[6] = {0};
    uint32_t notify_mult = 0;

    while (cap_off != 0 && safety++ < 64) {
        // Header bytes do cap (lidos via PCI config space).
        uint8_t cap_vndr = pci_read_cfg8(d, cap_off + 0x00);
        uint8_t cap_next = pci_read_cfg8(d, cap_off + 0x01);
        uint8_t cap_len  = pci_read_cfg8(d, cap_off + 0x02);
        uint8_t cfg_type = pci_read_cfg8(d, cap_off + 0x03);

        if (cap_vndr == PCI_CAP_ID_VENDOR && cap_len >= 16) {
            uint8_t bar    = pci_read_cfg8(d, cap_off + 0x04);
            uint32_t off   = pci_read_cfg32(d, cap_off + 0x08);
            uint32_t len   = pci_read_cfg32(d, cap_off + 0x0C);

            const char* tag = "?";
            switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG: tag = "COMMON_CFG"; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG: tag = "NOTIFY_CFG"; break;
                case VIRTIO_PCI_CAP_ISR_CFG:    tag = "ISR_CFG   "; break;
                case VIRTIO_PCI_CAP_DEVICE_CFG: tag = "DEVICE_CFG"; break;
                case VIRTIO_PCI_CAP_PCI_CFG:    tag = "PCI_CFG   "; break;
                default: tag = "?         "; break;
            }
            if (cfg_type >= 1 && cfg_type <= 5) {
                regions[cfg_type].bar = bar;
                regions[cfg_type].off = off;
                regions[cfg_type].len = len;
                regions[cfg_type].found = 1;
                if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && cap_len >= 20) {
                    notify_mult = pci_read_cfg32(d, cap_off + 0x10);
                }
                log_cap(tag, bar, off, len);
                if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    kputs("[vgpu]   notify_off_multiplier=");
                    kput_dec(notify_mult); kputc('\n');
                }
            }
        }
        if (cap_next == 0) break;
        cap_off = cap_next;
    }

    if (!regions[VIRTIO_PCI_CAP_COMMON_CFG].found) {
        kputs("[vgpu] sem COMMON_CFG cap; nao podemos prosseguir.\n");
        return 0;
    }

    g_vgpu.notify_off_multiplier = notify_mult;

    // 2) Mapear cada regiao no espaco virtual (uma por "slot").
    int slot = 0;
    for (int t = 1; t <= 4; t++) {
        if (!regions[t].found) continue;
        int is_64 = 0;
        uint64_t bar_phys = read_bar_phys(d, regions[t].bar, &is_64);
        if (!bar_phys) {
            kputs("[vgpu] BAR"); kput_dec(regions[t].bar);
            kputs(" invalido (0 ou I/O) p/ cfg_type "); kput_dec(t); kputc('\n');
            return 0;
        }
        uint64_t phys = bar_phys + regions[t].off;
        volatile void* virt = map_region(phys, regions[t].len, slot++);
        if (!virt) return 0;

        switch (t) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                g_vgpu.common      = (volatile VIRTIO_COMMON_CFG*)virt;
                g_vgpu.common_phys = phys; g_vgpu.common_size = regions[t].len;
                kputs("[vgpu] MMIO common @0x"); kput_hex((uint64_t)(uintptr_t)virt);
                kputs(" phys=0x"); kput_hex(phys); kputc('\n');
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                g_vgpu.notify      = (volatile uint8_t*)virt;
                g_vgpu.notify_phys = phys; g_vgpu.notify_size = regions[t].len;
                kputs("[vgpu] MMIO notify @0x"); kput_hex((uint64_t)(uintptr_t)virt);
                kputs(" phys=0x"); kput_hex(phys);
                kputs(" mult="); kput_dec(notify_mult); kputc('\n');
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                g_vgpu.isr      = (volatile uint8_t*)virt;
                g_vgpu.isr_phys = phys; g_vgpu.isr_size = regions[t].len;
                kputs("[vgpu] MMIO isr    @0x"); kput_hex((uint64_t)(uintptr_t)virt);
                kputs(" phys=0x"); kput_hex(phys); kputc('\n');
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                g_vgpu.device_cfg = (volatile uint8_t*)virt;
                g_vgpu.device_phys = phys; g_vgpu.device_size = regions[t].len;
                kputs("[vgpu] MMIO device @0x"); kput_hex((uint64_t)(uintptr_t)virt);
                kputs(" phys=0x"); kput_hex(phys); kputc('\n');
                break;
        }
    }

    if (!g_vgpu.common) {
        kputs("[vgpu] common config nao mapeou; bail.\n");
        return 0;
    }

    // 3) Status protocol.
    // Reset (status = 0)
    vwr8(&g_vgpu.common->device_status, 0);
    mmio_mb();
    // Poll status reset (paranoia; spec recomenda).
    for (int i = 0; i < 100 && vrd8(&g_vgpu.common->device_status) != 0; i++) {
        mmio_mb();
    }
    kputs("[vgpu] reset ok (status=0)\n");

    // ACK
    vwr8(&g_vgpu.common->device_status,
         (uint8_t)(vrd8(&g_vgpu.common->device_status) | VIRTIO_STATUS_ACKNOWLEDGE));
    mmio_mb();

    // DRIVER
    vwr8(&g_vgpu.common->device_status,
         (uint8_t)(vrd8(&g_vgpu.common->device_status) | VIRTIO_STATUS_DRIVER));
    mmio_mb();
    kputs("[vgpu] status ACK|DRIVER\n");

    // 4) Le device features (64 bits via dois selects).
    vwr32(&g_vgpu.common->device_feature_select, 0);
    mmio_mb();
    uint32_t feat_lo = vrd32(&g_vgpu.common->device_feature);
    vwr32(&g_vgpu.common->device_feature_select, 1);
    mmio_mb();
    uint32_t feat_hi = vrd32(&g_vgpu.common->device_feature);
    g_vgpu.features_lo = feat_lo;
    g_vgpu.features_hi = feat_hi;
    kputs("[vgpu] features lo=0x"); kput_hex(feat_lo);
    kputs(" hi=0x"); kput_hex(feat_hi); kputc('\n');

    // 5) Driver features: aceitar APENAS VIRTIO_F_VERSION_1 (bit 32). Rejeitar
    //    VIRGL (bit 0 do select=0). Nao oferecemos EDID/RESOURCE_BLOB nesta fase.
    vwr32(&g_vgpu.common->driver_feature_select, 0);
    mmio_mb();
    vwr32(&g_vgpu.common->driver_feature, 0);  // nada do range baixo
    mmio_mb();
    vwr32(&g_vgpu.common->driver_feature_select, 1);
    mmio_mb();
    uint32_t want_hi = 0;
    if (feat_hi & VIRTIO_F_VERSION_1_BIT) want_hi |= VIRTIO_F_VERSION_1_BIT;
    vwr32(&g_vgpu.common->driver_feature, want_hi);
    mmio_mb();
    kputs("[vgpu] driver features hi=0x"); kput_hex(want_hi);
    kputs(" (VERSION_1 ");
    kputs((want_hi & VIRTIO_F_VERSION_1_BIT) ? "aceito" : "indisponivel");
    kputs(")\n");

    // 6) FEATURES_OK
    uint8_t st = vrd8(&g_vgpu.common->device_status);
    vwr8(&g_vgpu.common->device_status, (uint8_t)(st | VIRTIO_STATUS_FEATURES_OK));
    mmio_mb();
    st = vrd8(&g_vgpu.common->device_status);
    if (!(st & VIRTIO_STATUS_FEATURES_OK)) {
        kputs("[vgpu] FEATURES_OK NAO persistiu (device rejeitou subset); FAILED.\n");
        vwr8(&g_vgpu.common->device_status, (uint8_t)(st | VIRTIO_STATUS_FAILED));
        mmio_mb();
        return 0;
    }
    kputs("[vgpu] FEATURES_OK\n");

    // Pronto para a fase 10.2.
    g_vgpu.active = 1;
    kputs("[vgpu] fase 10.1 concluida (deteccao + MMIO + status FEATURES_OK).\n");

    // ----------------------------------------------------------------------
    //  FASE 10.2 — virtqueue setup + DRIVER_OK.
    // ----------------------------------------------------------------------
    uint16_t nq = vrd16(&g_vgpu.common->num_queues);
    kputs("[vgpu] num_queues reportado pelo device = "); kput_dec(nq); kputc('\n');
    if (nq < 2) {
        kputs("[vgpu] device reportou menos de 2 queues; nao podemos seguir "
              "(controlq + cursorq sao obrigatorias).\n");
        vwr8(&g_vgpu.common->device_status,
             (uint8_t)(vrd8(&g_vgpu.common->device_status) | VIRTIO_STATUS_FAILED));
        mmio_mb();
        return 1;   // ainda voltamos active=1 (FEATURES_OK ocorreu); apenas sem DRIVER_OK.
    }

    if (!virtio_queue_init(VIRTIO_GPU_CONTROLQ_IDX, &g_vgpu.controlq)) {
        kputs("[vgpu] falha ao inicializar controlq; abortando DRIVER_OK.\n");
        vwr8(&g_vgpu.common->device_status,
             (uint8_t)(vrd8(&g_vgpu.common->device_status) | VIRTIO_STATUS_FAILED));
        mmio_mb();
        return 1;
    }
    if (!virtio_queue_init(VIRTIO_GPU_CURSORQ_IDX, &g_vgpu.cursorq)) {
        kputs("[vgpu] falha ao inicializar cursorq; abortando DRIVER_OK.\n");
        vwr8(&g_vgpu.common->device_status,
             (uint8_t)(vrd8(&g_vgpu.common->device_status) | VIRTIO_STATUS_FAILED));
        mmio_mb();
        return 1;
    }

    // DRIVER_OK: device passa para o estado de operacao normal.
    st = vrd8(&g_vgpu.common->device_status);
    vwr8(&g_vgpu.common->device_status, (uint8_t)(st | VIRTIO_STATUS_DRIVER_OK));
    mmio_mb();
    st = vrd8(&g_vgpu.common->device_status);
    if (!(st & VIRTIO_STATUS_DRIVER_OK)) {
        kputs("[vgpu] DRIVER_OK nao persistiu (status=0x"); kput_hex(st);
        kputs("); marcando FAILED.\n");
        vwr8(&g_vgpu.common->device_status, (uint8_t)(st | VIRTIO_STATUS_FAILED));
        mmio_mb();
        return 1;
    }
    if (st & VIRTIO_STATUS_NEEDS_RESET) {
        kputs("[vgpu] device pediu reset (NEEDS_RESET) apos DRIVER_OK.\n");
        return 1;
    }

    g_vgpu.queues_ok = 1;
    kputs("[vgpu] DRIVER_OK setado. status=0x"); kput_hex(st); kputc('\n');
    kputs("[vgpu] fase 10.2 concluida (controlq + cursorq + DRIVER_OK).\n");
    return 1;
}

int      virtio_gpu_active(void)      { return g_vgpu.active; }
uint32_t virtio_gpu_features_lo(void) { return g_vgpu.features_lo; }
uint32_t virtio_gpu_features_hi(void) { return g_vgpu.features_hi; }

VIRTIO_QUEUE* virtio_gpu_controlq(void) {
    return g_vgpu.queues_ok ? &g_vgpu.controlq : (VIRTIO_QUEUE*)0;
}
VIRTIO_QUEUE* virtio_gpu_cursorq(void) {
    return g_vgpu.queues_ok ? &g_vgpu.cursorq : (VIRTIO_QUEUE*)0;
}

// ============================================================================
//  FASE 10.4 — Display init: aloca FB DMA + CREATE_2D + ATTACH + SET_SCANOUT.
//
//  Apos esta funcao retornar 1, o framebuffer (g_vgpu.framebuffer) e o destino
//  oficial de pixels: escrever nele + chamar virtio_gpu_present() pinta a tela.
//
//  Estrategia:
//    1) Pede GET_DISPLAY_INFO p/ saber o que o host suporta no scanout 0.
//       (Apenas log; programamos a resolucao pedida — o host re-escala.)
//    2) Aloca framebuffer contiguo via pmm_alloc_contiguous(num_pages):
//         num_pages = ceil(w*h*4 / 4096)
//       Como PMM cobre [64 MiB, 1 GiB) e a identidade tambem cobre, o virt
//       e igual ao phys — o driver pode escrever direto pelo endereco do PMM.
//    3) Zera o FB (cor inicial = preto solido).
//    4) RESOURCE_CREATE_2D id=FB_RESOURCE_ID com formato BGRA8 w/h.
//    5) RESOURCE_ATTACH_BACKING id=FB_RESOURCE_ID phys=fb_phys len=fb_size.
//    6) SET_SCANOUT scanout=0 resource=FB_RESOURCE_ID r=(0,0,w,h).
//       A partir daqui o host esta apresentando NOSSO framebuffer no display.
//    7) TRANSFER + FLUSH inicial para o host ver os pixels ja escritos (preto).
//
//  Em caso de qualquer falha, libera o que conseguiu e retorna 0 — o caller
//  (gpu.c::gpu_init) faz fallback para o Bochs VBE.
// ============================================================================
#define VIRTIO_GPU_FB_RESOURCE_ID  1

int virtio_gpu_init_display(uint32_t width, uint32_t height) {
    if (!g_vgpu.queues_ok) {
        kputs("[vgpu] init_display: queues nao prontas (DRIVER_OK ausente)\n");
        return 0;
    }
    if (g_vgpu.display_ok) {
        kputs("[vgpu] init_display: ja inicializado, reutilizando FB existente\n");
        return 1;
    }
    if (width == 0 || height == 0) return 0;

    // 1) Pergunta ao host (apenas para log). Mesmo se vier outro modo, vamos
    //    pedir a resolucao que o caller informou — virtio-gpu redimensiona.
    uint32_t dw = 0, dh = 0, denab = 0;
    uint32_t t = virtio_gpu_cmd_get_display_info(&dw, &dh, &denab);
    if (t != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        kputs("[vgpu] init_display: GET_DISPLAY_INFO falhou; abortando\n");
        return 0;
    }
    kputs("[vgpu] init_display: pedido ");
    kput_dec(width); kputc('x'); kput_dec(height);
    kputs(" (host scanout0: "); kput_dec(dw); kputc('x'); kput_dec(dh);
    kputs(" enabled="); kput_dec(denab); kputs(")\n");

    // 2) Aloca framebuffer fisico contiguo. 1024x768x4 = 3 MiB = 768 pages.
    uint32_t pitch = width * 4;
    uint64_t total = (uint64_t)pitch * (uint64_t)height;
    // Alinha em 4 KiB para baixo (caller usa potencias de 2; defensivo).
    uint64_t aligned = (total + 0xFFFULL) & ~0xFFFULL;
    uint64_t num_pages = aligned / 4096;
    uint64_t fb_phys = pmm_alloc_contiguous(num_pages);
    if (!fb_phys) {
        kputs("[vgpu] init_display: pmm_alloc_contiguous(");
        kput_dec(num_pages); kputs(") FALHOU (sem RAM contigua)\n");
        return 0;
    }
    // Identidade cobre 0..1 GiB -> virt = phys.
    volatile uint32_t* fb = (volatile uint32_t*)(uintptr_t)fb_phys;

    // 3) Zera o framebuffer (preto solido inicial).
    for (uint64_t i = 0; i < total / 4; i++) fb[i] = 0x00000000u;

    kputs("[vgpu] init_display: FB DMA phys=0x");
    kput_hex(fb_phys); kputs(" size="); kput_dec(aligned);
    kputs(" (pitch="); kput_dec(pitch); kputs(")\n");

    // 4) CREATE_2D resource_id=1
    t = virtio_gpu_cmd_resource_create_2d(VIRTIO_GPU_FB_RESOURCE_ID,
                                          VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                          width, height);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] init_display: CREATE_2D falhou\n");
        // libera frames (pmm_free_frame por pagina)
        for (uint64_t i = 0; i < num_pages; i++) pmm_free_frame(fb_phys + i * 4096);
        return 0;
    }

    // 5) ATTACH_BACKING — anexa um mem_entry contiguo (phys, len).
    t = virtio_gpu_cmd_resource_attach_backing(VIRTIO_GPU_FB_RESOURCE_ID,
                                               fb_phys, (uint32_t)aligned);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] init_display: ATTACH_BACKING falhou\n");
        (void)virtio_gpu_cmd_resource_unref(VIRTIO_GPU_FB_RESOURCE_ID);
        for (uint64_t i = 0; i < num_pages; i++) pmm_free_frame(fb_phys + i * 4096);
        return 0;
    }

    // 6) SET_SCANOUT — host passa a apresentar este recurso.
    t = virtio_gpu_cmd_set_scanout(0, VIRTIO_GPU_FB_RESOURCE_ID, 0, 0, width, height);
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("[vgpu] init_display: SET_SCANOUT falhou\n");
        (void)virtio_gpu_cmd_resource_unref(VIRTIO_GPU_FB_RESOURCE_ID);
        for (uint64_t i = 0; i < num_pages; i++) pmm_free_frame(fb_phys + i * 4096);
        return 0;
    }

    // Estado pronto para o gpu.c usar.
    g_vgpu.fb_width        = width;
    g_vgpu.fb_height       = height;
    g_vgpu.fb_pitch        = pitch;
    g_vgpu.fb_size         = (uint32_t)aligned;
    g_vgpu.fb_phys         = fb_phys;
    g_vgpu.framebuffer     = fb;
    g_vgpu.fb_resource_id  = VIRTIO_GPU_FB_RESOURCE_ID;
    g_vgpu.display_ok      = 1;

    kputs("[vgpu] init_display: SET_SCANOUT 0 -> resource ");
    kput_dec(VIRTIO_GPU_FB_RESOURCE_ID); kputs(" OK\n");
    kputs("[vgpu] init_display: framebuffer ATIVO virt=phys=0x");
    kput_hex(fb_phys); kputs(" "); kput_dec(width); kputc('x');
    kput_dec(height); kputs("x32 BGRA\n");

    // 7) Frame inicial (preto) ja escrito; faz transfer + flush para o host
    //    pintar a tela do reset.
    (void)virtio_gpu_cmd_transfer_to_host_2d(VIRTIO_GPU_FB_RESOURCE_ID, 0,
                                             0, 0, width, height);
    (void)virtio_gpu_cmd_resource_flush(VIRTIO_GPU_FB_RESOURCE_ID,
                                        0, 0, width, height);
    kputs("[vgpu] init_display: primeiro frame transferido + flush\n");
    return 1;
}

// FASE 10.4 — apresenta o framebuffer: TRANSFER (RAM -> host resource) + FLUSH
// (host -> scanout). Sem isso o host nao re-le o backing (so faz no SET ou
// quando explicitamente avisado pelo driver).
void virtio_gpu_present(void) {
    if (!g_vgpu.display_ok) return;
    (void)virtio_gpu_cmd_transfer_to_host_2d(g_vgpu.fb_resource_id, 0,
                                             0, 0,
                                             g_vgpu.fb_width, g_vgpu.fb_height);
    (void)virtio_gpu_cmd_resource_flush(g_vgpu.fb_resource_id,
                                        0, 0,
                                        g_vgpu.fb_width, g_vgpu.fb_height);
}

// Acessores para o gpu.c.
int      virtio_gpu_display_ok(void)    { return g_vgpu.display_ok; }
uint32_t virtio_gpu_fb_width(void)      { return g_vgpu.fb_width; }
uint32_t virtio_gpu_fb_height(void)     { return g_vgpu.fb_height; }
uint32_t virtio_gpu_fb_pitch(void)      { return g_vgpu.fb_pitch; }
volatile uint32_t* virtio_gpu_framebuffer(void) { return g_vgpu.framebuffer; }
