//! # skald-types
//!
//! Spec §5 — type system: Type, Trait, GenericBound, Substitution, unification.
//! Compiler-known traits (Reflectable, Send, Sync, POD, Copy, Drop).
//!
//! The resolved `Type` here is the post-resolution form (vs. the AST `Type`
//! which carries surface names). The type checker walks resolved HIR and
//! produces `Type` values for every expression.

use ena::unify::{InPlaceUnificationTable, NoError, UnifyKey, UnifyValue};
use rustc_hash::FxHashMap;
use skald_ast::{Ident, Name, PrimType, MathType, Type as AstType};

// ---------- Resolved Type ----------

#[derive(Debug, Clone, PartialEq)]
pub enum Type {
    // Primitives
    Prim(PrimType),
    Math(MathType),
    ByteSlice,
    Unit,
    Never,
    Bool,
    Str,
    Name,
    Text,
    /// Named user type — resolved to a `TypeDefId`.
    Named(TypeDefId),
    /// Generic application: `arr<T>`, `map<K,V>`, `Pool<AActor>`.
    App { base: TypeDefId, args: Vec<Type> },
    /// `opt<T>` / `T?`.
    Optional(Box<Type>),
    /// `ref<T>` non-null UObject ptr.
    Ref(Box<Type>),
    /// `weak<T>` nullable GC-cleared.
    Weak(Box<Type>),
    /// `soft<T>` lazy-loaded.
    Soft(Box<Type>),
    /// `subclass<T>`.
    Subclass(Box<Type>),
    /// `mut T`.
    Mut(Box<Type>),
    /// Raw pointer `*T` / `*mut T`.
    Ptr { ty: Box<Type>, mutable: bool },
    /// Function pointer / closure type.
    Fn { params: Vec<Type>, ret: Box<Type> },
    /// Multicast delegate.
    Delegate,
    /// Mass ECS query.
    Query(Vec<Type>),
    /// Tuple `(T, U, V)`.
    Tuple(Vec<Type>),
    /// Generic type variable (during inference).
    Var(TypeVar),
    /// Inference placeholder — unified later.
    Infer(InferVar),
    /// Self type (in trait/impl context).
    SelfTy,
    /// Error sentinel — propagates to avoid cascading errors.
    Err,
}

impl Type {
    pub fn is_unit(&self) -> bool { matches!(self, Type::Unit) }
    pub fn is_never(&self) -> bool { matches!(self, Type::Never) }
    pub fn is_err(&self) -> bool { matches!(self, Type::Err) }
    pub fn is_optional(&self) -> bool { matches!(self, Type::Optional(_)) }
    pub fn is_ref(&self) -> bool { matches!(self, Type::Ref(_)) }
    pub fn is_weak(&self) -> bool { matches!(self, Type::Weak(_)) }
    pub fn is_uobject_ref(&self) -> bool {
        matches!(self, Type::Ref(_) | Type::Weak(_) | Type::Soft(_) | Type::Subclass(_))
    }
    pub fn unwrap_optional(&self) -> Type {
        match self { Type::Optional(t) => (**t).clone(), _ => self.clone() }
    }
    pub fn is_send(&self) -> bool {
        // Per spec §5.5: UObjects are !Send by default. POD structs are Send + Sync if all fields are.
        match self {
            Type::Prim(_) | Type::Math(_) | Type::Bool | Type::ByteSlice => true,
            Type::Ref(_) | Type::Weak(_) | Type::Soft(_) | Type::Subclass(_) => false,
            Type::Optional(t) => t.is_send(),
            Type::Tuple(ts) => ts.iter().all(|t| t.is_send()),
            _ => false,
        }
    }
    pub fn is_sync(&self) -> bool { self.is_send() }
    pub fn is_pod(&self) -> bool {
        match self {
            Type::Prim(_) | Type::Math(_) | Type::Bool => true,
            Type::Tuple(ts) => ts.iter().all(|t| t.is_pod()),
            _ => false,
        }
    }
}

// ---------- IDs ----------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TypeDefId(pub u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TypeVar(pub u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct InferVar(pub u32);

// ---------- Type definitions ----------

#[derive(Debug, Clone)]
pub enum TypeDef {
    Class(ClassDef),
    Struct(StructDef),
    Enum(EnumDef),
    Trait(TraitDef),
    TypeAlias(TypeAliasDef),
    Primitive(PrimType),
}

#[derive(Debug, Clone)]
pub struct ClassDef {
    pub name: String,
    pub generics: Vec<TypeParamDef>,
    pub parent: Option<Type>,
    pub fields: Vec<FieldDef>,
    pub methods: Vec<MethodDef>,
    pub traits: Vec<TraitRef>,
}

