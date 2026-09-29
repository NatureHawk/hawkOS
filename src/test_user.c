// src/test_user.c — the ELF loader, the syscall surface, and real programs
//
// Three layers, cheapest first. The ELF parser is pure: hand-built images,
// good and hostile, no disk and no process. The syscall handlers are driven
// with a fake process (an address space bound to the test's own task) so
// argument validation is checked without needing a ring-3 program to make the
// mistakes. And the programs built from user/ are spawned for real and their
// exit codes read back, which is the only test that the whole path -- loader,
// argv frame, gate, handlers, waitpid -- agrees with itself.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/pmm.h"
#include "header/paging.h"
#include "header/proc.h"
#include "header/syscall.h"
#include "header/vfs.h"
#include "header/vm.h"
#include "header/signal.h"
#include "header/task.h"
#include "header/fat32.h"
#include "header/ata.h"

// ---------------------------------------------------------------- ELF

static void p16(uint8_t* b, uint32_t off, uint16_t v){ memcpy(b + off, &v, 2); }
static void p32(uint8_t* b, uint32_t off, uint32_t v){ memcpy(b + off, &v, 4); }

#define IMG_LEN 512
#define PH0     52          // first program header

// A valid single-segment executable: the whole file is the segment, mapped at
// PROC_BASE with a page of memory (so bss follows the file bytes), entry just
// past the headers.
static void mk_good(uint8_t* b){
    memset(b, 0, IMG_LEN);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 1; b[5] = 1; b[6] = 1;
    p16(b, 16, 2);                      // ET_EXEC
    p16(b, 18, 3);                      // EM_386
    p32(b, 20, 1);
    p32(b, 24, PROC_BASE + 0x100);      // entry
    p32(b, 28, PH0);                    // phoff
    p16(b, 40, 52);
    p16(b, 42, 32);                     // phentsize
    p16(b, 44, 1);                      // phnum
    p32(b, PH0 + 0, 1);                 // PT_LOAD
    p32(b, PH0 + 4, 0);                 // offset
    p32(b, PH0 + 8, PROC_BASE);         // vaddr
    p32(b, PH0 + 12, PROC_BASE);        // paddr
    p32(b, PH0 + 16, IMG_LEN);          // filesz
    p32(b, PH0 + 20, 4096);             // memsz
    p32(b, PH0 + 24, 7);
    p32(b, PH0 + 28, 0x1000);
}

static uint8_t img[IMG_LEN];

KTEST(elf, accepts_a_valid_image){
    mk_good(img);
    proc_elf_t e;
    KT_EQ(proc_elf_parse(img, IMG_LEN, &e), PROC_ELF_OK);
    KT_EQ(e.entry, PROC_BASE + 0x100);
    KT_EQ(e.span, 4096);
    KT_EQ(e.nload, 1);
}

KTEST(elf, non_elf_falls_through_to_flat){
    uint8_t flat[16] = { 0x90, 0x90, 0xEB, 0xFE };
    KT_EQ(proc_elf_parse(flat, sizeof(flat), 0), PROC_ELF_NOTELF);
    KT_EQ(proc_elf_parse(img, 3, 0), PROC_ELF_NOTELF);
    KT_EQ(proc_elf_parse(0, 100, 0), PROC_ELF_NOTELF);
}

KTEST(elf, rejects_bad_headers){
    // Truncated: magic present, header not.
    mk_good(img);
    KT_EQ(proc_elf_parse(img, 40, 0), PROC_ELF_TRUNC);

    mk_good(img); img[4] = 2;                       // ELFCLASS64
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_CLASS);
    mk_good(img); img[5] = 2;                       // big-endian
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_CLASS);
    mk_good(img); p32(img, 20, 2);                  // e_version
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_CLASS);

    mk_good(img); p16(img, 16, 3);                  // ET_DYN
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_TYPE);
    mk_good(img); p16(img, 18, 40);                 // EM_ARM
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_TYPE);
    mk_good(img); p32(img, PH0, 3);                 // PT_INTERP
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_TYPE);

    mk_good(img); p16(img, 42, 20);                 // wrong phentsize
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_PHDR);
    mk_good(img); p16(img, 44, 0);                  // no headers
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_PHDR);
    mk_good(img); p16(img, 44, 200);                // absurd count
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_PHDR);
    mk_good(img); p32(img, 28, IMG_LEN - 16);       // table runs off the file
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_TRUNC);
    mk_good(img); p32(img, 28, 0xFFFFFFF0u);        // phoff that would wrap
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_TRUNC);
}

