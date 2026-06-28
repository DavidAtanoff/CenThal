//! # skald-driver
//!
//! Spec §3.2 — CLI entry point orchestrating lexer → parser → resolve → types → borrowck.
//! For Phase 1 frontend milestone, this is a check-only driver (no codegen).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use rustc_hash::FxHashMap;
use skald_ast::{FileId, Interner, Module};
use skald_borrowck::BorrowChecker;
use skald_lexer::LexError;
use skald_modifiers::{compute_class, compute_field, compute_method, compute_free_fn, compute_struct, compute_enum};
use skald_parser::{parse, ParseError};
use skald_resolve::Resolver;
use skald_types::TypeDb;

// ---------- Report ----------

#[derive(Debug, Clone)]
pub struct Report {
    pub file: PathBuf,
    pub lex_errors: Vec<LexError>,
    pub parse_errors: Vec<ParseError>,
    pub resolve_errors: Vec<skald_resolve::ResolveError>,
    pub borrow_errors: Vec<skald_borrowck::BorrowError>,
    pub modifier_errors: Vec<String>,
    pub items: usize,
    pub top_level_classes: usize,
    pub top_level_structs: usize,
    pub top_level_fns: usize,
    pub top_level_traits: usize,
    pub top_level_enums: usize,
}

impl Report {
    pub fn has_errors(&self) -> bool {
        !self.lex_errors.is_empty() || !self.parse_errors.is_empty() || !self.borrow_errors.is_empty()
    }
    pub fn error_count(&self) -> usize {
        self.lex_errors.len() + self.parse_errors.len() + self.borrow_errors.len() + self.modifier_errors.len()
    }
}

// ---------- FileId allocator ----------

static NEXT_FILE: AtomicU32 = AtomicU32::new(0);

pub fn alloc_file_id() -> FileId {
    NEXT_FILE.fetch_add(1, Ordering::SeqCst)
}

// ---------- Pipeline ----------

pub struct Driver {
    pub db: TypeDb,
    pub modules: Vec<(PathBuf, Module)>,
    pub reports: Vec<Report>,
    pub interner: Interner,
    /// Per-module interners (so we can look up names per file).
    pub module_interners: FxHashMap<PathBuf, Interner>,
}

impl Default for Driver {
    fn default() -> Self { Self::new() }
}

impl Driver {
    pub fn new() -> Self {
        Self {
            db: TypeDb::new(),
            modules: vec![],
            reports: vec![],
            interner: Interner::new(),
            module_interners: FxHashMap::default(),
        }
    }

    /// Process a single .skald source file end-to-end.
    pub fn process_file(&mut self, path: &Path, src: &str) -> &Report {
        let file_id = alloc_file_id();
        let module_name = path.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();

        // Phase 1: lex + parse.
        let (module, parse_errors, lex_errors) = parse(src, file_id, module_name.clone());
        let interner_copy = module.interner.clone();
        self.module_interners.insert(path.to_path_buf(), module.interner.clone());

        // Phase 2: resolve (populates TypeDb).
        let mut resolver = Resolver::new(&mut self.db, &interner_copy);
        resolver.resolve_module(&module);
        let resolve_errors = resolver.errors.clone();
        drop(resolver);

        // Phase 3: borrowck.
        let mut bc = BorrowChecker::new(&self.db, &interner_copy);
        bc.check_module(&module);
        let borrow_errors = bc.errors.clone();
        drop(bc);

        // Phase 4: modifier validation.
        let mut modifier_errors = vec![];
        let (mut n_classes, mut n_structs, mut n_fns, mut n_traits, mut n_enums) = (0, 0, 0, 0, 0);
        for item in &module.items {
            match item {
                skald_ast::Item::Class(c) => {
                    n_classes += 1;
                    let (ef, errs) = compute_class(c);
                    if !errs.is_empty() {
                        for e in errs {
                            modifier_errors.push(format!("class `{}`: {:?}", interner_copy.lookup(c.name.ident), e));
                        }
                    }
                    let _ = ef;
                    // Per-field + per-method modifier checks.
                    let class_name = interner_copy.lookup(c.name.ident).to_string();
                    for m in &c.members {
                        match m {
                            skald_ast::ClassMember::Field(f) => {
                                let (_, errs) = compute_field(f, &class_name);
                                for e in errs {
                                    modifier_errors.push(format!("field `{}`: {:?}", interner_copy.lookup(f.name.ident), e));
                                }
                            }
                            skald_ast::ClassMember::Method(m) => {
                                let (_, errs) = compute_method(m, &class_name);
                                for e in errs {
                                    modifier_errors.push(format!("method `{}`: {:?}", interner_copy.lookup(m.name.ident), e));
                                }
                            }
                        }
                    }
                }
                skald_ast::Item::Struct(s) => {
                    n_structs += 1;
                    let (_, errs) = compute_struct(s);
                    for e in errs {
                        modifier_errors.push(format!("struct `{}`: {:?}", interner_copy.lookup(s.name.ident), e));
                    }
                    let class_name = interner_copy.lookup(s.name.ident).to_string();
                    for f in &s.fields {
                        let (_, errs) = compute_field(f, &class_name);
                        for e in errs {
                            modifier_errors.push(format!("field `{}`: {:?}", interner_copy.lookup(f.name.ident), e));
                        }
                    }
                    for m in &s.methods {
                        let (_, errs) = compute_method(m, &class_name);
                        for e in errs {
                            modifier_errors.push(format!("method `{}`: {:?}", interner_copy.lookup(m.name.ident), e));
                        }
                    }
                }
                skald_ast::Item::Enum(e) => {
                    n_enums += 1;
                    let (_, errs) = compute_enum(e);
                    for er in errs {
                        modifier_errors.push(format!("enum `{}`: {:?}", interner_copy.lookup(e.name.ident), er));
                    }
                }
                skald_ast::Item::Trait(t) => {
                    n_traits += 1;
                    let _ = t;
                }
                skald_ast::Item::Fn(f) => {
                    n_fns += 1;
                    let (_, errs) = compute_free_fn(f);
                    for e in errs {
                        modifier_errors.push(format!("fn `{}`: {:?}", interner_copy.lookup(f.name.ident), e));
                    }
                }
                _ => {}
            }
        }

        let items_count = module.items.len();
        self.modules.push((path.to_path_buf(), module));

        let report = Report {
            file: path.to_path_buf(),
            lex_errors,
            parse_errors,
            resolve_errors,
            borrow_errors,
            modifier_errors,
            items: items_count,
            top_level_classes: n_classes,
            top_level_structs: n_structs,
            top_level_fns: n_fns,
            top_level_traits: n_traits,
            top_level_enums: n_enums,
        };
        self.reports.push(report);
        self.reports.last().unwrap()
    }

