// ============================================================================
//  pe_export_image.c — constroi, EM MEMORIA, uma imagem PE32+ "ntoskrnl.exe"
//  sintetica cuja EXPORT DIRECTORY e valida e contem TODOS os nomes que o
//   pintok.sys) resolve.
//
//  POR QUE ISSO EXISTE
//  -------------------
//  O pintok.sys NAO usa a IAT para achar funcoes do kernel. Durante a DriverEntry
//  ele:
//    1. enumera os modulos carregados via ZwQuerySystemInformation(class 0x4D,
//       SystemModuleInformationEx) e pega a ImageBase do "ntoskrnl.exe";
//    2. faz o PARSE MANUAL do PE nessa base: valida "MZ" + e_lfanew + "PE\0\0",
//       le o Optional Header (magic 0x20B), acessa DataDirectory[0] (EXPORT);
//    3. percorre a export directory por BUSCA BINARIA em AddressOfNames (que
//       PRECISA estar ordenado por strcmp), e em cada acerto faz
//       name-index -> AddressOfNameOrdinals[idx] -> AddressOfFunctions[ord], e
//       CHAMA  (ImageBase + AddressOfFunctions[ord]).
//
//  Confirmado pelos traces do emulador (scratchpad/run_strcmp.txt,
//  run_expdir.txt, ntos_winreads.pkl): pintok.sys le os campos da export directory em
//  +0x14 (NumberOfFunctions), +0x18 (NumberOfNames), +0x1C (AddressOfFunctions),
//  +0x20 (AddressOfNames), +0x24 (AddressOfNameOrdinals) — layout padrao de
//  IMAGE_EXPORT_DIRECTORY — e faz ~11 probes por alvo (busca binaria sobre
//  ~3070 nomes). Com um strcmp REAL, todos os 217 alvos resolvem.
//
//  Por isso, a imagem registrada como "ntoskrnl.exe" no MeuOS PRECISA ter uma
//  export directory de verdade (ordenada, completa, com RVAs chamaveis). Esta e
//  a maior peca estrutural do gate. O resto do ntoskrnl.c (a tabela
//  g_ntexports/ntkrnl_resolve) continua servindo a IAT dos drivers comuns; aqui
//  apenas espelhamos esses mesmos thunks numa export table parseavel.
//
//  COMO OS RVAs APONTAM PARA OS THUNKS REAIS
//  -----------------------------------------
//  Os thunks NT_* vivem no .text do kernel, em enderecos de 64 bits longe da
//  base desta imagem — o delta NAO cabe num RVA de 32 bits. Entao, para cada
//  export, emitimos DENTRO da imagem um pequeno trampolim de 12 bytes:
//        48 B8 <imm64>   movabs rax, <endereco real do thunk NT_*>
//        FF E0           jmp rax
//  e AddressOfFunctions[i] aponta para o RVA desse trampolim. Quando o pintok.sys
//  chama (ImageBase + RVA), cai no trampolim, que salta para o thunk real. A
//  identidade de baixo (onde mora o .bss do kernel) e mapeada RWX no MeuOS
//  (paging.c nunca seta NX), entao o trampolim e executavel em ring 0.
//
//  REGISTRO
//  --------
//  ldr_pe_export_image_build() monta a imagem uma vez num buffer estatico e
//  devolve {base, size}. main.c chama ldr_register("ntoskrnl.exe", img) e o
//  provedor de SystemModuleInformation deve reportar ESTE base/size para o
//  ntoskrnl.exe (ver tasks). pe_get_export() do proprio MeuOS tambem passa a
//  funcionar sobre esta imagem (mesma export table).
// ============================================================================
#include <stdint.h>
#include "ntddk.h"
#include "ntoskrnl.h"
#include "ldr/loader.h"
#include "ldr/pe.h"

extern void  kputs(const char* s);
extern void  kput_hex(uint64_t v);
extern void  kput_dec(uint64_t v);

// A lista (ASCII-ordenada) dos nomes de export que o pintok.sys procura. Gerada do
// trace run_strcmp.txt: a UNIAO de todos os alvos (b=) + nomes sondados (a=).
#include "ldr/ntos_export_names.inc"   // g_ntos_export_names[], NTOS_EXPORT_COUNT