KTEST(elf, rejects_segments_outside_the_user_range){
    mk_good(img); p32(img, PH0 + 8, PROC_BASE - 0x1000);        // below the range
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_SEGMENT);
    mk_good(img); p32(img, PH0 + 8, 0x100000);                  // kernel memory
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_SEGMENT);
    mk_good(img); p32(img, PH0 + 8, PROC_BASE + PROC_IMAGE_MAX); // just past the end
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_SEGMENT);
    mk_good(img); p32(img, PH0 + 20, PROC_IMAGE_MAX + 1);       // one byte too big
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_SEGMENT);
    mk_good(img); p32(img, PH0 + 8, 0xFFFFFFF0u); p32(img, PH0 + 16, 0x10); p32(img, PH0 + 20, 0x40);   // wraps past 4 GB
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_SEGMENT);
    mk_good(img); p32(img, PH0 + 20, 0xFFFFFFFFu);              // 4 GB of memory
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_SEGMENT);

    // The largest legal segment is accepted, so the bound is exact.
    mk_good(img); p32(img, PH0 + 20, PROC_IMAGE_MAX);
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_OK);
}

KTEST(elf, rejects_bad_file_extents){
    mk_good(img); p32(img, PH0 + 16, IMG_LEN + 1);              // data past EOF
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_FILESZ);
    mk_good(img); p32(img, PH0 + 4, IMG_LEN - 4); p32(img, PH0 + 16, 8);
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_FILESZ);
    mk_good(img); p32(img, PH0 + 4, 0xFFFFFFF0u); p32(img, PH0 + 16, 0x20);   // offset+filesz wraps
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_FILESZ);
    mk_good(img); p32(img, PH0 + 16, 4097);                     // filesz > memsz
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_FILESZ);
}

KTEST(elf, rejects_bad_entry_and_layout){
    mk_good(img); p32(img, 24, 0x1000);                         // entry in low memory
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_ENTRY);
    mk_good(img); p32(img, 24, PROC_BASE + 2048);               // in bss, not file-backed
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_ENTRY);
    mk_good(img); p32(img, 24, PROC_BASE + IMG_LEN);            // first byte past the data
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_ENTRY);
    mk_good(img); p32(img, PH0, 4);                             // PT_NOTE only: nothing to load
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_NOLOAD);

    // Two segments claiming the same bytes.
    mk_good(img);
    p16(img, 44, 2);
    memcpy(img + PH0 + 32, img + PH0, 32);
    p32(img, PH0 + 32 + 8, PROC_BASE + 0x800);
    KT_EQ(proc_elf_parse(img, IMG_LEN, 0), PROC_ELF_SEGMENT);

    // ...and two that sit side by side are fine.
    p32(img, PH0 + 32 + 8, PROC_BASE + 0x1000);
    p32(img, PH0 + 32 + 4, 0);
    proc_elf_t e;
    KT_EQ(proc_elf_parse(img, IMG_LEN, &e), PROC_ELF_OK);
    KT_EQ(e.nload, 2);
    KT_EQ(e.span, 0x2000);
}

// ------------------------------------------------------------ syscalls

static int sc(uint32_t nr, uint32_t a, uint32_t b, uint32_t c){
    syscall_regs_t r;
    memset(&r, 0, sizeof(r));
    r.eax = nr; r.ebx = a; r.ecx = b; r.edx = c;
    syscall_dispatch(&r);
    return (int)r.eax;
}

// Errors come back as negative errno values; most checks only care that it failed.
#define KT_ERR(e) KT_TRUE((e) < 0)

#define EFAULT_  (-14)
#define ERANGE_  (-34)

