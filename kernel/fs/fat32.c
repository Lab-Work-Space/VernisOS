// fat32.c — FAT32 read-only driver (Phase 67).
//
// The volume lives at a fixed sector offset on the boot disk (default
// sector 8192, produced by mformat in the Makefile) and all I/O goes
// through the block cache. Scope: BPB parse, FAT chain walk, directory
// enumeration and file read with 8.3 names. LFN entries are skipped —
// files written as short names by mtools match fine. No write support.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fat32.h"
#include "bcache.h"

#define FAT32_EOC       0x0FFFFFF8u
#define DIRENT_SIZE     32
#define ATTR_DIRECTORY  0x10
#define ATTR_VOLUME_ID  0x08
#define ATTR_LFN        0x0F

static struct {
    int      mounted;
    uint32_t base_lba;       // volume start on disk
    uint32_t fat_start;      // sectors, relative to base
    uint32_t data_start;     // sectors, relative to base
    uint32_t root_cluster;
    uint32_t sectors_per_cluster;
    uint32_t total_clusters;
} g_vol;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_rel(uint32_t rel_sector, uint8_t *buf) {
    return bcache_read(g_vol.base_lba + rel_sector, 1, buf);
}

static uint32_t cluster_first_sector(uint32_t cluster) {
    return g_vol.data_start + (cluster - 2) * g_vol.sectors_per_cluster;
}

// Follow the FAT chain one hop. Returns next cluster, or >= FAT32_EOC.
static uint32_t fat_next(uint32_t cluster) {
    static uint8_t sec[512];
    uint32_t byte_off = cluster * 4;
    if (read_rel(g_vol.fat_start + byte_off / 512, sec) < 0) return FAT32_EOC;
    return rd32(&sec[byte_off % 512]) & 0x0FFFFFFF;
}

int fat32_mount(uint32_t base_lba) {
    static uint8_t bpb[512];
    g_vol.mounted = 0;
    g_vol.base_lba = base_lba;
    if (read_rel(0, bpb) < 0) return -1;
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) return -1;
    uint16_t bytes_per_sec = rd16(&bpb[11]);
    uint8_t  spc           = bpb[13];
    uint16_t rsvd          = rd16(&bpb[14]);
    uint8_t  nfats         = bpb[16];
    uint16_t root_ent16    = rd16(&bpb[17]);   // 0 on FAT32
    uint16_t fatsz16       = rd16(&bpb[22]);   // 0 on FAT32
    uint32_t fatsz32       = rd32(&bpb[36]);
    uint32_t root_cluster  = rd32(&bpb[44]);
    uint32_t total_sec     = rd16(&bpb[19]);
    if (total_sec == 0) total_sec = rd32(&bpb[32]);
    if (bytes_per_sec != 512 || spc == 0 || rsvd == 0 || nfats == 0) return -1;
    if (root_ent16 != 0 || fatsz16 != 0 || fatsz32 == 0) return -1; // not FAT32
    if (root_cluster < 2) return -1;
    g_vol.fat_start           = rsvd;
    g_vol.data_start          = rsvd + (uint32_t)nfats * fatsz32;
    g_vol.root_cluster        = root_cluster;
    g_vol.sectors_per_cluster = spc;
    g_vol.total_clusters      = (total_sec - g_vol.data_start) / spc;
    g_vol.mounted = 1;
    return 0;
}

int fat32_mounted(void) { return g_vol.mounted; }

