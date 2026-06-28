//! # skald-ast
//!
//! Pure data structures for the Skald language. No logic, no parsing.
//! Spec reference: §3.2, §4, §5, §6, §7.
//!
//! Everything is `#[derive(Debug, Clone)]` and uses `Box`/`SmallVec` to keep
//! `Node` sizes reasonable. Spans are stored out-of-line in a side table for
//! the parser; the AST itself is span-free for cheap cloning. (Span retrieval
//! via a `SpanMap` is the rust-analyzer pattern.)

#![allow(clippy::needless_lifetimes)]
#![allow(clippy::enum_variant_names)]

use rustc_hash::FxHashMap;
use serde::{Deserialize, Serialize};

// ---------- IDs ----------

pub type NodeId = u32;
pub type FileId = u32;

/// Byte-offset span into a source file. End-exclusive.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Span {
    pub file: FileId,
    pub start: u32,
    pub end: u32,
}

impl Span {
    pub const DUMMY: Span = Span { file: 0, start: 0, end: 0 };
    pub fn new(file: FileId, start: u32, end: u32) -> Self {
        Span { file, start, end }
    }
    pub fn dummy() -> Self { Span::DUMMY }
    pub fn len(&self) -> u32 { self.end.saturating_sub(self.start) }
    pub fn is_dummy(&self) -> bool { *self == Span::DUMMY }
    pub fn contains(&self, other: Span) -> bool {
        self.file == other.file && self.start <= other.start && self.end >= other.end
    }
}

// ---------- Identifiers ----------

/// Interned identifier. The lexer/parser interning table maps these to
/// `&'static str`-equivalents at runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Ident(pub u32);

impl Ident {
    pub const DUMMY: Ident = Ident(u32::MAX);
}

/// A name in source order with its span.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Name {
    pub ident: Ident,
    pub span: Span,
}

impl Name {
    pub fn new(ident: Ident, span: Span) -> Self { Name { ident, span } }
    pub fn dummy() -> Self { Name { ident: Ident::DUMMY, span: Span::DUMMY } }
}

// ---------- Visibility ----------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Visibility {
    /// `pub` — reflected by default.
    Pub,
    /// `protected` — reflected with BlueprintProtected.
    Protected,
    /// `private` (explicit) or no keyword (implicit).
    Private,
}

impl Default for Visibility {
    fn default() -> Self { Visibility::Private }
}

impl Visibility {
    pub fn is_reflected(self) -> bool {
        matches!(self, Visibility::Pub | Visibility::Protected)
    }
}

// ---------- Annotations (`@`, Skald-native) ----------

