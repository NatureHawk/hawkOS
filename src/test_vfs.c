// src/test_vfs.c — the VFS layer, pipes and signals
//
// Like test_fat32.c these run against the real disk image, and clean up after
// themselves. Everything that touches the disk goes through the VFS, so a pass
// also means the path -> FAT32 mapping, the write-back buffering and the
// long-name handling agree with each other.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/ata.h"
#include "header/fat32.h"
#include "header/vfs.h"
#include "header/pipe.h"
#include "header/signal.h"
#include "header/task.h"

static int disk_ready(void){
    if (!ata_present()){ ktest_skip("no ATA device on the primary bus"); return 0; }
    if (!fat32_writable()){ ktest_skip("volume not mounted or not writable"); return 0; }
    return 1;
}

static int write_str(vfs_fdtable_t* t, int fd, const char* s){
    return vfs_write(t, fd, s, (uint32_t)strlen(s));
}

// ------------------------------------------------------------------ paths

static void norm_is(const char* cwd, const char* in, const char* want){
    char out[VFS_PATH_MAX];
    int r = vfs_normalize(cwd, in, out, sizeof(out));
    KT_EQ(r, 0);
    KT_STREQ(out, want);
}

KTEST(vfs, normalize_resolves_dots_and_slashes){
    norm_is("/", "/", "/");
    norm_is("/", "a/b", "/a/b");
    norm_is("/x/y", "z", "/x/y/z");
    norm_is("/x/y", "../z", "/x/z");
    norm_is("/x/y", "../../..", "/");          // ".." at the root stays at the root
    norm_is("/x/y", "/abs//path/", "/abs/path");
    norm_is("/x", "./a/./b/../c", "/x/a/c");
    norm_is(0, "rel", "/rel");                 // no cwd means "/"
    norm_is("/dev", "../DOCS", "/DOCS");

    char out[VFS_PATH_MAX];
    KT_EQ(vfs_normalize("/", "", out, sizeof(out)), VFS_ENOENT);
    KT_EQ(vfs_normalize("/", "/a/b/c", out, 4), VFS_ENAMETOOLONG);
    static char longcomp[VFS_NAME_MAX + 8];
    memset(longcomp, 'q', sizeof(longcomp) - 1); longcomp[sizeof(longcomp) - 1] = 0;
    KT_EQ(vfs_normalize("/", longcomp, out, sizeof(out)), VFS_ENAMETOOLONG);
}

// -------------------------------------------------------------- FAT files

KTEST(vfs, file_create_write_seek_read_unlink){
    if (!disk_ready()) return;
    vfs_fdtable_t t; vfs_fd_init(&t);
    vfs_unlink("/", "/KTV1.TXT");

    KT_EQ(vfs_open(&t, "/", "/KTV1.TXT", VFS_O_RDWR), VFS_ENOENT);
    int fd = vfs_open(&t, "/", "/KTV1.TXT", VFS_O_RDWR | VFS_O_CREAT | VFS_O_EXCL);
    KT_EQ(fd, 0);
    KT_EQ(write_str(&t, fd, "hello, "), 7);
    KT_EQ(write_str(&t, fd, "world"), 5);
    KT_EQ(vfs_lseek(&t, fd, 0, VFS_SEEK_CUR), 12);
    KT_EQ(vfs_lseek(&t, fd, 0, VFS_SEEK_END), 12);
    KT_EQ(vfs_lseek(&t, fd, 0, VFS_SEEK_SET), 0);

    char buf[32];
    memset(buf, 0, sizeof(buf));
    KT_EQ(vfs_read(&t, fd, buf, 5), 5);
    KT_MEMEQ(buf, "hello", 5);
    KT_EQ(vfs_read(&t, fd, buf, 32), 7);       // the rest, then EOF
    KT_EQ(vfs_read(&t, fd, buf, 32), 0);
    KT_EQ(vfs_lseek(&t, fd, -5, VFS_SEEK_END), 7);
    KT_EQ(vfs_read(&t, fd, buf, 5), 5);
    KT_MEMEQ(buf, "world", 5);
    KT_EQ(vfs_lseek(&t, fd, -1, VFS_SEEK_SET), VFS_EINVAL);

    // Overwrite the start, then extend past the end with a hole.
    KT_EQ(vfs_lseek(&t, fd, 0, VFS_SEEK_SET), 0);
    KT_EQ(write_str(&t, fd, "J"), 1);
    KT_EQ(vfs_lseek(&t, fd, 20, VFS_SEEK_SET), 20);
    KT_EQ(write_str(&t, fd, "!"), 1);

    vfs_stat_t st;
    KT_EQ(vfs_fstat(&t, fd, &st), 0);
    KT_EQ(st.type, VFS_T_FILE);
    KT_EQ(st.size, 21);
    KT_EQ(vfs_close(&t, fd), 0);
    KT_EQ(vfs_close(&t, fd), VFS_EBADF);

    // The close flushed it: a fresh read-only open sees the bytes.
    KT_EQ(vfs_stat("/", "KTV1.TXT", &st), 0);
    KT_EQ(st.size, 21);
    fd = vfs_open(&t, "/", "ktv1.txt", VFS_O_RDONLY);
    KT_TRUE(fd >= 0);
    uint8_t all[32];
    memset(all, 0xEE, sizeof(all));
    KT_EQ(vfs_read(&t, fd, all, sizeof(all)), 21);
    KT_MEMEQ(all, "Jello, world", 12);
    KT_EQ(all[12], 0);                         // the hole reads back as zeros
    KT_EQ(all[19], 0);
    KT_EQ(all[20], '!');
    KT_EQ(write_str(&t, fd, "x"), VFS_EBADF);  // read-only description
    vfs_close(&t, fd);

    KT_EQ(vfs_unlink("/", "/KTV1.TXT"), 0);
    KT_EQ(vfs_stat("/", "/KTV1.TXT", &st), VFS_ENOENT);
    KT_EQ(vfs_unlink("/", "/KTV1.TXT"), VFS_ENOENT);
}

