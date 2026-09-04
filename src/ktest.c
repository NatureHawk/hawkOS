// src/ktest.c — the test runner
//
// Output goes to the serial port in a fixed, greppable shape so tools/test.py
// can decide pass or fail without parsing prose:
//
//     ok   pmm/used_kb_moves
//     SKIP fat32/write            no FAT32 volume attached
//     FAIL fat32/write_roundtrip  src/fat32.c:212: n == 512 (got 0, want 512)
//     1..24 passed=23 failed=1 skipped=0
//
// The guest then asks QEMU to exit through the isa-debug-exit device, so a
// run is a normal process that succeeds or fails rather than a VM someone has
// to watch and kill.
#include <stdint.h>
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/kprintf.h"
#include "header/io.h"
#include "header/multiboot.h"
#include "serial.h"

// Bracketed by linker.ld. Declared as arrays so the symbols' addresses are
// the section bounds; taking &__ktests_start would give the wrong thing.
extern const ktest_t __ktests_start[];
extern const ktest_t __ktests_end[];

#define DEBUG_EXIT_PORT 0xF4

static int cur_failed = 0;
static int cur_skip   = 0;
static char skip_why[64];

static void out(const char* s){ serial_puts(s); }

void ktest_fail(const char* file, int line, const char* what){
    char buf[192];
    ksnprintf(buf, sizeof(buf), "\n     %s:%d: %s", file, line, what);
    out(buf);
    cur_failed++;
}

void ktest_fail_eq(const char* file, int line, const char* what,
                   long got, long want){
    char buf[224];
    ksnprintf(buf, sizeof(buf), "\n     %s:%d: %s (got %d, want %d)",
              file, line, what, (int)got, (int)want);
    out(buf);
    cur_failed++;
}

void ktest_fail_str(const char* file, int line, const char* what,
                    const char* got, const char* want){
    char buf[288];
    ksnprintf(buf, sizeof(buf), "\n     %s:%d: %s (got \"%s\", want \"%s\")",
              file, line, what, got ? got : "(null)", want ? want : "(null)");
    out(buf);
    cur_failed++;
}

void ktest_skip(const char* why){
    cur_skip = 1;
    strncpy(skip_why, why ? why : "", sizeof(skip_why) - 1);
    skip_why[sizeof(skip_why) - 1] = 0;
}

int ktest_skipped(void){ return cur_skip; }

int ktest_run_all(void){
    int total = (int)(__ktests_end - __ktests_start);
    int passed = 0, failed = 0, skipped = 0;

    char buf[160];
    ksnprintf(buf, sizeof(buf), "\n=== hawkOS self-test: %d tests ===\n", total);
    out(buf);

    for (const ktest_t* t = __ktests_start; t < __ktests_end; t++){
        cur_failed = 0;
        cur_skip   = 0;
        skip_why[0] = 0;

        // The label goes out before the test runs, so a test that hangs or
        // triple-faults names itself in the log instead of leaving the last
        // successful test looking like the culprit.
        ksnprintf(buf, sizeof(buf), "     %s/%s", t->suite, t->name);
        out(buf);

        t->fn();

        if (cur_failed){
            ksnprintf(buf, sizeof(buf), "\r FAIL %s/%s\n", t->suite, t->name);
            failed++;
        } else if (cur_skip){
            ksnprintf(buf, sizeof(buf), "\r SKIP %s/%s  (%s)\n",
                      t->suite, t->name, skip_why);
            skipped++;
        } else {
            ksnprintf(buf, sizeof(buf), "\r ok   %s/%s\n", t->suite, t->name);
            passed++;
        }
        out(buf);
    }

    ksnprintf(buf, sizeof(buf), "\n1..%d passed=%d failed=%d skipped=%d\n",
              total, passed, failed, skipped);
    out(buf);
    return failed;
}

int ktest_requested(uint32_t mb_magic, uint32_t mb_info){
    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC || !mb_info) return 0;
    const multiboot_info_t* mbi = (const multiboot_info_t*)mb_info;
    if (!(mbi->flags & MULTIBOOT_FLAG_CMDLINE) || !mbi->cmdline) return 0;
    return strstr((const char*)mbi->cmdline, "selftest") != 0;
}

// QEMU's isa-debug-exit device turns a port write into a process exit code of
// (value << 1) | 1, so it can never collide with 0 -- which is why the
// success code is 0x10 (33) rather than something that looks like success by
// accident.
void ktest_exit(int failed){
    outb(DEBUG_EXIT_PORT, failed ? 0x11 : 0x10);
    // If the device is not attached the write is a no-op; park rather than
    // fall back into a half-initialised system.
    for(;;) __asm__ __volatile__("hlt");
}
