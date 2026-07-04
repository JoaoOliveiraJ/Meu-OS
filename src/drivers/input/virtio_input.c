// ============================================================================
//  virtio_input.c — Driver virtio-input PCI (modern transport, virtio 1.1 spec).
//
//  Dirige um `virtio-tablet-pci` (absoluto). Ver virtio_input.h pro PORQUE.
//
//  Estrutura espelha o setup virtio modern do VirtioGpu.c (mesma sequencia da
//  spec 1.1 sec 3.1: reset -> ACK -> DRIVER -> features -> FEATURES_OK ->
//  virtqueues -> DRIVER_OK). Mantido AUTOCONTIDO de proposito: o encanamento do
//  VirtioGpu e' `static`/amarrado ao g_vgpu, e duplicar ~150 linhas de ABI fixa
//  e' mais seguro do que refatorar o driver de DISPLAY (critico). As structs
//  abaixo sao layout de ABI (spec), nao "invencao".
//
//  So a EVENTQ (queue 0) e' usada: o device escreve um virtio_input_event por
//  buffer e o coloca no used ring. Nao ha "request/response" — o device empurra
//  eventos assincronamente conforme a entrada chega. Fazemos POLLING (avail
//  marcada NO_INTERRUPT; sem IRQ wiring).
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "input/virtio_input.h"
#include "hal/hal.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "win32/win32k.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// ----- IDs / constantes virtio -----
#define VIRTIO_PCI_VENDOR          0x1AF4
#define VIRTIO_INPUT_DEVICE_ID     0x1052   // virtio device type 18 (input): 0x1040+18

#define PCI_CAP_ID_VENDOR          0x09
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

#define VIRTIO_STATUS_ACKNOWLEDGE  0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FEATURES_OK  0x08
#define VIRTIO_STATUS_FAILED       0x80
#define VIRTIO_F_VERSION_1_BIT     (1u << 0)   // bit 32, dentro do select=1

#define VQ_SIZE                    64
#define VIRTQ_DESC_F_NEXT          0x1
#define VIRTQ_DESC_F_WRITE         0x2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 0x1

// ----- ABI virtio (split virtqueue, spec sec 2.6) -----
typedef struct __attribute__((packed)) VIRTIO_COMMON_CFG {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} VIRTIO_COMMON_CFG;
_Static_assert(sizeof(VIRTIO_COMMON_CFG) == 0x38, "common cfg layout");

struct __attribute__((packed)) vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
struct __attribute__((packed)) vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
    uint16_t used_event;
};
struct __attribute__((packed)) vq_used_elem {
    uint32_t id;
    uint32_t len;
};
struct __attribute__((packed)) vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[VQ_SIZE];
    uint16_t avail_event;
};

// ----- evento virtio-input (Linux input_event compactado, spec sec 5.8.6) -----
struct __attribute__((packed)) virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};
_Static_assert(sizeof(struct virtio_input_event) == 8, "input event 8B");

// Tipos/codigos Linux input (subset que a tablet usa).
#define EV_SYN      0x00
#define EV_KEY      0x01
#define EV_ABS      0x03
#define SYN_REPORT  0x00
#define ABS_X       0x00
#define ABS_Y       0x01
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

// QEMU virtio-tablet reporta ABS_X/ABS_Y no intervalo 0..32767 (absinfo.max=
// 0x7fff fixo no hw/input/virtio-input-hid.c do QEMU). Escalamos pra tela.
#define VTAB_ABS_MAX  32767

// Convencao de botoes que o win32k espera (igual ao mouse PS/2): bit0=L bit1=R
// bit2=M (ver win32k_on_mouse_event).
#define BTN_BIT_LEFT    0x01
#define BTN_BIT_RIGHT   0x02
#define BTN_BIT_MIDDLE  0x04

// ----- estado do driver -----
static int                        s_active = 0;
static const hal_pci_device_t*    s_pci = 0;
static volatile VIRTIO_COMMON_CFG* s_common = 0;
static volatile uint8_t*          s_notify = 0;
static uint32_t                   s_notify_mult = 0;
static volatile uint16_t*         s_notify_reg = 0;

