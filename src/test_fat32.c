// src/test_fat32.c — the ATA driver and the FAT32 filesystem
//
// These run against the real disk image over the real PIO path, so a pass
// means the bytes actually reached the drive and came back. Every test cleans
// up after itself: the image is shared with the running system and is not
// reformatted between runs, so a test that leaves rubbish behind is a test
// that breaks the Files app for whoever boots next.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/ata.h"
#include "header/fat32.h"

#define TEST_DIR fat32_root_cluster()

// Every test that touches the disk needs the same two preconditions, and
// "there is no disk attached" is a skip rather than a failure -- the harness
// is expected to run in configurations without one.
static int disk_ready(void){
    if (!ata_present()){ ktest_skip("no ATA device on the primary bus"); return 0; }
    if (!fat32_writable()){ ktest_skip("volume not mounted or not writable"); return 0; }
    return 1;
}

KTEST(ata, device_responds){
    if (!ata_present()){ ktest_skip("no ATA device on the primary bus"); return; }
    uint8_t sec[512];
    KT_EQ(ata_read_sectors(0, 1, sec), 0);
    // A FAT32 image made by mkfs.vfat always carries the boot signature.
    KT_EQ(sec[510], 0x55);
    KT_EQ(sec[511], 0xAA);
}

KTEST(ata, read_is_stable){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    uint8_t a[512], b[512];
    KT_EQ(ata_read_sectors(1, 1, a), 0);
    KT_EQ(ata_read_sectors(1, 1, b), 0);
    KT_MEMEQ(a, b, 512);
}

KTEST(ata, multi_sector_read_matches_single){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    uint8_t multi[512 * 4], one[512];
    KT_EQ(ata_read_sectors(0, 4, multi), 0);
    for (uint32_t s = 0; s < 4; s++){
        KT_EQ(ata_read_sectors(s, 1, one), 0);
        KT_MEMEQ(multi + s * 512, one, 512);
    }
}

KTEST(ata, write_roundtrip_restores_the_sector){
    if (!disk_ready()) return;

    // Scribble on a sector well past anything the filesystem uses, then put
    // back exactly what was there. Reading the original first is what makes
    // this safe to run against a live image.
    const uint32_t lba = 40000;
    uint8_t original[512], scratch[512], back[512];
    KT_EQ(ata_read_sectors(lba, 1, original), 0);

    for (int i = 0; i < 512; i++) scratch[i] = (uint8_t)(i * 7 + 3);
    KT_EQ(ata_write_sectors(lba, 1, scratch), 0);
    KT_EQ(ata_read_sectors(lba, 1, back), 0);
    KT_MEMEQ(back, scratch, 512);

    KT_EQ(ata_write_sectors(lba, 1, original), 0);
    KT_EQ(ata_read_sectors(lba, 1, back), 0);
    KT_MEMEQ(back, original, 512);
}

KTEST(fat32, volume_is_mounted){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    KT_TRUE(fat32_root_cluster() >= 2);
    KT_TRUE(fat32_bytes_per_cluster() >= 512);
    KT_EQ(fat32_bytes_per_cluster() % 512, 0);
}

KTEST(fat32, root_listing_does_not_fault){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    fat32_dirent_t ents[32];
    int n = fat32_list(fat32_root_cluster(), ents, 32);
    KT_TRUE(n >= 0);
    for (int i = 0; i < n; i++){
        KT_TRUE(strlen(ents[i].name) > 0);
        KT_TRUE(strlen(ents[i].name) < FAT32_NAME_MAX);
    }
}

KTEST(fat32, write_read_delete_roundtrip){
    if (!disk_ready()) return;

    const char* name = "KTWRITE.TXT";
    const char* body = "hawkOS write test: the quick brown fox jumps over the lazy dog.";
    uint32_t len = (uint32_t)strlen(body);

    KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)body, len), 0);

    fat32_dirent_t f;
    KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
    KT_EQ(f.size, len);
    KT_EQ(f.is_dir, 0);

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    KT_EQ(fat32_read_file(&f, buf, sizeof(buf)), len);
    KT_MEMEQ(buf, body, len);

    KT_EQ(fat32_delete(TEST_DIR, name), 0);
    KT_TRUE(fat32_find(TEST_DIR, name, &f) != 0);
}

