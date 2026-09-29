// src/vfs.c — the virtual file layer: open-file descriptions, per-process
// descriptor tables, path normalisation, the mount table, and the two
// filesystems that live in it (FAT32 at "/", a tiny devfs at "/dev").
// The contract is documented in header/vfs.h.
#include <stdint.h>
#include "header/vfs.h"
#include "header/fat32.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kprintf.h"

// ================================================================ files

vfs_file_t* vfs_file_new(const vfs_fileops_t* ops, uint32_t type, int flags, void* priv){
    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!f) return 0;
    f->ops = ops; f->refs = 1; f->flags = flags; f->pos = 0; f->type = type; f->priv = priv;
    return f;
}

void vfs_file_ref(vfs_file_t* f){ f->refs++; }

void vfs_file_put(vfs_file_t* f){
    if (!f || --f->refs > 0) return;
    if (f->ops->flush) f->ops->flush(f);
    if (f->ops->close) f->ops->close(f);
    kfree(f);
}

int vfs_file_read(vfs_file_t* f, void* buf, uint32_t n){
    if ((f->flags & VFS_O_ACCMODE) == VFS_O_WRONLY) return VFS_EBADF;
    if (f->type == VFS_T_DIR) return VFS_EISDIR;
    if (!f->ops->read) return VFS_EINVAL;
    int r = f->ops->read(f, buf, n, f->pos);
    if (r > 0 && f->ops->size) f->pos += (uint32_t)r;
    return r;
}

int vfs_file_write(vfs_file_t* f, const void* buf, uint32_t n){
    if ((f->flags & VFS_O_ACCMODE) == VFS_O_RDONLY) return VFS_EBADF;
    if (f->type == VFS_T_DIR) return VFS_EISDIR;
    if (!f->ops->write) return VFS_EINVAL;
    if ((f->flags & VFS_O_APPEND) && f->ops->size){
        int sz = f->ops->size(f);
        if (sz < 0) return sz;
        f->pos = (uint32_t)sz;
    }
    int r = f->ops->write(f, buf, n, f->pos);
    if (r > 0 && f->ops->size) f->pos += (uint32_t)r;
    return r;
}

int vfs_file_lseek(vfs_file_t* f, int32_t off, int whence){
    if (!f->ops->size) return VFS_ESPIPE;
    int64_t base;
    if (whence == VFS_SEEK_SET) base = 0;
    else if (whence == VFS_SEEK_CUR) base = f->pos;
    else if (whence == VFS_SEEK_END){
        int sz = f->ops->size(f);
        if (sz < 0) return sz;
        base = sz;
    } else return VFS_EINVAL;

    int64_t np = base + off;
    if (np < 0 || np > 0x7FFFFFFF) return VFS_EINVAL;
    f->pos = (uint32_t)np;
    return (int)np;
}

int vfs_file_readdir(vfs_file_t* f, vfs_dirent_t* out){
    if (f->type != VFS_T_DIR) return VFS_ENOTDIR;
    if (!f->ops->readdir) return VFS_EINVAL;
    int r = f->ops->readdir(f, f->pos, out);
    if (r == 1) f->pos++;
    return r;
}

int vfs_file_fstat(vfs_file_t* f, vfs_stat_t* st){
    if (!f->ops->fstat) return VFS_EINVAL;
    return f->ops->fstat(f, st);
}

int vfs_file_flush(vfs_file_t* f){
    return f->ops->flush ? f->ops->flush(f) : 0;
}

// ================================================================= paths

static int path_apply(char* out, uint32_t* len, uint32_t cap, const char* p){
    while (*p){
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        uint32_t clen = (uint32_t)(p - s);

        if (clen == 1 && s[0] == '.') continue;
        if (clen == 2 && s[0] == '.' && s[1] == '.'){
            while (*len > 1 && out[*len - 1] != '/') (*len)--;
            if (*len > 1) (*len)--;                     // drop the slash too
            out[*len] = 0;
            continue;
        }
        if (clen >= VFS_NAME_MAX) return VFS_ENAMETOOLONG;
        if (*len + clen + 2 > cap) return VFS_ENAMETOOLONG;
        if (*len > 1) out[(*len)++] = '/';
        memcpy(out + *len, s, clen);
        *len += clen;
        out[*len] = 0;
    }
    return 0;
}

