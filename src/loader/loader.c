#include <stdint.h>
#include "loader/loader.h"
#include "loader/pe.h"
#include "mm/paging.h"
#include "ke/usermode.h"
#include "nt/process.h"
#include "win32/win32k.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

#define MAX_MODULES 32

static struct { char name[32]; const void* bytes; void* base; } s_mods[MAX_MODULES];
static int s_nmods;

// ---- utilitarios de string ----
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ieq(const char* a, const char* b) {
    while (*a && *b) { if (lower(*a) != lower(*b)) return 0; a++; b++; }
    return *a == *b;
}

static const char* basename(const char* path) {
    const char* p = path;
    for (const char* c = path; *c; c++) if (*c == '\\' || *c == '/') p = c + 1;
    return p;
}

int ldr_match_ext(const char* path, const char* ext) {
    const char* s = basename(path);
    int ls = 0, le = 0;
    while (s[ls]) ls++;
    while (ext[le]) le++;
    if (ls < le) return 0;
    for (int i = 0; i < le; i++) if (lower(s[ls - le + i]) != lower(ext[i])) return 0;
    return 1;
}

// ---- registro de modulos ----
void ldr_register(const char* path, const void* image) {
    if (s_nmods >= MAX_MODULES) return;
    const char* n = basename(path);
    int i = 0;
    while (n[i] && i < 31) { s_mods[s_nmods].name[i] = n[i]; i++; }
    s_mods[s_nmods].name[i] = 0;
    s_mods[s_nmods].bytes = image;
    s_mods[s_nmods].base = 0;
    s_nmods++;
}

static int find_mod(const char* name) {
    for (int i = 0; i < s_nmods; i++) if (ieq(s_mods[i].name, name)) return i;
    return -1;
}

// Bytes brutos de um modulo registrado, pelo nome. 0 se nao registrado.
const void* ldr_get_module_bytes(const char* name) {
    int i = find_mod(name);
    return (i >= 0) ? s_mods[i].bytes : 0;
}

// resolve um import (dll!fn) -> endereco, carregando a DLL se preciso
static void* ldr_resolve(const char* dll, const char* fn) {
    void* base = ldr_load(dll);
    if (!base) return 0;
    return pe_get_export(base, fn);
}

void* ldr_load(const char* name) {
    int i = find_mod(name);
    if (i < 0) { kputs("[ldr] DLL nao registrada: "); kputs(name); kputc('\n'); return 0; }
    if (s_mods[i].base) return s_mods[i].base;          // ja carregada

    void* entry = 0;
    void* base = pe_map(s_mods[i].bytes, &entry);
    if (!base) return 0;
    s_mods[i].base = base;                              // registra ANTES de resolver (evita loop)
    mm_map_user((uint64_t)(uintptr_t)base, 0x200000);   // DLL acessivel ao usuario

    kputs("[ldr] carregando "); kputs(s_mods[i].name);
    kputs(" @ "); kput_hex((uint64_t)(uintptr_t)base); kputc('\n');
    pe_bind_imports(base, ldr_resolve);                 // resolve os imports da DLL (recursivo)
    return base;
}

// Base de carga para EXEs de 32 bits (faixa separada da 64-bit). Se o PE32
// tiver .reloc, carregamos numa base DESLOCADA da preferida para EXERCITAR as
// relocacoes; senao (relocs strip) carregamos no ImageBase preferido.
#define LOAD32_BASE 0x1600000ULL

// Verdadeiro se a imagem tem um data directory de base relocations nao vazio.
static int pe_has_relocs(const void* image) {
    pe_info_t pi;
    if (!pe_parse(image, &pi)) return 0;
    if (pi.ndirs <= 5) return 0;
    const uint8_t* f = (const uint8_t*)image;
    uint32_t e = *(const uint32_t*)(f + 0x3C);
    const uint8_t* opt = f + e + 4 + 20;
    const uint8_t* dirs = opt + pi.dir_off;
    uint32_t reloc_rva  = *(const uint32_t*)(dirs + 5 * 8);
    uint32_t reloc_size = *(const uint32_t*)(dirs + 5 * 8 + 4);
    return reloc_rva && reloc_size;
}

