//! The spec model: `docs/spec/format.yaml` and `docs/spec/rules.yaml`.
//!
//! Stage 1 is structural and done by serde (`deny_unknown_fields` everywhere,
//! so a misspelt key is an error, not silence). Stage 2 is semantic, runs only on
//! stage-1-valid data, and collects every problem before failing.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

use serde::Deserialize;

/// The only `spec_schema` this generator understands. Refused loudly otherwise.
pub const SPEC_SCHEMA: u32 = 1;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Format {
    pub spec_schema: u32,
    pub format_version: Version,
    pub status: String,
    pub inputs: Vec<String>,
    pub endianness: String,
    pub offset_origin: String,
    pub acceptance: Acceptance,
    pub constants: Vec<Constant>,
    pub magics: Vec<Magic>,
    pub enums: Vec<Enum>,
    pub content_kind_by_type: BTreeMap<String, Vec<String>>,
    pub flag_sets: Vec<FlagSet>,
    pub block_types: Vec<BlockType>,
    pub layouts: Vec<Layout>,
    pub index_tables: Vec<IndexTable>,
    pub heap: Heap,
    pub phases: Vec<Phase>,
    pub notes: Vec<Note>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Version {
    pub major: u16,
    pub minor: u16,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Acceptance {
    pub rows: Vec<AcceptRow>,
    pub otherwise: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AcceptRow {
    pub file_major: u16,
    pub file_minor: u16,
    pub verdict: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Constant {
    pub name: String,
    pub value: u64,
    pub doc: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Magic {
    pub name: String,
    pub bytes: Vec<u8>,
    pub doc: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Enum {
    pub name: String,
    pub width: String,
    #[serde(default)]
    pub mask: Option<u64>,
    pub values: Vec<EnumValue>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EnumValue {
    pub name: String,
    pub value: u64,
    #[serde(default)]
    pub registered_undefined: bool,
    pub doc: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FlagSet {
    pub name: String,
    pub width: String,
    pub unknown_odd_rule: String,
    pub bits: Vec<FlagBit>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FlagBit {
    pub bit: u32,
    pub name: String,
    pub doc: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BlockType {
    pub id: String,
    pub status: String,
    pub doc: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Layout {
    pub name: String,
    pub size: u64,
    pub doc: String,
    #[serde(default)]
    pub heap_refs: Vec<(String, String)>,
    pub fields: Vec<Field>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Field {
    pub off: u64,
    pub name: String,
    #[serde(rename = "type")]
    pub ty: String,
    #[serde(default)]
    pub doc: Option<String>,
    #[serde(default, rename = "enum")]
    pub enum_: Option<String>,
    #[serde(default)]
    pub flags: Option<String>,
    #[serde(default)]
    pub reserved: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct IndexTable {
    pub name: String,
    pub record: Option<String>,
    pub count: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Heap {
    pub length_field: String,
    pub offset_origin: String,
    pub follows: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Phase {
    pub name: String,
    pub doc: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Note {
    pub text: String,
    pub not_a_rule_because: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rules {
    pub rules: Vec<Rule>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rule {
    pub id: String,
    pub level: String,
    #[serde(default)]
    pub phase: Option<String>,
    pub scope: String,
    pub text: String,
    #[serde(default)]
    pub fixtures: Vec<String>,
    #[serde(default)]
    pub no_fixture_because: Option<String>,
}

impl Format {
    pub fn layout(&self, name: &str) -> Option<&Layout> {
        self.layouts.iter().find(|l| l.name == name)
    }
    pub fn enum_(&self, name: &str) -> Option<&Enum> {
        self.enums.iter().find(|e| e.name == name)
    }
    pub fn flag_set(&self, name: &str) -> Option<&FlagSet> {
        self.flag_sets.iter().find(|f| f.name == name)
    }
}

impl Layout {
    pub fn field(&self, name: &str) -> Option<&Field> {
        self.fields.iter().find(|f| f.name == name)
    }
}

impl Enum {
    pub fn value(&self, name: &str) -> Option<u64> {
        self.values.iter().find(|v| v.name == name).map(|v| v.value)
    }
}

/// Byte width of a field type, or None if the type is not in the vocabulary.
pub fn type_size(ty: &str) -> Option<u64> {
    match ty {
        "u16" => Some(2),
        "u32" => Some(4),
        "u64" | "i64" => Some(8),
        _ => ty.strip_prefix("bytes").and_then(|n| n.parse().ok()),
    }
}

pub fn read_yaml<T: for<'de> Deserialize<'de>>(path: &Path) -> Result<T, String> {
    let text = fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    yaml_serde::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))
}

/// Rust enum variant for a rule id: `header.version_unsupported` -> `HeaderVersionUnsupported`.
pub fn rule_variant(id: &str) -> String {
    id.split(['.', '_'])
        .map(|w| {
            let mut c = w.chars();
            c.next()
                .map(|f| f.to_ascii_uppercase().to_string() + c.as_str())
                .unwrap_or_default()
        })
        .collect()
}

/// Stage 2. Returns every problem found; empty means valid.
pub fn validate(spec_dir: &Path, f: &Format, r: &Rules) -> Vec<String> {
    let mut e = Vec::new();

    // ---- clocks and declared inputs -------------------------------------
    if f.spec_schema != SPEC_SCHEMA {
        // Refuse before anything else: the vocabulary itself may differ.
        return vec![format!(
            "format.yaml spec_schema {} is not understood (this generator reads {SPEC_SCHEMA})",
            f.spec_schema
        )];
    }
    let mut declared: BTreeSet<String> = f.inputs.iter().cloned().collect();
    declared.insert("format.yaml".into());
    match fs::read_dir(spec_dir) {
        Ok(rd) => {
            let present: BTreeSet<String> = rd
                .filter_map(|d| d.ok())
                .map(|d| d.file_name().to_string_lossy().into_owned())
                .collect();
            for p in present.difference(&declared) {
                e.push(format!(
                    "docs/spec/{p} is not a declared input of format.yaml; declare it or remove it"
                ));
            }
            for p in declared.difference(&present) {
                e.push(format!("declared input docs/spec/{p} does not exist"));
            }
        }
        Err(err) => e.push(format!("{}: {err}", spec_dir.display())),
    }
    if f.endianness != "little" {
        e.push(format!("endianness {:?} unsupported", f.endianness));
    }
    let unstable = f.format_version.major == 0;
    if unstable != (f.status == "UNSTABLE") {
        e.push("status must be UNSTABLE exactly when format_version.major == 0".into());
    }

    // ---- acceptance -----------------------------------------------------
    if f.acceptance.otherwise != "reject" {
        e.push("acceptance.otherwise must be reject".into());
    }
    let mut rows = BTreeSet::new();
    for row in &f.acceptance.rows {
        if row.verdict != "accept" {
            e.push(format!(
                "acceptance row ({}, {}) verdict must be accept; unlisted versions are rejected",
                row.file_major, row.file_minor
            ));
        }
        if !rows.insert((row.file_major, row.file_minor)) {
            e.push(format!(
                "acceptance row ({}, {}) duplicated",
                row.file_major, row.file_minor
            ));
        }
    }
    if !rows.contains(&(f.format_version.major, f.format_version.minor)) {
        e.push("acceptance must accept the current format_version".into());
    }

    // ---- names ------------------------------------------------------------
    let mut names = BTreeSet::new();
    for n in f
        .constants
        .iter()
        .map(|c| &c.name)
        .chain(f.magics.iter().map(|m| &m.name))
    {
        if !names.insert(n.clone()) {
            e.push(format!("constant/magic {n} defined twice"));
        }
        if !n
            .bytes()
            .all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_')
        {
            e.push(format!("constant/magic {n} must be SCREAMING_SNAKE_CASE"));
        }
    }
    for m in &f.magics {
        if m.bytes.len() != 8 {
            e.push(format!("magic {} must be 8 bytes", m.name));
        }
    }
    let magic_set: BTreeSet<&Vec<u8>> = f.magics.iter().map(|m| &m.bytes).collect();
    if magic_set.len() != f.magics.len() {
        e.push("two magics have identical bytes".into());
    }

    // ---- enums --------------------------------------------------------------
    let mut enum_names = BTreeSet::new();
    for en in &f.enums {
        if !enum_names.insert(&en.name) {
            e.push(format!("enum {} defined twice", en.name));
        }
        let bits = match en.width.as_str() {
            "u16" => 16,
            "u32" => 32,
            w => {
                e.push(format!("enum {} width {w} unsupported", en.name));
                continue;
            }
        };
        match en.values.first() {
            Some(v) if v.name == "invalid" && v.value == 0 => {}
            _ => e.push(format!("enum {}: first value must be invalid = 0", en.name)),
        }
        let mut vn = BTreeSet::new();
        let mut vv = BTreeSet::new();
        for v in &en.values {
            if !vn.insert(&v.name) || !vv.insert(v.value) {
                e.push(format!(
                    "enum {}: duplicate name or value at {}",
                    en.name, v.name
                ));
            }
            if bits < 64 && v.value >> bits != 0 {
                e.push(format!(
                    "enum {}: {} does not fit {}",
                    en.name, v.name, en.width
                ));
            }
            if let Some(mask) = en.mask
                && v.value & !mask != 0
            {
                e.push(format!(
                    "enum {}: {} has bits outside mask",
                    en.name, v.name
                ));
            }
            if v.registered_undefined && v.value == 0 {
                e.push(format!(
                    "enum {}: invalid cannot be registered_undefined",
                    en.name
                ));
            }
        }
    }
    match (f.enum_("file_type"), f.enum_("content_kind")) {
        (Some(ft), Some(ck)) => {
            let types: BTreeSet<&str> = ft.values[1..].iter().map(|v| v.name.as_str()).collect();
            let keys: BTreeSet<&str> = f.content_kind_by_type.keys().map(String::as_str).collect();
            if types != keys {
                e.push(
                    "content_kind_by_type keys must be exactly the non-invalid file_type names"
                        .into(),
                );
            }
            for (t, kinds) in &f.content_kind_by_type {
                for k in kinds {
                    if k == "invalid" || ck.value(k).is_none() {
                        e.push(format!(
                            "content_kind_by_type.{t}: {k} is not a valid content_kind"
                        ));
                    }
                }
            }
        }
        _ => e.push("enums file_type and content_kind are required".into()),
    }

    // ---- rules (needed by flag sets) -----------------------------------------
    let rule_ids: BTreeMap<&str, &Rule> = r.rules.iter().map(|x| (x.id.as_str(), x)).collect();
    let phase_names: BTreeSet<&str> = f.phases.iter().map(|p| p.name.as_str()).collect();
    if phase_names.len() != f.phases.len() {
        e.push("phase names must be unique".into());
    }
    let mut seen_ids = BTreeSet::new();
    let mut seen_variants = BTreeSet::new();
    let mut seen_fixtures = BTreeSet::new();
    for rule in &r.rules {
        let ok_id = rule.id.split('.').count() >= 2
            && rule.id.split('.').all(|p| {
                !p.is_empty()
                    && p.bytes()
                        .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
            });
        if !ok_id {
            e.push(format!(
                "rule id {:?} must be dotted lower_snake segments",
                rule.id
            ));
        }
        if !seen_ids.insert(&rule.id) {
            e.push(format!("rule {} defined twice", rule.id));
        }
        if !seen_variants.insert(rule_variant(&rule.id)) {
            e.push(format!(
                "rule {} collides with another rule's Rust variant name",
                rule.id
            ));
        }
        if !matches!(rule.level.as_str(), "MUST" | "SHOULD") {
            e.push(format!("rule {}: level must be MUST or SHOULD", rule.id));
        }
        match rule.scope.as_str() {
            "file" | "chain" => {
                match &rule.phase {
                    Some(p) if phase_names.contains(p.as_str()) => {}
                    _ => e.push(format!(
                        "rule {}: needs a phase from format.yaml phases",
                        rule.id
                    )),
                }
                if rule.fixtures.is_empty() {
                    e.push(format!(
                        "rule {}: a {} rule needs at least one broken fixture",
                        rule.id, rule.scope
                    ));
                }
                if rule.no_fixture_because.is_some() {
                    e.push(format!(
                        "rule {}: no_fixture_because is only for writer/behaviour rules",
                        rule.id
                    ));
                }
                if rule.level != "MUST" {
                    e.push(format!(
                        "rule {}: a reader-checked rule must be MUST; a SHOULD cannot reject",
                        rule.id
                    ));
                }
            }
            "writer" | "behaviour" => {
                if !rule.fixtures.is_empty() || rule.phase.is_some() {
                    e.push(format!(
                        "rule {}: {} rules take no fixtures and no phase",
                        rule.id, rule.scope
                    ));
                }
                if rule.no_fixture_because.as_deref().is_none_or(str::is_empty) {
                    e.push(format!("rule {}: no_fixture_because is required", rule.id));
                }
            }
            s => e.push(format!("rule {}: unknown scope {s}", rule.id)),
        }
        for fx in &rule.fixtures {
            if !seen_fixtures.insert(fx) {
                e.push(format!(
                    "fixture {fx} is cited by more than one rule; it must violate exactly one"
                ));
            }
        }
    }

    // ---- flag sets ---------------------------------------------------------
    for fs_ in &f.flag_sets {
        let bits = match fs_.width.as_str() {
            "u32" => 32,
            w => {
                e.push(format!("flag set {} width {w} unsupported", fs_.name));
                continue;
            }
        };
        let mut seen = BTreeSet::new();
        for b in &fs_.bits {
            if b.bit >= bits || !seen.insert(b.bit) {
                e.push(format!(
                    "flag set {}: bit {} out of range or duplicated",
                    fs_.name, b.name
                ));
            }
        }
        match rule_ids.get(fs_.unknown_odd_rule.as_str()) {
            Some(rule) if rule.scope == "file" => {}
            _ => e.push(format!(
                "flag set {}: unknown_odd_rule {} is not a file rule",
                fs_.name, fs_.unknown_odd_rule
            )),
        }
    }

    // ---- block types ---------------------------------------------------------
    let mut ids = BTreeSet::new();
    for bt in &f.block_types {
        if bt.id.len() != 4
            || !bt
                .id
                .bytes()
                .all(|b| b.is_ascii_uppercase() || b.is_ascii_digit())
        {
            e.push(format!(
                "block type {:?} must be 4 uppercase ASCII alphanumerics",
                bt.id
            ));
        }
        if !ids.insert(&bt.id) {
            e.push(format!("block type {} defined twice", bt.id));
        }
        if !matches!(bt.status.as_str(), "defined" | "registered_undefined") {
            e.push(format!(
                "block type {}: unknown status {}",
                bt.id, bt.status
            ));
        }
    }
    for need in ["DATA", "INDX"] {
        if !f
            .block_types
            .iter()
            .any(|b| b.id == need && b.status == "defined")
        {
            e.push(format!("block type {need} must be defined"));
        }
    }

    // ---- layouts: the tiling check ---------------------------------------------
    let mut lnames = BTreeSet::new();
    for l in &f.layouts {
        if !lnames.insert(&l.name) {
            e.push(format!("layout {} defined twice", l.name));
        }
        let mut cursor = 0u64;
        let mut fnames = BTreeSet::new();
        for fl in &l.fields {
            if !fnames.insert(&fl.name) {
                e.push(format!("{}.{} defined twice", l.name, fl.name));
            }
            if fl.off != cursor {
                e.push(format!(
                    "{}.{} at offset {} but the previous field ends at {cursor} (gap or overlap)",
                    l.name, fl.name, fl.off
                ));
            }
            let Some(sz) = type_size(&fl.ty) else {
                e.push(format!("{}.{}: unknown type {}", l.name, fl.name, fl.ty));
                continue;
            };
            cursor = fl.off + sz;
            if let Some(en) = &fl.enum_ {
                match f.enum_(en) {
                    Some(x) if x.width == fl.ty => {}
                    _ => e.push(format!(
                        "{}.{}: enum {en} missing or width differs",
                        l.name, fl.name
                    )),
                }
            }
            if let Some(fsn) = &fl.flags {
                match f.flag_set(fsn) {
                    Some(x) if x.width == fl.ty => {}
                    _ => e.push(format!(
                        "{}.{}: flag set {fsn} missing or width differs",
                        l.name, fl.name
                    )),
                }
            }
            if fl.reserved != fl.name.starts_with("reserved") {
                e.push(format!(
                    "{}.{}: `reserved: true` exactly for fields named reserved*",
                    l.name, fl.name
                ));
            }
        }
        if cursor != l.size {
            e.push(format!(
                "layout {}: fields end at {cursor}, declared size {}",
                l.name, l.size
            ));
        }
        if l.size % 8 != 0 {
            e.push(format!(
                "layout {}: size {} is not a multiple of 8",
                l.name, l.size
            ));
        }
        for (off, len) in &l.heap_refs {
            match (l.field(off), l.field(len)) {
                (Some(o), Some(n)) if o.ty == "u64" && matches!(n.ty.as_str(), "u32" | "u64") => {}
                _ => e.push(format!(
                    "layout {}: heap ref ({off}, {len}) needs a u64 offset and a u32/u64 length",
                    l.name
                )),
            }
        }
    }
    for need in [
        "header",
        "trailer",
        "block_header",
        "data_header",
        "index_header",
    ] {
        if f.layout(need).is_none() {
            e.push(format!("layout {need} is required"));
        }
    }

    // ---- index tables and heap -------------------------------------------------
    if let Some(ih) = f.layout("index_header") {
        let mut tnames = BTreeSet::new();
        for t in &f.index_tables {
            if !tnames.insert(&t.name) {
                e.push(format!("index table {} listed twice", t.name));
            }
            match ih.field(&t.count) {
                Some(c) if c.ty == "u64" || c.ty == "u32" => {}
                _ => e.push(format!(
                    "index table {}: count field index_header.{} missing",
                    t.name, t.count
                )),
            }
            if let Some(rec) = &t.record
                && f.layout(rec).is_none()
            {
                e.push(format!(
                    "index table {}: record layout {rec} missing",
                    t.name
                ));
            }
        }
        match f.heap.length_field.strip_prefix("index_header.") {
            Some(h) if ih.field(h).is_some() => {}
            _ => e.push("heap.length_field must name an index_header field".into()),
        }
    }

    e
}
