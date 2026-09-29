// src/acpi.c — ACPI table discovery, power-off and reset
//
// The firmware leaves a chain of tables in RAM: the RSDP (found by scanning
// for its signature) names the RSDT/XSDT, which lists every other table. Two
// matter here. The FADT ("FACP") gives the PM1 control ports and the reset
// register; the MADT ("APIC") lists the CPUs and interrupt controllers. The
// DSDT holds the \_S5 object -- the value to write to PM1_CNT to enter soft
// off -- inside AML bytecode, which is not interpreted here; the four-byte
// name is found by search and the package after it decoded by hand.
//
// The kernel identity-maps only the RAM the multiboot map calls usable, and
// firmware tables live in a reserved region, sometimes above that. Every
// table is therefore mapped on first touch.
#include <stdint.h>
#include "header/acpi.h"
#include "header/io.h"
#include "header/kprintf.h"
#include "header/kstring.h"
#include "header/paging.h"

#define MAX_TABLES 64
#define MAX_TABLE_LEN (4u * 1024u * 1024u)    // anything longer is a corrupt header

static acpi_info_t info;
static uint32_t    tables[MAX_TABLES];
static int         ntables;

static uint8_t  rd8 (uint32_t a, uint32_t off){ return *(volatile const uint8_t*)(a + off); }
static uint16_t rd16(uint32_t a, uint32_t off){ return *(volatile const uint16_t*)(a + off); }
static uint32_t rd32(uint32_t a, uint32_t off){ return *(volatile const uint32_t*)(a + off); }

// Identity-maps [phys, phys+len) if it is not mapped already.
static void map_range(uint32_t phys, uint32_t len){
    uint32_t dir = paging_kernel_dir();
    uint32_t end = phys + len;
    for (uint32_t p = phys & 0xFFFFF000u; p < end; p += 0x1000u){
        if (!paging_phys_of(dir, p)) paging_map(p, p, PAGE_RW);
        if (p + 0x1000u < p) break;                    // wrapped past 4 GB
    }
}

int acpi_checksum_ok(const void* p, uint32_t len){
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

static int sig_is(uint32_t a, const char* s){
    for (int i = 0; i < 4; i++) if (rd8(a, (uint32_t)i) != (uint8_t)s[i]) return 0;
    return 1;
}

// Maps a whole table given its address and returns its length (0 if the
// header is implausible).
static uint32_t table_map(uint32_t a){
    map_range(a, 36);
    uint32_t len = rd32(a, 4);
    if (len < 36 || len > MAX_TABLE_LEN) return 0;
    map_range(a, len);
    return len;
}

// ------------------------------------------------------------------ RSDP

static int rsdp_at(uint32_t a){
    const char* s = "RSD PTR ";
    for (int i = 0; i < 8; i++) if (rd8(a, (uint32_t)i) != (uint8_t)s[i]) return 0;
    return acpi_checksum_ok((const void*)a, 20);
}

static uint32_t rsdp_scan(uint32_t lo, uint32_t hi){
    map_range(lo, hi - lo);
    for (uint32_t a = lo; a + 20 <= hi; a += 16)
        if (rsdp_at(a)) return a;
    return 0;
}

static uint32_t rsdp_find(void){
    // Real-mode segment of the EBDA, at 0x40E; the RSDP is in its first KB.
    uint32_t bda = 0x40E;
    __asm__("" : "+r"(bda));                      // hide the constant: page 0 is mapped, GCC cannot know
    uint32_t ebda = (uint32_t)rd16(bda, 0) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000){
        uint32_t r = rsdp_scan(ebda, ebda + 1024);
        if (r) return r;
    }
    return rsdp_scan(0xE0000, 0x100000);
}

// ------------------------------------------------------------------ FADT