int vfs_normalize(const char* cwd, const char* path, char* out, uint32_t cap){
    if (!path || !out || cap < 2) return VFS_EINVAL;
    if (!path[0]) return VFS_ENOENT;
    out[0] = '/'; out[1] = 0;
    uint32_t len = 1;
    if (path[0] != '/' && cwd && cwd[0] == '/'){
        int r = path_apply(out, &len, cap, cwd);
        if (r < 0) return r;
    }
    return path_apply(out, &len, cap, path);
}

// ================================================================ mounts

#define MOUNT_MAX 8

typedef struct {
    int                 used;
    char                path[VFS_PATH_MAX];
    uint32_t            plen;
    const vfs_fsops_t*  fs;
} mount_t;

static mount_t mounts[MOUNT_MAX];

int vfs_mount(const char* path, const vfs_fsops_t* fs){
    char canon[VFS_PATH_MAX];
    if (!fs || vfs_normalize("/", path, canon, sizeof(canon)) < 0) return VFS_EINVAL;
    int slot = -1;
    for (int i = 0; i < MOUNT_MAX; i++){
        if (mounts[i].used && strcmp(mounts[i].path, canon) == 0) return VFS_EEXIST;
        if (!mounts[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return VFS_ENOMEM;
    mounts[slot].used = 1;
    strcpy(mounts[slot].path, canon);
    mounts[slot].plen = (uint32_t)strlen(canon);
    mounts[slot].fs = fs;
    return 0;
}

int vfs_umount(const char* path){
    char canon[VFS_PATH_MAX];
    if (vfs_normalize("/", path, canon, sizeof(canon)) < 0) return VFS_EINVAL;
    for (int i = 0; i < MOUNT_MAX; i++)
        if (mounts[i].used && strcmp(mounts[i].path, canon) == 0){
            mounts[i].used = 0;
            return 0;
        }
    return VFS_ENOENT;
}

// Longest mount prefix at a component boundary. *rel points into `abs`, or at
// "/" when abs is exactly the mount point.
static const mount_t* find_mount(const char* abs, const char** rel){
    const mount_t* best = 0;
    for (int i = 0; i < MOUNT_MAX; i++){
        const mount_t* m = &mounts[i];
        if (!m->used) continue;
        if (m->plen > 1){
            if (strncmp(abs, m->path, m->plen) != 0) continue;
            if (abs[m->plen] != 0 && abs[m->plen] != '/') continue;
        }
        if (!best || m->plen > best->plen) best = m;
    }
    if (!best) return 0;
    if (best->plen <= 1) *rel = abs;
    else *rel = abs[best->plen] ? abs + best->plen : "/";
    return best;
}

static void vfs_ensure(void);

static int resolve(const char* cwd, const char* path, const mount_t** m,
                   char* abs, uint32_t cap, const char** rel){
    vfs_ensure();
    int r = vfs_normalize(cwd, path, abs, cap);
    if (r < 0) return r;
    *m = find_mount(abs, rel);
    return *m ? 0 : VFS_ENOENT;
}

// ================================================================= FAT32

typedef struct {
    fat32_dirent_t ent;
    uint32_t       dir;          // cluster of the containing directory
    uint8_t*       buf;          // whole-file write-back buffer (writable opens)
    uint32_t       len, cap;
    int            buffered, dirty;
} fatf_t;

static uint32_t cluster_of(const fat32_dirent_t* e){
    return e->first_cluster ? e->first_cluster : fat32_root_cluster();
}

static int bad_leaf(const char* n){
    for (; *n; n++)
        if ((unsigned char)*n < 0x20 || strchr("\\:*?\"<>|", *n)) return 1;
    return 0;
}

// Walks `rel` from the root. *parent, if wanted, gets the cluster of the
// directory that holds the final component.
static int fat_walk(const char* rel, fat32_dirent_t* out, uint32_t* parent){
    uint32_t root = fat32_root_cluster();
    if (!root) return VFS_EIO;

    fat32_dirent_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.is_dir = 1; cur.first_cluster = root;
    uint32_t par = root;

    const char* p = rel;
    while (*p){
        while (*p == '/') p++;
        if (!*p) break;
        char comp[FAT32_NAME_MAX];
        uint32_t n = 0;
        while (*p && *p != '/'){
            if (n >= FAT32_NAME_LIMIT) return VFS_ENAMETOOLONG;
            comp[n++] = *p++;
        }
        comp[n] = 0;
        if (!cur.is_dir) return VFS_ENOTDIR;
        par = cluster_of(&cur);
        if (fat32_find(par, comp, &cur) != 0) return VFS_ENOENT;
    }
    *out = cur;
    if (parent) *parent = par;
    return 0;
}

// Splits `rel` into the directory that must hold it and the leaf name.
static int fat_parent(const char* rel, uint32_t* dir, char* leaf){
    uint32_t len = (uint32_t)strlen(rel);
    if (len <= 1) return VFS_EINVAL;
    uint32_t sl = len;
    while (sl > 0 && rel[sl - 1] != '/') sl--;       // rel[sl-1] is the last '/'
    if (len - sl >= FAT32_NAME_MAX) return VFS_ENAMETOOLONG;

    char pbuf[VFS_PATH_MAX];
    if (sl <= 1){ pbuf[0] = '/'; pbuf[1] = 0; }
    else { memcpy(pbuf, rel, sl - 1); pbuf[sl - 1] = 0; }

    fat32_dirent_t pe;
    int r = fat_walk(pbuf, &pe, 0);
    if (r < 0) return r;
    if (!pe.is_dir) return VFS_ENOTDIR;
    *dir = cluster_of(&pe);
    memcpy(leaf, rel + sl, len - sl + 1);
    return 0;
}

static int fat_stat(const char* rel, vfs_stat_t* st){
    fat32_dirent_t e;
    int r = fat_walk(rel, &e, 0);
    if (r < 0) return r;
    st->type = e.is_dir ? VFS_T_DIR : VFS_T_FILE;
    st->size = e.is_dir ? 0 : e.size;
    st->ino  = e.first_cluster;
    st->dev  = 1;
    return 0;
}

static int fat_read(vfs_file_t* f, void* buf, uint32_t n, uint32_t off){
    fatf_t* fp = (fatf_t*)f->priv;
    if (fp->buffered){
        if (off >= fp->len) return 0;
        if (n > fp->len - off) n = fp->len - off;
        memcpy(buf, fp->buf + off, n);
        return (int)n;
    }
    return (int)fat32_read_at(&fp->ent, off, (uint8_t*)buf, n);
}

static int fat_write(vfs_file_t* f, const void* buf, uint32_t n, uint32_t off){
    fatf_t* fp = (fatf_t*)f->priv;
    if (!fp->buffered) return VFS_EBADF;
    if (n == 0) return 0;
    if (off > VFS_FILE_MAX || n > VFS_FILE_MAX - off) return VFS_ENOSPC;

    uint32_t end = off + n;
    if (end > fp->cap){
        uint32_t nc = fp->cap * 2;
        if (nc < end) nc = end;
        if (nc < 512) nc = 512;
        if (nc > VFS_FILE_MAX) nc = VFS_FILE_MAX;
        uint8_t* nb = (uint8_t*)krealloc(fp->buf, nc);
        if (!nb) return VFS_ENOMEM;
        fp->buf = nb; fp->cap = nc;
    }
    if (off > fp->len) memset(fp->buf + fp->len, 0, off - fp->len);   // a seek past EOF leaves a hole of zeros
    memcpy(fp->buf + off, buf, n);
    if (end > fp->len) fp->len = end;
    fp->dirty = 1;
    return (int)n;
}

static int fat_size(vfs_file_t* f){
    fatf_t* fp = (fatf_t*)f->priv;
    if (fp->ent.is_dir) return 0;
    return (int)(fp->buffered ? fp->len : fp->ent.size);
}

static int fat_flush(vfs_file_t* f){
    fatf_t* fp = (fatf_t*)f->priv;
    if (!fp->dirty) return 0;
    if (fat32_write_file(fp->dir, fp->ent.name, fp->buf, fp->len) != 0){
        kprintf("[vfs] write-back of %s failed\n", fp->ent.name);
        return VFS_EIO;
    }
    fp->ent.size = fp->len;
    fp->dirty = 0;
    return 0;
}

static void fat_close(vfs_file_t* f){
    fatf_t* fp = (fatf_t*)f->priv;
    if (fp->buf) kfree(fp->buf);
    kfree(fp);
}

static int fat_readdir(vfs_file_t* f, uint32_t index, vfs_dirent_t* out){
    fatf_t* fp = (fatf_t*)f->priv;
    fat32_dirent_t e;
    int r = fat32_readdir(cluster_of(&fp->ent), index, &e);
    if (r < 0) return VFS_EIO;
    if (r == 0) return 0;
    strncpy(out->name, e.name, VFS_NAME_MAX - 1);
    out->name[VFS_NAME_MAX - 1] = 0;
    out->type = e.is_dir ? VFS_T_DIR : VFS_T_FILE;
    out->size = e.is_dir ? 0 : e.size;
    return 1;
}

static int fat_fstat(vfs_file_t* f, vfs_stat_t* st){
    fatf_t* fp = (fatf_t*)f->priv;
    st->type = fp->ent.is_dir ? VFS_T_DIR : VFS_T_FILE;
    st->size = (uint32_t)fat_size(f);
    st->ino  = fp->ent.first_cluster;
    st->dev  = 1;
    return 0;
}

static const vfs_fileops_t fat_fops = {
    .read = fat_read, .write = fat_write, .size = fat_size, .readdir = fat_readdir,
    .fstat = fat_fstat, .flush = fat_flush, .close = fat_close,
};

static int fat_open(const char* rel, int flags, vfs_file_t** out){
    int acc = flags & VFS_O_ACCMODE;
    fat32_dirent_t e;
    uint32_t parent = 0;
    int r = fat_walk(rel, &e, &parent);

    if (r == VFS_ENOENT && (flags & VFS_O_CREAT)){
        char leaf[FAT32_NAME_MAX];
        uint32_t dir;
        r = fat_parent(rel, &dir, leaf);       // also proves the directories above exist
        if (r < 0) return r;
        if (bad_leaf(leaf)) return VFS_EINVAL;
        if (!fat32_writable()) return VFS_EROFS;
        if (fat32_write_file(dir, leaf, 0, 0) != 0) return VFS_ENOSPC;
        r = fat_walk(rel, &e, &parent);
        if (r < 0) return VFS_EIO;
    } else if (r < 0){
        return r;
    } else if ((flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)){
        return VFS_EEXIST;
    }

    int writing = (acc != VFS_O_RDONLY);
    if (e.is_dir){
        if (writing) return VFS_EISDIR;
    } else if (writing && !fat32_writable()){
        return VFS_EROFS;
    }

    fatf_t* fp = (fatf_t*)kmalloc(sizeof(fatf_t));
    if (!fp) return VFS_ENOMEM;
    memset(fp, 0, sizeof(*fp));
    fp->ent = e;
    fp->dir = parent;

    if (writing && !e.is_dir){
        fp->buffered = 1;
        if (flags & VFS_O_TRUNC){
            fp->dirty = 1;                      // empty on close even if never written
        } else if (e.size > 0){
            if (e.size > VFS_FILE_MAX){ kfree(fp); return VFS_ENOSPC; }
            fp->buf = (uint8_t*)kmalloc(e.size);
            if (!fp->buf){ kfree(fp); return VFS_ENOMEM; }
            fp->cap = e.size;
            if (fat32_read_file(&e, fp->buf, e.size) != e.size){
                kfree(fp->buf); kfree(fp);
                return VFS_EIO;
            }
            fp->len = e.size;
        }
    }

    int keep = flags & ~(VFS_O_CREAT | VFS_O_EXCL | VFS_O_TRUNC);
    vfs_file_t* f = vfs_file_new(&fat_fops, e.is_dir ? VFS_T_DIR : VFS_T_FILE, keep, fp);
    if (!f){
        if (fp->buf) kfree(fp->buf);
        kfree(fp);
        return VFS_ENOMEM;
    }
    *out = f;
    return 0;
}

static int fat_mkdir(const char* rel){
    uint32_t dir;
    char leaf[FAT32_NAME_MAX];
    int r = fat_parent(rel, &dir, leaf);
    if (r < 0) return r == VFS_EINVAL ? VFS_EEXIST : r;     // mkdir of the root itself
    if (bad_leaf(leaf)) return VFS_EINVAL;
    fat32_dirent_t e;
    if (fat32_find(dir, leaf, &e) == 0) return VFS_EEXIST;
    if (!fat32_writable()) return VFS_EROFS;
    return fat32_mkdir(dir, leaf) == 0 ? 0 : VFS_ENOSPC;
}

static int fat_unlink(const char* rel){
    fat32_dirent_t e; uint32_t parent;
    int r = fat_walk(rel, &e, &parent);
    if (r < 0) return r;
    if (e.is_dir) return VFS_EISDIR;
    if (!fat32_writable()) return VFS_EROFS;
    return fat32_delete(parent, e.name) == 0 ? 0 : VFS_EIO;
}

static int fat_rmdir(const char* rel){
    fat32_dirent_t e; uint32_t parent;
    if (strcmp(rel, "/") == 0) return VFS_EINVAL;
    int r = fat_walk(rel, &e, &parent);
    if (r < 0) return r;
    if (!e.is_dir) return VFS_ENOTDIR;
    if (!fat32_writable()) return VFS_EROFS;
    return fat32_rmdir(parent, e.name) == 0 ? 0 : VFS_ENOTEMPTY;
}

static int fat_rename(const char* from, const char* to){
    if (strcmp(from, "/") == 0 || strcmp(to, "/") == 0) return VFS_EINVAL;
    if (strcmp(from, to) == 0) return 0;

    // A directory cannot move into itself.
    uint32_t fl = (uint32_t)strlen(from);
    if (strncmp(to, from, fl) == 0 && to[fl] == '/') return VFS_EINVAL;

    fat32_dirent_t e; uint32_t od;
    int r = fat_walk(from, &e, &od);
    if (r < 0) return r;

    uint32_t nd;
    char nleaf[FAT32_NAME_MAX];
    r = fat_parent(to, &nd, nleaf);
    if (r < 0) return r;
    if (bad_leaf(nleaf)) return VFS_EINVAL;

    fat32_dirent_t other;
    int same_entry = (od == nd && kstricmp(e.name, nleaf) == 0);
    if (!same_entry && fat32_find(nd, nleaf, &other) == 0) return VFS_EEXIST;
    if (!fat32_writable()) return VFS_EROFS;
    return fat32_rename(od, e.name, nd, nleaf) == 0 ? 0 : VFS_EIO;
}

static const vfs_fsops_t fat_fs = {
    "fat32", fat_stat, fat_open, fat_mkdir, fat_unlink, fat_rmdir, fat_rename,
};

// ================================================================= devfs

static const char* const dev_names[] = { "null", "zero", "console" };
#define DEV_COUNT 3

static int dev_lookup(const char* rel){
    if (rel[0] != '/') return -1;
    for (int i = 0; i < DEV_COUNT; i++)
        if (strcmp(rel + 1, dev_names[i]) == 0) return i;
    return -1;
}

static int dev_read(vfs_file_t* f, void* buf, uint32_t n, uint32_t off){
    (void)off;
    uintptr_t id = (uintptr_t)f->priv;
    if (id == 1){ memset(buf, 0, n); return (int)n; }     // zero
    return 0;                        // null and console: always EOF (no stdin wired yet)
}

static int dev_write(vfs_file_t* f, const void* buf, uint32_t n, uint32_t off){
    (void)off;
    uintptr_t id = (uintptr_t)f->priv;
    if (id == 2){                    // console: through kprintf, so it follows any sink
        const uint8_t* p = (const uint8_t*)buf;
        char chunk[65];
        uint32_t i = 0;
        while (i < n){
            uint32_t k = 0;
            while (k < 64 && i < n){
                char c = (char)p[i++];
                chunk[k++] = c ? c : ' ';
            }
            chunk[k] = 0;
            kprintf("%s", chunk);
        }
    }
    return (int)n;
}

static int dev_size(vfs_file_t* f){ (void)f; return 0; }

static int dev_readdir(vfs_file_t* f, uint32_t index, vfs_dirent_t* out){
    (void)f;
    if (index >= DEV_COUNT) return 0;
    strcpy(out->name, dev_names[index]);
    out->type = VFS_T_CHR;
    out->size = 0;
    return 1;
}

static int dev_fstat(vfs_file_t* f, vfs_stat_t* st){
    st->type = f->type; st->size = 0; st->ino = (uint32_t)(uintptr_t)f->priv; st->dev = 2;
    return 0;
}

static const vfs_fileops_t dev_fops = {
    .read = dev_read, .write = dev_write, .size = dev_size,
    .readdir = dev_readdir, .fstat = dev_fstat,
};

static int dev_stat(const char* rel, vfs_stat_t* st){
    if (strcmp(rel, "/") == 0){ st->type = VFS_T_DIR; st->size = 0; st->ino = 0; st->dev = 2; return 0; }
    int id = dev_lookup(rel);
    if (id < 0) return VFS_ENOENT;
    st->type = VFS_T_CHR; st->size = 0; st->ino = (uint32_t)id; st->dev = 2;
    return 0;
}

static int dev_open(const char* rel, int flags, vfs_file_t** out){
    int is_root = strcmp(rel, "/") == 0;
    int id = is_root ? 0 : dev_lookup(rel);
    if (id < 0) return VFS_ENOENT;          // devfs has a fixed set of nodes; CREAT cannot add one
    if ((flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) return VFS_EEXIST;
    if (is_root && (flags & VFS_O_ACCMODE) != VFS_O_RDONLY) return VFS_EISDIR;
    vfs_file_t* f = vfs_file_new(&dev_fops, is_root ? VFS_T_DIR : VFS_T_CHR,
                                 flags & ~(VFS_O_CREAT | VFS_O_EXCL | VFS_O_TRUNC),
                                 (void*)(uintptr_t)id);
    if (!f) return VFS_ENOMEM;
    *out = f;
    return 0;
}

static int dev_deny1(const char* a){ (void)a; return VFS_EACCES; }
static int dev_deny2(const char* a, const char* b){ (void)a; (void)b; return VFS_EACCES; }

static const vfs_fsops_t dev_fs = {
    "devfs", dev_stat, dev_open, dev_deny1, dev_deny1, dev_deny1, dev_deny2,
};

// ================================================================ setup

static int vfs_ready = 0;

void vfs_init(void){
    if (vfs_ready) return;
    vfs_ready = 1;
    vfs_mount("/", &fat_fs);
    vfs_mount("/dev", &dev_fs);
}

static void vfs_ensure(void){ if (!vfs_ready) vfs_init(); }

// ============================================================= path API

int vfs_stat(const char* cwd, const char* path, vfs_stat_t* st){
    const mount_t* m; const char* rel; char abs[VFS_PATH_MAX];
    int r = resolve(cwd, path, &m, abs, sizeof(abs), &rel);
    return r < 0 ? r : m->fs->stat(rel, st);
}

int vfs_open_file(const char* cwd, const char* path, int flags, vfs_file_t** out){
    const mount_t* m; const char* rel; char abs[VFS_PATH_MAX];
    int r = resolve(cwd, path, &m, abs, sizeof(abs), &rel);
    return r < 0 ? r : m->fs->open(rel, flags, out);
}

int vfs_mkdir(const char* cwd, const char* path){
    const mount_t* m; const char* rel; char abs[VFS_PATH_MAX];
    int r = resolve(cwd, path, &m, abs, sizeof(abs), &rel);
    return r < 0 ? r : m->fs->mkdir(rel);
}

int vfs_unlink(const char* cwd, const char* path){
    const mount_t* m; const char* rel; char abs[VFS_PATH_MAX];
    int r = resolve(cwd, path, &m, abs, sizeof(abs), &rel);
    return r < 0 ? r : m->fs->unlink(rel);
}

int vfs_rmdir(const char* cwd, const char* path){
    const mount_t* m; const char* rel; char abs[VFS_PATH_MAX];
    int r = resolve(cwd, path, &m, abs, sizeof(abs), &rel);
    return r < 0 ? r : m->fs->rmdir(rel);
}

int vfs_rename(const char* cwd, const char* from, const char* to){
    const mount_t *m1, *m2; const char *r1, *r2;
    char a1[VFS_PATH_MAX], a2[VFS_PATH_MAX];
    int r = resolve(cwd, from, &m1, a1, sizeof(a1), &r1);
    if (r < 0) return r;
    r = resolve(cwd, to, &m2, a2, sizeof(a2), &r2);
    if (r < 0) return r;
    if (m1 != m2) return VFS_EXDEV;
    return m1->fs->rename(r1, r2);
}

int vfs_chdir(char* cwd, const char* path){
    const mount_t* m; const char* rel; char abs[VFS_PATH_MAX];
    int r = resolve(cwd, path, &m, abs, sizeof(abs), &rel);
    if (r < 0) return r;
    vfs_stat_t st;
    r = m->fs->stat(rel, &st);
    if (r < 0) return r;
    if (st.type != VFS_T_DIR) return VFS_ENOTDIR;
    strcpy(cwd, abs);
    return 0;
}

// ================================================================== fds

void vfs_fd_init(vfs_fdtable_t* t){ memset(t, 0, sizeof(*t)); }

int vfs_fd_install(vfs_fdtable_t* t, vfs_file_t* file){
    for (int i = 0; i < VFS_FD_MAX; i++)
        if (!t->f[i]){ t->f[i] = file; return i; }
    vfs_file_put(file);
    return VFS_EMFILE;
}

vfs_file_t* vfs_fd_get(vfs_fdtable_t* t, int fd){
    if (fd < 0 || fd >= VFS_FD_MAX) return 0;
    return t->f[fd];
}

int vfs_fd_close(vfs_fdtable_t* t, int fd){
    vfs_file_t* f = vfs_fd_get(t, fd);
    if (!f) return VFS_EBADF;
    t->f[fd] = 0;
    vfs_file_put(f);
    return 0;
}

int vfs_fd_dup(vfs_fdtable_t* t, int fd){
    vfs_file_t* f = vfs_fd_get(t, fd);
    if (!f) return VFS_EBADF;
    for (int i = 0; i < VFS_FD_MAX; i++)
        if (!t->f[i]){ vfs_file_ref(f); t->f[i] = f; return i; }
    return VFS_EMFILE;
}

int vfs_fd_dup2(vfs_fdtable_t* t, int oldfd, int newfd){
    vfs_file_t* f = vfs_fd_get(t, oldfd);
    if (!f || newfd < 0 || newfd >= VFS_FD_MAX) return VFS_EBADF;
    if (oldfd == newfd) return newfd;
    vfs_file_ref(f);                    // before closing newfd: they may share a description
    if (t->f[newfd]) vfs_file_put(t->f[newfd]);
    t->f[newfd] = f;
    return newfd;
}

void vfs_fd_clone(vfs_fdtable_t* dst, const vfs_fdtable_t* src){
    if (dst == src) return;
    vfs_fd_closeall(dst);
    for (int i = 0; i < VFS_FD_MAX; i++)
        if (src->f[i]){ vfs_file_ref(src->f[i]); dst->f[i] = src->f[i]; }
}

void vfs_fd_closeall(vfs_fdtable_t* t){
    for (int i = 0; i < VFS_FD_MAX; i++)
        if (t->f[i]){ vfs_file_t* f = t->f[i]; t->f[i] = 0; vfs_file_put(f); }
}

int vfs_open(vfs_fdtable_t* t, const char* cwd, const char* path, int flags){
    vfs_file_t* f;
    int r = vfs_open_file(cwd, path, flags, &f);
    if (r < 0) return r;
    return vfs_fd_install(t, f);
}

#define WITH_FD(call)                                   \
    vfs_file_t* f = vfs_fd_get(t, fd);                  \
    if (!f) return VFS_EBADF;                           \
    return call

int vfs_read(vfs_fdtable_t* t, int fd, void* buf, uint32_t n){ WITH_FD(vfs_file_read(f, buf, n)); }
int vfs_write(vfs_fdtable_t* t, int fd, const void* buf, uint32_t n){ WITH_FD(vfs_file_write(f, buf, n)); }
int vfs_lseek(vfs_fdtable_t* t, int fd, int32_t off, int whence){ WITH_FD(vfs_file_lseek(f, off, whence)); }
int vfs_fstat(vfs_fdtable_t* t, int fd, vfs_stat_t* st){ WITH_FD(vfs_file_fstat(f, st)); }
int vfs_readdir(vfs_fdtable_t* t, int fd, vfs_dirent_t* out){ WITH_FD(vfs_file_readdir(f, out)); }
int vfs_fsync(vfs_fdtable_t* t, int fd){ WITH_FD(vfs_file_flush(f)); }
int vfs_close(vfs_fdtable_t* t, int fd){ return vfs_fd_close(t, fd); }
int vfs_dup(vfs_fdtable_t* t, int fd){ return vfs_fd_dup(t, fd); }
int vfs_dup2(vfs_fdtable_t* t, int oldfd, int newfd){ return vfs_fd_dup2(t, oldfd, newfd); }
