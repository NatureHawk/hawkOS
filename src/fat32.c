// src/fat32.c — read-only FAT32: BPB parsing, cluster-chain walking,
// directory traversal (root and subdirectories alike, since FAT32's root
// is itself just a cluster chain), long-filename (VFAT) entries skipped.
// No write support — see header/fat32.h.
#include <stdint.h>
#include "header/fat32.h"
#include "header/ata.h"
#include "header/kprintf.h"

#define ATTR_LFN       0x0F
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10

static uint16_t bytes_per_sec;
static uint8_t  sec_per_clus;
static uint16_t rsvd_sec_cnt;
static uint8_t  num_fats;
static uint32_t fat_sz32;
static uint32_t root_clus;
static uint32_t first_data_sector;
static int      mounted = 0;

static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int fat_eoc(uint32_t c){ return c >= 0x0FFFFFF8u; }

static inline uint32_t cluster_to_lba(uint32_t cluster){
    return first_data_sector + (cluster - 2u) * sec_per_clus;
}

static uint32_t fat_next_cluster(uint32_t cluster){
    uint32_t fat_offset = cluster * 4u;
    uint32_t fat_sector = rsvd_sec_cnt + (fat_offset / bytes_per_sec);
    uint32_t ent_off    = fat_offset % bytes_per_sec;
    uint8_t sec[512];
    if (ata_read_sectors(fat_sector, 1, sec) != 0) return 0x0FFFFFFFu;
    return rd32(sec + ent_off) & 0x0FFFFFFFu;
}

int fat32_init(void){
    uint8_t sec[512];
    if (ata_read_sectors(0, 1, sec) != 0) {
        kprintf("[fat32] disk read failed (no disk attached?)\n");
        return -1;
    }
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
        kprintf("[fat32] no boot sector signature\n");
        return -1;
    }

    bytes_per_sec = rd16(sec + 11);
    sec_per_clus  = sec[13];
    rsvd_sec_cnt  = rd16(sec + 14);
    num_fats      = sec[16];
    fat_sz32      = rd32(sec + 36);
    root_clus     = rd32(sec + 44);

    if (bytes_per_sec != 512 || fat_sz32 == 0 || sec_per_clus == 0) {
        kprintf("[fat32] not a FAT32 volume\n");
        return -1;
    }

    first_data_sector = rsvd_sec_cnt + (uint32_t)num_fats * fat_sz32;
    mounted = 1;
    kprintf("[fat32] mounted: %u B/sector, %u sectors/cluster, root cluster=%u\n",
            bytes_per_sec, sec_per_clus, root_clus);
    return 0;
}

uint32_t fat32_root_cluster(void){ return root_clus; }

static void format_name(const uint8_t* raw, char* out){
    int oi = 0;
    for (int i = 0; i < 8; i++){ if (raw[i] == ' ') break; out[oi++] = (char)raw[i]; }
    if (raw[8] != ' '){
        out[oi++] = '.';
        for (int i = 8; i < 11; i++){ if (raw[i] == ' ') break; out[oi++] = (char)raw[i]; }
    }
    out[oi] = 0;
}

static void to_fat_name(const char* in, uint8_t out[11]){
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, oi = 0;
    while (in[i] && in[i] != '.' && oi < 8){
        char c = in[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[oi++] = (uint8_t)c;
    }
    while (in[i] && in[i] != '.') i++;
    if (in[i] == '.') i++;
    oi = 8;
    while (in[i] && oi < 11){
        char c = in[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[oi++] = (uint8_t)c;
    }
}

static void fill_dirent(const uint8_t* ent, fat32_dirent_t* out){
    format_name(ent, out->name);
    out->size = rd32(ent + 28);
    uint32_t hi = rd16(ent + 20), lo = rd16(ent + 26);
    out->first_cluster = (hi << 16) | lo;
    out->is_dir = (ent[11] & ATTR_DIRECTORY) ? 1 : 0;
}

int fat32_list(uint32_t dir_cluster, fat32_dirent_t* out, int max){
    if (!mounted) return -1;
    int count = 0;
    uint32_t cluster = dir_cluster;
    uint8_t sec[512];
    while (!fat_eoc(cluster) && cluster >= 2){
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sec_per_clus; s++){
            if (ata_read_sectors(lba + s, 1, sec) != 0) return count;
            for (int e = 0; e < 16; e++){
                const uint8_t* ent = sec + e * 32;
                if (ent[0] == 0x00) return count;
                if (ent[0] == 0xE5) continue;
                uint8_t attr = ent[11];
                if (attr == ATTR_LFN) continue;
                if (attr & ATTR_VOLUME_ID) continue;
                if (count >= max) return count;
                fill_dirent(ent, &out[count]);
                count++;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return count;
}

int fat32_find(uint32_t dir_cluster, const char* name, fat32_dirent_t* out){
    if (!mounted) return -1;
    uint8_t want[11];
    to_fat_name(name, want);
    uint32_t cluster = dir_cluster;
    uint8_t sec[512];
    while (!fat_eoc(cluster) && cluster >= 2){
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sec_per_clus; s++){
            if (ata_read_sectors(lba + s, 1, sec) != 0) return -1;
            for (int e = 0; e < 16; e++){
                const uint8_t* ent = sec + e * 32;
                if (ent[0] == 0x00) return -1;
                if (ent[0] == 0xE5) continue;
                uint8_t attr = ent[11];
                if (attr == ATTR_LFN) continue;
                if (attr & ATTR_VOLUME_ID) continue;
                int match = 1;
                for (int i = 0; i < 11; i++) if (ent[i] != want[i]) { match = 0; break; }
                if (match){ fill_dirent(ent, out); return 0; }
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return -1;
}

uint32_t fat32_read_file(const fat32_dirent_t* f, uint8_t* buf, uint32_t buf_cap){
    if (!mounted || f->is_dir) return 0;
    uint32_t remaining = f->size < buf_cap ? f->size : buf_cap;
    uint32_t written = 0;
    uint32_t cluster = f->first_cluster;
    uint8_t sec[512];
    while (!fat_eoc(cluster) && cluster >= 2 && written < remaining){
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sec_per_clus && written < remaining; s++){
            if (ata_read_sectors(lba + s, 1, sec) != 0) return written;
            uint32_t chunk = remaining - written;
            if (chunk > 512u) chunk = 512u;
            for (uint32_t i = 0; i < chunk; i++) buf[written + i] = sec[i];
            written += chunk;
        }
        cluster = fat_next_cluster(cluster);
    }
    return written;
}
