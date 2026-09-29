// src/pipe.c — anonymous pipes: a ring buffer shared by a read end and a
// write end, each a vfs_file_t. See header/pipe.h for the semantics.
#include <stdint.h>
#include "header/pipe.h"
#include "header/signal.h"
#include "header/sync.h"
#include "header/task.h"
#include "header/irqctl.h"
#include "header/kheap.h"
#include "header/kstring.h"

typedef struct {
    uint8_t  buf[PIPE_CAPACITY];
    uint32_t head;       // next byte to read
    uint32_t count;      // bytes buffered
    int      readers;    // open read ends (0 or 1: dups share one description)
    int      writers;
    waitq_t  wq;         // readers and writers both sleep here; every state change wakes it
} pipe_t;

// The single place a blocked pipe operation waits: asleep on the pipe's queue
// until a read, write or close changes something. `seq` was read before the
// caller looked at the state, so a wake that landed in between is not lost.
// The timeout is only so a signal aimed at the sleeper is noticed. Returns 0
// to retry, or VFS_EINTR when a signal is waiting for the caller.
static int pipe_wait(pipe_t* p, uint32_t seq){
    if (signal_pending_current()) return VFS_EINTR;
    waitq_wait_seq(&p->wq, seq, 50);
    if (signal_pending_current()) return VFS_EINTR;
    return 0;
}

static int pipe_read(vfs_file_t* f, void* vbuf, uint32_t n, uint32_t off){
    (void)off;
    pipe_t* p = (pipe_t*)f->priv;
    uint8_t* buf = (uint8_t*)vbuf;
    if (n == 0) return 0;

    // Interrupts are off while the buffer is inspected and copied, so a task
    // switch cannot interleave another reader or writer half-way through.
    uint32_t seq = waitq_seq(&p->wq);
    uint32_t fl = irq_save();
    while (p->count == 0){
        if (p->writers == 0){ irq_restore(fl); return 0; }   // EOF
        if (f->flags & VFS_O_NONBLOCK){ irq_restore(fl); return VFS_EAGAIN; }
        irq_restore(fl);
        int w = pipe_wait(p, seq);
        if (w < 0) return w;
        seq = waitq_seq(&p->wq);
        fl = irq_save();
    }

    uint32_t got = n < p->count ? n : p->count;
    for (uint32_t i = 0; i < got; i++)
        buf[i] = p->buf[(p->head + i) % PIPE_CAPACITY];
    p->head = (p->head + got) % PIPE_CAPACITY;
    p->count -= got;
    irq_restore(fl);
    waitq_wake_all(&p->wq);              // room for a blocked writer
    return (int)got;
}

static int pipe_write(vfs_file_t* f, const void* vbuf, uint32_t n, uint32_t off){
    (void)off;
    pipe_t* p = (pipe_t*)f->priv;
    const uint8_t* buf = (const uint8_t*)vbuf;
    uint32_t done = 0;

    while (done < n){
        uint32_t seq = waitq_seq(&p->wq);
        uint32_t fl = irq_save();
        if (p->readers == 0){ irq_restore(fl); return done ? (int)done : VFS_EPIPE; }
        uint32_t room = PIPE_CAPACITY - p->count;
        if (room == 0){
            irq_restore(fl);
            if (f->flags & VFS_O_NONBLOCK) return done ? (int)done : VFS_EAGAIN;
            int w = pipe_wait(p, seq);
            if (w < 0) return done ? (int)done : w;
            continue;
        }
        uint32_t chunk = n - done < room ? n - done : room;
        uint32_t tail = (p->head + p->count) % PIPE_CAPACITY;
        for (uint32_t i = 0; i < chunk; i++)
            p->buf[(tail + i) % PIPE_CAPACITY] = buf[done + i];
        p->count += chunk;
        irq_restore(fl);
        waitq_wake_all(&p->wq);          // data for a blocked reader
        done += chunk;
    }
    return (int)done;
}

static int pipe_fstat(vfs_file_t* f, vfs_stat_t* st){
    pipe_t* p = (pipe_t*)f->priv;
    st->type = VFS_T_PIPE;
    st->size = p->count;
    st->ino  = 0;
    st->dev  = 3;
    return 0;
}

static void pipe_close(vfs_file_t* f){
    pipe_t* p = (pipe_t*)f->priv;
    if ((f->flags & VFS_O_ACCMODE) == VFS_O_WRONLY) p->writers = 0;
    else p->readers = 0;
    if (!p->readers && !p->writers){ kfree(p); return; }
    waitq_wake_all(&p->wq);              // the peer sees EOF / EPIPE
}

static const vfs_fileops_t pipe_ops = {
    .read = pipe_read, .write = pipe_write, .fstat = pipe_fstat, .close = pipe_close,
};

int pipe_create(vfs_file_t** rd, vfs_file_t** wr){
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) return VFS_ENOMEM;
    p->head = p->count = 0;
    p->readers = p->writers = 1;
    waitq_init(&p->wq);

    vfs_file_t* r = vfs_file_new(&pipe_ops, VFS_T_PIPE, VFS_O_RDONLY, p);
    vfs_file_t* w = vfs_file_new(&pipe_ops, VFS_T_PIPE, VFS_O_WRONLY, p);
    if (!r || !w){
        // Neither end was closed through vfs_file_put, so account for them by
        // hand: dropping an end that never existed must not free the pipe
        // out from under the one that did.
        if (r){ p->writers = 0; vfs_file_put(r); }
        else if (w){ p->readers = 0; vfs_file_put(w); }
        else kfree(p);
        return VFS_ENOMEM;
    }
    *rd = r;
    *wr = w;
    return 0;
}

int pipe_open_fds(vfs_fdtable_t* t, int fds[2]){
    vfs_file_t *r, *w;
    int rc = pipe_create(&r, &w);
    if (rc < 0) return rc;

    int rfd = vfs_fd_install(t, r);
    if (rfd < 0){ vfs_file_put(w); return rfd; }
    int wfd = vfs_fd_install(t, w);
    if (wfd < 0){ vfs_fd_close(t, rfd); return wfd; }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

void pipe_set_nonblock(vfs_file_t* end, int on){
    if (on) end->flags |= VFS_O_NONBLOCK;
    else    end->flags &= ~VFS_O_NONBLOCK;
}

uint32_t pipe_buffered(vfs_file_t* end){
    return ((pipe_t*)end->priv)->count;
}
