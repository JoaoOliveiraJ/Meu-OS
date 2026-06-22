#include <stdint.h>
#include <stddef.h>
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "cpu/pit.h"
#include "cpu/isr.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "loader/pe.h"
#include "nt/driver.h"

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

    // --- Carrega os binarios Windows passados pelo boot (modulos Multiboot) ---
    // Nada e hardcoded: roda QUALQUER PE passado. Detecta pelo Subsystem:
    // NATIVE(1) -> driver .sys (executiva NT);  senao -> aplicativo .exe (Win32).
    vga_set_color(0x0B, 0x00);
    kputs("\n--- Binarios Windows recebidos do boot ---\n");
    vga_set_color(0x0F, 0x00);
    if (mbflags & (1u << 3)) {                                  // bit 3: modulos validos
        uint32_t mods_count = *(volatile uint32_t*)(uintptr_t)(mb_info + 20);
        uint32_t mods_addr  = *(volatile uint32_t*)(uintptr_t)(mb_info + 24);
        kputs("[boot] modulos: "); kput_dec(mods_count); kputc('\n');
        for (uint32_t i = 0; i < mods_count; i++) {
            const uint32_t* m = (const uint32_t*)(uintptr_t)(mods_addr + i * 16);
            const void* image = (const void*)(uintptr_t)m[0];  // mod_start
            int ss = pe_subsystem(image);
            if (ss == 1) { kputs("\n[boot] modulo NATIVE -> driver .sys:\n"); driver_load(image); }
            else if (ss > 0) { kputs("\n[boot] modulo -> aplicativo .exe:\n"); pe_run(image); }
            else kputs("\n[boot] modulo nao e um PE valido; ignorado.\n");
        }
    } else {
        kputs("[boot] nenhum modulo. Ex.:  .\\run.ps1  (passa os exemplos)\n");
        kputs("       ou  .\\run.ps1 -Modules C:\\caminho\\app.exe\n");
    }

    vga_set_color(0x0E, 0x00);
    kputs("\nSistema no ar. Digite algo (teclado por interrupcao):\n\n");
    vga_set_color(0x0F, 0x00);
    kputs("> ");

    for (;;) __asm__ volatile ("hlt");   // ocioso: tudo acontece via interrupcoes
}
