// src/syscall.c — the system call dispatcher
//
// Every argument arriving here came from a program the kernel does not trust,
// so every pointer is checked before it is dereferenced. That check is the
// entire point of the privilege boundary: without it, ring 3 is a formality
// and a user program can still make the kernel read or write anywhere by
// passing a kernel address to write().
//
// The check is against the calling process's region list (vm_range_ok), and
// data crosses the boundary through vm_copy_to / vm_copy_from, which fault
// demand-zero pages in. Nothing here dereferences a user pointer directly.
//
// Errors are negative errno values in eax (header/vfs.h). Before returning to
// ring 3 every call checks for a pending signal, so a kill lands at the next
// system call boundary; calls that block (pipes, waitpid, sleep) wake up to
// notice one and return VFS_EINTR.
#include <stdint.h>
#include "header/syscall.h"
#include "header/idt.h"
#include "header/task.h"
#include "header/proc.h"
#include "header/vfs.h"
#include "header/vm.h"
#include "header/pipe.h"
#include "header/signal.h"
#include "header/kprintf.h"
#include "header/kstring.h"

extern volatile unsigned long long ticks;
extern void syscall_stub(void);

static uint32_t calls = 0;

uint32_t syscall_count(void){ return calls; }

void syscall_init(void){
    // DPL 3 so ring 3 may invoke it; every other vector stays ring 0.
    set_gate_dpl(0x80, (uint32_t)syscall_stub, 3);
    kprintf("[syscall] int 0x80 gate installed (%d calls)\n", SYS_MAX - 1);
}

// A console write of more than this is refused rather than truncated: it is
// far more likely to be a wild length than a genuine request. File transfers
// are bounded by the file size limit instead.
#define WRITE_MAX 4096
#define IO_MAX    (1024u * 1024u)
#define CHUNK     4096u        // bounce buffer for moving data across the boundary
#define MMAP_MAX  (64u * 1024u * 1024u)

#define FAIL   ((uint32_t)-1)
#define NO_TID 0xFFFFFFFFu
#define ENOMEM_ VFS_ENOMEM
#define ERANGE_ (-34)

// -------------------------------------------------------------- helpers

static int path_arg(uint32_t addr, char* out){
    int rc = proc_copy_string(addr, out, VFS_PATH_MAX);
    if (rc == 0 && out[0] == 0){ out[0] = '.'; out[1] = 0; }   // "" means here
    return rc;
}

static int put_user(proc_t* p, uint32_t dst, const void* src, uint32_t n){
    if (!vm_range_ok(p->vm, dst, n, 1)) return SYS_EFAULT;
    return vm_copy_to(p->vm, dst, src, n) == 0 ? 0 : SYS_EFAULT;
}

static int do_write(proc_t* p, int fd, uint32_t buf, uint32_t n){
    vfs_file_t* f = vfs_fd_get(&p->fdt, fd);
    if (!f) return VFS_EBADF;
    if (n > IO_MAX || (f->type == VFS_T_CHR && n > WRITE_MAX)) return VFS_EINVAL;
    if (!proc_check_range(buf, n, 0)) return SYS_EFAULT;

    uint8_t tmp[CHUNK];
    uint32_t done = 0;
    while (done < n){
        uint32_t c = n - done < CHUNK ? n - done : CHUNK;
        if (vm_copy_from(p->vm, buf + done, tmp, c) != 0) return done ? (int)done : SYS_EFAULT;
        int w = vfs_file_write(f, tmp, c);
        if (w < 0) return done ? (int)done : w;
        done += (uint32_t)w;
        if ((uint32_t)w < c) break;
    }
    return (int)done;
}

static int do_read(proc_t* p, int fd, uint32_t buf, uint32_t n){
    vfs_file_t* f = vfs_fd_get(&p->fdt, fd);
    if (!f) return VFS_EBADF;
    if (n > IO_MAX) return VFS_EINVAL;
    if (!proc_check_range(buf, n, 1)) return SYS_EFAULT;

    // Only a regular file is worth a second chunk: a pipe or device that
    // returned short has nothing more right now, and asking again would block.
    int regular = (f->type == VFS_T_FILE);
    uint8_t tmp[CHUNK];
    uint32_t done = 0;
    while (done < n){
        uint32_t c = n - done < CHUNK ? n - done : CHUNK;
        int r = vfs_file_read(f, tmp, c);
        if (r < 0) return done ? (int)done : r;
        if (r == 0) break;
        if (vm_copy_to(p->vm, buf + done, tmp, (uint32_t)r) != 0) return done ? (int)done : SYS_EFAULT;
        done += (uint32_t)r;
        if ((uint32_t)r < c || !regular) break;
    }
    return (int)done;
}

