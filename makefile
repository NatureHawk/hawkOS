AS = nasm
CC = gcc
LD = ld

SRCDIR = src
INCDIR = header
OUTBIN = hawkos.bin
OUTISO = hawkos.iso
DISKIMG = disk.img

# Naming the format explicitly matters now that the guest writes: with the
# format left to probing, QEMU refuses writes to block 0 as a safety measure
# and prints a warning about it on every run.
DISK = -drive file=$(DISKIMG),format=raw,if=ide,index=0,media=disk

ASFLAGS = -f elf32
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic -O2 -Wall -Wextra -std=gnu11 -I. -I$(INCDIR) -Icompat -Ithird_party/bearssl/inc
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib -z max-page-size=0x1000

# GCC lowers 64-bit division and modulo on i386 into calls to __udivdi3 and
# friends, which live in libgcc. -nostdlib drops it, so link it back in
# explicitly rather than hand-writing those helpers.
LIBGCC = $(shell $(CC) -m32 -print-libgcc-file-name)

# BearSSL is vendored under third_party/ and built by its own script into a
# static archive, because compiling ~290 upstream files through this
# makefile's pattern rules would bury the kernel's own build output.
BEARSSL = build/libbearssl.a

# add files here as you grow (e.g., $(SRCDIR)/pic.o $(SRCDIR)/idt_stubs.o $(SRCDIR)/idt_load.o)
# objects to link into the kernel (order matters a bit: boot first)
OBJS = \
  $(SRCDIR)/boot.o \
  $(SRCDIR)/kernel.o \
  $(SRCDIR)/gdt.o \
  $(SRCDIR)/gdt_asm.o \
  $(SRCDIR)/idt.o \
  $(SRCDIR)/pic.o \
  $(SRCDIR)/pit.o \
  $(SRCDIR)/pmm.o\
  $(SRCDIR)/paging.o\
  $(SRCDIR)/kheap.o\
  $(SRCDIR)/idt_asm.o\
  $(SRCDIR)/isr.o\
  $(SRCDIR)/irq0.o\
  $(SRCDIR)/kbd.o\
  $(SRCDIR)/console.o\
  $(SRCDIR)/exceptions.o\
  $(SRCDIR)/isr_exn.o\
  $(SRCDIR)/serial.o\
  $(SRCDIR)/kprintf.o\
  $(SRCDIR)/tty.o\
  $(SRCDIR)/shell.o\
  $(SRCDIR)/cpuid.o\
  $(SRCDIR)/rtc.o\
  $(SRCDIR)/gfx.o\
  $(SRCDIR)/font8x8.o\
  $(SRCDIR)/mouse.o\
  $(SRCDIR)/theme.o\
  $(SRCDIR)/desktop.o\
  $(SRCDIR)/ata.o\
  $(SRCDIR)/fat32.o\
  $(SRCDIR)/task.o\
  $(SRCDIR)/switch.o\
  $(SRCDIR)/font8x16.o\
  $(SRCDIR)/fontprop.o\
  $(SRCDIR)/kstring.o\
  $(SRCDIR)/wm.o\
  $(SRCDIR)/app_taskman.o\
  $(SRCDIR)/app_files.o\
  $(SRCDIR)/app_term.o\
  $(SRCDIR)/app_about.o\
  $(SRCDIR)/app_browser.o\
  $(SRCDIR)/pci.o\
  $(SRCDIR)/rtl8139.o\
  $(SRCDIR)/netcfg.o\
  $(SRCDIR)/tcp.o\
  $(SRCDIR)/net.o\
  $(SRCDIR)/tls.o\
  $(SRCDIR)/bearssl_glue.o\
  $(SRCDIR)/http.o\
  $(SRCDIR)/html.o\
  $(SRCDIR)/css.o\
  $(SRCDIR)/test_css.o\
  $(SRCDIR)/vmmouse.o\
  $(SRCDIR)/ktest.o\
  $(SRCDIR)/test_kstring.o\
  $(SRCDIR)/test_mem.o\
  $(SRCDIR)/test_url.o\
  $(SRCDIR)/test_html.o\
  $(SRCDIR)/syscall_asm.o\
  $(SRCDIR)/syscall.o\
  $(SRCDIR)/proc.o\
  $(SRCDIR)/test_fat32.o\
  $(SRCDIR)/test_proc.o

