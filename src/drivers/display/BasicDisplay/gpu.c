// ============================================================================
//  gpu.c — Camada generica de GPU sobre o(s) driver(s) instalado(s).
//
//  Backends (em ordem de preferencia, decidido em runtime no gpu_init):
//    1) virtio-gpu (drivers/display/VirtioGpu)
//         - Modern virtio PCI 1AF4:1050 (ou transitional 1040..107F+sub=0x10).
//         - Framebuffer DMA alocado pelo driver via PMM (identidade cobre, virt
//           == phys). O host SO RE-LE o backing apos TRANSFER_TO_HOST_2D +
//           RESOURCE_FLUSH; entao gpu_present() VIRA o vsync logico desse path.
//         - Quando ativo, as primitivas (clear/pixel/fill_rect/copy_rect) usam
//           o FB virtio diretamente (32 bpp BGRA).
//
//    2) Bochs VBE (drivers/display/BasicDisplay)
//         - PCI 1234:1111 com VBE Dispi. LFB e MMIO, sem comandos: cada escrita
//           no LFB ja vai pra tela (sem need de present). gpu_present() = no-op.
//
//  As primitivas trabalham em RGBA32 (0xAARRGGBB). Em 32 bpp BGRA o LFB usa
//  byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = X — armazenamos como uint32_t
//  little-endian: a palavra fica 0x00RRGGBB, identica ao RGBA sem alpha. Isto
//  permite uma unica funcao pack32 servir aos dois backends.
//
//  Clipping em cada primitiva — coordenadas fora da tela sao descartadas.
// ============================================================================
#include <stdint.h>
#include "gpu.h"
#include "BasicDisplay.h"
#include "display/VirtioGpu/VirtioGpu.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado: qual backend foi selecionado em gpu_init -----------------------
typedef enum { GPU_BACKEND_NONE = 0, GPU_BACKEND_VGPU, GPU_BACKEND_BOCHS } gpu_backend_t;
static gpu_backend_t s_backend = GPU_BACKEND_NONE;
static int           s_active  = 0;

// ----------------------------------------------------------------------------
//  Funcoes de geometria por backend (zero overhead — branch unico em runtime).
// ----------------------------------------------------------------------------
static inline uint32_t pack32(uint32_t rgba) {
    return rgba & 0x00FFFFFFu;
}

static inline uint32_t gpu_w_active(void) {
    if (s_backend == GPU_BACKEND_VGPU)  return virtio_gpu_fb_width();
    if (s_backend == GPU_BACKEND_BOCHS) return bochsvbe_width();
    return 0;
}
static inline uint32_t gpu_h_active(void) {
    if (s_backend == GPU_BACKEND_VGPU)  return virtio_gpu_fb_height();
    if (s_backend == GPU_BACKEND_BOCHS) return bochsvbe_height();
    return 0;
}
static inline uint32_t gpu_pitch_active(void) {
    if (s_backend == GPU_BACKEND_VGPU)  return virtio_gpu_fb_pitch();
    if (s_backend == GPU_BACKEND_BOCHS) return bochsvbe_pitch();
    return 0;
}

// Acesso ao framebuffer em virt (writable). Devolve uint8_t* p/ permitir
// aritmetica de pitch e indexacao por linha; o caller que conhece o BPP faz
// o cast pra uint32_t (32 bpp).
static inline uint8_t* gpu_fb_active(void) {
    if (s_backend == GPU_BACKEND_VGPU)  return (uint8_t*)virtio_gpu_framebuffer();
    if (s_backend == GPU_BACKEND_BOCHS) return bochsvbe_lfb();
    return 0;
}

// ----------------------------------------------------------------------------
//  gpu_init — tenta virtio-gpu primeiro; senao Bochs VBE; senao falha.
// ----------------------------------------------------------------------------
int gpu_init(uint32_t width, uint32_t height) {
    if (s_active) return 1;

    // 1) virtio-gpu PRIMEIRO. Se o detect+queues ocorreu antes (em main.c) e o
    //    host tem o device, init_display vai alocar FB DMA, criar resource 2D,
    //    attach backing, e SET_SCANOUT no scanout 0. Pos esse passo, o host
    //    apresenta NOSSO framebuffer.
    if (virtio_gpu_active() && virtio_gpu_init_display(width, height)) {
        s_backend = GPU_BACKEND_VGPU;
        s_active  = 1;
        kputs("[gpu] backend = virtio-gpu ");
        kput_dec(virtio_gpu_fb_width());  kputs("x");
        kput_dec(virtio_gpu_fb_height()); kputs("x32 BGRA");
        kputs("  fb@virt=0x"); kput_hex((uint64_t)(uintptr_t)virtio_gpu_framebuffer());
        kputs("  pitch="); kput_dec(virtio_gpu_fb_pitch()); kputs("\n");
        return 1;
    }

    // 2) Fallback: Bochs VBE em 32 bpp.
    if (bochsvbe_init(width, height, 32)) {
        s_backend = GPU_BACKEND_BOCHS;
        s_active  = 1;
        kputs("[gpu] backend = Bochs VBE (fallback) ");
        kput_dec(bochsvbe_width()); kputs("x");
        kput_dec(bochsvbe_height()); kputs("x");
        kput_dec(bochsvbe_bpp());
        kputs("  LFB=0x"); kput_hex((uint64_t)(uintptr_t)bochsvbe_lfb());
        kputs("  pitch="); kput_dec(bochsvbe_pitch());
        kputs("\n");
        return 1;
    }

    kputs("[gpu] nem virtio-gpu nem Bochs VBE disponiveis; GPU NAO ativada.\n");
    return 0;
}

