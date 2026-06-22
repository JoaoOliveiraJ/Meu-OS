// ============================================================================
//  fltmgr.c — Filter Manager do MeuOS (FASE 13 stub).
//
//  Tabelas estaticas de filtros registrados + ports de comunicacao. Sem
//  interceptacao de IRP_MJ_* real (o I/O Manager continua despachando direto
//  para o filesystem driver, sem chamar callbacks de minifilter). As APIs
//  ms_abi devolvem handles fake (>0) que o caller pode passar de volta.
//
//  Logs em '[fltmgr] ...'.
// ============================================================================
#include <stdint.h>
#include "fltmgr.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// --- estado interno -------------------------------------------------------
#define FLTMGR_MAX_FILTERS 8
#define FLTMGR_MAX_PORTS   16

typedef struct fltmgr_filter_entry {
    int   used;
    int   started;
    void* driver_object;
} fltmgr_filter_entry_t;

typedef struct fltmgr_port_entry {
    int   used;
    int   max_connections;
    void* filter;
} fltmgr_port_entry_t;

static int                    s_initialized = 0;
static int                    s_filter_count = 0;
static int                    s_port_count   = 0;
static volatile uint64_t      s_msg_count    = 0;
static volatile uint64_t      s_dirty_count  = 0;
static fltmgr_filter_entry_t  s_filters[FLTMGR_MAX_FILTERS];
static fltmgr_port_entry_t    s_ports[FLTMGR_MAX_PORTS];

int fltmgr_init(void) {
    if (s_initialized) return 1;
    for (int i = 0; i < FLTMGR_MAX_FILTERS; i++) {
        s_filters[i].used = 0;
        s_filters[i].started = 0;
        s_filters[i].driver_object = 0;
    }
    for (int i = 0; i < FLTMGR_MAX_PORTS; i++) {
        s_ports[i].used = 0;
        s_ports[i].max_connections = 0;
        s_ports[i].filter = 0;
    }
    s_filter_count = 0;
    s_port_count   = 0;
    s_msg_count    = 0;
    s_dirty_count  = 0;
    s_initialized  = 1;
    kputs("[fltmgr] Filter Manager init (slots filtro=");
    kput_dec(FLTMGR_MAX_FILTERS); kputs(" porta=");
    kput_dec(FLTMGR_MAX_PORTS); kputs(")\n");
    return 1;
}

int      fltmgr_filter_count(void)  { return s_filter_count; }
int      fltmgr_port_count(void)    { return s_port_count;   }
uint64_t fltmgr_message_count(void) { return s_msg_count;    }

__attribute__((ms_abi))
NTSTATUS fltmgr_FltRegisterFilter(void* driver_object, void* registration,
                                  PFLT_FILTER* out_filter) {
    (void)registration;
    if (!s_initialized) fltmgr_init();
    if (s_filter_count >= FLTMGR_MAX_FILTERS) {
        kputs("[fltmgr] FltRegisterFilter: SEM SLOT (max=");
        kput_dec(FLTMGR_MAX_FILTERS); kputs(")\n");
        if (out_filter) *out_filter = 0;
        return STATUS_NO_MEMORY;
    }
    int idx = s_filter_count;
    s_filters[idx].used          = 1;
    s_filters[idx].started       = 0;
    s_filters[idx].driver_object = driver_object;
    s_filter_count++;
    // handle fake = ponteiro nao-zero p/ a entrada interna.
    PFLT_FILTER fake = (PFLT_FILTER)&s_filters[idx];
    if (out_filter) *out_filter = fake;
    kputs("[fltmgr] FltRegisterFilter: filter #");
    kput_dec((uint64_t)s_filter_count); kputs(" registrado, handle=");
    kput_hex((uint64_t)(uintptr_t)fake); kputc('\n');
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
NTSTATUS fltmgr_FltStartFiltering(PFLT_FILTER filter) {
    if (!filter) return STATUS_INVALID_PARAMETER;
    fltmgr_filter_entry_t* e = (fltmgr_filter_entry_t*)filter;
    e->started = 1;
    kputs("[fltmgr] FltStartFiltering chamado para handle=");
    kput_hex((uint64_t)(uintptr_t)filter); kputc('\n');
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
void fltmgr_FltUnregisterFilter(PFLT_FILTER filter) {
    if (!filter) return;
    fltmgr_filter_entry_t* e = (fltmgr_filter_entry_t*)filter;
    e->started = 0;
    e->used = 0;
    kputs("[fltmgr] FltUnregisterFilter chamado para handle=");
    kput_hex((uint64_t)(uintptr_t)filter); kputc('\n');
}

__attribute__((ms_abi))
NTSTATUS fltmgr_FltCreateCommunicationPort(PFLT_FILTER filter, PFLT_PORT* server_port,
                                           void* object_attributes, void* server_port_cookie,
                                           void* connect_notify, void* disconnect_notify,
                                           void* message_notify, int max_connections) {
    (void)object_attributes; (void)server_port_cookie;
    (void)connect_notify; (void)disconnect_notify; (void)message_notify;
    if (!s_initialized) fltmgr_init();
    if (s_port_count >= FLTMGR_MAX_PORTS) {
        kputs("[fltmgr] FltCreateCommunicationPort: SEM SLOT (max=");
        kput_dec(FLTMGR_MAX_PORTS); kputs(")\n");
        if (server_port) *server_port = 0;
        return STATUS_NO_MEMORY;
    }
    int idx = s_port_count;
    s_ports[idx].used = 1;
    s_ports[idx].max_connections = max_connections;
    s_ports[idx].filter = filter;
    s_port_count++;
    PFLT_PORT fake = (PFLT_PORT)&s_ports[idx];
    if (server_port) *server_port = fake;
    kputs("[fltmgr] FltCreateCommunicationPort: port #");
    kput_dec((uint64_t)s_port_count); kputs(" criado, max_conn=");
    kput_dec((uint64_t)max_connections); kputc('\n');
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
NTSTATUS fltmgr_FltSendMessage(PFLT_FILTER filter, PFLT_PORT* client_port,
                               void* sender_buffer, uint32_t sender_buffer_length,
                               void* reply_buffer, uint32_t* reply_length,
                               void* timeout) {
    (void)filter; (void)client_port; (void)sender_buffer;
    (void)sender_buffer_length; (void)reply_buffer; (void)timeout;
    s_msg_count++;
    // Sem servico user-mode ouvindo, devolve 0 bytes de reply.
    if (reply_length) *reply_length = 0;
    return STATUS_SUCCESS;
}

__attribute__((ms_abi))
void fltmgr_FltSetCallbackDataDirty(PFLT_CALLBACK_DATA data) {
    (void)data;
    s_dirty_count++;
}
