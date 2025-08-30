AS = nasm
CC = gcc
LD = ld

SRCDIR = src
INCDIR = header
OUTBIN = hawkos.bin
OUTISO = hawkos.iso

ASFLAGS = -f elf32
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic -O2 -Wall -Wextra -std=gnu11 -I$(INCDIR) -Iheader
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib -z max-page-size=0x1000

# add files here as you grow (e.g., $(SRCDIR)/pic.o $(SRCDIR)/idt_stubs.o $(SRCDIR)/idt_load.o)
# objects to link into the kernel (order matters a bit: boot first)
OBJS = \
  $(SRCDIR)/boot.o \
  $(SRCDIR)/kernel.o \
  $(SRCDIR)/gdt.o \
  $(SRCDIR)/idt.o \
  $(SRCDIR)/pic.o \
  $(SRCDIR)/pit.o \
  $(SRCDIR)/isr.o \
  $(SRCDIR)/idt_asm.o\
  $(SRCDIR)/irq0.o

all: $(OUTISO)

# kernel ELF/bin
$(OUTBIN): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# bootable ISO with GRUB
$(OUTISO): $(OUTBIN) grub.cfg
	mkdir -p iso/boot/grub
	cp $(OUTBIN) iso/boot/hawkos.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(OUTISO) iso

# Run helpers
run: $(OUTISO)
	qemu-system-i386 -cdrom $(OUTISO) -serial stdio

run-bin: $(OUTBIN)
	qemu-system-i386 -kernel $(OUTBIN) -serial stdio

# Build rules
$(SRCDIR)/boot.o: $(SRCDIR)/boot.s
	$(AS) $(ASFLAGS) $< -o $@

# NASM .asm (gdt, idt -> idt_asm.o, isr)
$(SRCDIR)/gdt.o: $(SRCDIR)/gdt.asm
	$(AS) $(ASFLAGS) $< -o $@

$(SRCDIR)/idt_asm.o: $(SRCDIR)/idt.asm
	$(AS) $(ASFLAGS) $< -o $@

$(SRCDIR)/isr.o: $(SRCDIR)/isr.asm
	$(AS) $(ASFLAGS) $< -o $@
	
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRCDIR)/*.o $(OUTBIN) $(OUTISO)
	rm -rf iso

.PHONY: all run run-bin clean
CFLAGS += -I.
