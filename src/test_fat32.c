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

// ------------------------------------------------------------ long names

static int count_named(const char* name){
    fat32_dirent_t ents[128];
    int n = fat32_list(TEST_DIR, ents, 128);
    int hits = 0;
    for (int i = 0; i < n; i++) if (strcmp(ents[i].name, name) == 0) hits++;
    return hits;
}

KTEST(fat32, lfn_roundtrip){
    if (!disk_ready()) return;
    const char* name = "A Long File Name.txt";
    const char* body = "long names round-trip";
    fat32_dirent_t f;
    fat32_delete(TEST_DIR, name);                    // clear any leftover

    KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)body, (uint32_t)strlen(body)), 0);

    KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
    KT_STREQ(f.name, name);
    KT_STREQ(f.alias, "ALONGF~1.TXT");
    KT_EQ(f.size, strlen(body));

    // Lookup is case-insensitive, and the 8.3 alias reaches the same file.
    KT_EQ(fat32_find(TEST_DIR, "a LONG file NAME.TXT", &f), 0);
    KT_STREQ(f.name, name);
    KT_EQ(fat32_find(TEST_DIR, "alongf~1.txt", &f), 0);
    KT_STREQ(f.name, name);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    KT_EQ(fat32_read_file(&f, buf, sizeof(buf)), strlen(body));
    KT_MEMEQ(buf, body, strlen(body));

    // Listed once, under the long name -- not as a second ALONGF~1 entry.
    KT_EQ(count_named(name), 1);
    KT_EQ(count_named("ALONGF~1.TXT"), 0);

    // Overwriting keeps one entry and the same name.
    KT_EQ(fat32_write_file(TEST_DIR, "a long file name.txt", (const uint8_t*)"x", 1), 0);
    KT_EQ(count_named(name), 1);
    KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
    KT_EQ(f.size, 1);

    KT_EQ(fat32_delete(TEST_DIR, name), 0);
    KT_TRUE(fat32_find(TEST_DIR, name, &f) != 0);
    KT_TRUE(fat32_find(TEST_DIR, "ALONGF~1.TXT", &f) != 0);
    KT_EQ(count_named(name), 0);
}

KTEST(fat32, lfn_aliases_are_unique){
    if (!disk_ready()) return;
    const char* a = "A Long File Name.txt";
    const char* b = "A Long File Name 2.txt";
    const char* c = "A Long File Name.TXT.bak";
    fat32_delete(TEST_DIR, a); fat32_delete(TEST_DIR, b); fat32_delete(TEST_DIR, c);

    KT_EQ(fat32_write_file(TEST_DIR, a, (const uint8_t*)"a", 1), 0);
    KT_EQ(fat32_write_file(TEST_DIR, b, (const uint8_t*)"b", 1), 0);
    KT_EQ(fat32_write_file(TEST_DIR, c, (const uint8_t*)"c", 1), 0);

    fat32_dirent_t fa, fb, fc;
    KT_EQ(fat32_find(TEST_DIR, a, &fa), 0);
    KT_EQ(fat32_find(TEST_DIR, b, &fb), 0);
    KT_EQ(fat32_find(TEST_DIR, c, &fc), 0);
    KT_TRUE(strcmp(fa.alias, fb.alias) != 0);
    KT_TRUE(strcmp(fa.alias, fc.alias) != 0);
    KT_TRUE(strcmp(fb.alias, fc.alias) != 0);
    KT_STREQ(fa.alias, "ALONGF~1.TXT");
    KT_STREQ(fb.alias, "ALONGF~2.TXT");

    // Each still reads back its own contents (distinct chains, not aliased).
    uint8_t ch = 0;
    KT_EQ(fat32_read_file(&fb, &ch, 1), 1);
    KT_EQ(ch, 'b');

    KT_EQ(fat32_delete(TEST_DIR, a), 0);
    KT_EQ(fat32_delete(TEST_DIR, b), 0);
    KT_EQ(fat32_delete(TEST_DIR, c), 0);
}

