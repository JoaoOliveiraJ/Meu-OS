// ============================================================================
//  FASE 7 — Registro em memoria (subset funcional).
//  Arvore: REG_KEY tem 'parent', lista encadeada de 'children' (REG_KEY) e
//  lista encadeada de 'values' (REG_VALUE). Caminho separado por '\\'.
// ============================================================================
#include "cm/registry.h"
#include "ob/object.h"
#include "ex/callbacks.h"
#include "mm/heap.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

#define MAX_NAME 64

typedef struct REG_VALUE {
    char     name[MAX_NAME];
    uint32_t type;
    uint32_t size;
    uint8_t* data;
    struct REG_VALUE* next;
} REG_VALUE;

typedef struct REG_KEY {
    char     name[MAX_NAME];
    struct REG_KEY*   parent;
    struct REG_KEY*   children;
    struct REG_KEY*   sibling;
    REG_VALUE*        values;
} REG_KEY;

static REG_KEY* s_root = 0;

static int strcmpci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return 1;
        a++; b++;
    }
    return (*a == *b) ? 0 : 1;
}
static void strcpyn(char* dst, const char* src, int max) {
    int i = 0;
    if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static REG_KEY* reg_alloc_key(const char* name, REG_KEY* parent) {
    REG_KEY* k = (REG_KEY*)kmalloc(sizeof(REG_KEY));
    if (!k) return 0;
    strcpyn(k->name, name, MAX_NAME);
    k->parent = parent;
    k->children = 0;
    k->sibling = 0;
    k->values = 0;
    return k;
}

static REG_KEY* reg_find_child(REG_KEY* k, const char* name) {
    for (REG_KEY* c = k->children; c; c = c->sibling) if (strcmpci(c->name, name) == 0) return c;
    return 0;
}
static REG_KEY* reg_get_or_create_child(REG_KEY* k, const char* name) {
    REG_KEY* c = reg_find_child(k, name);
    if (c) return c;
    c = reg_alloc_key(name, k);
    if (!c) return 0;
    c->sibling = k->children;
    k->children = c;
    return c;
}

// Resolve um caminho "A\\B\\C" a partir de uma raiz (ou s_root se NULL).
static REG_KEY* reg_resolve_path(REG_KEY* base, const char* path, int create) {
    if (!base) base = s_root;
    if (!base || !path) return 0;
    REG_KEY* k = base;
    const char* p = path;
    while (*p) {
        if (*p == '\\' || *p == '/') { p++; continue; }
        char name[MAX_NAME]; int i = 0;
        while (*p && *p != '\\' && *p != '/' && i < MAX_NAME - 1) name[i++] = *p++;
        name[i] = 0;
        REG_KEY* nxt = create ? reg_get_or_create_child(k, name) : reg_find_child(k, name);
        if (!nxt) return 0;
        k = nxt;
    }
    return k;
}

static REG_VALUE* reg_find_value(REG_KEY* k, const char* name) {
    for (REG_VALUE* v = k->values; v; v = v->next) if (strcmpci(v->name, name) == 0) return v;
    return 0;
}
static REG_VALUE* reg_set_value(REG_KEY* k, const char* name, uint32_t type, const void* data, uint32_t size) {
    REG_VALUE* v = reg_find_value(k, name);
    if (v) {
        if (v->data) kfree(v->data);
    } else {
        v = (REG_VALUE*)kmalloc(sizeof(REG_VALUE));
        if (!v) return 0;
        strcpyn(v->name, name, MAX_NAME);
        v->next = k->values;
        k->values = v;
    }
    v->type = type; v->size = size;
    v->data = size ? (uint8_t*)kmalloc(size) : 0;
    if (v->data && data) for (uint32_t i = 0; i < size; i++) v->data[i] = ((const uint8_t*)data)[i];
    return v;
}

void registry_init(void) {
    if (s_root) return;
    s_root = reg_alloc_key("\\Registry", 0);
    REG_KEY* machine = reg_get_or_create_child(s_root, "Machine");
    REG_KEY* users   = reg_get_or_create_child(s_root, "Users");
    (void)users;
    REG_KEY* sw      = reg_get_or_create_child(machine, "Software");
    REG_KEY* meuos   = reg_get_or_create_child(sw, "MeuOS");
    reg_set_value(meuos, "ProductName",     REG_SZ, "MeuOS", 6);
    reg_set_value(meuos, "CurrentVersion",  REG_SZ, "0.1",   4);
    reg_set_value(meuos, "BuildNumber",     REG_DWORD, (uint32_t[]){26021}, 4);

    // Algumas chaves padrao do NT que drivers consultam.
    REG_KEY* csys = reg_get_or_create_child(reg_get_or_create_child(machine, "System"),
                                             "CurrentControlSet");
    REG_KEY* serv = reg_get_or_create_child(csys, "Services");
    reg_set_value(serv, "Start", REG_DWORD, (uint32_t[]){1}, 4);

    kputs("[reg] registry_init: arvore vazia + \\Registry\\Machine\\Software\\MeuOS criada.\n");
}

// FASE 7.11 (Hipotese A) — pre-cria a sub-arvore \Registry\Machine\System\
// CurrentControlSet\Services\<DriverName> com os valores padrao que um driver
// .sys espera achar (Type, Start, ErrorControl, ImagePath) + subchave Parameters
// vazia. Drivers reais (NT-style, nao packados) que abrem essa chave de dentro
// do DriverEntry encontram o caminho valido e nao bailam com STATUS_NOT_FOUND.
void registry_create_driver_service(const char* drv_basename) {
    if (!s_root || !drv_basename) return;
    REG_KEY* serv = reg_resolve_path(s_root, "Machine\\System\\CurrentControlSet\\Services", 1);
    if (!serv) return;
    // Strip ".sys" se presente.
    char nm[64]; int i = 0;
    while (drv_basename[i] && i < 60) { nm[i] = drv_basename[i]; i++; }
    nm[i] = 0;
    if (i >= 4 && nm[i-4] == '.' && nm[i-3] == 's' && nm[i-2] == 'y' && nm[i-1] == 's') nm[i-4] = 0;
    REG_KEY* svc = reg_get_or_create_child(serv, nm);
    if (!svc) return;
    // Valores padrao do NT para um driver de kernel ja "carregado":
    reg_set_value(svc, "Type",         REG_DWORD, (uint32_t[]){1},        4);   // 1 = SERVICE_KERNEL_DRIVER
    reg_set_value(svc, "Start",        REG_DWORD, (uint32_t[]){0},        4);   // 0 = SERVICE_BOOT_START
    reg_set_value(svc, "ErrorControl", REG_DWORD, (uint32_t[]){1},        4);   // 1 = NORMAL
    char ipath[128]; int p = 0;
    const char* prefix = "\\??\\C:\\Windows\\System32\\drivers\\";
    while (prefix[p] && p < 100) { ipath[p] = prefix[p]; p++; }
    int q = 0; while (drv_basename[q] && p < 120) { ipath[p++] = drv_basename[q++]; }
    ipath[p] = 0;
    reg_set_value(svc, "ImagePath",    REG_SZ, ipath, (uint32_t)(p + 1));
    // Subchave Parameters (drivers leem coisas dela frequentemente):
    REG_KEY* params = reg_get_or_create_child(svc, "Parameters");
    if (params) reg_set_value(params, "Initialized", REG_DWORD, (uint32_t[]){1}, 4);
    kputs("[reg] criada chave Services\\"); kputs(nm); kputs(" (+Parameters)\n");
}

// ===== ASCII helpers =====
int registry_set_value_ascii(const char* keypath, const char* valuename, uint32_t type, const void* data, uint32_t size) {
    REG_KEY* k = reg_resolve_path(s_root, keypath, 1);
    if (!k) return 0;
    return reg_set_value(k, valuename, type, data, size) ? 1 : 0;
}
int registry_get_value_ascii(const char* keypath, const char* valuename, uint32_t* outType, void* outData, uint32_t maxSize, uint32_t* outSize) {
    REG_KEY* k = reg_resolve_path(s_root, keypath, 0);
    if (!k) return 0;
    REG_VALUE* v = reg_find_value(k, valuename);
    if (!v) return 0;
    if (outType) *outType = v->type;
    if (outSize) *outSize = v->size;
    if (outData && maxSize) {
        uint32_t n = v->size < maxSize ? v->size : maxSize;
        for (uint32_t i = 0; i < n; i++) ((uint8_t*)outData)[i] = v->data[i];
    }
    return 1;
}

// Dump simples (uso em demo). Imprime so as ate ~3 niveis primeiros.
static void dump_level(REG_KEY* k, int depth) {
    for (int i = 0; i < depth; i++) kputs("  ");
    kputs("[K] "); kputs(k->name); kputc('\n');
    for (REG_VALUE* v = k->values; v; v = v->next) {
        for (int i = 0; i < depth + 1; i++) kputs("  ");
        kputs("[V] "); kputs(v->name); kputs(" type="); kput_dec(v->type);
        kputs(" size="); kput_dec(v->size); kputc('\n');
    }
    if (depth < 4) for (REG_KEY* c = k->children; c; c = c->sibling) dump_level(c, depth + 1);
}
void registry_dump(void) { if (s_root) dump_level(s_root, 0); }

// ===== UNICODE -> ASCII =====
static void uni_to_ascii(PUNICODE_STRING u, char* out, int max) {
    int n = (u && u->Buffer) ? (u->Length / 2) : 0;
    int i = 0;
    for (; i < n && i < max - 1; i++) out[i] = (char)(u->Buffer[i] & 0xFF);
    out[i] = 0;
}

// Handles do registro: como objetos do OB com tipo proprio (REG_KEY).
#define OB_TYPE_REGKEY 10

NTSTATUS NTAPI NtCreateKey_k(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                             ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition) {
    (void)DesiredAccess; (void)TitleIndex; (void)Class; (void)CreateOptions;
    if (!KeyHandle || !ObjectAttributes || !ObjectAttributes->ObjectName) return STATUS_INVALID_PARAMETER;
    char path[256];
    uni_to_ascii(ObjectAttributes->ObjectName, path, sizeof(path));
    // RootDirectory pode ser uma chave aberta; resolvemos relativo se !=0.
    REG_KEY* base = s_root;
    if (ObjectAttributes->RootDirectory) {
        REG_KEY* r = (REG_KEY*)ob_handle_to_object(ObjectAttributes->RootDirectory, OB_TYPE_REGKEY);
        if (r) base = r;
    }
    int existed = reg_resolve_path(base, path, 0) ? 1 : 0;
    REG_KEY* k = reg_resolve_path(base, path, 1);
    if (!k) return STATUS_UNSUCCESSFUL;
    if (Disposition) *Disposition = existed ? 2u /*REG_OPENED_EXISTING_KEY*/ : 1u /*REG_CREATED_NEW_KEY*/;
    *KeyHandle = ob_create_handle(k);
    kputs("[reg] NtCreateKey '"); kputs(path); kputs(existed ? "' (opened)\n" : "' (created)\n");
    callbacks_fire_registry_op(RegNtPreCreateKey, path);
    return STATUS_SUCCESS;
}
extern volatile int g_pintok_trace;

NTSTATUS NTAPI NtOpenKey_k(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes) {
    (void)DesiredAccess;
    if (!KeyHandle || !ObjectAttributes || !ObjectAttributes->ObjectName) return STATUS_INVALID_PARAMETER;
    char path[256];
    uni_to_ascii(ObjectAttributes->ObjectName, path, sizeof(path));
    if (g_pintok_trace) {
        kputs("  [trace] NtOpenKey('"); kputs(path); kputs("')\n");
    }
    REG_KEY* base = s_root;
    if (ObjectAttributes->RootDirectory) {
        REG_KEY* r = (REG_KEY*)ob_handle_to_object(ObjectAttributes->RootDirectory, OB_TYPE_REGKEY);
        if (r) base = r;
    }
    REG_KEY* k = reg_resolve_path(base, path, 0);
    if (!k) {
        if (g_pintok_trace) kputs("  [trace]   -> STATUS_OBJECT_NAME_NOT_FOUND\n");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *KeyHandle = ob_create_handle(k);
    kputs("[reg] NtOpenKey '"); kputs(path); kputs("' OK\n");
    callbacks_fire_registry_op(RegNtPreOpenKey, path);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtCloseKey_k(HANDLE KeyHandle) { ob_close_handle(KeyHandle); return STATUS_SUCCESS; }
NTSTATUS NTAPI NtDeleteKey_k(HANDLE KeyHandle) {
    REG_KEY* k = (REG_KEY*)ob_handle_to_object(KeyHandle, OB_TYPE_REGKEY);
    if (!k || !k->parent) return STATUS_INVALID_HANDLE;
    REG_KEY* p = k->parent;
    REG_KEY** pp = &p->children;
    while (*pp && *pp != k) pp = &(*pp)->sibling;
    if (*pp) *pp = k->sibling;
    // (sem free recursivo — TODO)
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtEnumerateKey_k(HANDLE KeyHandle, ULONG Index, ULONG KeyInformationClass,
                                PVOID KeyInformation, ULONG Length, PULONG ResultLength) {
    (void)KeyInformationClass;
    REG_KEY* k = (REG_KEY*)ob_handle_to_object(KeyHandle, OB_TYPE_REGKEY);
    if (!k) return STATUS_INVALID_HANDLE;
    REG_KEY* c = k->children;
    for (ULONG i = 0; c && i < Index; i++) c = c->sibling;
    if (!c) return STATUS_NO_MORE_ENTRIES;
    // Devolve o nome em UTF-16 num formato simplificado (compatible with KeyBasicInformation).
    typedef struct { ULONG TitleIndex; ULONG NameLength; WCHAR Name[1]; } KBI_LITE;
    int nlen = 0; while (c->name[nlen]) nlen++;
    ULONG need = (ULONG)(sizeof(KBI_LITE) + (nlen * 2));
    if (ResultLength) *ResultLength = need;
    if (Length < need || !KeyInformation) return STATUS_BUFFER_TOO_SMALL;
    KBI_LITE* o = (KBI_LITE*)KeyInformation;
    o->TitleIndex = 0; o->NameLength = (ULONG)(nlen * 2);
    for (int i = 0; i < nlen; i++) o->Name[i] = (WCHAR)c->name[i];
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtEnumerateValueKey_k(HANDLE KeyHandle, ULONG Index, ULONG KeyValueInfoClass,
                                     PVOID KeyValueInformation, ULONG Length, PULONG ResultLength) {
    (void)KeyValueInfoClass;
    REG_KEY* k = (REG_KEY*)ob_handle_to_object(KeyHandle, OB_TYPE_REGKEY);
    if (!k) return STATUS_INVALID_HANDLE;
    REG_VALUE* v = k->values;
    for (ULONG i = 0; v && i < Index; i++) v = v->next;
    if (!v) return STATUS_NO_MORE_ENTRIES;
    int nlen = 0; while (v->name[nlen]) nlen++;
    typedef struct { ULONG TitleIndex; ULONG Type; ULONG DataOffset; ULONG DataLength; ULONG NameLength; WCHAR Name[1]; } KVFI_LITE;
    ULONG need = (ULONG)(sizeof(KVFI_LITE) + (nlen * 2) + v->size);
    if (ResultLength) *ResultLength = need;
    if (Length < need || !KeyValueInformation) return STATUS_BUFFER_TOO_SMALL;
    KVFI_LITE* o = (KVFI_LITE*)KeyValueInformation;
    o->TitleIndex = 0; o->Type = v->type; o->NameLength = (ULONG)(nlen * 2);
    for (int i = 0; i < nlen; i++) o->Name[i] = (WCHAR)v->name[i];
    o->DataOffset = (ULONG)(sizeof(KVFI_LITE) + (nlen * 2));
    o->DataLength = v->size;
    if (v->size && v->data) {
        uint8_t* d = (uint8_t*)KeyValueInformation + o->DataOffset;
        for (uint32_t i = 0; i < v->size; i++) d[i] = v->data[i];
    }
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtSetValueKey_k(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex,
                               ULONG Type, PVOID Data, ULONG DataSize) {
    (void)TitleIndex;
    REG_KEY* k = (REG_KEY*)ob_handle_to_object(KeyHandle, OB_TYPE_REGKEY);
    if (!k) return STATUS_INVALID_HANDLE;
    char name[MAX_NAME]; uni_to_ascii(ValueName, name, sizeof(name));
    REG_VALUE* v = reg_set_value(k, name, Type, Data, DataSize);
    kputs("[reg] NtSetValueKey '"); kputs(name);
    kputs("' type="); kput_dec(Type); kputs(" size="); kput_dec(DataSize); kputc('\n');
    callbacks_fire_registry_op(RegNtPreSetValueKey, name);
    return v ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
NTSTATUS NTAPI NtQueryValueKey_k(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG KeyValueInfoClass,
                                 PVOID KeyValueInformation, ULONG Length, PULONG ResultLength) {
    REG_KEY* k = (REG_KEY*)ob_handle_to_object(KeyHandle, OB_TYPE_REGKEY);
    if (!k) return STATUS_INVALID_HANDLE;
    char name[MAX_NAME]; uni_to_ascii(ValueName, name, sizeof(name));
    REG_VALUE* v = reg_find_value(k, name);
    if (!v) return STATUS_OBJECT_NAME_NOT_FOUND;
    // KeyValuePartialInformation (class 2) — o mais comum: { TitleIndex, Type, DataLength, Data[] }
    (void)KeyValueInfoClass;
    typedef struct { ULONG TitleIndex; ULONG Type; ULONG DataLength; uint8_t Data[1]; } KVPI_LITE;
    ULONG need = (ULONG)(sizeof(KVPI_LITE) - 1 + v->size);
    if (ResultLength) *ResultLength = need;
    if (Length < need || !KeyValueInformation) return STATUS_BUFFER_TOO_SMALL;
    KVPI_LITE* o = (KVPI_LITE*)KeyValueInformation;
    o->TitleIndex = 0; o->Type = v->type; o->DataLength = v->size;
    if (v->size && v->data) for (uint32_t i = 0; i < v->size; i++) o->Data[i] = v->data[i];
    callbacks_fire_registry_op(RegNtPreQueryValueKey, name);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtDeleteValueKey_k(HANDLE KeyHandle, PUNICODE_STRING ValueName) {
    REG_KEY* k = (REG_KEY*)ob_handle_to_object(KeyHandle, OB_TYPE_REGKEY);
    if (!k) return STATUS_INVALID_HANDLE;
    char name[MAX_NAME]; uni_to_ascii(ValueName, name, sizeof(name));
    REG_VALUE** pp = &k->values;
    while (*pp && strcmpci((*pp)->name, name)) pp = &(*pp)->next;
    if (!*pp) return STATUS_OBJECT_NAME_NOT_FOUND;
    REG_VALUE* gone = *pp;
    *pp = gone->next;
    if (gone->data) kfree(gone->data);
    kfree(gone);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtFlushKey_k(HANDLE KeyHandle) { (void)KeyHandle; return STATUS_SUCCESS; }
