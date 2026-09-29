#pragma once
#include <stdint.h>

// ACPI: just enough to find the tables, learn the machine's CPUs and APICs,
// and turn it off or reset it properly.
//
// Nothing here runs AML. The one thing the firmware normally hides in bytecode
// that power-off needs -- the \_S5 sleep type -- is fished out of the DSDT by
// pattern, which is what every hobby kernel (and a fair few real ones) does.

#define ACPI_MAX_CPUS    16
#define ACPI_MAX_IOAPICS 4

typedef struct {
    uint8_t  apic_id;
    uint8_t  acpi_id;
    uint8_t  enabled;
} acpi_cpu_t;

typedef struct {
    uint8_t  id;
    uint32_t addr;
    uint32_t gsi_base;
} acpi_ioapic_t;

typedef struct {
    int      valid;              // RSDP found and its checksum(s) good
    uint32_t rsdp_addr;
    uint8_t  rsdp_rev;           // 0 = ACPI 1.0, >= 2 has an XSDT
    int      rsdp_ok;            // first 20 bytes sum to 0 (and extended, if rev >= 2)
    char     oem[7];

    int      used_xsdt;          // 1 if the XSDT (not the RSDT) was walked
    uint32_t root_addr;          // physical address of the table walked
    int      root_ok;
    int      ntables;

    uint32_t fadt_addr;
    int      fadt_ok;            // FADT found, checksum valid
    uint8_t  fadt_rev;
    uint32_t smi_cmd;
    uint8_t  acpi_enable_val, acpi_disable_val;
    int      sci_en;             // ACPI mode already on (or turned on by us)
    uint32_t pm1a_cnt, pm1b_cnt;
    uint32_t dsdt_addr;

    int      have_reset_reg;
    uint8_t  reset_space;        // 0 = memory, 1 = I/O
    uint32_t reset_addr;
    uint8_t  reset_value;

    int      s5_found;
    uint8_t  slp_typa, slp_typb;

    int      madt_ok;
    uint32_t lapic_addr;
    uint32_t madt_flags;         // bit 0: dual 8259 present
    int      ncpus;              // every LAPIC entry
    int      ncpus_enabled;
    acpi_cpu_t cpus[ACPI_MAX_CPUS];
    int      nioapics;
    acpi_ioapic_t ioapics[ACPI_MAX_IOAPICS];
} acpi_info_t;

// Finds and parses everything and logs `[acpi] ...` lines. Safe to call once
// early; returns 0 if usable tables were found, -1 if not (poweroff/reboot
// then use the non-ACPI fallbacks only).
int  acpi_init(void);

const acpi_info_t* acpi_info(void);

// Tables can be looked up again by signature ("APIC", "FACP", ...). Returns
// the table's physical (== virtual) address, or 0.
const void* acpi_find_table(const char sig[4]);

// Checksum helper, exposed for tests: 1 if `len` bytes at p sum to 0 mod 256.
int  acpi_checksum_ok(const void* p, uint32_t len);

// Neither returns. Power-off: PM1 control write with \_S5, then QEMU/Bochs/
// VirtualBox ports, then halt. Reboot: FADT reset register, 8042 pulse, then
// a deliberate triple fault.
void acpi_poweroff(void) __attribute__((noreturn));
void acpi_reboot(void)   __attribute__((noreturn));