// ----------------------------------------------------------------------------
//  Buffer estatico da imagem. Calculo do tamanho (folgado):
//    headers              : 0x1000  (MZ + PE + opt + 1 section header, com folga)
//    export dir header    : 40
//    AddressOfFunctions   : N*4
//    AddressOfNames       : N*4
//    AddressOfNameOrdinals: N*2
//    nomes (strings)      : sum(len+1)  (<= N*64)
//    trampolins           : N*12
//  Para N=642: ~0x1000 + 40 + 2568 + 2568 + 1284 + ~14450 + 7704 ≈ 36 KB.
//  Reservamos 96 KB para sobrar margem se a lista crescer.
// ----------------------------------------------------------------------------
#define NTOS_IMG_CAP   (96u * 1024u)
#define HDR_SIZE       0x400u          // MZ+PE+opt+section header cabem com folga
#define EXP_RVA        0x1000u         // RVA da secao .rdata (export)
#define TRAMP_BYTES    12u             // movabs rax,imm64 ; jmp rax

// Alinhado a pagina; identidade RWX cobre o .bss do kernel.
static uint8_t g_ntos_img[NTOS_IMG_CAP] __attribute__((aligned(4096)));
static int     g_built = 0;
static uint32_t g_img_size = 0;        // SizeOfImage efetivo