    /// Run the pipeline over a directory of .skald files.
    pub fn process_dir(&mut self, dir: &Path) -> Vec<Report> {
        let mut paths = vec![];
        collect_skald_files(dir, &mut paths);
        paths.sort();
        let mut out = vec![];
        for p in paths {
            let src = std::fs::read_to_string(&p).unwrap_or_default();
            self.process_file(&p, &src);
            // Clone the most recent report so we can return owned values.
            if let Some(r) = self.reports.last() {
                out.push(r.clone());
            }
        }
        out
    }
}

fn collect_skald_files(dir: &Path, out: &mut Vec<PathBuf>) {
    if let Ok(entries) = std::fs::read_dir(dir) {
        for e in entries.flatten() {
            let p = e.path();
            if p.is_dir() {
                collect_skald_files(&p, out);
            } else if p.extension().map(|e| e == "skald").unwrap_or(false) {
                out.push(p);
            }
        }
    }
}

// ---------- Rendering ----------

pub fn render_report(report: &Report) -> String {
    let mut s = String::new();
    s.push_str(&format!("== {} ==\n", report.file.display()));
    s.push_str(&format!("  items: {} (classes={}, structs={}, enums={}, traits={}, fns={})\n",
        report.items, report.top_level_classes, report.top_level_structs,
        report.top_level_enums, report.top_level_traits, report.top_level_fns));
    if report.lex_errors.is_empty() && report.parse_errors.is_empty()
        && report.resolve_errors.is_empty() && report.borrow_errors.is_empty()
        && report.modifier_errors.is_empty() {
        s.push_str("  OK — no errors\n");
    } else {
        if !report.lex_errors.is_empty() {
            s.push_str(&format!("  -- {} lex errors --\n", report.lex_errors.len()));
            for e in &report.lex_errors {
                s.push_str(&format!("    {:?}\n", e));
            }
        }
        if !report.parse_errors.is_empty() {
            s.push_str(&format!("  -- {} parse errors --\n", report.parse_errors.len()));
            for e in &report.parse_errors {
                s.push_str(&format!("    [{}-{}]: {}\n", e.span.start, e.span.end, e.msg));
            }
        }
        if !report.resolve_errors.is_empty() {
            s.push_str(&format!("  -- {} resolve errors --\n", report.resolve_errors.len()));
            for e in &report.resolve_errors {
                s.push_str(&format!("    [{}-{}]: {}\n", e.span.start, e.span.end, e.msg));
            }
        }
        if !report.borrow_errors.is_empty() {
            s.push_str(&format!("  -- {} borrow errors --\n", report.borrow_errors.len()));
            for e in &report.borrow_errors {
                s.push_str(&format!("    {:?}\n", e));
            }
        }
        if !report.modifier_errors.is_empty() {
            s.push_str(&format!("  -- {} modifier errors --\n", report.modifier_errors.len()));
            for e in &report.modifier_errors {
                s.push_str(&format!("    {}\n", e));
            }
        }
    }
    s
}
