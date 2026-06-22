// ============================================================================
//  ndis.c — NDIS framework do MeuOS (Fase 12 stub).
//
//  Mantem tabelas estaticas de miniports e protocolos. As APIs NdisMRegister*,
//  NdisAllocate*, NdisM*Complete vivem em ntoskrnl.c (com __attribute__((ms_abi)))
//  e delegam para as funcoes internas registradas aqui. Logs em '[ndis] ...'.
//
//  Sem TX/RX real (a fila de pacotes nunca e drenada). Em apps reais, recv()
//  simplesmente devolve 0 bytes — comportamento esperado de uma rede vazia.
// ============================================================================
#include <stdint.h>
#include "ndis.h"

// kputs e amigos vivem em src/ntos/init/main.c.
extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado interno -------------------------------------------------------
#define NDIS_MAX_MINIPORTS 8
#define NDIS_MAX_PROTOCOLS 4

typedef struct ndis_miniport_entry {
    int         used;
    const char* name;
    void*       driver_handle;
} ndis_miniport_entry_t;

typedef struct ndis_protocol_entry {
    int         used;
    const char* name;
} ndis_protocol_entry_t;

static int                    s_initialized = 0;
static int                    s_mp_count    = 0;
static int                    s_proto_count = 0;
static ndis_miniport_entry_t  s_miniports[NDIS_MAX_MINIPORTS];
static ndis_protocol_entry_t  s_protocols[NDIS_MAX_PROTOCOLS];

// Contadores informativos: cada chamada das APIs ms_abi (registradas em
// ntoskrnl.c) incrementa um destes. Util para confirmar em log que o e1000
// chamou NdisMRegisterMiniportDriver, etc.
static volatile uint64_t s_call_init_wrapper      = 0;
static volatile uint64_t s_call_register_miniport = 0;
static volatile uint64_t s_call_register_protocol = 0;
static volatile uint64_t s_call_alloc_nbl         = 0;
static volatile uint64_t s_call_free_nbl          = 0;
static volatile uint64_t s_call_send_complete     = 0;

int ndis_init(void) {
    if (s_initialized) return 1;
    for (int i = 0; i < NDIS_MAX_MINIPORTS; i++) {
        s_miniports[i].used = 0;
        s_miniports[i].name = 0;
        s_miniports[i].driver_handle = 0;
    }
    for (int i = 0; i < NDIS_MAX_PROTOCOLS; i++) {
        s_protocols[i].used = 0;
        s_protocols[i].name = 0;
    }
    s_mp_count    = 0;
    s_proto_count = 0;
    s_initialized = 1;
    kputs("[ndis] init: framework carregado (slots miniport=");
    kput_dec(NDIS_MAX_MINIPORTS);
    kputs(" protocolo=");
    kput_dec(NDIS_MAX_PROTOCOLS);
    kputs(")\n");
    return 1;
}

int ndis_miniport_count(void) { return s_mp_count;    }
int ndis_protocol_count(void) { return s_proto_count; }

int ndis_register_miniport(const char* name, void* driver_handle) {
    if (!s_initialized) ndis_init();
    if (s_mp_count >= NDIS_MAX_MINIPORTS) {
        kputs("[ndis] register_miniport: SEM SLOT (max="); kput_dec(NDIS_MAX_MINIPORTS); kputs(")\n");
        return 0;
    }
    s_miniports[s_mp_count].used          = 1;
    s_miniports[s_mp_count].name          = name ? name : "(anon)";
    s_miniports[s_mp_count].driver_handle = driver_handle;
    s_mp_count++;
    kputs("[ndis] miniport registrado: '"); kputs(name ? name : "(anon)"); kputs("' total=");
    kput_dec(s_mp_count); kputc('\n');
    return 1;
}

int ndis_register_protocol(const char* name) {
    if (!s_initialized) ndis_init();
    if (s_proto_count >= NDIS_MAX_PROTOCOLS) {
        kputs("[ndis] register_protocol: SEM SLOT (max="); kput_dec(NDIS_MAX_PROTOCOLS); kputs(")\n");
        return 0;
    }
    s_protocols[s_proto_count].used = 1;
    s_protocols[s_proto_count].name = name ? name : "(anon)";
    s_proto_count++;
    kputs("[ndis] protocolo registrado: '"); kputs(name ? name : "(anon)"); kputs("' total=");
    kput_dec(s_proto_count); kputc('\n');
    return 1;
}

