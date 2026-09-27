//! Method 3a: libext2fs, dump's metadata path. The library scans inodes,
//! maps extents and iterates directories; file data is read with our own
//! pread on the device, as dump's workers do.

use std::ffi::{CString, c_char, c_int, c_void};
use std::ptr::null_mut;

use crate::common::{BUFSZ, Ctx, die, die_os};
use crate::ext4::{Ext, Fent, Parsed, xfer_all};
use crate::sys::ext2fs::*;

unsafe extern "C" fn dir_cb(
    _dir: ext2_ino_t,
    entry: c_int,
    de: *mut ext2_dir_entry,
    _offset: c_int,
    _blocksize: c_int,
    _buf: *mut c_char,
    priv_data: *mut c_void,
) -> c_int {
    // SAFETY: libext2fs passes a valid entry and our &mut u64 back to us.
    unsafe {
        if entry == DIRENT_OTHER_FILE as c_int && (*de).inode != 0 {
            *(priv_data as *mut u64) += 1;
        }
    }
    0
}

fn check(err: errcode_t, what: impl std::fmt::Display) {
    if err != 0 {
        die(format_args!("{what}: error {err}"));
    }
}

pub fn run(ctx: &mut Ctx, dev: &str) {
    let c = CString::new(dev).unwrap();
    let mut fs: ext2_filsys = null_mut();
    // SAFETY: plain libext2fs calls; every out-pointer is a live local.
    unsafe {
        let io = unix_io_manager;
        check(
            ext2fs_open(c.as_ptr(), EXT2_FLAG_64BITS as c_int, 0, 0, io, &mut fs),
            format_args!("ext2fs_open {dev}"),
        );
        let bs = (*fs).blocksize;
        let sp = &*(*fs).super_;
        let mut p = Parsed {
            internal: [
                sp.s_journal_inum,
                sp.s_usr_quota_inum,
                sp.s_grp_quota_inum,
                sp.s_prj_quota_inum,
                sp.s_orphan_file_inum,
                sp.s_snapshot_inum,
            ],
            ..Default::default()
        };
        let first_ino = if sp.s_rev_level == 0 {
            11
        } else {
            sp.s_first_ino
        } as u64;

        // 1 MiB of inode table per read, the same as the hand-written parser
        let mut scan: ext2_inode_scan = null_mut();
        check(
            ext2fs_open_inode_scan(fs, (BUFSZ as u32 / bs) as c_int, &mut scan),
            "ext2fs_open_inode_scan",
        );
        let mut inode: ext2_inode = std::mem::zeroed();
        let mut ino: ext2_ino_t = 0;
        loop {
            check(
                ext2fs_get_next_inode(scan, &mut ino, &mut inode),
                "ext2fs_get_next_inode",
            );
            if ino == 0 {
                break;
            }
            if inode.i_links_count == 0 {
                continue;
            }
            let typ = inode.i_mode & 0xF000;
            if typ == 0x4000 {
                p.dirs.push(Fent {
                    ino: ino as u64,
                    size: 0,
                    first: 0,
                    n: 0,
                });
            } else if typ == 0x8000 && ino as u64 >= first_ino && !p.is_internal(ino as u64) {
                if inode.i_flags & 0x10000000 != 0 {
                    die(format_args!("ino {ino}: inline data unsupported"));
                }
                if inode.i_flags & 0x80000 == 0 {
                    die(format_args!("ino {ino}: block-mapped files unsupported"));
                }
                let size = inode.i_size as u64 | (inode.i_size_high as u64) << 32;
                let first = p.ex.len();
                let mut h: ext2_extent_handle_t = null_mut();
                check(
                    ext2fs_extent_open2(fs, ino, &mut inode, &mut h),
                    format_args!("ext2fs_extent_open2 {ino}"),
                );
                let mut e: ext2fs_extent = std::mem::zeroed();
                let mut err = ext2fs_extent_get(h, EXT2_EXTENT_ROOT as c_int, &mut e);
                while err == 0 {
                    if e.e_flags & EXT2_EXTENT_FLAGS_LEAF != 0 {
                        let uninit = e.e_flags & EXT2_EXTENT_FLAGS_UNINIT != 0;
                        p.ex.push(Ext {
                            lblk: e.e_lblk,
                            pblk: e.e_pblk,
                            len: e.e_len,
                            uninit,
                        });
                    }
                    err = ext2fs_extent_get(h, EXT2_EXTENT_NEXT as c_int, &mut e);
                }
                if err != EXT2_ET_EXTENT_NO_NEXT as errcode_t
                    && err != EXT2_ET_NO_CURRENT_NODE as errcode_t
                {
                    check(err, format_args!("ext2fs_extent_get {ino}"));
                }
                ext2fs_extent_free(h);
                p.files.push(Fent {
                    ino: ino as u64,
                    size,
                    first,
                    n: p.ex.len() - first,
                });
            }
        }
        ext2fs_close_inode_scan(scan);

        for d in &p.dirs {
            let names = &mut ctx.names as *mut u64 as *mut c_void;
            check(
                ext2fs_dir_iterate2(fs, d.ino as ext2_ino_t, 0, null_mut(), Some(dir_cb), names),
                format_args!("ext2fs_dir_iterate2 {}", d.ino),
            );
        }

        let fd = libc::open(c.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC);
        if fd < 0 {
            die_os(format_args!("open {dev}"));
        }
        xfer_all(ctx, fd, bs, &p);
        libc::close(fd);
        ext2fs_close_free(&mut fs);
    }
}