static volatile struct vq_desc*   s_desc  = 0;
static volatile struct vq_avail*  s_avail = 0;
static volatile struct vq_used*   s_used  = 0;
static uint16_t                   s_qsize = 0;
static uint16_t                   s_last_used = 0;
static volatile struct virtio_input_event* s_evbuf = 0;   // VQ_SIZE eventos

// Estado acumulado entre eventos ate o EV_SYN (a tablet envia ABS_X, ABS_Y,
// [EV_KEY], EV_SYN por movimento; comitamos no SYN_REPORT).
static int32_t  s_abs_x = 0, s_abs_y = 0;
static int      s_have_pos = 0;
static uint32_t s_buttons = 0;

static inline void mmio_mb(void) { __sync_synchronize(); }
static inline uint8_t  vrd8 (volatile uint8_t*  p) { return *p; }
static inline uint16_t vrd16(volatile uint16_t* p) { return *p; }
static inline uint32_t vrd32(volatile uint32_t* p) { return *p; }
static inline void     vwr8 (volatile uint8_t*  p, uint8_t  v) { *p = v; }
static inline void     vwr16(volatile uint16_t* p, uint16_t v) { *p = v; }
static inline void     vwr32(volatile uint32_t* p, uint32_t v) { *p = v; }
static inline void     vwr64(volatile uint64_t* p, uint64_t v) { *p = v; }

static void vi_bzero(volatile void* p, uint64_t n) {
    volatile uint8_t* q = (volatile uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) q[i] = 0;
}

// ----- PCI helpers (mesma logica do VirtioGpu) -----
static uint32_t pci_rd32(const hal_pci_device_t* d, uint8_t off) {
    return HalPciReadConfigUlong(d->bus, d->device, d->function, off);
}
static uint8_t pci_rd8(const hal_pci_device_t* d, uint8_t off) {
    return HalPciReadConfigUchar(d->bus, d->device, d->function, off);
}

static uint64_t read_bar_phys(const hal_pci_device_t* d, int bar_idx) {
    if (bar_idx < 0 || bar_idx >= 6) return 0;
    uint32_t lo = pci_rd32(d, (uint8_t)(0x10 + bar_idx * 4));
    if (lo & 1) return 0;                       // I/O space
    uint8_t type = (uint8_t)((lo >> 1) & 3);
    uint64_t base = (uint64_t)(lo & 0xFFFFFFF0u);
    if (type == 2 && bar_idx < 5) {             // 64-bit BAR
        uint32_t hi = pci_rd32(d, (uint8_t)(0x10 + (bar_idx + 1) * 4));
        base |= ((uint64_t)hi) << 32;
    }
    return base;
}

static void enable_pci_memory(const hal_pci_device_t* d) {
    uint32_t cmd  = pci_rd32(d, 0x04);
    uint32_t want = cmd | 0x6;     // Memory Space + Bus Master
    if (want != cmd)
        HalPciWriteConfigUlong(d->bus, d->device, d->function, 0x04, want);
}

// Mapeia [phys, phys+size) acima da identidade de 1 GiB. Base distinta da do
// VirtioGpu (0xFFFFC100...) pra nao colidir: 0xFFFFC110... + slot*1 MiB.
static volatile void* map_region(uint64_t phys, uint64_t size, int slot) {
    if (size == 0) return 0;
    uint64_t aligned_phys = phys & ~0xFFFULL;
    uint64_t skew = phys - aligned_phys;
    uint64_t aligned_size = (size + skew + 0xFFFULL) & ~0xFFFULL;
    uint64_t virt_base = 0xFFFFC11000000000ULL + (uint64_t)slot * 0x100000ULL;
    uint64_t mflags = MM_FLAG_PRESENT | MM_FLAG_RW | MM_FLAG_PCD;
    if (!mm_map_phys_range(virt_base, aligned_phys, aligned_size, mflags)) {
        kputs("[vinput] mm_map_phys_range FALHOU phys=0x"); kput_hex(phys); kputc('\n');
        return 0;
    }
    return (volatile void*)(uintptr_t)(virt_base + skew);
}

static const hal_pci_device_t* find_virtio_input(void) {
    int n = hal_pci_count();
    for (int i = 0; i < n; i++) {
        const hal_pci_device_t* d = hal_pci_get(i);
        if (!d) continue;
        if (d->vendor_id == VIRTIO_PCI_VENDOR && d->device_id == VIRTIO_INPUT_DEVICE_ID)
            return d;
    }
    return 0;
}

