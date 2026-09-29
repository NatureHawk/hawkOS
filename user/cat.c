/* user/cat.c — copy files named on the command line to standard output;
 * with no arguments, copy standard input (so it works at the end of a pipe) */
#include "hawk.h"

static char buf[512];

static int copy(int fd){
    int n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0){
        /* Console writes are capped per call; sizeof(buf) is well under it. */
        if (sys_write(1, buf, (unsigned)n) < 0) return 1;
    }
    return n < 0;
}

void _start(int argc, char** argv){
    int status = 0;
    if (argc < 2) sys_exit(copy(0));
    for (int i = 1; i < argc; i++){
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0){
            hawk_puts("cat: cannot open ");
            hawk_puts(argv[i]);
            hawk_puts("\n");
            status = 1;
            continue;
        }
        if (copy(fd)){
            hawk_puts("cat: read error on ");
            hawk_puts(argv[i]);
            hawk_puts("\n");
            status = 1;
        }
        sys_close(fd);
    }
    sys_exit(status);
}
