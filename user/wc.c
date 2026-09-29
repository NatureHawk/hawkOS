/* user/wc.c — count lines, words and bytes of standard input (or of files)
 *
 * Prints "LINES WORDS BYTES\n" to standard output. Meant to sit at the end of
 * a pipe: it closes every descriptor above 2 first, because a spawn with
 * SPAWN_INHERIT hands the child all of its parent's, including the write end
 * of the pipe it is about to read -- and it would wait forever for an EOF that
 * its own copy of that end prevents. */
#include "hawk.h"

static char buf[512];
static unsigned lines, words, bytes;
static int in_word;

static int count(int fd){
    int n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0){
        for (int i = 0; i < n; i++){
            char c = buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n' || c == '\t'){ in_word = 0; }
            else if (!in_word){ in_word = 1; words++; }
        }
    }
    return n < 0;
}

void _start(int argc, char** argv){
    for (int fd = 3; fd < 16; fd++) sys_close(fd);

    int status = 0;
    if (argc < 2) status = count(0);
    for (int i = 1; i < argc; i++){
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0){
            hawk_puts("wc: cannot open ");
            hawk_puts(argv[i]);
            hawk_puts("\n");
            status = 1;
            continue;
        }
        status |= count(fd);
        sys_close(fd);
    }
    hawk_putnum(lines); hawk_puts(" ");
    hawk_putnum(words); hawk_puts(" ");
    hawk_putnum(bytes); hawk_puts("\n");
    sys_exit(status);
}
