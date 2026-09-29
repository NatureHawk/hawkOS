// src/test_hw.c — ACPI tables, the local APIC and ATA bus-master DMA
//
// Like the rest of the suite these run against whatever machine the kernel
// booted on. Tests that need a feature the machine lacks skip rather than
// fail. The DMA write test uses sectors far from the FAT32 metadata and the
// files the other tests touch, and restores what it overwrote.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/acpi.h"
#include "header/apic.h"
#include "header/ata.h"
#include "header/pit.h"

static int acpi_ready(void){
    if (!acpi_info()->valid){ ktest_skip("no ACPI tables on this machine"); return 0; }
    return 1;
}

KTEST(acpi, rsdp_and_root_checksums){
    if (!acpi_ready()) return;
    const acpi_info_t* a = acpi_info();
    KT_TRUE(a->rsdp_ok);
    KT_TRUE(a->root_ok);
    KT_TRUE(acpi_checksum_ok((const void*)a->rsdp_addr, 20));
    KT_TRUE(a->ntables > 0);
}

KTEST(acpi, fadt_valid){
    if (!acpi_ready()) return;
    const acpi_info_t* a = acpi_info();
    const uint8_t* f = (const uint8_t*)acpi_find_table("FACP");
    KT_NOTNULL(f);
    if (!f) return;
    KT_TRUE(a->fadt_ok);
    KT_TRUE(acpi_checksum_ok(f, *(const uint32_t*)(f + 4)));
    KT_NE(a->pm1a_cnt, 0);
    KT_TRUE(a->sci_en);
}

KTEST(acpi, s5_sleep_type_found){
    if (!acpi_ready()) return;
    const acpi_info_t* a = acpi_info();
    KT_TRUE(a->s5_found);
    // SLP_TYP is a 3-bit field.
    KT_TRUE(a->slp_typa < 8);
    KT_TRUE(a->slp_typb < 8);
}

KTEST(acpi, madt_sane){
    if (!acpi_ready()) return;
    const acpi_info_t* a = acpi_info();
    KT_TRUE(a->madt_ok);
    KT_TRUE(a->ncpus >= 1);
    KT_TRUE(a->ncpus_enabled >= 1);
    KT_TRUE(a->ncpus_enabled <= a->ncpus);
    KT_EQ(a->lapic_addr & 0xFFF, 0);
    KT_NE(a->lapic_addr, 0);
    // The boot CPU's APIC id must be one of the listed processors.
    if (apic_ready()){
        int found = 0;
        for (int i = 0; i < a->ncpus; i++) if (a->cpus[i].apic_id == apic_id()) found = 1;
        KT_TRUE(found);
    }
    for (int i = 0; i < a->nioapics; i++) KT_EQ(a->ioapics[i].addr & 0xFFF, 0);
}

KTEST(apic, timer_ticks_at_requested_rate){
    if (!apic_ready()){ ktest_skip("no usable local APIC"); return; }
    uint32_t l0 = apic_tick_count();
    KT_EQ(apic_timer_start(100), 0);
    unsigned long long p0 = ticks;
    while (ticks - p0 < 50) __asm__ __volatile__("hlt");      // 500 ms of PIT time
    apic_timer_stop();
    uint32_t got = apic_tick_count() - l0;
    // 50 expected; virtual-machine clocks jitter, so allow a wide band.
    KT_TRUE(got >= 30 && got <= 75);
    // The PIT tick must be untouched by all of this.
    KT_TRUE(ticks - p0 >= 50);
    KT_FALSE(apic_timer_running());
    uint32_t after = apic_tick_count();
    unsigned long long q0 = ticks;
    while (ticks - q0 < 10) __asm__ __volatile__("hlt");
    KT_EQ(apic_tick_count(), after);                            // really stopped
}

// A spread that covers sector 0, a 64-sector chunk boundary, and a run long
// enough to need several bounce-buffer chunks.
KTEST(ata, dma_read_matches_pio){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    if (!ata_dma_available()){ ktest_skip("no IDE bus-master DMA"); return; }

    static const struct { uint32_t lba; uint8_t n; } cases[] = {
        { 0, 1 }, { 1, 3 }, { 60, 8 }, { 2048, 64 }, { 100, 130 }, { 5000, 200 },
    };
    static uint8_t a[200 * 512], b[200 * 512];
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++){
        uint32_t bytes = (uint32_t)cases[i].n * 512u;
        memset(a, 0xA5, bytes); memset(b, 0x5A, bytes);
        KT_EQ(ata_read_sectors_pio(cases[i].lba, cases[i].n, a), 0);
        KT_EQ(ata_read_sectors_dma(cases[i].lba, cases[i].n, b), 0);
        KT_MEMEQ(a, b, bytes);
    }
}

KTEST(ata, dma_write_roundtrip_on_scratch_sectors){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    if (!ata_dma_available()){ ktest_skip("no IDE bus-master DMA"); return; }

    // Two runs well past FAT32 metadata and the sectors test_fat32 borrows
    // (40000); saved first and restored last, so the image is left as found.
    const uint32_t lba = 100000, n = 70;                // > one 64-sector chunk
    static uint8_t orig[70 * 512], pat[70 * 512], viadma[70 * 512], viapio[70 * 512];
    KT_EQ(ata_read_sectors_pio(lba, (uint8_t)n, orig), 0);

    for (uint32_t i = 0; i < sizeof(pat); i++) pat[i] = (uint8_t)((i * 131u + (i >> 9) * 7u + 3u) & 0xFF);
    KT_EQ(ata_write_sectors_dma(lba, (uint8_t)n, pat), 0);
    KT_EQ(ata_read_sectors_dma(lba, (uint8_t)n, viadma), 0);
    KT_EQ(ata_read_sectors_pio(lba, (uint8_t)n, viapio), 0);
    KT_MEMEQ(viadma, pat, sizeof(pat));
    KT_MEMEQ(viapio, pat, sizeof(pat));

    // And the transparent API, with DMA disabled then enabled, agrees.
    ata_dma_enable(0);
    KT_EQ(ata_write_sectors(lba, 5, orig), 0);
    ata_dma_enable(1);
    KT_EQ(ata_read_sectors(lba, 5, viadma), 0);
    KT_MEMEQ(viadma, orig, 5 * 512);

    KT_EQ(ata_write_sectors_dma(lba, (uint8_t)n, orig), 0);
    KT_EQ(ata_read_sectors_pio(lba, (uint8_t)n, viapio), 0);
    KT_MEMEQ(viapio, orig, sizeof(orig));
}

KTEST(ata, transparent_api_uses_dma_when_available){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    if (!ata_dma_available()){ ktest_skip("no IDE bus-master DMA"); return; }
    ata_stats_t s0, s1;
    uint8_t buf[512 * 8];
    ata_stats(&s0);
    KT_EQ(ata_read_sectors(0, 8, buf), 0);
    ata_stats(&s1);
    KT_EQ(s1.dma_ops, s0.dma_ops + 1);
    KT_EQ(s1.errors, s0.errors);
}
