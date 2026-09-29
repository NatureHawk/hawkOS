#pragma once
#include "header/vfs.h"

// Anonymous pipes.
//
// A pipe is a bounded ring buffer with two ends, each an ordinary vfs_file_t,
// so read/write/close/dup/dup2 on them go through the same descriptor table as
// files. Semantics follow POSIX:
//   read  : returns whatever is buffered; blocks while empty and a writer end
//           is open; returns 0 (EOF) once empty with every writer closed.
//   write : blocks while full and a reader end is open; VFS_EPIPE once every
//           reader is closed. A blocking write returns after all bytes are in.
// Ends opened with VFS_O_NONBLOCK (see pipe_set_nonblock) return VFS_EAGAIN
// instead of blocking (or a short count if some bytes did fit).
//
// Blocking is a wait-queue sleep behind pipe_wait() in pipe.c; read, write and
// close wake it. Pending signals interrupt it (VFS_EINTR), see header/signal.h.

#define PIPE_CAPACITY 4096

// Creates a pipe. *rd is the read end, *wr the write end, each with one
// reference. 0, or VFS_ENOMEM.
int  pipe_create(vfs_file_t** rd, vfs_file_t** wr);

// Creates a pipe and installs both ends in `t`: fds[0] read, fds[1] write.
// This is what a pipe() system call does. 0 or -err (nothing left open).
int  pipe_open_fds(vfs_fdtable_t* t, int fds[2]);

void pipe_set_nonblock(vfs_file_t* end, int on);

// Bytes currently buffered (for tests and diagnostics).
uint32_t pipe_buffered(vfs_file_t* end);