KTEST(vfs, trunc_and_append){
    if (!disk_ready()) return;
    vfs_fdtable_t t; vfs_fd_init(&t);
    vfs_unlink("/", "/KTV2.TXT");

    int fd = vfs_open(&t, "/", "/KTV2.TXT", VFS_O_WRONLY | VFS_O_CREAT);
    KT_EQ(write_str(&t, fd, "0123456789"), 10);
    vfs_close(&t, fd);

    fd = vfs_open(&t, "/", "/KTV2.TXT", VFS_O_WRONLY | VFS_O_APPEND);
    KT_EQ(write_str(&t, fd, "AB"), 2);
    KT_EQ(vfs_lseek(&t, fd, 0, VFS_SEEK_SET), 0);
    KT_EQ(write_str(&t, fd, "CD"), 2);         // O_APPEND ignores the seek
    char scratch[4];
    KT_EQ(vfs_read(&t, fd, scratch, 4), VFS_EBADF);   // write-only
    vfs_close(&t, fd);

    vfs_stat_t st;
    KT_EQ(vfs_stat("/", "/KTV2.TXT", &st), 0);
    KT_EQ(st.size, 14);

    fd = vfs_open(&t, "/", "/KTV2.TXT", VFS_O_RDONLY);
    char buf[16]; memset(buf, 0, sizeof(buf));
    KT_EQ(vfs_read(&t, fd, buf, 16), 14);
    KT_MEMEQ(buf, "0123456789ABCD", 14);
    vfs_close(&t, fd);

    // O_TRUNC empties it even if nothing is written afterwards.
    fd = vfs_open(&t, "/", "/KTV2.TXT", VFS_O_WRONLY | VFS_O_TRUNC);
    vfs_close(&t, fd);
    KT_EQ(vfs_stat("/", "/KTV2.TXT", &st), 0);
    KT_EQ(st.size, 0);

    KT_EQ(vfs_unlink("/", "/KTV2.TXT"), 0);
}

