// ext2.c — ext2 read-only driver (Phase 68).
//
// The volume lives at a fixed sector offset on the boot disk (default
// sector 81920, image built by ai/tools/mkfs_ext2.py) and all I/O goes
// through the block cache. Scope: superblock + group descriptors, inode
// table, directory walk, file read via direct + single-indirect blocks.
// rev 0 (128-byte inodes) and rev 1 (s_inode_size) both handled. Read-only.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ext2.h"
#include "bcache.h"

#define EXT2_MAGIC     0xEF53
#define EXT2_ROOT_INO  2
#define S_IFMT         0xF000
#define S_IFDIR        0x4000
#define S_IFREG        0x8000
#define EXT2_NDIR      12      // direct block pointers per inode

static struct {
    int      mounted;
    uint32_t base_lba;
    uint32_t block_size;        // 1024 << s_log_block_size (max 4096 here)
    uint32_t sectors_per_block;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;  // 1 for 1KB blocks, 0 otherwise
    uint32_t gd_block;          // block holding the group descriptor table
} g_e2;

static uint16_t erd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t erd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read one fs block into buf (must hold block_size bytes, max 4096).
static int read_block(uint32_t block, uint8_t *buf) {
    return bcache_read(g_e2.base_lba + block * g_e2.sectors_per_block,
                       (uint8_t)g_e2.sectors_per_block, buf);
}

int ext2_mount(uint32_t base_lba) {
    static uint8_t sb[1024];
    g_e2.mounted = 0;
    g_e2.base_lba = base_lba;
    // Superblock is always at byte offset 1024 = sector 2.
    if (bcache_read(base_lba + 2, 2, sb) < 0) return -1;
    if (erd16(&sb[56]) != EXT2_MAGIC) return -1;
    uint32_t log_bs = erd32(&sb[24]);
    if (log_bs > 2) return -1; // 1KB/2KB/4KB only
    g_e2.block_size        = 1024u << log_bs;
    g_e2.sectors_per_block = g_e2.block_size / 512;
    g_e2.inodes_per_group  = erd32(&sb[40]);
    g_e2.first_data_block  = erd32(&sb[20]);
    uint32_t rev = erd32(&sb[76]);
    g_e2.inode_size = (rev >= 1) ? erd16(&sb[88]) : 128;
    if (g_e2.inode_size < 128 || g_e2.inodes_per_group == 0) return -1;
    g_e2.gd_block = g_e2.first_data_block + 1;
    g_e2.mounted = 1;
    return 0;
}

int ext2_mounted(void) { return g_e2.mounted; }

// Load inode metadata: mode, size, and the 12 direct + 1 indirect pointers.
static int inode_load(uint32_t ino, uint16_t *mode, uint32_t *size,
                      uint32_t blocks[EXT2_NDIR + 1]) {
    static uint8_t blk[4096];
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / g_e2.inodes_per_group;
    uint32_t index = (ino - 1) % g_e2.inodes_per_group;
    // Group descriptor (32 bytes each); table may span blocks.
    uint32_t gd_per_block = g_e2.block_size / 32;
    if (read_block(g_e2.gd_block + group / gd_per_block, blk) < 0) return -1;
    uint32_t itable = erd32(&blk[(group % gd_per_block) * 32 + 8]);
    // Inode within the table.
    uint32_t byte_off = index * g_e2.inode_size;
    if (read_block(itable + byte_off / g_e2.block_size, blk) < 0) return -1;
    const uint8_t *in = &blk[byte_off % g_e2.block_size];
    *mode = erd16(&in[0]);
    *size = erd32(&in[4]);
    for (int i = 0; i <= EXT2_NDIR; i++) blocks[i] = erd32(&in[40 + i * 4]);
    return 0;
}

// Get the file-relative n-th data block number (direct + single indirect).
static uint32_t inode_nth_block(const uint32_t blocks[EXT2_NDIR + 1], uint32_t n) {
    static uint8_t ind[4096];
    static uint32_t cached_ind = 0;
    if (n < EXT2_NDIR) return blocks[n];
    n -= EXT2_NDIR;
    if (n >= g_e2.block_size / 4 || blocks[EXT2_NDIR] == 0) return 0;
    if (cached_ind != blocks[EXT2_NDIR]) {
        if (read_block(blocks[EXT2_NDIR], ind) < 0) return 0;
        cached_ind = blocks[EXT2_NDIR];
    }
    return erd32(&ind[n * 4]);
}

