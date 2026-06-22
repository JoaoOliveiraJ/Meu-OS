// ============================================================================
//  dxgkrnl.c — DirectX kernel dispatcher (estilo dxgkrnl.sys do W10/11).
//
//  Inicializa o subsistema DX, gerencia pools estaticos de adapter/device/
//  context/allocation, e delega o trabalho real (copiar para a tela) para
//  o backend gpu_* do BasicDisplay (LFB Bochs VBE).
//
//  Por que pools estaticos:
//   - kernel ainda nao tem GC; lifetime dos objetos e gerenciado pelo
//     proprio caller (Dxgk*Create + Dxgk*Destroy ao fim do app);
//   - pool fixo + slot in_use = lookup O(N) trivial, sem free-list, e
//     o N e pequeno (4/16/32/64) — folgado pro uso atual (1 dxdemo).
//   - WDDM real usa LIST_ENTRY + handle table, mas isso e complexidade que
//     so vai pagar quando tivermos varios apps DX simultaneos.
//
//  Sobre DxgkPresentDisplayOnly: e o caminho exato de um "display-only
//  adapter" WDDM (BasicDisplay e indddi.sys do XP em mode VPI). UMD passa
//  o buffer cru, dxgkrnl chama Dxgk*Ddi*PresentDisplayOnly do KMD; aqui
//  o "KMD" e a propria libgpu (gpu_pixel/gpu_fill_rect/gpu_copy_rect).
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "dxgkrnl.h"
#include "mm/heap.h"
#include "display/BasicDisplay/gpu.h"

extern void kputs(const char* s);
extern void kput_dec(uint64_t v);
extern void kput_hex(uint64_t v);

// Limites do espelho-WDDM. Numeros baixos a proposito: sao folgados pra 1-2 apps
// de DirectX rodando simultaneamente, e qualquer "vazamento" e visivel de cara.
#define DXGK_MAX_ADAPTERS    4
#define DXGK_MAX_DEVICES     16
#define DXGK_MAX_CONTEXTS    32
#define DXGK_MAX_ALLOCATIONS 64

static DXGK_ADAPTER    g_adapters   [DXGK_MAX_ADAPTERS];
static DXGK_DEVICE     g_devices    [DXGK_MAX_DEVICES];
static DXGK_CONTEXT    g_contexts   [DXGK_MAX_CONTEXTS];
static DXGK_ALLOCATION g_allocations[DXGK_MAX_ALLOCATIONS];

static int g_dxgk_initialized = 0;

// Procura o primeiro slot livre num dos pools. Generico via macro pra evitar
// copiar a logica 4x — sao 4 tipos diferentes mas o teste "in_use" e identico.
#define FIND_FREE_SLOT(pool, max, name)                                       \
    static int dxgk_find_free_##name(void) {                                  \
        for (int i = 0; i < (max); i++) if (!(pool)[i].in_use) return i;      \
        return -1;                                                            \
    }
// adapter slot: nao tem find_free hoje (o adapter primario = slot 0 sempre);
// quando suportarmos multi-GPU isso ja existe e basta tirar o atributo unused.
__attribute__((unused))
FIND_FREE_SLOT(g_adapters,    DXGK_MAX_ADAPTERS,    adapter)
FIND_FREE_SLOT(g_devices,     DXGK_MAX_DEVICES,     device)
FIND_FREE_SLOT(g_contexts,    DXGK_MAX_CONTEXTS,    context)
FIND_FREE_SLOT(g_allocations, DXGK_MAX_ALLOCATIONS, allocation)

