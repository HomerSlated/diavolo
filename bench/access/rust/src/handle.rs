//! Method 2a: file handles on any filesystem. Walk once collecting
//! name_to_handle_at handles, sort by inode, then open_by_handle_at in that order.

use std::ffi::{CStr, CString, c_int};

use crate::common::{Ctx, clear_errno, die, die_os, dot_or_dotdot, errno};

const HWORDS: usize = (8 + libc::MAX_HANDLE_SZ as usize) / 8;

/// Handles live in a u64 arena so each struct file_handle is suitably aligned.
struct Handles {
    arena: Vec<u64>,
    list: Vec<(u64, usize)>, // (inode, word offset into arena)
}

fn walk(ctx: &mut Ctx, hs: &mut Handles, dfd: c_int) {
    let d = unsafe { libc::fdopendir(dfd) };
    if d.is_null() {
        die_os("fdopendir");
    }
    let mut raw = [0u64; HWORDS];
    loop {
        clear_errno();
        let e = unsafe { libc::readdir(d) };
        if e.is_null() {
            if errno() != 0 {
                die_os("readdir");
            }
            break;
        }
        let (name, mut t, dino) = unsafe {
            (
                CStr::from_ptr((*e).d_name.as_ptr()),
                (*e).d_type,
                (*e).d_ino,
            )
        };
        if dot_or_dotdot(name.to_bytes()) {
            continue;
        }
        ctx.names += 1;
        let pfd = unsafe { libc::dirfd(d) };
        if t == libc::DT_UNKNOWN {
            let mut st: libc::stat = unsafe { std::mem::zeroed() };
            if unsafe { libc::fstatat(pfd, name.as_ptr(), &mut st, libc::AT_SYMLINK_NOFOLLOW) } != 0
            {
                die_os(format_args!("fstatat {}", name.to_string_lossy()));
            }
            t = match st.st_mode & libc::S_IFMT {
                libc::S_IFDIR => libc::DT_DIR,
                libc::S_IFREG => libc::DT_REG,
                _ => libc::DT_UNKNOWN,
            };
        }
        if t == libc::DT_DIR {
            let flags = libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC;
            let c = unsafe { libc::openat(pfd, name.as_ptr(), flags) };
            if c < 0 {
                die_os(format_args!("openat dir {}", name.to_string_lossy()));
            }
            walk(ctx, hs, c);
        } else if t == libc::DT_REG && ctx.seen.add(dino) {
            let fh = raw.as_mut_ptr() as *mut libc::file_handle;
            let mut mnt_id: c_int = 0;
            unsafe { (*fh).handle_bytes = libc::MAX_HANDLE_SZ as u32 };
            if unsafe { libc::name_to_handle_at(pfd, name.as_ptr(), fh, &mut mnt_id, 0) } != 0 {
                die_os(format_args!("name_to_handle_at {}", name.to_string_lossy()));
            }
            let words = (8 + unsafe { (*fh).handle_bytes } as usize).div_ceil(8);
            hs.list.push((dino, hs.arena.len()));
            hs.arena.extend_from_slice(&raw[..words]);
        }
    }
    unsafe { libc::closedir(d) };
}

pub fn run(ctx: &mut Ctx, mnt: &str) {
    let c = CString::new(mnt).unwrap();
    let mfd = unsafe {
        libc::open(
            c.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
        )
    };
    let dfd = if mfd < 0 {
        -1
    } else {
        unsafe { libc::dup(mfd) }
    };
    if dfd < 0 {
        die_os(format_args!("open {mnt}"));
    }
    let mut hs = Handles {
        arena: Vec::with_capacity(1 << 17),
        list: Vec::with_capacity(4096),
    };
    walk(ctx, &mut hs, dfd);
    hs.list.sort_unstable_by_key(|h| h.0); // inode order, as bulkstat gives it
    for &(ino, off) in &hs.list {
        let fh = hs.arena[off..].as_ptr() as *mut libc::file_handle;
        let f = unsafe { libc::open_by_handle_at(mfd, fh, libc::O_RDONLY | libc::O_CLOEXEC) };
        if f < 0 {
            die_os(format_args!("open_by_handle_at ino {ino}"));
        }
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(f, &mut st) } != 0 {
            die_os("fstat");
        }
        let (n, dg) = ctx.read_stream(f);
        if n != st.st_size as u64 {
            die(format_args!("ino {ino}: short read"));
        }
        ctx.add_record(st.st_ino, n, dg);
        unsafe { libc::close(f) };
    }
}
