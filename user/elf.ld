/* Link script for ELF user programs.
 *
 * Unlike user.ld this produces a real ELF32 executable: proc_spawn reads the
 * program headers, maps each PT_LOAD segment at its p_vaddr and jumps to
 * e_entry. One RWX segment holds the headers, code, data and bss, linked at
 * PROC_BASE (header/proc.h, 0x40000000) -- the loader rejects anything
 * outside [PROC_BASE, PROC_BASE + PROC_IMAGE_MAX). bss is memsz beyond
 * filesz, which the loader zero-fills. */

ENTRY(_start)

PHDRS
{
  image PT_LOAD FILEHDR PHDRS FLAGS(7);
}

SECTIONS
{
  . = 0x40000000 + SIZEOF_HEADERS;

  .text   : { *(.text.entry) *(.text*) } :image
  .rodata : { *(.rodata*) } :image
  .data   : { *(.data*) } :image
  .bss    : { *(COMMON) *(.bss*) } :image

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