// ----- setup da eventq (queue 0) -----
static int eventq_setup(void) {
    // 1) Seleciona a queue 0.
    vwr16(&s_common->queue_select, 0);
    mmio_mb();
    uint16_t dev_qsize = vrd16(&s_common->queue_size);
    if (dev_qsize == 0) { kputs("[vinput] eventq queue_size=0\n"); return 0; }
    uint16_t qsize = dev_qsize > VQ_SIZE ? VQ_SIZE : dev_qsize;
    if (qsize != dev_qsize) { vwr16(&s_common->queue_size, qsize); mmio_mb(); }
    s_qsize = qsize;

    // 2) Notify register pra esta queue.
    uint16_t notify_off = vrd16(&s_common->queue_notify_off);
    s_notify_reg = (volatile uint16_t*)(uintptr_t)((uint64_t)(uintptr_t)s_notify +
                   (uint64_t)notify_off * (uint64_t)s_notify_mult);

    // 3) Aloca desc/avail/used (1 pagina cada; phys==virt na identidade) + 1
    //    pagina pros buffers de evento (qsize * 8 bytes).
    uint64_t desc_phys  = pmm_alloc_contiguous(1);
    uint64_t avail_phys = pmm_alloc_contiguous(1);
    uint64_t used_phys  = pmm_alloc_contiguous(1);
    uint64_t evb_phys   = pmm_alloc_contiguous(1);
    if (!desc_phys || !avail_phys || !used_phys || !evb_phys) {
        kputs("[vinput] pmm_alloc_contiguous falhou (sem RAM)\n"); return 0;
    }
    s_desc  = (volatile struct vq_desc*) (uintptr_t)desc_phys;
    s_avail = (volatile struct vq_avail*)(uintptr_t)avail_phys;
    s_used  = (volatile struct vq_used*) (uintptr_t)used_phys;
    s_evbuf = (volatile struct virtio_input_event*)(uintptr_t)evb_phys;
    vi_bzero(s_desc, 4096);
    vi_bzero(s_avail, 4096);
    vi_bzero(s_used, 4096);
    vi_bzero((volatile void*)s_evbuf, 4096);

    // 4) Cada descriptor i aponta pro buffer i e e' GRAVAVEL pelo device. Todos
    //    ficam disponiveis no avail ring desde o inicio (receive queue).
    s_avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;   // pure polling: sem IRQ
    for (uint16_t i = 0; i < qsize; i++) {
        s_desc[i].addr  = evb_phys + (uint64_t)i * sizeof(struct virtio_input_event);
        s_desc[i].len   = sizeof(struct virtio_input_event);
        s_desc[i].flags = VIRTQ_DESC_F_WRITE;
        s_desc[i].next  = 0;
        s_avail->ring[i] = i;
    }
    mmio_mb();
    s_avail->idx = qsize;     // todos os buffers disponiveis
    s_last_used  = 0;

    // 5) Escreve os enderecos fisicos e habilita.
    vwr64(&s_common->queue_desc,   desc_phys);
    vwr64(&s_common->queue_driver, avail_phys);
    vwr64(&s_common->queue_device, used_phys);
    mmio_mb();
    vwr16(&s_common->queue_enable, 1);
    mmio_mb();

    kputs("[vinput] eventq size="); kput_dec(qsize);
    kputs(" desc@0x"); kput_hex(desc_phys);
    kputs(" evbuf@0x"); kput_hex(evb_phys);
    kputs(" notify_reg=0x"); kput_hex((uint64_t)(uintptr_t)s_notify_reg); kputc('\n');
    return 1;
}

