#include <stdint.h>
#include "ke/amd64/syscall.h"
#include "win32/win32.h"
#include "win32/win32k.h"
#include "ob/object.h"
#include "io/io.h"
#include "ps/process.h"
#include "lpc/pipe.h"
#include "io/driver.h"
#include "ldr/loader.h"
#include "ldr/pe.h"
#include "mm/pmm.h"
#include "input/keyboard.h"
#include "filesystems/ntfs/ntfs.h"
#include "hal/cpu.h"            // HalReadMsr / HalWriteMsr (salvar/restaurar GS base)
#include "ke/amd64/kpcr.h"     // MSR_IA32_GS_BASE
#include "ke/amd64/gdt.h"      // tss_get_rsp0 / tss_set_rsp0 (segunda pilha de kernel)
#include "mm/paging.h"         // mm_get_cr3 / mm_switch_cr3
#include "ke/amd64/usermode.h" // usermode_set_cmdline (argv do filho, Fase 4c)

// Device de volume NTFS (ntfs_fs.c) — para rotear NtCreateFile/Read/QueryDir.
PDEVICE_OBJECT ntfs_fs_volume_device(void);

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern void* kmalloc(uint64_t n);   // heap (tambem externado junto ao sys_loadlibrary)
extern void  kfree(void* p);

void* g_user_exit[5];

// ---- Numeros de syscall (SSDT). Devem casar com dll/ntdll.c. ----
enum {
    SYS_WRITE = 1, SYS_EXIT = 2, SYS_MESSAGEBOX = 3,
    SYS_CREATEFILE = 4, SYS_DEVICEIOCONTROL = 5, SYS_CLOSE = 6,
    SYS_CREATEPROCESS = 7, SYS_CREATETHREAD = 8,
    SYS_TERMINATEPROCESS = 9, SYS_WAITFORSINGLEOBJECT = 10,
    SYS_WRITEFILE = 11, SYS_READFILE = 12,
    SYS_OPENKEY = 13, SYS_QUERYVALUEKEY = 14,
    SYS_GETMODULEHANDLE = 15, SYS_GETPROCADDRESS = 16,
    // --- FASE 2: win32k (janelas + mensagens + GDI). Numeros novos >=17. ---
    SYS_USERREGISTERCLASS = 17, SYS_USERCREATEWINDOWEX = 18,
    SYS_USERDESTROYWINDOW = 19, SYS_USERSHOWWINDOW = 20,
    SYS_USERGETMESSAGE = 21, SYS_USERDISPATCHMESSAGE = 22,
    SYS_USERPOSTMESSAGE = 23, SYS_USERPOSTQUITMESSAGE = 24,
    SYS_USERGETDC = 25, SYS_USERINVALIDATE = 26,
    SYS_GDIGETSTOCKOBJECT = 27, SYS_GDICREATESOLIDBRUSH = 28,
    SYS_GDITEXTOUT = 29, SYS_GDIFILLRECT = 30,
    // --- FASE 3: Named Pipes (IPC). Numeros novos >=31. ---
    SYS_CREATENAMEDPIPE = 31, SYS_CONNECTNAMEDPIPE = 32,
    // --- FASE 4: syscalls de informacao (NtQuery*). Numeros novos >=33. ---
    SYS_QUERYSYSTEMINFO = 33, SYS_QUERYINFORMATIONPROCESS = 34,
    SYS_READVIRTUALMEMORY = 35, SYS_WRITEVIRTUALMEMORY = 36,
    // --- FASE 5: shell cmd.exe (enumerar objetos + carregar/descarregar driver). ---
    SYS_ENUMPROCESSES = 37, SYS_ENUMDRIVERS = 38,
    SYS_LOADDRIVER = 39, SYS_UNLOADDRIVER = 40,
    // --- FASE 6: desktop + barra de tarefas + cmd numa janela. ---
    SYS_USERSETFOCUS = 41, SYS_USERPOSTKEY = 42, SYS_GDITEXTOUTEX = 43,
    // --- FASE 3 (NTFS): listar diretorio via I/O Manager. Numero novo >=44. ---
    SYS_QUERYDIRECTORYFILE = 44,
    // --- FASE 5: info de volume (rotulo/serial/tamanho/fs name). Numero novo >=45. ---
    SYS_QUERYVOLUMEINFORMATION = 45,
    // --- FASE 11: cursor do mouse (le/ajusta posicao). ---
    SYS_USERGETCURSORPOS = 46,
    SYS_USERSETCURSORPOS = 47,
    // --- FASE 3f: LoadLibrary em runtime (carrega DLL registrada, devolve a base). ---
    SYS_LOADLIBRARY = 48,
    // --- Frente C (explorer real): tempo p/ o usermode (le o KUSER_SHARED_DATA). ---
    SYS_QUERYSYSTEMTIME = 49,
    // --- Frente C (explorer real): VirtualAlloc (backing do HeapAlloc). ---
    SYS_VIRTUALALLOC = 50,
};

// "Console device": handles padrao do Win32 (GetStdHandle). Sao pseudo-handles
// (valores sentinela), como no NT, e nao passam pela handle table do Object
// Manager. NtWriteFile para um destes escreve na saida do kernel (VGA+serial).
//   STD_INPUT_HANDLE  = -10, STD_OUTPUT_HANDLE = -11, STD_ERROR_HANDLE = -12
#define STD_HANDLE_OUTPUT ((uintptr_t)(intptr_t)-11)
#define STD_HANDLE_ERROR  ((uintptr_t)(intptr_t)-12)
#define STD_HANDLE_INPUT  ((uintptr_t)(intptr_t)-10)

static int is_console_handle(uintptr_t h) {
    return h == STD_HANDLE_OUTPUT || h == STD_HANDLE_ERROR || h == STD_HANDLE_INPUT;
}

// ---- Serviços (cada um lê os argumentos dos registradores salvos) ----
static void sys_write(struct regs* r) {
    kputs((const char*)(uintptr_t)r->rdi);
    r->rax = 0;
}
static void sys_messagebox(struct regs* r) {
    win32k_messagebox((const char*)(uintptr_t)r->rdi, (const char*)(uintptr_t)r->rsi);
    r->rax = 1;   // IDOK
}
static void sys_exit(struct regs* r) {
    (void)r;
    __builtin_longjmp(g_user_exit, 1);
}

// Reconhece um nome de Named Pipe (qualquer das formas Win32/NT).
static char sc_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  sc_starts(const char* s, const char* p) {
    if (!s) return 0;
    while (*p) { if (sc_lower(*s) != sc_lower(*p)) return 0; s++; p++; }
    return 1;
}
static int is_pipe_name(const char* name) {
    return sc_starts(name, "\\\\.\\pipe\\") ||
           sc_starts(name, "\\Pipe\\")       ||
           sc_starts(name, "\\Device\\NamedPipe\\");
}

// FASE 5: o NTFS e montado como a unidade C:. Um caminho do usuario pode vir
// como "C:\hello.txt" / "C:hello.txt" / "C:" (forma DOS, do cmd.exe) OU como o
// caminho NT cru "\Device\Harddisk0\Partition1\hello.txt". Esta funcao reconhece
// AMBOS e devolve o restante do caminho NTFS (ex.: "\hello.txt"), normalizando
// barras para '\'. Para evitar realocacao, escreve a forma normalizada num buffer
// estatico (single-threaded; suficiente sem escalonador). Devolve 0 se 'name' nao
// se refere ao volume.
static char s_subpath_buf[260];

static const char* ntfs_volume_subpath(const char* name) {
    if (!name) return 0;
    const char* rest = 0;

    // (1) Drive letter C: (case-insensitive). "C:" / "C:\..." / "C:foo".
    if ((name[0] == 'C' || name[0] == 'c') && name[1] == ':') {
        rest = name + 2;
    } else {
        // (2) Caminho NT cru \Device\Harddisk0\Partition1[\...]
        const char* dev = NTFS_VOLUME_DEVICE;   // "\\Device\\Harddisk0\\Partition1"
        const char* s = name;
        while (*dev) { if (sc_lower(*s) != sc_lower(*dev)) return 0; s++; dev++; }
        rest = s;
    }

    // Normaliza o restante para "\..." (raiz = "\"); aceita '/' como separador.
    int i = 0;
    if (*rest != '\\' && *rest != '/') s_subpath_buf[i++] = '\\';  // garante raiz
    while (*rest && i < (int)sizeof(s_subpath_buf) - 1) {
        char c = *rest++;
        s_subpath_buf[i++] = (c == '/') ? '\\' : c;
    }
    s_subpath_buf[i] = 0;
    // "C:" sozinho vira "\" (raiz); "C:\" idem.
    if (s_subpath_buf[0] == 0) { s_subpath_buf[0] = '\\'; s_subpath_buf[1] = 0; }
    return s_subpath_buf;
}

