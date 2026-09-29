/* user/hi.c — the smallest ELF program: hello from a real executable
 *
 * Exercises what the ELF loader has to get right beyond "the code runs": an
 * initialised .data value, a .bss array that must arrive zeroed, and the
 * argc/argv frame at the top of the stack. Exits 0 only if all of that held,
 * so a parent that waits for it learns whether the load was faithful. */
#include "hawk.h"

static int  magic = 0x4841574B;      /* .data */
static char scratch[600];            /* .bss */

void _start(int argc, char** argv){
    int bad = 0;
    if (magic != 0x4841574B) bad++;
    for (unsigned i = 0; i < sizeof(scratch); i++) if (scratch[i]) bad++;

    hawk_puts("hello from an ELF program, pid ");
    hawk_putnum((unsigned)sys_getpid());
    hawk_puts("\n");
    for (int i = 0; i < argc; i++){
        hawk_puts("  argv[");
        hawk_putnum((unsigned)i);
        hawk_puts("] = ");
        hawk_puts(argv[i]);
        hawk_puts("\n");
    }
    sys_exit(bad);
}
