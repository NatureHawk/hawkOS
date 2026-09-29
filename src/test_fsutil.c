// src/test_fsutil.c — the clipboard and the file operations behind Files
//
// The clipboard tests need nothing but memory. The file-operation tests run
// against the real disk image like test_fat32.c does, build their own scratch
// folders under names no other test or user is going to use, and remove them
// again; they skip rather than fail when there is no writable disk.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/ata.h"
#include "header/fat32.h"
#include "header/fsutil.h"
#include "header/clipboard.h"

#define ROOT fat32_root_cluster()

static int disk_ready(void){
    if (!ata_present()){ ktest_skip("no ATA device on the primary bus"); return 0; }
    if (!fat32_writable()){ ktest_skip("volume not mounted or not writable"); return 0; }
    return 1;
}

// ---------------------------------------------------------------- clipboard

KTEST(clipboard, text_round_trips_and_is_replaced){
    clip_set_text("hello", 5);
    uint32_t n = 0;
    const char* t = clip_text(&n);
    KT_EQ(n, 5);
    KT_STREQ(t, "hello");

    clip_set_text("bye", 3);
    t = clip_text(&n);
    KT_EQ(n, 3);
    KT_STREQ(t, "bye");
    KT_EQ(clip_text_len(), 3);

    clip_set_text(0, 0);
    KT_EQ(clip_text_len(), 0);
    KT_STREQ(clip_text(&n), "");
}

KTEST(clipboard, copying_text_leaves_a_pending_cut_alone){
    static const char names[2][CLIP_NAME_MAX] = { "A.TXT", "B.TXT" };
    clip_set_files(1234, names, 2, 1);
    clip_set_text("sentence", 8);

    KT_EQ(clip_file_count(), 2);
    KT_EQ(clip_files_cut(), 1);
    KT_EQ(clip_files_dir(), 1234);
    KT_STREQ(clip_file_name(1), "B.TXT");
    KT_STREQ(clip_file_name(2), "");           // out of range is empty, not a crash

    clip_files_clear();
    KT_EQ(clip_file_count(), 0);
    KT_EQ(clip_files_cut(), 0);
    clip_set_text(0, 0);
}

// -------------------------------------------------------------------- names

KTEST(fsutil, names_reject_what_no_filesystem_accepts){
    KT_TRUE(fs_name_ok("notes.txt"));
    KT_TRUE(fs_name_ok("New Text Document.txt"));
    KT_TRUE(fs_name_ok(".profile"));
    KT_FALSE(fs_name_ok(""));
    KT_FALSE(fs_name_ok("."));
    KT_FALSE(fs_name_ok(".."));
    KT_FALSE(fs_name_ok("a/b"));
    KT_FALSE(fs_name_ok("a\\b"));
    KT_FALSE(fs_name_ok("what?"));
    KT_FALSE(fs_name_ok("star*"));
    KT_FALSE(fs_name_ok("pipe|"));
    KT_FALSE(fs_name_ok("trailing "));
    KT_FALSE(fs_name_ok("trailing."));
}

KTEST(fsutil, text_detection){
    KT_TRUE(fs_is_text_name("README.TXT"));
    KT_TRUE(fs_is_text_name("main.c"));
    KT_TRUE(fs_is_text_name("page.HTM"));
    KT_FALSE(fs_is_text_name("HI.ELF"));
    KT_FALSE(fs_is_text_name("NOEXT"));
    KT_FALSE(fs_is_text_name("trailingdot."));

    KT_TRUE(fs_looks_like_text((const uint8_t*)"plain words\n\tand a tab", 21));
    KT_TRUE(fs_looks_like_text((const uint8_t*)"", 0));
    const uint8_t elf[] = { 0x7F, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0 };
    KT_FALSE(fs_looks_like_text(elf, sizeof(elf)));       // a NUL byte settles it
}

// -------------------------------------------------------------- on the disk

static int read_back(uint32_t dir, const char* name, char* out, uint32_t cap){
    fat32_dirent_t e;
    if (fat32_find(dir, name, &e) != 0) return -1;
    uint32_t n = fat32_read_file(&e, (uint8_t*)out, cap - 1);
    out[n] = 0;
    return (int)n;
}

KTEST(fsutil, unique_name_counts_up_past_existing_copies){
    if (!disk_ready()) return;
    char out[FAT32_NAME_MAX];

    fs_unique_name(ROOT, "KTUNIQ.TXT", out, sizeof(out));
    KT_STREQ(out, "KTUNIQ.TXT");                          // free: unchanged

    KT_EQ(fat32_write_file(ROOT, "KTUNIQ.TXT", (const uint8_t*)"x", 1), 0);
    fs_unique_name(ROOT, "KTUNIQ.TXT", out, sizeof(out));
    KT_STREQ(out, "KTUNIQ - Copy.TXT");

    KT_EQ(fat32_write_file(ROOT, out, (const uint8_t*)"x", 1), 0);
    fs_unique_name(ROOT, "KTUNIQ.TXT", out, sizeof(out));
    KT_STREQ(out, "KTUNIQ - Copy (2).TXT");

    KT_EQ(fat32_delete(ROOT, "KTUNIQ - Copy.TXT"), 0);
    KT_EQ(fat32_delete(ROOT, "KTUNIQ.TXT"), 0);
}