static int do_readdir(proc_t* p, uint32_t pa, uint32_t idx, uint32_t ua){
    char path[VFS_PATH_MAX];
    int rc = path_arg(pa, path);
    if (rc < 0) return rc;
    if (!proc_check_range(ua, sizeof(proc_dirent_t), 1)) return SYS_EFAULT;

    vfs_file_t* d;
    rc = vfs_open_file(p->cwd, path, VFS_O_RDONLY, &d);
    if (rc < 0) return rc;
    if (d->type != VFS_T_DIR){ vfs_file_put(d); return VFS_ENOTDIR; }
    d->pos = idx;
    vfs_dirent_t e;
    rc = vfs_file_readdir(d, &e);
    vfs_file_put(d);
    if (rc != 1) return rc;

    proc_dirent_t out;
    memset(&out, 0, sizeof(out));
    strncpy(out.name, e.name, sizeof(out.name) - 1);
    out.size   = e.size;
    out.is_dir = e.type == VFS_T_DIR;
    rc = put_user(p, ua, &out, sizeof(out));
    return rc < 0 ? rc : 1;
}

static int do_fsize(proc_t* p, uint32_t pa){
    char path[VFS_PATH_MAX];
    int rc = path_arg(pa, path);
    if (rc < 0) return rc;
    vfs_stat_t st;
    rc = vfs_stat(p->cwd, path, &st);
    if (rc < 0) return rc;
    return st.type == VFS_T_FILE ? (int)st.size : VFS_EISDIR;
}

static int do_readfile(proc_t* p, uint32_t pa, uint32_t buf, uint32_t cap){
    char path[VFS_PATH_MAX];
    int rc = path_arg(pa, path);
    if (rc < 0) return rc;
    if (cap > IO_MAX) return VFS_EINVAL;
    if (!proc_check_range(buf, cap, 1)) return SYS_EFAULT;
    vfs_file_t* f;
    rc = vfs_open_file(p->cwd, path, VFS_O_RDONLY, &f);
    if (rc < 0) return rc;
    if (f->type != VFS_T_FILE){ vfs_file_put(f); return VFS_EISDIR; }
    uint8_t tmp[CHUNK];
    uint32_t done = 0;
    while (done < cap){
        uint32_t c = cap - done < CHUNK ? cap - done : CHUNK;
        int r = vfs_file_read(f, tmp, c);
        if (r <= 0) break;
        if (vm_copy_to(p->vm, buf + done, tmp, (uint32_t)r) != 0) break;
        done += (uint32_t)r;
    }
    vfs_file_put(f);
    return (int)done;
}

static int do_spawn(uint32_t pa, uint32_t aa, uint32_t flags){
    char path[VFS_PATH_MAX];
    char args[PROC_ARGS_BYTES + 1];
    int rc = proc_copy_string(pa, path, sizeof(path));
    if (rc < 0) return rc;
    if (aa){
        rc = proc_copy_string(aa, args, sizeof(args));
        if (rc < 0) return rc;
    }
    return proc_spawn_ex(path, aa ? args : 0, 0, (int)(flags & PROC_SPAWN_INHERIT));
}

// Calls that take one path and hand it to a VFS entry point.
static int do_path1(proc_t* p, uint32_t nr, uint32_t pa){
    char path[VFS_PATH_MAX];
    int rc = path_arg(pa, path);
    if (rc < 0) return rc;
    switch (nr){
        case SYS_MKDIR:  return vfs_mkdir(p->cwd, path);
        case SYS_UNLINK: return vfs_unlink(p->cwd, path);
        case SYS_RMDIR:  return vfs_rmdir(p->cwd, path);
        default:         return vfs_chdir(p->cwd, path);
    }
}

static int do_rename(proc_t* p, uint32_t fa, uint32_t ta){
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    int rc = path_arg(fa, from);
    if (rc == 0) rc = path_arg(ta, to);
    if (rc < 0) return rc;
    return vfs_rename(p->cwd, from, to);
}