KTEST(fat32, overwrite_replaces_contents_and_size){
    if (!disk_ready()) return;
    const char* name = "KTOVER.TXT";

    const char* first = "the first contents, which are longer than the second";
    KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)first, (uint32_t)strlen(first)), 0);

    const char* second = "short";
    KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)second, (uint32_t)strlen(second)), 0);

    fat32_dirent_t f;
    KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
    KT_EQ(f.size, strlen(second));

    // The old contents must be gone, not merely shortened past.
    uint8_t buf[128];
    memset(buf, 0xCC, sizeof(buf));
    KT_EQ(fat32_read_file(&f, buf, sizeof(buf)), strlen(second));
    KT_MEMEQ(buf, second, strlen(second));

    // And only one directory entry should exist for the name.
    fat32_dirent_t ents[64];
    int n = fat32_list(TEST_DIR, ents, 64);
    int hits = 0;
    for (int i = 0; i < n; i++) if (strcmp(ents[i].name, name) == 0) hits++;
    KT_EQ(hits, 1);

    KT_EQ(fat32_delete(TEST_DIR, name), 0);
}

KTEST(fat32, multi_cluster_file_roundtrips){
    if (!disk_ready()) return;
    const char* name = "KTBIG.BIN";

    // Comfortably more than one cluster, with a pattern that catches a
    // chain walked in the wrong order or a sector written twice.
    static uint8_t out[9000];
    static uint8_t in[9000];
    for (uint32_t i = 0; i < sizeof(out); i++)
        out[i] = (uint8_t)((i * 31u + (i >> 8)) & 0xFF);

    KT_EQ(fat32_write_file(TEST_DIR, name, out, sizeof(out)), 0);

    fat32_dirent_t f;
    KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
    KT_EQ(f.size, sizeof(out));

    memset(in, 0, sizeof(in));
    KT_EQ(fat32_read_file(&f, in, sizeof(in)), sizeof(out));
    KT_MEMEQ(in, out, sizeof(out));

    KT_EQ(fat32_delete(TEST_DIR, name), 0);
}

KTEST(fat32, empty_file_uses_no_clusters){
    if (!disk_ready()) return;
    const char* name = "KTEMPTY.TXT";

    uint32_t free_before = fat32_free_clusters();
    KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)"", 0), 0);

    fat32_dirent_t f;
    KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
    KT_EQ(f.size, 0);
    KT_EQ(f.first_cluster, 0);
    KT_EQ(fat32_free_clusters(), free_before);

    KT_EQ(fat32_delete(TEST_DIR, name), 0);
}

KTEST(fat32, delete_returns_the_clusters){
    if (!disk_ready()) return;
    const char* name = "KTFREE.BIN";

    uint32_t before = fat32_free_clusters();
    KT_TRUE(before > 8);

    static uint8_t blob[6000];
    memset(blob, 0xA5, sizeof(blob));
    KT_EQ(fat32_write_file(TEST_DIR, name, blob, sizeof(blob)), 0);
    KT_TRUE(fat32_free_clusters() < before);

    KT_EQ(fat32_delete(TEST_DIR, name), 0);
    KT_EQ(fat32_free_clusters(), before);
}

KTEST(fat32, many_files_do_not_leak_clusters){
    if (!disk_ready()) return;

    // Create, then remove, a batch, twice. Only the second round is measured.
    //
    // The first round may legitimately consume a cluster that never comes
    // back: a directory that runs out of entry slots grows by a cluster, and
    // FAT directories never shrink again -- deleting the files only marks the
    // slots free for reuse. Measuring a second, identical round is what
    // isolates the property actually worth asserting, which is that ordinary
    // steady-state use returns exactly what it takes.
    char name[16];
    char body[64];

    for (int round = 0; round < 2; round++){
        uint32_t before = fat32_free_clusters();

        for (int i = 0; i < 12; i++){
            ksnprintf(name, sizeof(name), "KTM%d.TXT", i);
            ksnprintf(body, sizeof(body), "file number %d with some payload", i);
            KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)body,
                                   (uint32_t)strlen(body)), 0);
        }
        for (int i = 0; i < 12; i++){
            ksnprintf(name, sizeof(name), "KTM%d.TXT", i);
            KT_EQ(fat32_delete(TEST_DIR, name), 0);
        }

        if (round == 1) KT_EQ(fat32_free_clusters(), before);
    }
}

