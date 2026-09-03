#!/bin/sh
# Compiles the vendored BearSSL tree for the kernel's freestanding i386
# target and archives it into build/libbearssl.a.
#
# BearSSL builds as "compile everything under src/"; the one exclusion is
# sysrng.c, which reaches for /dev/urandom or the Windows crypto API. The
# kernel seeds the DRBG itself instead (see src/tls.c).
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SRC=third_party/bearssl/src
OUT=build/bearssl
mkdir -p "$OUT"

# BearSSL picks AES-NI, PCLMUL and SSE2 implementations at runtime after a
# CPUID check. Those instructions fault with #UD unless the kernel turns SSE
# on in CR0/CR4 and saves XMM state across context switches, and neither is
# worth doing for a browser that is not CPU-bound. Forcing the portable
# constant-time implementations keeps every code path to plain i386.
FLAGS="-DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_RDRAND=0 -DBR_POWER8=0 -m32 -ffreestanding -fno-stack-protector -fno-pic -O2 -std=gnu99 \
  -Icompat -Ithird_party/bearssl/inc -I$SRC"

ok=0
fail=0
for f in $(find "$SRC" -name '*.c' ! -name 'sysrng.c' | sort); do
    o="$OUT/$(echo "$f" | tr '/' '_').o"
    if [ "$f" -ot "$o" ] && [ -f "$o" ]; then
        ok=$((ok + 1))
        continue
    fi
    if gcc $FLAGS -c "$f" -o "$o" 2>/tmp/bearssl_err.txt; then
        ok=$((ok + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $f"
        head -4 /tmp/bearssl_err.txt
    fi
done

echo "bearssl: compiled=$ok failed=$fail"
[ "$fail" -eq 0 ] || exit 1

rm -f build/libbearssl.a
ar rcs build/libbearssl.a "$OUT"/*.o
echo "bearssl: archived $(ls -l build/libbearssl.a | awk '{print $5}') bytes"