static int do_stat(proc_t* p, uint32_t pa, uint32_t ua){
    char path[VFS_PATH_MAX];
    int rc = path_arg(pa, path);
    if (rc < 0) return rc;
    if (!proc_check_range(ua, sizeof(vfs_stat_t), 1)) return SYS_EFAULT;
    vfs_stat_t st;
    rc = vfs_stat(p->cwd, path, &st);
    if (rc < 0) return rc;
    return put_user(p, ua, &st, sizeof(st));
}

static int do_fstat(proc_t* p, int fd, uint32_t ua){
    if (!proc_check_range(ua, sizeof(vfs_stat_t), 1)) return SYS_EFAULT;
    vfs_stat_t st;
    int rc = vfs_fstat(&p->fdt, fd, &st);
    if (rc < 0) return rc;
    return put_user(p, ua, &st, sizeof(st));
}

static int do_pipe(proc_t* p, uint32_t ua){
    if (!proc_check_range(ua, 2 * sizeof(int), 1)) return SYS_EFAULT;
    int fds[2];
    int rc = pipe_open_fds(&p->fdt, fds);
    if (rc < 0) return rc;
    if (put_user(p, ua, fds, sizeof(fds)) < 0){
        vfs_fd_close(&p->fdt, fds[0]);
        vfs_fd_close(&p->fdt, fds[1]);
        return SYS_EFAULT;
    }
    return 0;
}

static int do_getcwd(proc_t* p, uint32_t buf, uint32_t size){
    uint32_t len = (uint32_t)strlen(p->cwd) + 1;
    if (size < len) return ERANGE_;
    if (!proc_check_range(buf, len, 1)) return SYS_EFAULT;
    return put_user(p, buf, p->cwd, len) < 0 ? SYS_EFAULT : (int)len;
}

static int do_sleep(uint32_t ms){
    if (ms > 60000u) ms = 60000u;
    // In slices, so a signal aimed at a sleeper is noticed within one.
    while (ms){
        uint32_t s = ms < 20 ? ms : 20;
        task_sleep(s);
        ms -= s;
        if (ms && signal_pending_current()) return VFS_EINTR;
    }
    return 0;
}

static uint32_t do_sbrk(proc_t* p, int32_t incr){
    uint32_t old = p->vm->brk;
    int64_t nb = (int64_t)old + incr;
    if (nb < 0 || nb > (int64_t)0xFFFFFFFFu) return FAIL;
    if (incr != 0 && vm_brk(p->vm, (uint32_t)nb) != (uint32_t)nb) return FAIL;
    return old;
}

// -------------------------------------------------------------- dispatch