KTEST(syscall, pointer_validation){
    uint32_t frames = pmm_free_frames();
    proc_t* p = proc_test_create();
    KT_NOTNULL(p);
    if (!p) return;
    proc_test_bind(p);
    char* u = (char*)PROC_BASE;
    memcpy(u, "abc", 4);

    // Inside the image, the stack, and nowhere else.
    KT_TRUE(proc_check_range(PROC_BASE, 4096, 0));
    KT_FALSE(proc_check_range(PROC_BASE, 4097, 0));
    KT_FALSE(proc_check_range(PROC_BASE - 1, 2, 0));
    KT_TRUE(proc_check_range(PROC_STACK_TOP - PROC_STACK_SIZE, PROC_STACK_SIZE, 1));
    KT_TRUE(proc_check_range(PROC_STACK_TOP - 4, 4, 0));
    KT_FALSE(proc_check_range(PROC_STACK_TOP - 4, 5, 0));
    KT_FALSE(proc_check_range(PROC_STACK_TOP - PROC_STACK_SIZE - 1, 1, 0));   // grows on a fault, not on a check
    KT_TRUE(proc_check_range(0xDEAD0000u, 0, 0));           // empty is trivially fine
    KT_FALSE(proc_check_range(0x100000, 4, 0));             // kernel
    KT_FALSE(proc_check_range(0, 1, 0));
    KT_FALSE(proc_check_range(0xFFFFFFF0u, 0x20, 0));       // wraps
    KT_FALSE(proc_check_range(PROC_BASE, 0xFFFFFFFFu, 0));

    char s[16];
    KT_EQ(proc_copy_string(PROC_BASE, s, sizeof(s)), 0);
    KT_STREQ(s, "abc");
    memset(u + 100, 'x', 3996);                             // unterminated to the page end
    KT_EQ(proc_copy_string(PROC_BASE + 100, s, sizeof(s)), VFS_ENAMETOOLONG);
    static char big[4200];
    KT_EQ(proc_copy_string(PROC_BASE + 100, big, sizeof(big)), EFAULT_);   // runs off the image
    KT_EQ(proc_copy_string(0x100000, s, sizeof(s)), EFAULT_);

    // The same rules through the handlers.
    KT_EQ(sc(SYS_WRITE, 1, PROC_BASE, 3), 3);
    KT_EQ(sc(SYS_WRITE, 2, PROC_BASE, 3), 3);
    KT_EQ(sc(SYS_WRITE, 1, 0x100000, 3), EFAULT_);
    KT_EQ(sc(SYS_WRITE, 1, PROC_BASE, 4097), VFS_EINVAL);   // console write cap
    KT_EQ(sc(SYS_WRITE, 1, PROC_BASE, 0x7FFFFFFF), VFS_EINVAL);
    KT_EQ(sc(SYS_WRITE, 1, 0xFFFFFFF0u, 0x20), EFAULT_);
    KT_EQ(sc(SYS_WRITE, 9, PROC_BASE, 3), VFS_EBADF);       // no such fd
    KT_EQ(sc(SYS_WRITE, 0, PROC_BASE, 3), VFS_EBADF);       // stdin is not writable
    KT_EQ(sc(SYS_READ, 0, PROC_BASE, 8), 0);                // stdin: EOF
    KT_EQ(sc(SYS_READ, 0, 0x100000, 8), EFAULT_);
    KT_EQ(sc(SYS_READ, 5, PROC_BASE, 8), VFS_EBADF);
    KT_EQ(sc(SYS_READ, (uint32_t)-1, PROC_BASE, 8), VFS_EBADF);
    KT_EQ(sc(SYS_READ, 100, PROC_BASE, 8), VFS_EBADF);
    KT_EQ(sc(SYS_OPEN, 0x100000, O_RDONLY, 0), EFAULT_);
    KT_EQ(sc(SYS_OPEN, PROC_BASE + 100, O_RDONLY, 0), VFS_ENAMETOOLONG);   // unterminated name
    KT_EQ(sc(SYS_FSIZE, 0x100000, 0, 0), EFAULT_);
    KT_EQ(sc(SYS_READFILE, PROC_BASE, 0x100000, 16), EFAULT_);
    KT_EQ(sc(SYS_READDIR, PROC_BASE, 0, 0x100000), EFAULT_);
    KT_EQ(sc(SYS_STAT, PROC_BASE, 0x100000, 0), EFAULT_);
    KT_EQ(sc(SYS_STAT, 0x100000, PROC_BASE, 0), EFAULT_);
    KT_EQ(sc(SYS_FSTAT, 1, 0x100000, 0), EFAULT_);
    KT_EQ(sc(SYS_FSTAT, 77, PROC_BASE, 0), VFS_EBADF);
    KT_EQ(sc(SYS_MKDIR, 0x100000, 0, 0), EFAULT_);
    KT_EQ(sc(SYS_UNLINK, 0x100000, 0, 0), EFAULT_);
    KT_EQ(sc(SYS_RMDIR, 0x100000, 0, 0), EFAULT_);
    KT_EQ(sc(SYS_CHDIR, 0x100000, 0, 0), EFAULT_);
    KT_EQ(sc(SYS_RENAME, PROC_BASE, 0x100000, 0), EFAULT_);
    KT_EQ(sc(SYS_SPAWN, 0x100000, 0, 0), EFAULT_);
    KT_EQ(sc(SYS_SPAWN, PROC_BASE, 0x100000, 0), EFAULT_);  // bad args pointer
    KT_EQ(sc(SYS_WAITPID, 1, 0x100000, 0), EFAULT_);
    KT_EQ(sc(SYS_GETCWD, 0x100000, 8, 0), EFAULT_);
    KT_EQ(sc(SYS_GETCWD, PROC_BASE, 1, 0), ERANGE_);        // too small for "/"
    KT_EQ(sc(SYS_GETCWD, PROC_BASE, 8, 0), 2);
    KT_EQ(u[0], '/');
    KT_EQ(sc(SYS_PIPE, 0x100000, 0, 0), EFAULT_);
    KT_EQ(sc(SYS_DUP, 1, 0, 0), 3);                         // the failed pipe() left no descriptors behind
    KT_EQ(sc(SYS_DUP, 99, 0, 0), VFS_EBADF);
    KT_EQ(sc(SYS_DUP2, 99, 5, 0), VFS_EBADF);
    KT_EQ(sc(SYS_KILL, 999999, 15, 0), SIGNAL_ESRCH);
    KT_EQ(sc(SYS_KILL, proc_current_pid(), 77, 0), SIGNAL_EINVAL);
    KT_EQ(sc(SYS_MMAP, 0, 3, 0), VFS_EINVAL);
    KT_EQ(sc(SYS_MMAP, 4096, 0, 0), VFS_EINVAL);
    KT_EQ(sc(SYS_MUNMAP, PROC_BASE + 1, 4096, 0), VFS_EINVAL);
    KT_EQ(sc(999, 0, 0, 0), -1);

    proc_test_destroy(p);
    KT_EQ(pmm_free_frames(), frames);
}

