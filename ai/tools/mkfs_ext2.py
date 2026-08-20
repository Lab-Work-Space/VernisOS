#!/usr/bin/env python3
"""mkfs_ext2.py — build a minimal ext2 (rev 0) image for the VernisOS driver.

Single block group, 1KB blocks, 128-byte inodes, direct blocks only
(files up to 12KB). Enough to exercise kernel/fs/ext2.c: superblock parse,
group descriptor, inode table, directory walk and file read.

Usage:
    python3 ai/tools/mkfs_ext2.py -o make/ext2.img --size-mb 4 \
        --add hello.txt=/HELLO.TXT --add nested.txt=/SUBDIR/NESTED.TXT
"""
import argparse
import os
import struct

BLOCK_SIZE = 1024
INODE_SIZE = 128
INODES_COUNT = 64
FIRST_INO = 11          # first non-reserved inode (rev 0 fixed)
ROOT_INO = 2
EXT2_MAGIC = 0xEF53

S_IFREG = 0x8000
S_IFDIR = 0x4000


class Ext2Builder:
    def __init__(self, size_mb):
        self.blocks_count = size_mb * 1024 * 1024 // BLOCK_SIZE
        self.img = bytearray(self.blocks_count * BLOCK_SIZE)
        # Layout: block 0 boot, 1 superblock, 2 group desc, 3 block bitmap,
        # 4 inode bitmap, 5.. inode table, then data.
        self.inode_table_blocks = INODES_COUNT * INODE_SIZE // BLOCK_SIZE
        self.first_data_block_no = 5 + self.inode_table_blocks
        self.next_block = self.first_data_block_no
        self.next_ino = FIRST_INO
        self.inodes = {}          # ino -> (mode, size, [blocks])
        self.dirs = {}            # ino -> list of (name, ino)
        self.mkdir_root()

    def alloc_block(self):
        b = self.next_block
        self.next_block += 1
        if b >= self.blocks_count:
            raise SystemExit("mkfs_ext2: image full")
        return b

    def mkdir_root(self):
        self.inodes[ROOT_INO] = [S_IFDIR | 0o755, 0, []]
        self.dirs[ROOT_INO] = [(".", ROOT_INO), ("..", ROOT_INO)]

    def mkdir(self, parent_ino, name):
        ino = self.next_ino
        self.next_ino += 1
        self.inodes[ino] = [S_IFDIR | 0o755, 0, []]
        self.dirs[ino] = [(".", ino), ("..", parent_ino)]
        self.dirs[parent_ino].append((name, ino))
        return ino

    def add_file(self, parent_ino, name, data):
        ino = self.next_ino
        self.next_ino += 1
        blocks = []
        for off in range(0, len(data), BLOCK_SIZE):
            b = self.alloc_block()
            chunk = data[off:off + BLOCK_SIZE]
            self.img[b * BLOCK_SIZE:b * BLOCK_SIZE + len(chunk)] = chunk
            blocks.append(b)
        if len(blocks) > 12:
            raise SystemExit(f"mkfs_ext2: {name} needs indirect blocks (>12KB)")
        self.inodes[ino] = [S_IFREG | 0o644, len(data), blocks]
        self.dirs[parent_ino].append((name, ino))
        return ino

    def resolve_dir(self, path):
        """Ensure all directories of path exist; return parent ino + basename."""
        parts = [p for p in path.strip("/").split("/") if p]
        cur = ROOT_INO
        for comp in parts[:-1]:
            key = None
            for (n, i) in self.dirs[cur]:
                if n == comp and i in self.dirs:
                    key = i
                    break
            if key is None:
                key = self.mkdir(cur, comp)
            cur = key
        return cur, parts[-1]

    def write_dir_blocks(self):
        for ino, entries in self.dirs.items():
            b = self.alloc_block()
            block = bytearray(BLOCK_SIZE)
            off = 0
            for idx, (name, target) in enumerate(entries):
                nb = name.encode("ascii")
                need = 8 + (len(nb) + 3) // 4 * 4
                rec_len = need if idx < len(entries) - 1 else BLOCK_SIZE - off
                struct.pack_into("<IHH", block, off, target, rec_len, len(nb))
                block[off + 8:off + 8 + len(nb)] = nb
                off += rec_len
            self.img[b * BLOCK_SIZE:(b + 1) * BLOCK_SIZE] = block
            self.inodes[ino][1] = BLOCK_SIZE
            self.inodes[ino][2] = [b]

    def write_inode_table(self):
        base = 5 * BLOCK_SIZE
        for ino, (mode, size, blocks) in self.inodes.items():
            off = base + (ino - 1) * INODE_SIZE
            links = 2 if mode & S_IFDIR else 1
            if mode & S_IFDIR and ino == ROOT_INO:
                links = 2 + sum(1 for (_, i) in self.dirs[ROOT_INO]
                                if i in self.dirs and i != ROOT_INO)
            struct.pack_into("<HHIIIIIHHII", self.img, off,
                             mode, 0, size, 0, 0, 0, 0, links,
                             len(blocks) * (BLOCK_SIZE // 512), 0, 0)
            for i, b in enumerate(blocks):
                struct.pack_into("<I", self.img, off + 40 + i * 4, b)

    def write_bitmaps(self):
        bb = bytearray(BLOCK_SIZE)
        for b in range(1, self.next_block):  # blocks 1..next-1 used
            bb[b // 8] |= 1 << (b % 8)
        self.img[3 * BLOCK_SIZE:4 * BLOCK_SIZE] = bb
        ib = bytearray(BLOCK_SIZE)
        for ino in range(1, self.next_ino):  # reserved 1-10 + ours
            ib[(ino - 1) // 8] |= 1 << ((ino - 1) % 8)
        self.img[4 * BLOCK_SIZE:5 * BLOCK_SIZE] = ib

    def write_superblock(self):
        free_blocks = self.blocks_count - self.next_block
        free_inodes = INODES_COUNT - (self.next_ino - 1)
        sb = struct.pack("<13I6H4I2H",
                         INODES_COUNT,          # s_inodes_count
                         self.blocks_count,     # s_blocks_count
                         0,                     # s_r_blocks_count
                         free_blocks,           # s_free_blocks_count
                         free_inodes,           # s_free_inodes_count
                         1,                     # s_first_data_block (1KB blocks)
                         0,                     # s_log_block_size -> 1024
                         0,                     # s_log_frag_size
                         8192,                  # s_blocks_per_group
                         8192,                  # s_frags_per_group
                         INODES_COUNT,          # s_inodes_per_group
                         0, 0,                  # s_mtime, s_wtime
                         0, 20,                 # s_mnt_count, s_max_mnt_count
                         EXT2_MAGIC,            # s_magic
                         1, 1,                  # s_state, s_errors
                         0,                     # s_minor_rev_level
                         0, 0,                  # s_lastcheck, s_checkinterval
                         0,                     # s_creator_os
                         0,                     # s_rev_level (rev 0: 128B inodes)
                         0, 0)                  # s_def_resuid, s_def_resgid
        self.img[1024:1024 + len(sb)] = sb

    def write_group_desc(self):
        ndirs = len(self.dirs)
        gd = struct.pack("<3I3H",
                         3, 4, 5,                                  # bitmaps + itable
                         self.blocks_count - self.next_block,      # free blocks
                         INODES_COUNT - (self.next_ino - 1),       # free inodes
                         ndirs)
        self.img[2 * BLOCK_SIZE:2 * BLOCK_SIZE + len(gd)] = gd

    def finish(self, out_path):
        self.write_dir_blocks()
        self.write_inode_table()
        self.write_bitmaps()
        self.write_superblock()
        self.write_group_desc()
        with open(out_path, "wb") as f:
            f.write(self.img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--size-mb", type=int, default=4)
    ap.add_argument("--add", action="append", default=[],
                    help="hostfile=/IMG/PATH (directories auto-created)")
    args = ap.parse_args()

    b = Ext2Builder(args.size_mb)
    for spec in args.add:
        host, _, img_path = spec.partition("=")
        with open(host, "rb") as f:
            data = f.read()
        parent, base = b.resolve_dir(img_path)
        b.add_file(parent, base, data)
    b.finish(args.output)
    print(f"mkfs_ext2: {args.output} ({args.size_mb}MB, "
          f"{b.next_ino - FIRST_INO} files/dirs added)")


if __name__ == "__main__":
    main()
