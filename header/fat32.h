#pragma once
#include <stdint.h>

#define FAT32_NAME_MAX 13   // "12345678.123" + NUL

typedef struct {
    char     name[FAT32_NAME_MAX];
    uint32_t size;
    uint32_t first_cluster;
    uint8_t  is_dir;
} fat32_dirent_t;

// Reads the boot sector off the ATA disk and parses the BPB. Returns 0 on
// success (a real FAT32 volume), -1 otherwise (no disk, or not FAT32).
int fat32_init(void);

uint32_t fat32_root_cluster(void);

// Lists directory `dir_cluster` into out[0..max). Returns entry count, or -1.
int fat32_list(uint32_t dir_cluster, fat32_dirent_t* out, int max);

// Finds `name` (8.3, case-insensitive) inside dir_cluster. 0 if found.
int fat32_find(uint32_t dir_cluster, const char* name, fat32_dirent_t* out);

// Reads up to buf_cap bytes of a file into buf. Returns bytes actually read.
uint32_t fat32_read_file(const fat32_dirent_t* f, uint8_t* buf, uint32_t buf_cap);

// ------------------------------------------------------------------ writing
//
// Whole-file writes only: there is no seek, no append and no partial update.
// That is the right shape for what this system does with a disk -- save a
// document, keep a downloaded file, persist a settings blob -- and it avoids
// the hardest part of a FAT writer, which is keeping a partially-rewritten
// cluster chain consistent if the machine stops halfway.
//
// The order of operations is chosen so an interrupted write loses the new
// contents rather than corrupting the volume: the data goes into freshly
// allocated clusters first, and only once all of it is on disk is the
// directory entry repointed at the new chain.

// Creates `name` in dir_cluster, or replaces its contents if it already
// exists. Returns 0 on success, -1 on failure (read-only disk, no free
// clusters, directory full and not extendable).
int fat32_write_file(uint32_t dir_cluster, const char* name,
                     const uint8_t* data, uint32_t len);

// Removes `name` and frees the clusters it held. Returns 0, or -1 if there
// is no such file.
int fat32_delete(uint32_t dir_cluster, const char* name);

// Creates an empty subdirectory, with its "." and ".." entries. Returns 0.
int fat32_mkdir(uint32_t dir_cluster, const char* name);

// Removes an empty subdirectory. Returns -1 if it does not exist, is not a
// directory, or still has anything in it besides "." and ".." -- recursive
// deletion is a policy decision that belongs to the caller, not the driver.
int fat32_rmdir(uint32_t dir_cluster, const char* name);

// True when a volume is mounted and the underlying disk accepted a write.
// Checked once, lazily, by writing a sector back to itself.
int fat32_writable(void);

// Free space, in clusters and bytes. Walks the FAT, so it is a scan rather
// than a lookup; the Files app calls it once per refresh, not per frame.
uint32_t fat32_free_clusters(void);
uint32_t fat32_bytes_per_cluster(void);
