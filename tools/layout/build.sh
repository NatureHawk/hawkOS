#!/bin/sh
# Builds the host-side layout harness. Run from the repository root.
#
# The kernel's own string routines are compiled in rather than libc's: html.c
# calls kstrnicmp and strstr side by side, and mixing the two implementations
# is exactly the kind of difference that would make the harness disagree with
# the real thing. -fno-builtin keeps GCC from turning those calls back into
# its own inlined versions.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build
gcc -O1 -g -Wall -Wextra -fno-builtin -I. -Iheader \
    -o build/layout \
    tools/layout/main.c tools/layout/stubs.c \
    src/html.c src/css.c src/kstring.c src/fontprop.c src/theme.c
echo "built build/layout"