KTEST(fat32, lfn_preserves_case_and_fits_8_3_when_it_can){
    if (!disk_ready()) return;
    fat32_dirent_t f;
    fat32_delete(TEST_DIR, "Readme.txt");

    KT_EQ(fat32_write_file(TEST_DIR, "Readme.txt", (const uint8_t*)"r", 1), 0);
    KT_EQ(fat32_find(TEST_DIR, "README.TXT", &f), 0);
    KT_STREQ(f.name, "Readme.txt");                  // case kept via an LFN
    KT_STREQ(f.alias, "README.TXT");                 // no numeric tail needed
    KT_EQ(fat32_delete(TEST_DIR, "readme.txt"), 0);

    // A plain upper-case name stays a bare 8.3 entry (no LFN written).
    KT_EQ(fat32_write_file(TEST_DIR, "KTPLAIN.TXT", (const uint8_t*)"p", 1), 0);
    KT_EQ(fat32_find(TEST_DIR, "ktplain.txt", &f), 0);
    KT_STREQ(f.name, "KTPLAIN.TXT");
    KT_STREQ(f.alias, "KTPLAIN.TXT");
    KT_EQ(fat32_delete(TEST_DIR, "KTPLAIN.TXT"), 0);
}

KTEST(fat32, lfn_rejects_illegal_names){
    if (!disk_ready()) return;
    KT_TRUE(fat32_write_file(TEST_DIR, "bad*name.txt", (const uint8_t*)"x", 1) != 0);
    KT_TRUE(fat32_write_file(TEST_DIR, "trailing.", (const uint8_t*)"x", 1) != 0);
    KT_TRUE(fat32_write_file(TEST_DIR, "a/b.txt", (const uint8_t*)"x", 1) != 0);
    static char big[200];
    memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    KT_TRUE(fat32_write_file(TEST_DIR, big, (const uint8_t*)"x", 1) != 0);   // > FAT32_NAME_LIMIT
}

KTEST(fat32, lfn_long_names_span_many_entries){
    if (!disk_ready()) return;
    // 100 characters: eight LFN entries plus the short entry.
    char name[128];
    for (int i = 0; i < 96; i++) name[i] = (char)((i % 26 < 13) ? ('a' + i % 26) : ('A' + i % 26 - 13));
    memcpy(name + 96, ".dat", 5);
    fat32_delete(TEST_DIR, name);

    KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)"z", 1), 0);
    fat32_dirent_t f;
    KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
    KT_STREQ(f.name, name);
    KT_EQ(count_named(name), 1);
    KT_EQ(fat32_delete(TEST_DIR, name), 0);
    KT_EQ(count_named(name), 0);
}

KTEST(fat32, lfn_runs_survive_sector_and_cluster_boundaries){
    if (!disk_ready()) return;
    // Enough three-entry files to cross several 16-entry sectors, so some LFN
    // runs straddle a sector boundary.
    enum { N = 24 };
    char name[48];
    int before = 0;
    { fat32_dirent_t e[128]; before = fat32_list(TEST_DIR, e, 128); }

    for (int i = 0; i < N; i++){
        ksnprintf(name, sizeof(name), "kt boundary test file number %d.txt", i);
        KT_EQ(fat32_write_file(TEST_DIR, name, (const uint8_t*)"b", 1), 0);
    }
    for (int i = 0; i < N; i++){
        ksnprintf(name, sizeof(name), "kt boundary test file number %d.txt", i);
        fat32_dirent_t f;
        KT_EQ(fat32_find(TEST_DIR, name, &f), 0);
        KT_STREQ(f.name, name);
    }
    { fat32_dirent_t e[128]; KT_EQ(fat32_list(TEST_DIR, e, 128), before + N); }

    for (int i = 0; i < N; i++){
        ksnprintf(name, sizeof(name), "kt boundary test file number %d.txt", i);
        KT_EQ(fat32_delete(TEST_DIR, name), 0);
    }
    { fat32_dirent_t e[128]; KT_EQ(fat32_list(TEST_DIR, e, 128), before); }
}

