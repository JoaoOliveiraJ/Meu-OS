#include <stdint.h>
#include "ldr/loader.h"
#include "ldr/pe.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "ke/amd64/usermode.h"
#include "ps/process.h"
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

// ---- Enumeracao de modulos (GATE 2 do pintok.sys) ----------------------------
int ldr_get_module_count(void) { return s_nmods; }

int ldr_get_module_info(int i, uint64_t* out_base, uint32_t* out_size, const char** out_name) {
    if (i < 0 || i >= s_nmods) return 0;
    if (out_name) *out_name = s_mods[i].name;
    // Base de carga: a VA mapeada se ja foi carregado; senao o ponteiro dos
    // bytes brutos (identidade — o kernel mapeia 1 GiB 1:1, entao um driver
    // ainda nao "carregado" mas com bytes na RAM ainda tem um endereco valido).
    uint64_t base = s_mods[i].base
                  ? (uint64_t)(uintptr_t)s_mods[i].base
                  : (uint64_t)(uintptr_t)s_mods[i].bytes;
    if (out_base) *out_base = base;
    // SizeOfImage lido do optional header do PE (offset 0x3C -> e_lfanew;
    // +4 PE sig +20 file header = optional header; SizeOfImage @ +0x38 do opt).
    uint32_t size = 0;
    const uint8_t* f = (const uint8_t*)s_mods[i].bytes;
    if (f) {
        uint32_t e = *(const uint32_t*)(f + 0x3C);
        // valida 'PE\0\0' antes de ler o opt header (evita lixo).
        if (*(const uint32_t*)(f + e) == 0x00004550u) {
            const uint8_t* opt = f + e + 4 + 20;
            size = *(const uint32_t*)(opt + 0x38);   // SizeOfImage (PE32 e PE32+ no mesmo offset)
        }
    }
    if (out_size) *out_size = size;
    return 1;
}

void ldr_module_set_base(const char* name, void* base) {
    int i = find_mod(name);
    if (i >= 0) s_mods[i].base = base;
}

static int has_prefix_ci(const char* s, const char* pfx) {
    while (*pfx) { if (!*s || lower(*s) != lower(*pfx)) return 0; s++; pfx++; }
    return 1;
}