#[derive(Debug, Clone)]
pub struct StructDef {
    pub name: String,
    pub generics: Vec<TypeParamDef>,
    pub fields: Vec<FieldDef>,
    pub methods: Vec<MethodDef>,
    pub is_pod: bool,
}

#[derive(Debug, Clone)]
pub struct EnumDef {
    pub name: String,
    pub base: Option<PrimType>,
    pub variants: Vec<EnumVariantDef>,
}

#[derive(Debug, Clone)]
pub struct EnumVariantDef {
    pub name: String,
    pub payload: Option<Vec<Type>>,
}

#[derive(Debug, Clone)]
pub struct TraitDef {
    pub name: String,
    pub generics: Vec<TypeParamDef>,
    pub supertraits: Vec<TraitRef>,
    pub methods: Vec<TraitMethodDef>,
}

#[derive(Debug, Clone)]
pub struct TraitMethodDef {
    pub name: String,
    pub params: Vec<(String, Type)>,
    pub ret: Type,
    pub has_default: bool,
}

#[derive(Debug, Clone)]
pub struct TypeAliasDef {
    pub name: String,
    pub generics: Vec<TypeParamDef>,
    pub alias: Type,
}

#[derive(Debug, Clone)]
pub struct TypeParamDef {
    pub name: String,
    pub bounds: Vec<TraitRef>,
    pub default: Option<Type>,
}

#[derive(Debug, Clone)]
pub struct FieldDef {
    pub name: String,
    pub ty: Type,
    pub vis: skald_ast::Visibility,
}

#[derive(Debug, Clone)]
pub struct MethodDef {
    pub name: String,
    pub generics: Vec<TypeParamDef>,
    pub params: Vec<(String, Type)>,
    pub ret: Type,
    pub dispatch: skald_ast::MethodDispatch,
    pub vis: skald_ast::Visibility,
}

#[derive(Debug, Clone)]
pub struct TraitRef {
    pub trait_id: TypeDefId,
    pub args: Vec<Type>,
}

// ---------- TypeDb ----------

#[derive(Debug, Default)]
pub struct TypeDb {
    pub defs: Vec<TypeDef>,
    pub by_name: FxHashMap<String, TypeDefId>,
    pub impls: Vec<TraitImpl>,
    /// Auto-impls: `Reflectable` for any pub var type.
    pub reflectable: Vec<TypeDefId>,
    pub send_types: Vec<TypeDefId>,
    pub pod_types: Vec<TypeDefId>,
}

impl TypeDb {
    pub fn new() -> Self { Self::default() }
    pub fn add(&mut self, def: TypeDef) -> TypeDefId {
        let id = TypeDefId(self.defs.len() as u32);
        let name = match &def {
            TypeDef::Class(c) => c.name.clone(),
            TypeDef::Struct(s) => s.name.clone(),
            TypeDef::Enum(e) => e.name.clone(),
            TypeDef::Trait(t) => t.name.clone(),
            TypeDef::TypeAlias(a) => a.name.clone(),
            TypeDef::Primitive(p) => format!("{:?}", p),
        };
        self.by_name.insert(name, id);
        self.defs.push(def);
        id
    }
    pub fn lookup(&self, name: &str) -> Option<TypeDefId> {
        self.by_name.get(name).copied()
    }
    pub fn get(&self, id: TypeDefId) -> &TypeDef {
        &self.defs[id.0 as usize]
    }
    pub fn add_impl(&mut self, imp: TraitImpl) {
        self.impls.push(imp);
    }
    /// Does `target` implement `trait_id`?
    pub fn implements(&self, target: Type, trait_id: TypeDefId) -> bool {
        self.impls.iter().any(|i| i.trait_id == trait_id && i.target == target)
            || self.builtin_impl(&target, trait_id)
    }
    fn builtin_impl(&self, ty: &Type, trait_id: TypeDefId) -> bool {
        let name = match self.get(trait_id) {
            TypeDef::Trait(t) => t.name.as_str(),
            _ => return false,
        };
        match name {
            "Reflectable" => ty.is_ref() || ty.is_weak() || matches!(ty, Type::Prim(_) | Type::Math(_) | Type::Str | Type::Bool | Type::Name | Type::Text),
            "Send" => ty.is_send(),
            "Sync" => ty.is_sync(),
            "POD" => ty.is_pod(),
            "Copy" => ty.is_pod(),
            _ => false,
        }
    }
}