KTEST(vfs, long_names_through_the_vfs){
    if (!disk_ready()) return;
    vfs_fdtable_t t; vfs_fd_init(&t);
    vfs_unlink("/", "/A Long File Name.txt");

    int fd = vfs_open(&t, "/", "/A Long File Name.txt", VFS_O_RDWR | VFS_O_CREAT);
    KT_TRUE(fd >= 0);
    write_str(&t, fd, "vfs long");
    vfs_close(&t, fd);

    vfs_stat_t st;
    KT_EQ(vfs_stat("/", "a long FILE name.TXT", &st), 0);
    KT_EQ(st.size, 8);

    // Directory listing shows the long name, not ALONGF~1.TXT.
    fd = vfs_open(&t, "/", "/", VFS_O_RDONLY);
    KT_TRUE(fd >= 0);
    vfs_dirent_t e;
    int seen_long = 0, seen_alias = 0;
    while (vfs_readdir(&t, fd, &e) == 1){
        if (strcmp(e.name, "A Long File Name.txt") == 0) seen_long++;
        if (strcmp(e.name, "ALONGF~1.TXT") == 0) seen_alias++;
    }
    KT_EQ(seen_long, 1);
    KT_EQ(seen_alias, 0);
    vfs_close(&t, fd);

    KT_EQ(vfs_rename("/", "/A Long File Name.txt", "/Another Long Name.md"), 0);
    KT_EQ(vfs_stat("/", "/A Long File Name.txt", &st), VFS_ENOENT);
    KT_EQ(vfs_stat("/", "/Another Long Name.md", &st), 0);
    KT_EQ(vfs_unlink("/", "/Another Long Name.md"), 0);
}

KTEST(vfs, directories_cwd_and_relative_paths){
    if (!disk_ready()) return;
    vfs_fdtable_t t; vfs_fd_init(&t);
    vfs_unlink("/", "/KTVD/inner file.txt");
    vfs_rmdir("/", "/KTVD");

    KT_EQ(vfs_mkdir("/", "/KTVD"), 0);
    KT_EQ(vfs_mkdir("/", "/KTVD"), VFS_EEXIST);
    KT_EQ(vfs_mkdir("/", "/NO_SUCH_PARENT/x"), VFS_ENOENT);

    char cwd[VFS_PATH_MAX] = "/";
    KT_EQ(vfs_chdir(cwd, "KTVD"), 0);
    KT_STREQ(cwd, "/KTVD");
    KT_EQ(vfs_chdir(cwd, "/KTVD/inner"), VFS_ENOENT);
    KT_STREQ(cwd, "/KTVD");                    // a failed chdir leaves cwd alone

    int fd = vfs_open(&t, cwd, "inner file.txt", VFS_O_RDWR | VFS_O_CREAT);
    KT_TRUE(fd >= 0);
    write_str(&t, fd, "in a subdirectory");
    vfs_close(&t, fd);

    vfs_stat_t st;
    KT_EQ(vfs_stat(cwd, "inner file.txt", &st), 0);
    KT_EQ(vfs_stat(cwd, "./inner file.txt", &st), 0);
    KT_EQ(vfs_stat(cwd, "../KTVD/inner file.txt", &st), 0);
    KT_EQ(vfs_stat(cwd, "/KTVD/inner file.txt", &st), 0);
    KT_EQ(vfs_stat(cwd, ".", &st), 0);
    KT_EQ(st.type, VFS_T_DIR);
    KT_EQ(vfs_stat(cwd, "..", &st), 0);
    KT_EQ(st.type, VFS_T_DIR);
    KT_EQ(vfs_chdir(cwd, "inner file.txt"), VFS_ENOTDIR);
    KT_EQ(vfs_open(&t, cwd, "inner file.txt/x", VFS_O_RDONLY), VFS_ENOTDIR);

    // Listing the directory: ".", ".." and the file.
    fd = vfs_open(&t, cwd, ".", VFS_O_RDONLY);
    KT_TRUE(fd >= 0);
    vfs_dirent_t e; int n = 0, found = 0;
    while (vfs_readdir(&t, fd, &e) == 1){ n++; if (strcmp(e.name, "inner file.txt") == 0) found = 1; }
    KT_EQ(n, 3);
    KT_EQ(found, 1);
    char one;
    KT_EQ(vfs_read(&t, fd, &one, 1), VFS_EISDIR);
    KT_EQ(vfs_lseek(&t, fd, 0, VFS_SEEK_SET), 0);        // rewinddir
    KT_EQ(vfs_readdir(&t, fd, &e), 1);
    vfs_close(&t, fd);

    KT_EQ(vfs_open(&t, cwd, "/KTVD", VFS_O_WRONLY), VFS_EISDIR);
    KT_EQ(vfs_rmdir("/", "/KTVD"), VFS_ENOTEMPTY);
    KT_EQ(vfs_unlink("/", "/KTVD"), VFS_EISDIR);

    // Moves: file out to the root, directory rename, and the loop guard.
    KT_EQ(vfs_rename(cwd, "inner file.txt", "/KTVMOVED.TXT"), 0);
    KT_EQ(vfs_stat("/", "/KTVMOVED.TXT", &st), 0);
    KT_EQ(vfs_rename("/", "/KTVD", "/KTVD/sub"), VFS_EINVAL);
    KT_EQ(vfs_rename("/", "/KTVD", "/KTVD2"), 0);
    KT_EQ(vfs_stat("/", "/KTVD2", &st), 0);
    KT_EQ(vfs_rename("/", "/KTVMOVED.TXT", "/KTVD2/back in.txt"), 0);
    KT_EQ(vfs_stat("/", "/KTVD2/back in.txt", &st), 0);
    KT_EQ(vfs_rename("/", "/KTVD2/back in.txt", "/dev/x"), VFS_EXDEV);

    KT_EQ(vfs_unlink("/", "/KTVD2/back in.txt"), 0);
    KT_EQ(vfs_rmdir("/", "/KTVD2"), 0);
    KT_EQ(vfs_stat("/", "/KTVD2", &st), VFS_ENOENT);
}