// resolve um import (dll!fn) -> endereco, carregando a DLL se preciso
// API Sets (api-ms-win-*.dll) sao contratos virtuais: o Windows os encaminha p/ a DLL
// implementadora REAL. Mapeamos cada familia p/ a NOSSA DLL real (mesmas funcoes por
// nome). Familias sem implementacao ainda (combase/shell32/shcore/...) NAO tem redirect
// -> o import fica NAO resolvido (honesto: aparece no log e vira o proximo item a
// implementar DE VERDADE). NUNCA um stub generico catch-all.
static const char* apiset_redirect(const char* dll) {
    if (!dll) return dll;
    if (has_prefix_ci(dll, "api-ms-win-crt-"))           return "ucrtbase.dll";
    if (has_prefix_ci(dll, "api-ms-win-core-com"))       return "combase.dll";  // COM base (CoTaskMemAlloc/Co*)
    if (has_prefix_ci(dll, "api-ms-win-core-winrt"))     return "combase.dll";  // WinRT (HSTRING/Ro*/error) vive na combase, igual ao Windows real
    if (has_prefix_ci(dll, "api-ms-win-core-registry"))  return "advapi32.dll"; // Reg* vivem na advapi32
    if (has_prefix_ci(dll, "api-ms-win-core-"))          return "kernel32.dll"; // kernelbase -> nosso kernel32
    if (has_prefix_ci(dll, "api-ms-win-rtcore-ntuser-")) return "user32.dll";
    if (has_prefix_ci(dll, "api-ms-win-ntuser-"))        return "user32.dll";
    if (has_prefix_ci(dll, "api-ms-win-security-"))      return "advapi32.dll";
    if (has_prefix_ci(dll, "api-ms-win-eventing-"))      return "advapi32.dll"; // ETW: no-op (sem tracing)
    if (has_prefix_ci(dll, "api-ms-win-shell-"))         return "shell32.dll"; // namespace/changenotify/dataobject/shdirectory
    if (has_prefix_ci(dll, "api-ms-win-shcore-"))        return "shcore.dll";  // DPI/stream/registro/thread/appid
    // api-ms-win-shlwapi-winrt-storage: SHPinDllOfCLSID / StrRetTo* / SHCreateWorkerWindowW /
    // AssocQueryStringW / IUnknown_GetWindow ... — hospedadas na shcore. (As familias
    // api-ms-win-CORE-shlwapi-legacy/obsolete sao OUTRO contrato: caem no "api-ms-win-core-"
    // abaixo -> kernel32, onde ja vivem os Path*/Str*.) Sem este redirect, os imports ficavam
    // slot=0 e o 1o CALL (SHPinDllOfCLSID, na init pesada do shell) dava #PF rip=0.
    if (has_prefix_ci(dll, "api-ms-win-shlwapi-"))       return "shcore.dll";
    if (has_prefix_ci(dll, "api-ms-win-storage-"))       return "shell32.dll"; // storage-exports-internal: SHGetFolderPathEx/KnownFolderIDList + Get/SetThreadFlags
    // Extension API Sets (ext-ms-win-*): contratos OPCIONAIS (delay-load) -> DLL host real.
    if (has_prefix_ci(dll, "ext-ms-win-rtcore-ntuser-")) return "user32.dll";
    if (has_prefix_ci(dll, "ext-ms-win-ntuser-"))        return "user32.dll";
    if (has_prefix_ci(dll, "ext-ms-win-session-winsta-"))return "user32.dll";
    if (has_prefix_ci(dll, "ext-ms-win-gdi-"))           return "gdi32.dll";
    if (has_prefix_ci(dll, "ext-ms-win-shell32-"))       return "shell32.dll";
    if (has_prefix_ci(dll, "ext-ms-win-shell-"))         return "shell32.dll";
    if (has_prefix_ci(dll, "ext-ms-win-security-"))      return "advapi32.dll";
    if (has_prefix_ci(dll, "ext-ms-win-core-"))          return "kernel32.dll";
    // DLLs DIRETAS mapeadas p/ um host que ja temos (estilo Wine): evita um modulo
    // Multiboot extra (limite ~16). userenv (perfil/appcontainer) -> advapi32 (seguranca).
    if (has_prefix_ci(dll, "userenv"))                   return "advapi32.dll";
    if (has_prefix_ci(dll, "sspicli"))                   return "advapi32.dll";  // SSPI: GetUserNameExW etc.
    // KernelBase.dll hospeda no Windows as APIs core que os contratos api-ms-win-core-*
    // encaminham. Nos hospedamos essas mesmas funcoes no nosso kernel32 -> kernelbase =
    // kernel32 (estilo Wine). Explorer faz LoadLibrary("kernelbase.dll") em runtime.
    if (has_prefix_ci(dll, "kernelbase"))                return "kernel32.dll";
    return dll;
}

static void* ldr_resolve(const char* dll, const char* fn) {
    void* base = ldr_load(apiset_redirect(dll));
    if (!base) return 0;
    if (fn && fn[0] == '#') {                       // import por ORDINAL (#N)
        uint32_t ord = 0;
        for (const char* p = fn + 1; *p >= '0' && *p <= '9'; p++) ord = ord * 10 + (uint32_t)(*p - '0');
        return pe_get_export_by_ordinal(base, ord);
    }
    return pe_get_export(base, fn);
}

// Frente C: base >= 64 MiB cai no territorio gerenciado pelo PMM (o mesmo pool que
// o pmm_alloc_contiguous entrega ao VirtualAlloc do explorer). Uma DLL mapeada FIXA
// nessa faixa seria CORROMPIDA quando o heap do explorer crescesse e o PMM reentregasse
// esses frames. Abaixo disso e' regiao de identidade fora do PMM (segura).
#define LDR_PMM_TERRITORY 0x4000000ULL

static int pe_has_relocs(const void* image);   // definido adiante

