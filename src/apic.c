// src/apic.c — local APIC: enable, timer calibration, optional periodic tick
//
// The LAPIC is memory-mapped at 4K somewhere the firmware chose (normally
// 0xFEE00000), reported by the IA32_APIC_BASE MSR and the MADT. It is mapped
// uncached, as device registers must be.
//
// The system tick stays on the PIT. Enabling the LAPIC is not free of risk on
// the legacy interrupt path: once it is software-enabled, the 8259 reaches the
// CPU only through LINT0, so LINT0 is programmed as ExtINT (virtual wire) or
// the timer, keyboard and disk would all go quiet. apic_init proves the PIT
// tick still advances before declaring success.
#include <stdint.h>
#include "header/apic.h"
#include "header/acpi.h"
#include "header/idt.h"
#include "header/io.h"
#include "header/pit.h"
#include "header/paging.h"
#include "header/kprintf.h"

#define LAPIC_ID     0x20
#define LAPIC_VER    0x30
#define LAPIC_TPR    0x80
#define LAPIC_EOI    0xB0
#define LAPIC_SVR    0xF0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define LVT_MASKED   (1u << 16)
#define LVT_PERIODIC (1u << 17)

#define MSR_APIC_BASE 0x1B

static volatile uint32_t* lapic;         // 0 until mapped
static int      ready;
static uint32_t base_phys;
static uint32_t timer_hz;                // divided counter clock, from calibration
static uint32_t target_hz;
static volatile uint32_t lapic_ticks;
static int      running;

static uint32_t rd(uint32_t reg){ return lapic[reg / 4]; }
static void     wr(uint32_t reg, uint32_t v){ lapic[reg / 4] = v; }

int apic_supported(void){
    uint32_t a = 1, b, c, d;
    __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d & (1u << 9)) && (d & (1u << 5));           // APIC and MSR
}

