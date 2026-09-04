/* Link script for user programs.
 *
 * The output is a flat binary, not an ELF: proc_spawn copies the file straight
 * to PROC_BASE and jumps to its first byte, so there is no header to parse and
 * nothing to relocate. That puts two requirements on this script -- the image
 * has to be linked at exactly the address it will be loaded at, and _start has
 * to be the first thing in it.
 *
 * PROC_BASE (header/proc.h) is 0x40000000. Keep the two in step. */

ENTRY(_start)

SECTIONS
{
  . = 0x40000000;

  .text : {
    *(.text.entry)      /* _start, first, so the entry is at offset 0 */
    *(.text*)
  }

  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(COMMON) *(.bss*) }

  /* Nothing below is loaded, and leaving it in would inflate the flat binary
     with metadata the loader would faithfully copy into the process image. */
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