KTEST(syscall, paths_resolve_through_the_vfs){
    proc_t* p = proc_test_create();
    KT_NOTNULL(p);
    if (!p) return;
    proc_test_bind(p);
    char* u = (char*)PROC_BASE;

    // Not found, however they are spelled; ".." at the root stays at the root.
    static const char* bad[] = { "../X", "A/B.TXT", "C:X", "\\X", "NOSUCH.FIL", "/nodir/x" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++){
        strcpy(u, bad[i]);
        KT_ERR(sc(SYS_OPEN, PROC_BASE, O_RDONLY, 0));
        KT_EQ(sc(SYS_SPAWN, PROC_BASE, 0, 0), -1);
    }
    strcpy(u, "/");
    KT_EQ(sc(SYS_SPAWN, PROC_BASE, 0, 0), -1);              // a directory is not a program
    strcpy(u, "NOSUCH.FIL");
    KT_ERR(sc(SYS_OPEN, PROC_BASE, O_RDONLY, 0));
    KT_ERR(sc(SYS_OPEN, PROC_BASE, 3, 0));                  // invalid access mode

    // Directories open read-only and refuse reads and writes.
    strcpy(u, "/");
    int fd = sc(SYS_OPEN, PROC_BASE, O_RDONLY, 0);
    KT_TRUE(fd >= 3);
    KT_ERR(sc(SYS_READ, (uint32_t)fd, PROC_BASE + 64, 4));
    KT_EQ(sc(SYS_CLOSE, (uint32_t)fd, 0, 0), 0);

    proc_test_destroy(p);
}