// --- API ms_abi para os drivers .sys --------------------------------------
//
//  Estas sao registradas em ntoskrnl.c (g_ntexports[]). Os drivers chamam
//  NdisMRegisterMiniportDriver(...) pelo nome — pe_bind_imports resolve para
//  estes simbolos. Como o NDIS real tem assinaturas gigantes, aceitamos so o
//  necessario e retornamos um handle fake (= 1) que o caller pode passar de
//  volta em chamadas subsequentes.

__attribute__((ms_abi))
void ndis_NdisInitializeWrapper(void** wrapper, void* drvobj, void* regpath,
                                void* reserved) {
    (void)drvobj; (void)regpath; (void)reserved;
    s_call_init_wrapper++;
    if (wrapper) *wrapper = (void*)0xDEADBEEFEAFC0DE0ULL;  // handle fake
    kputs("[ndis] NdisInitializeWrapper chamado (call #");
    kput_dec(s_call_init_wrapper); kputs(")\n");
}

__attribute__((ms_abi))
NDIS_STATUS ndis_NdisMRegisterMiniportDriver(void* drvobj, void* regpath,
                                             void* context, void* chars,
                                             void** handle) {
    (void)drvobj; (void)regpath; (void)context; (void)chars;
    s_call_register_miniport++;
    if (handle) *handle = (void*)((uintptr_t)0x1D150000ULL + (uintptr_t)s_call_register_miniport);
    ndis_register_miniport("Miniport-via-API", handle ? *handle : 0);
    return NDIS_STATUS_SUCCESS;
}

__attribute__((ms_abi))
NDIS_STATUS ndis_NdisRegisterProtocolDriver(void* context, void* chars,
                                            void** handle) {
    (void)context; (void)chars;
    s_call_register_protocol++;
    if (handle) *handle = (void*)((uintptr_t)0x1A0C0000ULL + (uintptr_t)s_call_register_protocol);
    ndis_register_protocol("Protocol-via-API");
    return NDIS_STATUS_SUCCESS;
}

__attribute__((ms_abi))
PNET_BUFFER_LIST ndis_NdisAllocateNetBufferList(void* pool, uint16_t context_size,
                                                uint16_t backfill) {
    (void)pool; (void)context_size; (void)backfill;
    s_call_alloc_nbl++;
    // Devolve um NBL fake (ponteiro nao-nulo). Sem heap real (drivers de NDIS
    // raros chamam Free), retornamos um padding statico.
    static char fake_nbl_pool[8][256];
    if (s_call_alloc_nbl <= 8) {
        return (PNET_BUFFER_LIST)&fake_nbl_pool[s_call_alloc_nbl - 1][0];
    }
    return (PNET_BUFFER_LIST)&fake_nbl_pool[0][0];
}

__attribute__((ms_abi))
void ndis_NdisFreeNetBufferList(PNET_BUFFER_LIST nbl) {
    (void)nbl;
    s_call_free_nbl++;
}

__attribute__((ms_abi))
void ndis_NdisMSendNetBufferListsComplete(void* mp_handle, PNET_BUFFER_LIST nbl,
                                          uint32_t send_flags) {
    (void)mp_handle; (void)nbl; (void)send_flags;
    s_call_send_complete++;
}

__attribute__((ms_abi))
void ndis_NdisFreeMemory(void* mem, uint32_t length, uint32_t flags) {
    (void)mem; (void)length; (void)flags;
    // No-op: usamos pools estaticos.
}

__attribute__((ms_abi))
NDIS_STATUS ndis_NdisAllocateMemoryWithTag(void** out, uint32_t length, uint32_t tag) {
    (void)length; (void)tag;
    if (out) *out = (void*)0;   // sem alocacao real: devolve NULL, driver tipico
                                // checa e cai em fallback (ou falha graciosa).
    return NDIS_STATUS_RESOURCES;
}

__attribute__((ms_abi))
void ndis_NdisMResetComplete(void* handle, NDIS_STATUS status, int reset) {
    (void)handle; (void)status; (void)reset;
}

__attribute__((ms_abi))
uint32_t ndis_NdisGetVersion(void) {
    return (NDIS_MINIPORT_MAJOR_VERSION << 16) | NDIS_MINIPORT_MINOR_VERSION;
}
