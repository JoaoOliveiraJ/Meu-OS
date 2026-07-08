#include <stdint.h>
#include <stddef.h>
#include "video/vga.h"
#include "video/video.h"
#include "display/BasicDisplay/gpu.h"
#include "display/VirtioGpu/VirtioGpu.h"
#include "serial/serial.h"
#include "input/keyboard.h"
#include "input/mouse/mouse.h"   // FASE 11: driver PS/2 do mouse (IRQ12)
#include "input/virtio_input.h"  // FASE 14: tablet ABSOLUTA (virtio-input) — cursor visivel
#include "ke/amd64/gdt.h"
#include "ke/amd64/idt.h"
#include "ke/amd64/pic.h"
#include "ke/amd64/pit.h"
#include "ke/amd64/isr.h"
#include "ke/amd64/apic.h"   // Pilar 2 (NT foundation): Local APIC + IO-APIC
#include "ke/amd64/smp.h"    // Pilar 3 (NT foundation): MADT + INIT-SIPI-SIPI
#include "ke/sched.h"        // Pilar 4 (NT foundation): scheduler MP
#include "io.h"               // inb/outb para leitura direta de portas (proof P2)
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/paging.h"   // Pilar 1 (NT foundation): mmio arena + probe
#include "hal/hal.h"
#include "hal/cpu.h"
#include "hal/disk.h"
#include "filesystems/ntfs/ntfs.h"
#include "ldr/loader.h"
#include "ldr/pe_export_image.h"   // GATE 2/5/6 do pintok.sys: ntoskrnl.exe sintetico parseavel
#include "io/driver.h"
#include "ob/object.h"
#include "io/io.h"
#include "ps/process.h"
#include "ex/callbacks.h"
#include "cm/registry.h"
#include "ps/cid_table.h"   // FASE 7.5: PspCidTable
#include "win32/win32k.h"
#include "dx/dxgkrnl/dxgkrnl.h"   // dxgkrnl: dispatcher DirectX em kernel
#include "dx/dxgmms/dxgmms.h"     // dxgmms : memory manager de video (residency)
#include "audio/HDAudio/HDAudio.h" // FASE 11: HD Audio stub (so deteccao PCI)
#include "network/ndis/ndis.h"     // FASE 12: NDIS framework (kernel)
#include "network/tcpip/tcpip.h"   // FASE 12: TCP/IP stack (kernel)
#include "network/e1000/e1000.h"   // FASE 12: Intel 8254x NIC stub
#include "usb/usbport/usbport.h"   // FASE 13: USB Port framework
#include "usb/usbhub/usbhub.h"     // FASE 13: USB Hub class driver
#include "usb/xhci/xhci.h"         // FASE 13: xHCI/EHCI/OHCI/UHCI host controller stub
#include "acpi/acpi.h"             // FASE 13: ACPI stub (RSDP scan)
#include "fltmgr/fltmgr.h"         // FASE 13: Filter Manager
#include "io/pnp.h"                // FASE 13: PnP Manager (IRP_MJ_PNP)

// ============================================================================
//  main.c — ENXUTO (bring-up do explorer.exe REAL).
//
//  Este arquivo foi limpo: saiu o "desfile" de testes/demos de desenvolvimento
//  (leitura/escrita NTFS de exemplo, demo do framebuffer mode 13h, smoke test do
//  mouse, auto-testes das fases de fundacao, threads worker de demonstracao) e o
//  suporte ao SHELL CASEIRO (o explorer/desktop que estavamos recriando: relogio
//  da taskbar, refresh, compose de demo). A versao ANTERIOR completa esta salva em
//  `main.c.old` (nada foi perdido).
//
//  O QUE PERMANECE (o essencial p/ o boot real):
//   - init do kernel na ordem verificada (GDT/IDT/PIC/PIT -> MM -> HAL -> APIC ->
//     SMP -> subsistemas de fundacao/DX/audio/rede/USB -> Object/Process/win32k);
//   - as PROVAS dos Pilares 1/2/3 (paginacao, APIC, SMP) — base dourada do pintok;
//   - o carregador de MODULOS Multiboot (roda QUALQUER PE: .sys -> driver ring 0,
//     .exe -> ring 3), que e' como o pintok.sys e o explorer.exe REAL sobem;
//   - a PREEMPCAO (g_p4_active) — as threads ring-3 que o explorer cria via
//     CreateThread/SHCreateThread SO rodam com o scheduler preemptivo ligado.
// ============================================================================