#[derive(Debug, Clone)]
pub struct TraitImpl {
    pub trait_id: TypeDefId,
    pub target: Type,
}

// ---------- Unification ----------

#[derive(Debug, Clone, PartialEq)]
pub enum UnifyError {
    TypeMismatch { expected: Type, got: Type },
    OccursCheck { var: InferVar, ty: Type },
    ArityMismatch { expected: usize, got: usize },
    TraitNotSatisfied { ty: Type, trait_name: String },
}

impl UnifyKey for InferVar {
    type Value = InferValue;
    fn index(&self) -> u32 { self.0 }
    fn from_index(u: u32) -> Self { InferVar(u) }
    fn tag() -> &'static str { "InferVar" }
}

/// Newtype around `Option<Type>` so we can `impl UnifyValue` (orphan rule).
#[derive(Debug, Clone, PartialEq)]
pub struct InferValue(pub Option<Type>);

impl UnifyValue for InferValue {
    type Error = NoError;
    fn unify_values(value1: &Self, value2: &Self) -> Result<Self, NoError> {
        // For Skald's type checker, we want union semantics: if either side is
        // `None`, take the other; if both are `Some`, prefer the first (the
        // conflict-resolution happens at a higher level via the explicit
        // `unify` method which calls `unify` recursively first).
        match (&value1.0, &value2.0) {
            (None, _) => Ok(value2.clone()),
            (_, None) => Ok(value1.clone()),
            (Some(a), Some(b)) if a == b => Ok(value1.clone()),
            // Mismatch — we keep the first; the higher-level unify() will
            // return a TypeMismatch error before calling union_value.
            _ => Ok(value1.clone()),
        }
    }
}

pub struct Unifier {
    table: InPlaceUnificationTable<InferVar>,
    next_var: u32,
    pub errors: Vec<UnifyError>,
}

impl Default for Unifier {
    fn default() -> Self {
        Self { table: InPlaceUnificationTable::<InferVar>::new(), next_var: 0, errors: vec![] }
    }
}

impl Unifier {
    pub fn new() -> Self { Self::default() }
    pub fn fresh(&mut self) -> Type {
        let v = InferVar(self.next_var);
        self.next_var += 1;
        self.table.new_key(InferValue(None));
        Type::Infer(v)
    }
    pub fn unify(&mut self, a: &Type, b: &Type) -> Result<(), UnifyError> {
        let a = self.resolve(a);
        let b = self.resolve(b);
        match (&a, &b) {
            (Type::Err, _) | (_, Type::Err) => Ok(()),
            (Type::Infer(v), _) => {
                self.occurs_check(v, &b)?;
                self.table.union_value(*v, InferValue(Some(b.clone())));
                Ok(())
            }
            (_, Type::Infer(v)) => {
                self.occurs_check(v, &a)?;
                self.table.union_value(*v, InferValue(Some(a.clone())));
                Ok(())
            }
            (Type::Optional(x), Type::Optional(y)) => self.unify(x, y),
            (Type::Ref(x), Type::Ref(y)) => self.unify(x, y),
            (Type::Weak(x), Type::Weak(y)) => self.unify(x, y),
            (Type::Soft(x), Type::Soft(y)) => self.unify(x, y),
            (Type::Subclass(x), Type::Subclass(y)) => self.unify(x, y),
            (Type::Mut(x), Type::Mut(y)) => self.unify(x, y),
            (Type::Ptr { ty: x, mutable: mx }, Type::Ptr { ty: y, mutable: my }) if mx == my => self.unify(x, y),
            (Type::Tuple(xs), Type::Tuple(ys)) if xs.len() == ys.len() => {
                for (x, y) in xs.iter().zip(ys.iter()) { self.unify(x, y)?; }
                Ok(())
            }
            (Type::Fn { params: px, ret: rx }, Type::Fn { params: py, ret: ry }) if px.len() == py.len() => {
                for (x, y) in px.iter().zip(py.iter()) { self.unify(x, y)?; }
                self.unify(rx, ry)
            }
            (Type::App { base: xb, args: xa }, Type::App { base: yb, args: ya }) if xb == yb && xa.len() == ya.len() => {
                for (x, y) in xa.iter().zip(ya.iter()) { self.unify(x, y)?; }
                Ok(())
            }
            _ if a == b => Ok(()),
            _ => Err(UnifyError::TypeMismatch { expected: a.clone(), got: b.clone() }),
        }
    }
    fn occurs_check(&mut self, v: &InferVar, ty: &Type) -> Result<(), UnifyError> {
        let ty = self.resolve(ty);
        match &ty {
            Type::Infer(v2) if v2 == v => Err(UnifyError::OccursCheck { var: *v, ty }),
            Type::Optional(t) | Type::Ref(t) | Type::Weak(t) | Type::Soft(t) | Type::Subclass(t) | Type::Mut(t) => self.occurs_check(v, t),
            Type::Ptr { ty: t, .. } => self.occurs_check(v, t),
            Type::Tuple(ts) => { for t in ts { self.occurs_check(v, t)?; } Ok(()) }
            Type::Fn { params, ret } => {
                for t in params { self.occurs_check(v, t)?; }
                self.occurs_check(v, ret)
            }
            _ => Ok(()),
        }
    }
    pub fn resolve(&mut self, ty: &Type) -> Type {
        match ty {
            Type::Infer(v) => {
                let val = self.table.probe_value(*v);
                match val.0 {
                    Some(t) => self.resolve(&t),
                    None => ty.clone(),
                }
            }
            Type::Optional(t) => Type::Optional(Box::new(self.resolve(t))),
            Type::Ref(t) => Type::Ref(Box::new(self.resolve(t))),
            Type::Weak(t) => Type::Weak(Box::new(self.resolve(t))),
            Type::Soft(t) => Type::Soft(Box::new(self.resolve(t))),
            Type::Subclass(t) => Type::Subclass(Box::new(self.resolve(t))),
            Type::Mut(t) => Type::Mut(Box::new(self.resolve(t))),
            Type::Ptr { ty: t, mutable } => Type::Ptr { ty: Box::new(self.resolve(t)), mutable: *mutable },
            Type::Tuple(ts) => Type::Tuple(ts.iter().map(|t| self.resolve(t)).collect()),
            Type::Fn { params, ret } => Type::Fn { params: params.iter().map(|t| self.resolve(t)).collect(), ret: Box::new(self.resolve(ret)) },
            Type::App { base, args } => Type::App { base: *base, args: args.iter().map(|t| self.resolve(t)).collect() },
            _ => ty.clone(),
        }
    }
}

