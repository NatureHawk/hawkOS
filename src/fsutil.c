// src/fsutil.c — file operations shared by Files and the editor
#include <stdint.h>
#include "header/fsutil.h"
#include "header/fat32.h"
#include "header/kheap.h"
#include "header/kstring.h"

#define LIST_MAX    256
#define TREE_DEPTH  10

static int is_dot(const char* n){ return n[0] == '.' && n[1] == 0; }
static int is_dotdot(const char* n){ return n[0] == '.' && n[1] == '.' && n[2] == 0; }

int fs_name_ok(const char* name){
    if (!name || !name[0]) return 0;
    if (strlen(name) > FAT32_NAME_LIMIT) return 0;
    if (is_dot(name) || is_dotdot(name)) return 0;
    for (const char* p = name; *p; p++){
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) return 0;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') return 0;
    }
    // A trailing space or dot is silently dropped by FAT, which would create
    // a file under a different name from the one that was asked for.
    char last = name[strlen(name) - 1];
    if (last == ' ' || last == '.') return 0;
    return 1;
}

int fs_exists(uint32_t dir, const char* name, fat32_dirent_t* out){
    fat32_dirent_t tmp;
    return fat32_find(dir, name, out ? out : &tmp) == 0;
}

void fs_unique_name(uint32_t dir, const char* name, char* out, uint32_t cap){
    if (cap == 0) return;
    strncpy(out, name, cap - 1);
    out[cap - 1] = 0;
    if (!fs_exists(dir, out, 0)) return;

    // Split at the last dot, but not a leading one: ".profile" has no
    // extension and "notes.tar.gz" keeps only ".gz" as its.
    const char* dot = 0;
    for (const char* p = name; *p; p++) if (*p == '.' && p != name) dot = p;
    char base[FAT32_NAME_MAX], ext[FAT32_NAME_MAX];
    if (dot){
        uint32_t bl = (uint32_t)(dot - name);
        if (bl > FAT32_NAME_MAX - 1) bl = FAT32_NAME_MAX - 1;
        memcpy(base, name, bl); base[bl] = 0;
        strncpy(ext, dot, FAT32_NAME_MAX - 1); ext[FAT32_NAME_MAX - 1] = 0;
    } else {
        strncpy(base, name, FAT32_NAME_MAX - 1); base[FAT32_NAME_MAX - 1] = 0;
        ext[0] = 0;
    }

    // Leave room for " - Copy (99)" so the suffix is never the part that gets
    // cut when a name is already near the limit.
    uint32_t budget = FAT32_NAME_LIMIT;
    uint32_t el = (uint32_t)strlen(ext);
    budget = budget > el + 14 ? budget - el - 14 : 1;
    if (strlen(base) > budget) base[budget] = 0;

    for (int n = 1; n < 100; n++){
        if (n == 1) ksnprintf(out, cap, "%s - Copy%s", base, ext);
        else        ksnprintf(out, cap, "%s - Copy (%d)%s", base, n, ext);
        if (!fs_exists(dir, out, 0)) return;
    }
}

// ------------------------------------------------------------------- copy

static int copy_rec(uint32_t sdir, const fat32_dirent_t* e, uint32_t ddir,
                    const char* dname, uint32_t guard, int depth){
    (void)sdir;
    if (!e->is_dir){
        if (e->size > FS_COPY_MAX) return -1;
        uint8_t* buf = 0;
        if (e->size){
            buf = (uint8_t*)kmalloc(e->size);
            if (!buf) return -1;
            if (fat32_read_file(e, buf, e->size) != e->size){ kfree(buf); return -1; }
        }
        int r = fat32_write_file(ddir, dname, buf ? buf : (const uint8_t*)"", e->size);
        if (buf) kfree(buf);
        return r;
    }

    if (depth >= TREE_DEPTH) return -1;
    if (fat32_mkdir(ddir, dname) != 0) return -1;
    fat32_dirent_t nd;
    if (fat32_find(ddir, dname, &nd) != 0) return -1;

    // The folder just created may sit inside the one being copied (pasting a
    // folder into itself). Remember it at the top so the walk never descends
    // into its own output.
    if (!guard) guard = nd.first_cluster;

    uint32_t src = e->first_cluster ? e->first_cluster : fat32_root_cluster();
    fat32_dirent_t* list = (fat32_dirent_t*)kmalloc(sizeof(fat32_dirent_t) * LIST_MAX);
    if (!list) return -1;
    int n = fat32_list(src, list, LIST_MAX);
    int rc = 0;
    for (int i = 0; i < n; i++){
        if (is_dot(list[i].name) || is_dotdot(list[i].name)) continue;
        if (list[i].is_dir && list[i].first_cluster == guard) continue;
        if (copy_rec(src, &list[i], nd.first_cluster, list[i].name, guard, depth + 1) != 0) rc = -1;
    }
    kfree(list);
    return rc;
}

