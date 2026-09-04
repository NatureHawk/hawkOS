// src/fat32.c — FAT32: BPB parsing, cluster-chain walking, directory
// traversal (root and subdirectories alike, since FAT32's root is itself just
// a cluster chain), and whole-file writes. Long-filename (VFAT) entries are
// skipped on read and never created, so names are 8.3.
#include <stdint.h>
#include "header/fat32.h"
#include "header/ata.h"
#include "header/kstring.h"
#include "header/rtc.h"
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
static uint32_t count_of_clusters;   // data clusters, so valid numbers are 2 .. count+1
static uint16_t fsinfo_sec;
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
    fsinfo_sec        = rd16(sec + 48);

    // The cluster count bounds every allocation and every chain walk, so a
    // volume that does not report a size is not one to write to.
    uint32_t tot16 = rd16(sec + 19);
    uint32_t tot_sec = tot16 ? tot16 : rd32(sec + 32);
    count_of_clusters = (tot_sec > first_data_sector)
                      ? (tot_sec - first_data_sector) / sec_per_clus : 0;

    mounted = 1;
    kprintf("[fat32] mounted: %u B/sector, %u sectors/cluster, root cluster=%u, %u clusters\n",
            bytes_per_sec, sec_per_clus, root_clus, count_of_clusters);
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

// ============================================================== writing
//
// See header/fat32.h for the shape of the interface and why it is
// whole-file-only. The invariant everything below preserves: a directory
// entry never points at a chain that is not fully written, and a cluster is
// never in two chains at once.