all: $(OUTISO) user
iso: $(OUTISO)

# ---------------------------------------------------------------- userland
#
# User programs are flat binaries linked at PROC_BASE, built with the same
# compiler but nothing else in common with the kernel: no kernel headers, no
# libc, and -nostdlib so nothing is silently linked in. They reach the running
# system only through int 0x80.
USERDIR  = user
USERBINS = $(USERDIR)/hello.bin
USERCFLAGS = -m32 -ffreestanding -fno-pic -fno-stack-protector -O2 -Wall -Wextra -std=gnu11

$(USERDIR)/%.bin: $(USERDIR)/%.c $(USERDIR)/user.ld
	$(CC) $(USERCFLAGS) -c $< -o $(USERDIR)/$*.o
	$(LD) -m elf_i386 -T $(USERDIR)/user.ld -nostdlib --oformat binary -o $@ $(USERDIR)/$*.o

user: $(USERBINS)

# Copies the built user programs onto the FAT32 image so proc_spawn can find
# them. mcopy comes from mtools and writes into the image without needing root
# or a loop mount.
disk-sync: $(USERBINS) $(DISKIMG)
	@for b in $(USERBINS); do \
	  n=$$(basename $$b .bin | tr 'a-z' 'A-Z'); \
	  mcopy -i $(DISKIMG) -o $$b ::$$n.BIN && echo "copied $$b -> $$n.BIN"; \
	done

# kernel ELF/bin
$(BEARSSL):
	sh tools/build_bearssl.sh

$(OUTBIN): $(OBJS) $(BEARSSL) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BEARSSL) $(LIBGCC)

# bootable ISO with GRUB
$(OUTISO): $(OUTBIN) grub.cfg
	mkdir -p iso/boot/grub
	cp $(OUTBIN) iso/boot/hawkos.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(OUTISO) iso

# A blank 64MB FAT32 disk image for the ATA/FAT32 driver to read. Only
# created if it doesn't already exist, so rebuilding never wipes files
# you've copied onto it (mkfs.vfat comes from the dosfstools package).
$(DISKIMG):
	dd if=/dev/zero of=$(DISKIMG) bs=1M count=64
	mkfs.vfat -F 32 $(DISKIMG)

# The NIC hawkOS has a driver for, on QEMU's user-mode network. That gives
# the guest 10.0.2.15 with a DHCP server on 10.0.2.2 and DNS on 10.0.2.3,
# routed out through the host — no bridge, no root, no host configuration.
# It is part of the default run target because the browser is useless
# without it and "why is it offline" is not a puzzle worth setting.
NETDEV = -netdev user,id=n0 -device rtl8139,netdev=n0

# -boot d: force booting from the CD-ROM. Without it, QEMU's default BIOS
# boot order tries the hard disk (disk.img) first — which holds a plain FAT32
# filesystem, not a bootable one — and never reaches the actual ISO.
run: $(OUTISO) $(DISKIMG)
	qemu-system-i386 -m 128M -cdrom $(OUTISO) $(DISK) -boot d $(NETDEV) -serial stdio

# Same machine with the NIC left out, for testing that the system comes up
# and stays usable when there is no network at all.
run-offline: $(OUTISO) $(DISKIMG)
	qemu-system-i386 -m 128M -cdrom $(OUTISO) $(DISK) -boot d -serial stdio