int      gpu_active(void) {
    if (!s_active) return 0;
    if (s_backend == GPU_BACKEND_VGPU)  return virtio_gpu_display_ok();
    if (s_backend == GPU_BACKEND_BOCHS) return bochsvbe_active();
    return 0;
}
uint32_t gpu_width(void)  { return gpu_w_active(); }
uint32_t gpu_height(void) { return gpu_h_active(); }
uint32_t gpu_pitch(void)  { return gpu_pitch_active(); }
uint32_t gpu_bpp(void)    {
    // Ambos os backends servidos pelo gpu_init operam em 32 bpp (BGRA).
    return 32;
}

// ----------------------------------------------------------------------------
//  Primitivas — operam sempre em 32 bpp (ambos backends). Coordenadas fora da
//  tela sao descartadas; w/h negativos saem como no-op.
// ----------------------------------------------------------------------------
void gpu_clear(uint32_t rgba) {
    if (!gpu_active()) return;
    uint8_t*  fb    = gpu_fb_active();
    uint32_t  pitch = gpu_pitch_active();
    uint32_t  h     = gpu_h_active();
    uint32_t  w     = gpu_w_active();
    uint32_t  v     = pack32(rgba);

    for (uint32_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)(fb + y * pitch);
        for (uint32_t x = 0; x < w; x++) row[x] = v;
    }
}

void gpu_pixel(int x, int y, uint32_t rgba) {
    if (!gpu_active()) return;
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= gpu_w_active() || (uint32_t)y >= gpu_h_active()) return;
    uint8_t* fb = gpu_fb_active();
    uint32_t* row = (uint32_t*)(fb + (uint32_t)y * gpu_pitch_active());
    row[x] = pack32(rgba);
}

void gpu_fill_rect(int x, int y, int w, int h, uint32_t rgba) {
    if (!gpu_active()) return;
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; int y1 = y + h;
    if (x1 > (int)gpu_w_active()) x1 = (int)gpu_w_active();
    if (y1 > (int)gpu_h_active()) y1 = (int)gpu_h_active();
    if (x1 <= x0 || y1 <= y0) return;

    uint8_t* fb = gpu_fb_active();
    uint32_t pitch = gpu_pitch_active();
    uint32_t v = pack32(rgba);

    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = (uint32_t*)(fb + (uint32_t)yy * pitch);
        for (int xx = x0; xx < x1; xx++) row[xx] = v;
    }
}

// Copia retangulo intra-LFB. Handle overlap fazendo a copia em ordem
// reversa quando o destino esta abaixo/depois da origem.
void gpu_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    if (!gpu_active()) return;
    if (w <= 0 || h <= 0) return;
    int W = (int)gpu_w_active(), H = (int)gpu_h_active();
    // Clip basico: cancela copia se qualquer canto sai da tela.
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0) return;
    if (sx + w > W || sy + h > H) return;
    if (dx + w > W || dy + h > H) return;

    uint8_t* fb = gpu_fb_active();
    uint32_t pitch = gpu_pitch_active();
    const uint32_t bpp_b = 4;        // ambos backends sao 32 bpp

    if (dy > sy || (dy == sy && dx > sx)) {
        // Backwards (linhas de baixo p/ cima) p/ tolerar overlap descendente.
        for (int j = h - 1; j >= 0; j--) {
            uint8_t* src = fb + (sy + j) * pitch + sx * bpp_b;
            uint8_t* dst = fb + (dy + j) * pitch + dx * bpp_b;
            for (int i = w * (int)bpp_b - 1; i >= 0; i--) dst[i] = src[i];
        }
    } else {
        for (int j = 0; j < h; j++) {
            uint8_t* src = fb + (sy + j) * pitch + sx * bpp_b;
            uint8_t* dst = fb + (dy + j) * pitch + dx * bpp_b;
            for (int i = 0; i < w * (int)bpp_b; i++) dst[i] = src[i];
        }
    }
}

void gpu_present(void) {
    // virtio-gpu: TRANSFER_TO_HOST_2D + RESOURCE_FLUSH para publicar o frame.
    // Bochs VBE: cada escrita ja vai pra VRAM (LFB MMIO); no-op.
    if (s_backend == GPU_BACKEND_VGPU) virtio_gpu_present();
}

// ----------------------------------------------------------------------------
//  Cursor de HARDWARE. So o backend virtio-gpu tem cursor queue. No Bochs VBE
//  nao ha — devolvemos 0 e o win32k cai no sprite de software (recompose).
// ----------------------------------------------------------------------------
int gpu_cursor_set(const uint32_t* img64x64, uint32_t hot_x, uint32_t hot_y,
                   int init_x, int init_y) {
    if (s_backend != GPU_BACKEND_VGPU) return 0;
    if (init_x < 0) init_x = 0;
    if (init_y < 0) init_y = 0;
    return virtio_gpu_cursor_init(img64x64, hot_x, hot_y,
                                  (uint32_t)init_x, (uint32_t)init_y);
}

void gpu_cursor_move(int x, int y) {
    if (s_backend != GPU_BACKEND_VGPU) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    virtio_gpu_cursor_move((uint32_t)x, (uint32_t)y);
}

int gpu_has_hw_cursor(void) {
    return (s_backend == GPU_BACKEND_VGPU) && virtio_gpu_cursor_ok();
}
