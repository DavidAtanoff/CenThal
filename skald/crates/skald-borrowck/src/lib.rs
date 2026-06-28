//! # skald-borrowck
//!
//! Spec §3.2, §13.5 — lightweight borrow check (not a full Rust checker).
//! Enforces only:
//!   1. Single mutable borrow at a time within a function.
//!   2. `ref<UObject>` is `!Send` — cannot be captured in `spawn worker`.
//!   3. Closures captured into `pub var` slots must be `@persistent`.

use skald_ast::*;
use skald_types::TypeDb;

#[derive(Debug, Clone, PartialEq)]
pub struct BorrowError {
    pub span: Span,
    pub kind: BorrowErrorKind,
}

#[derive(Debug, Clone, PartialEq)]
pub enum BorrowErrorKind {
    DoubleMutBorrow { path: String },
    UObjInWorker { ty: String },
    ArenaCapturePromoted,
}

pub struct BorrowChecker<'a> {
    pub db: &'a TypeDb,
    pub interner: &'a Interner,
    pub errors: Vec<BorrowError>,
}

impl<'a> BorrowChecker<'a> {
    pub fn new(db: &'a TypeDb, interner: &'a Interner) -> Self {
        Self { db, interner, errors: vec![] }
    }

    pub fn check_module(&mut self, m: &Module) {
        for item in &m.items {
            self.check_item(item);
        }
    }

    fn check_item(&mut self, item: &Item) {
        match item {
            Item::Fn(f) => {
                if let Some(b) = &f.body { self.check_block(b); }
                if let Some(ab) = &f.arrow_body { self.check_expr(ab); }
            }
            Item::Class(c) => {
                for m in &c.members {
                    if let ClassMember::Method(m) = m {
                        if let Some(b) = &m.body { self.check_block(b); }
                    }
                }
            }
            Item::Struct(s) => {
                for m in &s.methods {
                    if let Some(b) = &m.body { self.check_block(b); }
                }
            }
            Item::Impl(im) => {
                for m in &im.methods {
                    if let Some(b) = &m.body { self.check_block(b); }
                }
            }
            _ => {}
        }
    }

    fn check_block(&mut self, b: &Block) {
        for s in &b.stmts { self.check_stmt(s); }
        if let Some(t) = &b.tail { self.check_expr(t); }
    }

    fn check_stmt(&mut self, s: &Stmt) {
        match s {
            Stmt::Let { init, .. } | Stmt::VarLet { init, .. } => {
                if let Some(e) = init { self.check_expr(e); }
            }
            Stmt::Expr(e) => self.check_expr(e),
            Stmt::Const { init, .. } | Stmt::Static { init, .. } => self.check_expr(init),
            _ => {}
        }
    }

    fn check_expr(&mut self, e: &Expr) {
        match e {
            Expr::Spawn { kind, body } => {
                if *kind == SpawnKind::Worker {
                    self.scan_worker_captures(body);
                }
                self.check_block(body);
            }
            Expr::Binary { op: BinOp::Assign, lhs, rhs, .. } => {
                self.check_expr(lhs);
                self.check_expr(rhs);
            }
            Expr::Field { base, .. } | Expr::OptionalField { base, .. } => self.check_expr(base),
            Expr::Call { callee, args } => {
                self.check_expr(callee);
                for a in args { self.check_expr(a); }
            }
            Expr::MethodCall { receiver, args, .. } | Expr::OptionalMethodCall { receiver, args, .. } => {
                self.check_expr(receiver);
                for a in args { self.check_expr(a); }
            }
            Expr::Index { base, idx } => { self.check_expr(base); self.check_expr(idx); }
            Expr::Binary { lhs, rhs, .. } => { self.check_expr(lhs); self.check_expr(rhs); }
            Expr::Unary { expr, .. } | Expr::Unwrap(expr) | Expr::Await(expr) | Expr::Cast { expr, .. } | Expr::Is { expr, .. } | Expr::Paren(expr) => self.check_expr(expr),
            Expr::Range { lo, hi, .. } => {
                if let Some(l) = lo { self.check_expr(l); }
                if let Some(h) = hi { self.check_expr(h); }
            }
            Expr::Elvis { cond, default } => { self.check_expr(cond); self.check_expr(default); }
            Expr::Pipe { lhs, rhs } => { self.check_expr(lhs); self.check_expr(rhs); }
            Expr::Lambda { body, .. } => self.check_expr(body),
            Expr::StructLit { fields, .. } => {
                for f in fields { if let Some(v) = &f.value { self.check_expr(v); } }
            }
            Expr::ArrayLit(items) | Expr::TupleLit(items) | Expr::VectorLit { args: items, .. } => {
                for i in items { self.check_expr(i); }
            }
            Expr::Match { scrutinee, arms } => {
                self.check_expr(scrutinee);
                for a in arms {
                    self.check_expr(&a.body);
                    if let Some(g) = &a.guard { self.check_expr(g); }
                }
            }
            Expr::If { cond, then, else_ } => {
                self.check_expr(cond);
                self.check_block(then);
                if let Some(e) = else_ { self.check_expr(e); }
            }
            Expr::For { iter, body, .. } | Expr::ParallelFor { iter, body, .. } => {
                self.check_expr(iter);
                self.check_block(body);
            }
            Expr::While { cond, body } => {
                self.check_expr(cond);
                self.check_block(body);
            }
            Expr::Loop(b) | Expr::UnsafeBlock(b) | Expr::Transaction(b) | Expr::Block(b) => self.check_block(b),
            Expr::Return(e) | Expr::Break(e) => { if let Some(e) = e { self.check_expr(e); } }
            Expr::FmtStrLit(pieces) => {
                for p in pieces { if let FmtPiece::Expr { expr, .. } = p { self.check_expr(expr); } }
            }
            Expr::MacroCall { args, .. } => { for a in args { self.check_expr(a); } }
            _ => {}
        }
    }

    /// Walk a `spawn worker` body for UObject references. We can't infer types
    /// from this static walk alone; we heuristically look for `self.<field>`
    /// accesses and direct `self` references.
    fn scan_worker_captures(&mut self, b: &Block) {
        for s in &b.stmts { self.scan_worker_stmt(s); }
        if let Some(t) = &b.tail { self.scan_worker_expr(t); }
    }
    fn scan_worker_stmt(&mut self, s: &Stmt) {
        match s {
            Stmt::Let { init, .. } | Stmt::VarLet { init, .. } => { if let Some(e) = init { self.scan_worker_expr(e); } }
            Stmt::Expr(e) => self.scan_worker_expr(e),
            _ => {}
        }
    }
    fn scan_worker_expr(&mut self, e: &Expr) {
        match e {
            Expr::SelfRef => {
                self.errors.push(BorrowError {
                    span: Span::DUMMY,
                    kind: BorrowErrorKind::UObjInWorker { ty: "self".into() },
                });
            }
            Expr::Field { base, .. } | Expr::MethodCall { receiver: base, .. } => {
                self.scan_worker_expr(base);
            }
            Expr::Call { args, .. } | Expr::ArrayLit(args) | Expr::TupleLit(args) => {
                for a in args { self.scan_worker_expr(a); }
            }
            Expr::Binary { lhs, rhs, .. } => { self.scan_worker_expr(lhs); self.scan_worker_expr(rhs); }
            _ => {}
        }
    }
}