// Roda um .exe de 32 bits (PE32) em ring 3 compatibility mode.
static void ldr_run32(const char* path, const void* image, const pe_info_t* pi) {
    kputs("[ldr] PE32 (32-bit) detectado: machine="); kput_hex(pi->machine);
    kputs(" magic="); kput_hex(pi->magic);
    kputs(" ImageBase preferido="); kput_hex(pi->preferred);
    kputs(" entryRVA="); kput_hex(pi->entry_rva); kputc('\n');

    // Escolhe a base de carga. Demonstra relocacao quando ha .reloc.
    int has_reloc = pe_has_relocs(image);
    uint64_t load_base = has_reloc ? LOAD32_BASE : pi->preferred;
    if (has_reloc && load_base == pi->preferred) load_base = LOAD32_BASE + 0x100000ULL;

    void* entry = 0;
    void* base = pe_map_at(image, load_base, &entry);
    if (!base || !entry) { kputs("[ldr] falha ao mapear o PE32\n"); return; }
    mm_map_user(load_base, 0x200000);

    kputs("[ldr] PE32 mapeado @ "); kput_hex((uint64_t)(uintptr_t)base);
    kputs(" (delta="); kput_hex((uint64_t)((uint64_t)(uintptr_t)base - pi->preferred));
    kputs(")\n");

    if (has_reloc) {
        uint32_t n = pe_relocate(base, pi->preferred);
        kputs("[ldr] relocacoes (.reloc) aplicadas: "); kput_dec(n); kputc('\n');
    } else {
        kputs("[ldr] sem .reloc (relocs strip): carregado no ImageBase preferido\n");
    }

    uint64_t cr3; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    PEPROCESS proc = PsCreateProcess(basename(path), (uint64_t)(uintptr_t)base, cr3);
    PETHREAD  thr  = proc ? PsCreateThread(proc, (uint64_t)(uintptr_t)entry, 0, 0) : 0;
    PEPROCESS prev_p = PsGetCurrentProcess();
    PETHREAD  prev_t = PsGetCurrentThread();
    ps_set_current(proc, thr);

    kputs("[ldr] resolvendo imports do PE32...\n");
    pe_bind_imports(base, ldr_resolve);

    kputs("[ldr] entrando no PE32 em RING 3 de 32 bits (compat mode)...\n");
    usermode_enter32((uint32_t)(uintptr_t)entry);
    kputs("[ldr] o PE32 (ring 3 32-bit) terminou; de volta ao kernel.\n");

    if (proc && !proc->Terminated) PsTerminateProcess(proc, 0);
    ps_set_current(prev_p, prev_t);
}

void ldr_run(const char* path, const void* image) {
    pe_info_t pi;
    if (!pe_parse(image, &pi)) { kputs("[ldr] PE invalido\n"); return; }
    if (!pi.is64) { ldr_run32(path, image, &pi); return; }   // caminho 32-bit

    void* entry = 0;
    void* base = pe_map(image, &entry);
    if (!base || !entry) { kputs("[ldr] falha ao mapear o .exe\n"); return; }
    mm_map_user((uint64_t)(uintptr_t)base, 0x200000);

    // Cria o EPROCESS para este .exe (objeto do Object Manager). Guarda o CR3
    // atual (espaco compartilhado por ora) e a base da imagem; a thread
    // principal recebe o entry point. Marca como processo corrente.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    PEPROCESS proc = PsCreateProcess(basename(path), (uint64_t)(uintptr_t)base, cr3);
    PETHREAD  thr  = proc ? PsCreateThread(proc, (uint64_t)(uintptr_t)entry, 0, 0) : 0;
    PEPROCESS prev_p = PsGetCurrentProcess();
    PETHREAD  prev_t = PsGetCurrentThread();
    ps_set_current(proc, thr);

    kputs("[ldr] resolvendo imports do .exe (carrega kernel32/user32/ntdll)...\n");
    pe_bind_imports(base, ldr_resolve);

    kputs("[ldr] entrando no .exe em RING 3 (modo usuario)...\n");
    usermode_enter((void (*)(void))entry);
    kputs("[ldr] o .exe (ring 3) terminou; de volta ao kernel.\n");

    // FASE 6: libera as janelas que este processo (app GUI) deixou abertas, para
    // a tabela do win32k nao acumular janelas fantasma entre apps. O framebuffer
    // permanece como o app pintou (o screendump mostra a ultima GUI).
    win32k_reap_process_windows(proc);

    if (proc && !proc->Terminated) PsTerminateProcess(proc, 0);
    ps_set_current(prev_p, prev_t);   // restaura o processo anterior (aninhamento)
}
