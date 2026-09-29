/* user/hawk.h — the user-side view of the hawkOS system call interface
 *
 * Header-only and freestanding: no libc, nothing linked in. Numbers mirror
 * header/syscall.h in the kernel; keep the two in step. Calls go through
 * int 0x80 with the number in eax and arguments in ebx, ecx, edx. Failures
 * come back as small negative errno values (the Linux numbers). */
#ifndef HAWK_H
#define HAWK_H

#define SYS_EXIT     1
#define SYS_WRITE    2
#define SYS_GETPID   3
#define SYS_YIELD    4
#define SYS_SLEEP    5
#define SYS_TICKS    6
#define SYS_FSIZE    7
#define SYS_READFILE 8
#define SYS_OPEN     9
#define SYS_CLOSE    10
#define SYS_READ     11
#define SYS_LSEEK    12
#define SYS_READDIR  13
#define SYS_SPAWN    14
#define SYS_WAITPID  15
#define SYS_BRK      16
#define SYS_SBRK     17
#define SYS_GETTIME  18
#define SYS_GETCWD   19
#define SYS_DUP      20
#define SYS_DUP2     21
#define SYS_PIPE     22
#define SYS_FSTAT    23
#define SYS_STAT     24
#define SYS_MKDIR    25
#define SYS_UNLINK   26
#define SYS_RMDIR    27
#define SYS_RENAME   28
#define SYS_CHDIR    29
#define SYS_KILL     30
#define SYS_MMAP     31
#define SYS_MUNMAP   32

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define SPAWN_INHERIT 1     /* the child shares the caller's open descriptors */

#define SIGINT  2
#define SIGKILL 9
#define SIGUSR1 10
#define SIGTERM 15

#define E_INTR   4          /* errors are returned negated: -E_INTR */
#define E_FAULT  14

#define STAT_FILE 1
#define STAT_DIR  2
#define STAT_CHR  3
#define STAT_PIPE 4

typedef struct {
    char     name[128];
    unsigned size;
    unsigned is_dir;
} hawk_dirent_t;

typedef struct {
    unsigned type;          /* STAT_* */
    unsigned size;
    unsigned ino;
    unsigned dev;
} hawk_stat_t;

static inline int hawk_sys3(int nr, int a, int b, int c){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

static inline int sys_write(int fd, const void* buf, unsigned n){
    return hawk_sys3(SYS_WRITE, fd, (int)buf, (int)n);
}
static inline int  sys_read(int fd, void* buf, unsigned n){ return hawk_sys3(SYS_READ, fd, (int)buf, (int)n); }
static inline int  sys_open(const char* p, int flags){ return hawk_sys3(SYS_OPEN, (int)p, flags, 0); }
static inline int  sys_close(int fd){ return hawk_sys3(SYS_CLOSE, fd, 0, 0); }
static inline int  sys_lseek(int fd, int off, int whence){ return hawk_sys3(SYS_LSEEK, fd, off, whence); }
static inline int  sys_readdir(const char* p, unsigned i, hawk_dirent_t* d){
    return hawk_sys3(SYS_READDIR, (int)p, (int)i, (int)d);
}
static inline int  sys_dup(int fd){ return hawk_sys3(SYS_DUP, fd, 0, 0); }
static inline int  sys_dup2(int oldfd, int newfd){ return hawk_sys3(SYS_DUP2, oldfd, newfd, 0); }
static inline int  sys_pipe(int fds[2]){ return hawk_sys3(SYS_PIPE, (int)fds, 0, 0); }
static inline int  sys_fstat(int fd, hawk_stat_t* st){ return hawk_sys3(SYS_FSTAT, fd, (int)st, 0); }
static inline int  sys_stat(const char* p, hawk_stat_t* st){ return hawk_sys3(SYS_STAT, (int)p, (int)st, 0); }
static inline int  sys_mkdir(const char* p){ return hawk_sys3(SYS_MKDIR, (int)p, 0, 0); }
static inline int  sys_unlink(const char* p){ return hawk_sys3(SYS_UNLINK, (int)p, 0, 0); }
static inline int  sys_rmdir(const char* p){ return hawk_sys3(SYS_RMDIR, (int)p, 0, 0); }
static inline int  sys_rename(const char* from, const char* to){ return hawk_sys3(SYS_RENAME, (int)from, (int)to, 0); }
static inline int  sys_chdir(const char* p){ return hawk_sys3(SYS_CHDIR, (int)p, 0, 0); }
static inline int  sys_fsize(const char* p){ return hawk_sys3(SYS_FSIZE, (int)p, 0, 0); }
static inline int  sys_readfile(const char* p, void* buf, unsigned cap){
    return hawk_sys3(SYS_READFILE, (int)p, (int)buf, (int)cap);
}
static inline int  sys_spawn3(const char* p, const char* args, int flags){
    return hawk_sys3(SYS_SPAWN, (int)p, (int)args, flags);
}
static inline int  sys_spawn(const char* p, const char* args){ return sys_spawn3(p, args, 0); }
static inline int  sys_waitpid(int pid, int* status){ return hawk_sys3(SYS_WAITPID, pid, (int)status, 0); }
static inline int  sys_kill(int pid, int sig){ return hawk_sys3(SYS_KILL, pid, sig, 0); }
static inline void* sys_sbrk(int incr){ return (void*)hawk_sys3(SYS_SBRK, incr, 0, 0); }
static inline int  sys_brk(void* addr){ return hawk_sys3(SYS_BRK, (int)addr, 0, 0); }
/* mmap returns the address, or a small negative errno (addresses are below 2 GB). */
static inline void* sys_mmap(unsigned len, int prot){ return (void*)hawk_sys3(SYS_MMAP, (int)len, prot, 0); }
static inline int  sys_munmap(void* addr, unsigned len){ return hawk_sys3(SYS_MUNMAP, (int)addr, (int)len, 0); }
static inline unsigned sys_gettime(void){ return (unsigned)hawk_sys3(SYS_GETTIME, 0, 0, 0); }
static inline int  sys_getcwd(char* buf, unsigned n){ return hawk_sys3(SYS_GETCWD, (int)buf, (int)n, 0); }
static inline int  sys_getpid(void){ return hawk_sys3(SYS_GETPID, 0, 0, 0); }
static inline unsigned sys_ticks(void){ return (unsigned)hawk_sys3(SYS_TICKS, 0, 0, 0); }
static inline int  sys_sleep(unsigned ms){ return hawk_sys3(SYS_SLEEP, (int)ms, 0, 0); }
static inline void sys_yield(void){ hawk_sys3(SYS_YIELD, 0, 0, 0); }
static inline void sys_exit(int code){
    hawk_sys3(SYS_EXIT, code, 0, 0);
    for (;;){ }
}

/* A little string and output support, since there is no libc to lean on. */
static inline unsigned hawk_strlen(const char* s){
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static inline int hawk_strcmp(const char* a, const char* b){
    while (*a && *a == *b){ a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int hawk_memeq(const void* a, const void* b, unsigned n){
    const char* x = (const char*)a;
    const char* y = (const char*)b;
    for (unsigned i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static inline void hawk_puts(const char* s){ sys_write(1, s, hawk_strlen(s)); }

static inline void hawk_putnum(unsigned v){
    char buf[12];
    int i = 11;
    buf[i--] = 0;
    if (!v) buf[i--] = '0';
    while (v && i >= 0){ buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    hawk_puts(buf + i + 1);
}

#endif