KTEST(fat32, lfn_rename_keeps_contents){
    if (!disk_ready()) return;
    const char* a = "KT old long name.txt";
    const char* b = "KT brand new long name.txt";
    const char* c = "KTNEW.TXT";
    fat32_delete(TEST_DIR, a); fat32_delete(TEST_DIR, b); fat32_delete(TEST_DIR, c);

    const char* body = "contents survive a rename";
    KT_EQ(fat32_write_file(TEST_DIR, a, (const uint8_t*)body, (uint32_t)strlen(body)), 0);

    KT_EQ(fat32_rename(TEST_DIR, a, TEST_DIR, b), 0);
    fat32_dirent_t f;
    KT_TRUE(fat32_find(TEST_DIR, a, &f) != 0);
    KT_EQ(fat32_find(TEST_DIR, b, &f), 0);
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    KT_EQ(fat32_read_file(&f, buf, sizeof(buf)), strlen(body));
    KT_MEMEQ(buf, body, strlen(body));

    // Long -> short, and onto an existing name (refused).
    KT_EQ(fat32_rename(TEST_DIR, b, TEST_DIR, c), 0);
    KT_EQ(fat32_find(TEST_DIR, c, &f), 0);
    KT_STREQ(f.name, c);
    KT_EQ(fat32_write_file(TEST_DIR, a, (const uint8_t*)"x", 1), 0);
    KT_TRUE(fat32_rename(TEST_DIR, a, TEST_DIR, c) != 0);

    // Case-only change within a directory.
    KT_EQ(fat32_rename(TEST_DIR, c, TEST_DIR, "KtNew.Txt"), 0);
    KT_EQ(fat32_find(TEST_DIR, "ktnew.txt", &f), 0);
    KT_STREQ(f.name, "KtNew.Txt");
    KT_EQ(count_named("KtNew.Txt"), 1);

    KT_EQ(fat32_delete(TEST_DIR, "KtNew.Txt"), 0);
    KT_EQ(fat32_delete(TEST_DIR, a), 0);
}

KTEST(fat32, lfn_directories_and_moves){
    if (!disk_ready()) return;
    fat32_dirent_t d, f;
    if (fat32_find(TEST_DIR, "KT Long Dir", &d) == 0){
        uint32_t cl = d.first_cluster;
        fat32_delete(cl, "inner file.txt");
        fat32_rmdir(TEST_DIR, "KT Long Dir");
    }
    fat32_delete(TEST_DIR, "KT mover.txt");

    KT_EQ(fat32_mkdir(TEST_DIR, "KT Long Dir"), 0);
    KT_EQ(fat32_find(TEST_DIR, "kt long dir", &d), 0);
    KT_EQ(d.is_dir, 1);
    KT_STREQ(d.name, "KT Long Dir");
    uint32_t dc = d.first_cluster;

    // Move a file into the directory and back out again.
    KT_EQ(fat32_write_file(TEST_DIR, "KT mover.txt", (const uint8_t*)"m", 1), 0);
    KT_EQ(fat32_rename(TEST_DIR, "KT mover.txt", dc, "inner file.txt"), 0);
    KT_TRUE(fat32_find(TEST_DIR, "KT mover.txt", &f) != 0);
    KT_EQ(fat32_find(dc, "inner file.txt", &f), 0);
    KT_TRUE(fat32_rmdir(TEST_DIR, "KT Long Dir") != 0);        // not empty
    KT_EQ(fat32_rename(dc, "inner file.txt", TEST_DIR, "KT mover.txt"), 0);
    KT_EQ(fat32_find(TEST_DIR, "KT mover.txt", &f), 0);
    KT_EQ(fat32_delete(TEST_DIR, "KT mover.txt"), 0);

    KT_EQ(fat32_rmdir(TEST_DIR, "KT Long Dir"), 0);
    KT_TRUE(fat32_find(TEST_DIR, "KT Long Dir", &d) != 0);
}

KTEST(fat32, read_at_matches_whole_file_reads){
    if (!disk_ready()) return;
    static uint8_t out[5000], in[5000];
    for (uint32_t i = 0; i < sizeof(out); i++) out[i] = (uint8_t)(i * 13u + (i >> 9));
    KT_EQ(fat32_write_file(TEST_DIR, "KTRDAT.BIN", out, sizeof(out)), 0);
    fat32_dirent_t f;
    KT_EQ(fat32_find(TEST_DIR, "KTRDAT.BIN", &f), 0);

    // Odd-sized sequential reads that straddle sector and cluster edges.
    uint32_t off = 0;
    while (off < sizeof(out)){
        uint32_t n = fat32_read_at(&f, off, in + off, 333);
        KT_TRUE(n > 0);
        if (n == 0) break;
        off += n;
    }
    KT_EQ(off, sizeof(out));
    KT_MEMEQ(in, out, sizeof(out));

    // Random access, backwards, and reads at/after EOF.
    uint8_t b[7];
    KT_EQ(fat32_read_at(&f, 4993, b, 7), 7);
    KT_MEMEQ(b, out + 4993, 7);
    KT_EQ(fat32_read_at(&f, 10, b, 7), 7);
    KT_MEMEQ(b, out + 10, 7);
    KT_EQ(fat32_read_at(&f, 4998, b, 7), 2);
    KT_EQ(fat32_read_at(&f, 5000, b, 7), 0);
    KT_EQ(fat32_delete(TEST_DIR, "KTRDAT.BIN"), 0);
}