KTEST(syscall, heap_grows_shrinks_and_is_bounded){
    uint32_t frames = pmm_free_frames();
    proc_t* p = proc_test_create();
    KT_NOTNULL(p);
    if (!p) return;
    proc_test_bind(p);

    uint32_t base = PROC_BASE + 4096 + VM_HEAP_GAP;
    KT_EQ(sc(SYS_BRK, 0, 0, 0), base);                      // query

    // Nothing there yet.
    KT_FALSE(proc_check_range(base, 1, 0));

    KT_EQ(sc(SYS_SBRK, 5000, 0, 0), base);                  // returns the old break
    KT_EQ(sc(SYS_BRK, 0, 0, 0), base + 5000);
    KT_TRUE(proc_check_range(base, 5000, 1));
    KT_TRUE(proc_check_range(base, 8192, 1));               // regions are whole pages...
    KT_FALSE(proc_check_range(base, 8193, 0));              // ...so the check ends at the page
    KT_FALSE(proc_check_range(PROC_BASE + 4090, 100, 0));   // the guard gap between image and heap

    char* h = (char*)base;
    for (int i = 0; i < 5000; i++) h[i] = (char)0xAA;       // demand-zero pages arrive on touch
    KT_EQ(sc(SYS_WRITE, 1, base + 4999, 1), 1);
    KT_EQ(sc(SYS_WRITE, 1, base + 8191, 2), EFAULT_);

    // Shrink, then regrow over the old bytes: they must come back zeroed.
    KT_EQ(sc(SYS_SBRK, (uint32_t)-5000, 0, 0), base + 5000);
    KT_FALSE(proc_check_range(base, 1, 0));
    KT_EQ(sc(SYS_BRK, base + 3000, 0, 0), base + 3000);
    int nonzero = 0;
    for (int i = 0; i < 3000; i++) if (h[i]) nonzero++;
    KT_EQ(nonzero, 0);

    // Refusals leave the break where it was.
    KT_EQ(sc(SYS_BRK, base - 1, 0, 0), -1);
    KT_EQ(sc(SYS_BRK, 0x100000, 0, 0), -1);
    KT_EQ(sc(SYS_BRK, 0x60000000, 0, 0), -1);
    KT_EQ(sc(SYS_SBRK, 0x7FFFFFFF, 0, 0), -1);
    KT_EQ(sc(SYS_SBRK, (uint32_t)-0x7FFFFFFF, 0, 0), -1);
    KT_EQ(sc(SYS_BRK, 0, 0, 0), base + 3000);

    proc_test_destroy(p);
    KT_EQ(pmm_free_frames(), frames);                       // heap frames all came back
}

KTEST(syscall, mmap_and_stack_growth){
    uint32_t frames = pmm_free_frames();
    proc_t* p = proc_test_create();
    KT_NOTNULL(p);
    if (!p) return;
    proc_test_bind(p);

    uint32_t a = (uint32_t)sc(SYS_MMAP, 10000, 3, 0);       // read+write, rounded to 3 pages
    KT_TRUE(a >= VM_MMAP_BASE && a < VM_MMAP_LIMIT);
    KT_TRUE(proc_check_range(a, 12288, 1));
    KT_FALSE(proc_check_range(a, 12289, 0));
    volatile char* m = (volatile char*)a;
    KT_EQ(m[0], 0);                                         // anonymous memory is zero
    m[12287] = 5;
    KT_EQ(sc(SYS_MUNMAP, a, 12288, 0), 0);
    KT_FALSE(proc_check_range(a, 1, 0));
    uint32_t b = (uint32_t)sc(SYS_MMAP, 4096, 1, 0);        // read-only: the kernel may not write into it
    KT_TRUE(b != 0);
    KT_FALSE(proc_check_range(b, 1, 1));
    KT_TRUE(proc_check_range(b, 1, 0));
    KT_EQ(sc(SYS_MUNMAP, b, 4096, 0), 0);
    KT_EQ(sc(SYS_MUNMAP, PROC_BASE, 4096, 0), 0);           // the image is unmappable too
    KT_EQ(sc(SYS_MUNMAP, p->vm->heap_start, 4096, 0), 0);   // holes are fine

    // The stack grows downward on a fault below its initial extent.
    uint32_t low = PROC_STACK_TOP - PROC_STACK_SIZE - 3 * 4096;
    KT_FALSE(proc_check_range(low, 1, 1));
    *(volatile char*)low = 9;
    KT_EQ(*(volatile char*)low, 9);
    KT_TRUE(proc_check_range(low, 1, 1));

    proc_test_destroy(p);
    KT_EQ(pmm_free_frames(), frames);
}

