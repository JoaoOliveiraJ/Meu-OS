#include <stdint.h>
#include "ob/object.h"
#include "mm/heap.h"

// Cabecalho de objeto (precede o corpo, como no NT). Padded p/ 16 bytes.
typedef struct _OBJECT_HEADER {
    uint32_t Type;
    int32_t  RefCount;
    int32_t  HandleCount;
    uint32_t Reserved;
    char     Name[64];
} OBJECT_HEADER;

#define BODY(h)   ((void*)((OBJECT_HEADER*)(h) + 1))
#define HEADER(b) ((OBJECT_HEADER*)(b) - 1)

#define MAX_NAMED   64
#define MAX_HANDLES 256
#define MAX_OBJECTS 256

static void* s_named[MAX_NAMED];
static int   s_nnamed;
static void* s_handles[MAX_HANDLES];   // index -> corpo; 0 = livre
static void* s_all[MAX_OBJECTS];       // TODOS os objetos criados (p/ enumeracao)
static int   s_nall;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  ieq(const char* a, const char* b) {
    while (*a && *b) { if (lower(*a) != lower(*b)) return 0; a++; b++; }
    return *a == *b;
}

void ob_init(void) {
    s_nnamed = 0;
    s_nall   = 0;
    for (int i = 0; i < MAX_HANDLES; i++) s_handles[i] = 0;
}

void* ObCreateObject(uint32_t type, size_t body_size, const char* name) {
    OBJECT_HEADER* h = (OBJECT_HEADER*)kmalloc(sizeof(OBJECT_HEADER) + body_size);
    if (!h) return 0;
    h->Type = type; h->RefCount = 1; h->HandleCount = 0; h->Reserved = 0;
    int i = 0;
    if (name) while (name[i] && i < 63) { h->Name[i] = name[i]; i++; }
    h->Name[i] = 0;

    void* body = BODY(h);
    for (size_t k = 0; k < body_size; k++) ((uint8_t*)body)[k] = 0;

    if (name && name[0] && s_nnamed < MAX_NAMED) s_named[s_nnamed++] = body;  // namespace
    if (s_nall < MAX_OBJECTS) s_all[s_nall++] = body;   // lista global p/ enumeracao
    return body;
}

void ObReferenceObject(void* body)   { if (body) HEADER(body)->RefCount++; }
void ObDereferenceObject(void* body) { if (body) HEADER(body)->RefCount--; }
uint32_t ob_type_of(void* body)      { return body ? HEADER(body)->Type : 0; }

void* ObLookupObject(const char* name) {
    for (int i = 0; i < s_nnamed; i++)
        if (ieq(HEADER(s_named[i])->Name, name)) return s_named[i];
    return 0;
}

HANDLE ob_create_handle(void* body) {
    for (int i = 1; i < MAX_HANDLES; i++) {       // handle 0 = invalido
        if (!s_handles[i]) {
            s_handles[i] = body;
            HEADER(body)->HandleCount++;
            ObReferenceObject(body);
            return (HANDLE)(uintptr_t)(i * 4);    // handles sao multiplos de 4 (como no Win)
        }
    }
    return 0;
}

void* ob_handle_to_object(HANDLE h, uint32_t type) {
    uintptr_t v = (uintptr_t)h;
    int i = (int)(v / 4);
    if (v == 0 || i <= 0 || i >= MAX_HANDLES) return 0;
    void* body = s_handles[i];
    if (!body) return 0;
    if (type != OB_TYPE_NONE && HEADER(body)->Type != type) return 0;
    return body;
}

void ob_close_handle(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    int i = (int)(v / 4);
    if (v == 0 || i <= 0 || i >= MAX_HANDLES) return;
    void* body = s_handles[i];
    if (!body) return;
    s_handles[i] = 0;
    HEADER(body)->HandleCount--;
    ObDereferenceObject(body);
}

// Enumeracao por tipo: caminha a lista global e devolve o n-esimo objeto vivo do
// tipo pedido. Permite listar EPROCESS (tasklist) e DRIVER_OBJECT (sc query) mesmo
// que sejam objetos SEM nome (que nao entram no namespace s_named[]).
void* ob_enum_by_type(uint32_t type, int index) {
    if (index < 0) return 0;
    int seen = 0;
    for (int i = 0; i < s_nall; i++) {
        void* body = s_all[i];
        if (!body) continue;
        if (HEADER(body)->Type != type) continue;
        if (seen == index) return body;
        seen++;
    }
    return 0;
}

int ob_count_by_type(uint32_t type) {
    int n = 0;
    for (int i = 0; i < s_nall; i++) {
        void* body = s_all[i];
        if (body && HEADER(body)->Type == type) n++;
    }
    return n;
}