// --- helpers de escrita little-endian (freestanding: sem libc) ---
static inline void wr16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void wr32(uint8_t* p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline void wr64(uint8_t* p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

static uint32_t align_up(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }

// ----------------------------------------------------------------------------
//  Constroi a imagem. Idempotente: na 2a chamada so devolve o que ja montou.
//  Retorna a base (ponteiro) e, via out_size, o SizeOfImage. 0 em erro.
// ----------------------------------------------------------------------------
void* ldr_pe_export_image_build(uint32_t* out_size) {
    if (g_built) { if (out_size) *out_size = g_img_size; return g_ntos_img; }

    const uint32_t N = NTOS_EXPORT_COUNT;
    uint8_t* img = g_ntos_img;
    for (uint32_t i = 0; i < NTOS_IMG_CAP; i++) img[i] = 0;

    // A base REAL desta imagem (= onde o .bss caiu). E o que pintok.sys soma aos RVAs.
    uint64_t image_base = (uint64_t)(uintptr_t)img;

    // ----- layout da secao .rdata (tudo em RVAs a partir de EXP_RVA) -----
    uint32_t off_expdir = EXP_RVA;                 // IMAGE_EXPORT_DIRECTORY (40 bytes)
    uint32_t off_funcs  = off_expdir + 40;         // AddressOfFunctions  (N * uint32)
    uint32_t off_names  = off_funcs  + N * 4;      // AddressOfNames      (N * uint32)
    uint32_t off_ords   = off_names  + N * 4;      // AddressOfNameOrdinals (N * uint16)
    uint32_t off_strs   = off_ords   + N * 2;      // pool de strings dos nomes
    // strings: preenche e anota o RVA de cada nome.
    // (Reaproveitamos AddressOfNames para guardar os RVAs ao escrever.)
    uint32_t cur = off_strs;
    // 1) escreve as strings e o array AddressOfNames (RVA de cada string).
    for (uint32_t i = 0; i < N; i++) {
        const char* nm = g_ntos_export_names[i];
        uint32_t name_rva = cur;
        uint32_t k = 0;
        while (nm[k]) { img[cur++] = (uint8_t)nm[k]; k++; }
        img[cur++] = 0;                            // NUL
        wr32(img + off_names + i * 4, name_rva);   // AddressOfNames[i]
        wr16(img + off_ords  + i * 2, (uint16_t)i);// AddressOfNameOrdinals[i] = i
    }
    // 2) trampolins (alinhados a 16 p/ ficar limpo) + AddressOfFunctions.
    uint32_t off_tramp = align_up(cur, 16);
    cur = off_tramp;
    for (uint32_t i = 0; i < N; i++) {
        const char* nm = g_ntos_export_names[i];
        // Resolve o endereco real do thunk NT_* (ou stub generico) via a mesma
        // tabela que serve a IAT. NUNCA devolve 0 (ntkrnl_resolve cai no stub).
        void* fn = ntkrnl_resolve("ntoskrnl.exe", nm);
        uint64_t target = (uint64_t)(uintptr_t)fn;
        uint32_t tramp_rva = cur;
        // movabs rax, imm64
        img[cur++] = 0x48; img[cur++] = 0xB8;
        wr64(img + cur, target); cur += 8;
        // jmp rax
        img[cur++] = 0xFF; img[cur++] = 0xE0;
        wr32(img + off_funcs + i * 4, tramp_rva);  // AddressOfFunctions[i]
    }

    // ----- IMAGE_EXPORT_DIRECTORY (offsets padrao) -----
    uint8_t* ed = img + off_expdir;
    wr32(ed + 0x00, 0);                 // Characteristics
    wr32(ed + 0x04, 0);                 // TimeDateStamp
    wr16(ed + 0x08, 0);                 // MajorVersion
    wr16(ed + 0x0A, 0);                 // MinorVersion
    wr32(ed + 0x0C, EXP_RVA + 40 + 0);  // Name RVA (aponta p/ uma string; usamos o 1o func slot? nao — aponta p/ um literal). Setado abaixo.
    wr32(ed + 0x10, 1);                 // Base (ordinal base = 1)
    wr32(ed + 0x14, N);                 // NumberOfFunctions
    wr32(ed + 0x18, N);                 // NumberOfNames
    wr32(ed + 0x1C, off_funcs);         // AddressOfFunctions (RVA)
    wr32(ed + 0x20, off_names);         // AddressOfNames (RVA)
    wr32(ed + 0x24, off_ords);          // AddressOfNameOrdinals (RVA)
    // Name RVA deve apontar p/ uma string ASCIIZ "ntoskrnl.exe". Colocamos no
    // fim do buffer usado (depois dos trampolins) e corrigimos o campo.
    {
        static const char modname[] = "ntoskrnl.exe";
        uint32_t name_rva = cur;
        for (uint32_t k = 0; modname[k]; k++) img[cur++] = (uint8_t)modname[k];
        img[cur++] = 0;
        wr32(ed + 0x0C, name_rva);
    }

    uint32_t sec_raw_end = cur;
    uint32_t size_image  = align_up(sec_raw_end, 0x1000);   // SizeOfImage (pag.)
    uint32_t sec_vsize   = size_image - EXP_RVA;            // tamanho virtual da secao

    if (size_image > NTOS_IMG_CAP) {                        // sanidade
        kputs("[pe-export] ERRO: imagem excede o buffer ("); kput_dec(size_image);
        kputs(" > "); kput_dec(NTOS_IMG_CAP); kputs(")\n");
        return 0;
    }

    // ----- MZ header -----
    img[0] = 'M'; img[1] = 'Z';
    wr32(img + 0x3C, 0x80);             // e_lfanew -> PE header em 0x80

    // ----- PE\0\0 + COFF File Header (em 0x80) -----
    uint8_t* pe = img + 0x80;
    pe[0]='P'; pe[1]='E'; pe[2]=0; pe[3]=0;
    uint8_t* coff = pe + 4;
    wr16(coff + 0x00, 0x8664);          // Machine = AMD64
    wr16(coff + 0x02, 1);               // NumberOfSections = 1 (.rdata)
    wr32(coff + 0x04, 0);               // TimeDateStamp
    wr32(coff + 0x08, 0);               // PointerToSymbolTable
    wr32(coff + 0x0C, 0);               // NumberOfSymbols
    wr16(coff + 0x10, 0xF0);            // SizeOfOptionalHeader (240 = PE32+ c/ 16 dirs)
    wr16(coff + 0x12, 0x2022);          // Characteristics: EXECUTABLE|LARGE_ADDR|DLL

    // ----- Optional Header PE32+ (logo apos COFF, offset 0x18 a partir do PE) -----
    uint8_t* opt = coff + 0x14;         // COFF tem 20 (0x14) bytes
    wr16(opt + 0x00, 0x020B);           // Magic = PE32+
    opt[0x02] = 14; opt[0x03] = 0;      // Major/Minor LinkerVersion
    wr32(opt + 0x04, sec_vsize);        // SizeOfCode (aprox.)
    wr32(opt + 0x08, 0);                // SizeOfInitializedData
    wr32(opt + 0x0C, 0);                // SizeOfUninitializedData
    wr32(opt + 0x10, 0);                // AddressOfEntryPoint (driver-image; sem EP)
    wr32(opt + 0x14, EXP_RVA);          // BaseOfCode
    wr64(opt + 0x18, image_base);       // ImageBase (= base real desta imagem)
    wr32(opt + 0x20, 0x1000);           // SectionAlignment
    wr32(opt + 0x24, 0x200);            // FileAlignment
    wr16(opt + 0x28, 10); wr16(opt + 0x2A, 0);   // Major/Minor OS Version
    wr16(opt + 0x2C, 0);  wr16(opt + 0x2E, 0);   // Image Version
    wr16(opt + 0x30, 10); wr16(opt + 0x32, 0);   // Major/Minor Subsystem Version
    wr32(opt + 0x34, 0);                // Win32VersionValue
    wr32(opt + 0x38, size_image);       // SizeOfImage
    wr32(opt + 0x3C, HDR_SIZE);         // SizeOfHeaders
    wr32(opt + 0x40, 0);                // CheckSum
    wr16(opt + 0x44, 1);                // Subsystem = NATIVE
    wr16(opt + 0x46, 0x4160);           // DllCharacteristics (HIGH_ENTROPY|NX|DYNBASE)
    wr64(opt + 0x48, 0x40000);          // SizeOfStackReserve
    wr64(opt + 0x50, 0x1000);           // SizeOfStackCommit
    wr64(opt + 0x58, 0x100000);         // SizeOfHeapReserve
    wr64(opt + 0x60, 0x1000);           // SizeOfHeapCommit
    wr32(opt + 0x68, 0);                // LoaderFlags
    wr32(opt + 0x6C, 16);               // NumberOfRvaAndSizes
    // DataDirectory[] comeca em opt+0x70. [0] = EXPORT.
    // Size do EXPORT dir = do header ate o fim das strings/ords/funcs/nomes/
    // trampolins. pintok.sys valida o range, entao cobrimos toda a regiao.
    uint8_t* dd = opt + 0x70;
    wr32(dd + 0x00, off_expdir);             // EXPORT VirtualAddress
    wr32(dd + 0x04, sec_raw_end - off_expdir);// EXPORT Size
    // demais DataDirectory[1..15] = 0 (ja zerados).

    // ----- Section Header ".rdata" (logo apos o Optional Header) -----
    uint8_t* sh = opt + 0xF0;           // SizeOfOptionalHeader = 0xF0
    const char* sname = ".rdata";
    for (int i = 0; i < 6; i++) sh[i] = (uint8_t)sname[i];   // Name[8] (resto 0)
    wr32(sh + 0x08, sec_vsize);         // VirtualSize
    wr32(sh + 0x0C, EXP_RVA);           // VirtualAddress
    wr32(sh + 0x10, sec_vsize);         // SizeOfRawData
    wr32(sh + 0x14, EXP_RVA);           // PointerToRawData (mapeada 1:1: raw==rva)
    wr32(sh + 0x18, 0);                 // PointerToRelocations
    wr32(sh + 0x1C, 0);                 // PointerToLinenumbers
    wr16(sh + 0x20, 0);                 // NumberOfRelocations
    wr16(sh + 0x22, 0);                 // NumberOfLinenumbers
    wr32(sh + 0x24, 0x60000020);        // Characteristics: CODE|EXECUTE|READ

    g_img_size = size_image;
    g_built = 1;

    kputs("[pe-export] ntoskrnl.exe sintetico montado: base="); kput_hex(image_base);
    kputs(" SizeOfImage="); kput_hex(size_image);
    kputs(" exports="); kput_dec(N); kputs("\n");
    if (out_size) *out_size = size_image;
    return img;
}

// Conveniencia p/ o provedor de SystemModuleInformation: base/size ja prontos
// (constroi sob demanda na 1a chamada).
void* ldr_ntoskrnl_image_base(void) {
    uint32_t sz; return ldr_pe_export_image_build(&sz);
}
uint32_t ldr_ntoskrnl_image_size(void) {
    uint32_t sz; (void)ldr_pe_export_image_build(&sz); return sz;
}

// Registra a imagem como "ntoskrnl.exe" no loader (espelha a IAT na export
// table parseavel). Chame UMA vez no boot, antes de carregar o pintok.sys.
void ldr_register_ntoskrnl_export_image(void) {
    uint32_t sz = 0;
    void* img = ldr_pe_export_image_build(&sz);
    if (!img) { kputs("[pe-export] falha ao montar a imagem sintetica\n"); return; }
    ldr_register("ntoskrnl.exe", img);
    // A base de carga = a propria buffer (identidade); o ImageBase do PE foi
    // setado p/ esse endereco. Fixa em s_mods[] p/ a enumeracao de modulos
    // (SystemModuleInformation) reportar base/size consistentes com o PE.
    ldr_module_set_base("ntoskrnl.exe", img);
    kputs("[pe-export] registrado 'ntoskrnl.exe' (export table parseavel pelo pintok.sys)\n");
}