// Scan directory inode dir_ino for name; returns child ino or 0.
static uint32_t dir_find(uint32_t dir_ino, const char *name, int name_len) {
    static uint8_t blk[4096];
    uint16_t mode;
    uint32_t size, blocks[EXT2_NDIR + 1];
    if (inode_load(dir_ino, &mode, &size, blocks) < 0) return 0;
    if ((mode & S_IFMT) != S_IFDIR) return 0;
    for (uint32_t n = 0; n * g_e2.block_size < size; n++) {
        uint32_t b = inode_nth_block(blocks, n);
        if (b == 0 || read_block(b, blk) < 0) return 0;
        uint32_t off = 0;
        while (off + 8 <= g_e2.block_size) {
            uint32_t ino = erd32(&blk[off]);
            uint16_t rec_len = erd16(&blk[off + 4]);
            uint8_t  nlen = blk[off + 6]; // low byte works for rev0 + FILETYPE
            if (rec_len < 8) return 0;    // corrupt
            if (ino != 0 && nlen == name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++)
                    if ((char)blk[off + 8 + i] != name[i]) { match = 0; break; }
                if (match) return ino;
            }
            off += rec_len;
        }
    }
    return 0;
}

// Resolve an absolute path to an inode number (0 = not found).
static uint32_t resolve(const char *path) {
    if (!g_e2.mounted || !path || path[0] != '/') return 0;
    uint32_t ino = EXT2_ROOT_INO;
    int i = 1;
    while (path[i]) {
        int start = i;
        while (path[i] && path[i] != '/') i++;
        int len = i - start;
        while (path[i] == '/') i++;
        if (len == 0) continue;
        if (len > 255) return 0;
        ino = dir_find(ino, &path[start], len);
        if (ino == 0) return 0;
    }
    return ino;
}

int ext2_list(const char *path, Ext2ListCb cb, void *ud) {
    static uint8_t blk[4096];
    uint32_t ino = resolve(path);
    if (ino == 0) return -1;
    uint16_t mode;
    uint32_t size, blocks[EXT2_NDIR + 1];
    if (inode_load(ino, &mode, &size, blocks) < 0) return -1;
    if ((mode & S_IFMT) != S_IFDIR) return -1;
    int count = 0;
    for (uint32_t n = 0; n * g_e2.block_size < size; n++) {
        uint32_t b = inode_nth_block(blocks, n);
        if (b == 0 || read_block(b, blk) < 0) break;
        uint32_t off = 0;
        while (off + 8 <= g_e2.block_size) {
            uint32_t e_ino = erd32(&blk[off]);
            uint16_t rec_len = erd16(&blk[off + 4]);
            uint8_t  nlen = blk[off + 6];
            if (rec_len < 8) break;
            if (e_ino != 0 && nlen > 0) {
                char name[256];
                for (int i = 0; i < nlen; i++) name[i] = (char)blk[off + 8 + i];
                name[nlen] = '\0';
                uint16_t cmode;
                uint32_t csize, cblocks[EXT2_NDIR + 1];
                if (inode_load(e_ino, &cmode, &csize, cblocks) == 0 && cb)
                    cb(name, csize, (cmode & S_IFMT) == S_IFDIR ? 1 : 0, ud);
                count++;
            }
            off += rec_len;
        }
    }
    return count;
}

int ext2_stat(const char *path, uint32_t *out_size, int *out_is_dir) {
    uint32_t ino = resolve(path);
    if (ino == 0) return -1;
    uint16_t mode;
    uint32_t size, blocks[EXT2_NDIR + 1];
    if (inode_load(ino, &mode, &size, blocks) < 0) return -1;
    *out_size = size;
    *out_is_dir = (mode & S_IFMT) == S_IFDIR ? 1 : 0;
    return 0;
}

int ext2_read(const char *path, uint8_t *buf, uint32_t max_len) {
    static uint8_t blk[4096];
    uint32_t ino = resolve(path);
    if (ino == 0) return -1;
    uint16_t mode;
    uint32_t size, blocks[EXT2_NDIR + 1];
    if (inode_load(ino, &mode, &size, blocks) < 0) return -1;
    if ((mode & S_IFMT) != S_IFREG) return -1;
    uint32_t want = size < max_len ? size : max_len;
    uint32_t done = 0;
    for (uint32_t n = 0; done < want; n++) {
        uint32_t b = inode_nth_block(blocks, n);
        if (b == 0 || read_block(b, blk) < 0) return -1;
        uint32_t m = want - done < g_e2.block_size ? want - done : g_e2.block_size;
        for (uint32_t i = 0; i < m; i++) buf[done + i] = blk[i];
        done += m;
    }
    return (int)done;
}