// "HELLO.TXT" -> "HELLO   TXT" (the on-disk 11-byte form), uppercased.
// Returns 0 on success, -1 if the name doesn't fit 8.3.
static int name_to_83(const char *name, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    for (; name[i] && name[i] != '.'; i++, o++) {
        if (o >= 8) return -1;
        char c = name[i];
        out[o] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (name[i] == '.') {
        i++;
        for (int e = 0; name[i]; i++, e++) {
            if (e >= 3) return -1;
            char c = name[i];
            out[8 + e] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
    }
    return 0;
}

// On-disk 11-byte form -> dot-form ("HELLO   TXT" -> "HELLO.TXT").
static void name_from_83(const uint8_t *raw, char out[13]) {
    int o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[o++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[o++] = (char)raw[i];
    }
    out[o] = '\0';
}

// Scan the directory at dir_cluster for name (11-byte form). On hit fills
// *out_cluster/*out_size/*out_is_dir and returns 0; -1 if absent.
static int dir_find(uint32_t dir_cluster, const char target[11],
                    uint32_t *out_cluster, uint32_t *out_size, int *out_is_dir) {
    static uint8_t sec[512];
    uint32_t cl = dir_cluster;
    while (cl < FAT32_EOC && cl >= 2) {
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_rel(cluster_first_sector(cl) + s, sec) < 0) return -1;
            for (int e = 0; e < 512 / DIRENT_SIZE; e++) {
                const uint8_t *d = &sec[e * DIRENT_SIZE];
                if (d[0] == 0x00) return -1;           // end of directory
                if (d[0] == 0xE5) continue;            // deleted
                uint8_t attr = d[11];
                if ((attr & ATTR_LFN) == ATTR_LFN) continue;
                if (attr & ATTR_VOLUME_ID) continue;
                int match = 1;
                for (int i = 0; i < 11; i++)
                    if (d[i] != (uint8_t)target[i]) { match = 0; break; }
                if (!match) continue;
                *out_cluster = ((uint32_t)rd16(&d[20]) << 16) | rd16(&d[26]);
                *out_size    = rd32(&d[28]);
                *out_is_dir  = (attr & ATTR_DIRECTORY) ? 1 : 0;
                return 0;
            }
        }
        cl = fat_next(cl);
    }
    return -1;
}

// Resolve an absolute path to its directory entry. Root resolves to
// cluster=root, is_dir=1. Returns 0 on success.
static int resolve(const char *path, uint32_t *out_cluster,
                   uint32_t *out_size, int *out_is_dir) {
    if (!g_vol.mounted || !path || path[0] != '/') return -1;
    uint32_t cl = g_vol.root_cluster;
    uint32_t size = 0;
    int is_dir = 1;
    int i = 1;
    while (path[i]) {
        char comp[13];
        int c = 0;
        while (path[i] && path[i] != '/' && c < 12) comp[c++] = path[i++];
        if (path[i] && path[i] != '/') return -1; // component too long
        comp[c] = '\0';
        while (path[i] == '/') i++;
        if (c == 0) continue;
        if (!is_dir) return -1; // path descends through a file
        char t83[11];
        if (name_to_83(comp, t83) < 0) return -1;
        if (dir_find(cl, t83, &cl, &size, &is_dir) < 0) return -1;
    }
    *out_cluster = cl;
    *out_size = size;
    *out_is_dir = is_dir;
    return 0;
}

int fat32_list(const char *path, Fat32ListCb cb, void *ud) {
    static uint8_t sec[512];
    uint32_t cl, size;
    int is_dir;
    if (resolve(path, &cl, &size, &is_dir) < 0 || !is_dir) return -1;
    if (cl < 2) cl = g_vol.root_cluster; // ".." entries store cluster 0 for root
    int count = 0;
    while (cl < FAT32_EOC && cl >= 2) {
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_rel(cluster_first_sector(cl) + s, sec) < 0) return count;
            for (int e = 0; e < 512 / DIRENT_SIZE; e++) {
                const uint8_t *d = &sec[e * DIRENT_SIZE];
                if (d[0] == 0x00) return count;
                if (d[0] == 0xE5) continue;
                uint8_t attr = d[11];
                if ((attr & ATTR_LFN) == ATTR_LFN) continue;
                if (attr & ATTR_VOLUME_ID) continue;
                char name[13];
                name_from_83(d, name);
                if (cb) cb(name, rd32(&d[28]), (attr & ATTR_DIRECTORY) ? 1 : 0, ud);
                count++;
            }
        }
        cl = fat_next(cl);
    }
    return count;
}

int fat32_stat(const char *path, uint32_t *out_size, int *out_is_dir) {
    uint32_t cl, size;
    int is_dir;
    if (resolve(path, &cl, &size, &is_dir) < 0) return -1;
    *out_size = size;
    *out_is_dir = is_dir;
    return 0;
}

int fat32_read(const char *path, uint8_t *buf, uint32_t max_len) {
    static uint8_t sec[512];
    uint32_t cl, size;
    int is_dir;
    if (resolve(path, &cl, &size, &is_dir) < 0 || is_dir) return -1;
    uint32_t want = size < max_len ? size : max_len;
    uint32_t done = 0;
    while (done < want && cl >= 2 && cl < FAT32_EOC) {
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster && done < want; s++) {
            if (read_rel(cluster_first_sector(cl) + s, sec) < 0) return -1;
            uint32_t n = want - done < 512 ? want - done : 512;
            for (uint32_t i = 0; i < n; i++) buf[done + i] = sec[i];
            done += n;
        }
        cl = fat_next(cl);
    }
    return (int)done;
}
