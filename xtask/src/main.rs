//! Repository automation. `cargo xtask codegen` regenerates every generated artifact.
//!
//! The generator owns its output directories outright: a file in one of them that
//! this run did not produce is deleted, so a stale artifact cannot survive a rename.

mod gen_docs;
mod gen_rust;
mod spec;

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use serde::Deserialize;

/// Directories whose entire contents the generator produces.
const OWNED: &[&str] = &[
    "docs/generated",
    "crates/diavolo-format/src/generated",
    "corpus",
];

fn main() -> ExitCode {
    match std::env::args().nth(1).as_deref() {
        Some("codegen") => match codegen() {
            Ok(()) => ExitCode::SUCCESS,
            Err(errs) => {
                for e in &errs {
                    eprintln!("error: {e}");
                }
                eprintln!("xtask codegen: {} error(s); nothing written", errs.len());
                ExitCode::FAILURE
            }
        },
        _ => {
            eprintln!("usage: cargo xtask codegen");
            ExitCode::from(2)
        }
    }
}

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives one level below the workspace root")
        .to_path_buf()
}

/// Read only `spec_schema`, tolerating every other key, so an unknown schema is
/// refused as such rather than as a confusing field error.
#[derive(Deserialize)]
struct SchemaProbe {
    spec_schema: u32,
}

fn codegen() -> Result<(), Vec<String>> {
    let root = root();
    let spec_dir = root.join("docs/spec");

    let probe: SchemaProbe = spec::read_yaml(&spec_dir.join("format.yaml")).map_err(|e| vec![e])?;
    if probe.spec_schema != spec::SPEC_SCHEMA {
        return Err(vec![format!(
            "docs/spec/format.yaml has spec_schema {}; this generator understands only {}",
            probe.spec_schema,
            spec::SPEC_SCHEMA
        )]);
    }
    let format: spec::Format =
        spec::read_yaml(&spec_dir.join("format.yaml")).map_err(|e| vec![e])?;
    let rules: spec::Rules = spec::read_yaml(&spec_dir.join("rules.yaml")).map_err(|e| vec![e])?;

    let errs = spec::validate(&spec_dir, &format, &rules);
    if !errs.is_empty() {
        return Err(errs);
    }

    let mut out: BTreeMap<PathBuf, Vec<u8>> = BTreeMap::new();
    let gen_dir = PathBuf::from("crates/diavolo-format/src/generated");
    out.insert(gen_dir.join("mod.rs"), gen_rust::mod_rs().into_bytes());
    out.insert(
        gen_dir.join("layout.rs"),
        gen_rust::layout_rs(&format).into_bytes(),
    );
    out.insert(
        gen_dir.join("rules.rs"),
        gen_rust::rules_rs(&rules).into_bytes(),
    );
    out.insert(
        "docs/generated/format.md".into(),
        gen_docs::format_md(&format).into_bytes(),
    );
    out.insert(
        "docs/generated/rules.md".into(),
        gen_docs::rules_md(&format, &rules).into_bytes(),
    );

    write_owned(&root, &out).map_err(|e| vec![e])
}

fn write_owned(root: &Path, out: &BTreeMap<PathBuf, Vec<u8>>) -> Result<(), String> {
    for rel in out.keys() {
        if !OWNED.iter().any(|o| rel.starts_with(o)) {
            return Err(format!(
                "{} is outside every owned directory",
                rel.display()
            ));
        }
    }
    let (mut written, mut removed) = (0, 0);
    for dir in OWNED {
        for existing in walk(&root.join(dir))? {
            let rel = existing.strip_prefix(root).map_err(|e| e.to_string())?;
            if !out.contains_key(rel) {
                fs::remove_file(&existing).map_err(|e| format!("{}: {e}", existing.display()))?;
                removed += 1;
            }
        }
    }
    for (rel, bytes) in out {
        let path = root.join(rel);
        if fs::read(&path).ok().as_deref() == Some(bytes.as_slice()) {
            continue;
        }
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
        }
        fs::write(&path, bytes).map_err(|e| format!("{}: {e}", path.display()))?;
        written += 1;
    }
    eprintln!(
        "xtask codegen: {} artifact(s), {written} written, {removed} stale removed",
        out.len()
    );
    Ok(())
}

fn walk(dir: &Path) -> Result<Vec<PathBuf>, String> {
    let mut files = Vec::new();
    let rd = match fs::read_dir(dir) {
        Ok(rd) => rd,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(files),
        Err(e) => return Err(format!("{}: {e}", dir.display())),
    };
    for entry in rd {
        let path = entry.map_err(|e| e.to_string())?.path();
        if path.is_dir() {
            files.extend(walk(&path)?);
        } else {
            files.push(path);
        }
    }
    Ok(files)
}