KTEST(fat32, written_file_survives_a_remount){
    if (!disk_ready()) return;
    const char* name = "KTPERS.TXT";
    const char* body = "persisted across a remount";

    KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)body, (uint32_t)strlen(body)), 0);

    // Re-reading the BPB and walking the directory again from scratch proves
    // the entry went to the disk rather than to a cache in this driver.
    KT_EQ(fat32_init(), 0);

    fat32_dirent_t f;
    KT_EQ(fat32_find(fat32_root_cluster(), name, &f), 0);
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    KT_EQ(fat32_read_file(&f, buf, sizeof(buf)), strlen(body));
    KT_MEMEQ(buf, body, strlen(body));

    KT_EQ(fat32_delete(fat32_root_cluster(), name), 0);
}

KTEST(fat32, mkdir_creates_a_usable_directory){
    if (!disk_ready()) return;
    const char* dir = "KTDIR";

    // Clear a leftover from an interrupted earlier run, so the test starts
    // from a known state instead of failing for a reason that has nothing to
    // do with the code under test.
    //
    // The cluster number is copied out before anything else is called: GCC's
    // -Wdangling-pointer otherwise reports the struct as escaping, which it
    // does not -- first_cluster is a value -- but reading the field once and
    // working from that is clearer regardless.
    {
        fat32_dirent_t stale;
        if (fat32_find(TEST_DIR, dir, &stale) == 0){
            uint32_t stale_cluster = stale.first_cluster;
            fat32_delete(stale_cluster, "IN.TXT");
            fat32_rmdir(TEST_DIR, dir);
        }
    }

    KT_EQ(fat32_mkdir(TEST_DIR, dir), 0);

    fat32_dirent_t d;
    KT_EQ(fat32_find(TEST_DIR, dir, &d), 0);
    KT_EQ(d.is_dir, 1);
    KT_TRUE(d.first_cluster >= 2);

    // "." and ".." and nothing else.
    fat32_dirent_t ents[8];
    int n = fat32_list(d.first_cluster, ents, 8);
    KT_EQ(n, 2);
    if (n == 2){
        KT_STREQ(ents[0].name, ".");
        KT_STREQ(ents[1].name, "..");
    }

    // A file written inside it must be findable through the subdirectory.
    const char* body = "inside a subdirectory";
    KT_EQ(fat32_write_file(d.first_cluster, "IN.TXT", (const uint8_t*)body,
                           (uint32_t)strlen(body)), 0);
    fat32_dirent_t f;
    KT_EQ(fat32_find(d.first_cluster, "IN.TXT", &f), 0);
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    KT_EQ(fat32_read_file(&f, buf, sizeof(buf)), strlen(body));
    KT_MEMEQ(buf, body, strlen(body));

    // A directory with something still in it must not be removable.
    KT_TRUE(fat32_rmdir(TEST_DIR, dir) != 0);

    KT_EQ(fat32_delete(d.first_cluster, "IN.TXT"), 0);
    KT_EQ(fat32_rmdir(TEST_DIR, dir), 0);
    KT_TRUE(fat32_find(TEST_DIR, dir, &d) != 0);
}

KTEST(fat32, rmdir_rejects_files_and_missing_names){
    if (!disk_ready()) return;
    KT_TRUE(fat32_rmdir(TEST_DIR, "NOSUCH") != 0);

    KT_EQ(fat32_write_file(TEST_DIR, "KTNOTDIR.TXT", (const uint8_t*)"x", 1), 0);
    KT_TRUE(fat32_rmdir(TEST_DIR, "KTNOTDIR.TXT") != 0);   // it is a file
    KT_EQ(fat32_delete(TEST_DIR, "KTNOTDIR.TXT"), 0);
}

KTEST(fat32, refuses_bad_arguments){
    if (!disk_ready()) return;
    KT_TRUE(fat32_write_file(TEST_DIR, "", (const uint8_t*)"x", 1) != 0);
    KT_TRUE(fat32_write_file(TEST_DIR, "X.TXT", 0, 5) != 0);
    KT_TRUE(fat32_delete(TEST_DIR, "NOSUCH.TXT") != 0);
}

KTEST(fat32, name_matching_is_case_insensitive){
    if (!disk_ready()) return;
    const char* body = "case test";
    KT_EQ(fat32_write_file(TEST_DIR, "ktcase.txt", (const uint8_t*)body,
                           (uint32_t)strlen(body)), 0);

    // 8.3 names are stored upper-cased, and lookup has to fold either way.
    fat32_dirent_t f;
    KT_EQ(fat32_find(TEST_DIR, "KTCASE.TXT", &f), 0);
    KT_EQ(fat32_find(TEST_DIR, "ktcase.txt", &f), 0);
    KT_STREQ(f.name, "KTCASE.TXT");

    KT_EQ(fat32_delete(TEST_DIR, "KTCASE.TXT"), 0);
}