// ------------------------------------------------------------------ devfs

KTEST(vfs, devfs_null_zero_console){
    vfs_fdtable_t t; vfs_fd_init(&t);
    vfs_stat_t st;

    KT_EQ(vfs_stat("/", "/dev", &st), 0);
    KT_EQ(st.type, VFS_T_DIR);
    KT_EQ(vfs_stat("/", "/dev/null", &st), 0);
    KT_EQ(st.type, VFS_T_CHR);
    KT_EQ(vfs_stat("/", "/dev/nothing", &st), VFS_ENOENT);
    KT_EQ(vfs_stat("/dev", "../dev/./zero", &st), 0);

    int nul = vfs_open(&t, "/", "/dev/null", VFS_O_RDWR);
    KT_TRUE(nul >= 0);
    char buf[16];
    memset(buf, 0x55, sizeof(buf));
    KT_EQ(vfs_write(&t, nul, "discarded", 9), 9);
    KT_EQ(vfs_read(&t, nul, buf, sizeof(buf)), 0);
    KT_EQ(buf[0], 0x55);

    int zero = vfs_open(&t, "/dev", "zero", VFS_O_RDONLY);
    KT_TRUE(zero >= 0);
    KT_EQ(vfs_read(&t, zero, buf, sizeof(buf)), 16);
    for (int i = 0; i < 16; i++) KT_EQ(buf[i], 0);
    KT_EQ(vfs_write(&t, zero, "x", 1), VFS_EBADF);

    int con = vfs_open(&t, "/", "/dev/console", VFS_O_WRONLY);
    KT_TRUE(con >= 0);
    KT_EQ(vfs_write(&t, con, "[vfs test] console write ok\n", 28), 28);

    // A device cannot be created or removed, and the directory lists all three.
    KT_EQ(vfs_open(&t, "/", "/dev/newnode", VFS_O_RDWR | VFS_O_CREAT), VFS_ENOENT);
    KT_EQ(vfs_unlink("/", "/dev/null"), VFS_EACCES);
    vfs_close(&t, nul); vfs_close(&t, zero); vfs_close(&t, con);

    int d = vfs_open(&t, "/", "/dev", VFS_O_RDONLY);
    KT_TRUE(d >= 0);
    vfs_dirent_t e; int n = 0, has_null = 0, has_zero = 0, has_console = 0;
    while (vfs_readdir(&t, d, &e) == 1){
        n++;
        if (!strcmp(e.name, "null")) has_null = 1;
        if (!strcmp(e.name, "zero")) has_zero = 1;
        if (!strcmp(e.name, "console")) has_console = 1;
    }
    KT_EQ(n, 3);
    KT_TRUE(has_null && has_zero && has_console);
    vfs_close(&t, d);

    char cwd[VFS_PATH_MAX] = "/";
    KT_EQ(vfs_chdir(cwd, "/dev"), 0);
    KT_STREQ(cwd, "/dev");
    KT_EQ(vfs_chdir(cwd, ".."), 0);
    KT_STREQ(cwd, "/");
}

// ----------------------------------------------------------- descriptors