static void parse_fadt(uint32_t a, uint32_t len){
    info.fadt_addr = a;
    info.fadt_rev  = rd8(a, 8);
    info.fadt_ok   = acpi_checksum_ok((const void*)a, len);
    if (len < 72) return;

    info.dsdt_addr        = rd32(a, 40);
    info.smi_cmd          = rd32(a, 48);
    info.acpi_enable_val  = rd8(a, 52);
    info.acpi_disable_val = rd8(a, 53);
    info.pm1a_cnt         = rd32(a, 64);
    info.pm1b_cnt         = rd32(a, 68);

    // Revision 2+ FADTs (>= 129 bytes) carry a Generic Address Structure for
    // the reset register at 116. Flags bit 10 says it is really implemented.
    if (len >= 129 && (rd32(a, 112) & (1u << 10))){
        info.reset_space = rd8(a, 116);
        info.reset_addr  = rd32(a, 120);
        info.reset_value = rd8(a, 128);
        // Only memory (0) and I/O (1) space, and a 32-bit-reachable address.
        if ((info.reset_space == 0 || info.reset_space == 1) && rd32(a, 124) == 0)
            info.have_reset_reg = 1;
    }
    // A DSDT pointer of zero falls through to the 64-bit field, if reachable.
    if (!info.dsdt_addr && len >= 148 && rd32(a, 144) == 0) info.dsdt_addr = rd32(a, 140);
}

// SCI_EN (PM1 control bit 0) says the chipset is in ACPI mode. QEMU's BIOS
// arrives with it set; on hardware that boots in legacy mode the OS asks the
// SMI handler to switch by writing ACPI_ENABLE to the SMI command port.
static void acpi_enable_mode(void){
    if (!info.pm1a_cnt) return;
    if (inw((uint16_t)info.pm1a_cnt) & 1u){ info.sci_en = 1; return; }
    if (!info.smi_cmd || !info.acpi_enable_val) return;

    outb((uint16_t)info.smi_cmd, info.acpi_enable_val);
    for (int i = 0; i < 300; i++){
        if (inw((uint16_t)info.pm1a_cnt) & 1u){ info.sci_en = 1; break; }
        for (int j = 0; j < 10000; j++) io_wait();
    }
}

// ------------------------------------------------------------------ \_S5

// Decodes one package element: ZeroOp, OneOp, or BytePrefix + byte. Returns
// bytes consumed, 0 if it is something else.
static int aml_small_int(const uint8_t* p, uint8_t* out){
    if (p[0] == 0x00){ *out = 0; return 1; }
    if (p[0] == 0x01){ *out = 1; return 1; }
    if (p[0] == 0x0A){ *out = p[1]; return 2; }
    return 0;
}

static void parse_s5(uint32_t dsdt, uint32_t len){
    const uint8_t* d = (const uint8_t*)dsdt;
    for (uint32_t i = 36; i + 8 < len; i++){
        if (d[i] != '_' || d[i+1] != 'S' || d[i+2] != '5' || d[i+3] != '_') continue;
        // Must be the definition (NameOp 0x08, optionally through a '\' root
        // prefix) and not a mention inside some method body.
        int named = (i >= 1 && d[i-1] == 0x08) || (i >= 2 && d[i-1] == 0x5C && d[i-2] == 0x08);
        if (!named || d[i+4] != 0x12) continue;              // PackageOp

        // PkgLength: the top two bits of the first byte give how many extra
        // length bytes follow. Then NumElements, then the elements.
        uint32_t o = i + 5;
        o += 1u + (uint32_t)(d[o] >> 6);
        if (o + 1 >= len) return;
        o++;                                                  // NumElements
        uint8_t a, b;
        int n = aml_small_int(d + o, &a);
        if (!n) continue;
        o += (uint32_t)n;
        n = aml_small_int(d + o, &b);
        if (!n) continue;
        info.slp_typa = a; info.slp_typb = b;
        info.s5_found = 1;
        return;
    }
}

// ------------------------------------------------------------------ MADT

