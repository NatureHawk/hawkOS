# hawkOS

A hobby operating system for 32-bit x86, written from scratch. It boots with
GRUB, runs a preemptive multitasking kernel, and comes up in a dark graphical
desktop with a file browser, a task manager, a terminal, and a tabbed web
browser that fetches real pages over its own TCP/IP and TLS stack.

```
make              # build hawkos.iso
make run          # boot in QEMU: 128 MB, NIC attached, serial to stdout
make run-offline  # same machine with no NIC, to exercise that path
make run-log      # serial to serial.log instead of the terminal
```

`make run` attaches the NIC hawkOS has a driver for, on QEMU's user-mode
network — no bridge and no host configuration:

```
-netdev user,id=n0 -device rtl8139,netdev=n0
```

That gives the guest 10.0.2.15, a DHCP server on 10.0.2.2 and DNS on
10.0.2.3. hawkOS runs DHCP itself at boot on a background task, so there is
nothing to configure inside it; the taskbar tray shows the address it got,
and `net`, `dhcp`, `dns <host>` and `ping <host>` in the Terminal inspect and
redo it.

## What is in it

**Boot and memory.** A Multiboot header requests a linear framebuffer from
GRUB; `boot.s` hands control to C after loading a flat GDT. The physical
memory manager reads GRUB's memory map into a frame bitmap, paging
identity-maps RAM, and a first-fit free-list allocator provides `kmalloc`.

**Multitasking.** A round-robin scheduler preempts on the 100 Hz PIT
interrupt. `src/switch.asm` swaps kernel stacks; tasks are kernel threads
with their own 32 KB stack. `task_sleep` blocks without spinning, which is
what lets the network and browser tasks coexist with the compositor.

**Devices.** PS/2 keyboard and mouse (including the IntelliMouse scroll
wheel), PIT, CMOS RTC, ATA PIO disk, a read-only FAT32 driver, and an RTL8139
Ethernet driver found by scanning the PCI bus.

The mouse prefers **absolute** positioning via the VMware pointer protocol
that QEMU emulates on port 0x5658 ([vmmouse.c](src/vmmouse.c)), falling back
to relative PS/2 when no vmport is present. This matters more than it
sounds: with a relative pointer the guest draws its cursor from accumulated
deltas while the host draws its own from the real pointer, and the two drift
apart for good the first time the guest clamps at a screen edge. Absolute
coordinates make them coincide, and QEMU stops needing to grab the pointer.

**Graphics and the desktop.** A double-buffered framebuffer with clipping,
an 8x16 bitmap font, and a compositing window manager: drag, focus, z-order,
minimise, close, rounded corners. The desktop adds a launcher menu, window
buttons, and a status tray with the network address, memory use against the
128 MB budget, and a clock and date. One dark palette in `header/theme.h`
covers every window, app and rendered web page.

**Networking.** Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and a client-side
TCP, all written for this kernel. HTTP/1.1 on top with redirects and chunked
transfer decoding. TLS comes from BearSSL, vendored under `third_party/` and
compiled for the freestanding target.

**Browser.** Tabs, an address bar that searches when what you typed is not a
URL, back, reload, and a single-pass HTML tokeniser and layout engine — no
DOM, no CSS — that emits a flat display list of positioned text runs. It
handles block structure, headings, lists, links, entity decoding, meta
refresh, and skips `<script>` and `<style>` contents. Search results go
through DuckDuckGo's HTML-only endpoint; result links are unwrapped from the
engine's redirector so a click goes straight to the destination.

### The 128 MB budget

The 128 MB limit is a design constraint enforced in software, not a property
of the hardware. Every subsystem is sized to fit inside it regardless of how
much RAM the host has: a 4 MB framebuffer back buffer, a 16 MB kernel heap, a
192 KB TCP receive buffer per connection, a 1 MB cap on a downloaded page.
`meminfo` in the terminal, the Task Manager, and the taskbar tray all report
actual usage against it.

## Security

TLS certificates are parsed but **not verified against a trust store**.
hawkOS ships no root CA bundle and has no trustworthy wall clock, so
`src/tls.c` installs an X.509 engine that accepts any well-formed chain.
Traffic is genuinely encrypted, which defeats passive eavesdropping, but not
an active man-in-the-middle. It is not a browser to type a password into.
The reasoning is written out at the top of `src/tls.c`.

## Layout

```
boot.s, linker.ld     multiboot entry and kernel image layout
src/, header/         the kernel
compat/               <string.h> shim so third-party C can build freestanding
third_party/bearssl/  upstream BearSSL, unmodified
tools/                build and test helpers
  build_bearssl.sh    compiles BearSSL for the kernel's target
  mkfont.py           generates src/font8x16.c from a console font
  seed_disk.sh        writes a file tree into disk.img with mtools
  shot.py             boots hawkOS headless, drives it, captures PNGs
  patch.py            applies JSON-described source edits
shots/                screenshots produced by tools/shot.py
```

## Testing without a screen

`tools/shot.py` boots the ISO in QEMU with no display, talks to its QMP
socket to inject keyboard and mouse events, and writes PNG screenshots. A
script file drives it:

```
python3 tools/shot.py --wait 9 --net --script tools/t1.txt
```

Directives are `wait`, `key`, `type`, `mouse`, `click`, `wheel` and `shot`.
Pointer moves are sent as absolute coordinates, which land on the pixel
asked for because the guest is running the vmmouse. If no vmport is present
the harness falls back to relative deltas and re-homes the pointer into the
top-left corner before every move — with a relative device there is no way
to ask the guest where it thinks the cursor is, so the only reliable
reference point is a corner it clamps against.

That is how every screenshot in `shots/` was produced, and how the GUI is
regression-tested.

## Credits

- [BearSSL](https://bearssl.org) by Thomas Pornin, MIT licensed — TLS.
- The 8x16 font is extracted from the `Lat15-VGA16` console font in Debian's
  `console-setup`, which carries the long-freely-used IBM VGA glyph shapes.
- [OSDev Wiki](https://wiki.osdev.org) for hardware documentation.
