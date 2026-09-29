#pragma once
#include <stdint.h>

// Virtual file layer.
//
// Everything a program can open -- a FAT32 file, a directory, a device node, a
// pipe end -- is a vfs_file_t: an open-file description with an operations
// table, a position and a reference count. A process holds them through a
// vfs_fdtable_t, whose slots may share one description (dup) exactly as POSIX
// file descriptors do: shared position, closed when the last reference goes.
//
// Paths are strings. There is no kernel-side "current directory" -- callers
// pass a cwd (an absolute path string) with every relative lookup, so each
// process can carry its own `char cwd[VFS_PATH_MAX]` and nothing here needs
// to know which task is running. vfs_normalize() resolves "." and ".." and
// repeated slashes lexically ("/a/../b" -> "/b"; ".." at "/" stays at "/").
//
// Mount table: FAT32 at "/" and a device filesystem at "/dev" (null, zero,
// console). The longest matching mount prefix wins. Mount points do not show
// up in a listing of their parent directory, but can be opened and stat'ed.
//
// Errors are negative values (Linux errno numbers, negated) so the syscall
// layer can hand them straight back to user space.

#define VFS_PATH_MAX  256
#define VFS_NAME_MAX  128       // == FAT32_NAME_MAX
#define VFS_FD_MAX    16
#define VFS_FILE_MAX  (4u * 1024u * 1024u)   // largest FAT32 file opened for writing

#define VFS_EPERM        (-1)
#define VFS_ENOENT       (-2)
#define VFS_ESRCH        (-3)
#define VFS_EINTR        (-4)
#define VFS_EIO          (-5)
#define VFS_EBADF        (-9)
#define VFS_EAGAIN       (-11)
#define VFS_ENOMEM       (-12)
#define VFS_EACCES       (-13)
#define VFS_EEXIST       (-17)
#define VFS_EXDEV        (-18)
#define VFS_ENOTDIR      (-20)
#define VFS_EISDIR       (-21)
#define VFS_EINVAL       (-22)
#define VFS_EMFILE       (-24)
#define VFS_ENOSPC       (-28)
#define VFS_ESPIPE       (-29)
#define VFS_EROFS        (-30)
#define VFS_EPIPE        (-32)
#define VFS_ENAMETOOLONG (-36)
#define VFS_ENOSYS       (-38)
#define VFS_ENOTEMPTY    (-39)

// open() flags -- the Linux i386 values, so user space can share them.
#define VFS_O_RDONLY    0x0000
#define VFS_O_WRONLY    0x0001
#define VFS_O_RDWR      0x0002
#define VFS_O_ACCMODE   0x0003
#define VFS_O_CREAT     0x0040
#define VFS_O_EXCL      0x0080
#define VFS_O_TRUNC     0x0200
#define VFS_O_APPEND    0x0400
#define VFS_O_NONBLOCK  0x0800

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

enum { VFS_T_FILE = 1, VFS_T_DIR = 2, VFS_T_CHR = 3, VFS_T_PIPE = 4 };

typedef struct {
    uint32_t type;      // VFS_T_*
    uint32_t size;      // bytes (0 for directories and devices)
    uint32_t ino;       // filesystem-specific id (FAT32: first cluster)
    uint32_t dev;       // 1 = FAT32, 2 = devfs, 3 = pipe
} vfs_stat_t;

typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
} vfs_dirent_t;

// ------------------------------------------------------------ open files

struct vfs_file;

typedef struct vfs_fileops {
    // `off` is the file position for seekable files (those with size()); the
    // generic wrappers advance f->pos, the callbacks must not.
    int  (*read)(struct vfs_file* f, void* buf, uint32_t n, uint32_t off);         // bytes, 0 = EOF, or -err
    int  (*write)(struct vfs_file* f, const void* buf, uint32_t n, uint32_t off);  // bytes or -err
    int  (*size)(struct vfs_file* f);            // NULL => not seekable (pipes)
    int  (*readdir)(struct vfs_file* f, uint32_t index, vfs_dirent_t* out);        // 1, 0 at end, or -err
    int  (*fstat)(struct vfs_file* f, vfs_stat_t* st);
    int  (*flush)(struct vfs_file* f);           // write back buffered data; optional
    void (*close)(struct vfs_file* f);           // last reference dropped; optional
} vfs_fileops_t;

typedef struct vfs_file {
    const vfs_fileops_t* ops;
    int      refs;
    int      flags;      // VFS_O_*
    uint32_t pos;        // byte offset, or entry index for directories
    uint32_t type;       // VFS_T_*
    void*    priv;
} vfs_file_t;