KTEST(syscall, file_round_trip_through_descriptors){
    if (!ata_present() || !fat32_writable()){ ktest_skip("no writable disk"); return; }
    proc_t* p = proc_test_create();
    KT_NOTNULL(p);
    if (!p) return;
    proc_test_bind(p);
    char* u = (char*)PROC_BASE;

    strcpy(u, "KTUSER.TMP");
    int fd = sc(SYS_OPEN, PROC_BASE, O_RDWR | O_CREAT | O_TRUNC, 0);
    KT_TRUE(fd >= 3);

    strcpy(u + 64, "the quick brown fox");
    KT_EQ(sc(SYS_WRITE, fd, PROC_BASE + 64, 19), 19);
    KT_EQ(sc(SYS_LSEEK, fd, (uint32_t)-3, 2), 16);          // from the end
    KT_EQ(sc(SYS_READ, fd, PROC_BASE + 128, 10), 3);
    KT_EQ(memcmp(u + 128, "fox", 3), 0);
    KT_EQ(sc(SYS_LSEEK, fd, 4, 0), 4);
    KT_EQ(sc(SYS_WRITE, fd, PROC_BASE + 64, 3), 3);         // overwrite in place
    KT_ERR(sc(SYS_LSEEK, fd, (uint32_t)-1, 0));
    KT_ERR(sc(SYS_LSEEK, fd, 0, 7));                        // bad whence
    KT_EQ(sc(SYS_LSEEK, fd, 30, 0), 30);                    // past the end...
    KT_EQ(sc(SYS_WRITE, fd, PROC_BASE + 64, 1), 1);         // ...writing zero-fills the gap
    KT_EQ(sc(SYS_CLOSE, fd, 0, 0), 0);
    KT_ERR(sc(SYS_CLOSE, fd, 0, 0));
    KT_ERR(sc(SYS_READ, fd, PROC_BASE + 128, 4));           // closed

    // What reached the disk is what was written.
    fat32_dirent_t f;
    KT_EQ(fat32_find(fat32_root_cluster(), "KTUSER.TMP", &f), 0);
    KT_EQ(f.size, 31);
    uint8_t back[32];
    KT_EQ(fat32_read_file(&f, back, sizeof(back)), 31);
    KT_EQ(memcmp(back, "the ", 4), 0);
    KT_EQ(memcmp(back + 4, "the", 3), 0);
    KT_EQ(back[19], 0);
    KT_EQ(back[30], 't');

    // Reopen read-only: reads work, writes are refused.
    fd = sc(SYS_OPEN, PROC_BASE, O_RDONLY, 0);
    KT_TRUE(fd >= 3);
    KT_ERR(sc(SYS_WRITE, fd, PROC_BASE + 64, 1));
    KT_EQ(sc(SYS_FSIZE, PROC_BASE, 0, 0), 31);
    KT_EQ(sc(SYS_READ, fd, PROC_BASE + 128, 100), 31);
    KT_EQ(sc(SYS_READ, fd, PROC_BASE + 128, 100), 0);       // EOF
    KT_EQ(sc(SYS_CLOSE, fd, 0, 0), 0);

    // Append, then leave it open: destroying the process must still flush.
    fd = sc(SYS_OPEN, PROC_BASE, O_WRONLY | O_APPEND, 0);
    KT_TRUE(fd >= 3);
    KT_EQ(sc(SYS_WRITE, fd, PROC_BASE + 64, 2), 2);
    proc_test_destroy(p);
    KT_EQ(fat32_find(fat32_root_cluster(), "KTUSER.TMP", &f), 0);
    KT_EQ(f.size, 33);

    fat32_delete(fat32_root_cluster(), "KTUSER.TMP");
}