static void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi){
    __asm__ __volatile__("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}
static void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi){
    __asm__ __volatile__("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

// Interrupt entry points. Same shape as the PIC stubs in isr.asm: save
// registers, call C, restore, iret. The spurious vector must not EOI.
__asm__(
    ".pushsection .text\n"
    ".global apic_timer_stub\n"
    "apic_timer_stub:\n"
    "    pusha\n"
    "    call apic_timer_isr\n"
    "    popa\n"
    "    iret\n"
    ".global apic_spurious_stub\n"
    "apic_spurious_stub:\n"
    "    iret\n"
    ".popsection\n"
);
extern void apic_timer_stub(void);
extern void apic_spurious_stub(void);

void apic_timer_isr(void){
    lapic_ticks++;
    wr(LAPIC_EOI, 0);
}

uint32_t apic_base_addr(void){ return base_phys; }
int      apic_ready(void){ return ready; }
uint32_t apic_id(void){ return ready ? rd(LAPIC_ID) >> 24 : 0; }
uint32_t apic_version(void){ return ready ? rd(LAPIC_VER) & 0xFF : 0; }
uint32_t apic_timer_hz(void){ return timer_hz; }
int      apic_timer_running(void){ return running; }
uint32_t apic_timer_target_hz(void){ return target_hz; }
uint32_t apic_tick_count(void){ return lapic_ticks; }

static void lapic_disable(void){
    wr(LAPIC_LVT_TIMER, LVT_MASKED);
    wr(LAPIC_SVR, rd(LAPIC_SVR) & ~0x100u);
}

// Counts LAPIC timer decrements over a fixed PIT channel-2 interval. Divide
// by 16, one-shot from 0xFFFFFFFF, masked so it interrupts nothing.
static uint32_t calibrate(void){
    wr(LAPIC_TIMER_DIV, 0x3);                            // divide by 16
    wr(LAPIC_LVT_TIMER, LVT_MASKED | APIC_TIMER_VECTOR);
    wr(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    pit_wait_us(20000);
    uint32_t left = rd(LAPIC_TIMER_CUR);
    wr(LAPIC_TIMER_INIT, 0);
    return (0xFFFFFFFFu - left) * 50u;                   // per 20 ms -> per second
}

int apic_init(void){
    if (ready) return 0;
    if (!apic_supported()){ kprintf("[apic] no local APIC (cpuid)\n"); return -1; }

    uint32_t lo, hi;
    rdmsr(MSR_APIC_BASE, &lo, &hi);
    base_phys = lo & 0xFFFFF000u;
    if (!(lo & (1u << 11))){                             // globally disabled: enable it
        wrmsr(MSR_APIC_BASE, lo | (1u << 11), hi);
        rdmsr(MSR_APIC_BASE, &lo, &hi);
        if (!(lo & (1u << 11))){ kprintf("[apic] cannot enable LAPIC via MSR\n"); return -1; }
    }
    const acpi_info_t* ai = acpi_info();
    if (ai->valid && ai->lapic_addr && ai->lapic_addr != base_phys)
        kprintf("[apic] note: MADT says LAPIC at 0x%x, MSR says 0x%x; using the MSR\n",
                (unsigned)ai->lapic_addr, (unsigned)base_phys);

    // Uncached mapping: PCD (0x10) | PWT (0x08).
    if (!paging_phys_of(paging_kernel_dir(), base_phys))
        paging_map(base_phys, base_phys, PAGE_RW | 0x18u);
    lapic = (volatile uint32_t*)base_phys;

    set_gate(APIC_TIMER_VECTOR, (uint32_t)apic_timer_stub);
    set_gate(APIC_SPURIOUS_VECTOR, (uint32_t)apic_spurious_stub);

    wr(LAPIC_TPR, 0);
    wr(LAPIC_LVT_LINT0, 0x700);                          // ExtINT, unmasked: keeps the 8259 wired through
    wr(LAPIC_LVT_LINT1, 0x400);                          // NMI
    wr(LAPIC_LVT_ERROR, LVT_MASKED | 0xFE);
    wr(LAPIC_LVT_TIMER, LVT_MASKED | APIC_TIMER_VECTOR);
    wr(LAPIC_SVR, 0x100 | APIC_SPURIOUS_VECTOR);         // software enable

    // The PIT tick must survive the enable, or the whole machine is deaf.
    unsigned long long t0 = ticks;
    pit_wait_us(30000);
    pit_wait_us(30000);
    if (ticks == t0){
        kprintf("[apic] PIT tick stopped after enabling LAPIC; backing out\n");
        lapic_disable();
        lapic = 0;
        return -1;
    }

    timer_hz = calibrate();
    if (timer_hz < 100000u){
        kprintf("[apic] timer calibration implausible (%u Hz)\n", (unsigned)timer_hz);
        lapic_disable();
        lapic = 0;
        return -1;
    }
    ready = 1;
    kprintf("[apic] LAPIC id %u ver 0x%x at 0x%x, timer clock %u Hz (bus/16, PIT-calibrated)\n",
            (unsigned)apic_id(), (unsigned)apic_version(), (unsigned)base_phys, (unsigned)timer_hz);
    if (ai->valid)
        kprintf("[apic] %d CPU(s) reported by MADT; only the boot CPU is running, no AP is started\n",
                ai->ncpus);
    return 0;
}

int apic_timer_start(uint32_t hz){
    if (!ready || hz == 0 || hz > 100000u) return -1;
    uint32_t count = timer_hz / hz;
    if (count == 0) return -1;

    wr(LAPIC_TIMER_INIT, 0);
    target_hz = hz;
    wr(LAPIC_TIMER_DIV, 0x3);
    wr(LAPIC_LVT_TIMER, LVT_PERIODIC | APIC_TIMER_VECTOR);
    wr(LAPIC_TIMER_INIT, count);
    running = 1;
    return 0;
}

void apic_timer_stop(void){
    if (!ready) return;
    wr(LAPIC_LVT_TIMER, LVT_MASKED | APIC_TIMER_VECTOR);
    wr(LAPIC_TIMER_INIT, 0);
    running = 0;
}

// The single boot-time entry point: ACPI first (the MADT tells apic_init what
// the firmware thinks the machine looks like), then the LAPIC. Called from
// kernel_main once interrupts are on.
void hw_init(void){
    acpi_init();
    apic_init();
}