void virtio_input_init(void) {
    const hal_pci_device_t* d = find_virtio_input();
    if (!d) {
        kputs("[vinput] virtio-input nao encontrado "
              "(rode com -device virtio-tablet-pci p/ cursor absoluto).\n");
        return;
    }
    s_pci = d;
    kputs("[vinput] PCI "); kput_hex(d->vendor_id); kputc(':'); kput_hex(d->device_id);
    kputs(" (virtio-tablet) bus="); kput_dec(d->bus);
    kputs(" dev="); kput_dec(d->device); kputs(" func="); kput_dec(d->function); kputc('\n');

    enable_pci_memory(d);

    // Caminha as capabilities (offset 0x34) coletando COMMON/NOTIFY/ISR/DEVICE.
    struct { uint8_t bar; uint32_t off; uint32_t len; int found; } regions[6] = {0};
    uint8_t cap_off = pci_rd8(d, 0x34);
    int safety = 0;
    while (cap_off != 0 && safety++ < 64) {
        uint8_t cap_vndr = pci_rd8(d, cap_off + 0x00);
        uint8_t cap_next = pci_rd8(d, cap_off + 0x01);
        uint8_t cap_len  = pci_rd8(d, cap_off + 0x02);
        uint8_t cfg_type = pci_rd8(d, cap_off + 0x03);
        if (cap_vndr == PCI_CAP_ID_VENDOR && cap_len >= 16 && cfg_type >= 1 && cfg_type <= 4) {
            regions[cfg_type].bar   = pci_rd8(d, cap_off + 0x04);
            regions[cfg_type].off   = pci_rd32(d, cap_off + 0x08);
            regions[cfg_type].len   = pci_rd32(d, cap_off + 0x0C);
            regions[cfg_type].found = 1;
            if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && cap_len >= 20)
                s_notify_mult = pci_rd32(d, cap_off + 0x10);
        }
        if (cap_next == 0) break;
        cap_off = cap_next;
    }
    if (!regions[VIRTIO_PCI_CAP_COMMON_CFG].found || !regions[VIRTIO_PCI_CAP_NOTIFY_CFG].found) {
        kputs("[vinput] faltou COMMON/NOTIFY cap; bail.\n");
        return;
    }

    // Mapeia cada regiao encontrada.
    int slot = 0;
    for (int t = 1; t <= 4; t++) {
        if (!regions[t].found) continue;
        uint64_t bar_phys = read_bar_phys(d, regions[t].bar);
        if (!bar_phys) { kputs("[vinput] BAR invalido p/ cfg_type "); kput_dec(t); kputc('\n'); return; }
        volatile void* virt = map_region(bar_phys + regions[t].off, regions[t].len, slot++);
        if (!virt) return;
        if (t == VIRTIO_PCI_CAP_COMMON_CFG) s_common = (volatile VIRTIO_COMMON_CFG*)virt;
        else if (t == VIRTIO_PCI_CAP_NOTIFY_CFG) s_notify = (volatile uint8_t*)virt;
    }
    if (!s_common || !s_notify) { kputs("[vinput] common/notify nao mapeou; bail.\n"); return; }

    // Status: reset -> ACK -> DRIVER.
    vwr8(&s_common->device_status, 0);
    mmio_mb();
    for (int i = 0; i < 100 && vrd8(&s_common->device_status) != 0; i++) mmio_mb();
    vwr8(&s_common->device_status, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_mb();
    vwr8(&s_common->device_status, (uint8_t)(vrd8(&s_common->device_status) | VIRTIO_STATUS_DRIVER));
    mmio_mb();

    // Features: aceitar APENAS VIRTIO_F_VERSION_1 (bit 32, no select=1).
    vwr32(&s_common->device_feature_select, 1);
    mmio_mb();
    uint32_t feat_hi = vrd32(&s_common->device_feature);
    vwr32(&s_common->driver_feature_select, 0);
    mmio_mb();
    vwr32(&s_common->driver_feature, 0);
    mmio_mb();
    vwr32(&s_common->driver_feature_select, 1);
    mmio_mb();
    vwr32(&s_common->driver_feature, (feat_hi & VIRTIO_F_VERSION_1_BIT) ? VIRTIO_F_VERSION_1_BIT : 0);
    mmio_mb();

    // FEATURES_OK.
    uint8_t st = vrd8(&s_common->device_status);
    vwr8(&s_common->device_status, (uint8_t)(st | VIRTIO_STATUS_FEATURES_OK));
    mmio_mb();
    st = vrd8(&s_common->device_status);
    if (!(st & VIRTIO_STATUS_FEATURES_OK)) {
        kputs("[vinput] FEATURES_OK nao persistiu; FAILED.\n");
        vwr8(&s_common->device_status, (uint8_t)(st | VIRTIO_STATUS_FAILED));
        return;
    }

    uint16_t nq = vrd16(&s_common->num_queues);
    if (nq < 1) { kputs("[vinput] num_queues<1; bail.\n"); return; }

    if (!eventq_setup()) {
        vwr8(&s_common->device_status, (uint8_t)(vrd8(&s_common->device_status) | VIRTIO_STATUS_FAILED));
        return;
    }

    // DRIVER_OK: a partir daqui o QEMU ativa a tablet como ponteiro absoluto.
    st = vrd8(&s_common->device_status);
    vwr8(&s_common->device_status, (uint8_t)(st | VIRTIO_STATUS_DRIVER_OK));
    mmio_mb();
    st = vrd8(&s_common->device_status);
    if (!(st & VIRTIO_STATUS_DRIVER_OK)) {
        kputs("[vinput] DRIVER_OK nao persistiu (status=0x"); kput_hex(st); kputs("); FAILED.\n");
        return;
    }

    // Notifica a eventq pra o device pegar os buffers disponiveis.
    if (s_notify_reg) { mmio_mb(); *s_notify_reg = 0; }

    s_active = 1;
    kputs("[vinput] DRIVER_OK — tablet ABSOLUTA ativa (QEMU sai do grab; cursor "
          "de HW deve aparecer e seguir o mouse).\n");
}