int fs_copy(uint32_t src_dir, const fat32_dirent_t* e, uint32_t dst_dir, const char* dst_name){
    return copy_rec(src_dir, e, dst_dir, dst_name, 0, 0);
}

// ----------------------------------------------------------------- delete

static int delete_rec(uint32_t dir, const fat32_dirent_t* e, int depth){
    if (!e->is_dir) return fat32_delete(dir, e->name);
    if (depth >= TREE_DEPTH) return -1;

    uint32_t cluster = e->first_cluster ? e->first_cluster : fat32_root_cluster();
    int failed = 0;

    // A listing holds at most LIST_MAX entries, so a bigger folder is emptied
    // in passes. Each pass either removes something or stops the loop.
    for (int pass = 0; pass < 64 && !failed; pass++){
        fat32_dirent_t* list = (fat32_dirent_t*)kmalloc(sizeof(fat32_dirent_t) * LIST_MAX);
        if (!list) return -1;
        int n = fat32_list(cluster, list, LIST_MAX);
        int any = 0;
        for (int i = 0; i < n; i++){
            if (is_dot(list[i].name) || is_dotdot(list[i].name)) continue;
            any = 1;
            if (delete_rec(cluster, &list[i], depth + 1) != 0) failed = 1;
        }
        kfree(list);
        if (!any) break;
    }
    if (failed) return -1;
    return fat32_rmdir(dir, e->name);
}

int fs_delete(uint32_t dir, const fat32_dirent_t* e){
    return delete_rec(dir, e, 0);
}

// ------------------------------------------------------------ containment

int fs_dir_inside(uint32_t ancestor, uint32_t dir){
    uint32_t root = fat32_root_cluster();
    if (!ancestor) ancestor = root;
    if (!dir) dir = root;
    for (int hops = 0; hops < 64; hops++){
        if (dir == ancestor) return 1;
        if (dir == root) return 0;
        fat32_dirent_t up;
        if (fat32_find(dir, "..", &up) != 0) return 0;
        dir = up.first_cluster ? up.first_cluster : root;
    }
    return 0;
}

// ------------------------------------------------------------------- text

static const char* const TEXT_EXT[] = {
    "TXT", "MD", "LOG", "CSV", "C", "H", "S", "ASM", "LD", "CFG", "INI", "CONF",
    "JSON", "XML", "HTM", "HTML", "CSS", "JS", "PY", "SH", "MK", "YML", "YAML",
    "TOML", "CPP", "HPP", "RS", "GO", "TEX", "BAT", "CMD", "SQL", "PS1", 0
};

int fs_is_text_name(const char* name){
    const char* dot = 0;
    for (const char* p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot || !dot[1]) return 0;
    for (int i = 0; TEXT_EXT[i]; i++)
        if (kstricmp(dot + 1, TEXT_EXT[i]) == 0) return 1;
    return 0;
}

int fs_looks_like_text(const uint8_t* data, uint32_t n){
    if (n == 0) return 1;
    uint32_t odd = 0;
    for (uint32_t i = 0; i < n; i++){
        uint8_t c = data[i];
        if (c == 0) return 0;
        if (c < 9 || (c > 13 && c < 32 && c != 27)) odd++;
    }
    return odd * 10 < n;
}