KTEST(vfs, fd_table_dup_close_and_limits){
    vfs_fdtable_t t; vfs_fd_init(&t);
    int a = vfs_open(&t, "/", "/dev/zero", VFS_O_RDONLY);
    KT_EQ(a, 0);

    // dup shares the description, so the position is shared too.
    int b = vfs_dup(&t, a);
    KT_EQ(b, 1);
    KT_EQ(vfs_lseek(&t, a, 100, VFS_SEEK_SET), 100);
    KT_EQ(vfs_lseek(&t, b, 0, VFS_SEEK_CUR), 100);
    KT_TRUE(vfs_fd_get(&t, a) == vfs_fd_get(&t, b));

    // Closing one fd leaves the description alive for the other.
    KT_EQ(vfs_close(&t, a), 0);
    char c;
    KT_EQ(vfs_read(&t, b, &c, 1), 1);
    KT_EQ(vfs_read(&t, a, &c, 1), VFS_EBADF);
    KT_EQ(vfs_fd_get(&t, b)->refs, 1);

    // dup2 replaces an occupied slot, and onto itself is a no-op.
    int n = vfs_open(&t, "/", "/dev/null", VFS_O_RDWR);
    KT_EQ(n, 0);
    KT_EQ(vfs_dup2(&t, b, 5), 5);
    KT_EQ(vfs_dup2(&t, n, 5), 5);
    KT_TRUE(vfs_fd_get(&t, 5) == vfs_fd_get(&t, n));
    KT_EQ(vfs_dup2(&t, 5, 5), 5);
    KT_EQ(vfs_dup2(&t, 9, 3), VFS_EBADF);
    KT_EQ(vfs_dup2(&t, 5, VFS_FD_MAX), VFS_EBADF);
    KT_EQ(vfs_fd_get(&t, b)->refs, 1);         // the zero device lost its dup on fd 5

    // clone (fork): both tables share, each holding its own reference.
    vfs_fdtable_t child; vfs_fd_init(&child);
    vfs_fd_clone(&child, &t);
    KT_TRUE(vfs_fd_get(&child, 5) == vfs_fd_get(&t, 5));
    KT_EQ(vfs_fd_get(&t, 5)->refs, 4);         // fds 0 and 5, in parent and child
    vfs_fd_closeall(&child);
    KT_EQ(vfs_fd_get(&t, 5)->refs, 2);

    // The table fills at VFS_FD_MAX; the failed open leaks nothing.
    int opened = 0;
    for (int i = 0; i < VFS_FD_MAX + 2; i++)
        if (vfs_open(&t, "/", "/dev/null", VFS_O_RDONLY) >= 0) opened++;
    KT_EQ(vfs_open(&t, "/", "/dev/null", VFS_O_RDONLY), VFS_EMFILE);
    KT_EQ(vfs_dup(&t, 5), VFS_EMFILE);
    KT_EQ(opened + 3, VFS_FD_MAX);             // fds 0, 1 and 5 were already taken

    vfs_fd_closeall(&t);
    KT_TRUE(vfs_fd_get(&t, 0) == 0);
}

// ------------------------------------------------------------------ pipes

KTEST(pipe, read_write_and_eof){
    vfs_fdtable_t t; vfs_fd_init(&t);
    int fds[2];
    KT_EQ(pipe_open_fds(&t, fds), 0);
    KT_EQ(fds[0], 0);
    KT_EQ(fds[1], 1);

    vfs_stat_t st;
    KT_EQ(vfs_fstat(&t, fds[0], &st), 0);
    KT_EQ(st.type, VFS_T_PIPE);

    KT_EQ(write_str(&t, fds[1], "ping"), 4);
    KT_EQ(write_str(&t, fds[1], "pong"), 4);
    char buf[16]; memset(buf, 0, sizeof(buf));
    KT_EQ(vfs_read(&t, fds[0], buf, 6), 6);            // short read: bytes in order
    KT_MEMEQ(buf, "pingpo", 6);
    KT_EQ(vfs_read(&t, fds[0], buf, 16), 2);
    KT_MEMEQ(buf, "ng", 2);

    // Wrong-direction use and seeking are refused.
    KT_EQ(vfs_write(&t, fds[0], "x", 1), VFS_EBADF);
    KT_EQ(vfs_read(&t, fds[1], buf, 1), VFS_EBADF);
    KT_EQ(vfs_lseek(&t, fds[0], 0, VFS_SEEK_SET), VFS_ESPIPE);

    // Leftover data is still delivered after the writer closes, then EOF.
    KT_EQ(write_str(&t, fds[1], "tail"), 4);
    KT_EQ(vfs_close(&t, fds[1]), 0);
    KT_EQ(vfs_read(&t, fds[0], buf, 16), 4);
    KT_EQ(vfs_read(&t, fds[0], buf, 16), 0);
    KT_EQ(vfs_read(&t, fds[0], buf, 16), 0);
    vfs_close(&t, fds[0]);
}