KTEST(syscall, descriptor_table_fills_and_recovers){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), "HELLO.BIN", &f) != 0){
        ktest_skip("HELLO.BIN not on the disk image"); return;
    }
    proc_t* p = proc_test_create();
    KT_NOTNULL(p);
    if (!p) return;
    proc_test_bind(p);
    strcpy((char*)PROC_BASE, "HELLO.BIN");

    int fds[VFS_FD_MAX];
    int n = 0;
    for (;;){
        int fd = sc(SYS_OPEN, PROC_BASE, O_RDONLY, 0);
        if (fd < 0) break;
        fds[n++] = fd;
        if (n > VFS_FD_MAX) break;
    }
    KT_EQ(n, VFS_FD_MAX - 3);                               // 0..2 are the console
    KT_EQ(fds[0], 3);
    KT_EQ(sc(SYS_CLOSE, (uint32_t)fds[1], 0, 0), 0);
    KT_EQ(sc(SYS_OPEN, PROC_BASE, O_RDONLY, 0), fds[1]);    // lowest slot reused

    // The file's own bytes come back through read.
    KT_EQ(sc(SYS_READ, 3, PROC_BASE + 256, 4), 4);
    proc_test_destroy(p);
}

KTEST(syscall, dup_pipe_and_cwd){
    proc_t* p = proc_test_create();
    KT_NOTNULL(p);
    if (!p) return;
    proc_test_bind(p);
    char* u = (char*)PROC_BASE;

    // pipe(): what goes in one end comes out the other; dup2 aliases.
    KT_EQ(sc(SYS_PIPE, PROC_BASE + 512, 0, 0), 0);
    int rfd = ((int*)(u + 512))[0], wfd = ((int*)(u + 512))[1];
    KT_TRUE(rfd >= 3 && wfd > rfd);
    int w2 = sc(SYS_DUP, (uint32_t)wfd, 0, 0);
    KT_TRUE(w2 > wfd);
    strcpy(u + 64, "ping");
    KT_EQ(sc(SYS_WRITE, (uint32_t)w2, PROC_BASE + 64, 4), 4);
    KT_EQ(sc(SYS_READ, (uint32_t)rfd, PROC_BASE + 128, 16), 4);
    KT_EQ(memcmp(u + 128, "ping", 4), 0);
    KT_EQ(sc(SYS_CLOSE, (uint32_t)wfd, 0, 0), 0);
    KT_EQ(sc(SYS_CLOSE, (uint32_t)w2, 0, 0), 0);
    KT_EQ(sc(SYS_READ, (uint32_t)rfd, PROC_BASE + 128, 16), 0);   // every writer gone: EOF
    KT_EQ(sc(SYS_DUP2, (uint32_t)rfd, 9, 0), 9);
    KT_EQ(sc(SYS_CLOSE, (uint32_t)rfd, 0, 0), 0);
    KT_EQ(sc(SYS_CLOSE, 9, 0, 0), 0);

    // fstat sees what fd 1 is; stat of the root is a directory.
    vfs_stat_t st;
    KT_EQ(sc(SYS_FSTAT, 1, PROC_BASE + 256, 0), 0);
    memcpy(&st, u + 256, sizeof(st));
    KT_EQ(st.type, VFS_T_CHR);
    strcpy(u, "/");
    KT_EQ(sc(SYS_STAT, PROC_BASE, PROC_BASE + 256, 0), 0);
    memcpy(&st, u + 256, sizeof(st));
    KT_EQ(st.type, VFS_T_DIR);

    // chdir/getcwd, against a directory that is always there.
    strcpy(u, "/dev");
    KT_EQ(sc(SYS_CHDIR, PROC_BASE, 0, 0), 0);
    KT_EQ(sc(SYS_GETCWD, PROC_BASE + 64, 32, 0), 5);
    KT_STREQ(u + 64, "/dev");
    strcpy(u, "console");                                   // relative to the new cwd
    int fd = sc(SYS_OPEN, PROC_BASE, O_WRONLY, 0);
    KT_TRUE(fd >= 3);
    sc(SYS_CLOSE, (uint32_t)fd, 0, 0);
    strcpy(u, "..");
    KT_EQ(sc(SYS_CHDIR, PROC_BASE, 0, 0), 0);
    KT_EQ(sc(SYS_GETCWD, PROC_BASE + 64, 32, 0), 2);
    strcpy(u, "/dev/null");
    KT_ERR(sc(SYS_CHDIR, PROC_BASE, 0, 0));                 // not a directory

    proc_test_destroy(p);
}

