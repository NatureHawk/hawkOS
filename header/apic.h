#pragma once
#include <stdint.h>

// Local APIC of the boot CPU: detection, enabling, and a timer calibrated
// against the PIT. Other CPUs are only counted (see acpi.h) -- no AP is
// started.
//
// The 100 Hz PIT tick remains the system clock. The LAPIC timer is an opt-in
// second tick source that counts into its own counter and never touches
// `ticks` or the scheduler.

#define APIC_TIMER_VECTOR 0x40
#define APIC_SPURIOUS_VECTOR 0xFF

// Call after interrupts are on (it checks the PIT tick still runs once the
// LAPIC is enabled, and backs out if it does not). Returns 0 if the LAPIC is
// enabled and the timer calibrated, -1 otherwise.
int      apic_init(void);

int      apic_supported(void);        // CPUID says a LAPIC exists
int      apic_ready(void);            // enabled, timer calibrated
uint32_t apic_base_addr(void);
uint32_t apic_id(void);
uint32_t apic_version(void);
uint32_t apic_timer_hz(void);         // calibrated counter clock (bus clock / 16)

// Periodic LAPIC timer interrupts at ~hz (1..100000). 0 on success.
int      apic_timer_start(uint32_t hz);
void     apic_timer_stop(void);
int      apic_timer_running(void);
uint32_t apic_timer_target_hz(void);
uint32_t apic_tick_count(void);       // LAPIC interrupts since boot