KTEST(pipe, write_to_a_closed_reader_fails){
    vfs_fdtable_t t; vfs_fd_init(&t);
    int fds[2];
    KT_EQ(pipe_open_fds(&t, fds), 0);
    KT_EQ(vfs_close(&t, fds[0]), 0);
    KT_EQ(write_str(&t, fds[1], "nobody home"), VFS_EPIPE);
    vfs_close(&t, fds[1]);
}

KTEST(pipe, dup_keeps_an_end_open_until_the_last_close){
    vfs_fdtable_t t; vfs_fd_init(&t);
    int fds[2];
    KT_EQ(pipe_open_fds(&t, fds), 0);
    pipe_set_nonblock(vfs_fd_get(&t, fds[0]), 1);

    int w2 = vfs_dup(&t, fds[1]);
    KT_TRUE(w2 >= 0);
    vfs_close(&t, fds[1]);
    char c;
    KT_EQ(vfs_read(&t, fds[0], &c, 1), VFS_EAGAIN);    // a writer still exists: no EOF yet
    KT_EQ(write_str(&t, w2, "z"), 1);
    KT_EQ(vfs_read(&t, fds[0], &c, 1), 1);
    KT_EQ(c, 'z');
    vfs_close(&t, w2);
    KT_EQ(vfs_read(&t, fds[0], &c, 1), 0);             // now it is EOF
    vfs_close(&t, fds[0]);
}

KTEST(pipe, capacity_is_bounded_and_wraps){
    vfs_fdtable_t t; vfs_fd_init(&t);
    int fds[2];
    KT_EQ(pipe_open_fds(&t, fds), 0);
    pipe_set_nonblock(vfs_fd_get(&t, fds[0]), 1);
    pipe_set_nonblock(vfs_fd_get(&t, fds[1]), 1);

    static uint8_t out[PIPE_CAPACITY + 100], in[PIPE_CAPACITY + 100];
    for (uint32_t i = 0; i < sizeof(out); i++) out[i] = (uint8_t)(i * 7u + 1);

    KT_EQ(vfs_read(&t, fds[0], in, 1), VFS_EAGAIN);
    KT_EQ(vfs_write(&t, fds[1], out, sizeof(out)), PIPE_CAPACITY);   // partial write fills it
    KT_EQ(pipe_buffered(vfs_fd_get(&t, fds[0])), PIPE_CAPACITY);
    KT_EQ(vfs_write(&t, fds[1], out, 1), VFS_EAGAIN);                // full
    KT_EQ(vfs_read(&t, fds[0], in, 1000), 1000);
    KT_MEMEQ(in, out, 1000);

    // Room again; the write wraps around the end of the ring.
    KT_EQ(vfs_write(&t, fds[1], out + PIPE_CAPACITY, 100), 100);
    KT_EQ(pipe_buffered(vfs_fd_get(&t, fds[0])), PIPE_CAPACITY - 1000 + 100);
    KT_EQ(vfs_read(&t, fds[0], in, PIPE_CAPACITY), PIPE_CAPACITY - 1000 + 100);
    KT_MEMEQ(in, out + 1000, PIPE_CAPACITY - 1000);
    KT_MEMEQ(in + PIPE_CAPACITY - 1000, out + PIPE_CAPACITY, 100);

    vfs_fd_closeall(&t);
}

static vfs_file_t* pw_end;
static volatile int pw_done;

static void pipe_writer_task(void* arg){
    (void)arg;
    task_sleep(40);                                    // let the reader block first
    vfs_file_write(pw_end, "late data", 9);
    pw_done = 1;
}

KTEST(pipe, blocking_read_waits_for_a_writer){
    vfs_file_t *r, *w;
    KT_EQ(pipe_create(&r, &w), 0);
    pw_end = w; pw_done = 0;
    KT_TRUE(task_create("ktpipe", pipe_writer_task, 0) >= 0);

    char buf[16]; memset(buf, 0, sizeof(buf));
    int n = vfs_file_read(r, buf, sizeof(buf));        // blocks until the task writes
    KT_EQ(n, 9);
    KT_MEMEQ(buf, "late data", 9);

    for (int i = 0; i < 500 && !pw_done; i++) task_sleep(10);
    KT_EQ(pw_done, 1);
    vfs_file_put(w);
    KT_EQ(vfs_file_read(r, buf, sizeof(buf)), 0);      // writer gone: EOF, does not block
    vfs_file_put(r);
}

