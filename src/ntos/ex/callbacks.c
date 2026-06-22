// ============================================================================
//  FASE 7 — implementacao das tabelas de callback (Ps/Ob/Cm/Ex).
//  Para cada categoria mantemos uma lista pequena (cap 16) com as rotinas
//  registradas pelos drivers. Cada registrar/remover/fire e LOGADO na serial.
// ============================================================================
#include "ex/callbacks.h"
#include "ntddk.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

#define CB_MAX 16

// ---- Process / Thread / Image ----
typedef struct { PCREATE_PROCESS_NOTIFY_ROUTINE    fn; int used; } CB_PS;
typedef struct { PCREATE_PROCESS_NOTIFY_ROUTINE_EX fn; int used; } CB_PSX;
typedef struct { PCREATE_THREAD_NOTIFY_ROUTINE     fn; int used; } CB_TH;
typedef struct { PLOAD_IMAGE_NOTIFY_ROUTINE        fn; int used; } CB_IM;

static CB_PS  s_proc[CB_MAX];
static CB_PSX s_proc_ex[CB_MAX];
static CB_TH  s_thread[CB_MAX];
static CB_IM  s_image[CB_MAX];

// ---- Ob (object handle filtering) ----
typedef struct { POB_CALLBACK_REGISTRATION reg; int used; } CB_OB;
static CB_OB s_ob[CB_MAX];

// ---- Cm (registry) ----
typedef struct { PEX_CALLBACK_FUNCTION fn; PVOID ctx; int used; } CB_CM;
static CB_CM s_cm[CB_MAX];

// "Tipos de objeto" do NT (expostos como ponteiros publicos). No NT real apontam
// p/ OBJECT_TYPE; aqui sao apenas sentinelas — o driver compara com o ponteiro.
static PVOID s_ps_process_type_obj = (PVOID)0x10000001;
static PVOID s_ps_thread_type_obj  = (PVOID)0x10000002;
static PVOID s_io_file_type_obj    = (PVOID)0x10000003;
PVOID* PsProcessType  = &s_ps_process_type_obj;
PVOID* PsThreadType   = &s_ps_thread_type_obj;
PVOID* IoFileObjectType = &s_io_file_type_obj;

// KdDebuggerEnabled = 0 (sem debugger anexado), KdDebuggerNotPresent = 1.
BOOLEAN KdDebuggerEnabled    = 0;
BOOLEAN KdDebuggerNotPresent = 1;

void callbacks_init(void) {
    for (int i = 0; i < CB_MAX; i++) {
        s_proc[i].used = s_proc_ex[i].used = s_thread[i].used = 0;
        s_image[i].used = s_ob[i].used = s_cm[i].used = 0;
    }
    kputs("[cb ] callbacks_init: tabelas Ps/Ob/Cm/Ex zeradas.\n");
}