static void parse_madt(uint32_t a, uint32_t len){
    info.madt_ok    = acpi_checksum_ok((const void*)a, len);
    info.lapic_addr = rd32(a, 36);
    info.madt_flags = rd32(a, 40);

    uint32_t o = 44;
    while (o + 2 <= len){
        uint8_t type = rd8(a, o), elen = rd8(a, o + 1);
        if (elen < 2 || o + elen > len) break;
        if (type == 0 && elen >= 8){
            uint32_t fl = rd32(a, o + 4);
            info.ncpus++;
            if (fl & 3u) info.ncpus_enabled++;      // enabled, or online-capable
            if (info.ncpus <= ACPI_MAX_CPUS){
                acpi_cpu_t* c = &info.cpus[info.ncpus - 1];
                c->acpi_id = rd8(a, o + 2);
                c->apic_id = rd8(a, o + 3);
                c->enabled = (uint8_t)(fl & 1u);
            }
        } else if (type == 1 && elen >= 12){
            if (info.nioapics < ACPI_MAX_IOAPICS){
                acpi_ioapic_t* io = &info.ioapics[info.nioapics];
                io->id = rd8(a, o + 2);
                io->addr = rd32(a, o + 4);
                io->gsi_base = rd32(a, o + 8);
            }
            info.nioapics++;
        } else if (type == 5 && elen >= 12){
            if (rd32(a, o + 8) == 0) info.lapic_addr = rd32(a, o + 4);   // 64-bit override, if reachable
        }
        o += elen;
    }
    if (info.ncpus > ACPI_MAX_CPUS) info.ncpus = ACPI_MAX_CPUS;
    if (info.nioapics > ACPI_MAX_IOAPICS) info.nioapics = ACPI_MAX_IOAPICS;
    if (info.ncpus_enabled > info.ncpus) info.ncpus_enabled = info.ncpus;
}

// ------------------------------------------------------------------ init

const acpi_info_t* acpi_info(void){ return &info; }

const void* acpi_find_table(const char sig[4]){
    for (int i = 0; i < ntables; i++)
        if (sig_is(tables[i], sig)) return (const void*)tables[i];
    return 0;
}

int acpi_init(void){
    if (info.valid) return 0;
    memset(&info, 0, sizeof(info));
    ntables = 0;

    uint32_t rsdp = rsdp_find();
    if (!rsdp){ kprintf("[acpi] no RSDP found, ACPI unavailable\n"); return -1; }

    info.rsdp_addr = rsdp;
    info.rsdp_rev  = rd8(rsdp, 15);
    info.rsdp_ok   = 1;                                  // rsdp_at already checked the first 20 bytes
    for (int i = 0; i < 6; i++) info.oem[i] = (char)rd8(rsdp, 9 + (uint32_t)i);
    info.oem[6] = 0;

    uint32_t rsdt = rd32(rsdp, 16);
    uint32_t xsdt = 0;
    if (info.rsdp_rev >= 2){
        // The extended checksum covers the whole 36-byte structure.
        if (!acpi_checksum_ok((const void*)rsdp, 36)) info.rsdp_ok = 0;
        else if (rd32(rsdp, 28) == 0) xsdt = rd32(rsdp, 24);   // above 4 GB is unreachable here
    }
    kprintf("[acpi] RSDP at 0x%x rev %u oem '%s'%s\n", (unsigned)rsdp,
            (unsigned)info.rsdp_rev, info.oem, info.rsdp_ok ? "" : " (bad extended checksum)");

    uint32_t root = 0, rlen = 0;
    int wide = 0;
    if (xsdt && (rlen = table_map(xsdt)) && sig_is(xsdt, "XSDT") && acpi_checksum_ok((const void*)xsdt, rlen)){
        root = xsdt; wide = 1;
    } else if (rsdt && (rlen = table_map(rsdt)) && sig_is(rsdt, "RSDT")){
        root = rsdt;
    }
    if (!root){ kprintf("[acpi] no usable RSDT/XSDT\n"); return -1; }

    info.used_xsdt = wide;
    info.root_addr = root;
    info.root_ok   = acpi_checksum_ok((const void*)root, rlen);

    uint32_t esz = wide ? 8u : 4u;
    for (uint32_t o = 36; o + esz <= rlen && ntables < MAX_TABLES; o += esz){
        if (wide && rd32(root, o + 4) != 0) continue;            // 64-bit address we cannot reach
        uint32_t t = rd32(root, o);
        if (!t) continue;
        if (!table_map(t)) continue;
        tables[ntables++] = t;
    }
    info.ntables = ntables;
    kprintf("[acpi] %s at 0x%x, %d tables%s\n", wide ? "XSDT" : "RSDT", (unsigned)root, ntables,
            info.root_ok ? "" : " (bad checksum)");

    info.valid = 1;

    const void* fadt = acpi_find_table("FACP");
    if (fadt){
        parse_fadt((uint32_t)fadt, rd32((uint32_t)fadt, 4));
        acpi_enable_mode();
        kprintf("[acpi] FADT rev %u: PM1a_CNT=0x%x PM1b_CNT=0x%x SMI_CMD=0x%x SCI_EN=%d reset=%s\n",
                (unsigned)info.fadt_rev, (unsigned)info.pm1a_cnt, (unsigned)info.pm1b_cnt,
                (unsigned)info.smi_cmd, info.sci_en,
                info.have_reset_reg ? (info.reset_space ? "io" : "mem") : "none");

        if (info.dsdt_addr){
            uint32_t dl = table_map(info.dsdt_addr);
            if (dl && sig_is(info.dsdt_addr, "DSDT")) parse_s5(info.dsdt_addr, dl);
        }
        if (info.s5_found)
            kprintf("[acpi] _S5 sleep type: SLP_TYPa=%u SLP_TYPb=%u\n",
                    (unsigned)info.slp_typa, (unsigned)info.slp_typb);
        else
            kprintf("[acpi] _S5 not found in DSDT (poweroff will use fallbacks)\n");
    } else {
        kprintf("[acpi] no FADT\n");
    }

    const void* madt = acpi_find_table("APIC");
    if (madt){
        parse_madt((uint32_t)madt, rd32((uint32_t)madt, 4));
        kprintf("[acpi] MADT: LAPIC at 0x%x, %d CPU(s) (%d enabled), %d IOAPIC(s)%s\n",
                (unsigned)info.lapic_addr, info.ncpus, info.ncpus_enabled, info.nioapics,
                info.madt_ok ? "" : " (bad checksum)");
        for (int i = 0; i < info.ncpus; i++)
            kprintf("[acpi]   cpu%d: apic id %u, acpi id %u, %s\n", i,
                    (unsigned)info.cpus[i].apic_id, (unsigned)info.cpus[i].acpi_id,
                    info.cpus[i].enabled ? "enabled" : "disabled");
        for (int i = 0; i < info.nioapics; i++)
            kprintf("[acpi]   ioapic id %u at 0x%x, gsi base %u\n",
                    (unsigned)info.ioapics[i].id, (unsigned)info.ioapics[i].addr,
                    (unsigned)info.ioapics[i].gsi_base);
    } else {
        kprintf("[acpi] no MADT\n");
    }
    return 0;
}

