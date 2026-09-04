// src/test_kstring.c — the freestanding libc replacements
//
// These are worth testing precisely because they look too simple to break.
// GCC lowers struct assignment and array initialisation into memcpy/memset
// calls, so a bug in one of these does not show up as a wrong string -- it
// shows up as a corrupted kernel structure three subsystems away.
#include "header/ktest.h"
#include "header/kstring.h"

KTEST(kstring, memset_and_memcpy){
    uint8_t a[32], b[32];

    memset(a, 0xAB, sizeof(a));
    for (int i = 0; i < 32; i++) KT_EQ(a[i], 0xAB);

    memset(b, 0, sizeof(b));
    memcpy(b, a, 16);
    KT_MEMEQ(b, a, 16);
    KT_EQ(b[16], 0);          // must not have run past the length
}

KTEST(kstring, memmove_overlaps_both_ways){
    // The whole point of memmove over memcpy. A forward copy into an
    // overlapping destination smears the first byte across the range if the
    // direction is not chosen from the operand order.
    char up[16] = "abcdefgh";
    memmove(up + 2, up, 6);
    KT_STREQ(up, "ababcdef");

    char down[16] = "abcdefgh";
    memmove(down, down + 2, 6);
    KT_STREQ(down, "cdefghgh");
}

KTEST(kstring, memcmp_sign_and_zero){
    KT_EQ(memcmp("abc", "abc", 3), 0);
    KT_TRUE(memcmp("abc", "abd", 3) < 0);
    KT_TRUE(memcmp("abd", "abc", 3) > 0);
    KT_EQ(memcmp("abc", "abd", 2), 0);      // bounded by n
}

KTEST(kstring, strlen_and_strcmp){
    KT_EQ(strlen(""), 0);
    KT_EQ(strlen("hawkOS"), 6);
    KT_EQ(strcmp("a", "a"), 0);
    KT_TRUE(strcmp("a", "b") < 0);
    KT_TRUE(strcmp("b", "a") > 0);
    KT_EQ(strncmp("abcdef", "abcXXX", 3), 0);
    KT_TRUE(strncmp("abcdef", "abcXXX", 4) != 0);
}

KTEST(kstring, strncpy_truncates_and_pads){
    char dst[8];

    memset(dst, 'Z', sizeof(dst));
    strncpy(dst, "hi", sizeof(dst));
    KT_STREQ(dst, "hi");

    // Source longer than the destination: the whole buffer is written and no
    // terminator is added, which is what callers here compensate for by
    // writing dst[cap-1] = 0 themselves.
    memset(dst, 'Z', sizeof(dst));
    strncpy(dst, "abcdefghijk", sizeof(dst));
    KT_MEMEQ(dst, "abcdefgh", 8);
}

KTEST(kstring, strchr_and_strstr){
    const char* s = "https://example.com/path";
    KT_TRUE(strchr(s, ':') == s + 5);
    KT_TRUE(strchr(s, '?') == 0);
    KT_TRUE(strstr(s, "example") == s + 8);
    KT_TRUE(strstr(s, "nope") == 0);
    KT_TRUE(strstr(s, "") == s);
}

KTEST(kstring, case_insensitive_compare){
    KT_EQ(kstricmp("Content-Type", "content-type"), 0);
    KT_TRUE(kstricmp("abc", "abd") != 0);
    KT_EQ(kstrnicmp("TEXT/HTML; charset=utf-8", "text/html", 9), 0);
    KT_TRUE(kstrnicmp("text/plain", "text/html", 9) != 0);
}

KTEST(kstring, ksnprintf_formats){
    char b[64];

    ksnprintf(b, sizeof(b), "%s=%d", "n", -42);
    KT_STREQ(b, "n=-42");

    ksnprintf(b, sizeof(b), "%u %x", 4000000000u, 0xDEADu);
    KT_STREQ(b, "4000000000 dead");

    ksnprintf(b, sizeof(b), "%c%%", 'q');
    KT_STREQ(b, "q%");
}

KTEST(kstring, ksnprintf_never_overruns){
    char b[8];
    memset(b, 'Z', sizeof(b));
    ksnprintf(b, 8, "%s", "abcdefghijklmnop");
    KT_EQ(b[7], 0);                 // always NUL-terminates
    KT_MEMEQ(b, "abcdefg", 7);

    // A cap of 1 leaves room for the terminator and nothing else.
    char one[2] = { 'X', 'X' };
    ksnprintf(one, 1, "abc");
    KT_EQ(one[0], 0);
    KT_EQ(one[1], 'X');             // did not touch the byte past the cap
}
