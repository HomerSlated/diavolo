//! Method 2b: XFS bulkstat, xfsdump's way. Inodes in bulk and in inode order,
//! directories and files opened by handle; no path walk at all.

use std::ffi::{CString, c_int, c_ulong};
use std::mem::size_of;

use crate::common::{Ctx, clear_errno, die, die_os, dot_or_dotdot, errno};
use crate::sys::xfs::{xfs_bulk_ireq, xfs_bulkstat};

// _IOR('X', 127, struct xfs_bulkstat_req); the request's size is its header's.
const XFS_IOC_BULKSTAT: c_ulong =
    (2 << 30) | ((size_of::<xfs_bulk_ireq>() as c_ulong) << 16) | ((b'X' as c_ulong) << 8) | 127;

struct BsRec {
    ino: u64,
    size: u64,
    gen_: u32,
}

/// XFS decodes FILEID_INO32_GEN | XFS_FILEID_TYPE_64FLAG as {u64 ino, u32 gen} (fs/xfs/xfs_export.c).
fn xfs_open(mfd: c_int, ino: u64, gen_: u32, flags: c_int) -> c_int {
    let mut raw = [0u64; 4];
    let fh = raw.as_mut_ptr() as *mut libc::file_handle;
    unsafe {
        (*fh).handle_bytes = 12;
        (*fh).handle_type = 0x81;
        let p = (fh as *mut u8).add(8);
        p.copy_from_nonoverlapping(ino.to_ne_bytes().as_ptr(), 8);
        p.add(8)
            .copy_from_nonoverlapping(gen_.to_ne_bytes().as_ptr(), 4);
        libc::open_by_handle_at(mfd, fh, flags | libc::O_CLOEXEC)
    }
}

pub fn run(ctx: &mut Ctx, mnt: &str) {
    const N: usize = 1024;
    let c = CString::new(mnt).unwrap();
    let mfd = unsafe {
        libc::open(
            c.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
        )
    };
    if mfd < 0 {
        die_os(format_args!("open {mnt}"));
    }
    let bytes = size_of::<xfs_bulk_ireq>() + N * size_of::<xfs_bulkstat>();
    let mut req = vec![0u64; bytes / 8];
    let (mut dirs, mut files) = (Vec::with_capacity(1024), Vec::with_capacity(4096));
    let mut next = 0u64;
    loop {
        let hdr = req.as_mut_ptr() as *mut xfs_bulk_ireq;
        unsafe {
            hdr.write_bytes(0, 1);
            (*hdr).ino = next;
            (*hdr).icount = N as u32;
            if libc::ioctl(mfd, XFS_IOC_BULKSTAT, req.as_mut_ptr()) != 0 {
                die_os("XFS_IOC_BULKSTAT (not XFS?)");
            }
        }
        let (ocount, ino) = unsafe { ((*hdr).ocount as usize, (*hdr).ino) };
        if ocount == 0 {
            break;
        }
        next = ino;
        // SAFETY: the kernel filled ocount entries straight after the header.
        let stats =
            unsafe { std::slice::from_raw_parts(hdr.add(1) as *const xfs_bulkstat, ocount) };
        for b in stats {
            let r = BsRec {
                ino: b.bs_ino,
                size: b.bs_size,
                gen_: b.bs_gen,
            };
            match b.bs_mode as u32 & libc::S_IFMT {
                libc::S_IFDIR => dirs.push(r),
                libc::S_IFREG if b.bs_nlink != 0 => files.push(r),
                _ => {}
            }
        }
    }
    // directories first, as dump and xfsdump do; opened by handle, no path walk
    for r in &dirs {
        let f = xfs_open(mfd, r.ino, r.gen_, libc::O_RDONLY | libc::O_DIRECTORY);
        let d = if f < 0 {
            std::ptr::null_mut()
        } else {
            unsafe { libc::fdopendir(f) }
        };
        if d.is_null() {
            die_os(format_args!("open dir ino {}", r.ino));
        }
        loop {
            clear_errno();
            let e = unsafe { libc::readdir(d) };
            if e.is_null() {
                if errno() != 0 {
                    die_os("readdir");
                }
                break;
            }
            let name = unsafe { std::ffi::CStr::from_ptr((*e).d_name.as_ptr()) };
            if !dot_or_dotdot(name.to_bytes()) {
                ctx.names += 1;
            }
        }
        unsafe { libc::closedir(d) };
    }
    for r in &files {
        let f = xfs_open(mfd, r.ino, r.gen_, libc::O_RDONLY);
        if f < 0 {
            die_os(format_args!("open ino {}", r.ino));
        }
        let (n, dg) = ctx.read_stream(f);
        if n != r.size {
            die(format_args!("ino {}: short read", r.ino));
        }
        ctx.add_record(r.ino, n, dg);
        unsafe { libc::close(f) };
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn ioctl_number_and_layout() {
        // _IOR('X', 127, struct xfs_bulkstat_req), as <xfs/xfs_fs.h> computes it
        assert_eq!(super::XFS_IOC_BULKSTAT, 0x8040587f);
        assert_eq!(std::mem::size_of::<super::xfs_bulk_ireq>(), 64);
        assert_eq!(std::mem::size_of::<super::xfs_bulkstat>(), 192);
    }
}