// -------------------------------------------------------------- power off

static void __attribute__((noreturn)) halt_forever(void){
    for (;;) __asm__ __volatile__("cli; hlt");
}

void acpi_poweroff(void){
    __asm__ __volatile__("cli");
    kprintf("[acpi] powering off\n");

    if (info.valid && info.s5_found && info.pm1a_cnt){
        // SLP_TYP in bits 12:10, SLP_EN in bit 13. B is written too when the
        // machine has a second register block.
        uint16_t a = (uint16_t)(((uint16_t)info.slp_typa << 10) | (1u << 13));
        uint16_t b = (uint16_t)(((uint16_t)info.slp_typb << 10) | (1u << 13));
        outw((uint16_t)info.pm1a_cnt, a);
        if (info.pm1b_cnt) outw((uint16_t)info.pm1b_cnt, b);
        for (int i = 0; i < 200000; i++) io_wait();
    }

    // Emulator-specific ports, for when the tables are missing or the write
    // above was ignored: QEMU (newer, then older/Bochs), and the port used
    // by VirtualBox and some other hypervisors.
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (int i = 0; i < 200000; i++) io_wait();

    kprintf("[acpi] poweroff did not take effect; halting\n");
    halt_forever();
}

// ------------------------------------------------------------------ reset

void acpi_reboot(void){
    __asm__ __volatile__("cli");
    kprintf("[acpi] rebooting\n");

    if (info.valid && info.have_reset_reg){
        if (info.reset_space == 1){
            outb((uint16_t)info.reset_addr, info.reset_value);
        } else {
            map_range(info.reset_addr, 1);
            *(volatile uint8_t*)info.reset_addr = info.reset_value;
        }
        for (int i = 0; i < 100000; i++) io_wait();
    }

    // The keyboard controller's output port bit 0 is wired to the CPU reset
    // line; command 0xFE pulses it. Wait for its input buffer to drain first.
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) io_wait();
    outb(0x64, 0xFE);
    for (int i = 0; i < 100000; i++) io_wait();

    // Last resort: an empty IDT makes the next interrupt unhandleable, which
    // escalates to a double fault and then a triple fault -- a CPU reset.
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ __volatile__("lidt %0; int3" :: "m"(null_idt));
    halt_forever();
}
