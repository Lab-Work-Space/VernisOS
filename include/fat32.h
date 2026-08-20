// fat32.h — FAT32 read-only filesystem driver (Phase 67)
//
// Mounts a FAT32 volume located at a sector offset on the boot disk and
// reads it through the block cache. Short (8.3) names only; long-name
// entries are skipped. Read-only.
#ifndef VERNISOS_FAT32_H
#define VERNISOS_FAT32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Default location of the FAT32 volume inside os.img (see Makefile).
#define FAT32_DEFAULT_LBA 8192u

// Parse the BPB at base_lba and cache the volume geometry.
// Returns 0 on success, -1 if no valid FAT32 volume is found.
int fat32_mount(uint32_t base_lba);

int fat32_mounted(void);

// Enumerate a directory ("/", "/DIR", "/DIR/SUB"). cb is invoked per entry
// with the 8.3 name (dot-form, e.g. "HELLO.TXT"), file size and a dir flag.
// Returns entry count, or -1 (bad path / not mounted).
typedef void (*Fat32ListCb)(const char *name, uint32_t size, int is_dir, void *ud);
int fat32_list(const char *path, Fat32ListCb cb, void *ud);

// Read a file by absolute path (case-insensitive 8.3 match) into buf.
// Returns bytes read, or -1 if not found / not a file / not mounted.
int fat32_read(const char *path, uint8_t *buf, uint32_t max_len);

// Resolve a path to its size and dir flag without reading data.
// Returns 0 on success, -1 if not found / not mounted.
int fat32_stat(const char *path, uint32_t *out_size, int *out_is_dir);

#endif // VERNISOS_FAT32_H
