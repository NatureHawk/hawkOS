#!/bin/sh
# Puts a small tree of files onto disk.img so the Files app has something
# real to browse. Uses mtools rather than a loopback mount, so it needs no
# root and no kernel FAT driver on the host.
#
# Names are 8.3: hawkOS's FAT32 driver reads short entries only, and a long
# name would show up as its mangled ~1 form.
set -e

# mtools refuses to touch an image whose geometry it cannot vouch for, and
# then waits on a prompt. This is a plain mkfs.vfat FAT32 volume, so skip the
# check -- and give every invocation an empty stdin so a prompt we did not
# anticipate fails fast instead of hanging the build.
export MTOOLS_SKIP_CHECK=1

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

IMG=disk.img
[ -f "$IMG" ] || { echo "$IMG missing - run make first"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/DOCS/NOTES" "$TMP/SRC"

# mmd blocks indefinitely when the directory already exists rather than
# returning an error, so check first and give it a hard timeout anyway.
mkdir_img() {
    if mdir -i "$IMG" "$1" </dev/null >/dev/null 2>&1; then return 0; fi
    timeout 10 mmd -i "$IMG" "$1" </dev/null >/dev/null 2>&1 || true
}

copy_img() {
    timeout 20 mcopy -i "$IMG" -o "$1" "$2" </dev/null
}

mkdir_img ::/DOCS
mkdir_img ::/DOCS/NOTES
mkdir_img ::/SRC

cat > "$TMP/README.TXT" <<'EOF'
hawkOS
======

A hobby operating system for 32-bit x86, written from scratch.

Boot:      GRUB multiboot, flat GDT, protected mode
Memory:    frame bitmap from the GRUB memory map, paging, kernel heap
Tasks:     preemptive round-robin on the 100 Hz PIT
Devices:   PS/2 keyboard and mouse, PIT, CMOS RTC, ATA PIO, RTL8139
Storage:   read-only FAT32 (this file came off it)
Graphics:  double-buffered linear framebuffer, 8x16 bitmap font
Desktop:   compositing window manager, taskbar, launcher, dark theme
Network:   Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP
Web:       HTTP/1.1, TLS via BearSSL, a single-pass HTML renderer

Everything is sized to fit inside a self-imposed 128 MB budget.
EOF

cat > "$TMP/HELLO.TXT" <<'EOF'
HELLO FROM HAWKOS FAT32
EOF

cat > "$TMP/DOCS/CHANGES.TXT" <<'EOF'
v0.7  Dark theme across the whole system. Taskbar launcher, clock and
      status tray. Browser gains tabs, search and a reload button.
      Scroll wheel support.

v0.6  Preemptive scheduler. Compositing window manager and desktop.
      Files, Terminal, Task Manager, About. PCI and RTL8139 driver,
      TCP/IP stack, DHCP, DNS, HTTP, TLS, and the browser.

v0.5  VGA driver and improved keyboard handling.

v0.4  Graphics mode, bitmap font, PS/2 mouse, ATA and FAT32 read support.

v0.3  Shell diagnostics: meminfo, cpuinfo, date, crash.

v0.2  Physical memory manager, paging, kernel heap.

v0.1  Multiboot kernel, GDT, IDT, PIC, PIT, keyboard, text console.
EOF

cat > "$TMP/DOCS/NOTES/TODO.TXT" <<'EOF'
Not done yet
------------

Ring 3 and a syscall interface. Everything currently runs in ring 0 as
kernel threads; user mode needs a TSS, per-process page directories and
an ELF loader.

FAT32 writing. The driver reads only, so nothing in the system can
modify this disk. Writing means cluster allocation plus FAT and FSInfo
updates, and it is worth doing carefully rather than quickly.

Certificate verification. TLS works but trusts any well-formed chain -
see the note at the top of src/tls.c. Fixing it means shipping a root
CA bundle and having a clock worth checking dates against.

Images. The renderer is text only; the img tag is ignored. PNG and JPEG
decoders are a lot of code for the amount of screen they would fill.

TCP performance. One segment in flight at a time, so a large page takes
many round trips. A real congestion window would help.
EOF

cat > "$TMP/SRC/BUILD.TXT" <<'EOF'
Building hawkOS
===============

  make              build hawkos.iso
  make run          boot it in QEMU with 128 MB of RAM
  make bearssl      rebuild the vendored TLS library
  make clean        remove objects and the ISO

With networking, add:

  -netdev user,id=n0 -device rtl8139,netdev=n0

Headless testing:

  python3 tools/shot.py --wait 9 --net --script tools/t1.txt

That boots with no display, drives the GUI through QEMU's QMP socket,
and writes PNG screenshots.
EOF

copy_img "$TMP/README.TXT"           ::/
copy_img "$TMP/HELLO.TXT"            ::/
copy_img "$TMP/DOCS/CHANGES.TXT"     ::/DOCS/
copy_img "$TMP/DOCS/NOTES/TODO.TXT"  ::/DOCS/NOTES/
copy_img "$TMP/SRC/BUILD.TXT"        ::/SRC/

echo "seeded $IMG:"
mdir -i "$IMG" ::/ </dev/null | sed -n '4,12p'
