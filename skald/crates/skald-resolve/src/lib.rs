//! # skald-resolve
//!
//! Spec §3.2 — name resolution: maps identifiers to declarations.
//! Two-phase:
//!   Phase 1 (module-level): collect all top-level items into a symbol table.
//!   Phase 2 (function bodies): walk local scopes + field accesses (light).

use rustc_hash::FxHashMap;
use skald_ast::*;
use skald_types::{TypeDb, TypeDef, ClassDef, StructDef, EnumDef, EnumVariantDef, TraitDef, TypeAliasDef, TypeParamDef};

// ---------- Symbol ----------

#[derive(Debug, Clone, PartialEq)]
pub enum Symbol {
    Item(ItemId),
    Local { depth: u32, idx: u32 },
    Builtin(&'static str),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ItemId(pub u32);

// ---------- Resolver ----------

pub struct Resolver<'a> {
    pub db: &'a mut TypeDb,
    pub interner: &'a Interner,
    pub items: Vec<Item>,
    pub item_map: FxHashMap<String, ItemId>,
    pub errors: Vec<ResolveError>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ResolveError {
    pub span: Span,
    pub msg: String,
    pub suggestion: Option<String>,
}

impl<'a> Resolver<'a> {
    pub fn new(db: &'a mut TypeDb, interner: &'a Interner) -> Self {
        Self {
            db,
            interner,
            items: vec![],
            item_map: FxHashMap::default(),
            errors: vec![],
        }
    }

    pub fn resolve_module(&mut self, m: &Module) {
        // Phase 1: collect top-level items.
        for (i, item) in m.items.iter().enumerate() {
            let id = ItemId(i as u32);
            self.items.push(item.clone());
            if let Some(name) = self.item_name(item) {
                self.item_map.insert(name, id);
                self.populate_type_db(id, item);
            }
        }
        // Phase 2: walk function bodies for sanity (no local-scope tracking yet).
        for (i, item) in m.items.iter().enumerate() {
            self.resolve_item_locals(ItemId(i as u32), item);
        }
    }

    fn item_name(&self, item: &Item) -> Option<String> {
        let name = match item {
            Item::Class(c) => self.interner.lookup(c.name.ident),
            Item::Struct(s) => self.interner.lookup(s.name.ident),
            Item::Enum(e) => self.interner.lookup(e.name.ident),
            Item::Trait(t) => self.interner.lookup(t.name.ident),
            Item::Fn(f) => self.interner.lookup(f.name.ident),
            Item::Const { name, .. } => self.interner.lookup(name.ident),
            Item::Static(g) => self.interner.lookup(g.name.ident),
            Item::TypeAlias { name, .. } => self.interner.lookup(name.ident),
            Item::Extern(e) => self.interner.lookup(e.name.ident),
            Item::Mod { name, .. } | Item::ModRef { name, .. } => self.interner.lookup(name.ident),
            _ => return None,
        };
        Some(name.to_string())
    }

    fn populate_type_db(&mut self, _id: ItemId, item: &Item) {
        match item {
            Item::Class(c) => {
                let def = ClassDef {
                    name: self.interner.lookup(c.name.ident).to_string(),
                    generics: c.generics.iter().map(|p| TypeParamDef {
                        name: self.interner.lookup(p.name.ident).to_string(),
                        bounds: vec![],
                        default: p.default.as_ref().map(|_| skald_types::Type::Err),
                    }).collect(),
                    parent: None,
                    fields: vec![],
                    methods: vec![],
                    traits: vec![],
                };
                self.db.add(TypeDef::Class(def));
            }
            Item::Struct(s) => {
                let def = StructDef {
                    name: self.interner.lookup(s.name.ident).to_string(),
                    generics: vec![],
                    fields: vec![],
                    methods: vec![],
                    is_pod: s.attrs.iter().any(|a| matches!(a, Annotation::Pod)),
                };
                self.db.add(TypeDef::Struct(def));
            }
            Item::Enum(e) => {
                let def = EnumDef {
                    name: self.interner.lookup(e.name.ident).to_string(),
                    base: e.base,
                    variants: e.variants.iter().map(|v| EnumVariantDef {
                        name: self.interner.lookup(v.name.ident).to_string(),
                        payload: None,
                    }).collect(),
                };
                self.db.add(TypeDef::Enum(def));
            }
            Item::Trait(t) => {
                let def = TraitDef {
                    name: self.interner.lookup(t.name.ident).to_string(),
                    generics: vec![],
                    supertraits: vec![],
                    methods: vec![],
                };
                self.db.add(TypeDef::Trait(def));
            }
            Item::TypeAlias { name, alias, .. } => {
                let def = TypeAliasDef {
                    name: self.interner.lookup(name.ident).to_string(),
                    generics: vec![],
                    alias: skald_types::resolve_ast_type_with_names(alias, self.db, self.interner),
                };
                self.db.add(TypeDef::TypeAlias(def));
            }
            _ => {}
        }
    }

    fn resolve_item_locals(&mut self, _id: ItemId, item: &Item) {
        match item {
            Item::Fn(f) => {
                if let Some(body) = &f.body {
                    self.resolve_block(body);
                } else if let Some(arr) = &f.arrow_body {
                    self.resolve_expr(arr);
                }
            }
            Item::Class(c) => {
                for m in &c.members {
                    if let ClassMember::Method(method) = m {
                        if let Some(body) = &method.body {
                            self.resolve_block(body);
                        }
                    }
                }
            }
            Item::Struct(s) => {
                for m in &s.methods {
                    if let Some(body) = &m.body {
                        self.resolve_block(body);
                    }
                }
            }
            Item::Impl(im) => {
                for m in &im.methods {
                    if let Some(body) = &m.body {
                        self.resolve_block(body);
                    }
                }
            }
            _ => {}
        }
    }

    fn resolve_block(&mut self, block: &Block) {
        for s in &block.stmts {
            self.resolve_stmt(s);
        }
        if let Some(tail) = &block.tail {
            self.resolve_expr(tail);
        }
    }

    fn resolve_stmt(&mut self, s: &Stmt) {
        match s {
            Stmt::Let { init, .. } | Stmt::VarLet { init, .. } => {
                if let Some(e) = init { self.resolve_expr(e); }
            }
            Stmt::Expr(e) => self.resolve_expr(e),
            Stmt::Const { init, .. } => self.resolve_expr(init),
            Stmt::Static { init, .. } => self.resolve_expr(init),
            _ => {}
        }
    }

    fn resolve_expr(&mut self, e: &Expr) {
        match e {
            Expr::Ident(n) => {
                let name = self.interner.lookup(n.ident);
                if !self.item_map.contains_key(name) {
                    let known = matches!(name, "log" | "math" | "io" | "mem" | "region" | "world" | "mass" | "game_thread" | "ue_log");
                    if !known {
                        // Don't push — locals aren't tracked, would cause false positives.
                    }
                }
            }
            Expr::Field { base, .. } | Expr::OptionalField { base, .. } => self.resolve_expr(base),
            Expr::Call { callee, args } => {
                self.resolve_expr(callee);
                for a in args { self.resolve_expr(a); }
            }
            Expr::MethodCall { receiver, args, .. } | Expr::OptionalMethodCall { receiver, args, .. } => {
                self.resolve_expr(receiver);
                for a in args { self.resolve_expr(a); }
            }
            Expr::Index { base, idx } => { self.resolve_expr(base); self.resolve_expr(idx); }
            Expr::Binary { lhs, rhs, .. } => { self.resolve_expr(lhs); self.resolve_expr(rhs); }
            Expr::Unary { expr, .. } | Expr::Unwrap(expr) | Expr::Await(expr) => self.resolve_expr(expr),
            Expr::Cast { expr, .. } | Expr::Is { expr, .. } => self.resolve_expr(expr),
            Expr::Range { lo, hi, .. } => {
                if let Some(l) = lo { self.resolve_expr(l); }
                if let Some(h) = hi { self.resolve_expr(h); }
            }
            Expr::Elvis { cond, default } => { self.resolve_expr(cond); self.resolve_expr(default); }
            Expr::Pipe { lhs, rhs } => { self.resolve_expr(lhs); self.resolve_expr(rhs); }
            Expr::Lambda { body, .. } => self.resolve_expr(body),
            Expr::StructLit { fields, .. } => {
                for f in fields { if let Some(v) = &f.value { self.resolve_expr(v); } }
            }
            Expr::ArrayLit(items) | Expr::TupleLit(items) => {
                for i in items { self.resolve_expr(i); }
            }
            Expr::Paren(e) => self.resolve_expr(e),
            Expr::Match { scrutinee, arms } => {
                self.resolve_expr(scrutinee);
                for a in arms { self.resolve_expr(&a.body); if let Some(g) = &a.guard { self.resolve_expr(g); } }
            }
            Expr::If { cond, then, else_ } => {
                self.resolve_expr(cond);
                self.resolve_block(then);
                if let Some(e) = else_ { self.resolve_expr(e); }
            }
            Expr::For { iter, body, .. } => { self.resolve_expr(iter); self.resolve_block(body); }
            Expr::While { cond, body } => { self.resolve_expr(cond); self.resolve_block(body); }
            Expr::Loop(b) | Expr::UnsafeBlock(b) | Expr::Transaction(b) | Expr::Block(b) | Expr::Spawn { body: b, .. } => self.resolve_block(b),
            Expr::Return(e) | Expr::Break(e) => { if let Some(e) = e { self.resolve_expr(e); } }
            Expr::Cont | Expr::NullLit | Expr::UnitLit | Expr::BoolLit(_) | Expr::IntLit { .. } | Expr::FloatLit { .. } | Expr::StrLit(_) | Expr::CharLit(_) | Expr::ByteStrLit(_) | Expr::SelfRef | Expr::SuperRef => {}
            Expr::VectorLit { args, .. } => { for a in args { self.resolve_expr(a); } }
            Expr::Path(_) | Expr::PathCall { .. } => {}
            Expr::ParallelFor { iter, body, .. } => { self.resolve_expr(iter); self.resolve_block(body); }
            Expr::FmtStrLit(pieces) => {
                for p in pieces {
                    if let FmtPiece::Expr { expr, .. } = p { self.resolve_expr(expr); }
                }
            }
            Expr::MacroCall { args, .. } => { for a in args { self.resolve_expr(a); } }
        }
    }
}
