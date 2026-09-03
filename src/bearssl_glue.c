// src/bearssl_glue.c — the two host-environment symbols BearSSL expects
//
// Both come from parts of the library the kernel deliberately does not use.
// Providing them here, rather than patching the vendored tree, keeps
// third_party/bearssl a pristine upstream checkout that can be re-cloned or
// updated without carrying local edits.
#include <stdint.h>
#include "bearssl.h"

// sysrng.c is excluded from the build: it reaches for /dev/urandom or the
// Windows crypto API, neither of which exists here. Reporting "no system
// seeder" is the documented way to say so; src/tls.c injects entropy
// explicitly instead.
br_prng_seeder br_prng_seeder_system(const char** name){
    if (name) *name = "none";
    return 0;
}

// Referenced by x509_minimal for certificate validity dates. hawkOS uses a
// custom X.509 engine that does no date checking (see the security note in
// src/tls.c), so this is never called on any live path — it only has to
// exist for the link to succeed. The prototype matches 32-bit glibc's, which
// is what x509_minimal.c was compiled against.
long time(long* t){
    if (t) *t = 0;
    return 0;
}
