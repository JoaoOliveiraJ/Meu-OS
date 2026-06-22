#include <stdint.h>
#include "lpc/pipe.h"
#include "ob/object.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_dec(uint64_t v);

// ---- utilitarios de string (sem libc no kernel) ----
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int starts_with_ci(const char* s, const char* prefix) {
    while (*prefix) { if (lower(*s) != lower(*prefix)) return 0; s++; prefix++; }
    return 1;
}

// Normaliza qualquer forma de nome de pipe para "\Pipe\Nome":
//   "\\.\pipe\Foo"  -> "\Pipe\Foo"   (forma Win32 do CreateFile)
//   "\Device\NamedPipe\Foo" -> "\Pipe\Foo"
//   "\Pipe\Foo"     -> "\Pipe\Foo"   (ja normalizado)
//   "Foo"           -> "\Pipe\Foo"   (so o nome)
void pipe_normalize_name(const char* in, char* out, int outcap) {
    const char* tail = in;
    if (!in) { out[0] = 0; return; }

    if (starts_with_ci(in, "\\\\.\\pipe\\"))            tail = in + 9;   // forma \\.\pipe\Nome
    else if (starts_with_ci(in, "\\Device\\NamedPipe\\")) tail = in + 18; // forma NT do namespace
    else if (starts_with_ci(in, "\\Pipe\\"))            tail = in + 6;   // ja prefixado
    else                                                tail = in;       // so o nome

    const char* pfx = "\\Pipe\\";
    int o = 0;
    for (int i = 0; pfx[i] && o < outcap - 1; i++) out[o++] = pfx[i];
    while (*tail && o < outcap - 1) out[o++] = *tail++;
    out[o] = 0;
}

PPIPE_OBJECT pipe_create(const char* name) {
    char full[64];
    pipe_normalize_name(name, full, sizeof(full));

    if (ObLookupObject(full)) {   // ja existe um pipe com esse nome
        kputs("[pipe] pipe_create: ja existe "); kputs(full); kputc('\n');
        return 0;
    }

    PPIPE_OBJECT p = (PPIPE_OBJECT)ObCreateObject(OB_TYPE_PIPE, sizeof(PIPE_OBJECT), full);
    if (!p) return 0;
    // ObCreateObject zera o corpo; preenchemos o nome e o estado.
    int i = 0; while (full[i] && i < 63) { p->Name[i] = full[i]; i++; } p->Name[i] = 0;
    p->State = PIPE_STATE_DISCONNECTED;
    p->Head = p->Tail = p->Count = 0;

    kputs("[pipe] pipe_create: '"); kputs(full);
    kputs("' criado (OB_TYPE_PIPE, buffer="); kput_dec(PIPE_BUF_SIZE);
    kputs(" bytes)\n");
    return p;
}

PPIPE_OBJECT pipe_open(const char* name) {
    char full[64];
    pipe_normalize_name(name, full, sizeof(full));

    void* body = ObLookupObject(full);
    if (!body || ob_type_of(body) != OB_TYPE_PIPE) {
        kputs("[pipe] pipe_open: nao existe "); kputs(full); kputc('\n');
        return 0;
    }
    PPIPE_OBJECT p = (PPIPE_OBJECT)body;
    p->ClientConnected = 1;
    p->State = PIPE_STATE_CONNECTED;
    kputs("[pipe] pipe_open: cliente conectou em '"); kputs(full);
    kputs("' (estado=CONNECTED)\n");
    return p;
}

NTSTATUS pipe_connect(PPIPE_OBJECT p) {
    if (!p) return STATUS_UNSUCCESSFUL;
    p->ServerConnected = 1;
    // Sem escalonador: nao bloqueamos. Se o cliente ja abriu, ficamos CONNECTED;
    // senao LISTENING (a demo abre o cliente logo em seguida).
    if (p->ClientConnected) p->State = PIPE_STATE_CONNECTED;
    else                    p->State = PIPE_STATE_LISTENING;
    kputs("[pipe] pipe_connect: servidor de '"); kputs(p->Name);
    kputs(p->ClientConnected ? "' (cliente ja presente -> CONNECTED)\n"
                             : "' aguardando cliente (LISTENING)\n");
    return STATUS_SUCCESS;
}

uint32_t pipe_write(PPIPE_OBJECT p, const void* buf, uint32_t len) {
    if (!p || !buf || !len) return 0;
    const uint8_t* b = (const uint8_t*)buf;
    uint32_t wrote = 0;
    while (wrote < len && p->Count < PIPE_BUF_SIZE) {
        p->Buffer[p->Tail] = b[wrote];
        p->Tail = (p->Tail + 1) % PIPE_BUF_SIZE;
        p->Count++;
        wrote++;
    }
    kputs("[pipe] pipe_write('"); kputs(p->Name); kputs("'): ");
    kput_dec(wrote); kputs(" bytes -> buffer (ocupacao="); kput_dec(p->Count);
    kputs(")\n");
    return wrote;
}

uint32_t pipe_read(PPIPE_OBJECT p, void* buf, uint32_t len) {
    if (!p || !buf || !len) return 0;
    uint8_t* b = (uint8_t*)buf;
    uint32_t got = 0;
    while (got < len && p->Count > 0) {
        b[got] = p->Buffer[p->Head];
        p->Head = (p->Head + 1) % PIPE_BUF_SIZE;
        p->Count--;
        got++;
    }
    kputs("[pipe] pipe_read('"); kputs(p->Name); kputs("'): ");
    kput_dec(got); kputs(" bytes <- buffer (restam="); kput_dec(p->Count);
    kputs(")\n");
    return got;
}