// ---------------------------------------------------------------- signals

static void sig_cleanup(void){
    signal_sigmask(SIG_SETMASK, 0);
    signal_task_exit(task_current_id());
}

KTEST(signal, defaults_are_fatal_and_dequeue_once){
    sig_cleanup();
    uint32_t me = task_current_id();

    KT_EQ(signal_pending_current(), 0);
    KT_EQ(signal_dequeue_current(), 0);

    KT_EQ(signal_send(me, SIGTERM), 0);
    KT_EQ(signal_pending(me), SIGMASK(SIGTERM));
    KT_EQ(signal_pending_current(), 1);
    KT_EQ(signal_dequeue_current(), SIGTERM);          // default action would terminate
    KT_EQ(signal_dequeue_current(), 0);                // consumed
    KT_EQ(signal_pending(me), 0);

    // SIGKILL outranks whatever else is pending; each one is delivered once.
    KT_EQ(signal_send(me, SIGINT), 0);
    KT_EQ(signal_send(me, SIGKILL), 0);
    KT_EQ(signal_send(me, SIGUSR1), 0);
    KT_EQ(signal_dequeue_current(), SIGKILL);
    KT_EQ(signal_dequeue_current(), SIGINT);
    KT_EQ(signal_dequeue_current(), SIGUSR1);
    KT_EQ(signal_dequeue_current(), 0);

    // signal_check_current() with nothing pending simply returns.
    signal_check_current();
    sig_cleanup();
}

KTEST(signal, send_validates_its_arguments){
    uint32_t me = task_current_id();
    KT_EQ(signal_send(me, 0), 0);                      // existence probe
    KT_EQ(signal_send(me, 3), SIGNAL_EINVAL);          // not a supported signal
    KT_EQ(signal_send(me, -1), SIGNAL_EINVAL);
    KT_EQ(signal_send(me, 99), SIGNAL_EINVAL);
    KT_EQ(signal_send(0x7FFFFFF0u, SIGTERM), SIGNAL_ESRCH);
    KT_EQ(signal_send(0x7FFFFFF0u, 0), SIGNAL_ESRCH);
    KT_EQ(signal_pending(me), 0);
    sig_cleanup();
}

KTEST(signal, ignore_and_block){
    sig_cleanup();
    uint32_t me = task_current_id();

    // Ignored signals are discarded at send time; SIGKILL cannot be ignored.
    KT_EQ(signal_ignore(SIGUSR1, 1), 0);
    KT_EQ(signal_send(me, SIGUSR1), 0);
    KT_EQ(signal_pending(me), 0);
    KT_EQ(signal_ignore(SIGKILL, 1), SIGNAL_EINVAL);
    KT_EQ(signal_ignore(SIGUSR1, 0), 0);
    KT_EQ(signal_send(me, SIGUSR1), 0);
    KT_EQ(signal_dequeue_current(), SIGUSR1);

    // Blocked signals stay pending until unblocked; SIGKILL cannot be blocked.
    signal_sigmask(SIG_BLOCK, SIGMASK(SIGTERM) | SIGMASK(SIGKILL));
    KT_EQ(signal_send(me, SIGTERM), 0);
    KT_EQ(signal_pending(me), SIGMASK(SIGTERM));
    KT_EQ(signal_pending_current(), 0);
    KT_EQ(signal_dequeue_current(), 0);
    KT_EQ(signal_send(me, SIGKILL), 0);
    KT_EQ(signal_dequeue_current(), SIGKILL);
    KT_EQ(signal_sigmask(SIG_UNBLOCK, SIGMASK(SIGTERM)), SIGMASK(SIGTERM));
    KT_EQ(signal_dequeue_current(), SIGTERM);
    sig_cleanup();
}

KTEST(signal, a_pending_signal_interrupts_a_blocking_pipe_read){
    sig_cleanup();
    vfs_file_t *r, *w;
    KT_EQ(pipe_create(&r, &w), 0);
    KT_EQ(signal_send(task_current_id(), SIGINT), 0);

    char c;
    KT_EQ(vfs_file_read(r, &c, 1), VFS_EINTR);         // would block forever otherwise
    KT_EQ(signal_dequeue_current(), SIGINT);
    vfs_file_put(r); vfs_file_put(w);
    sig_cleanup();
}