/// Spec §2.3, §5.6.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Annotation {
    /// `@region("name")` — named arena.
    Region(String),
    /// `@arena` — anonymous scope-local bump allocator.
    Arena,
    /// `@simd` — vectorizable loop body.
    Simd,
    /// `@layout(soa)` — struct-of-arrays on a private arr field.
    Layout(LayoutKind),
    /// `@unsafe` — raw pointers, no bounds checks.
    Unsafe,
    /// `@inline(always|never|hint)`.
    Inline(InlineKind),
    /// `@hot` / `@cold` — PGO hints.
    Hot,
    Cold,
    /// `@borrow` — pass FStringView, no FString copy.
    Borrow,
    /// `@persistent` — promote closure to persistent heap.
    Persistent,
    /// `@ustruct` — opt-in reflection for structs (retained from v0.2).
    UStruct,
    /// `@mass_fragment` — Mass ECS fragment.
    MassFragment,
    /// `@mass_processor(group=, tick_before=, tick_after=)`.
    MassProcessor {
        group: Option<String>,
        tick_before: Option<String>,
        tick_after: Option<String>,
    },
    /// `@pod` — POD marker (memcpy-safe, no dtor, no GC refs).
    Pod,
    /// `@deprecated("...")`.
    Deprecated(String),
    /// `@allow_private` — suppress the "field not reflected" warning.
    AllowPrivate,
    /// Unknown `@name(args)` — kept so the parser can keep going and report
    /// the error at a higher level.
    Unknown {
        name: String,
        args: Vec<String>,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum LayoutKind { Soa, Aos }

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum InlineKind { Always, Never, Hint }

// ---------- Modifiers (comma-separated, UE reflection) ----------

/// Spec §7 — complete modifier catalog. Modifiers are comma-separated after
/// a declaration and map to UE `UCLASS`/`UPROPERTY`/`UFUNCTION` specifiers.
///
/// These are the *parsed* forms. The `skald-modifiers` crate computes the
/// *effective* UE flag set from defaults + these overrides.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Modifier {
    // ----- Common to class/field/fn -----
    Category(String),
    Tooltip(String),
    DisplayName(String),
    Meta(String),

    // ----- Class (§7.1) -----
    Abstract,
    Config(String),
    DefaultConfig,
    GlobalConfig,
    NotBlueprintable,
    BlueprintType,
    NotBlueprintType,
    EditInlineNew,
    NotEditInlineNew,
    Placeable,
    NotPlaceable,
    Within(String),
    Transient,
    NonTransient,
    MinimalApi,
    Const,
    ConversionRoot,
    CustomConstructor,
    Deprecated,
    HideDropdown,
    HideFunctions(String),
    ShowFunctions(String),
    Spawnable,
    DefaultToInstanced,
    CollapseCategories,
    DontCollapseCategories,

    // ----- Field (§7.2) -----
    EditAnywhere,
    EditDefaultsOnly,
    EditInstanceOnly,
    NotEditable,
    VisibleAnywhere,
    VisibleDefaultsOnly,
    VisibleInstanceOnly,
    BlueprintReadWrite,
    BlueprintReadOnly,
    NotBlueprintAssignable,
    Replicated,
    ReplicatedUsing(String),
    NotReplicated,
    DuplicateTransient,
    NonTransactional,
    NoClear,
    ConfigField,        // `config` on a field
    GlobalConfigField,  // `global_config` on a field
    AssetBundle(String),
    Clamp(String, String),  // min, max (kept as strings for f32 / i32 / expr)
    Range(String, String),
    AdvancedView(u32),
    ArrayIndex(u32),

    // ----- Function (§7.3) -----
    Callable,         // default BlueprintCallable
    Pure,             // BlueprintPure
    NotCallable,
    Reliable,
    Unreliable,
    WithValidation,
    CustomThunk,
    BlueprintInternal,
    BlueprintCallable,
    BlueprintAuthorityOnly,
    BlueprintCosmetic,
    AdvancedDisplay(u32),
    ReturnDisplayName(String),
    AutoCreateRefTerm(String),

    // ----- Method dispatch keywords (§7.4) — sometimes folded in here -----
    Override,
    Virtual,
    Final,
    Static,

    // ----- Other field-like -----
    BpAssignable,  // for delegates (`bp_assignable`)

    // ----- Raw escape hatch — `name(args...)` we didn't recognize -----
    Unknown {
        name: String,
        args: Vec<ModifierArg>,
    },
}

/// Argument inside an unknown modifier.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum ModifierArg {
    Str(String),
    Int(i64),
    Float(f64),
    Ident(String),
}

// ---------- Types ----------