// ------------------------------------------------------------ programs

static int have(const char* name){
    fat32_dirent_t f;
    return ata_present() && fat32_find(fat32_root_cluster(), name, &f) == 0;
}

KTEST(proc, elf_hello_round_trip){
    if (!have("HI.ELF")){ ktest_skip("HI.ELF not on the disk image (run: make disk-sync)"); return; }
    uint32_t frames = pmm_free_frames();

    int pid = proc_spawn_args("HI.ELF", "alpha beta");
    KT_TRUE(pid > 0);
    if (pid <= 0) return;
    int code = -99;
    KT_EQ(proc_wait(pid, &code), pid);
    KT_EQ(code, 0);                                         // .data intact, .bss zero
    KT_EQ(proc_wait(pid, &code), -1);                       // reaped once
    KT_EQ(proc_count(), 0);

    task_sleep(120);
    KT_TRUE(pmm_free_frames() + 8 >= frames);
}

KTEST(proc, wait_rejects_strangers){
    KT_EQ(proc_wait(0, 0), -1);
    KT_EQ(proc_wait(-5, 0), -1);
    KT_EQ(proc_wait(123456, 0), -1);
}

KTEST(proc, corrupt_elf_on_disk_is_refused){
    if (!ata_present() || !fat32_writable()){ ktest_skip("no writable disk"); return; }
    mk_good(img);
    p32(img, PH0 + 8, 0x100000);                            // segment over kernel memory
    KT_EQ(fat32_write_file(fat32_root_cluster(), "BADELF.TMP", img, IMG_LEN), 0);
    KT_EQ(proc_spawn("BADELF.TMP"), -1);
    KT_EQ(proc_count(), 0);

    // Wrong architecture: refused the same way, and again nothing leaks.
    mk_good(img);
    p16(img, 18, 62);                                       // x86-64
    KT_EQ(fat32_write_file(fat32_root_cluster(), "BADELF.TMP", img, IMG_LEN), 0);
    KT_EQ(proc_spawn("BADELF.TMP"), -1);
    fat32_delete(fat32_root_cluster(), "BADELF.TMP");
}

KTEST(proc, cat_and_ls_run){
    if (!have("CAT.ELF") || !have("LS.ELF")){ ktest_skip("CAT.ELF/LS.ELF not on the disk image"); return; }
    int code = -99;

    int pid = proc_spawn_args("CAT.ELF", "HELLO.TXT");
    KT_TRUE(pid > 0);
    if (pid > 0){ KT_EQ(proc_wait(pid, &code), pid); KT_EQ(code, 0); }

    pid = proc_spawn_args("CAT.ELF", "NOSUCH.FIL");         // failure is reported, not fatal
    KT_TRUE(pid > 0);
    if (pid > 0){ KT_EQ(proc_wait(pid, &code), pid); KT_EQ(code, 1); }

    pid = proc_spawn("LS.ELF");
    KT_TRUE(pid > 0);
    if (pid > 0){ KT_EQ(proc_wait(pid, &code), pid); KT_EQ(code, 0); }
}

KTEST(proc, utest_program_passes_every_check){
    if (!have("UTEST.ELF")){ ktest_skip("UTEST.ELF not on the disk image"); return; }
    int pid = proc_spawn("UTEST.ELF");
    KT_TRUE(pid > 0);
    if (pid <= 0) return;
    int code = -99;
    KT_EQ(proc_wait(pid, &code), pid);
    KT_EQ(code, 0);                                         // the failure count
    KT_EQ(proc_count(), 0);
    fat32_delete(fat32_root_cluster(), "UTEST.TMP");
}