KTEST(fsutil, copy_and_delete_take_a_whole_tree){
    if (!disk_ready()) return;
    fat32_dirent_t e, sub, dst;
    char buf[64];

    KT_EQ(fat32_mkdir(ROOT, "KTTREE"), 0);
    KT_EQ(fat32_find(ROOT, "KTTREE", &e), 0);
    KT_EQ(fat32_write_file(e.first_cluster, "A.TXT", (const uint8_t*)"alpha", 5), 0);
    KT_EQ(fat32_mkdir(e.first_cluster, "SUB"), 0);
    KT_EQ(fat32_find(e.first_cluster, "SUB", &sub), 0);
    KT_EQ(fat32_write_file(sub.first_cluster, "B.TXT", (const uint8_t*)"beta", 4), 0);

    KT_EQ(fs_copy(ROOT, &e, ROOT, "KTTREE2"), 0);
    KT_EQ(fat32_find(ROOT, "KTTREE2", &dst), 0);
    KT_EQ(read_back(dst.first_cluster, "A.TXT", buf, sizeof(buf)), 5);
    KT_STREQ(buf, "alpha");
    fat32_dirent_t dsub;
    KT_EQ(fat32_find(dst.first_cluster, "SUB", &dsub), 0);
    KT_EQ(read_back(dsub.first_cluster, "B.TXT", buf, sizeof(buf)), 4);
    KT_STREQ(buf, "beta");

    // The original is untouched by the copy.
    KT_EQ(read_back(e.first_cluster, "A.TXT", buf, sizeof(buf)), 5);

    KT_EQ(fs_delete(ROOT, &dst), 0);
    KT_EQ(fs_delete(ROOT, &e), 0);
    KT_TRUE(fat32_find(ROOT, "KTTREE", &e) != 0);
    KT_TRUE(fat32_find(ROOT, "KTTREE2", &e) != 0);
}

KTEST(fsutil, pasting_a_folder_into_itself_terminates){
    if (!disk_ready()) return;
    fat32_dirent_t e, copy;
    char buf[32];

    KT_EQ(fat32_mkdir(ROOT, "KTSELF"), 0);
    KT_EQ(fat32_find(ROOT, "KTSELF", &e), 0);
    KT_EQ(fat32_write_file(e.first_cluster, "F.TXT", (const uint8_t*)"data", 4), 0);

    // Copy the folder into itself. Walking the source while writing into it
    // would follow its own output forever if the copy did not skip it.
    KT_EQ(fs_copy(ROOT, &e, e.first_cluster, "INNER"), 0);
    KT_EQ(fat32_find(e.first_cluster, "INNER", &copy), 0);
    KT_EQ(read_back(copy.first_cluster, "F.TXT", buf, sizeof(buf)), 4);
    KT_TRUE(fat32_find(copy.first_cluster, "INNER", &copy) != 0);   // no copy of the copy

    KT_EQ(fs_delete(ROOT, &e), 0);
    KT_TRUE(fat32_find(ROOT, "KTSELF", &e) != 0);
}

KTEST(fsutil, containment_follows_parent_links){
    if (!disk_ready()) return;
    fat32_dirent_t a, b, other;

    KT_EQ(fat32_mkdir(ROOT, "KTOUT"), 0);
    KT_EQ(fat32_find(ROOT, "KTOUT", &a), 0);
    KT_EQ(fat32_mkdir(a.first_cluster, "KTIN"), 0);
    KT_EQ(fat32_find(a.first_cluster, "KTIN", &b), 0);
    KT_EQ(fat32_mkdir(ROOT, "KTAWAY"), 0);
    KT_EQ(fat32_find(ROOT, "KTAWAY", &other), 0);

    KT_TRUE(fs_dir_inside(a.first_cluster, a.first_cluster));   // a folder is inside itself
    KT_TRUE(fs_dir_inside(a.first_cluster, b.first_cluster));   // child
    KT_FALSE(fs_dir_inside(b.first_cluster, a.first_cluster));  // but not the reverse
    KT_FALSE(fs_dir_inside(a.first_cluster, other.first_cluster));
    KT_TRUE(fs_dir_inside(ROOT, b.first_cluster));              // everything is under the root

    KT_EQ(fs_delete(ROOT, &a), 0);
    KT_EQ(fs_delete(ROOT, &other), 0);
}