# Boots the kernel straight from QEMU's multiboot loader with "selftest" on
# the command line, runs the in-kernel test suite, and exits. There is no
# framebuffer on this path and that is deliberate: the tests are headless and
# the run has to be something CI can fail on, not a window to watch.
#
# isa-debug-exit turns the guest's port write into a process exit code of
# (value << 1) | 1, so 0x10 becomes 33 for a clean run and 0x11 becomes 35.
test: $(OUTBIN) $(DISKIMG) disk-sync
	@timeout 180 qemu-system-i386 -m 128M -kernel $(OUTBIN) -append selftest \
	  $(DISK) $(NETDEV) -display none -serial stdio \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04; \
	code=$$?; \
	if [ $$code -eq 33 ]; then echo "--- self-test PASSED"; exit 0; \
	elif [ $$code -eq 35 ]; then echo "--- self-test FAILED"; exit 1; \
	elif [ $$code -eq 124 ]; then echo "--- self-test TIMED OUT"; exit 1; \
	else echo "--- self-test did not report (qemu exit $$code)"; exit 1; fi

run-bin: $(OUTBIN) $(DISKIMG)
	qemu-system-i386 -m 128M -kernel $(OUTBIN) $(DISK) $(NETDEV) -serial stdio

# -serial stdio doesn't reliably reach the terminal under some WSL/terminal
# setups. This logs the same kprintf output to a file instead — open
# serial.log in any editor after quitting QEMU.
run-log: $(OUTISO) $(DISKIMG)
	qemu-system-i386 -m 128M -cdrom $(OUTISO) $(DISK) -boot d $(NETDEV) -serial file:serial.log

# Every object depends on every header.
#
# Coarse on purpose: a proper dependency scan needs -MMD and a generated
# include file, and this project has one directory of headers that changes
# rarely. Getting it wrong is expensive in a way that a few seconds of
# recompiling is not -- editing the shared palette in header/theme.h and
# rebuilding used to leave every app linked against the previous colours,
# which looks exactly like a bug in the code that was just changed.
HEADERS = $(wildcard $(INCDIR)/*.h) $(wildcard *.h)

# Build rules
# NASM (ASM → OBJ)
$(SRCDIR)/%.o: $(SRCDIR)/%.asm
	$(AS) $(ASFLAGS) $< -o $@

$(SRCDIR)/boot.o: $(SRCDIR)/boot.s
	$(AS) $(ASFLAGS) $< -o $@

# Force idt.o to come from idt.c (exports idt_init, set_gate)
$(SRCDIR)/idt.o: $(SRCDIR)/idt.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Force idt_asm.o to come from idt.asm (exports idt_load, maybe isr_stub)
$(SRCDIR)/idt_asm.o: $(SRCDIR)/idt.asm
	$(AS) $(ASFLAGS) $< -o $@

# gdt.o is the C table builder; gdt_asm.o is the lgdt/ltr half that has to be
# assembly. Both come from files named gdt, so neither can be left to the
# pattern rules.
$(SRCDIR)/gdt.o: $(SRCDIR)/gdt.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(SRCDIR)/gdt_asm.o: $(SRCDIR)/gdt.asm
	$(AS) $(ASFLAGS) $< -o $@

$(SRCDIR)/syscall_asm.o: $(SRCDIR)/syscall.asm
	$(AS) $(ASFLAGS) $< -o $@

$(SRCDIR)/syscall.o: $(SRCDIR)/syscall.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Force isr.o to come from isr.asm (exports irq0_stub/irq1_stub) — do not let
# this fall back to a stray isr.c, which would collide on irq0_handler_c
$(SRCDIR)/isr.o: $(SRCDIR)/isr.asm
	$(AS) $(ASFLAGS) $< -o $@

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRCDIR)/*.o $(OUTBIN) $(OUTISO) serial.log
	rm -f $(USERDIR)/*.o $(USERDIR)/*.bin
	rm -rf iso

.PHONY: all iso run run-offline run-bin run-log test clean bearssl user disk-sync

bearssl:
	sh tools/build_bearssl.sh