/// Spec §5 — full type system reference. Parsed `Type` is the surface form;
/// `skald-types` has its own resolved representation.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Type {
    /// Primitive: i8/i16/i32/i64/i128, u8/u16/u32/u64/u128, f32/f64, bool, char, str, void, never, name, text.
    Prim(PrimType),
    /// `[u8]` byte slice.
    ByteSlice,
    /// `v2`/`v3`/`v4`/`quat`/`rot`/`mat4`.
    Math(MathType),
    /// Named user-defined type: `Sentry`, `AActor`, etc.
    Named(Name),
    /// Generic application: `arr<T>`, `map<K,V>`, `Pool<AActor>`.
    App { base: Box<Type>, args: Vec<Type> },
    /// `opt<T>` written explicitly or `T?` suffix.
    Optional(Box<Type>),
    /// `ref<T>` (non-null UObject ptr).
    Ref(Box<Type>),
    /// `weak<T>` (nullable, GC-cleared).
    Weak(Box<Type>),
    /// `soft<T>` (lazy-loaded).
    Soft(Box<Type>),
    /// `subclass<T>`.
    Subclass(Box<Type>),
    /// `mut T` — mutable version (mostly for references and iterators).
    Mut(Box<Type>),
    /// `*T` / `*mut T` — raw pointer (only in `@unsafe`).
    Ptr { ty: Box<Type>, mutable: bool },
    /// `fn(Args...) -> Ret` function pointer / closure type.
    Fn { params: Vec<Type>, ret: Box<Type> },
    /// `delegate` multicast delegate.
    Delegate,
    /// `query<mut A, B, ...>` Mass ECS query.
    Query(Vec<Type>),
    /// `()` unit / `void`.
    Unit,
    /// `_` type placeholder (used in inference contexts).
    Infer,
    /// `Self` keyword.
    SelfTy,
    /// Tuple `(T, U, V)`.
    Tuple(Vec<Type>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum PrimType {
    I8, I16, I32, I64, I128,
    U8, U16, U32, U64, U128,
    F32, F64,
    Bool, Char, Str, Name, Text,
    Void, Never,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MathType { V2, V3, V4, Quat, Rot, Mat4 }

// ---------- Expressions ----------

/// Spec §4.6 — operators. Listed in precedence order for the Pratt parser.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum BinOp {
    // Logical
    And, Or,
    // Comparison
    Eq, Ne, Lt, Le, Gt, Ge,
    // Arithmetic
    Add, Sub, Mul, Div, Mod,
    // Bitwise
    BitAnd, BitOr, BitXor, Shl, Shr,
    // Assignment variants
    Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
    BitAndAssign, BitOrAssign, BitXorAssign, ShlAssign, ShrAssign,
    // Null-coalescing
    NullCoalesce,
}

impl BinOp {
    /// Precedence — higher binds tighter. Spec §4.6 — C-family with extras.
    pub fn prec(self) -> u8 {
        use BinOp::*;
        match self {
            Assign | AddAssign | SubAssign | MulAssign | DivAssign | ModAssign
            | BitAndAssign | BitOrAssign | BitXorAssign | ShlAssign | ShrAssign => 1,
            Or => 2,
            And => 3,
            BitOr => 4,
            BitXor => 5,
            BitAnd => 6,
            Eq | Ne => 7,
            Lt | Le | Gt | Ge => 8,
            Shl | Shr => 9,
            Add | Sub => 10,
            Mul | Div | Mod => 11,
            NullCoalesce => 3, // tight as And per spec convention
        }
    }
    pub fn is_assign(self) -> bool {
        use BinOp::*;
        matches!(self,
            Assign | AddAssign | SubAssign | MulAssign | DivAssign | ModAssign
            | BitAndAssign | BitOrAssign | BitXorAssign | ShlAssign | ShrAssign)
    }
    pub fn is_cmp(self) -> bool {
        use BinOp::*;
        matches!(self, Eq | Ne | Lt | Le | Gt | Ge)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum UnOp {
    Neg,    // `-x`
    Not,    // `!x`  (also unwrap — disambiguated by parser)
    BitNot, // `~x`
    Deref,  // `*x`
}

/// Spec §4.6, §4.7 — full expression surface.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Expr {
    /// Integer literal: 123, 0xFF, 0b1010, 123i8, 123u64, etc.
    IntLit { value: i128, ty_hint: Option<PrimType> },
    /// Float literal: 1.0, 1.0f32.
    FloatLit { value: f64, ty_hint: Option<PrimType> },
    /// `"string"` — UTF-8 owned.
    StrLit(String),
    /// `b"bytes"` — byte string.
    ByteStrLit(Vec<u8>),
    /// `'c'` — Unicode scalar.
    CharLit(char),
    /// `f"..."` interpolated string. The pieces are alternating text/expr.
    FmtStrLit(Vec<FmtPiece>),
    /// `true` / `false`.
    BoolLit(bool),
    /// `null` keyword (only valid in `T?` contexts).
    NullLit,
    /// `v3(1, 2, 3)` — desugars to `v3 { x: 1, y: 2, z: 3 }`.
    VectorLit { kind: MathType, args: Vec<Expr> },
    /// Identifier reference.
    Ident(Name),
    /// `self`.
    SelfRef,
    /// `super`.
    SuperRef,
    /// `path::to::name` — path expression.
    Path(Vec<Name>),
    /// `expr.field`.
    Field { base: Box<Expr>, field: Name },
    /// `expr?.field` — optional chain.
    OptionalField { base: Box<Expr>, field: Name },
    /// `expr(args...)`.
    Call { callee: Box<Expr>, args: Vec<Expr> },
    /// `expr.method(args...)`.
    MethodCall { receiver: Box<Expr>, method: Name, args: Vec<Expr> },
    /// `expr?.method(args...)`.
    OptionalMethodCall { receiver: Box<Expr>, method: Name, args: Vec<Expr> },
    /// `Ty::method(args...)` or `Ty::CONST` or `Ty::assoc_fn()`.
    PathCall { path: Vec<Name>, args: Vec<Expr> },
    /// `expr[idx]`.
    Index { base: Box<Expr>, idx: Box<Expr> },
    /// `a + b`, `a = b`, etc.
    Binary { op: BinOp, lhs: Box<Expr>, rhs: Box<Expr> },
    /// `-x`, `!x`, `*x`, `~x`.
    Unary { op: UnOp, expr: Box<Expr> },
    /// `expr!` — never-null assertion (panics if null).
    Unwrap(Box<Expr>),
    /// `expr as Ty`.
    Cast { expr: Box<Expr>, ty: Type },
    /// `expr is Ty`.
    Is { expr: Box<Expr>, ty: Type },
    /// `a .. b` (exclusive) and `a ..= b` (inclusive).
    Range { lo: Option<Box<Expr>>, hi: Option<Box<Expr>>, inclusive: bool },
    /// `a ?: b` Elvis.
    Elvis { cond: Box<Expr>, default: Box<Expr> },
    /// `x |> f` pipe forward — desugars to `f(x)`.
    Pipe { lhs: Box<Expr>, rhs: Box<Expr> },
    /// `|a, b| body` or `|| body` lambda.
    Lambda { params: Vec<LambdaParam>, body: Box<Expr> },
    /// `Type { field: value, .. }` struct literal.
    StructLit { ty: Type, fields: Vec<StructLitField> },
    /// `arr::new()`, `arr::with_capacity(n)` — handled as PathCall but separated for clarity.
    /// `[a, b, c]` array literal.
    ArrayLit(Vec<Expr>),
    /// `(expr)` parenthesized.
    Paren(Box<Expr>),
    /// `()` unit.
    UnitLit,
    /// `(a, b, c)` tuple.
    TupleLit(Vec<Expr>),
    /// `match scrutinee { arms }`.
    Match { scrutinee: Box<Expr>, arms: Vec<MatchArm> },
    /// `if cond { ... } else { ... }` (else is optional).
    If { cond: Box<Expr>, then: Box<Block>, else_: Option<Box<Expr>> },
    /// `for pat in iter { ... }`.
    For { pat: Box<Pat>, iter: Box<Expr>, body: Box<Block> },
    /// `while cond { ... }`.
    While { cond: Box<Expr>, body: Box<Block> },
    /// `loop { ... }`.
    Loop(Box<Block>),
    /// `return expr?`.
    Return(Option<Box<Expr>>),
    /// `break expr?`.
    Break(Option<Box<Expr>>),
    /// `cont` (spec §4.3 — short for continue).
    Cont,
    /// `spawn worker { ... }` or `spawn async { ... }`.
    Spawn { kind: SpawnKind, body: Box<Block> },
    /// `await expr`.
    Await(Box<Expr>),
    /// `parallel for ...` — desugars to ParallelFor.
    ParallelFor { pat: Box<Pat>, iter: Box<Expr>, body: Box<Block> },
    /// Block expression `{ ... }`.
    Block(Box<Block>),
    /// `@unsafe { ... }` block.
    UnsafeBlock(Box<Block>),
    /// `transaction { ... }` — future AutoRTFM hook (open question §18.4).
    Transaction(Box<Block>),
    /// Macro-like expansions kept raw for later phases (none yet).
    MacroCall { name: Name, args: Vec<Expr> },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum FmtPiece {
    Text(String),
    Expr { expr: Box<Expr>, fmt: Option<String> },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LambdaParam {
    pub pat: Pat,
    pub ty: Option<Type>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct StructLitField {
    pub name: Name,
    pub value: Option<Expr>, // None means `name` shorthand
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum SpawnKind { Worker, Async }

// ---------- Patterns ----------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Pat {
    /// `_` wildcard.
    Wildcard,
    /// `name` binding.
    Bind(Name),
    /// `mut name` mutable binding.
    MutBind(Name),
    /// `name @ subpat`.
    At { name: Name, sub: Box<Pat> },
    /// Integer / float / string / bool / char literal.
    Lit(Box<Expr>),
    /// `A | B` or-pattern.
    Or(Vec<Pat>),
    /// `Some(x)` variant pattern (struct/enum).
    TupleStruct { path: Vec<Name>, args: Vec<Pat> },
    /// `Foo { x: pat, y: pat, .. }` struct pattern.
    Struct { path: Vec<Name>, fields: Vec<StructPatField>, rest: bool },
    /// `(a, b)` tuple pattern.
    Tuple(Vec<Pat>),
    /// `[a, b, .., c]` slice pattern.
    Slice(Vec<Pat>),
    /// `pat: Ty` typed pattern.
    Typed { pat: Box<Pat>, ty: Type },
    /// `a .. b` range pattern.
    Range { lo: Option<Box<Expr>>, hi: Option<Box<Expr>>, inclusive: bool },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct StructPatField {
    pub name: Name,
    pub pat: Pat,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MatchArm {
    pub pat: Pat,
    pub guard: Option<Expr>,
    pub body: Expr,
}

// ---------- Statements ----------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Stmt {
    /// `let x = ...` / `let x: T = ...`.
    Let { pat: Pat, ty: Option<Type>, init: Option<Expr> },
    /// `var x: T = ...` (mutable binding; rare — usually `let mut`).
    VarLet { pat: Pat, ty: Option<Type>, init: Option<Expr> },
    /// Expression-as-statement.
    Expr(Expr),
    /// `use path::to::item;`.
    Use(UseTree),
    /// `const NAME: T = expr;`.
    Const { name: Name, ty: Type, init: Expr },
    /// `static NAME: T = expr;`.
    Static { name: Name, ty: Type, init: Expr, mutable: bool },
    /// `type Alias = RealType;`.
    TypeAlias { name: Name, params: Vec<TypeParam>, alias: Type },
    /// Empty / separator.
    Empty,
}

// ---------- Use tree ----------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UseTree {
    pub prefix: Vec<Name>,
    pub kind: UseTreeKind,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum UseTreeKind {
    /// Single name `Foo`.
    Single(Name),
    /// `*` glob.
    Glob,
    /// `{ a, b, c }` nested.
    Nested(Vec<UseTree>),
}

// ---------- Block ----------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    /// Optional trailing expression (block value).
    pub tail: Option<Box<Expr>>,
}

impl Block {
    pub fn new(stmts: Vec<Stmt>, tail: Option<Expr>) -> Self {
        Block { stmts, tail: tail.map(Box::new) }
    }
    pub fn empty() -> Self { Block { stmts: vec![], tail: None } }
}

// ---------- Generic parameters & bounds ----------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TypeParam {
    pub name: Name,
    pub bounds: Vec<TraitBound>,
    pub default: Option<Type>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TraitBound {
    /// `Locatable` etc.
    pub path: Vec<Name>,
}

// ---------- Top-level declarations ----------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Param {
    pub name: Name,
    pub ty: Type,
    /// `@borrow` flag on this specific param.
    pub borrow: bool,
    /// Default value (for `?`-suffixed optional params — rare in Skald).
    pub default: Option<Expr>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FieldDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub ty: Type,
    pub init: Option<Expr>,
    pub modifiers: Vec<Modifier>,
    pub readonly: bool,  // syntactic `readonly` keyword
    pub is_static: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MethodDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub generics: Vec<TypeParam>,
    pub params: Vec<Param>,
    pub ret: Option<Type>,
    pub modifiers: Vec<Modifier>,
    pub dispatch: MethodDispatch,
    pub body: Option<Block>,  // None = abstract (no body)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MethodDispatch {
    /// Default — instance method.
    Instance,
    /// `override` — inherited C++ virtual.
    Override,
    /// `virtual` — Skald-managed vtable.
    Virtual,
    /// `final` — cannot be overridden.
    Final,
    /// `static` — class-level.
    Static,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ClassDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub generics: Vec<TypeParam>,
    pub parent: Option<Type>,
    pub traits: Vec<Type>,
    pub modifiers: Vec<Modifier>,
    pub members: Vec<ClassMember>,
    pub doc: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum ClassMember {
    Field(FieldDecl),
    Method(MethodDecl),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct StructDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub generics: Vec<TypeParam>,
    pub parent: Option<Type>,   // : POD marker (skald-syntax)
    pub modifiers: Vec<Modifier>,
    pub fields: Vec<FieldDecl>,
    pub methods: Vec<MethodDecl>,
    pub doc: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EnumVariant {
    pub name: Name,
    pub payload: Option<Vec<Type>>,
    pub discriminant: Option<Expr>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EnumDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub base: Option<PrimType>,  // u8 default for UENUM(BlueprintType)
    pub variants: Vec<EnumVariant>,
    pub modifiers: Vec<Modifier>,
    pub doc: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TraitDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub generics: Vec<TypeParam>,
    pub supertraits: Vec<TraitBound>,
    pub methods: Vec<TraitMethod>,
    pub doc: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TraitMethod {
    pub name: Name,
    pub params: Vec<Param>,
    pub ret: Option<Type>,
    pub default_body: Option<Block>,
    pub doc: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ImplBlock {
    pub generics: Vec<TypeParam>,
    pub trait_path: Option<Vec<Name>>,
    pub target: Type,
    pub methods: Vec<MethodDecl>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FreeFnDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub generics: Vec<TypeParam>,
    pub params: Vec<Param>,
    pub ret: Option<Type>,
    pub modifiers: Vec<Modifier>,
    pub dispatch: MethodDispatch,
    pub body: Option<Block>,
    /// `=>` arrow-body form: `fn square(x: f32) -> f32 => x * x`.
    pub arrow_body: Option<Box<Expr>>,
    pub doc: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct GlobalDecl {
    pub attrs: Vec<Annotation>,
    pub vis: Visibility,
    pub name: Name,
    pub ty: Type,
    pub init: Option<Expr>,
    pub mutable: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ExternDecl {
    pub vis: Visibility,
    pub name: Name,
    pub params: Vec<Param>,
    pub ret: Option<Type>,
    pub abi: String, // "C" by default
}

/// One top-level item in a Skald module.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Item {
    Use(UseTree),
    Class(ClassDecl),
    Struct(StructDecl),
    Enum(EnumDecl),
    Trait(TraitDecl),
    Impl(ImplBlock),
    Fn(FreeFnDecl),
    Const { name: Name, ty: Type, init: Expr, vis: Visibility },
    Static(GlobalDecl),
    TypeAlias { name: Name, params: Vec<TypeParam>, alias: Type, vis: Visibility },
    Extern(ExternDecl),
    /// `mod name { ... }` inline module.
    Mod { name: Name, items: Vec<Item>, vis: Visibility },
    /// `pub mod name;` external module reference.
    ModRef { name: Name, vis: Visibility },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Module {
    pub file: FileId,
    pub name: String,
    pub items: Vec<Item>,
    pub doc: Option<String>,
    /// Cached identifier table from the lexer (interns strings to `Ident`).
    pub interner: Interner,
}

// ---------- Identifier interner ----------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Interner {
    pub strings: Vec<String>,
    pub map: FxHashMap<String, Ident>,
}

impl Interner {
    pub fn new() -> Self { Self::default() }
    pub fn intern(&mut self, s: &str) -> Ident {
        if let Some(&id) = self.map.get(s) {
            return id;
        }
        let id = Ident(self.strings.len() as u32);
        self.strings.push(s.to_string());
        self.map.insert(s.to_string(), id);
        id
    }
    pub fn lookup(&self, id: Ident) -> &str {
        if id == Ident::DUMMY { return "<dummy>"; }
        &self.strings[id.0 as usize]
    }
    pub fn lookup_name(&self, n: Name) -> &str { self.lookup(n.ident) }
}
