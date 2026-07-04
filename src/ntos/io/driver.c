// Carregador de drivers de kernel — o papel do I/O Manager do NT ao dar
// "load" num .sys: monta o DRIVER_OBJECT e chama DriverEntry(Driver, Registry).
//
// Mantem tambem um "registro de drivers" (nome + DRIVER_OBJECT + estado), que e
// o que o cmd.exe consulta em 'sc query' e manipula em 'sc start'/'sc stop'
// (espelhando o Service Control Manager do Windows para drivers de kernel).
#include "ntddk.h"
#include "ldr/pe.h"
#include "ldr/loader.h"
#include "ntoskrnl.h"
#include "io/driver.h"
#include "ob/object.h"
#include "cm/registry.h"   // FASE 7.11: registry_create_driver_service

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// ---- Registro de drivers conhecidos (por nome) ----
#define MAX_DRIVERS 16

typedef struct _DRV_ENTRY {
    char           name[32];      // ex.: "mydriver.sys"
    uint32_t       state;         // DRV_STATE_STOPPED / DRV_STATE_RUNNING
    NTSTATUS       laststatus;    // status do ultimo DriverEntry
    PDRIVER_OBJECT object;        // DRIVER_OBJECT vivo (quando RUNNING)
    void*          base;          // base mapeada da imagem (quando RUNNING)
} DRV_ENTRY;

static DRV_ENTRY s_drivers[MAX_DRIVERS];
static int       s_ndrivers;

