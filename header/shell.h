#ifndef SHELL_H
#define SHELL_H
#include <stdint.h>

// Blocking read-eval-print loop on the raw text console. Only used when the
// desktop is unavailable (no framebuffer) or when booting with the GUI off.
void shell_run(void);

// Run one command line and print its output through kprintf. Split out of
// shell_run() so the GUI terminal can reuse every command by installing a
// kprintf sink around this call instead of reimplementing them.
void shell_exec_line(const char* line);

void shell_init(void);          // mounts the filesystem; safe to call twice
void shell_print_help(void);

int      shell_fs_ready(void);
uint32_t shell_cwd_cluster(void);

#endif