// ---------- AST Type → resolved Type conversion ----------

pub fn resolve_ast_type(ast: &AstType, db: &TypeDb) -> Type {
    match ast {
        AstType::Prim(p) => match p {
            PrimType::Bool => Type::Bool,
            PrimType::Str => Type::Str,
            PrimType::Name => Type::Name,
            PrimType::Text => Type::Text,
            PrimType::Void => Type::Unit,
            PrimType::Never => Type::Never,
            _ => Type::Prim(*p),
        },
        AstType::Math(m) => Type::Math(*m),
        AstType::ByteSlice => Type::ByteSlice,
        AstType::Unit => Type::Unit,
        AstType::Infer => Type::Err, // Should be replaced during inference
        AstType::SelfTy => Type::SelfTy,
        AstType::Named(n) => {
            // We need the name string; but Type only carries Ident. Need access to interner.
            // For now, look up by index — caller should provide name.
            let _ = n;
            Type::Err
        }
        AstType::App { base, args } => {
            // Recurse on base, expect Named.
            match &**base {
                AstType::Named(_) => {
                    let _ = args;
                    Type::Err
                }
                _ => Type::Err,
            }
        }
        AstType::Optional(t) => Type::Optional(Box::new(resolve_ast_type(t, db))),
        AstType::Ref(t) => Type::Ref(Box::new(resolve_ast_type(t, db))),
        AstType::Weak(t) => Type::Weak(Box::new(resolve_ast_type(t, db))),
        AstType::Soft(t) => Type::Soft(Box::new(resolve_ast_type(t, db))),
        AstType::Subclass(t) => Type::Subclass(Box::new(resolve_ast_type(t, db))),
        AstType::Mut(t) => Type::Mut(Box::new(resolve_ast_type(t, db))),
        AstType::Ptr { ty, mutable } => Type::Ptr { ty: Box::new(resolve_ast_type(ty, db)), mutable: *mutable },
        AstType::Fn { params, ret } => Type::Fn {
            params: params.iter().map(|t| resolve_ast_type(t, db)).collect(),
            ret: Box::new(resolve_ast_type(ret, db)),
        },
        AstType::Delegate => Type::Delegate,
        AstType::Query(args) => Type::Query(args.iter().map(|t| resolve_ast_type(t, db)).collect()),
        AstType::Tuple(ts) => Type::Tuple(ts.iter().map(|t| resolve_ast_type(t, db)).collect()),
    }
}