// ============================================================================
//  Ps* (process / thread / image)
// ============================================================================
NTSTATUS NTAPI PsSetCreateProcessNotifyRoutine_k(PCREATE_PROCESS_NOTIFY_ROUTINE n, BOOLEAN Remove) {
    if (!n) return STATUS_INVALID_PARAMETER;
    if (Remove) {
        for (int i = 0; i < CB_MAX; i++) if (s_proc[i].used && s_proc[i].fn == n) {
            s_proc[i].used = 0;
            kputs("[cb ] PsSetCreateProcessNotifyRoutine REMOVE @"); kput_hex((uint64_t)(uintptr_t)n); kputc('\n');
            return STATUS_SUCCESS;
        }
        return STATUS_NOT_IMPLEMENTED;
    }
    for (int i = 0; i < CB_MAX; i++) if (!s_proc[i].used) {
        s_proc[i].fn = n; s_proc[i].used = 1;
        kputs("[cb ] PsSetCreateProcessNotifyRoutine ADD @"); kput_hex((uint64_t)(uintptr_t)n); kputc('\n');
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}

NTSTATUS NTAPI PsSetCreateProcessNotifyRoutineEx_k(PCREATE_PROCESS_NOTIFY_ROUTINE_EX n, BOOLEAN Remove) {
    if (!n) return STATUS_INVALID_PARAMETER;
    if (Remove) {
        for (int i = 0; i < CB_MAX; i++) if (s_proc_ex[i].used && s_proc_ex[i].fn == n) {
            s_proc_ex[i].used = 0;
            kputs("[cb ] PsSetCreateProcessNotifyRoutineEx REMOVE @"); kput_hex((uint64_t)(uintptr_t)n); kputc('\n');
            return STATUS_SUCCESS;
        }
        return STATUS_NOT_IMPLEMENTED;
    }
    for (int i = 0; i < CB_MAX; i++) if (!s_proc_ex[i].used) {
        s_proc_ex[i].fn = n; s_proc_ex[i].used = 1;
        kputs("[cb ] PsSetCreateProcessNotifyRoutineEx ADD @"); kput_hex((uint64_t)(uintptr_t)n); kputc('\n');
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}

NTSTATUS NTAPI PsSetCreateThreadNotifyRoutine_k(PCREATE_THREAD_NOTIFY_ROUTINE n) {
    if (!n) return STATUS_INVALID_PARAMETER;
    for (int i = 0; i < CB_MAX; i++) if (!s_thread[i].used) {
        s_thread[i].fn = n; s_thread[i].used = 1;
        kputs("[cb ] PsSetCreateThreadNotifyRoutine ADD @"); kput_hex((uint64_t)(uintptr_t)n); kputc('\n');
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}
NTSTATUS NTAPI PsRemoveCreateThreadNotifyRoutine_k(PCREATE_THREAD_NOTIFY_ROUTINE n) {
    for (int i = 0; i < CB_MAX; i++) if (s_thread[i].used && s_thread[i].fn == n) {
        s_thread[i].used = 0; return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS NTAPI PsSetLoadImageNotifyRoutine_k(PLOAD_IMAGE_NOTIFY_ROUTINE n) {
    if (!n) return STATUS_INVALID_PARAMETER;
    for (int i = 0; i < CB_MAX; i++) if (!s_image[i].used) {
        s_image[i].fn = n; s_image[i].used = 1;
        kputs("[cb ] PsSetLoadImageNotifyRoutine ADD @"); kput_hex((uint64_t)(uintptr_t)n); kputc('\n');
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}
NTSTATUS NTAPI PsRemoveLoadImageNotifyRoutine_k(PLOAD_IMAGE_NOTIFY_ROUTINE n) {
    for (int i = 0; i < CB_MAX; i++) if (s_image[i].used && s_image[i].fn == n) {
        s_image[i].used = 0; return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

// ============================================================================
//  Ob* (object handle filtering)
// ============================================================================
NTSTATUS NTAPI ObRegisterCallbacks_k(POB_CALLBACK_REGISTRATION reg, PVOID* RegistrationHandle) {
    if (!reg || !RegistrationHandle) return STATUS_INVALID_PARAMETER;
    for (int i = 0; i < CB_MAX; i++) if (!s_ob[i].used) {
        s_ob[i].reg = reg; s_ob[i].used = 1;
        *RegistrationHandle = (PVOID)(uintptr_t)(0xCB000000ULL | (uint64_t)i);
        kputs("[cb ] ObRegisterCallbacks ADD slot="); kput_dec(i); kputc('\n');
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}
void NTAPI ObUnRegisterCallbacks_k(PVOID RegistrationHandle) {
    if (!RegistrationHandle) return;
    uint64_t v = (uint64_t)(uintptr_t)RegistrationHandle;
    if ((v >> 24) != 0xCB) return;
    int i = (int)(v & 0xFFFFFFu);
    if (i >= 0 && i < CB_MAX) {
        s_ob[i].used = 0;
        kputs("[cb ] ObUnRegisterCallbacks slot="); kput_dec((uint64_t)i); kputc('\n');
    }
}

// ============================================================================
//  Cm* (registry) + Ex* (generic)
// ============================================================================
NTSTATUS NTAPI CmRegisterCallback_k(PEX_CALLBACK_FUNCTION cb, PVOID Context, PLARGE_INTEGER Cookie) {
    if (!cb || !Cookie) return STATUS_INVALID_PARAMETER;
    for (int i = 0; i < CB_MAX; i++) if (!s_cm[i].used) {
        s_cm[i].fn = cb; s_cm[i].ctx = Context; s_cm[i].used = 1;
        Cookie->QuadPart = (LONGLONG)(0xCAF1000ULL | (uint64_t)i);
        kputs("[cb ] CmRegisterCallback ADD slot="); kput_dec(i); kputc('\n');
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}
NTSTATUS NTAPI CmRegisterCallbackEx_k(PEX_CALLBACK_FUNCTION cb, PUNICODE_STRING Altitude,
                                      PVOID Driver, PVOID Context, PLARGE_INTEGER Cookie, PVOID Reserved) {
    (void)Altitude; (void)Driver; (void)Reserved;
    return CmRegisterCallback_k(cb, Context, Cookie);
}
NTSTATUS NTAPI CmUnRegisterCallback_k(LARGE_INTEGER Cookie) {
    uint64_t v = (uint64_t)Cookie.QuadPart;
    if ((v >> 16) != 0xCAF1) return STATUS_INVALID_PARAMETER;
    int i = (int)(v & 0xFFFFu);
    if (i >= 0 && i < CB_MAX && s_cm[i].used) {
        s_cm[i].used = 0;
        kputs("[cb ] CmUnRegisterCallback slot="); kput_dec((uint64_t)i); kputc('\n');
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}
PVOID NTAPI ExRegisterCallback_k(PVOID CallbackObject, PEX_CALLBACK_FUNCTION cb, PVOID Context) {
    (void)CallbackObject;
    for (int i = 0; i < CB_MAX; i++) if (!s_cm[i].used) {   // reusa s_cm como pool generica
        s_cm[i].fn = cb; s_cm[i].ctx = Context; s_cm[i].used = 1;
        kputs("[cb ] ExRegisterCallback ADD slot="); kput_dec(i); kputc('\n');
        return (PVOID)(uintptr_t)(0xE0000000ULL | (uint64_t)i);
    }
    return 0;
}
void NTAPI ExUnregisterCallback_k(PVOID Registration) {
    uint64_t v = (uint64_t)(uintptr_t)Registration;
    if ((v >> 28) != 0xE) return;
    int i = (int)(v & 0xFFFFFFu);
    if (i >= 0 && i < CB_MAX) s_cm[i].used = 0;
}

// ============================================================================
//  Disparadores (kernel-side). Chamados pelo loader/process manager quando o
//  evento real acontece. Cada disparo loga e percorre as listas.
// ============================================================================
void callbacks_fire_process_create(uint32_t parent_pid, uint32_t pid, int create, const char* image_name) {
    kputs("[cb ] FIRE process_create pid="); kput_dec(pid);
    kputs(create ? " (CREATE)" : " (EXIT)");
    if (image_name) { kputs(" img='"); kputs(image_name); kputc('\''); }
    kputc('\n');
    for (int i = 0; i < CB_MAX; i++) if (s_proc[i].used && s_proc[i].fn) {
        s_proc[i].fn((HANDLE)(uintptr_t)parent_pid, (HANDLE)(uintptr_t)pid, (BOOLEAN)create);
    }
    // Ex (PROCESS_NOTIFY_INFO simplificado).
    if (create) {
        PS_CREATE_NOTIFY_INFO info = { 0 };
        info.Size = sizeof(info);
        info.ParentProcessId = (HANDLE)(uintptr_t)parent_pid;
        info.CreationStatus  = STATUS_SUCCESS;
        for (int i = 0; i < CB_MAX; i++) if (s_proc_ex[i].used && s_proc_ex[i].fn) {
            s_proc_ex[i].fn(0, (HANDLE)(uintptr_t)pid, &info);
        }
    } else {
        for (int i = 0; i < CB_MAX; i++) if (s_proc_ex[i].used && s_proc_ex[i].fn) {
            s_proc_ex[i].fn(0, (HANDLE)(uintptr_t)pid, 0);
        }
    }
}
void callbacks_fire_thread_create(uint32_t pid, uint32_t tid, int create) {
    kputs("[cb ] FIRE thread_create pid="); kput_dec(pid); kputs(" tid="); kput_dec(tid);
    kputs(create ? " (CREATE)\n" : " (EXIT)\n");
    for (int i = 0; i < CB_MAX; i++) if (s_thread[i].used && s_thread[i].fn) {
        s_thread[i].fn((HANDLE)(uintptr_t)pid, (HANDLE)(uintptr_t)tid, (BOOLEAN)create);
    }
}
void callbacks_fire_image_load(const char* image_name, uint32_t pid, void* base, uint32_t size) {
    kputs("[cb ] FIRE image_load pid="); kput_dec(pid); kputs(" img='");
    if (image_name) kputs(image_name); kputs("' base="); kput_hex((uint64_t)(uintptr_t)base);
    kputs(" size="); kput_dec((uint64_t)size); kputc('\n');
    // ImageInfo simplificado (so o ImageBase). Drivers nao deveriam derreferenciar
    // alem de ImageInfo->ImageBase / ImageSelector / Size — nos so passamos NULL e
    // contamos com a maioria dos drivers tolerarem (defensive). TODO completar.
    for (int i = 0; i < CB_MAX; i++) if (s_image[i].used && s_image[i].fn) {
        s_image[i].fn(0, (HANDLE)(uintptr_t)pid, 0);
    }
}
void callbacks_fire_registry_op(uint32_t op, const char* key_name) {
    kputs("[cb ] FIRE registry op="); kput_dec(op);
    if (key_name) { kputs(" key='"); kputs(key_name); kputc('\''); }
    kputc('\n');
    for (int i = 0; i < CB_MAX; i++) if (s_cm[i].used && s_cm[i].fn) {
        s_cm[i].fn(s_cm[i].ctx, (PVOID)(uintptr_t)op, 0);   // Arg1 = NotifyClass simplificado
    }
}

// ---- Telemetria ----
int callbacks_count_process(void){ int n=0; for(int i=0;i<CB_MAX;i++) if(s_proc[i].used) n++; for(int i=0;i<CB_MAX;i++) if(s_proc_ex[i].used) n++; return n; }
int callbacks_count_thread(void) { int n=0; for(int i=0;i<CB_MAX;i++) if(s_thread[i].used) n++; return n; }
int callbacks_count_image(void)  { int n=0; for(int i=0;i<CB_MAX;i++) if(s_image[i].used) n++; return n; }
int callbacks_count_ob(void)     { int n=0; for(int i=0;i<CB_MAX;i++) if(s_ob[i].used) n++; return n; }
int callbacks_count_cm(void)     { int n=0; for(int i=0;i<CB_MAX;i++) if(s_cm[i].used) n++; return n; }