// Escreve um caractere na tela (VGA) E na serial.
void kputc(char c) {
    vga_putc(c);
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}
void kputs(const char* s) { while (*s) kputc(*s++); }

void kput_hex(uint64_t v) {
    const char* d = "0123456789ABCDEF";
    kputc('0'); kputc('x');
    for (int i = 60; i >= 0; i -= 4) kputc(d[(v >> i) & 0xF]);
}
void kput_dec(uint64_t v) {
    char buf[21]; int i = 0;
    if (v == 0) { kputc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) kputc(buf[i]);
}

// Rotinas que o compilador pode chamar implicitamente (freestanding).
void* memset(void* dst, int v, size_t n) {
    uint8_t* p = (uint8_t*)dst; while (n--) *p++ = (uint8_t)v; return dst;
}
void* memcpy(void* dst, const void* s, size_t n) {
    uint8_t* a = (uint8_t*)dst; const uint8_t* b = (const uint8_t*)s;
    while (n--) *a++ = *b++; return dst;
}
void* memmove(void* dst, const void* s, size_t n) {
    uint8_t* a = (uint8_t*)dst; const uint8_t* b = (const uint8_t*)s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

// ============================================================================
//  Pilar 1 (NT foundation) — PROVA de paginacao dinamica.
//
//  Tres etapas, em ordem:
//   (a) Mapeia o Local APIC em 0xFEE00000 (FISICO ACIMA DE 1 GIB, fora da
//       identidade) e LE o registrador LAPIC ID (offset 0x20). Sem PCD esse
//       acesso poderia retornar lixo bufferizado; nosso hal_map_mmio seta
//       PCD|PWT explicitamente. Logamos o valor lido.
//   (b) Alias-mapeia uma mesma pagina fisica em DOIS enderecos virtuais
//       diferentes do arena MMIO. Escreve 0xDEADBEEF via V1, le via V2:
//       deve bater. Prova que mm_map_phys_range esta mesmo populando PTEs.
//   (c) Desmapeia V1, le via V1 dentro de mm_probe_read_u32 (que aciona o
//       caminho expected-PF em isr.c). Deve disparar #PF -> g_mm_pf_caught=1.
//       V2 continua valido (sao PTEs diferentes apontando p/ o mesmo phys).
// ============================================================================
static int proof_pillar1_paging(void) {
    int ok = 1;
    kputs("\n[P1] ==== prova de paginacao dinamica (Pilar 1) ====\n");

    // (a) — LAPIC MMIO acima de 1 GiB.
    volatile uint32_t* lapic = (volatile uint32_t*)hal_map_mmio(0xFEE00000ULL, 0x1000ULL);
    if (!lapic) {
        kputs("[P1] FAIL: hal_map_mmio(LAPIC) devolveu 0\n");
        return 0;
    }
    uint32_t lapic_id_raw = lapic[0x20 / 4];
    kputs("[P1] LAPIC mapeado @ "); kput_hex((uint64_t)(uintptr_t)lapic);
    kputs("  ID raw="); kput_hex(lapic_id_raw);
    kputs("  (APIC ID = "); kput_dec((lapic_id_raw >> 24) & 0xFF); kputs(")\n");

    // (b) — alias de uma mesma pagina fisica em DOIS VAs.
    uint64_t phys = pmm_alloc_frame();
    if (!phys) { kputs("[P1] FAIL: pmm_alloc_frame sem RAM\n"); return 0; }
    uint64_t v1 = mm_mmio_reserve(0x1000);
    uint64_t v2 = mm_mmio_reserve(0x1000);
    if (!v1 || !v2) { kputs("[P1] FAIL: mm_mmio_reserve\n"); return 0; }
    if (!mm_map_phys_range(v1, phys, 0x1000, MM_FLAG_PRESENT | MM_FLAG_RW)) {
        kputs("[P1] FAIL: map v1\n"); return 0;
    }
    if (!mm_map_phys_range(v2, phys, 0x1000, MM_FLAG_PRESENT | MM_FLAG_RW)) {
        kputs("[P1] FAIL: map v2\n"); return 0;
    }
    volatile uint32_t* p1 = (volatile uint32_t*)(uintptr_t)v1;
    volatile uint32_t* p2 = (volatile uint32_t*)(uintptr_t)v2;
    *p1 = 0xDEADBEEFu;
    uint32_t back = *p2;
    kputs("[P1] alias map: phys="); kput_hex(phys);
    kputs(" v1="); kput_hex(v1); kputs(" v2="); kput_hex(v2);
    kputs(" write(v1)=0xDEADBEEF read(v2)="); kput_hex(back);
    if (back == 0xDEADBEEFu) kputs(" OK\n");
    else { kputs(" FAIL\n"); ok = 0; }

    // (c) — unmap em V1; V1 deve faltar #PF, V2 continua valido.
    if (!mm_unmap_range(v1, 0x1000)) {
        kputs("[P1] FAIL: mm_unmap_range(v1)\n"); return 0;
    }
    uint32_t junk = 0xCAFEBABEu;
    int got = mm_probe_read_u32(p1, &junk);
    kputs("[P1] probe(v1 apos unmap): retorno="); kput_dec((uint64_t)got);
    kputs(" caught="); kput_dec((uint64_t)g_mm_pf_caught);
    if (!got && g_mm_pf_caught) kputs(" OK (PF capturado)\n");
    else { kputs(" FAIL (esperava PF)\n"); ok = 0; }

    uint32_t back2 = *p2;
    kputs("[P1] read(v2) pos-unmap(v1)="); kput_hex(back2);
    if (back2 == 0xDEADBEEFu) kputs(" OK (alias independente)\n");
    else { kputs(" FAIL\n"); ok = 0; }

    mm_unmap_range(v2, 0x1000);
    pmm_free_frame(phys);

    if (ok) kputs("[P1] ==== PROVA PASSOU ====\n\n");
    else    kputs("[P1] ==== PROVA FALHOU ====\n\n");
    return ok;
}

// ============================================================================
//  Pilar 2 (NT foundation) — PROVA de APIC (Local APIC timer + IO-APIC).
//   (a) Antes do APIC: confere que g_ticks avanca pelo PIT.
//   (b) apic_init() — mapeia LAPIC + IO-APIC, calibra timer contra PIT,
//       programa LVT Timer periodico em 0xD1, redireciona IRQ1, MASCARA o PIC.
//   (c) Confirma PIC mascarado (PIC1=0xFF, PIC2=0xFF).
//   (d) g_ticks precisa CONTINUAR avancando — agora pelo vetor 0xD1 (APIC).
// ============================================================================
static int proof_pillar2_apic(void) {
    int ok = 1;
    kputs("\n[P2] ==== prova de APIC (Pilar 2) ====\n");

    uint64_t base = g_ticks;
    while (g_ticks < base + 5) __asm__ volatile ("hlt");
    kputs("[P2] PIT base: g_ticks "); kput_dec(base);
    kputs(" -> "); kput_dec(g_ticks); kputs(" (PIT alimentando)\n");

    apic_init();
    if (!apic_active()) { kputs("[P2] FAIL: apic_init nao ativou\n"); return 0; }

    uint8_t pm1 = inb(0x21);
    uint8_t pm2 = inb(0xA1);
    kputs("[P2] PIC mask: PIC1=0x"); kput_hex(pm1);
    kputs(" PIC2=0x"); kput_hex(pm2);
    if (pm1 == 0xFFu && pm2 == 0xFFu) kputs(" OK (8259 silente)\n");
    else { kputs(" FAIL\n"); ok = 0; }

    uint64_t t0 = g_ticks;
    while (g_ticks < t0 + 20) __asm__ volatile ("hlt");
    uint64_t t1 = g_ticks;
    kputs("[P2] APIC timer: g_ticks "); kput_dec(t0);
    kputs(" -> "); kput_dec(t1);
    kputs(" delta="); kput_dec(t1 - t0);
    if (t1 - t0 >= 20) kputs(" OK (APIC alimentando com PIT desligado)\n");
    else { kputs(" FAIL\n"); ok = 0; }

    if (ok) kputs("[P2] ==== PROVA PASSOU ====\n\n");
    else    kputs("[P2] ==== PROVA FALHOU ====\n\n");
    return ok;
}

// ============================================================================
//  Pilar 3 (NT foundation) — PROVA de SMP.
//   (a) acpi_init() — acha RSDP, le RSDT phys.
//   (b) smp_init() — parseia MADT, lanca cada AP via trampoline + INIT-SIPI-SIPI.
//   (c) Espera AP sinalizar alive. (d) Passa se MADT >= 2 CPUs e >= 1 AP alive.
// ============================================================================
static int proof_pillar3_smp(void) {
    kputs("\n[P3] ==== prova de SMP (Pilar 3) ====\n");

    extern int acpi_init(void);
    extern uint64_t acpi_rsdt_phys(void);
    acpi_init();
    uint64_t rsdt = acpi_rsdt_phys();
    kputs("[P3] RSDT phys="); kput_hex(rsdt); kputc('\n');

    smp_init();

    int cpus = smp_cpu_count();
    uint32_t alive = smp_ap_alive_count();
    kputs("[P3] CPUs MADT="); kput_dec((uint64_t)cpus);
    kputs("  APs_alive="); kput_dec((uint64_t)alive); kputc('\n');

    int ok = 1;
    if (cpus < 2)  { kputs("[P3] FAIL: MADT relatou < 2 CPUs (esperava >= 2 com -smp 2)\n"); ok = 0; }
    if (alive < 1) { kputs("[P3] FAIL: nenhum AP sinalizou alive\n"); ok = 0; }
    if (cpus >= 2 && !smp_ap_seen(1)) { kputs("[P3] FAIL: AP com APIC_ID=1 nao sinalizou\n"); ok = 0; }

    if (ok) kputs("[P3] ==== PROVA PASSOU ====\n\n");
    else    kputs("[P3] ==== PROVA FALHOU ====\n\n");
    return ok;
}

// ============================================================================
//  PREEMPCAO (scheduler MP). Liga o gate g_p4_active: dai em diante o timer 0xD1
//  chama ki_quantum_end (isr.c) e escalona as threads ready. CRITICO p/ o
//  explorer REAL: as threads RING-3 que ele cria (CreateThread / SHCreateThread
//  -> ki_launch_ring3_thread) SO rodam concorrentes com g_p4_active=1. Ligada
//  quando o .exe do shell vai subir, depois que todo o carregamento pesado de
//  drivers/DLLs ja rodou a toda velocidade (sem swap de contexto no init fragil).
//  g_p4_active e' referenciado por isr.c (gate do timer) e sched.c (ki_can_block).
// ============================================================================
volatile int g_p4_active = 0;

static int s_preempt_on = 0;
static void enable_preemption(void) {
    if (s_preempt_on) return;   // idempotente
    s_preempt_on = 1;
    __atomic_store_n(&g_p4_active, 1, __ATOMIC_SEQ_CST);
    kputs("[sched] preempcao LIGADA (g_p4_active=1). Timer 0xD1 escalona agora.\n");
}

// Substring simples (detecta o shell pelo nome do modulo, p/ ligar a preempcao
// logo antes de rodar o .exe do shell).
static int name_contains(const char* s, const char* needle) {
    if (!s || !needle) return 0;
    for (; *s; s++) {
        const char* a = s; const char* b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

extern int win32k_was_active(void);

void kmain(uint32_t mb_info) {
    vga_init();
    serial_init();

    vga_set_color(0x0A, 0x00);
    kputs("==================================================\n");
    kputs("   MeuOS  -  kernel 64 bits, escrito do zero em C\n");
    kputs("==================================================\n");
    vga_set_color(0x0F, 0x00);

    kputs("[ok] Long mode 64 bits + GDT + SSE\n");
    kputs("[ok] Video VGA + Serial COM1\n");

    gdt_init();
    kputs("[ok] GDT completa (ring 0 + ring 3) + TSS\n");

    idt_init();
    kputs("[ok] IDT carregada (256 vetores)\n");

    pic_remap();
    pit_init(100);
    kputs("[ok] PIC remapeado + PIT 100 Hz + teclado por IRQ\n");

    __asm__ volatile ("sti");
    kputs("[ok] Interrupcoes habilitadas (sti)\n");

    // Demonstra dispatch de excecao pela IDT (int3, nao-fatal):
    __asm__ volatile ("int3");

    // Prova o timer (IRQ0): espera ~0,5 s contando ticks.
    while (g_ticks < 50) __asm__ volatile ("hlt");
    kputs("[ok] Timer IRQ0 contando: ");
    kput_dec(g_ticks);
    kputs(" ticks em ~0,5s\n");

    // --- Gerencia de memoria (base para carregar programas) ---
    uint32_t mbflags = *(volatile uint32_t*)(uintptr_t)(mb_info + 0);
    uint64_t mem_top = 0x100000ULL;
    if (mbflags & 1) {                 // bit 0: campos mem_lower/mem_upper validos
        uint32_t mem_upper = *(volatile uint32_t*)(uintptr_t)(mb_info + 8);
        mem_top = 0x100000ULL + (uint64_t)mem_upper * 1024ULL;
    }
    kputs("[ok] RAM detectada: "); kput_dec(mem_top / 1024 / 1024); kputs(" MiB\n");

    pmm_init(mem_top);
    kputs("[ok] PMM: "); kput_dec(pmm_free_frames()); kputs(" frames de 4 KiB livres\n");

    heap_init();
    // Pilar 1: inicializa o MMIO arena ANTES de qualquer caminho que precise
    // mapear fisicos > 1 GiB (Local APIC no Pilar 2, virtio-gpu BAR, etc.).
    mm_mmio_init();
    void* a = kmalloc(64);
    void* b = kmalloc(4096);
    kputs("     kmalloc(64)    = "); kput_hex((uint64_t)(uintptr_t)a); kputc('\n');
    kputs("     kmalloc(4096)  = "); kput_hex((uint64_t)(uintptr_t)b); kputc('\n');
    kfree(a);
    void* d = kmalloc(32);
    kputs("     reuso pos-free  = "); kput_hex((uint64_t)(uintptr_t)d); kputc('\n');
    uint64_t fr = pmm_alloc_frame();
    kputs("     pmm_alloc_frame = "); kput_hex(fr); kputc('\n');
    kputs("[ok] Heap (kmalloc/kfree) + PMM operacionais\n");

    // --- HAL: I/O ports + MMIO + enumeracao PCI ---
    hal_init();

    // Pilar 1: PROVA de paginacao dinamica. Se falhar, NAO seguimos (paginacao
    // quebrada invalida APIC, SMP e tudo que vem depois).
    if (!proof_pillar1_paging()) {
        kputs("[P1] paginacao dinamica nao passou na prova; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // Pilar 2: APIC (Local APIC + IO-APIC) substituindo o 8259/PIT.
    if (!proof_pillar2_apic()) {
        kputs("[P2] APIC nao passou na prova; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // Pilar 3: SMP. O BSP precisa estar registrado no scheduler ANTES do AP
    // rodar ap_entry (ap_entry chama ki_init_processor que incrementa
    // g_ki_cpu_count). Idle thread do BSP fica pronta.
    ki_init_processor(0, apic_bsp_id());
    if (!proof_pillar3_smp()) {
        // Gate NAO-fatal: a prova roda e loga, mas o boot CONTINUA (o desktop
        // nao depende de SMP). Ver FUTURE.md.
        kputs("[P3] PARCIAL — SMP nao passou na prova; seguindo sem gate (ver FUTURE.md).\n");
    }

    // O LVT timer do BSP fica LIGADO: o timer ISR (0xD1) alimenta g_ticks +
    // mm_kuser_tick a cada tick (100 Hz) — relogio do sistema. A PREEMPCAO
    // (ki_quantum_end no timer) so entra quando g_p4_active=1, ligado adiante por
    // enable_preemption() ja com todos os subsistemas prontos.
    kputs("[apic] LVT timer do BSP ATIVO (relogio/g_ticks correndo; preempcao liga adiante)\n");

    hal_cpu_init();        // FASE 7: CPUID -> vendor/family/model + features
    // FASE FUNDACAO: calibra o TSC (KeStallExecutionProcessor / KeQueryPerformanceCounter).
    extern void hal_tsc_calibrate(void);
    hal_tsc_calibrate();
    // FASE FUNDACAO: subsistemas de fundacao (DPC per-CPU, KTIMER, modelo de interrupcao).
    extern void KiInitializeDpcSubsystem(void);
    KiInitializeDpcSubsystem();
    extern void KiInitializeTimerSubsystem(void);
    KiInitializeTimerSubsystem();
    extern void ke_interrupt_init(void);
    ke_interrupt_init();
    // FASE 7.7: CR4 + XCR0 (OSXSAVE/AVX/SMEP/UMIP/PCIDE quando o CPU expoe; cada
    // bit gateado por CPUID). Drivers reais (pintok.sys) leem CR4 e o valor bate com o NT.
    extern void cpu_features_init(void);
    cpu_features_init();
    // FASE 7.8: deteccao de HYPERVISOR via CPUID (0x40000000..).
    extern void hv_detect_init(void);
    hv_detect_init();
    // FASE 7.1: KUSER_SHARED_DATA em 0xFFFFF780_00000000 (drivers leem TickCount/NtVersion).
    extern void mm_map_kuser_shared_data(void);
    mm_map_kuser_shared_data();
    // FASE 7.2: KPCR em GS_BASE (gs:[..] -> CurrentThread/ProcessorNumber/etc).
    extern void kpcr_init(void);
    kpcr_init();
    // FASE 7.4: habilita a instrucao SYSCALL (EFER.SCE + STAR/LSTAR/SFMASK/CSTAR).
    // pintok.sys e ntdll.dll usam SYSCALL; sem SCE=1 a instrucao gera #GP.
    extern void syscall_msr_init(void);
    syscall_msr_init();
    kputs("[ok] HAL: I/O + MMIO + PCI + CPUID + KUSER_SHARED_DATA + KPCR/GS_BASE + SYSCALL\n");

    // --- FASE 10.1: detecta virtio-gpu (modern, virtio 1.1) ---
    if (virtio_gpu_detect()) {
        kputs("[ok] virtio-gpu: deteccao + MMIO + FEATURES_OK\n");
        if (virtio_gpu_smoke_test()) {
            kputs("[ok] virtio-gpu: smoke test passou (CREATE_2D/ATTACH/TRANSFER/FLUSH/UNREF + GET_DISPLAY_INFO)\n");
        } else {
            kputs("[ok] virtio-gpu: smoke test FALHOU (continuando com Bochs VBE)\n");
        }
    } else {
        kputs("[ok] virtio-gpu: indisponivel; Bochs VBE como caminho de video\n");
    }

    // --- FASE GPU: backend de display (virtio-gpu OU Bochs VBE) ---
    if (gpu_init(1024, 768)) {
        if (virtio_gpu_display_ok()) {
            kputs("[ok] GPU: virtio-gpu 1024x768x32 BGRA (SET_SCANOUT 0)\n");
        } else {
            kputs("[ok] GPU: Bochs VBE 1024x768x32 (LFB mapeado fora da identidade)\n");
        }
    } else {
        kputs("[ok] GPU: hardware nao detectado; mode13h fallback ativo\n");
    }

    // --- FASE DX: dxgkrnl + dxgmms (adapter primario display-only) ---
    DxgkInitialize();
    DxgMmsInitialize();
    kputs("[ok] DX: dxgkrnl + dxgmms inicializados (adapter primario pronto)\n");

    // --- FASE 11 (audio): HD Audio stub (deteccao PCI) ---
    hda_init();
    kputs("[ok] FASE 11: HD Audio detection (stack ring 3 mmdevapi/audioses/dsound/winmm disponivel)\n");

    // --- FASE 12 (network): NDIS + TCPIP + E1000 stub ---
    ndis_init();
    tcpip_init();
    e1000_init();
    kputs("[ok] FASE 12: NDIS + TCPIP + E1000 (stack ring 3 ws2_32.dll disponivel)\n");

    // --- FASE 13: USB (usbport+usbhub+xhci) + ACPI + PnP + FltMgr ---
    usbport_init();
    xhci_init();
    usbhub_init();
    acpi_init();
    pnp_init();
    fltmgr_init();
    kputs("[ok] FASE 13: USB (usbport+usbhub+xhci) + ACPI + PnP + FltMgr\n");

    ob_init();
    kputs("[ok] Object Manager + namespace (\\Device\\, \\Driver\\)\n");

    // FASE 7 — Driver Framework: tabelas de callbacks (Ps/Ob/Cm/Ex) + registro.
    callbacks_init();
    registry_init();
    kputs("[ok] FASE 7: Callbacks (Ps/Ob/Cm/Ex) + Registro em memoria (\\Registry\\)\n");

    // FASE 7.5: PspCidTable ANTES de ps_init/PsCreateProcess (as insercoes usam).
    cid_init();
    ps_init();
    kputs("[ok] Process Manager (EPROCESS/ETHREAD via Object Manager + PspCidTable)\n");

    win32k_init();          // fb_init e feito sob demanda (lazy) no 1o desenho
    kputs("[ok] win32k: window manager + filas de mensagens + GDI\n");

    // FASE 11 — driver PS/2 do mouse (IRQ12). DEPOIS de win32k_init + pic_remap.
    mouse_init();
    kputs("[ok] FASE 11: mouse PS/2 (IRQ12) + cursor sprite + WM_MOUSE* routing\n");

    // FASE 14 — Tablet ABSOLUTA (virtio-input): tira o QEMU do grab -> cursor de
    // HW do virtio-gpu visivel + coords absolutas p/ hit-test/cliques. Depende de
    // win32k_init (resolucao) p/ a escala abs->pixels.
    virtio_input_init();

    // --- Carrega os binarios Windows passados pelo boot (modulos Multiboot) ---
    // Nada e hardcoded: roda QUALQUER PE. Detecta pelo Subsystem:
    // NATIVE(1) -> driver .sys (executiva NT);  senao -> aplicativo .exe (Win32).
    vga_set_color(0x0B, 0x00);
    kputs("\n--- Binarios Windows recebidos do boot ---\n");
    vga_set_color(0x0F, 0x00);
    if (mbflags & (1u << 3)) {                                  // bit 3: modulos validos
        uint32_t mods_count = *(volatile uint32_t*)(uintptr_t)(mb_info + 20);
        uint32_t mods_addr  = *(volatile uint32_t*)(uintptr_t)(mb_info + 24);
        kputs("[boot] modulos: "); kput_dec(mods_count); kputc('\n');

        // GATE 2/5/6 do pintok.sys: registra PRIMEIRO um "ntoskrnl.exe" sintetico
        // com export table parseavel (as 217 funcoes que o pintok resolve por parse
        // manual da export directory). Tem que ser o modulo[0] que
        // ZwQuerySystemInformation(SystemModuleInformation) devolve.
        ldr_register_ntoskrnl_export_image();

        // Passo 1: registra TODOS os modulos por nome (as DLLs ficam disponiveis).
        for (uint32_t i = 0; i < mods_count; i++) {
            const uint32_t* m = (const uint32_t*)(uintptr_t)(mods_addr + i * 16);
            const void* bytes = (const void*)(uintptr_t)m[0];          // mod_start
            const char* path  = (const char*)(uintptr_t)m[2];          // string (nome)
            ldr_register(path, bytes);
        }
        // Passo 2: .sys -> driver (ring 0);  .exe -> roda em ring 3 (carrega as DLLs).
        for (uint32_t i = 0; i < mods_count; i++) {
            const uint32_t* m = (const uint32_t*)(uintptr_t)(mods_addr + i * 16);
            const void* bytes = (const void*)(uintptr_t)m[0];
            const char* path  = (const char*)(uintptr_t)m[2];
            if (ldr_match_ext(path, ".sys")) {
                kputs("\n[boot] driver de kernel: "); kputs(path); kputc('\n');
                driver_load(path, bytes);   // exercita I/O real internamente (antes do Unload)
            } else if (ldr_match_ext(path, ".exe")) {
                // Antes de subir o .exe do shell (explorer), LIGA a preempcao:
                // todo o carregamento pesado ja rodou a toda velocidade; agora as
                // threads ring-3 que o explorer criar rodam concorrentes.
                if (name_contains(path, "explorer")) enable_preemption();
                kputs("\n[boot] aplicativo: "); kputs(path); kputc('\n');
                ldr_run(path, bytes);
            }
            // .dll: carregada sob demanda pelo loader (LdrLoadDll)
        }
    } else {
        kputs("[boot] nenhum modulo. Rode:  .\\run.ps1\n");
    }

    // Fallback: se nenhum .exe de shell ligou a preempcao no laco, liga aqui —
    // assim o scheduler roda mesmo sem shell (ex.: cenario pintok, so .sys).
    enable_preemption();

    // Estado final da tela: se alguma app GUI compos o desktop (o explorer cria
    // janelas via win32k), o framebuffer ja reflete isso — deixamos como esta para
    // o screendump mostrar o ambiente.
    if (win32k_was_active()) {
        kputs("\n[win32k] estado final: desktop/janela(s) permanecem no framebuffer.\n");
    }

    vga_set_color(0x0E, 0x00);
    kputs("\nSistema no ar. Digite algo (teclado por interrupcao):\n");
    vga_set_color(0x0F, 0x00);
    kputs("> ");

    // Idle loop: STI antes do HLT. As IRQs (mouse IRQ12, teclado IRQ1) chegam pelo
    // IO-APIC e so sao servidas com IF=1. `sti; hlt` garante IF=1 e acorda a CPU a
    // cada IRQ pra servir o handler (padrao canonico de idle loop NT/x86).
    for (;;) {
        __asm__ volatile ("sti; hlt");   // ocioso: tudo acontece via interrupcoes
        virtio_input_poll();             // drena a tablet absoluta (sem IRQ wired)
    }
}
