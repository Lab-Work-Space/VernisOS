// ext2.h — ext2 read-only filesystem driver (Phase 68)
//
// Mounts an ext2 volume at a sector offset on the boot disk, reading
// through the block cache. rev 0/1, block size 1024-4096, direct +
// single-indirect blocks. Read-only.
#ifndef VERNISOS_EXT2_H
#define VERNISOS_EXT2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Default location of the ext2 volume inside os.img (see Makefile).
#define EXT2_DEFAULT_LBA 81920u

int ext2_mount(uint32_t base_lba);
int ext2_mounted(void);

// Enumerate a directory. cb gets each entry's name, file size and dir flag.
// Returns entry count or -1.
typedef void (*Ext2ListCb)(const char *name, uint32_t size, int is_dir, void *ud);
int ext2_list(const char *path, Ext2ListCb cb, void *ud);

// Read a file by absolute path (case-sensitive, like real ext2).
// Returns bytes read or -1.
int ext2_read(const char *path, uint8_t *buf, uint32_t max_len);

// Resolve a path to its size and dir flag without reading data.
// Returns 0 on success, -1 if not found / not mounted.
int ext2_stat(const char *path, uint32_t *out_size, int *out_is_dir);

#endif // VERNISOS_EXT2_H
