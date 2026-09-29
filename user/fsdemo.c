/* user/fsdemo.c — a tour of the file and memory calls, for trying by hand
 *
 * Creates 'A Long File Name.txt', writes to it, renames it, lists the
 * directory, reads it back, and removes it; then grows the heap with sbrk and
 * takes an anonymous mapping. Prints what it does and exits with the number
 * of steps that went wrong. Try it with:  run fsdemo.elf */
#include "hawk.h"

static int failures;

static void step(int ok, const char* what){
    hawk_puts(ok ? "  ok    " : "  FAIL  ");
    hawk_puts(what);
    hawk_puts("\n");
    if (!ok) failures++;
}

static void list(const char* dir){
    hawk_dirent_t d;
    for (unsigned i = 0; sys_readdir(dir, i, &d) == 1; i++){
        hawk_puts("        ");
        hawk_puts(d.is_dir ? "[dir] " : "      ");
        hawk_puts(d.name);
        hawk_puts("\n");
    }
}

void _start(void){
    static const char first[]  = "A Long File Name.txt";
    static const char second[] = "Renamed Long File.txt";
    static const char text[]   = "hawkOS keeps long names.\n";
    char back[64];

    hawk_puts("fsdemo: files\n");
    int fd = sys_open(first, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0){
        hawk_puts("fsdemo: cannot create files here (read-only disk?)\n");
        sys_exit(1);
    }
    step(sys_write(fd, text, sizeof(text) - 1) == (int)sizeof(text) - 1, "write");
    step(sys_lseek(fd, 0, SEEK_SET) == 0, "seek to start");
    int n = sys_read(fd, back, sizeof(back));
    step(n == (int)sizeof(text) - 1 && hawk_memeq(back, text, (unsigned)n), "read back");
    sys_close(fd);

    hawk_stat_t st;
    step(sys_stat(first, &st) == 0 && st.type == STAT_FILE && st.size == sizeof(text) - 1, "stat");
    step(sys_rename(first, second) == 0, "rename");
    step(sys_stat(first, &st) < 0, "old name is gone");
    hawk_puts("  directory now holds:\n");
    list(".");
    step(sys_unlink(second) == 0, "unlink");

    hawk_puts("fsdemo: memory\n");
    char* h = (char*)sys_sbrk(64 * 1024);
    step((int)h > 0, "sbrk 64 KB");
    if ((int)h > 0){
        int zero = 1;
        for (int i = 0; i < 64 * 1024; i += 512) if (h[i]) zero = 0;
        step(zero, "heap starts zeroed");
        h[0] = 42; h[64 * 1024 - 1] = 43;
        step(h[0] == 42 && h[64 * 1024 - 1] == 43, "heap is writable end to end");
        sys_sbrk(-64 * 1024);
    }
    char* m = (char*)sys_mmap(1 << 20, PROT_READ | PROT_WRITE);
    step((int)m > 0, "mmap 1 MB");
    if ((int)m > 0){
        m[0] = 1; m[(1 << 20) - 1] = 2;
        step(m[0] == 1 && m[(1 << 20) - 1] == 2 && m[4096] == 0, "mapping is writable and zero-filled");
        step(sys_munmap(m, 1 << 20) == 0, "munmap");
    }

    hawk_puts("fsdemo: ");
    hawk_putnum((unsigned)failures);
    hawk_puts(" failure(s)\n");
    sys_exit(failures);
}
