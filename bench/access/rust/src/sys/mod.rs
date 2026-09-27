//! bindgen output for libext2fs and the XFS ioctl structs; see ../../gen-bindings.sh.

#[allow(
    non_camel_case_types,
    non_upper_case_globals,
    non_snake_case,
    dead_code,
    unsafe_op_in_unsafe_fn,
    clippy::all
)]
pub mod ext2fs;
#[allow(
    non_camel_case_types,
    non_upper_case_globals,
    non_snake_case,
    dead_code,
    unsafe_op_in_unsafe_fn,
    clippy::all
)]
pub mod xfs;
