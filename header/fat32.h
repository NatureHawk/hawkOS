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