// Allocates a description with one reference. NULL on out of memory.
vfs_file_t* vfs_file_new(const vfs_fileops_t* ops, uint32_t type, int flags, void* priv);
void        vfs_file_ref(vfs_file_t* f);
// Drops a reference; at zero, flushes, calls ops->close and frees.
void        vfs_file_put(vfs_file_t* f);

int vfs_file_read(vfs_file_t* f, void* buf, uint32_t n);
int vfs_file_write(vfs_file_t* f, const void* buf, uint32_t n);
int vfs_file_lseek(vfs_file_t* f, int32_t off, int whence);   // new position, or -err
int vfs_file_readdir(vfs_file_t* f, vfs_dirent_t* out);       // 1, 0 at end, or -err
int vfs_file_fstat(vfs_file_t* f, vfs_stat_t* st);
int vfs_file_flush(vfs_file_t* f);

// ------------------------------------------------------------ mounting

typedef struct vfs_fsops {
    const char* name;
    // `rel` is a canonical absolute path inside the filesystem ("/" is its root).
    int (*stat)(const char* rel, vfs_stat_t* st);
    int (*open)(const char* rel, int flags, vfs_file_t** out);
    int (*mkdir)(const char* rel);
    int (*unlink)(const char* rel);
    int (*rmdir)(const char* rel);
    int (*rename)(const char* from, const char* to);
} vfs_fsops_t;

// Mounts the two built-in filesystems. Idempotent, and called on demand by
// every entry point below, so explicit use is optional.
void vfs_init(void);
int  vfs_mount(const char* path, const vfs_fsops_t* fs);
int  vfs_umount(const char* path);

// ------------------------------------------------------------ paths

// Joins cwd (NULL = "/") and path into a canonical absolute path in `out`.
// 0, or VFS_ENAMETOOLONG / VFS_EINVAL.
int vfs_normalize(const char* cwd, const char* path, char* out, uint32_t cap);

int vfs_stat(const char* cwd, const char* path, vfs_stat_t* st);
int vfs_mkdir(const char* cwd, const char* path);
int vfs_unlink(const char* cwd, const char* path);     // files only
int vfs_rmdir(const char* cwd, const char* path);      // empty directories only
// Renames or moves; fails with VFS_EEXIST if `to` exists (no replace), and
// VFS_EXDEV across mounts.
int vfs_rename(const char* cwd, const char* from, const char* to);
// Changes `cwd` (a char[VFS_PATH_MAX] owned by the caller) if `path` is a
// directory. 0 or -err; on failure cwd is untouched.
int vfs_chdir(char* cwd, const char* path);

int vfs_open_file(const char* cwd, const char* path, int flags, vfs_file_t** out);

// ------------------------------------------------------------ descriptors

// Embed one of these in each process. Zero-initialised memory is a valid
// empty table; vfs_fd_init() makes that explicit.
typedef struct {
    vfs_file_t* f[VFS_FD_MAX];
} vfs_fdtable_t;

void        vfs_fd_init(vfs_fdtable_t* t);
// Installs `file` (consuming the caller's reference) at the lowest free slot.
// Returns the fd, or VFS_EMFILE -- in which case the reference is dropped.
int         vfs_fd_install(vfs_fdtable_t* t, vfs_file_t* file);
vfs_file_t* vfs_fd_get(vfs_fdtable_t* t, int fd);            // NULL if not open
int         vfs_fd_close(vfs_fdtable_t* t, int fd);
int         vfs_fd_dup(vfs_fdtable_t* t, int fd);            // lowest free fd sharing the description
int         vfs_fd_dup2(vfs_fdtable_t* t, int oldfd, int newfd);  // closes newfd first; returns newfd
// fork/spawn: every open description in src gains a reference in dst
// (dst's previous contents are closed first).
void        vfs_fd_clone(vfs_fdtable_t* dst, const vfs_fdtable_t* src);
void        vfs_fd_closeall(vfs_fdtable_t* t);               // process exit

int vfs_open(vfs_fdtable_t* t, const char* cwd, const char* path, int flags);   // fd or -err
int vfs_read(vfs_fdtable_t* t, int fd, void* buf, uint32_t n);
int vfs_write(vfs_fdtable_t* t, int fd, const void* buf, uint32_t n);
int vfs_lseek(vfs_fdtable_t* t, int fd, int32_t off, int whence);
int vfs_close(vfs_fdtable_t* t, int fd);
int vfs_fstat(vfs_fdtable_t* t, int fd, vfs_stat_t* st);
int vfs_readdir(vfs_fdtable_t* t, int fd, vfs_dirent_t* out);   // one entry per call: 1, 0 at end, -err
int vfs_fsync(vfs_fdtable_t* t, int fd);
int vfs_dup(vfs_fdtable_t* t, int fd);
int vfs_dup2(vfs_fdtable_t* t, int oldfd, int newfd);
