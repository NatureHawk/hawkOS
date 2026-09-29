/* user/utest.c — the system call interface, exercised from ring 3
 *
 * The kernel tests drive the handlers with a fake process; this runs the same
 * calls the way a real program would, through int 0x80, with hostile
 * arguments. It exits with the number of checks that failed, and the kernel
 * test that spawns it asserts that number is zero. */
#include "hawk.h"

static int failures = 0;

static void check(int ok, const char* what){
    if (ok) return;
    failures++;
    hawk_puts("utest FAIL: ");
    hawk_puts(what);
    hawk_puts("\n");
}

/* Recursion with a kilobyte of frame per level: 300 levels is far past the
   16 KB the stack starts with, so this only works if it grows on demand. */
static int deep(int n){
    volatile char pad[1024];
    pad[0] = (char)n;
    pad[1023] = (char)n;
    if (n == 0) return 0;
    int r = deep(n - 1);
    return r + (pad[0] == (char)n && pad[1023] == (char)n);
}

static void memory_checks(void){
    /* The heap: grows on demand, is zeroed, and is reachable only up to the
       break (rounded to a page). */
    char* h = (char*)sys_sbrk(8192);
    check((int)h > 0, "sbrk grows");
    if ((int)h > 0){
        int zero = 1;
        for (int i = 0; i < 8192; i++) if (h[i]) zero = 0;
        check(zero, "fresh heap is zeroed");
        h[0] = 1; h[8191] = 2;
        check(sys_sbrk(0) == (void*)(h + 8192), "break moved by the increment");
        check(sys_write(1, "", 0) == 0, "empty write");
        check(sys_write(1, h + 8190, 2) == 2, "write from heap tail");
        check(sys_write(1, h + 8190, 3) < 0, "write past the break");
        check(sys_sbrk(-8192) == (void*)(h + 8192), "sbrk shrinks");
        check(sys_write(1, h, 1) < 0, "write from released heap");
        char* h2 = (char*)sys_sbrk(4096);
        int zero2 = 1;
        for (int i = 0; i < 4096; i++) if (h2[i]) zero2 = 0;
        check(zero2, "regrown heap is zeroed again");
        sys_sbrk(-4096);
    }
    check(sys_brk((void*)0x100000) < 0, "brk into kernel space");
    check((int)sys_sbrk(0x7FFFFFFF) < 0, "sbrk beyond the heap limit");

    /* Demand-zero: a megabyte of heap costs nothing until it is touched, and
       every page comes up zero. */
    {
        unsigned big = 1u << 20;
        char* p = (char*)sys_sbrk((int)big);
        check((int)p > 0, "sbrk 1 MB");
        if ((int)p > 0){
            int zero = 1;
            for (unsigned i = 0; i < big; i += 4096) if (p[i]) zero = 0;
            check(zero, "1 MB of heap reads zero");
            for (unsigned i = 0; i < big; i += 4096) p[i] = (char)(i >> 12);
            int keep = 1;
            for (unsigned i = 0; i < big; i += 4096) if (p[i] != (char)(i >> 12)) keep = 0;
            check(keep, "1 MB of heap holds what was written");
            sys_sbrk(-(int)big);
        }
    }

    /* mmap: page aligned, zeroed, writable, and gone after munmap. */
    {
        char* m = (char*)sys_mmap(20000, PROT_READ | PROT_WRITE);
        check((int)m > 0 && ((unsigned)m & 4095) == 0, "mmap returns an aligned address");
        if ((int)m > 0){
            check(m[0] == 0 && m[19999] == 0, "mapping is zero-filled");
            m[0] = 7; m[19999] = 8;
            check(sys_write(1, m + 19998, 2) == 2, "write from a mapping");
            check(sys_munmap(m, 20000) == 0, "munmap");
            check(sys_write(1, m, 1) < 0, "write from an unmapped mapping");
        }
        check((int)sys_mmap(0, PROT_READ) < 0, "mmap of length 0");
        check((int)sys_mmap(4096, 0) < 0, "mmap without protection");
        check(sys_munmap((void*)0x100000, 4096) < 0, "munmap of kernel memory");
    }

    check(deep(300) == 300, "recursion far past the initial stack");
}

