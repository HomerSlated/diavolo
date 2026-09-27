fn main() {
    println!("cargo:rustc-link-lib=ext2fs");
    println!("cargo:rustc-link-lib=com_err");
}
