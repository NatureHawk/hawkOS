#pragma once
#include <stdint.h>
#include <stddef.h>

// In-kernel test harness.
//
// The tests run inside the real kernel, on the real hardware the kernel
// targets, against the real allocators and drivers. That is not a compromise
// forced by the lack of a host build -- it is the only way an assertion about
// this system means anything. A FAT32 write test that passes against a mock
// block device has tested the mock. This one runs the actual ATA PIO path
// against the actual disk image.
//
// A test is registered by defining it with KTEST(). The entry lands in a
// .ktests section that linker.ld brackets with __ktests_start/__ktests_end,
// so tests live next to the code they exercise and nothing has to maintain a
// central list that would go stale the first time someone forgot it.
//
// Boot with "selftest" on the multiboot command line to run them instead of
// the desktop -- see `make test`.

typedef struct {
    const char* suite;
    const char* name;
    void      (*fn)(void);
} ktest_t;

#define KTEST(suite_, name_)                                                  \
    static void ktest_fn_##suite_##_##name_(void);                            \
    __attribute__((section(".ktests"), used))                                 \
    static const ktest_t ktest_ent_##suite_##_##name_ = {                     \
        #suite_, #name_, ktest_fn_##suite_##_##name_                          \
    };                                                                        \
    static void ktest_fn_##suite_##_##name_(void)

// Records a failure against the running test. Reporting rather than
// aborting: one test that fails an early assertion usually fails several,
// and seeing all of them says more about what broke than the first does.
void ktest_fail(const char* file, int line, const char* what);
void ktest_fail_eq(const char* file, int line, const char* what,
                   long got, long want);
void ktest_fail_str(const char* file, int line, const char* what,
                    const char* got, const char* want);

// Marks the rest of the current test as skipped, with a reason. For things
// that legitimately cannot run in this environment -- a disk test with no
// disk attached -- as distinct from a failure.
void ktest_skip(const char* why);
int  ktest_skipped(void);

#define KT_TRUE(cond)                                                         \
    do { if (ktest_skipped()) break;                                          \
         if (!(cond)) ktest_fail(__FILE__, __LINE__, #cond); } while (0)

#define KT_FALSE(cond)                                                        \
    do { if (ktest_skipped()) break;                                          \
         if ((cond)) ktest_fail(__FILE__, __LINE__, "!(" #cond ")"); } while (0)

#define KT_EQ(got_, want_)                                                    \
    do { if (ktest_skipped()) break;                                          \
         long g_ = (long)(got_), w_ = (long)(want_);                          \
         if (g_ != w_) ktest_fail_eq(__FILE__, __LINE__,                      \
                                     #got_ " == " #want_, g_, w_); } while (0)

#define KT_NE(got_, want_)                                                    \
    do { if (ktest_skipped()) break;                                          \
         long g_ = (long)(got_), w_ = (long)(want_);                          \
         if (g_ == w_) ktest_fail_eq(__FILE__, __LINE__,                      \
                                     #got_ " != " #want_, g_, w_); } while (0)

#define KT_STREQ(got_, want_)                                                 \
    do { if (ktest_skipped()) break;                                          \
         const char* g_ = (got_); const char* w_ = (want_);                   \
         if (!g_ || !w_ || strcmp(g_, w_) != 0)                               \
             ktest_fail_str(__FILE__, __LINE__, #got_ " == " #want_, g_, w_); \
    } while (0)

#define KT_MEMEQ(got_, want_, n_)                                             \
    do { if (ktest_skipped()) break;                                          \
         if (memcmp((got_), (want_), (n_)) != 0)                              \
             ktest_fail(__FILE__, __LINE__,                                   \
                        "memcmp(" #got_ ", " #want_ ", " #n_ ")"); } while (0)

#define KT_NOTNULL(p_)                                                        \
    do { if (ktest_skipped()) break;                                          \
         if (!(p_)) ktest_fail(__FILE__, __LINE__, #p_ " != NULL"); } while (0)

// Runs every registered test and reports. Returns the number that failed.
int ktest_run_all(void);

// True when "selftest" was passed on the multiboot command line.
int ktest_requested(uint32_t mb_magic, uint32_t mb_info);

// Asks QEMU to terminate with a code derived from the failure count. Does not
// return.
void ktest_exit(int failed);