static void pointer_checks(void){
    char msg[] = "utest: ";
    /* Pointers that must be refused: kernel memory, a wrapping range, the
       unmapped gap, and a descriptor that was never opened. */
    check(sys_write(1, (const void*)0x100000, 16) == -E_FAULT, "write from kernel address");
    check(sys_write(1, (const void*)0xFFFFFFF0u, 0x40) < 0, "write with wrapping range");
    check(sys_write(1, (const void*)0x20000000u, 16) < 0, "write from unmapped memory");
    check(sys_write(99, msg, 3) < 0, "write to bad fd");
    check(sys_write(-1, msg, 3) < 0, "write to negative fd");
    check(sys_read(3, msg, 3) < 0, "read from closed fd");
    check(sys_read(0, (void*)0x100000, 16) < 0, "read into kernel address");
    check(sys_open((const char*)0x100000, O_RDONLY) < 0, "open with kernel path");
    check(sys_open("../PASSWD", O_RDONLY) < 0, "open with dotdot path");
    check(sys_open("NOSUCH.FIL", O_RDONLY) < 0, "open of missing file");
    check(sys_open("A/B", O_RDONLY) < 0, "open through a missing directory");
    check(sys_close(7) < 0, "close of unopened fd");
    check(sys_lseek(7, 0, SEEK_SET) < 0, "lseek on unopened fd");
    check(sys_spawn((const char*)0x100000, 0) < 0, "spawn with kernel path");
    check(sys_spawn("NOSUCH.ELF", 0) < 0, "spawn of missing program");
    check(sys_waitpid(4242, 0) < 0, "waitpid on a stranger");
    check(sys_waitpid(1, (int*)0x100000) < 0, "waitpid with kernel status pointer");
    check(sys_getcwd((char*)0x100000, 8) < 0, "getcwd into kernel address");
    check(sys_pipe((int*)0x100000) < 0, "pipe into kernel address");
    check(sys_stat("/", (hawk_stat_t*)0x100000) < 0, "stat into kernel address");
    check(sys_kill(4242, SIGTERM) < 0, "kill of a stranger");
    check(sys_kill(sys_getpid(), 99) < 0, "kill with a bad signal");
    {
        hawk_dirent_t d;
        check(sys_readdir("/", 0, (hawk_dirent_t*)0x100000) < 0, "readdir into kernel address");
        check(sys_readdir("/NOPE", 0, &d) < 0, "readdir of a missing directory");
        check(sys_readdir("/", 0, &d) == 1, "readdir entry 0");
        check(sys_readdir("/", 100000, &d) == 0, "readdir past the end");
    }
}

