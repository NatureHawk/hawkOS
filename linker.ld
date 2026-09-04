ENTRY(_start)

SECTIONS
{
  . = 1M;
  kernel_start = .;

  .multiboot ALIGN(4K): {
    *(.multiboot)
  }

  .text ALIGN(4K): {
    *(.text*)
  }

  .rodata ALIGN(4K): {
    *(.rodata*)
  }

  /* Test registry. Every KTEST() drops one descriptor here, so tests can sit
     beside the code they exercise instead of in a central list that goes
     stale the first time someone forgets to add to it. KEEP stops --gc-sections
     from discarding entries nothing appears to reference. */
  .ktests ALIGN(4K): {
    __ktests_start = .;
    KEEP(*(.ktests))
    __ktests_end = .;
  }

  .data ALIGN(4K): {
    *(.data*)
  }

  .bss ALIGN(4K): {
    *(COMMON)
    *(.bss*)
  }

  . = ALIGN(4K);
  kernel_end = .;
}