void* ldr_load(const char* name) {
    int i = find_mod(name);
    if (i < 0) { kputs("[ldr] DLL nao registrada: "); kputs(name); kputc('\n'); return 0; }
    if (s_mods[i].base) return s_mods[i].base;          // ja carregada

    void* entry = 0;
    void* base  = 0;

    // DLLs cuja base PREFERIDA cai no territorio do PMM (>= 64 MiB): nao da p/ mapear
    // no lugar preferido (o VirtualAlloc do explorer reusaria os frames e corromperia a
    // DLL — provado pelo #UD em msvcp_win apos o heap crescer). Alocamos frames
    // RESERVADOS no PMM e RELOCAMOS a imagem (elas tem .reloc/--dynamicbase), igual o
    // ldr_run faz p/ .exe de base alta. Frames do PMM ficam marcados como usados, entao
    // o VirtualAlloc nunca os reentrega. DLLs de base baixa seguem o caminho original.
    // Aplica a QUALQUER base preferida no territorio do PMM. Se a DLL tem .reloc,
    // pe_relocate corrige os enderecos absolutos; se NAO tem (as nossas stubs sao
    // position-independent — clang x64 usa RIP-relative), pe_relocate e' no-op e a
    // imagem funciona no novo lugar mesmo assim. O ponto e' tirar a DLL da faixa que
    // o VirtualAlloc reusaria; sem .reloc-gate porque a stub sem relocs e' justamente
    // a que pode ser movida em seguranca.
    pe_info_t pi;
    if (pe_parse(s_mods[i].bytes, &pi) && pi.preferred >= LDR_PMM_TERRITORY) {
        uint64_t pages = (pi.size_image + 0xFFFu) / 0x1000u;
        uint64_t alt   = pmm_alloc_contiguous(pages);
        if (!alt || alt >= 0x40000000ULL) { kputs("[ldr] sem RAM contigua p/ DLL de base alta: "); kputs(name); kputc('\n'); return 0; }
        base = pe_map_at(s_mods[i].bytes, alt, &entry);
        if (!base) return 0;
        s_mods[i].base = base;                          // registra ANTES de resolver (evita loop)
        uint64_t map_sz = (pi.size_image + 0x1FFFFFull) & ~0x1FFFFFull;
        if (map_sz < 0x200000u) map_sz = 0x200000u;
        mm_map_user(alt, map_sz);                       // DLL acessivel ao usuario
        uint32_t n = pe_relocate(base, pi.preferred);
        kputs("[ldr] carregando (PMM+reloc) "); kputs(s_mods[i].name);
        kputs(" @ "); kput_hex(alt); kputs(" (pref "); kput_hex(pi.preferred);
        kputs(", relocs="); kput_dec(n); kputs(")\n");
        pe_bind_imports(base, ldr_resolve);             // resolve os imports da DLL (recursivo)
        return base;
    }

    base = pe_map(s_mods[i].bytes, &entry);
    if (!base) return 0;
    s_mods[i].base = base;                              // registra ANTES de resolver (evita loop)
    mm_map_user((uint64_t)(uintptr_t)base, 0x200000);   // DLL acessivel ao usuario

    kputs("[ldr] carregando "); kputs(s_mods[i].name);
    kputs(" @ "); kput_hex((uint64_t)(uintptr_t)base); kputc('\n');
    pe_bind_imports(base, ldr_resolve);                 // resolve os imports da DLL (recursivo)
    return base;
}

// LoadLibrary em RUNTIME (kernel32!LoadLibraryW -> sys_loadlibrary). Diferente do bind
// ESTATICO (ldr_resolve, que ja aplica apiset_redirect), o caminho de runtime chamava
// ldr_load DIRETO — por isso falhava em (1) apisets versionados ("api-ms-win-core-synch-
// l1-2-0.dll") e (2) nomes SEM extensao (LoadLibraryW(L"dui70")). Aqui aplicamos as duas
// regras do Windows: encaminhamento de contrato (apiset_redirect) e o default de anexar
// ".dll" quando o nome nao tem extensao. Assim o runtime resolve as MESMAS DLLs que o
// bind estatico. So imprime "nao registrada" se, apos ambas as regras, o modulo faltar
// mesmo (honesto: vira o proximo alvo a implementar).
// anexa ".dll" ao basename SE nao houver extensao; devolve 1 se anexou (out em 'buf').
static int append_dll_if_bare(const char* red, char* buf, int cap) {
    const char* bn = basename(red);
    for (const char* c = bn; *c; c++) if (*c == '.') return 0;   // ja tem extensao
    int k = 0; while (red[k] && k < cap - 5) { buf[k] = red[k]; k++; }
    buf[k++] = '.'; buf[k++] = 'd'; buf[k++] = 'l'; buf[k++] = 'l'; buf[k] = 0;
    return 1;
}

void* ldr_load_runtime(const char* name) {
    if (!name) return 0;
    const char* red = apiset_redirect(name);            // contrato virtual -> DLL real
    if (find_mod(red) >= 0) {
        void* b = ldr_load(red);
        if (b && red != name) { kputs("[ldr] runtime '"); kputs(name); kputs("' -> "); kputs(red); kputc('\n'); }
        return b;                                        // achou direto (talvez redirecionado)
    }
    char buf[64];
    if (append_dll_if_bare(red, buf, sizeof(buf)) && find_mod(buf) >= 0) {
        void* b = ldr_load(buf);
        if (b) { kputs("[ldr] runtime '"); kputs(name); kputs("' -> "); kputs(buf); kputc('\n'); }
        return b;
    }
    return ldr_load(red);                                // honesto: imprime a falta real
}

