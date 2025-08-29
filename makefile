AS=nasm
CC=gcc
LD=ld

ASFLAGS=-f elf32
CFLAGS=-m32 -ffreestanding -fno-stack-protector -fno-pic -O2 -Wall -Wextra -std=gnu11
LDFLAGS=-m elf_i386 -T linker.ld -nostdlib -z max-page-size=0x1000

OBJS=boot.o kernel.o

all: hawkos.iso

hawkos.bin: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

hawkos.iso: hawkos.bin grub.cfg
	mkdir -p iso/boot/grub
	cp hawkos.bin iso/boot/hawkos.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o hawkos.iso iso

run: hawkos.iso
	qemu-system-i386 -cdrom hawkos.iso -serial stdio

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf *.o hawkos.bin hawkos.iso iso

.PHONY: all run clean
