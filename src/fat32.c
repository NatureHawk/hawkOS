// src/fat32.c — FAT32: BPB parsing, cluster-chain walking, directory
// traversal (root and subdirectories alike, since FAT32's root is itself just
// a cluster chain), whole-file writes, and VFAT long file names: LFN entries
// are folded into names on read, and created (with a generated 8.3 alias) on
// write, so names round-trip with Windows and Linux.
#include <stdint.h>
#include "header/fat32.h"
#include "header/ata.h"
#include "header/kstring.h"
#include "header/rtc.h"
#include "header/kprintf.h"

#define ATTR_LFN       0x0F
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20

#define LFN_MAX_ENTRIES 20      // 20 * 13 = 260 units, past the 255 FAT allows

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

// One-entry cache for fat32_read_at: "cluster number rc_idx of the chain that
// starts at rc_first is rc_cluster". Any FAT write clears it.
static uint32_t rc_first = 0, rc_idx = 0, rc_cluster = 0;

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
    rc_first = 0;
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

static void wr16(uint8_t* p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ======================================================= FAT (chain) layer

// Writes one FAT entry to every FAT on the volume. Updating only the first
// would leave the mirror stale, and the next machine to mount the volume is
// entitled to believe either copy.
static int fat_set(uint32_t cluster, uint32_t value){
    if (cluster < 2 || cluster >= count_of_clusters + 2) return -1;
    rc_first = 0;
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

// ================================================== directory cursor layer
//
// A directory is a cluster chain of 32-byte entries. A cursor names one
// entry as (cluster, sector-in-cluster, entry-in-sector); advancing it walks
// the chain, and can grow the directory by a zeroed cluster when asked to.

typedef struct { uint32_t cluster, s, e; } dcur_t;

typedef struct {
    dcur_t   cur;
    uint32_t lba_loaded;     // sector currently in sec[] (0 = none; sector 0 is never a directory)
    uint8_t  sec[512];
    int      done;
} diter_t;

static uint32_t dcur_lba(const dcur_t* c){ return cluster_to_lba(c->cluster) + c->s; }

static int zero_cluster(uint32_t c){
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    uint32_t lba = cluster_to_lba(c);
    for (uint32_t s = 0; s < sec_per_clus; s++)
        if (ata_write_sectors(lba + s, 1, zero) != 0) return -1;
    return 0;
}

static int dcur_adv(dcur_t* c, int extend){
    if (++c->e < 16) return 0;
    c->e = 0;
    if (++c->s < sec_per_clus) return 0;
    c->s = 0;
    uint32_t nxt = fat_next_cluster(c->cluster);
    if (nxt >= 2 && !fat_eoc(nxt)){ c->cluster = nxt; return 0; }
    if (!extend) return -1;

    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    if (fat_set(c->cluster, nc) != 0){ fat_set(nc, 0); return -1; }
    if (zero_cluster(nc) != 0) return -1;
    c->cluster = nc;
    return 0;
}

static void diter_start(diter_t* it, uint32_t dir){
    it->cur.cluster = dir; it->cur.s = 0; it->cur.e = 0;
    it->lba_loaded = 0;
    it->done = (dir < 2 || fat_eoc(dir)) ? 1 : 0;
}

static const uint8_t* diter_ent(diter_t* it){
    uint32_t lba = dcur_lba(&it->cur);
    if (it->lba_loaded != lba){
        if (ata_read_sectors(lba, 1, it->sec) != 0) return 0;
        it->lba_loaded = lba;
    }
    return it->sec + it->cur.e * 32u;
}

// Read-modify-write of one directory entry.
static int dent_write(const dcur_t* c, const uint8_t ent[32]){
    uint8_t sec[512];
    uint32_t lba = dcur_lba(c);
    if (ata_read_sectors(lba, 1, sec) != 0) return -1;
    memcpy(sec + c->e * 32u, ent, 32);
    return ata_write_sectors(lba, 1, sec) == 0 ? 0 : -1;
}

static int dent_mark_deleted(const dcur_t* c){
    uint8_t sec[512];
    uint32_t lba = dcur_lba(c);
    if (ata_read_sectors(lba, 1, sec) != 0) return -1;
    sec[c->e * 32u] = 0xE5;
    return ata_write_sectors(lba, 1, sec) == 0 ? 0 : -1;
}

// ============================================================== names

static uint8_t lfn_checksum(const uint8_t sfn[11]){
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

static char up(char c){ return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

// "NAME    EXT" -> "NAME.EXT", honouring the NT lower-case flags in byte 12
// that Linux and Windows set on names like "readme.txt".
static void format_name(const uint8_t* raw, char* out, int use_case_flags){
    uint8_t fl = use_case_flags ? raw[12] : 0;
    int oi = 0;
    for (int i = 0; i < 8; i++){
        if (raw[i] == ' ') break;
        char c = (char)raw[i];
        if ((fl & 0x08) && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[oi++] = c;
    }
    if (raw[8] != ' '){
        out[oi++] = '.';
        for (int i = 8; i < 11; i++){
            if (raw[i] == ' ') break;
            char c = (char)raw[i];
            if ((fl & 0x10) && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[oi++] = c;
        }
    }
    out[oi] = 0;
}

// Characters a name may not contain at all (LFN or 8.3).
static int name_valid(const char* n){
    size_t len = strlen(n);
    if (len == 0 || len > FAT32_NAME_LIMIT) return 0;
    if (n[len - 1] == '.' || n[len - 1] == ' ') return 0;
    if (n[0] == ' ') return 0;
    for (size_t i = 0; i < len; i++){
        unsigned char c = (unsigned char)n[i];
        if (c < 0x20 || c == 0x7F) return 0;
        if (strchr("\\/:*?\"<>|", c)) return 0;
    }
    return 1;
}

static int is_83_char(unsigned char c){
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c && strchr("!#$%&'()-@^_`{}~", c) != 0;
}

// Tries to express `name` as a plain 8.3 name. Returns 0 if it does not fit
// (too long, several dots, spaces, ...), else 1 with sfn[] filled, plus 2 when
// the name mixes upper and lower case -- meaning an 8.3 entry alone would lose
// the case and an LFN is needed to keep it.
static int name_to_83(const char* name, uint8_t sfn[11]){
    memset(sfn, ' ', 11);
    int i = 0, oi = 0, dots = 0, upper = 0, lower = 0;
    if (name[0] == '.') return 0;
    for (; name[i]; i++){
        unsigned char c = (unsigned char)name[i];
        if (c == '.'){
            if (++dots > 1) return 0;
            oi = 8;
            continue;
        }
        int lim = dots ? 11 : 8;
        if (oi >= lim) return 0;
        if (c >= 'a' && c <= 'z'){ lower = 1; c = (unsigned char)(c - 'a' + 'A'); }
        else if (c >= 'A' && c <= 'Z') upper = 1;
        if (!is_83_char(c)) return 0;
        sfn[oi++] = c;
    }
    if (dots && oi == 8) return 0;      // "name." with nothing after the dot
    return 1 | ((upper && lower) ? 2 : 0);
}

// ============================================================ scanning

typedef struct {
    uint8_t  raw[32];                 // the short (8.3) entry
    dcur_t   pos;                     // where it lives
    char     name[256];               // long name, or the formatted short name
    int      has_lfn;
    dcur_t   lfn_pos[LFN_MAX_ENTRIES];
    int      lfn_n;                   // LFN entries that belong to this entry
    uint16_t u[LFN_MAX_ENTRIES * 13 + 1];
} dent_t;

static const uint8_t lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

// Advances to the next live entry, folding any preceding LFN run into
// d->name. Returns 1 with *d filled, 0 at the end of the directory, -1 on a
// read error.
static int dir_scan(diter_t* it, dent_t* d){
    int nexp = 0, total = 0;          // nexp: ordinal of the last LFN piece taken (0 = none)
    uint8_t csum = 0;
    d->lfn_n = 0;

    for (;;){
        if (it->done) return 0;
        const uint8_t* e = diter_ent(it);
        if (!e) return -1;
        if (e[0] == 0x00){ it->done = 1; return 0; }

        dcur_t here = it->cur;
        uint8_t ent[32];
        memcpy(ent, e, 32);
        if (dcur_adv(&it->cur, 0) != 0) it->done = 1;   // last slot of the chain: still process it

        if (ent[0] == 0xE5){ nexp = 0; d->lfn_n = 0; continue; }

        if (ent[11] == ATTR_LFN){
            int ord = ent[0], seq = ord & 0x3F;
            if (ord & 0x40){
                d->lfn_n = 0;
                nexp = (seq >= 1 && seq <= LFN_MAX_ENTRIES) ? seq : 0;
                total = nexp;
                csum = ent[13];
            } else if (nexp >= 2 && seq == nexp - 1 && ent[13] == csum){
                nexp = seq;
            } else {
                nexp = 0;
            }
            if (nexp){
                for (int i = 0; i < 13; i++) d->u[(seq - 1) * 13 + i] = rd16(ent + lfn_off[i]);
                d->lfn_pos[d->lfn_n++] = here;
            }
            continue;
        }

        if (ent[11] & ATTR_VOLUME_ID){ nexp = 0; d->lfn_n = 0; continue; }

        memcpy(d->raw, ent, 32);
        d->pos = here;
        d->has_lfn = (nexp == 1 && d->lfn_n == total && csum == lfn_checksum(ent)) ? 1 : 0;
        if (d->has_lfn){
            int n = 0;
            for (int i = 0; i < total * 13 && n < 255; i++){
                uint16_t c = d->u[i];
                if (c == 0) break;
                d->name[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
            d->name[n] = 0;
        } else {
            d->lfn_n = 0;
            format_name(ent, d->name, 1);
        }
        return 1;
    }
}

static void fill_dirent(const dent_t* d, fat32_dirent_t* out){
    size_t n = strlen(d->name);
    if (n > FAT32_NAME_LIMIT) n = FAT32_NAME_LIMIT;
    memcpy(out->name, d->name, n);
    out->name[n] = 0;
    format_name(d->raw, out->alias, 0);
    out->size = rd32(d->raw + 28);
    out->first_cluster = ((uint32_t)rd16(d->raw + 20) << 16) | rd16(d->raw + 26);
    out->is_dir = (d->raw[11] & ATTR_DIRECTORY) ? 1 : 0;
}

int fat32_list(uint32_t dir_cluster, fat32_dirent_t* out, int max){
    if (!mounted) return -1;
    diter_t it; dent_t d;
    diter_start(&it, dir_cluster);
    int count = 0;
    while (count < max){
        int r = dir_scan(&it, &d);
        if (r < 0) return count;
        if (r == 0) break;
        fill_dirent(&d, &out[count++]);
    }
    return count;
}

int fat32_readdir(uint32_t dir_cluster, uint32_t index, fat32_dirent_t* out){
    if (!mounted) return -1;
    diter_t it; dent_t d;
    diter_start(&it, dir_cluster);
    for (;;){
        int r = dir_scan(&it, &d);
        if (r < 0) return -1;
        if (r == 0) return 0;
        if (index-- == 0){ fill_dirent(&d, out); return 1; }
    }
}

// 1 found (*d filled), 0 not there, -1 read error.
static int dir_find(uint32_t dir, const char* name, dent_t* d){
    diter_t it;
    diter_start(&it, dir);
    for (;;){
        int r = dir_scan(&it, d);
        if (r <= 0) return r;
        if (kstricmp(d->name, name) == 0) return 1;
        if (d->has_lfn){
            char alias[13];
            format_name(d->raw, alias, 0);
            if (kstricmp(alias, name) == 0) return 1;
        }
    }
}

// Is there already an entry whose 8.3 field is exactly sfn?
static int dir_alias_exists(uint32_t dir, const uint8_t sfn[11]){
    diter_t it; dent_t d;
    diter_start(&it, dir);
    for (;;){
        int r = dir_scan(&it, &d);
        if (r <= 0) return 0;
        if (memcmp(d.raw, sfn, 11) == 0) return 1;
    }
}

int fat32_find(uint32_t dir_cluster, const char* name, fat32_dirent_t* out){
    if (!mounted || !name || !name[0]) return -1;
    dent_t d;
    if (dir_find(dir_cluster, name, &d) != 1) return -1;
    fill_dirent(&d, out);
    return 0;
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

uint32_t fat32_read_at(const fat32_dirent_t* f, uint32_t off, uint8_t* buf, uint32_t len){
    if (!mounted || f->is_dir || f->first_cluster < 2 || off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;

    uint32_t bpc = (uint32_t)bytes_per_sec * sec_per_clus;
    uint32_t idx = off / bpc, co = off % bpc;
    uint32_t cluster = f->first_cluster, i = 0;
    if (rc_first == f->first_cluster && rc_idx <= idx && rc_cluster >= 2){
        cluster = rc_cluster; i = rc_idx;
    }
    for (; i < idx; i++){
        cluster = fat_next_cluster(cluster);
        if (cluster < 2 || fat_eoc(cluster)) return 0;
    }

    uint32_t done = 0;
    uint8_t sec[512];
    while (done < len && cluster >= 2 && !fat_eoc(cluster)){
        rc_first = f->first_cluster; rc_idx = idx; rc_cluster = cluster;
        uint32_t lba = cluster_to_lba(cluster);
        while (co < bpc && done < len){
            uint32_t s = co / 512u, so = co % 512u;
            uint32_t chunk = 512u - so;
            if (chunk > len - done) chunk = len - done;
            if (ata_read_sectors(lba + s, 1, sec) != 0) return done;
            memcpy(buf + done, sec + so, chunk);
            co += chunk; done += chunk;
        }
        if (done >= len) break;
        cluster = fat_next_cluster(cluster);
        idx++; co = 0;
    }
    return done;
}

// ============================================================== writing
//
// See header/fat32.h for the shape of the interface and why it is
// whole-file-only. The invariant everything below preserves: a directory
// entry never points at a chain that is not fully written, and a cluster is
// never in two chains at once.

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

// Finds `need` consecutive free directory slots (deleted entries, or the
// terminator and everything after it), growing the directory by a cluster
// when the chain runs out. Positions come back in order.
static int dir_alloc_slots(uint32_t dir, int need, dcur_t* out){
    diter_t it;
    diter_start(&it, dir);
    if (it.done) return -1;

    int run = 0, past_end = 0;
    uint32_t guard = (count_of_clusters + 2u) * sec_per_clus * 16u + 64u;
    while (guard--){
        int is_free = past_end;
        if (!is_free){
            const uint8_t* e = diter_ent(&it);
            if (!e) return -1;
            if (e[0] == 0x00){ past_end = 1; is_free = 1; }
            else if (e[0] == 0xE5) is_free = 1;
        }
        if (is_free){
            out[run++] = it.cur;
            if (run == need) return 0;
        } else {
            run = 0;
        }
        if (dcur_adv(&it.cur, 1) != 0) return -1;
    }
    return -1;
}

// Builds a numeric-tail alias ("ALONGF~1.TXT") for `name` that no entry in
// `dir` already uses.
static int gen_alias(uint32_t dir, const char* name, uint8_t sfn[11]){
    const char* dot = 0;
    for (const char* p = name; *p; p++) if (*p == '.' && p != name) dot = p;

    char base[9], ext[4];
    int bn = 0, en = 0;
    const char* end = dot ? dot : name + strlen(name);
    for (const char* p = name; p < end; p++){
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '.') continue;
        c = (unsigned char)up((char)c);
        if (!is_83_char(c)) c = '_';
        if (bn < 8) base[bn++] = (char)c;
    }
    if (dot){
        for (const char* p = dot + 1; *p; p++){
            unsigned char c = (unsigned char)*p;
            if (c == ' ') continue;
            c = (unsigned char)up((char)c);
            if (!is_83_char(c)) c = '_';
            if (en < 3) ext[en++] = (char)c;
        }
    }
    if (bn == 0) base[bn++] = '_';

    for (uint32_t n = 1; n < 1000000u; n++){
        char tail[10];
        int tl = 0;
        tail[tl++] = '~';
        char digits[8]; int dn = 0;
        for (uint32_t v = n; v; v /= 10u) digits[dn++] = (char)('0' + (v % 10u));
        while (dn) tail[tl++] = digits[--dn];

        int keep = 8 - tl;
        if (keep > bn) keep = bn;
        memset(sfn, ' ', 11);
        memcpy(sfn, base, (size_t)keep);
        memcpy(sfn + keep, tail, (size_t)tl);
        memcpy(sfn + 8, ext, (size_t)en);
        if (!dir_alias_exists(dir, sfn)) return 0;
    }
    return -1;
}

// Creates a directory entry for `name` (LFN entries first, the short entry
// last, so a half-written run is only orphan LFN entries, which readers
// ignore). `tmpl`, if given, is an existing short entry whose timestamps are
// carried over (used by rename).
static int dir_create(uint32_t dir, const char* name, uint8_t attr,
                      uint32_t first, uint32_t size, const uint8_t* tmpl){
    if (!name_valid(name)) return -1;
    size_t len = strlen(name);

    uint8_t sfn[11];
    int fit = name_to_83(name, sfn);
    int nlfn = 0;
    if (!fit || (fit & 2)){
        nlfn = (int)((len + 12) / 13);
        int plain = (fit != 0) && !dir_alias_exists(dir, sfn);   // "Readme.txt" -> README.TXT
        if (!plain && gen_alias(dir, name, sfn) != 0) return -1;
    }

    dcur_t slots[LFN_MAX_ENTRIES + 1];
    if (dir_alloc_slots(dir, nlfn + 1, slots) != 0) return -1;

    uint8_t csum = lfn_checksum(sfn);
    for (int k = 0; k < nlfn; k++){
        int ord = nlfn - k;                       // first slot holds the highest ordinal
        uint8_t e[32];
        memset(e, 0, sizeof(e));
        e[0] = (uint8_t)(ord | (ord == nlfn ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = csum;
        for (int i = 0; i < 13; i++){
            size_t pos = (size_t)(ord - 1) * 13 + (size_t)i;
            uint16_t c = pos < len ? (uint16_t)(unsigned char)name[pos]
                       : pos == len ? 0x0000 : 0xFFFF;
            wr16(e + lfn_off[i], c);
        }
        if (dent_write(&slots[k], e) != 0) return -1;
    }

    uint8_t e[32];
    uint16_t date, time;
    fat_now(&date, &time);
    memset(e, 0, sizeof(e));
    if (tmpl) memcpy(e + 12, tmpl + 12, 16);
    else {
        wr16(e + 14, time); wr16(e + 16, date); wr16(e + 18, date);
        wr16(e + 22, time); wr16(e + 24, date);
    }
    memcpy(e, sfn, 11);
    e[11] = attr;
    e[12] = 0;
    wr16(e + 20, (uint16_t)(first >> 16));
    wr16(e + 26, (uint16_t)(first & 0xFFFFu));
    wr32(e + 28, size);
    return dent_write(&slots[nlfn], e);
}

// Marks an entry and its LFN run deleted. The short entry goes first: once it
// is gone the LFN entries are orphans, which is a state readers already cope
// with, whereas the reverse order would briefly show a short-name-only file.
static int dir_remove(const dent_t* d){
    if (dent_mark_deleted(&d->pos) != 0) return -1;
    for (int i = 0; i < d->lfn_n; i++) dent_mark_deleted(&d->lfn_pos[i]);
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
    if (!name || !name[0] || !name_valid(name)) return -1;
    if (len && !data) return -1;

    dent_t d;
    int found = dir_find(dir_cluster, name, &d);
    if (found < 0) return -1;
    // Refuse to overwrite a directory with a file; the caller almost
    // certainly meant a different name.
    if (found == 1 && (d.raw[11] & ATTR_DIRECTORY)) return -1;

    // New contents first, into clusters nothing points at yet. Only when all
    // of it is on disk does the entry get repointed -- so an interrupted
    // write leaves the old file intact rather than a half-written one.
    uint32_t first = 0;
    if (len > 0){
        first = write_new_chain(data, len);
        if (!first) return -1;
    }

    if (found == 0){
        if (dir_create(dir_cluster, name, ATTR_ARCHIVE, first, len, 0) != 0){
            if (first) free_chain(first);
            return -1;
        }
        fsinfo_invalidate();
        return 0;
    }

    uint32_t old_first = ((uint32_t)rd16(d.raw + 20) << 16) | rd16(d.raw + 26);
    uint8_t sec[512];
    uint32_t lba = dcur_lba(&d.pos);
    if (ata_read_sectors(lba, 1, sec) != 0){
        if (first) free_chain(first);
        return -1;
    }

    uint16_t date, time;
    fat_now(&date, &time);

    uint8_t* e = sec + d.pos.e * 32u;
    e[11] = (uint8_t)((e[11] & ~ATTR_DIRECTORY) | ATTR_ARCHIVE);
    wr16(e + 18, date);                 // last access date
    wr16(e + 20, (uint16_t)(first >> 16));
    wr16(e + 22, time);                 // write time
    wr16(e + 24, date);                 // write date
    wr16(e + 26, (uint16_t)(first & 0xFFFFu));
    wr32(e + 28, len);

    if (ata_write_sectors(lba, 1, sec) != 0){
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
    if (!name || !name[0]) return -1;

    dent_t d;
    if (dir_find(dir_cluster, name, &d) != 1) return -1;
    if (d.raw[11] & ATTR_DIRECTORY) return -1;   // use a directory remove

    uint32_t first = ((uint32_t)rd16(d.raw + 20) << 16) | rd16(d.raw + 26);
    if (dir_remove(&d) != 0) return -1;

    if (first >= 2) free_chain(first);
    fsinfo_invalidate();
    return 0;
}

int fat32_rmdir(uint32_t dir_cluster, const char* name){
    if (!mounted || !fat32_writable()) return -1;
    if (!name || !name[0]) return -1;

    dent_t d;
    if (dir_find(dir_cluster, name, &d) != 1) return -1;
    if (!(d.raw[11] & ATTR_DIRECTORY)) return -1;
    uint32_t first = ((uint32_t)rd16(d.raw + 20) << 16) | rd16(d.raw + 26);
    if (first < 2) return -1;
    if (d.raw[0] == '.') return -1;

    // Refuse unless it holds nothing but "." and "..". Removing a populated
    // directory would orphan every chain inside it -- clusters marked in use
    // that nothing can ever reach again.
    fat32_dirent_t ents[4];
    int n = fat32_list(first, ents, 4);
    if (n != 2) return -1;

    free_chain(first);
    if (dir_remove(&d) != 0) return -1;

    fsinfo_invalidate();
    return 0;
}

int fat32_mkdir(uint32_t dir_cluster, const char* name){
    if (!mounted || !fat32_writable()) return -1;
    if (!name || !name_valid(name)) return -1;

    dent_t d;
    if (dir_find(dir_cluster, name, &d) != 0) return -1;   // already exists, or unreadable

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

    if (dir_create(dir_cluster, name, ATTR_DIRECTORY, nc, 0, 0) != 0){
        fat_set(nc, 0);
        return -1;
    }
    fsinfo_invalidate();
    return 0;
}

int fat32_rename(uint32_t old_dir, const char* oldname,
                 uint32_t new_dir, const char* newname){
    if (!mounted || !fat32_writable()) return -1;
    if (!oldname || !oldname[0] || !newname || !name_valid(newname)) return -1;

    dent_t d;
    if (dir_find(old_dir, oldname, &d) != 1) return -1;
    if (d.raw[0] == '.') return -1;                  // never move "." or ".."

    // Anything already at the destination blocks the rename -- except the
    // entry itself, for a change of case.
    if (!(old_dir == new_dir && kstricmp(d.name, newname) == 0)){
        dent_t other;
        if (dir_find(new_dir, newname, &other) != 0) return -1;
    }

    uint8_t attr = d.raw[11];
    uint32_t first = ((uint32_t)rd16(d.raw + 20) << 16) | rd16(d.raw + 26);
    uint32_t size  = rd32(d.raw + 28);
    if (dir_create(new_dir, newname, attr, first, size, d.raw) != 0) return -1;

    if ((attr & ATTR_DIRECTORY) && old_dir != new_dir && first >= 2){
        // A moved directory's ".." must name its new parent (0 for the root).
        uint8_t sec[512];
        uint32_t lba = cluster_to_lba(first);
        if (ata_read_sectors(lba, 1, sec) == 0 && sec[32] == '.' && sec[33] == '.'){
            uint32_t parent = (new_dir == root_clus) ? 0 : new_dir;
            wr16(sec + 32 + 20, (uint16_t)(parent >> 16));
            wr16(sec + 32 + 26, (uint16_t)(parent & 0xFFFFu));
            ata_write_sectors(lba, 1, sec);
        }
    }

    // The new entry is in place; drop the old one. Its position is still
    // valid: growing a directory appends clusters and never moves entries.
    dir_remove(&d);
    fsinfo_invalidate();
    return 0;
}