// GetModuleHandle: base de um modulo JA carregado (NAO carrega), aplicando as mesmas regras
// do LoadLibrary (apiset_redirect + ".dll"). Antes o sys_getmodulehandle chamava ldr_load
// direto (sem redirect) e imprimia "nao registrada" p/ apisets/kernelbase que na verdade
// ja estao carregados como kernel32. Devolve 0 se registrado-mas-nao-carregado ou ausente.
void* ldr_find_runtime(const char* name) {
    if (!name) return 0;
    const char* red = apiset_redirect(name);
    int i = find_mod(red);
    if (i < 0) { char buf[64]; if (append_dll_if_bare(red, buf, sizeof(buf))) i = find_mod(buf); }
    return (i >= 0) ? s_mods[i].base : 0;                // base cacheada (0 = ainda nao mapeado)
}

// FASE 3g — carrega uma imagem PE a partir de BYTES em memoria (ex.: uma DLL lida do
// disco NTFS pelo sys_loadlibrary), registrando-a sob basename(name) e mapeando +
// bindando IGUAL ao ldr_load faz p/ modulos de boot. Devolve a base (0 se falhar). Se
// ja estava carregada sob esse nome, devolve a base existente.
void* ldr_load_image(const char* name, const void* bytes) {
    const char* bn = basename(name);
    int i = find_mod(bn);
    if (i >= 0 && s_mods[i].base) return s_mods[i].base;   // ja carregada
    if (i < 0) {
        ldr_register(bn, bytes);                            // registra os bytes sob o basename
        i = find_mod(bn);
        if (i < 0) return 0;                                // tabela de modulos cheia
    } else {
        s_mods[i].bytes = bytes;
    }
    void* entry = 0;
    void* base  = pe_map(bytes, &entry);
    if (!base) return 0;
    s_mods[i].base = base;
    mm_map_user((uint64_t)(uintptr_t)base, 0x200000);       // DLL acessivel ao usuario
    kputs("[ldr] mapeando imagem (do disco) "); kputs(bn);
    kputs(" @ "); kput_hex((uint64_t)(uintptr_t)base); kputc('\n');
    pe_bind_imports(base, ldr_resolve);                     // resolve os imports da DLL
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

    // 1 GiB e o limite da identidade (mesmo criterio do caminho de driver em
    // driver.c:98-129). Um .exe REAL do Windows costuma ter ImageBase alto (ex.:
    // 0x140000000 = 5 GiB, default do MinGW/MSVC) e/ou nao caber na faixa baixa;
    // nesse caso carregamos numa base baixa via PMM e aplicamos o .reloc. Os apps
    // de ImageBase baixo (todos os nossos, < 1 GiB) seguem EXATAMENTE no caminho
    // original (pe_map no preferido) — sem qualquer mudanca de comportamento.
    void* entry = 0;
    void* base  = 0;
    if (pi.preferred >= 0x40000000ULL || pi.size_image > 0x800000u) {
        if (!pe_has_relocs(image)) {
            kputs("[ldr] .exe de ImageBase alto SEM .reloc — nao da p/ realocar (");
            kput_hex(pi.preferred); kputs("); abortando\n");
            return;
        }
        uint64_t pages = (pi.size_image + 4095u) / 4096u;
        uint64_t alt   = pmm_alloc_contiguous(pages);
        if (!alt || alt >= 0x40000000ULL) {
            kputs("[ldr] sem RAM contigua p/ o .exe ("); kput_dec(pi.size_image); kputs(" bytes)\n");
            return;
        }
        kputs("[ldr] .exe em base alternativa @"); kput_hex(alt);
        kputs(" (ImageBase preferido "); kput_hex(pi.preferred); kputs(", .reloc sera aplicado)\n");
        base = pe_map_at(image, alt, &entry);
        if (!base || !entry) { kputs("[ldr] pe_map_at falhou\n"); return; }
        uint64_t map_sz = (pi.size_image + 0x1FFFFFull) & ~0x1FFFFFull;
        if (map_sz < 0x200000u) map_sz = 0x200000u;
        mm_map_user(alt, map_sz);
        uint32_t n = pe_relocate(base, pi.preferred);
        kputs("[ldr] relocacoes (.reloc) aplicadas: "); kput_dec(n); kputc('\n');
    } else {
        base = pe_map(image, &entry);
        if (!base || !entry) { kputs("[ldr] falha ao mapear o .exe\n"); return; }
        mm_map_user((uint64_t)(uintptr_t)base, 0x200000);
    }

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
