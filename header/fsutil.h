#pragma once
#include <stdint.h>
#include "header/fat32.h"

// File operations built on the FAT32 driver, shared by the Files app and the
// editor. The driver itself does one thing per call -- write a file, remove an
// entry, make an empty directory -- and deliberately leaves policy to the
// caller. This is that policy: copying a folder means copying what is in it,
// deleting one means deleting what is in it first, and a paste that would
// overwrite something gets a new name instead.

// 1 if `name` can be created: non-empty, short enough, none of the characters
// FAT and every other filesystem in common use refuse, and not "." or "..".
int  fs_name_ok(const char* name);

// 1 and *out filled if `name` exists in `dir`, else 0. `out` may be null.
int  fs_exists(uint32_t dir, const char* name, fat32_dirent_t* out);

// Writes into `out` a name that does not exist in `dir`: `name` itself if it
// is free, otherwise "name - Copy.ext", "name - Copy (2).ext", and so on.
void fs_unique_name(uint32_t dir, const char* name, char* out, uint32_t cap);

// Copies a file, or a folder and everything under it, to dst_dir under
// dst_name. Returns 0, or -1 if anything failed (the disk is full or
// read-only, a file exceeds the copy limit, the tree is deeper than the
// recursion limit). A failed folder copy can leave part of the folder behind.
int  fs_copy(uint32_t src_dir, const fat32_dirent_t* e, uint32_t dst_dir, const char* dst_name);

// Deletes a file, or a folder and everything in it. Returns 0 or -1.
int  fs_delete(uint32_t dir, const fat32_dirent_t* e);

// 1 if `dir` is the folder whose first cluster is `ancestor`, or lies inside
// it. Moving a folder into itself would create a loop the driver cannot see,
// so callers ask this first.
int  fs_dir_inside(uint32_t ancestor, uint32_t dir);

// Whether a file should open in the text editor: by extension first, and for
// names with none (or an unfamiliar one), by looking at the first bytes.
int  fs_is_text_name(const char* name);
int  fs_looks_like_text(const uint8_t* data, uint32_t n);

// Largest single file fs_copy will move through memory.
#define FS_COPY_MAX (32u * 1024u * 1024u)
