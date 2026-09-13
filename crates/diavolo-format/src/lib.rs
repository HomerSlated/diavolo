//! Diavolo archive format: types, generated constants and the hand-written decoder.
//!
//! The byte layout is defined by `docs/spec/format.yaml` and nowhere else. The
//! constants in [`generated`] are produced from it by `cargo xtask codegen`.

pub mod field;
#[rustfmt::skip]
pub mod generated;
