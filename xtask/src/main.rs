//! Repository automation. `cargo xtask codegen` regenerates every generated artifact.

use std::process::ExitCode;

fn main() -> ExitCode {
    match std::env::args().nth(1).as_deref() {
        Some("codegen") => {
            // Placeholder until docs/spec/format.yaml has content to generate from.
            eprintln!("xtask codegen: no spec inputs yet");
            ExitCode::SUCCESS
        }
        _ => {
            eprintln!("usage: cargo xtask codegen");
            ExitCode::from(2)
        }
    }
}
