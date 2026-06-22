// ============================================================================
//  FASE 7 — HAL: MSR + CPUID + Ke*PerformanceCounter.
// ============================================================================
#include "hal/cpu.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);
extern volatile uint64_t g_ticks;

static hal_cpu_info_t s_cpu;

// FASE 7.10: tracer global controlado por driver.c
extern volatile int g_pintok_trace;

MS_ABI uint64_t HalReadMsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    uint64_t v = ((uint64_t)hi << 32) | lo;
    if (g_pintok_trace) {
        kputs("  [trace] HalReadMsr(0x"); kput_hex(msr);
        kputs(") = "); kput_hex(v); kputs("\n");
    }
    return v;
}
MS_ABI void HalWriteMsr(uint32_t msr, uint64_t value) {
    if (g_pintok_trace) {
        kputs("  [trace] HalWriteMsr(0x"); kput_hex(msr);
        kputs(", "); kput_hex(value); kputs(")\n");
    }
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

MS_ABI void HalCpuid(uint32_t leaf, uint32_t subleaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    uint32_t ra, rb, rc, rd;
    __asm__ volatile ("cpuid"
        : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
        : "a"(leaf), "c"(subleaf));
    if (a) *a = ra; if (b) *b = rb; if (c) *c = rc; if (d) *d = rd;
    if (g_pintok_trace) {
        kputs("  [trace] HalCpuid(leaf=0x"); kput_hex(leaf);
        kputs(" sub=0x"); kput_hex(subleaf);
        kputs(") -> EAX="); kput_hex(ra);
        kputs(" EBX="); kput_hex(rb);
        kputs(" ECX="); kput_hex(rc);
        kputs(" EDX="); kput_hex(rd); kputs("\n");
    }
}

void hal_cpu_init(void) {
    uint32_t a, b, c, d;
    HalCpuid(0, 0, &a, &b, &c, &d);
    // vendor = ebx + edx + ecx (12 bytes)
    ((uint32_t*)s_cpu.vendor)[0] = b;
    ((uint32_t*)s_cpu.vendor)[1] = d;
    ((uint32_t*)s_cpu.vendor)[2] = c;
    s_cpu.vendor[12] = 0;
    HalCpuid(1, 0, &a, &b, &c, &d);
    uint8_t fam   = (uint8_t)((a >> 8) & 0xF);
    uint8_t model = (uint8_t)((a >> 4) & 0xF);
    uint8_t ext_fam = (uint8_t)((a >> 20) & 0xFF);
    uint8_t ext_model = (uint8_t)((a >> 16) & 0xF);
    if (fam == 0xF) fam = (uint8_t)(fam + ext_fam);
    if (fam == 0x6 || fam == 0xF) model = (uint8_t)(model | (ext_model << 4));
    s_cpu.family   = fam;
    s_cpu.model    = model;
    s_cpu.stepping = (uint8_t)(a & 0xF);
    s_cpu.feature_ecx = c;
    s_cpu.feature_edx = d;
    kputs("[cpu] vendor='"); kputs(s_cpu.vendor);
    kputs("' family="); kput_dec(s_cpu.family);
    kputs(" model=");   kput_dec(s_cpu.model);
    kputs(" stepping=");kput_dec(s_cpu.stepping); kputc('\n');
}
const hal_cpu_info_t* hal_cpu_get(void) { return &s_cpu; }

ULONG NTAPI KeQueryActiveProcessorCount_k(void* ActiveProcessors) {
    if (ActiveProcessors) *(uint64_t*)ActiveProcessors = 1;
    return 1;
}
ULONGLONG NTAPI KeQueryActiveProcessors_k(void) { return 1; }

uint64_t hal_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
void NTAPI KeQueryPerformanceCounter_k(PLARGE_INTEGER PerformanceFrequency) {
    if (PerformanceFrequency) PerformanceFrequency->QuadPart = 100;   // 100 Hz (PIT)
    // RAX retornaria o valor; mas a assinatura ms_abi devolveria LARGE_INTEGER.
    // Drivers que so leem Frequency ficam bem.
}