// Comita o estado acumulado (chamado no EV_SYN/SYN_REPORT).
static void commit_state(void) {
    int W = win32k_screen_width();
    int H = win32k_screen_height();
    if (W <= 1) W = 2;
    if (H <= 1) H = 2;
    int32_t px = s_have_pos ? (int32_t)(((int64_t)s_abs_x * (W - 1)) / VTAB_ABS_MAX)
                            : win32k_cursor_x();
    int32_t py = s_have_pos ? (int32_t)(((int64_t)s_abs_y * (H - 1)) / VTAB_ABS_MAX)
                            : win32k_cursor_y();
    win32k_on_mouse_abs(px, py, s_buttons);
}

static void handle_event(uint16_t type, uint16_t code, uint32_t value) {
    switch (type) {
        case EV_ABS:
            if      (code == ABS_X) { s_abs_x = (int32_t)value; s_have_pos = 1; }
            else if (code == ABS_Y) { s_abs_y = (int32_t)value; s_have_pos = 1; }
            break;
        case EV_KEY: {
            uint32_t bit = 0;
            if      (code == BTN_LEFT)   bit = BTN_BIT_LEFT;
            else if (code == BTN_RIGHT)  bit = BTN_BIT_RIGHT;
            else if (code == BTN_MIDDLE) bit = BTN_BIT_MIDDLE;
            if (bit) { if (value) s_buttons |= bit; else s_buttons &= ~bit; }
            break;
        }
        case EV_SYN:
            if (code == SYN_REPORT) commit_state();
            break;
        default:
            break;   // EV_REL/EV_MSC/etc: ignorado (tablet e' absoluta)
    }
}

void virtio_input_poll(void) {
    if (!s_active) return;
    int recycled = 0;
    // Drena tudo que o device completou desde o ultimo poll.
    while (s_last_used != s_used->idx) {
        uint16_t ui  = (uint16_t)(s_last_used % s_qsize);
        uint32_t id  = s_used->ring[ui].id;
        uint32_t len = s_used->ring[ui].len;
        s_last_used = (uint16_t)(s_last_used + 1);
        if (id < s_qsize && len >= sizeof(struct virtio_input_event)) {
            uint16_t type  = s_evbuf[id].type;
            uint16_t code  = s_evbuf[id].code;
            uint32_t value = s_evbuf[id].value;
            handle_event(type, code, value);
        }
        // Recicla o buffer (id == indice do descriptor) de volta pro avail ring.
        if (id < s_qsize) {
            uint16_t ai = (uint16_t)(s_avail->idx % s_qsize);
            s_avail->ring[ai] = (uint16_t)id;
            mmio_mb();
            s_avail->idx = (uint16_t)(s_avail->idx + 1);
            recycled = 1;
        }
    }
    if (recycled && s_notify_reg) { mmio_mb(); *s_notify_reg = 0; }
}

int virtio_input_active(void) { return s_active; }
