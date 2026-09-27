#!/bin/sh
# Regenerates src/sys/{ext2fs,xfs}.rs from the installed headers
# (e2fsprogs-devel, xfsprogs-devel). The output is committed so a build
# needs no bindgen; rerun this after upgrading either package.
set -e
cd "$(dirname "$0")"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
echo '#include <ext2fs/ext2fs.h>' > "$t/ext2fs.h"
echo '#include <xfs/xfs.h>' > "$t/xfs.h"
bindgen "$t/ext2fs.h" --rust-edition 2024 --rust-target 1.85 --use-core --no-layout-tests \
	--allowlist-function 'ext2fs_(open|close_free|open_inode_scan|close_inode_scan|get_next_inode|extent_open2|extent_get|extent_free|dir_iterate2)' \
	--allowlist-var 'unix_io_manager|EXT2_(ET_EXTENT_NO_NEXT|ET_NO_CURRENT_NODE|FLAG_64BITS|EXTENT_ROOT|EXTENT_NEXT|EXTENT_FLAGS_LEAF|EXTENT_FLAGS_UNINIT)|DIRENT_OTHER_FILE' \
	--allowlist-type 'ext2_inode|ext2fs_extent|ext2_dir_entry|ext2_super_block' \
	-o src/sys/ext2fs.rs
bindgen "$t/xfs.h" --rust-edition 2024 --rust-target 1.85 --use-core --no-layout-tests \
	--allowlist-type 'xfs_bulkstat|xfs_bulk_ireq|xfs_bulkstat_req' \
	-o src/sys/xfs.rs -- -D_GNU_SOURCE
