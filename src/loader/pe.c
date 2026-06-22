#include <stdint.h>
#include "loader/pe.h"
#include "win32/win32.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

typedef struct __attribute__((packed)) { uint32_t rva, size; } datadir_t;

// Le o campo Subsystem do PE (sem carregar): 1=NATIVE/.sys, 2=GUI, 3=console.
int pe_subsystem(const void* image) {
    const uint8_t* f = (const uint8_t*)image;
    if (f[0] != 'M' || f[1] != 'Z') return -1;
    uint32_t e_lfanew = *(const uint32_t*)(f + 0x3C);
    const uint8_t* nt = f + e_lfanew;
    if (!(nt[0] == 'P' && nt[1] == 'E')) return -1;
    const uint8_t* opt = nt + 4 + 20;           // pula assinatura + COFF header
    return *(const uint16_t*)(opt + 68);        // Subsystem
}

// Carrega um PE32+ no seu ImageBase preferido (sem relocacoes), resolve os
// imports via callback e devolve a base; *entry_out = entry point.
void* pe_load(const void* image, pe_resolver_t resolve, void** entry_out) {
    const uint8_t* f = (const uint8_t*)image;
    if (entry_out) *entry_out = 0;

    if (f[0] != 'M' || f[1] != 'Z') { kputs("[PE] cabecalho MZ invalido\n"); return 0; }
    uint32_t e_lfanew = *(const uint32_t*)(f + 0x3C);
    const uint8_t* nt = f + e_lfanew;
    if (!(nt[0]=='P' && nt[1]=='E' && nt[2]==0 && nt[3]==0)) {
        kputs("[PE] assinatura PE invalida\n"); return 0;
    }

    const uint8_t* coff = nt + 4;
    uint16_t machine      = *(const uint16_t*)(coff + 0);
    uint16_t num_sections = *(const uint16_t*)(coff + 2);
    uint16_t opt_size     = *(const uint16_t*)(coff + 16);
    const uint8_t* opt    = coff + 20;
    uint16_t opt_magic    = *(const uint16_t*)(opt + 0);
    if (machine != 0x8664 || opt_magic != 0x20B) {
        kputs("[PE] nao e PE32+ x86-64\n"); return 0;
    }

    uint32_t entry_rva  = *(const uint32_t*)(opt + 16);
    uint64_t image_base = *(const uint64_t*)(opt + 24);
    uint32_t size_image = *(const uint32_t*)(opt + 56);
    uint32_t size_hdrs  = *(const uint32_t*)(opt + 60);
    uint32_t num_dirs   = *(const uint32_t*)(opt + 108);
    const datadir_t* dirs = (const datadir_t*)(opt + 112);

    uint8_t* img = (uint8_t*)(uintptr_t)image_base;   // carregamos aqui (identidade)

    kputs("[PE] PE32+ x64 | ImageBase="); kput_hex(image_base);
    kputs(" | SizeOfImage="); kput_hex(size_image);
    kputs(" | secoes="); kput_dec(num_sections); kputc('\n');

    for (uint32_t i = 0; i < size_image; i++) img[i] = 0;
    for (uint32_t i = 0; i < size_hdrs;  i++) img[i] = f[i];

    const uint8_t* sh = opt + opt_size;
    for (uint16_t s = 0; s < num_sections; s++, sh += 40) {
        uint32_t vaddr  = *(const uint32_t*)(sh + 12);
        uint32_t rawsz  = *(const uint32_t*)(sh + 16);
        uint32_t rawptr = *(const uint32_t*)(sh + 20);
        for (uint32_t i = 0; i < rawsz; i++) img[vaddr + i] = f[rawptr + i];
    }

    // ---- tabela de imports (data directory 1) ----
    if (num_dirs > 1 && dirs[1].rva) {
        const uint8_t* imp = img + dirs[1].rva;
        for (;; imp += 20) {
            uint32_t oft  = *(const uint32_t*)(imp + 0);
            uint32_t name = *(const uint32_t*)(imp + 12);
            uint32_t ft   = *(const uint32_t*)(imp + 16);
            if (oft == 0 && name == 0 && ft == 0) break;

            const char* dll = (const char*)(img + name);
            const uint64_t* ilt = (const uint64_t*)(img + (oft ? oft : ft));
            uint64_t*       iat = (uint64_t*)(img + ft);

            kputs("[PE] importando de "); kputs(dll); kputs(":\n");
            for (uint32_t k = 0; ilt[k]; k++) {
                uint64_t thunk = ilt[k];
                void* fn = 0;
                const char* fname = "(ordinal)";
                if (!(thunk & (1ULL << 63))) {
                    fname = (const char*)(img + (uint32_t)thunk + 2);  // pula Hint
                    fn = resolve(dll, fname);
                }
                kputs("       - "); kputs(fname);
                kputs(fn ? "  -> ok\n" : "  -> NAO IMPLEMENTADO\n");
                iat[k] = (uint64_t)(uintptr_t)fn;
            }
        }
    }

    if (entry_out) *entry_out = (void*)(uintptr_t)(img + entry_rva);
    return img;
}

// Executa a imagem como um .exe Win32 (entry void(void), ABI Microsoft).
void pe_run(const void* image) {
    void* entry = 0;
    if (!pe_load(image, win32_resolve, &entry) || !entry) return;

    typedef void (__attribute__((ms_abi)) * entry_t)(void);
    kputs("[PE] chamando entry em "); kput_hex((uint64_t)(uintptr_t)entry); kputs(" ...\n");

    if (__builtin_setjmp(g_pe_exit) == 0) {
        ((entry_t)entry)();
        kputs("[PE] o entry retornou por conta propria.\n");
    } else {
        kputs("[PE] processo encerrado; controle de volta ao kernel.\n");
    }
}