static void wr16(uint8_t* p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Writes one FAT entry to every FAT on the volume. Updating only the first
// would leave the mirror stale, and the next machine to mount the volume is
// entitled to believe either copy.
static int fat_set(uint32_t cluster, uint32_t value){
    if (cluster < 2 || cluster >= count_of_clusters + 2) return -1;
    uint32_t fat_offset = cluster * 4u;
    uint32_t sec_in_fat = fat_offset / bytes_per_sec;
    uint32_t ent_off    = fat_offset % bytes_per_sec;

    uint8_t sec[512];
    for (uint8_t f = 0; f < num_fats; f++){
        uint32_t lba = rsvd_sec_cnt + (uint32_t)f * fat_sz32 + sec_in_fat;
        if (ata_read_sectors(lba, 1, sec) != 0) return -1;
        // The top four bits of a FAT32 entry are reserved and must be kept.
        uint32_t old = rd32(sec + ent_off);
        wr32(sec + ent_off, (old & 0xF0000000u) | (value & 0x0FFFFFFFu));
        if (ata_write_sectors(lba, 1, sec) != 0) return -1;
    }
    return 0;
}

// Finds a free cluster and claims it as a one-cluster chain. Scanning a whole
// FAT sector per read rather than probing one entry at a time matters: at one
// sector per cluster a 64 MB volume has ~130k entries, and a read each would
// make every file creation take minutes.
static uint32_t fat_alloc(void){
    uint8_t sec[512];
    uint32_t ents_per_sec = bytes_per_sec / 4u;
    uint32_t last = count_of_clusters + 2u;

    for (uint32_t c0 = 0; c0 < last; c0 += ents_per_sec){
        uint32_t lba = rsvd_sec_cnt + (c0 / ents_per_sec);
        if (ata_read_sectors(lba, 1, sec) != 0) return 0;
        for (uint32_t e = 0; e < ents_per_sec; e++){
            uint32_t c = c0 + e;
            if (c < 2) continue;
            if (c >= last) break;
            if ((rd32(sec + e * 4u) & 0x0FFFFFFFu) == 0){
                if (fat_set(c, 0x0FFFFFFFu) != 0) return 0;
                return c;
            }
        }
    }
    return 0;   // volume full
}

static void free_chain(uint32_t cluster){
    // Bounded by the cluster count so a corrupt loop in the FAT cannot spin
    // here forever.
    uint32_t guard = count_of_clusters + 2u;
    while (cluster >= 2 && !fat_eoc(cluster) && guard--){
        uint32_t next = fat_next_cluster(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

uint32_t fat32_bytes_per_cluster(void){
    return (uint32_t)bytes_per_sec * sec_per_clus;
}

uint32_t fat32_free_clusters(void){
    if (!mounted) return 0;
    uint8_t sec[512];
    uint32_t ents_per_sec = bytes_per_sec / 4u;
    uint32_t last = count_of_clusters + 2u;
    uint32_t free_n = 0;

    for (uint32_t c0 = 0; c0 < last; c0 += ents_per_sec){
        uint32_t lba = rsvd_sec_cnt + (c0 / ents_per_sec);
        if (ata_read_sectors(lba, 1, sec) != 0) break;
        for (uint32_t e = 0; e < ents_per_sec; e++){
            uint32_t c = c0 + e;
            if (c < 2) continue;
            if (c >= last) break;
            if ((rd32(sec + e * 4u) & 0x0FFFFFFFu) == 0) free_n++;
        }
    }
    return free_n;
}

// FSInfo carries a cached free-cluster count and an allocation hint. Keeping
// them accurate through every operation is more bookkeeping than it is worth
// here, so they are marked unknown instead -- which the spec explicitly
// allows and which makes the next driver to mount the volume recompute them
// rather than trust a stale number.
static void fsinfo_invalidate(void){
    if (!fsinfo_sec) return;
    uint8_t sec[512];
    if (ata_read_sectors(fsinfo_sec, 1, sec) != 0) return;
    if (rd32(sec) != 0x41615252u) return;          // "RRaA" lead signature
    if (rd32(sec + 484) != 0x61417272u) return;    // "rrAa" struct signature
    wr32(sec + 488, 0xFFFFFFFFu);                  // free count: unknown
    wr32(sec + 492, 0xFFFFFFFFu);                  // next free hint: unknown
    ata_write_sectors(fsinfo_sec, 1, sec);
}

// Packs the current wall clock into the FAT date/time encoding, so files made
// here have sensible timestamps when the image is opened on a real machine.
static void fat_now(uint16_t* date, uint16_t* time){
    rtc_time_t t;
    rtc_read(&t);
    int year = (int)t.year - 1980;
    if (year < 0)   year = 0;
    if (year > 127) year = 127;
    *date = (uint16_t)((year << 9) | ((t.month & 0x0F) << 5) | (t.day & 0x1F));
    *time = (uint16_t)(((t.hour & 0x1F) << 11) | ((t.min & 0x3F) << 5) | ((t.sec / 2) & 0x1F));
}

// Locates `want` in a directory. With make_slot set, a name that is not there
// yields a free entry to create it in -- extending the directory by a cluster
// if every slot is taken.
//
// Returns 1 for an existing entry, 0 for a fresh slot, -1 for not-found
// (make_slot clear) or out of space.
static int dir_locate(uint32_t dir_cluster, const uint8_t want[11], int make_slot,
                      uint32_t* out_lba, uint32_t* out_off, uint32_t* out_first){
    uint8_t sec[512];
    uint32_t cluster = dir_cluster;
    uint32_t free_lba = 0, free_off = 0;
    int have_free = 0;
    uint32_t last_cluster = dir_cluster;
    uint32_t guard = count_of_clusters + 2u;

    while (cluster >= 2 && !fat_eoc(cluster) && guard--){
        last_cluster = cluster;
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sec_per_clus; s++){
            if (ata_read_sectors(lba + s, 1, sec) != 0) return -1;
            for (int e = 0; e < 16; e++){
                const uint8_t* ent = sec + e * 32;

                if (ent[0] == 0x00){
                    // End of the directory. Nothing past here is in use, so
                    // this slot is the natural place for a new entry -- and
                    // the zero stays as the terminator for the next one.
                    if (!have_free){
                        free_lba = lba + s; free_off = (uint32_t)e * 32u; have_free = 1;
                    }
                    goto finished;
                }
                if (ent[0] == 0xE5){
                    if (!have_free){
                        free_lba = lba + s; free_off = (uint32_t)e * 32u; have_free = 1;
                    }
                    continue;
                }
                if (ent[11] == ATTR_LFN) continue;
                if (ent[11] & ATTR_VOLUME_ID) continue;

                int match = 1;
                for (int i = 0; i < 11; i++) if (ent[i] != want[i]) { match = 0; break; }
                if (match){
                    *out_lba = lba + s;
                    *out_off = (uint32_t)e * 32u;
                    *out_first = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
                    return 1;
                }
            }
        }
        cluster = fat_next_cluster(cluster);
    }

finished:
    if (!make_slot) return -1;
    if (have_free){
        *out_lba = free_lba; *out_off = free_off; *out_first = 0;
        return 0;
    }

    // Every slot in every cluster is taken: grow the directory.
    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    if (fat_set(last_cluster, nc) != 0){ fat_set(nc, 0); return -1; }

    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    uint32_t nlba = cluster_to_lba(nc);
    for (uint32_t s = 0; s < sec_per_clus; s++)
        if (ata_write_sectors(nlba + s, 1, zero) != 0) return -1;

    *out_lba = nlba; *out_off = 0; *out_first = 0;
    return 0;
}

// Writes `len` bytes into a newly allocated chain and returns its first
// cluster, or 0 on failure -- having released anything it managed to claim,
// so a full disk does not leak clusters on every attempt.
static uint32_t write_new_chain(const uint8_t* data, uint32_t len){
    uint32_t first = 0, prev = 0, off = 0;
    uint8_t sec[512];

    while (off < len){
        uint32_t c = fat_alloc();
        if (!c) goto fail;
        if (prev){
            if (fat_set(prev, c) != 0){ fat_set(c, 0); goto fail; }
        } else {
            first = c;
        }
        prev = c;

        uint32_t lba = cluster_to_lba(c);
        for (uint32_t s = 0; s < sec_per_clus && off < len; s++){
            uint32_t chunk = len - off;
            if (chunk > bytes_per_sec) chunk = bytes_per_sec;
            // Zero the tail of the last sector rather than leaving whatever
            // the cluster held before: that would hand the previous file's
            // contents to anyone who reads past the recorded length.
            memset(sec, 0, sizeof(sec));
            memcpy(sec, data + off, chunk);
            if (ata_write_sectors(lba + s, 1, sec) != 0) goto fail;
            off += chunk;
        }
    }
    return first;

fail:
    if (first) free_chain(first);
    return 0;
}

static int write_probe_done = 0;
static int write_probe_ok   = 0;

int fat32_writable(void){
    if (!mounted) return 0;
    if (write_probe_done) return write_probe_ok;
    write_probe_done = 1;

    // Read a sector and write it straight back. It changes nothing, and it is
    // the only way to find out whether the image underneath is writable --
    // QEMU silently refuses writes on a read-only drive rather than failing
    // the bus.
    uint8_t sec[512];
    uint32_t probe = rsvd_sec_cnt;                  // first FAT sector
    if (ata_read_sectors(probe, 1, sec) != 0) return 0;
    write_probe_ok = (ata_write_sectors(probe, 1, sec) == 0);
    return write_probe_ok;
}

int fat32_write_file(uint32_t dir_cluster, const char* name,
                     const uint8_t* data, uint32_t len){
    if (!mounted || !fat32_writable()) return -1;
    if (!name || !name[0]) return -1;
    if (len && !data) return -1;

    uint8_t want[11];
    to_fat_name(name, want);

    uint32_t ent_lba = 0, ent_off = 0, old_first = 0;
    int found = dir_locate(dir_cluster, want, 1, &ent_lba, &ent_off, &old_first);
    if (found < 0) return -1;
    if (found == 1){
        // Refuse to overwrite a directory with a file; the caller almost
        // certainly meant a different name.
        uint8_t sec[512];
        if (ata_read_sectors(ent_lba, 1, sec) != 0) return -1;
        if (sec[ent_off + 11] & ATTR_DIRECTORY) return -1;
    }

    // New contents first, into clusters nothing points at yet. Only when all
    // of it is on disk does the entry get repointed -- so an interrupted
    // write leaves the old file intact rather than a half-written one.
    uint32_t first = 0;
    if (len > 0){
        first = write_new_chain(data, len);
        if (!first) return -1;
    }

    uint8_t sec[512];
    if (ata_read_sectors(ent_lba, 1, sec) != 0){
        if (first) free_chain(first);
        return -1;
    }

    uint16_t date, time;
    fat_now(&date, &time);

    uint8_t* e = sec + ent_off;
    memset(e, 0, 32);
    memcpy(e, want, 11);
    e[11] = 0x20;                       // ATTR_ARCHIVE
    wr16(e + 14, time);                 // creation time
    wr16(e + 16, date);                 // creation date
    wr16(e + 18, date);                 // last access date
    wr16(e + 20, (uint16_t)(first >> 16));
    wr16(e + 22, time);                 // write time
    wr16(e + 24, date);                 // write date
    wr16(e + 26, (uint16_t)(first & 0xFFFFu));
    wr32(e + 28, len);

    if (ata_write_sectors(ent_lba, 1, sec) != 0){
        if (first) free_chain(first);
        return -1;
    }

    // Only now is the old chain unreachable, so this is the safe moment to
    // release it.
    if (old_first >= 2) free_chain(old_first);
    fsinfo_invalidate();
    return 0;
}

int fat32_delete(uint32_t dir_cluster, const char* name){
    if (!mounted || !fat32_writable()) return -1;

    uint8_t want[11];
    to_fat_name(name, want);

    uint32_t lba = 0, off = 0, first = 0;
    if (dir_locate(dir_cluster, want, 0, &lba, &off, &first) != 1) return -1;

    uint8_t sec[512];
    if (ata_read_sectors(lba, 1, sec) != 0) return -1;
    if (sec[off + 11] & ATTR_DIRECTORY) return -1;   // use a directory remove

    sec[off] = 0xE5;
    if (ata_write_sectors(lba, 1, sec) != 0) return -1;

    if (first >= 2) free_chain(first);
    fsinfo_invalidate();
    return 0;
}

int fat32_rmdir(uint32_t dir_cluster, const char* name){
    if (!mounted || !fat32_writable()) return -1;

    uint8_t want[11];
    to_fat_name(name, want);

    uint32_t lba = 0, off = 0, first = 0;
    if (dir_locate(dir_cluster, want, 0, &lba, &off, &first) != 1) return -1;

    uint8_t sec[512];
    if (ata_read_sectors(lba, 1, sec) != 0) return -1;
    if (!(sec[off + 11] & ATTR_DIRECTORY)) return -1;
    if (first < 2) return -1;

    // Refuse unless it holds nothing but "." and "..". Removing a populated
    // directory would orphan every chain inside it -- clusters marked in use
    // that nothing can ever reach again.
    fat32_dirent_t ents[4];
    int n = fat32_list(first, ents, 4);
    if (n != 2) return -1;

    free_chain(first);

    if (ata_read_sectors(lba, 1, sec) != 0) return -1;
    sec[off] = 0xE5;
    if (ata_write_sectors(lba, 1, sec) != 0) return -1;

    fsinfo_invalidate();
    return 0;
}

int fat32_mkdir(uint32_t dir_cluster, const char* name){
    if (!mounted || !fat32_writable()) return -1;

    uint8_t want[11];
    to_fat_name(name, want);

    uint32_t ent_lba = 0, ent_off = 0, old_first = 0;
    if (dir_locate(dir_cluster, want, 1, &ent_lba, &ent_off, &old_first) != 0)
        return -1;                                   // already exists, or full

    uint32_t nc = fat_alloc();
    if (!nc) return -1;

    uint16_t date, time;
    fat_now(&date, &time);

    // A directory's first cluster holds "." and ".." and nothing else. ".."
    // stores 0 for the root rather than the root's cluster number, which is
    // what the format requires and what every other driver expects to see.
    uint8_t sec[512];
    memset(sec, 0, sizeof(sec));
    for (int i = 0; i < 11; i++){ sec[i] = ' '; sec[32 + i] = ' '; }
    sec[0] = '.';
    sec[32] = '.'; sec[33] = '.';
    sec[11] = ATTR_DIRECTORY;
    sec[32 + 11] = ATTR_DIRECTORY;
    wr16(sec + 20, (uint16_t)(nc >> 16));
    wr16(sec + 26, (uint16_t)(nc & 0xFFFFu));
    uint32_t parent = (dir_cluster == root_clus) ? 0 : dir_cluster;
    wr16(sec + 32 + 20, (uint16_t)(parent >> 16));
    wr16(sec + 32 + 26, (uint16_t)(parent & 0xFFFFu));
    wr16(sec + 22, time); wr16(sec + 24, date);
    wr16(sec + 32 + 22, time); wr16(sec + 32 + 24, date);

    uint32_t nlba = cluster_to_lba(nc);
    if (ata_write_sectors(nlba, 1, sec) != 0){ fat_set(nc, 0); return -1; }

    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    for (uint32_t s = 1; s < sec_per_clus; s++)
        if (ata_write_sectors(nlba + s, 1, zero) != 0){ fat_set(nc, 0); return -1; }

    if (ata_read_sectors(ent_lba, 1, sec) != 0){ fat_set(nc, 0); return -1; }
    uint8_t* e = sec + ent_off;
    memset(e, 0, 32);
    memcpy(e, want, 11);
    e[11] = ATTR_DIRECTORY;
    wr16(e + 14, time); wr16(e + 16, date); wr16(e + 18, date);
    wr16(e + 20, (uint16_t)(nc >> 16));
    wr16(e + 22, time); wr16(e + 24, date);
    wr16(e + 26, (uint16_t)(nc & 0xFFFFu));
    wr32(e + 28, 0);                                 // directories record size 0

    if (ata_write_sectors(ent_lba, 1, sec) != 0){ fat_set(nc, 0); return -1; }
    fsinfo_invalidate();
    return 0;
}