static char d_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  d_ieq(const char* a, const char* b) {
    while (*a && *b) { if (d_lower(*a) != d_lower(*b)) return 0; a++; b++; }
    return *a == *b;
}
static void d_copy(char* dst, const char* src, int max) {
    int i = 0;
    if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static const char* d_basename(const char* path) {
    const char* p = path;
    for (const char* c = path; *c; c++) if (*c == '\\' || *c == '/') p = c + 1;
    return p;
}

static DRV_ENTRY* drv_find(const char* name) {
    name = d_basename(name);
    for (int i = 0; i < s_ndrivers; i++) if (d_ieq(s_drivers[i].name, name)) return &s_drivers[i];
    return 0;
}

// Acrescenta (ou devolve) a entrada do driver no registro.
static DRV_ENTRY* drv_intern(const char* name) {
    name = d_basename(name);
    DRV_ENTRY* e = drv_find(name);
    if (e) return e;
    if (s_ndrivers >= MAX_DRIVERS) return 0;
    e = &s_drivers[s_ndrivers++];
    d_copy(e->name, name, (int)sizeof(e->name));
    e->state = DRV_STATE_STOPPED;
    e->laststatus = STATUS_SUCCESS;
    e->object = 0;
    e->base = 0;
    return e;
}

// FASE 7: caminho seguro de carga de driver — detecta ImageBase fora da
// identidade de 1 GiB OU image grande demais p/ o heap e realoca via PMM
// contiguo + pe_relocate.
#include "mm/pmm.h"
#include "ldr/pe.h"

// Define onde grandes drivers caem no espaco fisico. Comeca em 64 MiB (PMM base)
// e cresce conforme aloca. Como a identidade cobre 1 GiB e a RAM da VM e ~256 MiB,
// drivers ate ~150 MiB cabem sem precisar mexer nas page tables.
static PDRIVER_OBJECT driver_build_and_entry(const char* drv_name, const void* image, void** out_base, NTSTATUS* out_st) {
    void* entry = 0;
    void* base = 0;

    pe_info_t pi;
    if (!pe_parse(image, &pi)) {
        kputs("[io] PE invalido (.sys nao reconhecido)\n");
        if (out_st) *out_st = STATUS_UNSUCCESSFUL;
        return 0;
    }
    kputs("[io] PE info: machine="); kput_hex(pi.machine);
    kputs(" magic="); kput_hex(pi.magic);
    kputs(" SizeOfImage="); kput_hex(pi.size_image);
    kputs(" ImageBase preferido="); kput_hex(pi.preferred); kputc('\n');

    // 1 GiB e o limite da identidade. Se ImageBase preferido >= 1 GiB, precisamos
    // de uma base alternativa (com .reloc). Tambem se o image nao cabe no heap.
    int needs_relocate = (pi.preferred >= 0x40000000ULL);
    int too_big_for_heap = (pi.size_image > 0x800000u);   // heap so tem 16 MiB

    if (needs_relocate || too_big_for_heap) {
        // Aloca um bloco contiguo via PMM (frames de 4 KiB).
        uint64_t pages = (pi.size_image + 4095u) / 4096u;
        uint64_t alt   = pmm_alloc_contiguous(pages);
        if (!alt || alt >= 0x40000000ULL) {
            kputs("[io] sem RAM contigua p/ carregar o driver (");
            kput_dec(pi.size_image); kputs(" bytes)\n");
            if (out_st) *out_st = STATUS_NO_MEMORY;
            return 0;
        }
        kputs("[io] carregando driver em base alternativa @"); kput_hex(alt);
        kputs(" (.reloc sera aplicado)\n");
        base = pe_map_at(image, alt, &entry);
        if (!base || !entry) {
            kputs("[io] pe_map_at falhou\n");
            if (out_st) *out_st = STATUS_UNSUCCESSFUL;
            return 0;
        }
        uint32_t n = pe_relocate(base, pi.preferred);
        kputs("[io] relocacoes (.reloc) aplicadas: "); kput_dec(n); kputc('\n');
    } else {
        // Caminho original: carrega no ImageBase preferido (cabe na identidade).
        base = pe_map(image, &entry);
        if (!base || !entry) {
            kputs("[io] falha ao carregar o .sys\n");
            if (out_st) *out_st = STATUS_UNSUCCESSFUL;
            return 0;
        }
    }
    pe_bind_imports(base, ntkrnl_resolve);          // resolve imports do ntoskrnl

    // GATE 2 do pintok.sys: registra a base de carga REAL no s_mods[] do loader,
    // para ZwQuerySystemInformation(SystemModuleInformation/Ex) reportar este
    // .sys com ImageBase correto (drivers pintok.sys enumeram a si mesmos e ao
    // ntoskrnl pela lista de modulos, e depois validam por base/tamanho).
    ldr_module_set_base(drv_name, base);

    PDRIVER_OBJECT drv = (PDRIVER_OBJECT)ObCreateObject(OB_TYPE_DRIVER, sizeof(DRIVER_OBJECT), 0);
    if (!drv) { kputs("[io] sem memoria para o DRIVER_OBJECT\n"); if (out_st) *out_st = STATUS_UNSUCCESSFUL; return 0; }
    drv->Type        = 4;                       // IO_TYPE_DRIVER
    drv->Size        = (SHORT)sizeof(DRIVER_OBJECT);
    drv->DriverStart = base;
    drv->DriverInit  = entry;
    // FASE 7.11 (Hipotese B): preenche mais campos do DRIVER_OBJECT que drivers
    // reais inspecionam. DriverName UNICODE_STRING com \Driver\<Nome>.
    static WCHAR drv_name_buf[64];
    if (drv_name) {
        const char* bn = d_basename(drv_name);
        const char* prefix = "\\Driver\\";
        int i = 0; while (prefix[i] && i < 16) { drv_name_buf[i] = (WCHAR)prefix[i]; i++; }
        int j = 0; while (bn[j] && i < 60) { drv_name_buf[i++] = (WCHAR)bn[j]; j++; }
        if (i >= 4 && drv_name_buf[i-4] == '.' && drv_name_buf[i-3] == 's' &&
            drv_name_buf[i-2] == 'y' && drv_name_buf[i-1] == 's') i -= 4;
        drv_name_buf[i] = 0;
        drv->DriverName.Buffer = drv_name_buf;
        drv->DriverName.Length = (USHORT)(i * 2);
        drv->DriverName.MaximumLength = (USHORT)((i + 1) * 2);
    }
    // DriverExtension: aloca uma struct minima (so o back-pointer + zeros).
    // No NT real e DRIVER_EXTENSION com fields p/ AddDevice, ServiceKeyName etc.
    extern void* kmalloc(size_t n);
    typedef struct { PVOID DriverObject; PVOID AddDevice; uint64_t Count;
                     UNICODE_STRING ServiceKeyName; PVOID ClientDriverExtension; } DRV_EXT;
    DRV_EXT* dx = (DRV_EXT*)kmalloc(sizeof(DRV_EXT));
    if (dx) {
        for (unsigned k = 0; k < sizeof(DRV_EXT); k++) ((uint8_t*)dx)[k] = 0;
        dx->DriverObject = drv;
        dx->ServiceKeyName = drv->DriverName;
        drv->DriverExtension = dx;
    }
    drv->DriverSize = pi.size_image;

    // FASE 7.14: DriverObject->DriverSection = KLDR_DATA_TABLE_ENTRY minima. Drivers reais
    // (pintok.sys) leem DriverSection->FullDllName MUITO cedo p/ derivar o proprio diretorio de
    // imagem (monta o path do log: '%s%s\Logs\%s_%s%s') e fazem self-locate por DllBase/
    // SizeOfImage no bloco de integridade. Sem DriverSection o deref de NULL->FullDllName.Buffer
    // faulta (CR2~0x50), o PF-recovery mapeia pagina zero -> FullDllName vazia -> self-locate
    // falha -> bail 0xC0000365. Espelha o KLDR do emulador (DllBase/EntryPoint/SizeOfImage/
    // FullDllName='\SystemRoot\system32\drivers\<nome>' / BaseDllName='<nome>').
    {
        typedef struct { LIST_ENTRY InLoad, InMem, InInit; PVOID DllBase, EntryPoint;
                         uint64_t SizeOfImage; UNICODE_STRING FullDllName, BaseDllName; } KLDR_MIN;
        static WCHAR full_w[96]; static WCHAR base_w[40];
        const char* fp = "\\SystemRoot\\system32\\drivers\\";
        int fi = 0; while (fp[fi] && fi < 60) { full_w[fi] = (WCHAR)fp[fi]; fi++; }
        const char* bn = drv_name ? d_basename(drv_name) : "pintok.sys";
        int bi = 0; while (bn[bi] && fi < 94 && bi < 38) { full_w[fi++] = (WCHAR)bn[bi]; base_w[bi] = (WCHAR)bn[bi]; bi++; }
        full_w[fi] = 0; base_w[bi] = 0;
        KLDR_MIN* kldr = (KLDR_MIN*)kmalloc(sizeof(KLDR_MIN));
        if (kldr) {
            for (unsigned k = 0; k < sizeof(KLDR_MIN); k++) ((uint8_t*)kldr)[k] = 0;
            kldr->InLoad.Flink = kldr->InLoad.Blink = &kldr->InLoad;   // self-link (driver pode andar)
            kldr->InMem.Flink  = kldr->InMem.Blink  = &kldr->InMem;
            kldr->InInit.Flink = kldr->InInit.Blink = &kldr->InInit;
            kldr->DllBase = base; kldr->EntryPoint = entry; kldr->SizeOfImage = pi.size_image;
            kldr->FullDllName.Buffer = full_w;
            kldr->FullDllName.Length = (USHORT)(fi * 2);
            kldr->FullDllName.MaximumLength = (USHORT)((fi + 1) * 2);
            kldr->BaseDllName.Buffer = base_w;
            kldr->BaseDllName.Length = (USHORT)(bi * 2);
            kldr->BaseDllName.MaximumLength = (USHORT)((bi + 1) * 2);
            drv->DriverSection = kldr;
        }
    }

    // FASE 7.11: monta um RegistryPath valido para passar ao DriverEntry, como
    // faz o I/O Manager do NT real. Formato: \Registry\Machine\System\CurrentControlSet\Services\<NomeSemSys>
    // Hipotese (validada pela ausencia total de tracing): pintok bailout em
    // STATUS_NOT_FOUND porque RegistryPath->Buffer estava NULL. Vamos popular.
    static WCHAR reg_path_w[256];
    static const char* prefix = "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
    int pn = 0; while (prefix[pn]) pn++;
    int i;
    for (i = 0; i < pn && i < 250; i++) reg_path_w[i] = (WCHAR)prefix[i];
    // append o nome do driver sem o ".sys" final.
    int dn = 0; if (drv_name) { const char* nm = d_basename(drv_name);
        while (nm[dn] && dn < 32) { reg_path_w[i++] = (WCHAR)nm[dn]; dn++; }
        // remove ".sys" se presente
        if (i >= 4 && reg_path_w[i-4] == '.' && reg_path_w[i-3] == 's' &&
            reg_path_w[i-2] == 'y' && reg_path_w[i-1] == 's') i -= 4;
    }
    reg_path_w[i] = 0;
    UNICODE_STRING reg;
    reg.Buffer        = reg_path_w;
    reg.Length        = (USHORT)(i * 2);
    reg.MaximumLength = (USHORT)((i + 1) * 2);
    kputs("[io] RegistryPath = '");
    for (int k = 0; k < i; k++) kputc((char)reg_path_w[k]);
    kputs("'\n");

    // FASE 7.11 (Hipotese A): pre-cria \Registry\Machine\System\CurrentControlSet\
    // Services\<DriverName> com Type/Start/ErrorControl/ImagePath/Parameters.
    // Drivers reais que abrem essa chave dentro do DriverEntry agora encontram
    // a chave existente em vez de STATUS_OBJECT_NAME_NOT_FOUND.
    registry_create_driver_service(drv_name ? d_basename(drv_name) : "unknown");

    typedef NTSTATUS (__attribute__((ms_abi)) * driver_entry_t)(PDRIVER_OBJECT, PUNICODE_STRING);
    driver_entry_t DriverEntry = (driver_entry_t)entry;

    // FASE 7.10: tracer global. Liga durante DriverEntry; cada API suspeita
    // (HalCpuid, HalReadMsr, MmGetSystemRoutineAddress, registry, etc.) loga
    // sua chamada SOMENTE durante esta janela. Apos retornar, desliga. Assim
    // vemos exatamente quais APIs o driver tocou e a ULTIMA antes do bail.
    extern volatile int g_pintok_trace;
    // FASE 7.13: interceptacao de CPUID via single-step (Trap Flag). Antes
    // do DriverEntry liga g_intercept_cpuid=1 e RFLAGS.TF=1. Cada instrucao
    // do driver dispara #DB; o handler em src/cpu/isr.c reescreve EAX/EBX/
    // ECX/EDX se a instrucao foi CPUID, escondendo o vendor TCG e o bit
    // HypervisorPresent. Apos retorno, desliga TF e o interceptor.
    extern volatile int g_intercept_cpuid;
    extern uint64_t cpuid_intercept_count(void);
    extern uint64_t rdtsc_intercept_count(void);
    extern uint64_t rdmsr_intercept_count(void);
    // FASE 7.14: neutralizacao do bloco anti-VM (int 0x20/iretq/syscall) do pintok.sys.
    extern uint64_t antivm_neutralize_count(void);
    kputs("[io] chamando DriverEntry...\n");
    g_pintok_trace = 1;
    uint64_t antivm_before = antivm_neutralize_count();
    uint64_t cpuid_before = cpuid_intercept_count();
    g_intercept_cpuid = 1;
    // Liga TF na RFLAGS atual modificando o valor empilhado e re-popando.
    __asm__ volatile (
        "pushfq                \n"
        "orq $0x100, (%%rsp)   \n"   // TF (bit 8) = 1
        "popfq                 \n"
        ::: "memory", "cc"
    );
    NTSTATUS st = DriverEntry(drv, &reg);
    // Desliga TF antes de qualquer outra coisa (proxima instrucao C nao seria
    // single-stepada porque o IRETQ ja restaurou TF=1, mas garantimos aqui).
    __asm__ volatile (
        "pushfq                  \n"
        "andq $-257, (%%rsp)     \n"   // TF (bit 8) = 0
        "popfq                   \n"
        ::: "memory", "cc"
    );
    g_intercept_cpuid = 0;
    g_pintok_trace = 0;
    uint64_t cpuid_after = cpuid_intercept_count();
    uint64_t rdtsc_n    = rdtsc_intercept_count();
    uint64_t rdmsr_n    = rdmsr_intercept_count();
    uint64_t antivm_n   = antivm_neutralize_count() - antivm_before;
    if (cpuid_after > cpuid_before || rdtsc_n || rdmsr_n || antivm_n) {
        kputs("[io] intercept totals: CPUID x"); kput_dec(cpuid_after - cpuid_before);
        kputs("  RDTSC x"); kput_dec(rdtsc_n);
        kputs("  RDMSR x"); kput_dec(rdmsr_n);
        kputs("  ANTIVM x"); kput_dec(antivm_n);   // int/iretq/syscall neutralizados
        kputs("\n");
    }
    kputs("[io] DriverEntry retornou status="); kput_hex((uint32_t)st);
    kputs(st == STATUS_SUCCESS ? "  (STATUS_SUCCESS)\n" : "\n");

    if (drv->DeviceObject) kputs("[io] driver registrou device object(s).\n");
    if (out_base) *out_base = base;
    if (out_st)   *out_st   = st;
    return drv;
}

// Descricao curta dos NTSTATUS mais comuns que aparecem em DriverEntry,
// para o log ficar legivel sem precisar consultar tabela hex externa.
static const char* nt_status_desc(NTSTATUS st) {
    switch ((uint32_t)st) {
        case 0x00000000u: return "STATUS_SUCCESS";
        case 0xC0000001u: return "STATUS_UNSUCCESSFUL";
        case 0xC0000002u: return "STATUS_NOT_IMPLEMENTED";
        case 0xC0000005u: return "STATUS_ACCESS_VIOLATION";
        case 0xC000000Du: return "STATUS_INVALID_PARAMETER";
        case 0xC0000017u: return "STATUS_NO_MEMORY";
        case 0xC0000022u: return "STATUS_ACCESS_DENIED";
        case 0xC0000034u: return "STATUS_OBJECT_NAME_NOT_FOUND";
        case 0xC000009Au: return "STATUS_INSUFFICIENT_RESOURCES";
        case 0xC00000BBu: return "STATUS_NOT_SUPPORTED";
        case 0xC0000139u: return "STATUS_ENTRYPOINT_NOT_FOUND";
        case 0xC0000142u: return "STATUS_DLL_INIT_FAILED";
        case 0xC0000225u: return "STATUS_NOT_FOUND";
        case 0xC0000258u: return "STATUS_NOT_FOUND";
        case 0xC000010Au: return "STATUS_PROCESS_IS_TERMINATING";
        default:          return "STATUS_<desconhecido>";
    }
}

// Chama DriverUnload com validacao defensiva: rejeita ponteiros suspeitos
// (NULL ou abaixo de 0x1000, faixa onde so cabe lixo/page0). NT real tambem
// nunca pula direto para um endereco invalido — falha cedo e silenciosa.
static void driver_call_unload(PDRIVER_OBJECT drv) {
    if (!drv) return;
    void* pUnload = drv->DriverUnload;
    if (!pUnload) {
        // Sem DriverUnload registrado: comportamento normal, nao loga ruido.
        return;
    }
    if ((uint64_t)pUnload < 0x1000ULL) {
        kputs("[io] AVISO: DriverUnload com ponteiro suspeito (");
        kput_hex((uint64_t)pUnload);
        kputs("), pulando chamada para evitar #UD/#GP.\n");
        return;
    }
    typedef void (__attribute__((ms_abi)) * unload_t)(PDRIVER_OBJECT);
    kputs("[io] chamando DriverUnload @"); kput_hex((uint64_t)pUnload); kputc('\n');
    ((unload_t)pUnload)(drv);
}

// BOOT: carrega o .sys, chama DriverEntry e — APENAS SE retornou STATUS_SUCCESS —
// chama DriverUnload em seguida. Registra o driver como STOPPED (pronto p/ 'sc start').
//
// IMPORTANTE: no NT real, se DriverEntry falha, o I/O Manager descarta o driver
// SEM chamar DriverUnload (o driver pode nao ter completado a inicializacao do
// proprio Unload, e chama-lo nesse estado tende a crashar — exatamente o que
// estava acontecendo com pintok.sys, que retornava STATUS_NOT_FOUND e ainda
// assim tinha o "Unload" invocado, batendo em rip=0x3 com #UD).
void driver_load(const char* name, const void* image) {
    void* base = 0; NTSTATUS st = STATUS_UNSUCCESSFUL;
    PDRIVER_OBJECT drv = driver_build_and_entry(name, image, &base, &st);

    // Log destacado do STATUS retornado por DriverEntry (mesma fase do NT real).
    kputs("[io] DriverEntry status="); kput_hex((uint32_t)st);
    kputs(" "); kputs(nt_status_desc(st)); kputc('\n');

    DRV_ENTRY* e = drv_intern(name);
    if (e) { e->laststatus = st; e->state = DRV_STATE_STOPPED; e->object = 0; e->base = 0; }

    if (drv && st == STATUS_SUCCESS) {
        // TESTE REAL: com o driver carregado e o DriverEntry ja retornado (kernel
        // em modo real, g_pintok_trace=0), exercita I/O de verdade no device que
        // ele criou — ANTES do Unload apagar o device. Prova que um driver Windows
        // real PROCESSA IRP (write/read chegam ao dispatch e sao completados), nao
        // so carrega. No-op se o driver nao criou device. (pintok nunca cai aqui:
        // ele retorna != STATUS_SUCCESS, entao seu caminho fica intocado.)
        extern void KiExerciseDriverIO(PDRIVER_OBJECT);
        KiExerciseDriverIO(drv);

        // Sucesso: NT chamaria DriverUnload aqui se este fosse caminho de teste.
        // Mantemos o comportamento atual (descarrega no boot p/ deixar STOPPED).
        driver_call_unload(drv);
    } else if (drv) {
        // Falha: descarta sem chamar Unload (semantica NT correta).
        kputs("[io] DriverEntry falhou — DriverUnload NAO sera chamado (driver descartado).\n");
    }
    kputs("[io] driver finalizado.\n");
}

// 'sc start <nome>': carrega pelo nome (via loader) e deixa RODANDO.
NTSTATUS driver_load_by_name(const char* name) {
    DRV_ENTRY* e = drv_find(name);
    if (e && e->state == DRV_STATE_RUNNING) {
        kputs("[io] driver ja esta rodando: "); kputs(name); kputc('\n');
        return STATUS_SUCCESS;
    }
    const void* image = ldr_get_module_bytes(name);
    if (!image) { kputs("[io] driver nao registrado: "); kputs(name); kputc('\n'); return STATUS_UNSUCCESSFUL; }

    kputs("[io] sc start: carregando driver '"); kputs(name); kputs("'...\n");
    void* base = 0; NTSTATUS st = STATUS_UNSUCCESSFUL;
    PDRIVER_OBJECT drv = driver_build_and_entry(name, image, &base, &st);
    if (!drv) return st;

    e = drv_intern(name);
    if (e) {
        e->laststatus = st;
        if (st == STATUS_SUCCESS) { e->state = DRV_STATE_RUNNING; e->object = drv; e->base = base; }
        else                      { e->state = DRV_STATE_STOPPED; e->object = 0; e->base = 0; }
    }
    // 'sc start' NAO chama DriverUnload: o driver fica rodando (RUNNING) ate 'sc stop'.
    return st;
}

// 'sc stop <nome>': chama o DriverUnload e marca STOPPED.
NTSTATUS driver_unload_by_name(const char* name) {
    DRV_ENTRY* e = drv_find(name);
    if (!e) { kputs("[io] driver desconhecido: "); kputs(name); kputc('\n'); return STATUS_UNSUCCESSFUL; }
    if (e->state != DRV_STATE_RUNNING) {
        kputs("[io] driver nao esta rodando: "); kputs(name); kputc('\n');
        return STATUS_UNSUCCESSFUL;
    }
    kputs("[io] sc stop: descarregando driver '"); kputs(name); kputs("'...\n");
    driver_call_unload(e->object);
    e->state = DRV_STATE_STOPPED;
    e->object = 0;
    return STATUS_SUCCESS;
}

int driver_enum(int index, const char** name, uint32_t* state, NTSTATUS* laststatus) {
    if (index < 0 || index >= s_ndrivers) return 0;
    DRV_ENTRY* e = &s_drivers[index];
    if (name)       *name       = e->name;
    if (state)      *state      = e->state;
    if (laststatus) *laststatus = e->laststatus;
    return 1;
}