/// Like `resolve_ast_type` but with name lookup.
pub fn resolve_ast_type_with_names(ast: &AstType, db: &TypeDb, interner: &skald_ast::Interner) -> Type {
    match ast {
        AstType::Named(n) => {
            let name = interner.lookup(n.ident);
            if let Some(id) = db.lookup(name) {
                Type::Named(id)
            } else {
                Type::Err
            }
        }
        AstType::App { base, args } => {
            if let AstType::Named(n) = &**base {
                let name = interner.lookup(n.ident);
                if let Some(id) = db.lookup(name) {
                    return Type::App { base: id, args: args.iter().map(|t| resolve_ast_type_with_names(t, db, interner)).collect() };
                }
            }
            Type::Err
        }
        AstType::Optional(t) => Type::Optional(Box::new(resolve_ast_type_with_names(t, db, interner))),
        AstType::Ref(t) => Type::Ref(Box::new(resolve_ast_type_with_names(t, db, interner))),
        AstType::Weak(t) => Type::Weak(Box::new(resolve_ast_type_with_names(t, db, interner))),
        AstType::Soft(t) => Type::Soft(Box::new(resolve_ast_type_with_names(t, db, interner))),
        AstType::Subclass(t) => Type::Subclass(Box::new(resolve_ast_type_with_names(t, db, interner))),
        AstType::Mut(t) => Type::Mut(Box::new(resolve_ast_type_with_names(t, db, interner))),
        AstType::Ptr { ty, mutable } => Type::Ptr { ty: Box::new(resolve_ast_type_with_names(ty, db, interner)), mutable: *mutable },
        AstType::Fn { params, ret } => Type::Fn {
            params: params.iter().map(|t| resolve_ast_type_with_names(t, db, interner)).collect(),
            ret: Box::new(resolve_ast_type_with_names(ret, db, interner)),
        },
        AstType::Delegate => Type::Delegate,
        AstType::Query(args) => Type::Query(args.iter().map(|t| resolve_ast_type_with_names(t, db, interner)).collect()),
        AstType::Tuple(ts) => Type::Tuple(ts.iter().map(|t| resolve_ast_type_with_names(t, db, interner)).collect()),
        _ => resolve_ast_type(ast, db),
    }
}

// ---------- Default integer/float literal types ----------

/// Spec §4.7: 123 → i32, 1.0 → f64.
pub fn default_int_type() -> Type { Type::Prim(PrimType::I32) }
pub fn default_float_type() -> Type { Type::Prim(PrimType::F64) }

// ---------- Pretty-print ----------

pub fn display_type(ty: &Type) -> String {
    match ty {
        Type::Prim(p) => format!("{:?}", p).to_lowercase(),
        Type::Math(m) => format!("{:?}", m).to_lowercase(),
        Type::Bool => "bool".into(),
        Type::Str => "str".into(),
        Type::Name => "name".into(),
        Type::Text => "text".into(),
        Type::ByteSlice => "[u8]".into(),
        Type::Unit => "()".into(),
        Type::Never => "!".into(),
        Type::Err => "<error>".into(),
        Type::Named(id) => format!("#{}", id.0),
        Type::App { base, args } => {
            let args_s = args.iter().map(display_type).collect::<Vec<_>>().join(", ");
            format!("#{}<{}>", base.0, args_s)
        }
        Type::Optional(t) => format!("{}?", display_type(t)),
        Type::Ref(t) => format!("ref<{}>", display_type(t)),
        Type::Weak(t) => format!("weak<{}>", display_type(t)),
        Type::Soft(t) => format!("soft<{}>", display_type(t)),
        Type::Subclass(t) => format!("subclass<{}>", display_type(t)),
        Type::Mut(t) => format!("mut {}", display_type(t)),
        Type::Ptr { ty, mutable } => format!("*{} {}", if *mutable { "mut" } else { "" }, display_type(ty)),
        Type::Fn { params, ret } => format!("fn({}) -> {}", params.iter().map(display_type).collect::<Vec<_>>().join(", "), display_type(ret)),
        Type::Delegate => "delegate".into(),
        Type::Query(args) => format!("query<{}>", args.iter().map(display_type).collect::<Vec<_>>().join(", ")),
        Type::Tuple(ts) => format!("({})", ts.iter().map(display_type).collect::<Vec<_>>().join(", ")),
        Type::Var(v) => format!("'T{}", v.0),
        Type::Infer(v) => format!("?{}", v.0),
        Type::SelfTy => "Self".into(),
    }
}

// ---------- Unused stubs to keep imports happy ----------

#[allow(dead_code)]
fn _unused_ident(_i: Ident, _n: Name) {}
