/* user/ls.c — list a directory (the current one by default), long names included */
#include "hawk.h"

void _start(int argc, char** argv){
    const char* dir = argc > 1 ? argv[1] : ".";
    hawk_dirent_t d;
    unsigned n = 0;
    int rc;
    while ((rc = sys_readdir(dir, n, &d)) == 1){
        hawk_puts(d.is_dir ? "d " : "- ");
        hawk_puts(d.name);
        hawk_puts("  ");
        hawk_putnum(d.size);
        hawk_puts("\n");
        n++;
    }
    if (rc < 0){
        hawk_puts("ls: cannot read ");
        hawk_puts(dir);
        hawk_puts("\n");
        sys_exit(1);
    }
    sys_exit(0);
}
