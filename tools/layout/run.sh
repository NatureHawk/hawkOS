#!/bin/sh
# Lays out every committed fixture and every page in the corpus, and fails if
# any of them violates a rendering invariant. Run from anywhere.
#
#   sh tools/layout/run.sh            check
#   sh tools/layout/run.sh -v         check, printing each violation
#
# The corpus (tools/pages) is not committed -- run tools/pages/fetch.sh to
# build it. Without it this still checks the fixtures, which are the ones
# that pin specific bugs.
set -e
cd "$(dirname "$0")/../.."
sh tools/layout/build.sh > /dev/null

QUIET=--quiet
[ "$1" = "-v" ] && QUIET=

fail=0
run() { ./build/layout "$@" --width 880 $QUIET || fail=1; }

for f in tools/layout/fixtures/*.html; do
    [ -e "$f" ] || continue
    case "$f" in
        # Markup that must not reach the page. Geometry checks cannot see
        # this failure: leaked attribute text lays out perfectly well.
        *attr-gt.html) run "$f" --absent SPILL ;;
        *)             run "$f" ;;
    esac
done

for f in tools/pages/*.html; do
    [ -e "$f" ] || continue
    run "$f"
done

if [ "$fail" = 0 ]; then echo "layout: all pages clean"; else echo "layout: FAILURES"; fi
exit $fail
