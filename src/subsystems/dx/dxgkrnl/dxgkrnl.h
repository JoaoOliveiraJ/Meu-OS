#pragma once
#include <stdint.h>
#include "ntddk.h"

// ============================================================================
//  dxgkrnl.h  —  DirectX kernel dispatcher (estilo dxgkrnl.sys do Windows 10/11).
//
//  Aqui mora o dispatcher central do DirectX em modo kernel. No Windows real,
//  todas as chamadas vindas de d3d11/d3d12/dxgi.dll (user mode) entram aqui
//  via Dxg*/Nt* syscalls; e o Dxgkrnl chama de volta o KMD (Kernel Mode
//  Driver, ex.: nosso BasicDisplay) pelas DDIs (Display Driver Interface)
//  DxgkDdi* publicadas pelo driver.
//
//  Modelo WDDM 2.x simplificado:
//
//    UMD (d3d11.dll, dxgi.dll)
//       |  syscall
//       v
//    +---------- dxgkrnl (kernel) ----------+      <-- nos estamos aqui
//    |  - adapter pool (placas/PCI display) |
//    |  - device pool (D3D device per app)  |
//    |  - context pool (queues de comandos) |
//    |  - allocation pool (resources GPU)   |
//    |  - paging (delega a dxgmms)          |
//    +--------------+-----------------------+
//                   | DxgkDdi* (DDI)
//                   v
//             KMD: BasicDisplay (display-only) ou GPU completo
//                   |
//                   v
//                gpu_* (LFB, copy_rect, present)
//
//  Para o nosso OS (sem GPU real, so framebuffer LFB Bochs VBE) o backend
//  fisico e sempre o gpu_* do BasicDisplay. DxgkPresentDisplayOnly e o
//  caminho "display-only adapter" do WDDM — exatamente como o BasicDisplay
//  do Windows opera (sem 3D, so blit pra tela).
// ============================================================================

// Forward declarations dos objetos opacos. Quem usa nao precisa saber o layout
// interno — pega o ponteiro de Dxgk*Open/Create* e devolve em Dxgk*Close/Destroy.
typedef struct DXGK_ADAPTER    DXGK_ADAPTER;
typedef struct DXGK_DEVICE     DXGK_DEVICE;
typedef struct DXGK_CONTEXT    DXGK_CONTEXT;
typedef struct DXGK_ALLOCATION DXGK_ALLOCATION;

// ---- Layout publico (campos uteis para o resto do kernel) ----
struct DXGK_ADAPTER {
    int      in_use;          // 1 se o slot esta vivo
    PVOID    kmd_context;     // ponteiro opaco do KMD (BasicDisplay nao usa)
    uint32_t adapter_index;   // ordinal (0 = primario)
    uint32_t width;           // largura atual em pixels
    uint32_t height;          // altura  atual em pixels
    uint32_t bpp;             // bits por pixel (32 no Bochs VBE)
    uint32_t pitch;           // bytes por linha do LFB
};

struct DXGK_DEVICE {
    int           in_use;
    DXGK_ADAPTER* adapter;
    uint32_t      device_index;     // ordinal dentro do adapter
};

struct DXGK_CONTEXT {
    int           in_use;
    DXGK_DEVICE*  device;
    uint32_t      context_index;
    uint64_t      submitted_cmds;   // contador de DxgkSubmitCommand
};

struct DXGK_ALLOCATION {
    int           in_use;
    DXGK_DEVICE*  device;
    uint64_t      size;             // bytes
    PVOID         base;             // endereco virtual (heap por enquanto)
    uint32_t      width;            // dimensoes se for surface (0 = buffer cru)
    uint32_t      height;
    uint32_t      pitch;
    uint32_t      bpp;
};

// ============================================================================
//  APIs exportadas pelo "dxgkrnl.sys" — chamadas pelo resto do kernel ou
//  por DLLs UMD via syscall (ainda nao temos esse caminho; chamada direta hoje).
//  Todas em NTAPI (ms_abi) — mesma ABI que os exports de ntoskrnl.
// ============================================================================

// Inicializa o subsistema DX. Le o estado da GPU atual (gpu_active/width/...)
// e prepara o adapter primario. Idempotente: chamar de novo nao reinicializa.
// Retorna STATUS_SUCCESS sempre; mesmo sem GPU loga "[dxgk] sem display ativo".
NTSTATUS NTAPI DxgkInitialize(void);

// Pega o adapter primario (o que o BasicDisplay esta dirigindo). No NT real
// isso e Dxgk*OpenAdapterFromHdc/FromLuid, mas aqui so temos 1 placa.
NTSTATUS NTAPI DxgkOpenAdapter(DXGK_ADAPTER** out_adapter);

// Cria um device logico por aplicacao. O adapter pode ter varios devices.
NTSTATUS NTAPI DxgkCreateDevice(DXGK_ADAPTER* adapter, DXGK_DEVICE** out_device);

// Cria um context (queue de comandos) dentro de um device.
NTSTATUS NTAPI DxgkCreateContext(DXGK_DEVICE* device, DXGK_CONTEXT** out_ctx);

// Aloca um recurso GPU (texture, buffer, surface). Backed por heap por
// enquanto — sem VRAM real. Devolve ponteiro virtual em allocation->base.
NTSTATUS NTAPI DxgkCreateAllocation(DXGK_DEVICE* device,
                                    uint64_t size,
                                    DXGK_ALLOCATION** out_alloc);

// Apresenta uma allocation na tela: copia src->base para o LFB pela
// gpu_copy_rect ou gpu_pixel (depende do bpp/largura). Equivalente ao
// DxgkPresent do WDDM 2.x (queue-based, no nosso caso sincrono).
NTSTATUS NTAPI DxgkPresent(DXGK_DEVICE* device,
                          DXGK_ALLOCATION* src,
                          int x, int y, int w, int h);

// Submete um command buffer ao context. Stub que loga o tamanho e devolve
// STATUS_SUCCESS — sem GPU command processor, nao tem o que decodificar.
NTSTATUS NTAPI DxgkSubmitCommand(DXGK_CONTEXT* ctx,
                                void* cmd_buf,
                                uint32_t size);

// Caminho "display-only" do WDDM: blit direto de um buffer source pro LFB
// sem alocar DXGK_ALLOCATION (modo emergencia / boot graphics / VPI).
// Usado pelo win32k_compose quando quer mandar o frame inteiro pra tela.
NTSTATUS NTAPI DxgkPresentDisplayOnly(DXGK_ADAPTER* adapter,
                                     void* src,
                                     uint32_t pitch,
                                     int width,
                                     int height);

// Shutdown limpo do subsistema (libera adapters/devices/contexts/allocations).
NTSTATUS NTAPI DxgkShutdown(void);