// NtCreateFile(out HANDLE*, const char* name) — abre um dispositivo do namespace,
// um Named Pipe (\\.\pipe\Nome), OU um arquivo/diretorio do volume NTFS
// (\Device\Harddisk0\Partition1\caminho): resolve o caminho -> registro MFT e
// devolve um FILE_OBJECT ligado ao device de volume (FsContext = MFT).
static void sys_createfile(struct regs* r) {
    HANDLE* out = (HANDLE*)(uintptr_t)r->rdi;
    const char* name = (const char*)(uintptr_t)r->rsi;

    // --- Volume NTFS (\Device\Harddisk0\Partition1[\caminho]) ---
    const char* nt_path = ntfs_fs_registered() ? ntfs_volume_subpath(name) : 0;
    if (nt_path) {
        NTFS_FILE_INFO fi;
        if (!ntfs_resolve_path(nt_path, &fi)) {
            if (out) *out = 0;
            r->rax = (uint64_t)0xC0000034u;   // STATUS_OBJECT_NAME_NOT_FOUND
            return;
        }
        PDEVICE_OBJECT vol = ntfs_fs_volume_device();
        PFILE_OBJECT f = (PFILE_OBJECT)ObCreateObject(OB_TYPE_FILE, sizeof(FILE_OBJECT), 0);
        if (!f || !vol) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
        f->DeviceObject = vol;
        f->PipeObject = 0;
        f->FsContext = fi.mft_record;
        f->CurrentByteOffset = 0;
        f->IsDirectory = (uint32_t)fi.is_dir;
        // Aponta o contexto do volume para este alvo (read/dir control miram nele).
        ntfs_fs_set_target(fi.mft_record, fi.is_dir);
        // Avisa o driver (IRP_MJ_CREATE), como no NtCreateFile de um device.
        PIRP irp = io_build_request(IRP_MJ_CREATE, vol);
        if (irp) { IoCallDriver(vol, irp); io_free_irp(irp); }
        HANDLE h = ob_create_handle(f);
        if (out) *out = h;
        kputs("[ntfs.sys] NtCreateFile: '"); kputs(name);
        kputs("' -> MFT #"); kput_hex(fi.mft_record);
        kputs(fi.is_dir ? " (DIR)\n" : " (FILE)\n");
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }

    // --- Named Pipe (lado cliente): CreateFileA("\\.\pipe\Nome") ---
    if (is_pipe_name(name)) {
        PPIPE_OBJECT pipe = pipe_open(name);
        if (!pipe) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
        PFILE_OBJECT f = (PFILE_OBJECT)ObCreateObject(OB_TYPE_FILE, sizeof(FILE_OBJECT), 0);
        if (!f) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
        f->DeviceObject = 0;
        f->PipeObject   = pipe;
        HANDLE h = ob_create_handle(f);
        if (out) *out = h;
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }

    void* dev = ObLookupObject(name);
    if (!dev || ob_type_of(dev) != OB_TYPE_DEVICE) {
        if (out) *out = 0;
        r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL;
        return;
    }
    PFILE_OBJECT f = (PFILE_OBJECT)ObCreateObject(OB_TYPE_FILE, sizeof(FILE_OBJECT), 0);
    if (!f) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    f->DeviceObject = (PDEVICE_OBJECT)dev;

    PIRP irp = io_build_request(IRP_MJ_CREATE, f->DeviceObject);   // avisa o driver (open)
    if (irp) { IoCallDriver(f->DeviceObject, irp); io_free_irp(irp); }

    HANDLE h = ob_create_handle(f);
    if (out) *out = h;
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtDeviceIoControlFile(handle, ioctl, inBuf, inLen, outBuf, outLen)
static void sys_deviceiocontrol(struct regs* r) {
    HANDLE h    = (HANDLE)(uintptr_t)r->rdi;
    ULONG  code = (ULONG)r->rsi;
    void*  inB  = (void*)(uintptr_t)r->rdx;
    ULONG  inL  = (ULONG)r->r10;
    void*  outB = (void*)(uintptr_t)r->r8;
    ULONG  outL = (ULONG)r->r9;

    PFILE_OBJECT f = (PFILE_OBJECT)ob_handle_to_object(h, OB_TYPE_FILE);
    if (!f || !f->DeviceObject) { r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }

    PIRP irp = io_build_ioctl(code, f->DeviceObject, inB, inL, outB, outL);
    if (!irp) { r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    IoCallDriver(f->DeviceObject, irp);

    // METHOD_BUFFERED: devolve a saida do SystemBuffer para o buffer do usuario.
    uint64_t info = irp->IoStatus.Information;
    if (outB && irp->AssociatedIrp.SystemBuffer && info) {
        uint64_t n = info < outL ? info : outL;
        for (uint64_t i = 0; i < n; i++)
            ((uint8_t*)outB)[i] = ((uint8_t*)irp->AssociatedIrp.SystemBuffer)[i];
    }
    NTSTATUS st = irp->IoStatus.Status;
    io_free_irp(irp);
    r->rax = (uint64_t)(uint32_t)st;
}

static void sys_close(struct regs* r) {
    HANDLE h = (HANDLE)(uintptr_t)r->rdi;
    PFILE_OBJECT f = (PFILE_OBJECT)ob_handle_to_object(h, OB_TYPE_FILE);
    if (f && f->DeviceObject) {
        PIRP irp = io_build_request(IRP_MJ_CLOSE, f->DeviceObject);
        if (irp) { IoCallDriver(f->DeviceObject, irp); io_free_irp(irp); }
    }
    ob_close_handle(h);
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// ---- Process Manager (Ps*) ----
// FRENTE 4 (Fase 4a) — roda a imagem de um FILHO de forma SINCRONA e ANINHADA dentro
// do syscall do PAI. Reusa o caminho provado ldr_run -> usermode_enter, mas o filho
// reusa endereços FIXOS (pilha/TEB/PEB em 0x600000, o setjmp global g_user_exit, o
// gs-base) e a UNICA pilha de kernel (TSS.RSP0). Salvamos e restauramos tudo que o
// filho "pisa", senao o PAI volta corrompido:
//   1) TSS.RSP0 -> uma SEGUNDA pilha de kernel para o filho. Sem isso, o int 0x80 do
//      filho recarrega o topo da pilha de kernel COMPARTILHADA e sobrescreve o trap
//      frame do pai -> o pai NUNCA volta (falha deterministica);
//   2) g_user_exit[5] (o buffer setjmp/longjmp global; o setjmp do filho o sobrescreve);
//   3) a janela [0x600000,0x700000) = pilha + TEB + PEB do pai (o filho a reusa);
//   4) CR3 (mapa de memoria do pai);
//   5) IA32_GS_BASE (a saida do filho troca para o KPCR; o pai quer o seu TEB);
//   + FPU/SSE/MXCSR via fxsave/fxrstor.
// Vazamentos conhecidos (nao travam; enderecados depois): o CR3 do filho, a regiao de
// imagem contigua e os objetos EPROCESS nao sao liberados.
static void run_child_image(const char* name, const void* bytes, const char* cmdline) {
    const uint64_t UWIN_BASE = 0x600000ULL;   // base da janela pilha/TEB/PEB do pai
    const uint64_t UWIN_SIZE = 0x100000ULL;   // 1 MiB
    const uint64_t KCHILD    = 0x8000ULL;     // 32 KiB: pilha de kernel do filho

    void* kstk = kmalloc(KCHILD);
    if (!kstk) { kputs("[ps] createprocess: sem RAM p/ pilha de kernel do filho\n"); return; }
    void* win  = kmalloc(UWIN_SIZE);
    if (!win)  { kfree(kstk); kputs("[ps] createprocess: sem RAM p/ salvar a janela do pai\n"); return; }
    uint64_t kstk_top = ((uint64_t)(uintptr_t)kstk + KCHILD) & ~0xFULL;

    // ---- SALVA o estado do pai ----
    uint8_t fxarea[512] __attribute__((aligned(16)));
    __asm__ volatile ("fxsave (%0)" : : "r"(fxarea) : "memory");
    void* saved_jmp[5];
    for (int i = 0; i < 5; i++) saved_jmp[i] = g_user_exit[i];
    { volatile uint64_t* s = (volatile uint64_t*)(uintptr_t)UWIN_BASE;
      volatile uint64_t* d = (volatile uint64_t*)win;
      for (uint64_t i = 0; i < UWIN_SIZE / 8; i++) d[i] = s[i]; }
    uint64_t saved_cr3  = mm_get_cr3();
    uint64_t saved_gs   = HalReadMsr(MSR_IA32_GS_BASE);
    uint64_t saved_rsp0 = tss_get_rsp0();

    // ---- Troca a pilha de kernel e RODA o filho (sincrono; sai via longjmp) ----
    tss_set_rsp0(kstk_top);
    usermode_set_cmdline(cmdline);   // build_teb_peb do filho grava a cmdline em 0x601800
    kputs("[ps] createprocess: rodando o filho '"); kputs(name); kputs("' (sincrono)...\n");
    ldr_run(name, bytes);
    usermode_set_cmdline(0);          // limpa p/ o proximo processo (boot/outro filho) nao herdar

    // ---- RESTAURA o estado do pai ----
    tss_set_rsp0(saved_rsp0);
    { volatile uint64_t* s = (volatile uint64_t*)win;
      volatile uint64_t* d = (volatile uint64_t*)(uintptr_t)UWIN_BASE;
      for (uint64_t i = 0; i < UWIN_SIZE / 8; i++) d[i] = s[i]; }
    for (int i = 0; i < 5; i++) g_user_exit[i] = saved_jmp[i];
    mm_switch_cr3(saved_cr3);
    HalWriteMsr(MSR_IA32_GS_BASE, saved_gs);
    __asm__ volatile ("fxrstor (%0)" : : "r"(fxarea) : "memory");
    kputs("[ps] createprocess: filho terminou; pai restaurado.\n");

    kfree(win);
    kfree(kstk);
}

// NtCreateProcess(out HANDLE*, const char* image_name) — cria um EPROCESS (objeto +
// handle) e, na Fase 4a, RODA a imagem do filho de verdade (antes so criava o objeto).
static void sys_createprocess(struct regs* r) {
    HANDLE*     out     = (HANDLE*)(uintptr_t)r->rdi;
    const char* name    = (const char*)(uintptr_t)r->rsi;
    const char* cmdline = (const char*)(uintptr_t)r->rdx;   // Fase 4c: linha de comando (argv)
    uint64_t cr3; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    // (1) Objeto EPROCESS + handle (o handle devolvido ao pai). Inalterado.
    PEPROCESS p = PsCreateProcess(name, 0, cr3);
    if (!p) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    HANDLE h = ob_create_handle(p);
    if (out) *out = h;

    // (2) Resolve os bytes da imagem do filho e RODA. Fase 4b: se o nome for um caminho
    //     de DISCO (C:\... ou \Device\Harddisk0\...), le o .exe do volume NTFS (mesma via
    //     do sys_loadlibrary); senao (Fase 4a) usa um modulo de boot pelo nome. Se nao
    //     houver a imagem, mantem o comportamento antigo (so o objeto).
    const char* nt = ntfs_fs_registered() ? ntfs_volume_subpath(name) : 0;
    if (nt) {
        NTFS_FILE_INFO fi;
        if (ntfs_resolve_path(nt, &fi) && !fi.is_dir && fi.size) {
            void* buf = kmalloc(fi.size);
            if (buf) {
                if (ntfs_read_file(fi.mft_record, 0, buf, (uint32_t)fi.size) == (uint32_t)fi.size) {
                    kputs("[ps] createprocess: lendo o filho do DISCO NTFS: '"); kputs(name); kputs("'\n");
                    run_child_image(name, buf, cmdline);
                }
                kfree(buf);   // ldr_run ja copiou a imagem (pe_map); o buffer nao e mais preciso
            }
        }
    } else {
        const void* bytes = ldr_get_module_bytes(name);
        if (bytes) run_child_image(name, bytes, cmdline);
    }

    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtCreateThread(out HANDLE*, HANDLE process, void* start) — cria um ETHREAD
// no processo dado (ou no processo corrente se process==0). Nao agenda ainda.
// FRENTE THREADS — tabela de workers ring-3 PENDENTES. CreateThread(start,param) registra
// aqui; o 1o WaitForSingleObject sobre o handle do worker RODA o threadproc (usermode_run_
// worker), que vira a thread ring-3 corrente (modelo cooperativo — ver usermode.c). Assim o
// explorer real, cujo wWinMain bloqueia no ~WorkerWindow em WaitForSingleObject(worker_thread),
// PERSISTE em vez de sair. Pequena (o explorer cria 1 worker no caminho atual).
static struct { HANDLE h; uint64_t start; uint64_t param; int ran; } g_ring3_threads[8];
static int g_ring3_count = 0;

static void sys_createthread(struct regs* r) {
    HANDLE*  out   = (HANDLE*)(uintptr_t)r->rdi;
    HANDLE   hproc = (HANDLE)(uintptr_t)r->rsi;
    uint64_t start = (uint64_t)r->rdx;
    uint64_t param = (uint64_t)r->r10;   // NOVO: 4o arg (lpParameter do CreateThread)

    PEPROCESS p = hproc ? (PEPROCESS)ob_handle_to_object(hproc, OB_TYPE_PROCESS)
                        : PsGetCurrentProcess();
    if (!p) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }

    PETHREAD t = PsCreateThread(p, start, 0, 0);
    if (!t) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    HANDLE h = ob_create_handle(t);
    if (out) *out = h;
    // Registra o worker ring-3 (rodado sob demanda no 1o WaitForSingleObject sobre 'h').
    if (start && g_ring3_count < 8) {
        g_ring3_threads[g_ring3_count].h     = h;
        g_ring3_threads[g_ring3_count].start = start;
        g_ring3_threads[g_ring3_count].param = param;
        g_ring3_threads[g_ring3_count].ran   = 0;
        g_ring3_count++;
        kputs("[thread] CreateThread REGISTRADO: start="); kput_hex(start);
        kputs(" param="); kput_hex(param); kputs(" handle="); kput_hex((uint64_t)(uintptr_t)h); kputc('\n');
    }
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtTerminateProcess(HANDLE process, uint32_t status). Se process==0, encerra
// o processo corrente: comporta-se como o "exit" (volta ao kernel via longjmp).
static void sys_terminateprocess(struct regs* r) {
    HANDLE   hproc  = (HANDLE)(uintptr_t)r->rdi;
    uint32_t status = (uint32_t)r->rsi;

    if (hproc == 0) {
        PEPROCESS cur = PsGetCurrentProcess();
        if (cur) PsTerminateProcess(cur, status);
        __builtin_longjmp(g_user_exit, 1);   // nao retorna
    }
    PEPROCESS p = (PEPROCESS)ob_handle_to_object(hproc, OB_TYPE_PROCESS);
    if (!p) { r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    PsTerminateProcess(p, status);
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtWaitForSingleObject(HANDLE, uint32_t timeout_ms). Sem escalonador ainda:
// retorna imediatamente STATUS_SUCCESS (objeto considerado "sinalizado").
extern void usermode_run_worker(uint64_t start, uint64_t param);   // usermode.c

static void sys_waitforsingleobject(struct regs* r) {
    HANDLE h = (HANDLE)(uintptr_t)r->rdi;
    (void)r->rsi;   // timeout ignorado por ora
    // FRENTE THREADS: se e' o handle de um worker ring-3 ainda NAO rodado, RODA o threadproc
    // agora — ele vira a thread ring-3 corrente e NAO retorna (a thread principal fica "presa"
    // aqui, exatamente como o WaitForSingleObject(worker_thread, INFINITE) do ~WorkerWindow do
    // explorer). Efeito: o processo PERSISTE (nao chega ao DestroyWindow/ExitProcess).
    for (int i = 0; i < g_ring3_count; i++) {
        if (g_ring3_threads[i].h == h && !g_ring3_threads[i].ran && g_ring3_threads[i].start) {
            g_ring3_threads[i].ran = 1;
            kputs("[thread] WaitForSingleObject sobre worker -> rodando threadproc ring-3 (processo persiste)\n");
            usermode_run_worker(g_ring3_threads[i].start, g_ring3_threads[i].param);  // NAO retorna
            return;   // (defensivo) se voltar, cai no sucesso abaixo
        }
    }
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// FASE 3f — NtLoadLibrary(name): carrega uma DLL JA REGISTRADA (modulo de boot) sob
// demanda e devolve a base (HMODULE). Reaproveita o ldr_load do loader (mapeia + binda
// os imports no espaco do usuario corrente; devolve a base cacheada se ja carregada).
// GetProcAddress (LdrGetProcAddress) resolve os exports por cima. Nao le do disco por
// ora: so modulos passados no boot (Multiboot). rdi=nome; rax=base (0 se falhou).
extern void* ldr_load(const char* name);
extern void* ldr_load_runtime(const char* name);   // apiset_redirect + anexa ".dll"
extern void* ldr_load_image(const char* name, const void* bytes);
extern void* kmalloc(uint64_t n);
static void sys_loadlibrary(struct regs* r) {
    const char* name = (const char*)(uintptr_t)r->rdi;
    if (!name) { r->rax = 0; return; }
    // (1) Caminho de DISCO (C:\... ou \Device\Harddisk0\...): FASE 3g — le a DLL do
    // volume NTFS (data run resident OU nao-residente) p/ um buffer de kernel e mapeia
    // a partir dos bytes lidos. Prova LoadLibrary a partir de um ARQUIVO no disco.
    const char* nt = ntfs_fs_registered() ? ntfs_volume_subpath(name) : 0;
    if (nt) {
        NTFS_FILE_INFO fi;
        if (ntfs_resolve_path(nt, &fi) && !fi.is_dir && fi.size) {
            void* buf = kmalloc(fi.size);
            if (buf && ntfs_read_file(fi.mft_record, 0, buf, (uint32_t)fi.size) == (uint32_t)fi.size) {
                void* base = ldr_load_image(name, buf);   // ldr_load_image usa basename(name)
                kputs("[ldr] LoadLibrary DO DISCO: '"); kputs(name);
                kputs("' -> base "); kput_hex((uint64_t)(uintptr_t)base); kputc('\n');
                r->rax = (uint64_t)(uintptr_t)base;
                return;
            }
        }
        r->rax = 0;   // nao achou/nao leu o arquivo no disco
        return;
    }
    // (2) Modulo de boot registrado. Usa ldr_load_runtime: aplica apiset_redirect (contratos
    //     api-ms-win-*/kernelbase) e anexa ".dll" a nomes sem extensao (LoadLibraryW(L"dui70")).
    void* base = ldr_load_runtime(name);
    r->rax = (uint64_t)(uintptr_t)base;
}

// NtWriteFile(handle, const void* buf, uint32_t len, uint32_t* written).
// Console device: escreve 'len' bytes na saida do kernel (VGA+serial). Para um
// handle de arquivo do Object Manager (NtCreateFile), encaminha ao driver via
// IRP_MJ_WRITE (METHOD_BUFFERED). Retorna STATUS_SUCCESS e *written = bytes.
static void sys_writefile(struct regs* r) {
    uintptr_t   h     = (uintptr_t)r->rdi;
    const char* buf   = (const char*)(uintptr_t)r->rsi;
    uint32_t    len   = (uint32_t)r->rdx;
    uint32_t*   wrote = (uint32_t*)(uintptr_t)r->r10;

    if (is_console_handle(h)) {
        for (uint32_t i = 0; i < len; i++) kputc(buf ? buf[i] : 0);
        if (wrote) *wrote = len;
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }

    // Handle de arquivo/dispositivo/pipe.
    PFILE_OBJECT f = (PFILE_OBJECT)ob_handle_to_object((HANDLE)h, OB_TYPE_FILE);
    if (!f) {
        if (wrote) *wrote = 0;
        r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL;
        return;
    }
    // Named Pipe: escreve no buffer circular do pipe.
    if (f->PipeObject) {
        uint32_t n = pipe_write((PPIPE_OBJECT)f->PipeObject, buf, len);
        if (wrote) *wrote = n;
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }
    if (!f->DeviceObject) {
        if (wrote) *wrote = 0;
        r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL;
        return;
    }
    PIRP irp = io_build_write(f->DeviceObject, (void*)buf, len);
    if (!irp) { if (wrote) *wrote = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    // Volume NTFS: passa o registro MFT (Key) e o offset corrente p/ o driver de FS
    // (IRP_MJ_WRITE -> ntfs_write_file). Escrita sequencial: avanca o cursor.
    if (f->DeviceObject == ntfs_fs_volume_device() && f->FsContext) {
        IoGetNextIrpStackLocation(irp)->Parameters.Write.Key        = (ULONG)f->FsContext;
        IoGetNextIrpStackLocation(irp)->Parameters.Write.ByteOffset = f->CurrentByteOffset;
    }
    IoCallDriver(f->DeviceObject, irp);
    uint64_t info = irp->IoStatus.Information;
    NTSTATUS st = irp->IoStatus.Status;
    io_free_irp(irp);
    if (f->DeviceObject == ntfs_fs_volume_device()) f->CurrentByteOffset += info;  // avanca (write sequencial)
    if (wrote) *wrote = (uint32_t)info;
    r->rax = (uint64_t)(uint32_t)st;
}

// NtReadFile(handle, void* buf, uint32_t len, uint32_t* read). Console device:
// sem teclado bufferizado por ora -> le 0 bytes (EOF) com STATUS_SUCCESS.
// Handle de arquivo: encaminha ao driver via IRP_MJ_READ (METHOD_BUFFERED).
static void sys_readfile(struct regs* r) {
    uintptr_t h    = (uintptr_t)r->rdi;
    void*     buf  = (void*)(uintptr_t)r->rsi;
    uint32_t  len  = (uint32_t)r->rdx;
    uint32_t* read = (uint32_t*)(uintptr_t)r->r10;

    if (is_console_handle(h)) {
        // Console device: drena o que ja foi digitado (fila do teclado, IRQ1).
        // Nao bloqueia: devolve 0 bytes se nada foi digitado (ex.: headless sem
        // teclas) — o chamador trata 0 como "sem entrada". Com display, retorna
        // os caracteres digitados (entrada interativa do cmd.exe).
        uint32_t n = (buf && len) ? kbd_stdin_read((char*)buf, len) : 0;
        if (read) *read = n;
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }
    PFILE_OBJECT f = (PFILE_OBJECT)ob_handle_to_object((HANDLE)h, OB_TYPE_FILE);
    if (!f) {
        if (read) *read = 0;
        r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL;
        return;
    }
    // Named Pipe: le do buffer circular do pipe.
    if (f->PipeObject) {
        uint32_t n = pipe_read((PPIPE_OBJECT)f->PipeObject, buf, len);
        if (read) *read = n;
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }
    if (!f->DeviceObject) {
        if (read) *read = 0;
        r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL;
        return;
    }
    PIRP irp = io_build_read(f->DeviceObject, buf, len);
    if (!irp) { if (read) *read = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    // Volume NTFS: passa o registro MFT (Key) e o offset corrente p/ o driver de FS.
    if (f->DeviceObject == ntfs_fs_volume_device() && f->FsContext) {
        IoGetNextIrpStackLocation(irp)->Parameters.Read.Key        = (ULONG)f->FsContext;
        IoGetNextIrpStackLocation(irp)->Parameters.Read.ByteOffset = f->CurrentByteOffset;
    }
    IoCallDriver(f->DeviceObject, irp);
    uint64_t info = irp->IoStatus.Information;
    // METHOD_BUFFERED: copia a saida do SystemBuffer para o buffer do usuario.
    if (buf && irp->AssociatedIrp.SystemBuffer && info) {
        uint64_t n = info < len ? info : len;
        for (uint64_t i = 0; i < n; i++)
            ((uint8_t*)buf)[i] = ((uint8_t*)irp->AssociatedIrp.SystemBuffer)[i];
    }
    NTSTATUS st = irp->IoStatus.Status;
    io_free_irp(irp);
    if (f->DeviceObject == ntfs_fs_volume_device()) f->CurrentByteOffset += info;  // avança (read sequencial)
    if (read) *read = (uint32_t)info;
    r->rax = (uint64_t)(uint32_t)st;
}

// NtQueryDirectoryFile(HANDLE dir, void* outBuf, uint32_t outLen, uint32_t* retLen).
// Lista UMA entrada do diretorio aberto por NtCreateFile no volume NTFS, via o
// I/O Manager (IRP_MJ_DIRECTORY_CONTROL). Cada chamada avanca para a proxima
// entrada (estado no device do FS). RAX = STATUS_SUCCESS enquanto houver entrada;
// 0x80000006 (STATUS_NO_MORE_FILES) quando acabou. retLen recebe os bytes escritos.
static void sys_querydirectoryfile(struct regs* r) {
    HANDLE    h      = (HANDLE)(uintptr_t)r->rdi;
    void*     outB   = (void*)(uintptr_t)r->rsi;
    uint32_t  outLen = (uint32_t)r->rdx;
    uint32_t* retLen = (uint32_t*)(uintptr_t)r->r10;

    PFILE_OBJECT f = (PFILE_OBJECT)ob_handle_to_object(h, OB_TYPE_FILE);
    if (!f || !f->DeviceObject || f->DeviceObject != ntfs_fs_volume_device() || !outB) {
        if (retLen) *retLen = 0;
        r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL;
        return;
    }
    // Garante que o device aponta para ESTE diretorio antes de enumerar.
    ntfs_fs_set_target(f->FsContext, 1);

    PIRP irp = io_build_read(f->DeviceObject, outB, outLen);   // METHOD_BUFFERED
    if (!irp) { if (retLen) *retLen = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    IoGetNextIrpStackLocation(irp)->MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
    IoGetNextIrpStackLocation(irp)->Parameters.Read.Length = outLen;
    IoCallDriver(f->DeviceObject, irp);
    uint64_t info = irp->IoStatus.Information;
    if (outB && irp->AssociatedIrp.SystemBuffer && info) {
        uint64_t n = info < outLen ? info : outLen;
        for (uint64_t i = 0; i < n; i++)
            ((uint8_t*)outB)[i] = ((uint8_t*)irp->AssociatedIrp.SystemBuffer)[i];
    }
    NTSTATUS st = irp->IoStatus.Status;
    io_free_irp(irp);
    if (retLen) *retLen = (uint32_t)info;
    r->rax = (uint64_t)(uint32_t)st;
}

// NtQueryVolumeInformation(outBuf, outLen, *retLen) — FASE 5. Preenche um
// MEUOS_VOLUME_INFO (rotulo, serial, total/livre, fs name "NTFS") do volume NTFS
// montado como C:. Independe de handle (so ha um volume). RAX = STATUS_SUCCESS
// se ha volume e o buffer cabe; 0xC0000004 (INFO_LENGTH_MISMATCH) se pequeno;
// 0xC0000034 (OBJECT_NAME_NOT_FOUND) se nao ha volume montado.
typedef struct _MEUOS_VOLUME_INFO {
    uint64_t Serial;
    uint64_t TotalBytes;
    uint64_t FreeBytes;
    uint32_t BytesPerSector;
    uint32_t BytesPerCluster;
    char     FsName[8];    // "NTFS"
    char     Label[32];    // rotulo do volume
} MEUOS_VOLUME_INFO;

static void sys_queryvolumeinformation(struct regs* r) {
    void*     outB   = (void*)(uintptr_t)r->rdi;
    uint32_t  outLen = (uint32_t)r->rsi;
    uint32_t* retLen = (uint32_t*)(uintptr_t)r->rdx;

    uint32_t need = (uint32_t)sizeof(MEUOS_VOLUME_INFO);
    if (retLen) *retLen = need;
    if (!ntfs_fs_registered()) { r->rax = (uint64_t)0xC0000034u; return; }   // sem volume
    if (!outB || outLen < need) { r->rax = (uint64_t)0xC0000004u; return; }

    NTFS_VOLUME_INFO vi;
    if (!ntfs_volume_info(&vi)) { r->rax = (uint64_t)0xC0000034u; return; }

    MEUOS_VOLUME_INFO* o = (MEUOS_VOLUME_INFO*)outB;
    o->Serial          = vi.serial;
    o->TotalBytes      = vi.total_bytes;
    o->FreeBytes       = vi.free_bytes;
    o->BytesPerSector  = vi.bytes_per_sector;
    o->BytesPerCluster = vi.bytes_per_cluster;
    for (int i = 0; i < 8; i++)  o->FsName[i] = vi.fs_name[i];
    for (int i = 0; i < 32; i++) o->Label[i]  = vi.label[i];

    kputs("[ntfs.sys] NtQueryVolumeInformation: rotulo='"); kputs(o->Label);
    kputs("' fs="); kputs(o->FsName);
    kputs(" total="); kput_dec(o->TotalBytes); kputs(" bytes\n");
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// ---- Registro (stubs simples). Sem hive real ainda: NtOpenKey devolve um
// pseudo-handle (raiz fixa) e NtQueryValueKey responde valores conhecidos.
#define REG_ROOT_HANDLE ((uintptr_t)0x5245474B)   // 'REGK'
#define REG_TRACE 0   // 1 = loga cada NtOpenKey/NtQueryValueKey (diagnostico do que o
                      // explorer le no registro; util p/ mapear a init do shell). Off no commit.

static int str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int str_has(const char* h, const char* n) {   // substring (h contem n?)
    if (!h || !n) return 0;
    for (; *h; h++) { const char* a = h; const char* b = n; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
    return 0;
}

// NtOpenKey(out HANDLE*, const char* path). Stub sem hive: devolve a raiz fixa
// (REG_ROOT_HANDLE) para as chaves que "existem". EXCECAO honesta: chaves que de
// fato NAO existem devem FALHAR, senao o app acha que existem (vazias) e desiste do
// fallback. Ex.: Winlogon\AlternateShells\AvailableShells (feature de shell alternativo,
// ausente por padrao) — abri-la fazia o explorer enumerar valores vazios e cair no modo
// "janela de pasta" em vez de ler o valor Shell (que aponta explorer.exe) e virar o shell.
static void sys_openkey(struct regs* r) {
    HANDLE*     out  = (HANDLE*)(uintptr_t)r->rdi;
    const char* path = (const char*)(uintptr_t)r->rsi;
#if REG_TRACE
    kputs("[reg] NtOpenKey '"); kputs(path ? path : "(null)"); kputs("'\n");
#endif
    if (path && str_has(path, "AvailableShells")) {          // chave ausente -> nao encontrada
        if (out) *out = 0;
        r->rax = (uint64_t)0xC0000034u;                       // STATUS_OBJECT_NAME_NOT_FOUND
        return;
    }
    if (out) *out = (HANDLE)REG_ROOT_HANDLE;
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtQueryValueKey(HANDLE key, const char* name, void* buf, uint32_t buflen,
//                 uint32_t* outlen). Stub: responde valores conhecidos como
// string ASCII. Desconhecido -> STATUS_OBJECT_NAME_NOT_FOUND (0xC0000034).
static void sys_queryvaluekey(struct regs* r) {
    uintptr_t   key    = (uintptr_t)r->rdi;
    const char* name   = (const char*)(uintptr_t)r->rsi;
    char*       buf    = (char*)(uintptr_t)r->rdx;
    uint32_t    buflen = (uint32_t)r->r10;
    uint32_t*   outlen = (uint32_t*)(uintptr_t)r->r8;

#if REG_TRACE
    kputs("[reg] NtQueryValueKey '"); kputs(name ? name : "(null)"); kputs("'\n");
#endif
    const char* val = 0;
    if (key == REG_ROOT_HANDLE) {
        if (str_eq(name, "ProductName"))       val = "MeuOS";
        else if (str_eq(name, "CurrentVersion")) val = "0.1";
    }
    if (!val) { if (outlen) *outlen = 0; r->rax = (uint64_t)0xC0000034u; return; }

    uint32_t n = 0; while (val[n]) n++;
    n++;   // inclui o terminador
    if (buf && buflen) {
        uint32_t c = n < buflen ? n : buflen;
        for (uint32_t i = 0; i < c; i++) buf[i] = val[i];
    }
    if (outlen) *outlen = n;
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtGetModuleHandle(const char* name) -> base da imagem mapeada (ou 0). Apoia
// GetModuleHandleA: consulta o loader (que carregou as DLLs do boot). Em RAX.
static void sys_getmodulehandle(struct regs* r) {
    const char* name = (const char*)(uintptr_t)r->rdi;
    // ldr_find_runtime: aplica apiset_redirect + ".dll" e devolve a base JA carregada (nao
    // carrega). Antes chamava ldr_load direto -> "nao registrada" p/ kernelbase/apisets que
    // ja vivem no kernel32; agora GetModuleHandle("kernelbase")/("api-ms-win-core-*") acham.
    void* base = name ? ldr_find_runtime(name) : 0;
    r->rax = (uint64_t)(uintptr_t)base;
}

// NtGetProcAddress(void* module_base, const char* fn) -> endereco do export.
// Apoia GetProcAddress: caminha a export table da imagem (loader/pe.c). Em RAX.
// fn pode ser NOME (ponteiro >= 0x10000) OU ORDINAL (MAKEINTRESOURCEA: valor < 0x10000 no
// low word). Antes tratava tudo como string -> um ordinal viraria deref de endereco ~N e
// faltava (ou faltava por PF). Agora detecta o caso ordinal (ImageList_*/DPA_* do comctl32).
#define GPA_TRACE 0   // 1 = loga cada GetProcAddress que FALHA (mapeia os pontos de entrada
                      // que o explorer pede de dui70/comctl32 etc. -> lista nomeada do proximo
                      // muro). Diagnostico; commitado em 0.
static void sys_getprocaddress(struct regs* r) {
    void*       base = (void*)(uintptr_t)r->rdi;
    const char* fn   = (const char*)(uintptr_t)r->rsi;
    void* addr = 0;
    int is_ord = ((uintptr_t)fn < 0x10000);
    if (base && fn) {
        addr = is_ord ? pe_get_export_by_ordinal(base, (uint32_t)(uintptr_t)fn)
                      : pe_get_export(base, fn);
    }
#if GPA_TRACE
    if (base && fn && !addr) {
        kputs("[gpa] MISS ");
        if (is_ord) { kputs("#"); kput_dec((uint64_t)(uintptr_t)fn); }
        else kputs(fn);
        kputc('\n');
    }
#endif
    r->rax = (uint64_t)(uintptr_t)addr;
}

// ============================================================================
//  FASE 3 — Named Pipes (IPC). Args na convencao do nosso int 0x80.
// ============================================================================
// NtCreateNamedPipeFile(out HANDLE*, const char* name) — o SERVIDOR cria um
// Named Pipe no namespace (\Pipe\Nome) e recebe um handle de pipe. NtWriteFile/
// NtReadFile nesse handle vao para o buffer do pipe.
static void sys_createnamedpipe(struct regs* r) {
    HANDLE*     out  = (HANDLE*)(uintptr_t)r->rdi;
    const char* name = (const char*)(uintptr_t)r->rsi;

    PPIPE_OBJECT pipe = pipe_create(name);
    if (!pipe) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }

    // O handle do servidor e um FILE_OBJECT que aponta para o pipe.
    PFILE_OBJECT f = (PFILE_OBJECT)ObCreateObject(OB_TYPE_FILE, sizeof(FILE_OBJECT), 0);
    if (!f) { if (out) *out = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    f->DeviceObject = 0;
    f->PipeObject   = pipe;
    HANDLE h = ob_create_handle(f);
    if (out) *out = h;
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtConnectNamedPipe(HANDLE pipeServer) — o servidor sinaliza que esta pronto
// para um cliente. Sem escalonador nao bloqueia (ver pipe_connect).
static void sys_connectnamedpipe(struct regs* r) {
    HANDLE h = (HANDLE)(uintptr_t)r->rdi;
    PFILE_OBJECT f = (PFILE_OBJECT)ob_handle_to_object(h, OB_TYPE_FILE);
    if (!f || !f->PipeObject) { r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    r->rax = (uint64_t)(uint32_t)pipe_connect((PPIPE_OBJECT)f->PipeObject);
}

// ============================================================================
//  FASE 4 — Syscalls de informacao (estilo NT: NtQuerySystemInformation /
//  NtQueryInformationProcess / NtRead|WriteVirtualMemory).
// ============================================================================
// Classes de SystemInformationClass que respondemos (subconjunto do NT).
#define SystemBasicInformation        0
#define MeuOsVersionInformation       0x1000   // classe propria: string de versao do SO

// Layout do SYSTEM_BASIC_INFORMATION (subset, compativel com o NT no inicio).
typedef struct _SYSTEM_BASIC_INFORMATION {
    uint32_t Reserved;                 // 0x00
    uint32_t TimerResolution;          // 0x04
    uint32_t PageSize;                 // 0x08
    uint32_t NumberOfPhysicalPages;    // 0x0C
    uint32_t LowestPhysicalPageNumber; // 0x10
    uint32_t HighestPhysicalPageNumber;// 0x14
    uint32_t AllocationGranularity;    // 0x18
    uint64_t MinimumUserModeAddress;   // 0x1C (nao alinhado como no NT real; ok aqui)
    uint64_t MaximumUserModeAddress;
    uint64_t ActiveProcessorsAffinityMask;
    uint8_t  NumberOfProcessors;       // num de processadores (CPUs)
} SYSTEM_BASIC_INFORMATION;

// MeuOS roda single-CPU (sem SMP/APIC ainda): 1 processador logico.
#define MEUOS_NUM_PROCESSORS 1

static uint32_t copy_str_out(char* buf, uint32_t buflen, const char* s) {
    uint32_t n = 0; while (s[n]) n++; n++;   // inclui terminador
    if (buf && buflen) {
        uint32_t c = n < buflen ? n : buflen;
        for (uint32_t i = 0; i < c; i++) buf[i] = s[i];
    }
    return n;
}

// NtQuerySystemInformation(class, buf, buflen, *retlen). Responde:
//  - SystemBasicInformation: num de processadores, page size, paginas fisicas.
//  - MeuOsVersionInformation: string da versao do SO ("MeuOS 0.1 ...").
static void sys_querysysteminfo(struct regs* r) {
    uint32_t  cls    = (uint32_t)r->rdi;
    void*     buf    = (void*)(uintptr_t)r->rsi;
    uint32_t  buflen = (uint32_t)r->rdx;
    uint32_t* retlen = (uint32_t*)(uintptr_t)r->r10;

    if (cls == SystemBasicInformation) {
        uint32_t need = (uint32_t)sizeof(SYSTEM_BASIC_INFORMATION);
        if (retlen) *retlen = need;
        if (!buf || buflen < need) { r->rax = (uint64_t)0xC0000004u; return; } // STATUS_INFO_LENGTH_MISMATCH
        SYSTEM_BASIC_INFORMATION* bi = (SYSTEM_BASIC_INFORMATION*)buf;
        for (uint32_t i = 0; i < need; i++) ((uint8_t*)bi)[i] = 0;
        bi->PageSize                = 4096;
        bi->AllocationGranularity   = 4096;
        bi->NumberOfPhysicalPages   = (uint32_t)pmm_total_frames();
        bi->LowestPhysicalPageNumber  = 0;
        bi->HighestPhysicalPageNumber = (uint32_t)(pmm_total_frames() ? pmm_total_frames() - 1 : 0);
        bi->MinimumUserModeAddress  = 0x10000;
        bi->MaximumUserModeAddress  = 0x3FFFFFFFULL;     // 1 GiB de identidade
        bi->ActiveProcessorsAffinityMask = 1;
        bi->NumberOfProcessors      = MEUOS_NUM_PROCESSORS;
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }
    if (cls == MeuOsVersionInformation) {
        const char* ver = "MeuOS 0.1 (kernel 64-bit estilo NT)";
        uint32_t need = copy_str_out((char*)buf, buflen, ver);
        if (retlen) *retlen = need;
        r->rax = (uint64_t)(uint32_t)((buf && buflen >= need) ? STATUS_SUCCESS : 0xC0000004u);
        return;
    }
    if (retlen) *retlen = 0;
    r->rax = (uint64_t)0xC0000003u;   // STATUS_INVALID_INFO_CLASS
}

// Classes de ProcessInformationClass que respondemos.
#define ProcessBasicInformation  0

// Layout (subset) do PROCESS_BASIC_INFORMATION: o que entregamos do EPROCESS.
typedef struct _PROCESS_BASIC_INFORMATION {
    uint32_t ExitStatus;        // 0x00
    uint32_t Reserved0;         // 0x04
    uint64_t PebBaseAddress;    // 0x08  (usamos para a ImageBase do processo)
    uint64_t AffinityMask;      // 0x10
    uint64_t BasePriority;      // 0x18
    uint64_t UniqueProcessId;   // 0x20  (PID)
    uint64_t InheritedFromUniqueProcessId; // 0x28
} PROCESS_BASIC_INFORMATION;

// NtQueryInformationProcess(hProcess, class, buf, buflen, *retlen).
//  hProcess==0 -> processo corrente. ProcessBasicInformation: pid + ImageBase.
static void sys_queryinformationprocess(struct regs* r) {
    HANDLE    hproc  = (HANDLE)(uintptr_t)r->rdi;
    uint32_t  cls    = (uint32_t)r->rsi;
    void*     buf    = (void*)(uintptr_t)r->rdx;
    uint32_t  buflen = (uint32_t)r->r10;
    uint32_t* retlen = (uint32_t*)(uintptr_t)r->r8;

    PEPROCESS p = hproc ? (PEPROCESS)ob_handle_to_object(hproc, OB_TYPE_PROCESS)
                        : PsGetCurrentProcess();
    if (!p) { if (retlen) *retlen = 0; r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }

    if (cls == ProcessBasicInformation) {
        uint32_t need = (uint32_t)sizeof(PROCESS_BASIC_INFORMATION);
        if (retlen) *retlen = need;
        if (!buf || buflen < need) { r->rax = (uint64_t)0xC0000004u; return; }
        PROCESS_BASIC_INFORMATION* bi = (PROCESS_BASIC_INFORMATION*)buf;
        for (uint32_t i = 0; i < need; i++) ((uint8_t*)bi)[i] = 0;
        bi->ExitStatus      = p->ExitStatus;
        bi->PebBaseAddress  = p->ImageBase;     // expomos a base da imagem aqui
        bi->AffinityMask    = 1;
        bi->BasePriority    = 8;
        bi->UniqueProcessId = p->ProcessId;
        r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
        return;
    }
    if (retlen) *retlen = 0;
    r->rax = (uint64_t)0xC0000003u;   // STATUS_INVALID_INFO_CLASS
}

// NtReadVirtualMemory(hProcess, base, buf, len, *read). Simples: como o espaco e
// identidade-mapeado (1 GiB compartilhado), copiamos diretamente da faixa baixa.
static void sys_readvirtualmemory(struct regs* r) {
    HANDLE    hproc = (HANDLE)(uintptr_t)r->rdi;
    uint64_t  base  = (uint64_t)r->rsi;
    void*     buf   = (void*)(uintptr_t)r->rdx;
    uint32_t  len   = (uint32_t)r->r10;
    uint32_t* read  = (uint32_t*)(uintptr_t)r->r8;
    (void)hproc;

    if (!buf || base >= 0x40000000ULL || (base + len) > 0x40000000ULL) {
        if (read) *read = 0;
        r->rax = (uint64_t)0xC0000005u;   // STATUS_ACCESS_VIOLATION
        return;
    }
    const uint8_t* src = (const uint8_t*)(uintptr_t)base;
    for (uint32_t i = 0; i < len; i++) ((uint8_t*)buf)[i] = src[i];
    if (read) *read = len;
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// NtWriteVirtualMemory(hProcess, base, buf, len, *written). Idem (faixa baixa).
static void sys_writevirtualmemory(struct regs* r) {
    HANDLE    hproc   = (HANDLE)(uintptr_t)r->rdi;
    uint64_t  base    = (uint64_t)r->rsi;
    const void* buf   = (const void*)(uintptr_t)r->rdx;
    uint32_t  len     = (uint32_t)r->r10;
    uint32_t* written = (uint32_t*)(uintptr_t)r->r8;
    (void)hproc;

    if (!buf || base >= 0x40000000ULL || (base + len) > 0x40000000ULL) {
        if (written) *written = 0;
        r->rax = (uint64_t)0xC0000005u;   // STATUS_ACCESS_VIOLATION
        return;
    }
    uint8_t* dst = (uint8_t*)(uintptr_t)base;
    for (uint32_t i = 0; i < len; i++) dst[i] = ((const uint8_t*)buf)[i];
    if (written) *written = len;
    r->rax = (uint64_t)(uint32_t)STATUS_SUCCESS;
}

// ============================================================================
//  FASE 5 — Shell cmd.exe: enumerar objetos do Object Manager (Process/Driver)
//  e carregar/descarregar drivers de kernel por nome (sc start/stop).
// ============================================================================
// Layout devolvido por NtEnumProcesses para CADA indice. O cmd.exe (ring 3) tem
// uma copia desta struct. So expomos campos simples (sem ponteiros do kernel).
typedef struct _MEUOS_PROCESS_ENTRY {
    uint32_t ProcessId;        // PID
    uint32_t Terminated;       // 1 se ja encerrou
    uint64_t ImageBase;        // base da imagem
    uint32_t ThreadCount;      // threads do processo
    char     ImageName[32];    // nome da imagem (ex.: "test.exe")
} MEUOS_PROCESS_ENTRY;

// Layout devolvido por NtEnumDrivers para CADA indice (sc query).
typedef struct _MEUOS_DRIVER_ENTRY {
    uint32_t State;            // DRV_STATE_STOPPED(1) / DRV_STATE_RUNNING(4)
    uint32_t LastStatus;       // status do ultimo DriverEntry
    char     Name[32];         // nome do .sys (ex.: "mydriver.sys")
} MEUOS_DRIVER_ENTRY;

static void sc_copy_name(char* dst, const char* src, int max) {
    int i = 0;
    if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// NtEnumProcesses(index, MEUOS_PROCESS_ENTRY* out). Devolve o n-esimo EPROCESS
// (RAX=1 e preenche *out) ou RAX=0 quando 'index' passou do ultimo. Permite ao
// cmd.exe listar processos (tasklist) percorrendo index=0,1,2,...
static void sys_enumprocesses(struct regs* r) {
    int index = (int)r->rdi;
    MEUOS_PROCESS_ENTRY* out = (MEUOS_PROCESS_ENTRY*)(uintptr_t)r->rsi;

    PEPROCESS p = (PEPROCESS)ob_enum_by_type(OB_TYPE_PROCESS, index);
    if (!p || !out) { r->rax = 0; return; }
    out->ProcessId   = p->ProcessId;
    out->Terminated  = p->Terminated;
    out->ImageBase   = p->ImageBase;
    out->ThreadCount = p->ThreadCount;
    sc_copy_name(out->ImageName, p->ImageName, (int)sizeof(out->ImageName));
    r->rax = 1;
}

// NtEnumDrivers(index, MEUOS_DRIVER_ENTRY* out). Devolve o n-esimo driver
// conhecido (sc query): nome + estado (STOPPED/RUNNING) + ultimo status.
static void sys_enumdrivers(struct regs* r) {
    int index = (int)r->rdi;
    MEUOS_DRIVER_ENTRY* out = (MEUOS_DRIVER_ENTRY*)(uintptr_t)r->rsi;

    const char* name = 0; uint32_t state = 0; NTSTATUS last = 0;
    if (!out || !driver_enum(index, &name, &state, &last)) { r->rax = 0; return; }
    out->State      = state;
    out->LastStatus = (uint32_t)last;
    sc_copy_name(out->Name, name, (int)sizeof(out->Name));
    r->rax = 1;
}

// NtLoadDriver(const char* name) — 'sc start <nome>': carrega o .sys pelo nome
// (via I/O Manager + loader) e o deixa RODANDO. RAX = NTSTATUS.
static void sys_loaddriver(struct regs* r) {
    const char* name = (const char*)(uintptr_t)r->rdi;
    if (!name) { r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    r->rax = (uint64_t)(uint32_t)driver_load_by_name(name);
}

// NtUnloadDriver(const char* name) — 'sc stop <nome>': chama o DriverUnload e
// marca o driver como STOPPED. RAX = NTSTATUS.
static void sys_unloaddriver(struct regs* r) {
    const char* name = (const char*)(uintptr_t)r->rdi;
    if (!name) { r->rax = (uint64_t)(uint32_t)STATUS_UNSUCCESSFUL; return; }
    r->rax = (uint64_t)(uint32_t)driver_unload_by_name(name);
}

// ============================================================================
//  FASE 2 — win32k: janelas + mensagens + GDI (ring 3 -> int 0x80 -> win32k.c)
//  Args na convencao do nosso int 0x80: rdi, rsi, rdx, r10, r8, r9.
// ============================================================================
// NtUserRegisterClass(const char* className, void* wndProc)
static void sys_user_registerclass(struct regs* r) {
    r->rax = (uint64_t)NtUserRegisterClass_k((const char*)(uintptr_t)r->rdi,
                                             (void*)(uintptr_t)r->rsi);
}
// NtUserCreateWindowEx(W32_CREATE* c) -> HWND
static void sys_user_createwindowex(struct regs* r) {
    r->rax = (uint64_t)NtUserCreateWindowEx_k((W32_CREATE*)(uintptr_t)r->rdi);
}
// NtUserDestroyWindow(HWND)
static void sys_user_destroywindow(struct regs* r) {
    r->rax = (uint64_t)NtUserDestroyWindow_k((void*)(uintptr_t)r->rdi);
}
// NtUserShowWindow(HWND, int cmdShow)
static void sys_user_showwindow(struct regs* r) {
    r->rax = (uint64_t)NtUserShowWindow_k((void*)(uintptr_t)r->rdi, (int)r->rsi);
}
// NtUserGetMessage(W32_MSG* out) -> 1 normal / 0 WM_QUIT / -1 erro
static void sys_user_getmessage(struct regs* r) {
    int rc = NtUserGetMessage_k((W32_MSG*)(uintptr_t)r->rdi);
    r->rax = (uint64_t)(int64_t)rc;
}
// NtUserDispatchMessage(W32_MSG* msg)
static void sys_user_dispatchmessage(struct regs* r) {
    r->rax = (uint64_t)NtUserDispatchMessage_k((W32_MSG*)(uintptr_t)r->rdi);
}
// NtUserPostMessage(HWND, msg, wParam, lParam)
static void sys_user_postmessage(struct regs* r) {
    r->rax = (uint64_t)NtUserPostMessage_k((void*)(uintptr_t)r->rdi, (uint32_t)r->rsi,
                                           (uint64_t)r->rdx, (uint64_t)r->r10);
}
// NtUserPostQuitMessage(int exitCode)
static void sys_user_postquitmessage(struct regs* r) {
    r->rax = (uint64_t)NtUserPostQuitMessage_k((int)r->rdi);
}
// NtUserGetDC(HWND) -> HDC
static void sys_user_getdc(struct regs* r) {
    r->rax = (uint64_t)NtUserGetDC_k((void*)(uintptr_t)r->rdi);
}
// NtUserInvalidate(HWND)
static void sys_user_invalidate(struct regs* r) {
    r->rax = (uint64_t)NtUserInvalidate_k((void*)(uintptr_t)r->rdi);
}
// NtGdiGetStockObject(int index) -> HBRUSH
static void sys_gdi_getstockobject(struct regs* r) {
    r->rax = (uint64_t)NtGdiGetStockObject_k((int)r->rdi);
}
// NtGdiCreateSolidBrush(uint32_t color) -> HBRUSH
static void sys_gdi_createsolidbrush(struct regs* r) {
    r->rax = (uint64_t)NtGdiCreateSolidBrush_k((uint32_t)r->rdi);
}
// NtGdiTextOut(HDC, int x, int y, const char* str, int len)
static void sys_gdi_textout(struct regs* r) {
    r->rax = (uint64_t)NtGdiTextOut_k((void*)(uintptr_t)r->rdi, (int)r->rsi,
                                      (int)r->rdx, (const char*)(uintptr_t)r->r10,
                                      (int)r->r8);
}
// NtGdiFillRect(HDC, int x, int y, int w, int h, HBRUSH)
static void sys_gdi_fillrect(struct regs* r) {
    r->rax = (uint64_t)NtGdiFillRect_k((void*)(uintptr_t)r->rdi, (int)r->rsi,
                                       (int)r->rdx, (int)r->r10, (int)r->r8,
                                       (void*)(uintptr_t)r->r9);
}

// ============================================================================
//  FASE 6 — desktop + barra de tarefas + cmd numa janela.
// ============================================================================
// NtUserSetFocus(HWND) -> da o foco a janela (clique/Alt+Tab). RAX=1 se mudou.
static void sys_user_setfocus(struct regs* r) {
    r->rax = (uint64_t)NtUserSetFocus_k((void*)(uintptr_t)r->rdi);
}
// NtUserPostKey(HWND, char ascii, uint8_t scancode) -> posta WM_KEYDOWN+WM_CHAR
// para UMA janela (independe do foco). Demo: entrada deterministica headless.
static void sys_user_postkey(struct regs* r) {
    r->rax = (uint64_t)NtUserPostKey_k((void*)(uintptr_t)r->rdi,
                                       (char)(uint8_t)r->rsi, (uint8_t)r->rdx);
}
// NtGdiTextOutEx(HDC, x, y, str, len, fg, bg) -> TextOut com cor (console).
static void sys_gdi_textoutex(struct regs* r) {
    r->rax = (uint64_t)NtGdiTextOutEx_k((void*)(uintptr_t)r->rdi, (int)r->rsi,
                                        (int)r->rdx, (const char*)(uintptr_t)r->r10,
                                        (int)r->r8, (uint32_t)r->r9, /*bg*/0xFF);
}

// ============================================================================
//  FASE 11 — Cursor do mouse.
// ============================================================================
// Layout do POINT no win32 (compativel com o que apps usam pra GetCursorPos).
typedef struct _MEUOS_POINT { int32_t x, y; } MEUOS_POINT;

// NtUserGetCursorPos(POINT* out) -> 1 se preencheu, 0 senao. RAX = NTSTATUS-like.
static void sys_user_getcursorpos(struct regs* r) {
    MEUOS_POINT* out = (MEUOS_POINT*)(uintptr_t)r->rdi;
    if (!out) { r->rax = 0; return; }
    out->x = win32k_cursor_x();
    out->y = win32k_cursor_y();
    r->rax = 1;
}

// NtUserSetCursorPos(int x, int y) -> 1.
static void sys_user_setcursorpos(struct regs* r) {
    win32k_set_cursor((int32_t)(int)r->rdi, (int32_t)(int)r->rsi);
    r->rax = 1;
}

// Frente C (explorer real): tempo do sistema p/ o usermode. Le o KUSER_SHARED_DATA
// (mapeado em 0xFFFFF78000000000, atualizado por mm_kuser_tick): SystemTime (FILETIME
// 100ns @ +0x20) e TickCount em ms (@ +0x14). O CRT do explorer chama isto no
// __security_init_cookie (GetSystemTimeAsFileTime/GetTickCount). out = {int64 SystemTime; u32 TickCount}.
static void sys_querysystemtime(struct regs* r) {
    struct meuos_timeinfo { long long SystemTime; uint32_t TickCount; };
    struct meuos_timeinfo* out = (struct meuos_timeinfo*)(uintptr_t)r->rdi;
    if (!out) { r->rax = (uint64_t)0xC000000Du; return; }   // STATUS_INVALID_PARAMETER
    volatile uint8_t* kuser = (volatile uint8_t*)0xFFFFF78000000000ULL;
    out->SystemTime = *(volatile long long*)(kuser + 0x20);   // KSYSTEM_TIME (Low@0x20, High1@0x24)
    out->TickCount  = *(volatile uint32_t*)(kuser + 0x14);    // ms (paging.c: g_ticks*10)
    r->rax = 0;   // STATUS_SUCCESS
}

// Frente C (explorer real): VirtualAlloc. Aloca `size` bytes de frames CONTIGUOS no 1o
// GiB (identity-mapped) + abre o bit USER -> devolve o VA (== fisico). Backing do
// HeapAlloc do explorer. size = r->rdi. Devolve 0 se sem RAM.
extern uint64_t pmm_alloc_contiguous(uint64_t num_pages);
extern void     mm_map_user(uint64_t addr, uint64_t size);
static void sys_virtualalloc(struct regs* r) {
    uint64_t size = r->rdi;
    if (!size) { r->rax = 0; return; }
    uint64_t pages = (size + 0xFFF) >> 12;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) { r->rax = 0; return; }
    mm_map_user(phys, size);
    r->rax = phys;   // VA == phys (identidade, 1o GiB)
}

// ---- SSDT: tabela central de serviços ----
typedef void (*syscall_fn)(struct regs*);
static syscall_fn s_ssdt[] = {
    0,                          // 0
    sys_write,                  // 1
    sys_exit,                   // 2
    sys_messagebox,             // 3
    sys_createfile,             // 4
    sys_deviceiocontrol,        // 5
    sys_close,                  // 6
    sys_createprocess,          // 7
    sys_createthread,           // 8
    sys_terminateprocess,       // 9
    sys_waitforsingleobject,    // 10
    sys_writefile,              // 11
    sys_readfile,               // 12
    sys_openkey,                // 13
    sys_queryvaluekey,          // 14
    sys_getmodulehandle,        // 15
    sys_getprocaddress,         // 16
    sys_user_registerclass,     // 17
    sys_user_createwindowex,    // 18
    sys_user_destroywindow,     // 19
    sys_user_showwindow,        // 20
    sys_user_getmessage,        // 21
    sys_user_dispatchmessage,   // 22
    sys_user_postmessage,       // 23
    sys_user_postquitmessage,   // 24
    sys_user_getdc,             // 25
    sys_user_invalidate,        // 26
    sys_gdi_getstockobject,     // 27
    sys_gdi_createsolidbrush,   // 28
    sys_gdi_textout,            // 29
    sys_gdi_fillrect,           // 30
    sys_createnamedpipe,        // 31
    sys_connectnamedpipe,       // 32
    sys_querysysteminfo,        // 33
    sys_queryinformationprocess,// 34
    sys_readvirtualmemory,      // 35
    sys_writevirtualmemory,     // 36
    sys_enumprocesses,          // 37
    sys_enumdrivers,            // 38
    sys_loaddriver,             // 39
    sys_unloaddriver,           // 40
    sys_user_setfocus,          // 41
    sys_user_postkey,           // 42
    sys_gdi_textoutex,          // 43
    sys_querydirectoryfile,     // 44
    sys_queryvolumeinformation, // 45
    sys_user_getcursorpos,      // 46 (FASE 11)
    sys_user_setcursorpos,      // 47 (FASE 11)
    sys_loadlibrary,            // 48 (FASE 3f: LoadLibrary runtime)
    sys_querysystemtime,        // 49 (Frente C: tempo p/ o explorer real)
    sys_virtualalloc,           // 50 (Frente C: VirtualAlloc p/ o heap do explorer)
};
#define SSDT_COUNT (sizeof(s_ssdt) / sizeof(s_ssdt[0]))

void syscall_dispatch(struct regs* r) {
    // Syscall vinda de codigo de 32 bits (compatibility mode): o CS salvo e o
    // SEL_UCODE32 | RPL3 (0x3B). Em 32 bits, escrever EAX/EDI/ESI/EDX nao zera
    // os 32 bits altos do registrador de 64 bits; truncamos os argumentos para
    // 32 bits para que os servicos (que ja tratam ponteiros/inteiros baixos)
    // funcionem identicamente para PE32 e PE32+. Convencao 32-bit: EAX=numero;
    // args em EDI, ESI, EDX, ECX.
    if ((r->cs & 0xFFFF) == 0x3B) {
        r->rax = (uint32_t)r->rax;
        r->rdi = (uint32_t)r->rdi;
        r->rsi = (uint32_t)r->rsi;
        r->rdx = (uint32_t)r->rdx;
        r->rcx = (uint32_t)r->rcx;
    }

    uint64_t n = r->rax;
    if (n < SSDT_COUNT && s_ssdt[n]) { s_ssdt[n](r); return; }
    kputs("[ke] syscall desconhecida: "); kput_hex(n); kputc('\n');
    r->rax = (uint64_t)-1;
}