static void file_checks(void){
    /* A file, end to end: create, write, seek, read back, reopen. */
    static const char lname[] = "A Long File Name.txt";
    char back[16];
    int fd = sys_open("UTEST.TMP", O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0){
        hawk_puts("utest: disk not writable, skipping file checks\n");
        return;
    }
    check(sys_write(fd, "hello, disk", 11) == 11, "file write");
    check(sys_lseek(fd, 0, SEEK_END) == 11, "seek to end");
    check(sys_lseek(fd, -5, SEEK_CUR) == 6, "seek relative");
    check(sys_read(fd, back, 16) == 5 && hawk_memeq(back, " disk", 5), "read after seek");
    check(sys_read(fd, back, 16) == 0, "read at end of file");
    check(sys_lseek(fd, -1, SEEK_SET) < 0, "seek before start");
    check(sys_close(fd) == 0, "close flushes");
    check(sys_close(fd) < 0, "double close");

    fd = sys_open("UTEST.TMP", O_RDONLY);
    check(fd >= 3, "reopen");
    check(sys_write(fd, "x", 1) < 0, "write to a read-only fd");
    check(sys_read(fd, back, 16) == 11 && hawk_memeq(back, "hello, disk", 11),
          "contents persisted");
    sys_close(fd);
    check(sys_unlink("UTEST.TMP") == 0, "unlink");
    check(sys_open("UTEST.TMP", O_RDONLY) < 0, "unlinked file is gone");

    /* A long name with spaces: create, write, seek, read, stat, rename, unlink. */
    fd = sys_open(lname, O_RDWR | O_CREAT | O_TRUNC);
    check(fd >= 3, "create a long file name");
    if (fd >= 3){
        hawk_stat_t st;
        check(sys_write(fd, "0123456789", 10) == 10, "long-name write");
        check(sys_lseek(fd, 3, SEEK_SET) == 3, "long-name seek");
        check(sys_read(fd, back, 4) == 4 && hawk_memeq(back, "3456", 4), "long-name read");
        check(sys_fstat(fd, &st) == 0 && st.type == STAT_FILE && st.size == 10, "fstat");
        sys_close(fd);
        check(sys_stat(lname, &st) == 0 && st.size == 10, "stat by long name");
        check(sys_rename(lname, "Second Long Name.txt") == 0, "rename");
        check(sys_stat(lname, &st) < 0, "old name gone after rename");
        fd = sys_open("second long name.TXT", O_RDONLY);
        check(fd >= 3, "names match without regard to case");
        sys_close(fd);
        check(sys_unlink("Second Long Name.txt") == 0, "unlink a long name");
    }

    /* dup2: two descriptors, one file position. */
    fd = sys_open("UTEST.TMP", O_RDWR | O_CREAT | O_TRUNC);
    if (fd >= 0){
        check(sys_dup2(fd, 10) == 10, "dup2");
        check(sys_write(10, "ab", 2) == 2 && sys_write(fd, "cd", 2) == 2, "writes through both");
        check(sys_lseek(fd, 0, SEEK_CUR) == 4, "descriptors share a position");
        sys_lseek(10, 0, SEEK_SET);
        check(sys_read(fd, back, 8) == 4 && hawk_memeq(back, "abcd", 4), "shared file contents");
        check(sys_close(fd) == 0, "close one of two");
        check(sys_lseek(10, 0, SEEK_END) == 4, "the other stays usable");
        sys_close(10);
        sys_unlink("UTEST.TMP");
    }
    check(sys_dup2(1, 12) == 12, "dup2 of the console");
    check(sys_write(12, "", 0) == 0, "console alias");
    sys_close(12);

    /* Directories: mkdir, chdir, relative create, getcwd, cleanup. */
    if (sys_mkdir("utdir") == 0){
        char cwd[64];
        check(sys_mkdir("utdir") < 0, "mkdir of an existing directory");
        check(sys_rmdir("UTEST.NOPE") < 0, "rmdir of a missing directory");
        check(sys_chdir("utdir") == 0, "chdir into it");
        check(sys_getcwd(cwd, sizeof(cwd)) == 7 && hawk_strcmp(cwd, "/utdir") == 0, "getcwd");
        fd = sys_open("in dir.txt", O_WRONLY | O_CREAT);
        check(fd >= 3, "create relative to the new cwd");
        sys_close(fd);
        check(sys_rmdir("/utdir") < 0, "rmdir of a non-empty directory");
        check(sys_chdir("..") == 0, "chdir ..");
        check(sys_getcwd(cwd, sizeof(cwd)) == 2 && cwd[0] == '/', "back at the root");
        fd = sys_open("/utdir/in dir.txt", O_RDONLY);
        check(fd >= 3, "absolute path to the file");
        sys_close(fd);
        check(sys_unlink("utdir/in dir.txt") == 0, "unlink inside the directory");
        check(sys_chdir("/dev/null") < 0, "chdir to a non-directory");
        check(sys_rmdir("utdir") == 0, "rmdir");
    }
}

static int read_all(int fd, char* out, int cap){
    int n = 0, r;
    while (n < cap && (r = sys_read(fd, out + n, (unsigned)(cap - n))) > 0) n += r;
    return n;
}

