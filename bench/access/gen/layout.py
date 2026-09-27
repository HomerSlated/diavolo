#!/usr/bin/env python3
"""Measure how a file set actually landed on disk: layout.py MOUNTPOINT

Uses FIEMAP, so it works on any filesystem that supports it (ext4, XFS).
Prints one line for the image's .info file:

  files      regular files with data (hardlinks counted once)
  extents    mean extents per file; frag = share of files with more than one
  backward   visiting files in inode order, the share of steps where the next
             file starts at a LOWER disk address than the previous one. Fresh
             layouts should be near 0; a random layout near 50%. This, not
             e2fsck's non-contiguous %, is what inode-order reading cares about.
  jump       median absolute distance between consecutive files' first blocks
"""

import fcntl
import os
import statistics
import struct
import sys

FS_IOC_FIEMAP = 0xC020660B  # _IOWR('f', 11, struct fiemap)
HDR = struct.Struct("=QQIIII")  # fm_start, fm_length, fm_flags, fm_mapped_extents, fm_extent_count, fm_reserved
EXT = struct.Struct("=QQQQQIIII")  # fe_logical, fe_physical, fe_length, 2x reserved64, fe_flags, 3x reserved
FIEMAP_FLAG_SYNC = 1


def fiemap(fd, count):
    buf = bytearray(HDR.pack(0, 2**64 - 1, FIEMAP_FLAG_SYNC, 0, count, 0) + b"\0" * (EXT.size * count))
    fcntl.ioctl(fd, FS_IOC_FIEMAP, buf)
    mapped = HDR.unpack_from(buf)[3]
    first = EXT.unpack_from(buf, HDR.size)[1] if count and mapped else None
    return mapped, first


def main():
    root = sys.argv[1]
    seen, rows = set(), []
    for dp, dn, fn in os.walk(root):
        for n in fn:
            p = os.path.join(dp, n)
            st = os.lstat(p)
            if not os.path.isfile(p) or os.path.islink(p) or st.st_ino in seen or st.st_size == 0:
                continue
            seen.add(st.st_ino)
            fd = os.open(p, os.O_RDONLY)
            try:
                next_, _ = fiemap(fd, 0)
                _, first = fiemap(fd, 1)
            finally:
                os.close(fd)
            if first is not None:
                rows.append((st.st_ino, first, next_))
    if not rows:
        print("layout: no files with data")
        return
    rows.sort()
    exts = [r[2] for r in rows]
    frag = sum(1 for e in exts if e > 1) / len(exts)
    line = f"layout: files {len(rows)}, extents {statistics.mean(exts):.2f}/file (frag {frag:.1%})"
    if len(rows) > 1:
        back = sum(1 for a, b in zip(rows, rows[1:]) if b[1] < a[1])
        jumps = [abs(b[1] - a[1]) for a, b in zip(rows, rows[1:])]
        line += f", backward {back / (len(rows) - 1):.1%}, jump {statistics.median(jumps) / 1048576:.1f} MiB"
    print(line)


if __name__ == "__main__":
    main()
