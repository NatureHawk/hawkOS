#pragma once
#include <stdint.h>
#include "header/vfs.h"

// The system call interface.
//
// int 0x80 with the call number in eax and arguments in ebx, ecx, edx. The
// result comes back in eax. This is the Linux i386 convention, chosen because
// it is the one every reference and every reader already knows -- there is
// nothing to gain from inventing a different register order.
//
// The gate at 0x80 is the only IDT entry with DPL 3. Every other vector is
// ring 0, so a user program that tries to invoke one gets a general
// protection fault rather than a kernel entry point.

#define SYS_EXIT     1   // exit(code)
#define SYS_WRITE    2   // write(fd, buf, len) -> bytes or -errno
#define SYS_GETPID   3
#define SYS_YIELD    4
#define SYS_SLEEP    5   // sleep(ms) -> 0, or VFS_EINTR if a signal cut it short
#define SYS_TICKS    6
#define SYS_FSIZE    7   // fsize(path) -> size or -errno (legacy by-name call)
#define SYS_READFILE 8   // readfile(path, buf, cap) -> bytes or -errno (legacy by-name call)
#define SYS_OPEN     9   // open(path, flags) -> fd or -errno
#define SYS_CLOSE    10  // close(fd)
#define SYS_READ     11  // read(fd, buf, len) -> bytes, 0 at EOF, -errno
#define SYS_LSEEK    12  // lseek(fd, off, whence 0/1/2) -> new position
#define SYS_READDIR  13  // readdir(path, index, proc_dirent_t*) -> 1/0/-errno
#define SYS_SPAWN    14  // spawn(path, args or 0, flags) -> pid or -1; flags: PROC_SPAWN_INHERIT
#define SYS_WAITPID  15  // waitpid(pid, int* status or 0) -> pid, -1, or VFS_EINTR
#define SYS_BRK      16  // brk(addr, 0 queries) -> new break or -1
#define SYS_SBRK     17  // sbrk(incr) -> previous break or -1
#define SYS_GETTIME  18  // gettime() -> seconds since 1970
#define SYS_GETCWD   19  // getcwd(buf, size) -> length including NUL, or -errno
#define SYS_DUP      20  // dup(fd) -> new fd
#define SYS_DUP2     21  // dup2(oldfd, newfd) -> newfd
#define SYS_PIPE     22  // pipe(int fds[2]) -> 0; fds[0] read end, fds[1] write end
#define SYS_FSTAT    23  // fstat(fd, vfs_stat_t*) -> 0
#define SYS_STAT     24  // stat(path, vfs_stat_t*) -> 0
#define SYS_MKDIR    25  // mkdir(path)
#define SYS_UNLINK   26  // unlink(path)
#define SYS_RMDIR    27  // rmdir(path)
#define SYS_RENAME   28  // rename(from, to)
#define SYS_CHDIR    29  // chdir(path)
#define SYS_KILL     30  // kill(pid, sig) -> 0, -3 (no such process), -22 (bad signal)
#define SYS_MMAP     31  // mmap(len, prot 1=R 2=W 4=X) -> address or -errno (anonymous only)
#define SYS_MUNMAP   32  // munmap(addr, len) -> 0 or -errno
#define SYS_MAX      33

// Errors are negative errno values (see header/vfs.h); this one is not there.
#define SYS_EFAULT   (-14)   // a pointer argument outside the caller's memory

// Pushed by syscall_stub. The field order is the reverse of the pushes, so
// this maps exactly onto what pusha and the segment saves leave on the stack.
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} syscall_regs_t;

void syscall_init(void);
void syscall_dispatch(syscall_regs_t* r);

// Number of system calls serviced since boot, for the task manager.
uint32_t syscall_count(void);

// What SYS_READDIR fills in: one directory entry, long names included.
typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t size;
    uint32_t is_dir;
} proc_dirent_t;