static void process_checks(void){
    /* A child: spawn it with arguments, wait for it, learn its status. */
    int pid = sys_spawn("HI.ELF", "one two");
    if (pid < 0){
        hawk_puts("utest: HI.ELF not on the disk, skipping spawn checks\n");
        return;
    }
    int status = -99;
    check(sys_waitpid(pid, &status) == pid, "waitpid returns the child");
    check(status == 0, "child loaded faithfully and exited 0");
    check(sys_waitpid(pid, 0) < 0, "a child can be reaped once");

    /* A pipeline: WC reads a pipe that is still empty when it starts, so it
       blocks; the words are written later and their count comes back on a
       second pipe. The child shares our descriptors (SPAWN_INHERIT), so 0 and
       1 are swapped for the pipe ends around the spawn and put back after. */
    {
        int in[2], out[2];
        check(sys_pipe(in) == 0 && sys_pipe(out) == 0, "two pipes");
        int save0 = sys_dup(0), save1 = sys_dup(1);
        sys_dup2(in[0], 0);
        sys_dup2(out[1], 1);
        int wc = sys_spawn3("WC.ELF", 0, SPAWN_INHERIT);
        sys_dup2(save0, 0);
        sys_dup2(save1, 1);
        sys_close(save0); sys_close(save1);
        sys_close(in[0]); sys_close(out[1]);
        check(wc > 0, "spawn wc with redirected descriptors");
        if (wc > 0){
            char text[] = "one two three\nfour\n";
            sys_sleep(60);                                  /* let it block in read first */
            check(sys_write(in[1], text, sizeof(text) - 1) == (int)sizeof(text) - 1, "write to the pipe");
            sys_close(in[1]);                               /* EOF for the child */
            char res[32];
            int n = read_all(out[0], res, sizeof(res) - 1);
            res[n < 0 ? 0 : n] = 0;
            check(n == 7 && hawk_memeq(res, "2 4 19\n", 7), "wc counted the piped text");
            int st = -99;
            check(sys_waitpid(wc, &st) == wc && st == 0, "wc exited 0");
        }
        sys_close(in[1]); sys_close(out[0]);
    }

    /* Signals: a child blocked reading a pipe that never gets data is
       terminated by SIGTERM, and reports 128 + 15. */
    {
        int in[2], out[2];
        sys_pipe(in); sys_pipe(out);
        int save0 = sys_dup(0), save1 = sys_dup(1);
        sys_dup2(in[0], 0);
        sys_dup2(out[1], 1);
        int victim = sys_spawn3("WC.ELF", 0, SPAWN_INHERIT);
        sys_dup2(save0, 0);
        sys_dup2(save1, 1);
        sys_close(save0); sys_close(save1);
        check(victim > 0, "spawn a child to kill");
        if (victim > 0){
            sys_sleep(60);
            check(sys_kill(victim, SIGTERM) == 0, "kill delivers SIGTERM");
            int st = -99;
            check(sys_waitpid(victim, &st) == victim, "waitpid after kill");
            check(st == 128 + SIGTERM, "killed child reports 128+15");
        }
        sys_close(in[0]); sys_close(in[1]); sys_close(out[0]); sys_close(out[1]);
    }
}

void _start(int argc, char** argv){
    (void)argc; (void)argv;
    hawk_puts("utest: running\n");

    pointer_checks();
    memory_checks();
    file_checks();
    process_checks();

    check(sys_gettime() > 1600000000u, "gettime is after 2020");
    {
        char cwd[8];
        check(sys_getcwd(cwd, sizeof(cwd)) == 2 && cwd[0] == 0x2F && cwd[1] == 0, "getcwd");
        check(sys_getcwd(cwd, 1) < 0, "getcwd into too small a buffer");
    }

    hawk_puts("utest: ");
    hawk_putnum((unsigned)failures);
    hawk_puts(" failure(s)\n");
    sys_exit(failures);
}