// ============================================================================
//  DxgkInitialize: chamada por kmain APOS gpu_init. Le o estado da GPU e
//  prepara o adapter primario (#0). Mesmo se gpu_active() == 0, criamos o
//  adapter primario "sem display" — drivers reais (BasicDisplay/indddi)
//  toleram esse estado e ficam dormentes ate alguem chamar Dxgk*Open*.
// ============================================================================
NTSTATUS NTAPI DxgkInitialize(void) {
    if (g_dxgk_initialized) {
        kputs("[dxgk] DxgkInitialize: ja inicializado (no-op).\n");
        return STATUS_SUCCESS;
    }
    // Zera os pools (static -> ja zerado, mas explicito).
    for (int i = 0; i < DXGK_MAX_ADAPTERS;    i++) g_adapters   [i].in_use = 0;
    for (int i = 0; i < DXGK_MAX_DEVICES;     i++) g_devices    [i].in_use = 0;
    for (int i = 0; i < DXGK_MAX_CONTEXTS;    i++) g_contexts   [i].in_use = 0;
    for (int i = 0; i < DXGK_MAX_ALLOCATIONS; i++) g_allocations[i].in_use = 0;

    // Prepara o adapter primario (slot 0). gpu_active = 0 -> adapter "headless".
    DXGK_ADAPTER* a = &g_adapters[0];
    a->in_use        = 1;
    a->kmd_context   = 0;          // o KMD (BasicDisplay) e implicito
    a->adapter_index = 0;
    if (gpu_active()) {
        a->width  = gpu_width();
        a->height = gpu_height();
        a->bpp    = gpu_bpp();
        a->pitch  = gpu_pitch();
        kputs("[dxgk] DxgkInitialize: adapter primario ");
        kput_dec(a->width); kputs("x"); kput_dec(a->height);
        kputs("x"); kput_dec(a->bpp);
        kputs(" pitch="); kput_dec(a->pitch);
        kputs(" (gpu backend OK)\n");
    } else {
        a->width = a->height = a->bpp = a->pitch = 0;
        kputs("[dxgk] DxgkInitialize: adapter primario sem display "
              "(gpu_active=0; modo dormente)\n");
    }
    g_dxgk_initialized = 1;
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkOpenAdapter: devolve o adapter primario. Aqui nao tem enumeracao porque
//  o nosso loop de display so reconhece 1 placa (a Bochs VBE do QEMU).
// ============================================================================
NTSTATUS NTAPI DxgkOpenAdapter(DXGK_ADAPTER** out_adapter) {
    if (out_adapter) *out_adapter = 0;
    if (!g_dxgk_initialized) DxgkInitialize();
    if (!g_adapters[0].in_use) {
        kputs("[dxgk] OpenAdapter: adapter primario nao existe.\n");
        return STATUS_DEVICE_NOT_READY;
    }
    if (out_adapter) *out_adapter = &g_adapters[0];
    kputs("[dxgk] OpenAdapter: adapter primario devolvido (idx=0).\n");
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkCreateDevice: cria um D3D device dentro de um adapter. Cada app que
//  abre D3D vai chamar isso uma vez por instancia.
// ============================================================================
NTSTATUS NTAPI DxgkCreateDevice(DXGK_ADAPTER* adapter, DXGK_DEVICE** out_device) {
    if (out_device) *out_device = 0;
    if (!adapter || !adapter->in_use) return STATUS_INVALID_PARAMETER;
    int slot = dxgk_find_free_device();
    if (slot < 0) {
        kputs("[dxgk] CreateDevice: pool de devices cheio.\n");
        return STATUS_NO_MEMORY;
    }
    DXGK_DEVICE* d = &g_devices[slot];
    d->in_use       = 1;
    d->adapter      = adapter;
    d->device_index = (uint32_t)slot;
    if (out_device) *out_device = d;
    kputs("[dxgk] CreateDevice: idx="); kput_dec(slot);
    kputs(" (adapter idx="); kput_dec(adapter->adapter_index); kputs(")\n");
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkCreateContext: queue de comandos. No NT real existe a separacao
//  "node" (GPU engine: 3D, video, copy) — aqui uniformizamos.
// ============================================================================
NTSTATUS NTAPI DxgkCreateContext(DXGK_DEVICE* device, DXGK_CONTEXT** out_ctx) {
    if (out_ctx) *out_ctx = 0;
    if (!device || !device->in_use) return STATUS_INVALID_PARAMETER;
    int slot = dxgk_find_free_context();
    if (slot < 0) {
        kputs("[dxgk] CreateContext: pool de contexts cheio.\n");
        return STATUS_NO_MEMORY;
    }
    DXGK_CONTEXT* c = &g_contexts[slot];
    c->in_use         = 1;
    c->device         = device;
    c->context_index  = (uint32_t)slot;
    c->submitted_cmds = 0;
    if (out_ctx) *out_ctx = c;
    kputs("[dxgk] CreateContext: idx="); kput_dec(slot);
    kputs(" (device idx="); kput_dec(device->device_index); kputs(")\n");
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkCreateAllocation: backed por kmalloc. Sem VRAM, o ponteiro virtual ja
//  aponta pra RAM acessivel pela CPU (LFB para uma surface seria diferente,
//  mas isso e papel do DxgkPresent).
// ============================================================================
NTSTATUS NTAPI DxgkCreateAllocation(DXGK_DEVICE* device,
                                    uint64_t size,
                                    DXGK_ALLOCATION** out_alloc) {
    if (out_alloc) *out_alloc = 0;
    if (!device || !device->in_use) return STATUS_INVALID_PARAMETER;
    if (size == 0 || size > (uint64_t)0x10000000ULL) {
        kputs("[dxgk] CreateAllocation: size invalido ("); kput_dec(size);
        kputs(").\n");
        return STATUS_INVALID_PARAMETER;
    }
    int slot = dxgk_find_free_allocation();
    if (slot < 0) {
        kputs("[dxgk] CreateAllocation: pool de allocations cheio.\n");
        return STATUS_NO_MEMORY;
    }
    void* base = kmalloc((size_t)size);
    if (!base) {
        kputs("[dxgk] CreateAllocation: kmalloc("); kput_dec(size);
        kputs(") falhou.\n");
        return STATUS_NO_MEMORY;
    }
    DXGK_ALLOCATION* a = &g_allocations[slot];
    a->in_use = 1;
    a->device = device;
    a->size   = size;
    a->base   = base;
    a->width  = 0; a->height = 0; a->pitch = 0; a->bpp = 0;
    if (out_alloc) *out_alloc = a;
    kputs("[dxgk] CreateAllocation: idx="); kput_dec(slot);
    kputs(" size="); kput_dec(size);
    kputs(" base="); kput_hex((uint64_t)(uintptr_t)base);
    kputs("\n");
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkPresent: copia uma DXGK_ALLOCATION para o LFB. Stub: se a allocation
//  for "buffer cru" (width=0) so logamos; do contrario, chamamos
//  gpu_fill_rect/gpu_pixel pra desenhar dentro do retangulo (x,y,w,h).
//  Implementacao real faria copia pixel-a-pixel respeitando a->pitch — fica
//  como TODO ate termos um app que rasterize seu proprio framebuffer.
// ============================================================================
NTSTATUS NTAPI DxgkPresent(DXGK_DEVICE* device,
                          DXGK_ALLOCATION* src,
                          int x, int y, int w, int h) {
    if (!device || !device->in_use) return STATUS_INVALID_PARAMETER;
    if (!src    || !src->in_use)    return STATUS_INVALID_PARAMETER;
    if (!gpu_active()) {
        kputs("[dxgk] Present: gpu_active=0; sem display, no-op.\n");
        return STATUS_DEVICE_NOT_READY;
    }
    kputs("[dxgk] Present: alloc id ");
    kput_hex((uint64_t)(uintptr_t)src->base);
    kputs(" -> rect x="); kput_dec((uint64_t)x);
    kputs(" y=");          kput_dec((uint64_t)y);
    kputs(" w=");          kput_dec((uint64_t)w);
    kputs(" h=");          kput_dec((uint64_t)h);
    kputs("\n");
    // Stub: pinta um retangulo solido com o primeiro DWORD da allocation como
    // cor — comprova que o caminho funcionou no screendump sem precisar de
    // um blit completo.
    uint32_t color = 0;
    if (src->base && src->size >= 4) color = *(const uint32_t*)src->base;
    gpu_fill_rect(x, y, w, h, color);
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkSubmitCommand: stub. Sem command processor / sem ISA de GPU, nao temos
//  como decodificar. Log + STATUS_SUCCESS — drivers reais tolerariam.
// ============================================================================
NTSTATUS NTAPI DxgkSubmitCommand(DXGK_CONTEXT* ctx,
                                void* cmd_buf,
                                uint32_t size) {
    if (!ctx || !ctx->in_use) return STATUS_INVALID_PARAMETER;
    (void)cmd_buf;   // o conteudo do command buffer e privado da arquitetura GPU
    ctx->submitted_cmds++;
    kputs("[dxgk] SubmitCommand: ctx idx="); kput_dec(ctx->context_index);
    kputs(" cmd_buf=");                       kput_hex((uint64_t)(uintptr_t)cmd_buf);
    kputs(" size=");                          kput_dec((uint64_t)size);
    kputs(" total_subs=");                    kput_dec(ctx->submitted_cmds);
    kputs("\n");
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkPresentDisplayOnly: caminho "display-only adapter" do WDDM. UMD passa
//  o framebuffer source cru (pitch + width + height) e queremos jogar isso na
//  tela. Aqui implementamos pixel-a-pixel via gpu_pixel — devagar mas correto
//  (cobre o caso de qualquer pitch / qualquer offset).
// ============================================================================
NTSTATUS NTAPI DxgkPresentDisplayOnly(DXGK_ADAPTER* adapter,
                                     void* src,
                                     uint32_t pitch,
                                     int width,
                                     int height) {
    if (!adapter || !adapter->in_use) return STATUS_INVALID_PARAMETER;
    if (!src || pitch == 0 || width <= 0 || height <= 0)
        return STATUS_INVALID_PARAMETER;
    if (!gpu_active()) {
        kputs("[dxgk] PresentDisplayOnly: sem display ativo (no-op).\n");
        return STATUS_DEVICE_NOT_READY;
    }
    // Assume formato 32 bpp (XRGB) tanto na source quanto no LFB. Outros
    // formatos teriam de passar por um path de conversao — fora do escopo.
    int max_w = (int)adapter->width;
    int max_h = (int)adapter->height;
    int eff_w = width  < max_w ? width  : max_w;
    int eff_h = height < max_h ? height : max_h;
    const uint8_t* row = (const uint8_t*)src;
    for (int y = 0; y < eff_h; y++) {
        const uint32_t* px = (const uint32_t*)row;
        for (int x = 0; x < eff_w; x++) gpu_pixel(x, y, px[x]);
        row += pitch;
    }
    kputs("[dxgk] PresentDisplayOnly: blit ");
    kput_dec((uint64_t)eff_w); kputs("x"); kput_dec((uint64_t)eff_h);
    kputs(" pitch="); kput_dec((uint64_t)pitch);
    kputs(" -> LFB OK\n");
    return STATUS_SUCCESS;
}

// ============================================================================
//  DxgkShutdown: limpa todos os pools (libera memoria das allocations).
//  Idempotente — pode ser chamado em paths de erro.
// ============================================================================
NTSTATUS NTAPI DxgkShutdown(void) {
    int n_alloc = 0;
    for (int i = 0; i < DXGK_MAX_ALLOCATIONS; i++) {
        if (g_allocations[i].in_use) {
            if (g_allocations[i].base) kfree(g_allocations[i].base);
            g_allocations[i].in_use = 0;
            n_alloc++;
        }
    }
    for (int i = 0; i < DXGK_MAX_CONTEXTS; i++) g_contexts[i].in_use = 0;
    for (int i = 0; i < DXGK_MAX_DEVICES;  i++) g_devices [i].in_use = 0;
    for (int i = 0; i < DXGK_MAX_ADAPTERS; i++) g_adapters[i].in_use = 0;
    g_dxgk_initialized = 0;
    kputs("[dxgk] Shutdown: "); kput_dec((uint64_t)n_alloc);
    kputs(" allocations liberadas.\n");
    return STATUS_SUCCESS;
}