void syscall_dispatch(syscall_regs_t* r){
    calls++;

    uint32_t nr = r->eax;
    uint32_t a1 = r->ebx, a2 = r->ecx, a3 = r->edx;
    proc_t* p = proc_current();

    // Everything below needs a process; a kernel task has neither the
    // descriptors nor the address space these calls act on.
    switch (nr){
        case SYS_EXIT:
        case SYS_GETPID: case SYS_YIELD: case SYS_SLEEP: case SYS_TICKS:
        case SYS_GETTIME:
            break;
        default:
            if (nr < SYS_MAX && !p){ r->eax = FAIL; return; }
            break;
    }

    switch (nr){
        case SYS_EXIT:
            proc_exit((int)a1);
            r->eax = 0;
            break;

        case SYS_WRITE:
            // a1 = fd, a2 = buffer, a3 = length. The buffer has to lie
            // entirely inside the calling process's own regions.
            r->eax = (uint32_t)do_write(p, (int)a1, a2, a3);
            break;

        case SYS_GETPID:
            r->eax = p ? p->pid : task_current_id();
            break;

        case SYS_YIELD:
            task_yield();
            r->eax = 0;
            break;

        case SYS_SLEEP:
            r->eax = (uint32_t)do_sleep(a1);
            break;

        case SYS_TICKS:
            r->eax = (uint32_t)ticks;
            break;

        case SYS_FSIZE:
            r->eax = (uint32_t)do_fsize(p, a1);
            break;

        case SYS_READFILE:
            r->eax = (uint32_t)do_readfile(p, a1, a2, a3);
            break;

        case SYS_OPEN: {
            char path[VFS_PATH_MAX];
            int rc = path_arg(a1, path);
            if (rc == 0 && (a2 & VFS_O_ACCMODE) == VFS_O_ACCMODE) rc = VFS_EINVAL;
            r->eax = (uint32_t)(rc < 0 ? rc : vfs_open(&p->fdt, p->cwd, path, (int)a2));
        } break;

        case SYS_CLOSE:
            r->eax = (uint32_t)vfs_close(&p->fdt, (int)a1);
            break;

        case SYS_READ:
            r->eax = (uint32_t)do_read(p, (int)a1, a2, a3);
            break;

        case SYS_LSEEK:
            r->eax = (uint32_t)vfs_lseek(&p->fdt, (int)a1, (int32_t)a2, (int)a3);
            break;

        case SYS_READDIR:
            // a1 = path, a2 = entry index, a3 = proc_dirent_t to fill.
            r->eax = (uint32_t)do_readdir(p, a1, a2, a3);
            break;

        case SYS_SPAWN:
            // a1 = path, a2 = optional argument string (0 for none), a3 = flags.
            r->eax = (uint32_t)do_spawn(a1, a2, a3);
            break;

        case SYS_WAITPID: {
            // a1 = pid, a2 = optional int for the child's exit status.
            if (a2 && !proc_check_range(a2, sizeof(int), 1)){ r->eax = (uint32_t)SYS_EFAULT; break; }
            int code = 0;
            int rc = proc_wait((int)a1, &code);
            // Written after the wait, which can be long, into memory that has
            // to be checked again.
            if (rc >= 0 && a2 && put_user(p, a2, &code, sizeof(code)) < 0) rc = SYS_EFAULT;
            r->eax = (uint32_t)rc;
        } break;

        case SYS_BRK:
            if (a1 == 0) r->eax = p->vm->brk;
            else r->eax = vm_brk(p->vm, a1) == a1 ? a1 : FAIL;
            break;

        case SYS_SBRK:
            r->eax = do_sbrk(p, (int32_t)a1);
            break;

        case SYS_GETTIME:
            r->eax = proc_gettime();
            break;

        case SYS_GETCWD:
            r->eax = (uint32_t)do_getcwd(p, a1, a2);
            break;

        case SYS_DUP:
            r->eax = (uint32_t)vfs_dup(&p->fdt, (int)a1);
            break;

        case SYS_DUP2:
            r->eax = (uint32_t)vfs_dup2(&p->fdt, (int)a1, (int)a2);
            break;

        case SYS_PIPE:
            r->eax = (uint32_t)do_pipe(p, a1);
            break;

        case SYS_FSTAT:
            r->eax = (uint32_t)do_fstat(p, (int)a1, a2);
            break;

        case SYS_STAT:
            r->eax = (uint32_t)do_stat(p, a1, a2);
            break;

        case SYS_MKDIR: case SYS_UNLINK: case SYS_RMDIR: case SYS_CHDIR:
            r->eax = (uint32_t)do_path1(p, nr, a1);
            break;

        case SYS_RENAME:
            r->eax = (uint32_t)do_rename(p, a1, a2);
            break;

        case SYS_KILL: {
            // The pid is a process id; signals are addressed by task id.
            uint32_t tid = proc_tid_of(a1);
            r->eax = (uint32_t)(tid == NO_TID ? SIGNAL_ESRCH : signal_send(tid, (int)a2));
        } break;

        case SYS_MMAP: {
            // a1 = length, a2 = protection bits (1 read, 2 write, 4 exec).
            if (a1 == 0 || a1 > MMAP_MAX || !(a2 & 7) || (a2 & ~7u)){
                r->eax = (uint32_t)VFS_EINVAL; break;
            }
            uint32_t addr = vm_mmap_anon(p->vm, a1, (a2 & 7) | VM_PROT_USER);
            r->eax = addr ? addr : (uint32_t)ENOMEM_;
        } break;

        case SYS_MUNMAP:
            // Holes are fine to unmap, but an address below the user range is
            // a mistake worth reporting rather than a no-op.
            r->eax = (uint32_t)(a1 >= PROC_BASE && vm_unmap(p->vm, a1, a2) == 0 ? 0 : VFS_EINVAL);
            break;

        default:
            kprintf("[syscall] pid %u made unknown call %u\n", task_current_id(), nr);
            r->eax = FAIL;
            break;
    }

    // Last thing before ring 3: a pending fatal signal ends the process here.
    signal_check_current();
}
