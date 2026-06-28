# Skald — Language Specification v0.3

*A native systems programming language for Unreal Engine 5/6*

**Status:** Pareto-frontier draft. Supersedes v0.2.

**What changed from v0.2:**
- **Default-on reflection.** `@uclass`/`@uprop`/`@ufunc` annotations are gone. Visibility (`pub`/`private`) gates reflection; sensible UE defaults are auto-applied. The common case reads like Roblox Lua — declare a class with fields and methods, it's editable, Blueprint-callable, serialized, and GC-traced with zero ceremony.
- **Modifier system.** Comma-separated modifiers after declarations replace verbose `@`-annotations: `var health: f32, replicated, clamp(0, 1000)`.
- **`@`-annotations retained only for Skald-native concerns** (memory, codegen, FFI) — never for UE reflection flags.
- All v0.2 architectural corrections retained: FFI model split, quiescent-state hot-reload, arena/UObject lifecycle, snapshot/command worker pattern, Cranelift-only backend, Rust workspace.

**Document conventions:**
- All UE5 source paths are relative to a checkout of `github.com/EpicGames/UnrealEngine` `release` branch.
- "Verified seam" means the hook was located in the CenThal source checkout and is documented with a file:line citation.
- "Speculative" means a decision that cannot be validated without prototyping.
- Code blocks marked `// shim` are auto-generated C++ emitted by Skald into `Intermediate/.../UHT/Shims/`. Code blocks marked `// skaldc` are emitted Cranelift output described in pseudocode. Code blocks marked `// rust` are implementation-side Rust in the Skald compiler.

---

## Table of Contents

1. [Language Overview](#1-language-overview)
2. [Design Philosophy: Default-On Reflection](#2-design-philosophy-default-on-reflection)
3. [Rust Workspace & Crate Structure](#3-rust-workspace--crate-structure)
4. [Lexical Syntax](#4-lexical-syntax)
5. [Type System](#5-type-system)
6. [Visibility & Reflection Defaults](#6-visibility--reflection-defaults)
7. [Modifier Reference](#7-modifier-reference)
8. [Execution Model](#8-execution-model)
9. [Memory Management](#9-memory-management)
10. [Reflection Integration](#10-reflection-integration)
11. [FFI Strategy](#11-ffi-strategy)
12. [OOP & ECS](#12-oop--ecs)
13. [Concurrency](#13-concurrency)
14. [Hot Reload](#14-hot-reload)
15. [Developer Experience](#15-developer-experience)
16. [Implementation Roadmap](#16-implementation-roadmap)
17. [Migration Path: Cranelift → LLVM](#17-migration-path-cranelift--llvm)
18. [Open Questions](#18-open-questions)
19. [Constraints (Non-negotiable)](#19-constraints-non-negotiable)
20. [Appendix A: Roblox Lua Comparison](#20-appendix-a-roblox-lua-comparison)
21. [Appendix B: v0.2 → v0.3 Migration](#21-appendix-b-v02--v03-migration)

---

## 1. Language Overview

### 1.1 Name

**Skald** (Old Norse: *skáld*, "poet"). Short, typeable, unclaimed on crates.io and GitHub. Evokes "storyteller" — appropriate for a gameplay scripting language.

### 1.2 Character

The language has a specific, opinionated character that every design decision must serve:

1. **Simple by default.** The common case — writing gameplay code, defining classes, calling engine APIs — feels like Roblox Lua or SkookumScript, not C++. No headers, no forward declarations, no `IMPLEMENT_CLASS` macros, no `.Build.cs` boilerplate visible to the end user. **No reflection annotations required for the common case.** Declare a `pub class` with `pub var` fields and `pub fn` methods — it's automatically editable in the Details panel, Blueprint-callable, serialized, and GC-traced.
2. **Complex when asked.** When a developer needs performance-critical code, they opt in via `@`-annotations (`@region`, `@simd`, `@unsafe`, `@layout`) to access C++-grade control. When they need to override default reflection flags, they use comma-separated modifiers (`replicated`, `clamp(0, 1000)`, `category="AI"`). The language never prevents low-level control, but it requires the developer to *ask explicitly* so the default path stays simple.
3. **Native to UE5.** Not a bolted-on VM plugin. Every `pub class` is a real `UClass`; every `pub fn` is a real `UFunction`; GC-traced by UE's collector; Blueprint-callable; cookable. Verse (Epic's UEFN scripting language) is the reference architecture — `UVerseClass : public UClass` is the precedent for first-class type mirroring.
4. **AOT-compiled to native code.** No VM. No transpile to C++. The Cranelift backend emits object files linked into UE module `.dll`/`.so`/`.dylib` alongside normal C++ object files.
5. **No UE5 source modifications.** Ships as a UE5 plugin (`.uplugin`) + `.UbtPlugin.csproj` loaded into UBT. The supported hook is `ModuleRules.GenerateHeaderFuncs` (`Configuration/Rules/ModuleRules.cs:1513`), the same seam Verse's `VerseVMBytecodeGenerator` uses.

### 1.3 Non-goals

- **Not a sandbox.** Use Verse for UEFN-style untrusted code.
- **Not a Blueprint replacement.** A Blueprint *peer* — both can call each other and coexist in the same project.
- **Not memory-safe-by-proof.** No Rust-style borrow checker. Safe-by-default via GC + bounds checks; `@unsafe` is opt-in.
- **Not multi-runtime.** Targets UE5/UE6 only.
- **Not generic over backend.** v0.3 is Cranelift-only. LLVM migration is a documented future phase (§17), not a runtime switch.

### 1.4 Design principles

1. **One obvious way** for gameplay code.
2. **Two obvious ways** for hot paths (modifier or `@unsafe` block).
3. **Zero invisible cost** at the C++ boundary — direct call, no marshalling for reflected APIs.
4. **Reflection is free** — declaring `pub class Foo : AActor` is exactly the cost of writing the equivalent C++ with default `UCLASS()`/`UPROPERTY()`/`UFUNCTION()` macros. The developer should not need to know what `UCLASS`/`UPROPERTY`/`UFUNCTION` are to write working gameplay code.
5. **No silent ABI risk.** Wherever the spec depends on a fragile ABI assumption, it must be guarded by a `static_assert` injected at compile time.
6. **The C++ compiler always owns vtables.** Skald never emits a C++ vtable. Skald supplies function *bodies* via thunks; the C++ compiler builds the vtable from shim declarations. This eliminates the entire class of MSVC ABI vtable-emission bugs.
7. **Discoverability through autocomplete.** Type `, ` after a declaration and the IDE suggests valid modifiers. Developers discover UE5's feature surface through completion, not by reading documentation.
8. **Sensible defaults, explicit overrides.** Every reflected member gets sensible default UE flags. Modifiers override or extend those defaults. The developer never has to specify what's already the default.

---

## 2. Design Philosophy: Default-On Reflection

### 2.1 The problem with v0.2

v0.2 required `@uclass`, `@uprop`, `@ufunc` annotations on every reflected declaration. This had three costs:

1. **Cognitive load.** A gameplay programmer had to learn what `UCLASS`, `UPROPERTY`, and `UFUNCTION` are before writing their first class. These are C++ macros with hundreds of specifier combinations; the learning curve is steep and unrelated to gameplay logic.
2. **Forgetting annotations caused silent failures.** Forget `@uprop` on a field → the field is invisible to the editor and Blueprint, with no error. Forget `@ufunc` on a method → Blueprint can't call it, with no error. These are the worst kind of bugs: silent, no diagnostic, discovered late.
3. **Verbosity.** Every field had an annotation line above it. Every method had one too. A 50-line gameplay class became 80 lines, half of which was annotation noise.

### 2.2 The v0.3 solution

**Visibility gates reflection.** Skald's `pub` keyword (borrowed from Rust) doubles as the reflection opt-in:

- `pub var x: f32 = 0.0` → reflected as `UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="<ClassName>")`.
- `private var x: f32 = 0.0` → NOT reflected. Lives in instance memory, invisible to editor/Blueprint/GC (unless the type holds a `ref<T>`/`weak<T>`, which carries its own barrier).
- No visibility keyword → defaults to `private` (Rust semantics).

**Modifiers override defaults.** Comma-separated after the declaration:

```skald
pub class Sentry : AActor {
    pub var sight_radius: f32 = 500.0, replicated, clamp(0, 5000), category="AI|Sight"
    pub var target: weak<AActor>, replicated
    pub var debug_label: str = "Sentry", readonly

    pub fn alert(intensity: f32) -> bool, reliable, category="AI" {
        // ... Skald body ...
        return true
    }

    pub fn compute_score() -> f32, pure { ... }

    private var internal_counter: i32 = 0   # NOT reflected
}
```

The same class in v0.2 was:

```skald
@uclass(blueprintable)
pub class Sentry : AActor {
    @uprop(editanywhere, category="AI|Sight", replicated, clamp(0, 5000))
    var sight_radius: f32 = 500.0

    @uprop(replicated)
    var target: weak<AActor>

    @uprop(visibleanywhere)
    var debug_label: str = "Sentry"

    @ufunc(blueprintcallable, category="AI", reliable)
    fn alert(intensity: f32) -> bool { ... }

    @ufunc(blueprintpure)
    fn compute_score() -> f32 { ... }

    var internal_counter: i32 = 0   # NOT reflected (no @uprop)
}
```

v0.3 is shorter, more readable, and the developer doesn't need to know what `UCLASS`/`UPROPERTY`/`UFUNCTION` are.

### 2.3 What still uses `@`

The `@` prefix is retained **only for Skald-native concerns that have nothing to do with UE reflection**:

```
@region(name)      # Arena allocation
@arena             # Scope-local bump allocator
@simd              # Vectorize loop
@layout(soa)       # SoA layout
@unsafe            # Raw pointers, no bounds checks
@inline(always|never|hint)
@hot @cold         # PGO hints
@borrow            # FFI: pass FStringView
@persistent        # Closure promotion
```

These stay `@`-prefixed because:
1. They're Skald-specific (no UE equivalent).
2. They affect codegen, not reflection.
3. Keeping them visually distinct from UE modifiers helps the reader scan a file and instantly see "this is a UE concern" vs. "this is a Skald concern".

### 2.4 Costs of default-on reflection

Default-on reflection is not free. Three real costs:

**Binary size.** Every reflected field adds ~40 bytes of `FProperty` metadata; every reflected method adds ~200 bytes of `UFunction` metadata. A typical gameplay class with 20 fields and 10 methods adds ~3KB. Across a 1000-class game, that's ~3MB of reflection data in the shipping binary. **Mitigation:** `private` opts out.

**GC pressure.** Every reflected field is in the property chain and gets GC-scanned. For gameplay classes (10-50 fields), negligible. For data-heavy classes (100s of fields), this adds up. **Mitigation:** `private` fields holding non-UObject data (`f32`, `v3`, `arr<i32>`) aren't GC-scanned regardless. Only `ref<T>`/`weak<T>` fields are scanned, and those should usually be `pub` anyway.

**Editor noise.** Every `pub var` shows up in the Details panel. This is **good** for gameplay code (designers want to see everything), but you need categories to keep it organized. **Mitigation:** `category="..."` modifier. Default category is the class name; group related fields with explicit categories.

### 2.5 What it gains

1. **Roblox Lua ergonomics for the common case.** Declare a class with fields and methods — it's editable, Blueprint-callable, serialized, replicated-ready. Zero ceremony.
2. **No "did I forget the annotation?" bugs.** In v0.2, forgetting `@uprop` meant the field was invisible to the editor and Blueprint. In v0.3, forgetting a modifier means you get sensible defaults — usually what you wanted.
3. **Discoverable features.** Type `, ` after a field declaration and the IDE suggests `replicated`, `readonly`, `category`, `clamp`, etc. You discover UE5's feature surface through autocomplete, not by reading the spec.
4. **Cleaner diff.** Adding `replicated` to a field is a 12-character addition on the same line, not a new `@uprop(...)` line above it. Git diffs are tighter.
5. **Faster onboarding.** A gameplay programmer can write working Skald code without ever learning what `UCLASS`/`UPROPERTY`/`UFUNCTION` are. They just write code; the defaults handle reflection.

---

## 3. Rust Workspace & Crate Structure

The Skald compiler is written in Rust. The workspace is organized so each phase of the compiler is independently testable and so the UE5 integration layer is isolated from the language core.

### 3.1 Workspace layout

```
skald/
├── Cargo.toml                    # virtual workspace
├── crates/
│   ├── skald-ast/                # AST types
│   ├── skald-lexer/              # Tokenizer
│   ├── skald-parser/             # Parser → AST
│   ├── skald-hir/                # High-level IR (typed, resolved)
│   ├── skald-tir/                # Typed IR post-monomorphization
│   ├── skald-types/              # Type system, traits, unification
│   ├── skald-resolve/            # Name resolution
│   ├── skald-borrowck/           # Lightweight borrow check (mut ref uniqueness)
│   ├── skald-mono/               # Monomorphization
│   ├── skald-reflection/         # Reflection metadata extraction (→ shim + sidecar JSON)
│   ├── skald-modifiers/          # Modifier parser & validator (UE flag mapping)
│   ├── skald-codegen-cranelift/  # Cranelift backend → .obj/.o
│   ├── skald-codegen-llvm/       # (Stub; see §17 — populated during LLVM migration)
│   ├── skald-link/               # Object file archiving, sidecar JSON
│   ├── skald-bindgen/            # UE5 C++ binding generator (parses UHT JSON + libclang)
│   ├── skald-driver/             # CLI entry: `skaldc`
│   ├── skald-lsp/                # Language server
│   ├── skald-runtime/            # Rust runtime: arena, write barriers, panic handler
│   ├── skald-runtime-cpp/        # C++ side of runtime: FSkaldRefCollector, thunks
│   └── skald-ubt-plugin/         # C# plugin that invokes skaldc from UBT (separate csproj)
├── plugin/                       # UE5 plugin scaffold
│   ├── Skald.uplugin
│   ├── Source/
│   │   ├── SkaldRuntime/         # C++ runtime module (ModuleType=CPlusPlus)
│   │   ├── SkaldShims/           # Module that owns GenerateHeaderFuncs
│   │   └── SkaldDriver/          # .UbtPlugin.csproj
│   └── Content/
└── docs/
    └── skald-spec-v0.3.md        # this file
```

### 3.2 Crate responsibilities & key dependencies

#### `skald-lexer`
Tokenizer. Hand-written state machine (no regex; UE identifier rules differ from Rust's). Produces `Vec<Token>` with span info.

**Dependencies:**
- `logos` (or hand-rolled — `logos` is faster but adds a build dependency; lean toward hand-rolled to avoid macro hygiene issues with `proc-macro2` versions).
- `ariadne` (for diagnostic span rendering — same lib used by `rust-analyzer`'s sister projects).

#### `skald-parser`
Recursive-descent parser with Pratt-style expression parsing. Produces `skald_ast::Module`. Parses the modifier syntax (comma-separated after declarations).

**Dependencies:**
- `rowan` (green-tree AST, used by rust-analyzer — gives O(1) subtree sharing for incremental reparsing).
- `salsa` (incremental computation framework — lets LSP reuse parser results across edits; same architecture as rust-analyzer).
- `chumsky` (optional — if you want combinator-style error recovery; otherwise hand-rolled).

#### `skald-ast`
Pure data structures. No logic. `rowan`-backed syntax tree + typed AST wrappers. Includes `Modifier` enum covering all UE flags plus Skald-specific annotations.

**Dependencies:**
- `rowan`, `text-size` (Rust port of Language Server Protocol text types).

#### `skald-hir`
High-level IR: desugared AST with all loops normalized to `while`, all `?` operators expanded to explicit match, etc. This is the form the type checker operates on. Default reflection flags are applied here based on visibility.

**Dependencies:**
- `indexmap` (for stable iteration over declaration order — important for deterministic codegen).
- `rustc-hash` (FxHashMap — faster than std HashMap for compiler-internal symbol tables).

#### `skald-types`
The type system: `Type`, `Trait`, `GenericBound`, `Substitution`, unification logic. Includes the special compiler-known traits (`Reflectable`, `Send`, `Sync`, `POD`).

**Dependencies:**
- `ena` (union-find for type unification, same crate rustc uses).

#### `skald-resolve`
Name resolution: maps identifiers to declarations. Two-phase — module-level resolves first, then function bodies.

**Dependencies:**
- `rustc-hash`, `indexmap`.

#### `skald-borrowck`
Lightweight. Not a full Rust borrow checker — only enforces:
- Single mutable borrow at a time within a function.
- `ref<UObject>` captured into a closure must outlive the closure (conservative: closures can capture `ref<UObject>` only if the closure is in the same region as the reference's owner).

**Dependencies:**
- `polonius` (optional — rustc's borrow checker as a library; overkill for v1 but worth evaluating).

#### `skald-mono`
Monomorphization. Walks the HIR, finds all generic instantiations, produces a monomorphization map. Output is `skald_tir::Module`.

**Dependencies:**
- `bumpalo` (arena allocator for TIR nodes — TIR is transient, no need for `Drop`).

#### `skald-modifiers`
The modifier parser and validator. Maps Skald modifiers (`replicated`, `clamp(0, 1000)`, `category="AI"`, etc.) to UE `UCLASS`/`UPROPERTY`/`UFUNCTION` flags. Also computes the *effective* flag set after applying defaults + overrides.

This is a separate crate so it can be unit-tested in isolation against every UE5 specifier combination, and so the LSP can use it for autocomplete without pulling in the full compiler.

**Dependencies:**
- `serde`, `serde_json` (for serializing the resolved flag set into the sidecar reflection JSON).

#### `skald-reflection`
Walks the TIR + resolved modifier sets, emits:
- C++ shim headers into `Intermediate/.../UHT/Shims/<Module>_Skald.h` (one header per Skald module).
- A sidecar `<Module>.skald-reflection.json` describing every type's layout (used by §10.7 layout-drift detection and §11.2 binding generation).

**Dependencies:**
- `serde`, `serde_json`, `skald-modifiers`.

#### `skald-codegen-cranelift`
The Cranelift backend. Consumes `skald_tir::Module`, produces:
- Per-TU Cranelift `Module` (`cranelift_module::Module`), emits object files via `cranelift_object::ObjectModule`.
- `extern "C"` symbol names: `skald_<Module>_<Class>_<Method>_thunk` for `@ufunc` thunks (now: `pub fn` thunks), `skald_<Module>_<Class>_<Method>_body` for native method bodies, `skald_<Module>_<Function>` for free functions.

**Dependencies:**
- `cranelift-codegen` (core IR + ISLE rule sets).
- `cranelift-module` (module abstraction for symbol management).
- `cranelift-frontend` (builder API: `FunctionBuilder`).
- `cranelift-entity` (entity-keyed tables — `PrimaryMap`, `SecondaryMap`).
- `cranelift-object` (object file emission: ELF, Mach-O, COFF).
- `target-lexicon` (target triple parsing — UBT passes target info to skaldc).

**Why Cranelift (per user direction):**
- Compiles via `cargo` — no separate LLVM build step. UE5 itself uses LLVM/Clang, but Skald's compiler doesn't need to.
- Smaller conceptual surface than LLVM IR; easier to debug.
- Less legacy baggage — Cranelift was designed post-2010, doesn't carry 25 years of compiler evolution.
- Sufficient performance for prototype (Cranelift's `opt_level=speed_and_size` produces code within 10-20% of LLVM `-O2` on numeric code, which is acceptable for a v1 language; hot paths can drop to `@unsafe` for raw control).
- Migration path to LLVM IR is documented in §17; the TIR is designed to be backend-agnostic so the LLVM backend is a separate crate (`skald-codegen-llvm`) implementing the same interface.

**Limitations acknowledged:**
- No LTO with the C++ side. Mitigated by `@inline` annotation that emits the function body in a separate C++-visible `.inl` for hot paths where inlining matters (rare; see §11.5).
- No AArch64-Windows support. Mitigated by restricting Phase 1-4 to Win64-x86_64 and Linux-x86_64; console targets come in Phase 8 and may force the LLVM migration.
- No PDB/DWARF parity with MSVC. Mitigated by emitting DWARF (works in Rider/LLDB on all platforms; Visual Studio support requires `diasdk` integration via a separate path).

#### `skald-link`
Takes Cranelift's emitted `.obj`/`.o` files and:
- Archives them into a single `SkaldObjects_<Module>.lib` (Windows) or `libSkaldObjects_<Module>.a` (Unix) via invoking `lib.exe` / `ar`.
- Writes the archive path + shim header path into a sidecar JSON consumed by the `.Build.cs`.

**Dependencies:**
- `toml` (config parsing).
- `which` (locate `lib.exe`/`ar` in the toolchain).

#### `skald-bindgen`
Two-mode binding generator:

1. **Reflected mode**: parses UHT's reflection JSON (Skald's UHT exporter plugin emits this for all UE modules at first build). Generates a `skald-bindings.bin` blob consumed by `skald-resolve` and `skald-lsp`.
2. **Header mode**: uses `libclang` to parse a curated list of non-reflected headers (`FMath`, `FName`, math types). Generates `extern "C"` C-API shims and Skald-side `extern` declarations.

**Dependencies:**
- `clang-sys` (libclang FFI bindings).
- `serde`, `bincode` (binary serialization of the bindings blob).

#### `skald-driver`
CLI entry point. Parses arguments, orchestrates: lexer → parser → resolve → types → borrowck → mono → reflection → codegen → link. Also exposes `--emit-shims`, `--emit-reflection-json`, `--emit-layout-json` subcommands for the UBT plugin to invoke.

**Dependencies:**
- `clap` (CLI parsing).
- `tracing`, `tracing-subscriber` (structured logging — surfaces in UBT's logger via a custom `tracing-subscriber` that writes to UBT's `ILogger`).
- `rayon` (parallel compilation across TUs in a module).

#### `skald-lsp`
Language server. Speaks LSP 3.17 over stdio. Uses `salsa` for incremental computation — file edits invalidate only the affected query subgraph. Provides modifier autocomplete via `skald-modifiers`.

**Dependencies:**
- `tower-lsp` (LSP server framework).
- `salsa` (incremental queries).
- `tokio` (async runtime — LSP requires async).
- `dashmap` (concurrent symbol table for multi-file lookups).

#### `skald-runtime`
The Skald runtime library, written in Rust, compiled to a static library and linked into every UE module that uses Skald.

Contains:
- Arena allocator (`SkaldArena`).
- Write barrier slow-path (`skald_write_barrier_slow` — called by inline barrier check when TLS flag indicates GC active).
- Panic handler (converts Skald panics into UE `checkf` failures with source location).
- Type-info tables (one per `pub class`, used by the FFI thunks).

**Dependencies:**
- `mimalloc` (default global allocator for non-arena heap; optional — can defer to UE's `FMalloc`).
- `libc` (for `memcpy`, `memmove`).

#### `skald-runtime-cpp`
The C++ side of the runtime. This is a normal UE5 C++ module (`SkaldRuntime.Build.cs` declares `ModuleType = CPlusPlus`). Contains:
- `FSkaldRefCollector : FGCObject` — the bridge between Skald arenas and UE GC.
- `skald_register_arena_slot(UObject**)`, `skald_unregister_arena_slot(UObject**)` — C ABI called by `skald-runtime` Rust code.
- `USkaldClass : UClass` (subclass for Skald-defined classes; needed for hot-reload reinstancing, see §14).
- `USkaldFunction : UFunction` (subclass; needed if Skald needs per-function metadata not expressible via standard UFunction).
- Thunk utilities: `DECLARE_SKALD_FUNCTION(name, fnptr)` macro that wraps `UClass::AddNativeFunction`.

**Dependencies:**
- `Core`, `CoreUObject` (UE5 module deps declared in `.Build.cs`).

#### `skald-ubt-plugin` (C# project, not Rust)
A `.UbtPlugin.csproj` that gets compiled by UBT and loaded into UBT's process. Contains:
- `SkaldGenerateHeaderFunc` — the delegate registered in `ModuleRules.GenerateHeaderFuncs`. Locates the Skald compiler binary (shipped in `plugin/Source/SkaldDriver/bin/skaldc.exe`), invokes it with `--emit-shims --emit-reflection-json --target=<target> --module=<module>`, captures its log output, and forwards it to UBT's `ILogger`.
- `SkaldUhtPlugin` — implements `IUhtPlugin`, registers `[UhtCodeGeneratorInjector]` entries for layout-drift `static_assert`s (§10.7).

**Dependencies (C#):**
- `EpicGames.Core`, `EpicGames.UHT` (provided by UBT at compile time).
- `System.Text.Json` (for reading sidecar JSON).

### 3.3 Build & test commands

```bash
# Build the entire Skald compiler workspace
cargo build --release

# Run all tests
cargo test --workspace

# Run benchmarks (criterion-based)
cargo bench --workspace

# Build the UE5 plugin scaffolding (copies binaries into plugin/Source/SkaldDriver/bin/)
cargo run --bin skald-pack-plugin -- --ue-version=5.5
```

### 3.4 Why this crate split

- **Backend isolation**: `skald-codegen-cranelift` and (future) `skald-codegen-llvm` implement the same `skald_tir::Backend` trait. Switching backends is a cargo feature flag, not a code fork.
- **LSP reuse**: `skald-lsp` reuses `skald-lexer`/`skald-parser`/`skald-resolve`/`skald-types` via `salsa` queries. No duplicate parser.
- **Modifier isolation**: `skald-modifiers` is its own crate so the LSP can use it for autocomplete without pulling in the full compiler.
- **Testability**: each crate has focused unit tests. The parser doesn't need the type system to test.
- **Compile times**: small crates compile in parallel. `skald-codegen-cranelift` is the slowest (~30s clean), others are <10s.
- **Public API surface**: only `skald-driver` is a binary. All other crates are libraries, enabling external tooling (e.g., a future `skald-fmt` formatter) to reuse them.

---

## 4. Lexical Syntax

### 4.1 Source encoding

UTF-8. No BOM. LF or CRLF line endings (CRLF normalized to LF in the lexer).

### 4.2 Layout

Braces `{}`. *Rejected significant indentation*: UE developers cross between C++/C#/Skald constantly; indentation-based block syntax breaks copy-paste from forums and confuses git diffs where tabs/spaces mix. Braces are universal and unambiguous.

Optional trailing-block syntax for single-expression functions:
```skald
fn square(x: f32) -> f32 => x * x
```
Desugars to:
```skald
fn square(x: f32) -> f32 { x * x }
```

### 4.3 Keywords

Short, ≤6 characters. Rationale: UE5 codebases average ~70-character-wide identifiers (`FFunctionInvocationContext`, `UMassEntityQuery`). Reclaiming horizontal space on keywords (`fn` over `function`, `var` over `variable`, `pub` over `public`) is a measurable readability win. Familiar to anyone under 35 (Rust, Swift, Zig all use short keywords).

```
// Declarations
let var fn class struct enum trait impl pub private protected const
use mod type alias static

// Control flow
if else match for while loop break cont return
async await spawn

// Operators / keywords
as is in self Self super
true false null

// Modifiers
override virtual abstract final readonly
```

`cont` is used instead of `continue` (saves 4 chars; matches Rust's `continue` semantically but shorter — `cont` is unambiguous in context).

`readonly` is the only modifier that's a keyword (not a comma-separated modifier) because it's so common it deserves syntactic prominence. It maps to `VisibleAnywhere` + `BlueprintReadOnly` (replaces the default `EditAnywhere` + `BlueprintReadWrite`).

### 4.4 Modifiers (comma-separated, after declarations)

Modifiers replace v0.2's `@uclass`/`@uprop`/`@ufunc` annotations. They're comma-separated, appear after the declaration's basic form but before any block body:

```skald
pub class Sentry : AActor, abstract, config="Game" {
    pub var sight_radius: f32 = 500.0, replicated, clamp(0, 5000), category="AI|Sight"
    pub var target: weak<AActor>, replicated
    pub var debug_label: str = "Sentry", readonly

    pub fn alert(intensity: f32) -> bool, reliable, category="AI" { ... }
    pub fn compute_score() -> f32, pure { ... }
}
```

**Modifier categories:**

| Category | Examples | Affects |
|---|---|---|
| Visibility (keywords) | `pub`, `private`, `protected` | Reflection opt-in/out |
| Class modifiers | `abstract`, `config="X"`, `not_blueprintable`, `within="X"` | `UCLASS` flags |
| Field modifiers | `replicated`, `transient`, `readonly`, `category="X"`, `clamp(min,max)`, `range(min,max)`, `tooltip="..."`, `editdefaults_only`, `editinstance_only`, `no_clear`, `non_transactional`, `asset_bundle="X"`, `meta="..."` | `UPROPERTY` flags |
| Function modifiers | `reliable`, `unreliable`, `pure`, `category="X"`, `with_validation`, `custom_thunk`, `blueprint_internal`, `meta="..."` | `UFUNCTION` flags |
| Lifecycle (keywords) | `override`, `virtual`, `final` | Method dispatch |

Full modifier reference in §7.

### 4.5 Annotations (`@`-prefixed, Skald-native)

Annotations affect codegen, not reflection. They appear before the declaration:

```
@region(name)      # Arena allocation
@arena             # Scope-local bump allocator
@simd              # Vectorize loop
@layout(soa)       # SoA layout
@unsafe            # Raw pointers, no bounds checks
@inline(always|never|hint)
@hot @cold         # PGO hints
@borrow            # FFI: pass FStringView
@persistent        # Closure promotion
```

```skald
@region("sim_frame")
@simd
pub fn tick_particles(particles: mut arr<Particle>, dt: f32) {
    for mut p in particles {
        p.vel += v3(0, 0, -9.8) * dt
        p.pos += p.vel * dt
    }
}
```

### 4.6 Operators

Standard C-family, plus:
- `?.` — optional chain on `T?` and `weak<T>`.
- `?:` — Elvis operator (`a ?: b` → `if a != null { a } else { b }`).
- `..` — range (exclusive); `..=` — range (inclusive).
- `|>` — pipe forward (`x |> f` → `f(x)`).
- `=>` — lambda body separator (single-expr) or match arm.
- `?` — suffix on `T` for `opt<T>` (e.g., `ref<UActor>?`).
- `!` — suffix on `T` for "never-null assertion" — `actor!` unwraps `T?` to `T`, panics if null. Use sparingly; prefer `match`.

### 4.7 Literals

```
123              // i32 (default integer type)
123i8 / 123u8 / 123i64 / ... // explicit
1.0              // f64 (default float type — matches UE5's LargeWorldCoordinates default)
1.0f32           // explicit f32
0xFF / 0b1010    // hex / binary
"string"         // str (UTF-8)
's'              // char (Unicode scalar value)
b"bytes"         // [u8] (byte string)
f"{name} is {age}" // interpolated string
v3(1, 2, 3)      // vector literal sugar — desugars to v3 { x: 1, y: 2, z: 3 }
quat(0, 0, 0, 1) // quaternion literal
```

### 4.8 Comments

```
// line comment
/* block comment (nestable /* like this */) */
/// doc comment — fed to UHT ToolTip metadata automatically (no @tooltip modifier needed)
//! module-level doc comment
```

Doc comments support Markdown. UHT's `ToolTip` and `BriefDescription` metadata are populated from the first paragraph of the `///` comment above a `pub class`/`pub fn`/`pub var`; `DocumentationLink` from any `[doc](url)` link in the comment.

This means **the doc comment is the tooltip** — no separate `tooltip="..."` modifier needed for the common case. The modifier exists only for cases where the doc comment is too long for the Details panel and you want a shorter tooltip.

### 4.9 Identifiers

- `[_a-zA-Z][_a-zA-Z0-9]*` — standard.
- Snake_case for variables and functions (matches Rust convention; UE's PascalCase is auto-mapped at FFI boundary, see §11.3).
- PascalCase for types and traits (also matches Rust; matches UE directly, no mapping).
- `_` as a name = wildcard (no binding).
- `r#` raw identifier prefix (allows using `match`, `type` as field names if needed).

---

## 5. Type System

### 5.1 Primitives

```
// Signed/unsigned integers
i8 i16 i32 i64 i128    // i128 only on 64-bit platforms
u8 u16 u32 u64 u128

// Floating point
f32 f64   // f64 is the default float literal type (UE5 LWC)

// Other
bool      // 1 byte (matches UE bool)
char      // 4 bytes (Unicode scalar value; not UE TCHAR)
str       // owned UTF-8 string; aliases FString
[u8]      // byte slice; aliases TArray<uint8> when stored in a pub var

// Special
void      // unit type, also written ()
never     // bottom type, also written !; for fns that never return (panic, infinite loop)
```

**Aliasing to UE primitives** (verified at compile time via `static_assert` in the shim):
- `i32` → `int32` (`static_assert(sizeof(int32) == 4)`)
- `f32` → `float` (`static_assert(sizeof(float) == 4)`)
- `f64` → `double`
- `bool` → `bool`
- `str` → `FString` (with `FStringView` view at FFI boundary when `@borrow` is used)

### 5.2 Built-in UE-mapped types

```
// Math
v2 v3 v4         → FVector2D, FVector, FVector4 (f64 in LWC builds, f32 in non-LWC)
quat rot mat4    → FQuat, FRotator, FMatrix

// Strings & names
name             → FName (interned, cheap to copy)
text             → FText (localized)

// Containers (compile to UE containers when in a pub var; otherwise Skald-owned)
arr<T>           → TArray<T>           (when T: Reflectable)
map<K,V>         → TMap<K,V>
set<T>           → TSet<T>
opt<T>           → TOptional<T>        (when T: Reflectable)

// UObject references (the only nullable-or-not-distinct types)
ref<T>           → TObjectPtr<T>       (non-null by default; see §5.7)
weak<T>          → TWeakObjectPtr<T>   (nullable, auto-cleared by GC)
soft<T>          → TSoftObjectPtr<T>   (lazy-loaded)
subclass<T>      → TSubclassOf<T>      (UClass* constrained to T or subclass)
```

**Why alias instead of replace:** UE primitive types carry 30 years of muscle memory. Stack traces, memory dumps, `r.*` cvars, and Blueprint integration all read naturally when `v3` IS `FVector`. Aliasing means zero cognitive overhead for UE veterans.

### 5.3 Reference vs value types

| Declaration | Memory | Identity | GC | Reflection |
|---|---|---|---|---|
| `class` | Heap (UE heap) | Reference (by `ref<T>`) | UE GC | Real `UClass` (when `pub`) |
| `struct` | Inline / stack | Value (copy) | None (unless holds refs) | Real `UScriptStruct` (when `pub` and `@ustruct`) |
| `enum` | Inline (discriminant + payload) | Value | None | Real `UEnum` (when `pub`) |
| `trait` | N/A (only implemented) | N/A | N/A | `UInterface` (when `pub`) |

### 5.4 Generics

Monomorphized (no reified generic parameters at runtime — UHT has no template support, and Verse hit the same wall).

```
fn nearest<T: Locatable>(items: arr<T>, p: v3) -> T?
```

**Generic types cannot be reflected.** UHT has no template support; a generic class cannot produce a single `UClass`. Workaround: a generic `class` reflects only its concrete instantiations that are explicitly aliased:

```skald
class Pool<T> {
    var items: arr<T>
    fn acquire() -> T? { ... }
}

// Concrete instantiation — this gets reflected
type ActorPool = Pool<AActor>

pub class Spawner {
    pub var pool: ActorPool
}
```

For the common gameplay case, prefer composition over generic UClass: define `class Inventory<T>` only when `T` is non-reflected (POD); use `UObject*` plus runtime type checks for reflected generics. Skald provides `TAnyStruct<T>` (UE's `FInstancedStruct`) as an escape hatch.

### 5.5 Traits

Rust-style traits with explicit `impl Trait for Type` blocks.

**Special compiler-known traits** (cannot be user-implemented; auto-derived or rejected):

- `Reflectable` — auto-impl for any type used in a `pub var`. Compile error if a `pub var` field's type isn't `Reflectable`. For user-defined structs, `#[derive(Reflectable)]` generates the boilerplate (Skald has no procedural macros yet — `derive` is built into the compiler).
- `Send` / `Sync` — marker traits for thread safety. `UObject`-derived types are `!Send` by default. POD structs are `Send + Sync` if all fields are.
- `POD` — guarantees memcpy-safe, no destructor, no GC references. Required for `@layout(soa)` and recommended for `@simd`.
- `Copy` — opt-in (Rust-style). `struct` types are not `Copy` by default.
- `Drop` — implement to define a destructor. For `pub class` types, `Drop` runs in `UObject::BeginDestroy` (via a shim virtual override); for `pub struct`, runs at scope exit.

### 5.6 Annotations (opt-in complexity, Skald-native)

```
@region(name)
// Allocations within this scope route to a named arena.
// Arena lives until explicit `region::drop(name)` or end of frame (if named).
// Captured closures inherit the region.

@arena
// Anonymous scope-local bump allocator. Freed at `}`.
// Equivalent to RAII arena; cannot be returned from the function.

@simd
// Marks a `for` loop body as vectorizable.
// Compile error if the body has data dependencies preventing vectorization.
// Implies `@inline(hint)` on the loop body.

@layout(soa)
// Marks an `arr<T>` field as struct-of-arrays.
// RESTRICTION (v0.3): Only valid on non-reflected (private) fields.
// For reflected arrays needing SoA, use a custom FStructSerializer (out of scope for v1).
// Reason: implementing FSoaArrayProperty requires 4-8 weeks of work.

@unsafe { ... } / @unsafe fn ...
// Allows: raw pointers (`*T`, `*mut T`), pointer arithmetic, transmute, no bounds checks.
// Required for: direct GPU buffer writes, ffi calls to non-reflected APIs without bindings.

@inline(always|never|hint)
// Always: force inlining at codegen; may bloat code.
// Never: prevent inlining.
// Hint: default — let the backend decide.

@hot / @cold
// PGO hints. @hot functions placed in hot code section; @cold in cold section.

@borrow
// FFI annotation on a parameter or return type.
// Indicates the value is borrowed (not owned) — pass FStringView instead of FString copy.

@persistent
// On a closure: promote from arena to persistent Skald heap when stored in a pub var slot.
// Triggers a heap copy; the closure outlives its arena.
```

### 5.7 Nullability

No nulls by default. Every type `T` is non-null.

- `T?` (== `opt<T>`) is the only nullable form. Implemented as `TOptional<T>` for reflected types; as a Rust-style `Option<T>` discriminant for non-reflected.
- `ref<T>` is non-null. If C++ returns `nullptr` from an FFI call, the FFI layer converts to `ref<T>?`. Eliminates ~90% of UE null-deref crashes (`UWorld*` null, `GetOwner()` null, dangling actor).
- `weak<T>` is implicitly nullable (it's a `TWeakObjectPtr`); `.pin()` returns `ref<T>?`.
- `!` suffix unwraps `T?` to `T` with a panic if null. Use sparingly; prefer `match` or `?.`.

```skald
let actor: ref<AActor> = world.spawn_actor(...)        // non-null
let maybe_target: ref<AActor>? = self.target.pin()     // nullable
let target: ref<AActor> = maybe_target!                // panic if null
let name: str = maybe_target?.name() ?? "<none>"       // safe chain with default
```

### 5.8 Type inference

`let` bindings infer the type from the initializer. Function parameters and return types must be explicitly annotated (no whole-program inference — keeps signatures readable, matches Rust).

```skald
let x = 5              // i32
let y = 5.0            // f64
let z = 5.0f32         // f32
let v = v3(1, 2, 3)    // v3
let arr = arr::new()   // arr<_> — error, cannot infer element type
let arr: arr<i32> = arr::new()   // OK
```

---

## 6. Visibility & Reflection Defaults

### 6.1 The visibility → reflection mapping

Skald's `pub`/`private`/`protected` keywords double as the reflection opt-in/out. This is the central ergonomic win of v0.3.

| Visibility | Reflected? | Editor-visible? | Blueprint-callable? | GC-scanned? |
|---|---|---|---|---|
| `pub` | Yes (default flags) | Yes | Yes | Yes (if holds refs) |
| `protected` | Yes (BlueprintProtected) | Yes (read-only in derived) | Yes (from subclasses) | Yes |
| `private` | No | No | No | Only if holds `ref<T>`/`weak<T>` (via write barrier, not schema) |
| (no keyword) | No (defaults to `private`) | No | No | Same as `private` |

### 6.2 Default UE flags by declaration kind

When a declaration is `pub` (or `protected`) and has no modifiers overriding the defaults, the shim generator auto-applies these UE flags:

#### `pub class X : Y`
```cpp
UCLASS(BlueprintType, Blueprintable, Config=Engine, meta=(SkaldGenerated))
class X_API AX : public AY { GENERATED_BODY() ... };
```

#### `pub class X : Y, abstract`
```cpp
UCLASS(BlueprintType, Blueprintable, Abstract, Config=Engine, meta=(SkaldGenerated))
```

#### `pub struct X` (when `@ustruct` is present)
```cpp
USTRUCT(BlueprintType)
struct X { GENERATED_BODY() ... };
```

(Note: `struct` is NOT reflected by default in v0.3. To reflect a struct, use `@ustruct`. This is because most structs in gameplay code are internal data carriers that don't need editor visibility. Classes, being the primary gameplay type, ARE reflected by default. This asymmetry matches UE5's own convention where `USTRUCT` is opt-in but `UCLASS` is the default for gameplay classes.)

#### `pub enum X`
```cpp
UENUM(BlueprintType)
enum class X : uint8 { ... };
```

#### `pub trait X` (Skald) / `@uinterface` (UE)
```cpp
UINTERFACE(BlueprintType)
class UX : public UInterface { GENERATED_BODY() };
class IX { GENERATED_BODY() public: virtual void X_Method() = 0; };
```

#### `pub var x: T` (in a class)
```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="<ClassName>")
T x;
```

Default category is the class name. If the class is nested in a module path (e.g., `Game.AI.Sentry`), the category is just `Sentry`.

#### `pub var x: T, readonly`
```cpp
UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="<ClassName>")
T x;
```

`readonly` is a keyword (not a modifier) because it's so common. It replaces the default `EditAnywhere`/`BlueprintReadWrite` with `VisibleAnywhere`/`BlueprintReadOnly`.

#### `pub var x: T, replicated`
```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="<ClassName>")
T x;
```

`replicated` adds the `Replicated` flag but keeps the other defaults.

#### `pub var x: weak<T>, replicated`
```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="<ClassName>")
TWeakObjectPtr<T> x;
```

#### `pub fn x()`
```cpp
UFUNCTION(BlueprintCallable, Category="<ClassName>")
void x();
```

#### `pub fn x(), pure`
```cpp
UFUNCTION(BlueprintPure, Category="<ClassName>")
```

`pure` switches `BlueprintCallable` to `BlueprintPure`.

#### `pub fn x() override`
No `UFUNCTION` macro — this is a C++ virtual override, registered via the shim's vtable thunk (see §11). The `override` keyword is required (Java/C# style; avoids C++'s silent shadowing footgun).

#### `pub fn x() virtual`
A Skald-defined virtual (overridable by Skald subclasses). Goes through a Skald-managed vtable (a `Vec<fnptr>` in `USkaldClass`), not the C++ vtable. NOT reflected as a `UFunction` by default — use the `callable` modifier if you want both.

#### `private var x: T`
No `UPROPERTY` macro. The field exists in instance memory but is invisible to UE reflection. **Exception:** if `T` is `ref<U>`/`weak<U>`/`soft<U>`, the field still needs GC scanning — handled via the write barrier, not the property schema. The field is registered with `FSkaldRefCollector` at construction and deregistered at destruction.

### 6.3 Opting OUT of reflection defaults

Sometimes you want a `pub class` that is NOT Blueprintable (e.g., a hidden internal class). Use the `not_blueprintable` modifier:

```skald
pub class InternalHelper : UObject, not_blueprintable { ... }
```

Sometimes you want a `pub var` that is NOT editable. Use `readonly` or `not_editable`:

```skald
pub var cached_value: f32, readonly          // VisibleAnywhere + BlueprintReadOnly
pub var internal_state: f32, not_editable    // No EditAnywhere, but still BlueprintReadWrite
```

Sometimes you want a `pub fn` that is NOT Blueprint-callable. Use `not_callable`:

```skald
pub fn internal_helper(), not_callable { ... }   // C++ only, not exposed to Blueprint
```

### 6.4 When reflection is impossible

Some Skald constructs cannot be reflected. The compiler emits an error if you try:

- **Generic types as `pub class`**: `pub class Pool<T>` → error ("generic classes cannot be reflected; use a `type` alias for a concrete instantiation").
- **`static var` as `pub`**: `pub static var x: f32` → error ("static variables cannot be reflected; UE5 does not support static UPROPERTY"). Use a `pub fn get_x() -> f32, pure` instead.
- **`const` as `pub`**: `pub const MAX: f32 = 100.0` → not an error, but `const` is never reflected (it's a compile-time constant). No shim entry is emitted.
- **Closures as `pub var`**: `pub var callback: fn()` → error ("closures cannot be reflected; wrap in a UFUNCTION or use a UScriptStruct wrapper"). Workaround: store the closure in a `SkaldClosure` UScriptStruct (Skald stdlib type).

---

## 7. Modifier Reference

This is the complete modifier catalog. Modifiers are comma-separated after a declaration, before any block body. They map directly to UE `UCLASS`/`UPROPERTY`/`UFUNCTION` specifiers.

### 7.1 Class modifiers

| Modifier | Maps to | Default? | Notes |
|---|---|---|---|
| `abstract` | `UCLASS(Abstract)` | No | Cannot be instantiated; must be subclassed. |
| `config="Game"` | `UCLASS(Config=Game)` | `Config=Engine` | Config file to read/write. |
| `default_config` | `UCLASS(DefaultConfig)` | No | |
| `global_config` | `UCLASS(GlobalConfig)` | No | |
| `not_blueprintable` | `UCLASS(NotBlueprintable)` | No | Opts out of default `Blueprintable`. |
| `blueprint_type` | `UCLASS(BlueprintType)` | Yes | Default-on; can be opted out with `not_blueprint_type`. |
| `not_blueprint_type` | `UCLASS(NotBlueprintType)` | No | |
| `editinline_new` | `UCLASS(EditInlineNew)` | No | |
| `not_editinline_new` | `UCLASS(NotEditInlineNew)` | Yes | Default-on for actors; can be opted out. |
| `placeable` | `UCLASS(Placeable)` | Yes (for AActor) | |
| `not_placeable` | `UCLASS(NotPlaceable)` | No | |
| `within="X"` | `UCLASS(Within=X)` | No | Must be created inside an instance of X. |
| `transient` | `UCLASS(Transient)` | No | |
| `non_transient` | `UCLASS(NonTransient)` | No | |
| `minimal_api` | `UCLASS(MinimalAPI)` | No | |
| `const` | `UCLASS(Const)` | No | |
| `conversion_root` | `UCLASS(ConversionRoot)` | No | |
| `custom_constructor` | `UCLASS(CustomConstructor)` | No | |
| `deprecated` | `UCLASS(Deprecated)` | No | |
| `hide_dropdown` | `UCLASS(HideDropdown)` | No | |
| `hide_functions="..."` | `UCLASS(HideFunctions=...)` | No | Comma-separated list of category names. |
| `show_functions="..."` | `UCLASS(ShowFunctions=...)` | No | |
| `spawnable` | `UCLASS(Spawnable)` | No | |
| `default_to_instanced` | `UCLASS(DefaultToInstanced)` | No | |
| `collapse_categories` | `UCLASS(CollapseCategories)` | No | |
| `dont_collapse_categories` | `UCLASS(DontCollapseCategories)` | No | |
| `meta="..."` | `UCLASS(meta=(...))` | No | Escape hatch for any UCLASS meta key. |

### 7.2 Field (UPROPERTY) modifiers

| Modifier | Maps to | Default? | Notes |
|---|---|---|---|
| `editanywhere` | `EditAnywhere` | Yes | Default-on. |
| `editdefaults_only` | `EditDefaultsOnly` | No | |
| `editinstance_only` | `EditInstanceOnly` | No | |
| `not_editable` | `NoEditable` (custom) | No | Opts out of `EditAnywhere` but keeps `BlueprintReadWrite`. |
| `readonly` (keyword) | `VisibleAnywhere` + `BlueprintReadOnly` | No | Replaces `EditAnywhere`/`BlueprintReadWrite`. |
| `visibleanywhere` | `VisibleAnywhere` | No | Like `readonly` but keeps `BlueprintReadWrite`. |
| `visible_defaults_only` | `VisibleDefaultsOnly` | No | |
| `visible_instance_only` | `VisibleInstanceOnly` | No | |
| `blueprint_readwrite` | `BlueprintReadWrite` | Yes | Default-on. |
| `blueprint_read_only` | `BlueprintReadOnly` | No | |
| `not_blueprint_assignable` | `NotBlueprintAssignable` | No | |
| `replicated` | `Replicated` | No | |
| `replicated_using=fn_name` | `ReplicatedUsing=fn_name` | No | |
| `not_replicated` | `NotReplicated` | No | |
| `transient` | `Transient` | No | Not serialized. |
| `duplicate_transient` | `DuplicateTransient` | No | |
| `non_transactional` | `NonTransactional` | No | |
| `no_clear` | `NoClear` | No | |
| `config` | `Config` | No | |
| `global_config` | `GlobalConfig` | No | |
| `asset_bundle="X"` | `meta=(AssetBundles="X")` | No | |
| `category="X"` | `Category="X"` | Class name | Default category is the class name. |
| `clamp(min, max)` | `meta=(ClampMin="min", ClampMax="max")` | No | |
| `range(min, max)` | `meta=(UIMin="min", UIMax="max")` | No | UI slider range (no enforcement). |
| `tooltip="..."` | `meta=(ToolTip="...")` | From `///` doc | If a `///` doc comment is present, the tooltip comes from there; this modifier overrides. |
| `display_name="..."` | `DisplayName="..."` | No | |
| `advanced_view=N` | `AdvancedView=N` | No | |
| `array_index=N` | `meta=(ArrayIndex="N")` | No | |
| `meta="..."` | `meta=(...)` | No | Escape hatch for any UPROPERTY meta key. |

### 7.3 Function (UFUNCTION) modifiers

| Modifier | Maps to | Default? | Notes |
|---|---|---|---|
| `callable` (default) | `BlueprintCallable` | Yes | Default-on for `pub fn`. |
| `pure` | `BlueprintPure` | No | Replaces `BlueprintCallable` with `BlueprintPure`. |
| `not_callable` | `NoCallable` (custom) | No | Opts out of Blueprint exposure. |
| `reliable` | `Reliable` | No | |
| `unreliable` | `Unreliable` | No | |
| `with_validation` | `WithValidation` | No | Requires a `validate_x()` companion function. |
| `custom_thunk` | `CustomThunk` | No | |
| `blueprint_internal` | `BlueprintInternalUseOnly` | No | |
| `blueprint_callable` | `BlueprintCallable` | Yes | Explicit form of default. |
| `blueprint_authority_only` | `BlueprintAuthorityOnly` | No | |
| `blueprint_cosmetic` | `BlueprintCosmetic` | No | |
| `category="X"` | `Category="X"` | Class name | |
| `display_name="..."` | `DisplayName="..."` | No | |
| `tooltip="..."` | `meta=(ToolTip="...")` | From `///` doc | |
| `advanced_display=N` | `AdvancedDisplay=N` | No | |
| `return_display_name="..."` | `meta=(ReturnDisplayName="...")` | No | |
| `auto_create_ref_term="..."` | `AutoCreateRefTerm="..."` | No | |
| `meta="..."` | `meta=(...)` | No | Escape hatch. |

### 7.4 Method dispatch keywords

| Keyword | Maps to | Notes |
|---|---|---|
| `override` | C++ `override` | Required for inherited virtuals. No `UFUNCTION`. Body via shim vtable thunk. |
| `virtual` | Skald-managed vtable | Overridable by Skald subclasses. Not C++ virtual. |
| `final` | C++ `final` + Skald `final` | Cannot be overridden. |
| `static` | C++ static method | No `UFUNCTION`. Called as `ClassName.method()`. |

### 7.5 Modifier examples

**A replicated health field with clamping:**
```skald
pub class Combatant : AActor {
    /// Current health. Clamped to [0, max_health].
    pub var health: f32 = 100.0, replicated, clamp(0, 1000), category="Combat"
    pub var max_health: f32 = 100.0, clamp(1, 10000), category="Combat"
}
```

Generates:
```cpp
UCLASS(BlueprintType, Blueprintable, Config=Engine, meta=(SkaldGenerated))
class GAME_API ACombatant : public AActor {
    GENERATED_BODY()
public:
    /// Current health. Clamped to [0, max_health].
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="Combat", meta=(ClampMin="0", ClampMax="1000"))
    float health = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Combat", meta=(ClampMin="1", ClampMax="10000"))
    float max_health = 100.0f;
};
```

**A BlueprintPure function with a custom display name:**
```skald
pub class Combatant : AActor {
    /// Returns true if health > 0.
    pub fn is_alive() -> bool, pure, display_name="Is Alive" {
        return self.health > 0.0
    }
}
```

Generates:
```cpp
UFUNCTION(BlueprintPure, Category="Combatant", DisplayName="Is Alive", meta=(ToolTip="Returns true if health > 0."))
bool is_alive();
```

**An abstract, non-Blueprintable base class:**
```skald
pub class ComponentBase : AActor, abstract, not_blueprintable, within="AActor" {
    pub fn initialize() virtual { ... }
}
```

Generates:
```cpp
UCLASS(BlueprintType, NotBlueprintable, Abstract, Within=AActor, Config=Engine, meta=(SkaldGenerated))
class GAME_API AComponentBase : public AActor {
    GENERATED_BODY()
public:
    // virtual method — no UFUNCTION, dispatched via Skald vtable
    virtual void initialize();  // shim thunk calls skald_Game_AComponentBase_initialize_body
};
```

---

## 8. Execution Model

### 8.1 AOT compilation via Cranelift

**Decision: Cranelift only, no LLVM, no JIT.**

Per user direction:
- Cranelift compiles via `cargo` — no separate LLVM build step in the dev loop.
- Smaller conceptual surface, easier to debug.
- Less legacy baggage.
- Sufficient performance for prototype.
- LLVM migration is a documented future phase (§17), not a runtime switch.

The Cranelift backend (`skald-codegen-cranelift`) consumes `skald_tir::Module` and produces:
- Per-TU Cranelift `Function` instances.
- One `ObjectModule` per Skald module, emitting a single `.obj` (Windows) or `.o` (Unix).
- Object files are archived into `SkaldObjects_<Module>.lib`/`.a` by `skald-link`.

**Object file emission** uses `cranelift_object::ObjectModule`:
```rust
// skald-codegen-cranelift/src/lib.rs (sketch)
use cranelift_module::{Module, Linkage};
use cranelift_object::{ObjectModule, ObjectBuilder};

pub fn compile(tir_module: &tir::Module, target: &target_lexicon::Triple) -> PathBuf {
    let isa = cranelift_native::builder().unwrap().finish(target.clone()).unwrap();
    let builder = ObjectBuilder::new(isa, "skald_module.o".into(), cranelift_module::default_libcall_names()).unwrap();
    let mut module = ObjectModule::new(builder);

    for func in &tir_module.functions {
        let sig = lower_signature(&func.signature, &mut module);
        let func_id = module.declare_function(&func.symbol, Linkage::Export, &sig).unwrap();
        let mut ctx = module.make_context();
        ctx.func.name = func_id.into();
        lower_body(func, &mut ctx, &mut module);
        module.define_function(func_id, &mut ctx).unwrap();
        module.clear_context(&mut ctx);
    }

    let obj_bytes = module.finish().emit().unwrap();
    let out_path = output_dir.join(format!("{}.obj", tir_module.name));
    std::fs::write(&out_path, obj_bytes).unwrap();
    out_path
}
```

### 8.2 Build pipeline (one .skald file → linked .dll)

```
[UBT makefile build]
  ├─ ModuleRules.GenerateHeaderFuncs delegates run (incl. Skald's)
  │   ├─ skaldc --emit-shims --emit-reflection-json --target=<T> --module=<M>
  │   │   ├─ Lexer → Parser → Resolve → Types → Borrowck → Mono → TIR
  │   │   ├─ skald-modifiers resolves effective UE flag sets
  │   │   ├─ skald-reflection writes:
  │   │   │   ├─ Intermediate/.../UHT/Shims/<M>_Skald.h  (C++ shims with empty UCLASS bodies)
  │   │   │   └─ Intermediate/.../Skald/<M>.skald-reflection.json
  │   │   ├─ skald-codegen-cranelift writes:
  │   │   │   └─ Intermediate/.../Skald/<M>.obj
  │   │   └─ skald-link archives:
  │   │       └─ Intermediate/.../Skald/SkaldObjects_<M>.lib
  │   └─ sidecar JSON: paths to shim header + archive
  │
  ├─ UHT runs (in-process, UHTExecution.cs:1468)
  │   ├─ Parses shim headers (SkaldUhtPlugin's [UhtCodeGeneratorInjector] adds static_asserts)
  │   ├─ Emits <M>_Skald.generated.h / .gen.cpp into Intermediate/.../UHT/
  │   └─ Updates module init lists (Z_Register_Module<M>)
  │
  ├─ Module .Build.cs reads sidecar JSON:
  │   ├─ Adds shim .h to PublicIncludePaths (already done by GenerateHeaderFuncs)
  │   ├─ Adds UHT-generated .gen.cpp to module sources (automatic)
  │   └─ Adds SkaldObjects_<M>.lib to PublicAdditionalLibraries
  │
  └─ UEToolChain.CompileCPPFiles + LinkFiles:
      ├─ Compiles .gen.cpp normally (resolves Z_Construct_UClass_<X> singletons)
      ├─ Links against SkaldObjects_<M>.lib (resolves skald_<X>_<Y>_thunk symbols)
      └─ Produces <Target>.dll with all symbols resolved
```

### 8.3 Object file linkage (correcting v0.2's misuse of FilesToGenerate)

`FilesToGenerate` (`ModuleRules.cs:858`) is consumed by `UEBuildModuleCPP.cs:916-924`, which only accepts text content (typically C++ source). You cannot `#include` an object file. v0.3 uses the correct flow:

1. `skald-link` archives Cranelift's `.obj` output into `SkaldObjects_<Module>.lib` (Windows) or `libSkaldObjects_<Module>.a` (Unix) by invoking the platform archiver:
   - Windows: `lib.exe /OUT:SkaldObjects_<Module>.lib @objects.rsp`
   - Unix: `ar rcs libSkaldObjects_<Module>.a <objs...>`

2. `skald-link` writes a sidecar JSON:
   ```json
   {
     "module": "Game",
     "shim_header": "Intermediate/Build/Win64/UnrealEditor/Inc/Game/UHT/Shims/Game_Skald.h",
     "object_archive": "Intermediate/Build/Win64/UnrealEditor/Inc/Game/Skald/SkaldObjects_Game.lib",
     "layout_json": "Intermediate/Build/Win64/UnrealEditor/Inc/Game/Skald/Game.layout.json",
     "symbols": ["skald_Game_ASentry_alert_thunk", "skald_Game_ASentry_BeginPlay_body", ...]
   }
   ```

3. The Skald shim module's `.Build.cs` reads the sidecar and populates `PublicAdditionalLibraries`:
   ```csharp
   // SkaldShims.Build.cs
   public class SkaldShims : ModuleRules
   {
       public SkaldShims(ReadOnlyTargetRules Target) : base(Target)
       {
           PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
           PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "SkaldRuntime" });

           GenerateHeaderFuncs.Add(("Skald", (Logger, GeneratedDir) =>
           {
               // Invoke skaldc — it writes shim header, .obj, archive, sidecar JSON
               var sidecarPath = Path.Combine(GeneratedDir.FullName, "Skald", "sidecar.json");
               SkaldDriverInvoker.Compile(ModuleDirectory, GeneratedDir, Target, Logger);

               // Read sidecar to populate PublicAdditionalLibraries
               var sidecar = JsonSerializer.Deserialize<Sidecar>(File.ReadAllText(sidecarPath))!;
               PublicAdditionalLibraries.Add(sidecar.ObjectArchive);
           }));
       }
   }
   ```

### 8.4 Linkage model

Skald `.obj`/`.lib` files become part of the module's normal link line. **No separate Skald runtime DLL** for shipping — the Skald runtime is a static lib (`SkaldRuntime.lib`, ~80KB) linked into every module that uses Skald.

There is a tiny `SkaldRuntime` C++ module (UE module, not Rust) containing `FSkaldRefCollector`, the arena slot registration API, and the thunk utilities. This is a normal UE5 module declared in `SkaldRuntime.Build.cs` with `ModuleType = CPlusPlus`.

### 8.5 No live coding for Skald in editor (v0.3)

For v0.3, hot reload uses the patch-in-place mechanism described in §14. Live Coding (UE's DLL swap mechanism for C++) does not interact with Skald's patch-in-place — they are parallel systems. Skald's hot reload handles Skald code; UE's Live Coding handles C++ code.

If a developer has both Skald changes and C++ changes, the C++ changes go through Live Coding (which reloads the whole module DLL, including Skald-compiled objects), and Skald's hot-reload state is reset (all Skald classes are re-instanced from scratch). This is acceptable for v0.3.

---

## 9. Memory Management

### 9.1 Decision: Hybrid model (Option B)

- **Reflected types** (`pub class`, `@ustruct`): live on the UE heap. UE's GC owns them. Skald does not GC them.
- **Non-reflected reference types** (closures, internal collections, owned strings, AST-like data): live on the Skald heap. Allocated via arena or `mimalloc`.
- **Bridge**: a single `FSkaldRefCollector : FGCObject` per module roots the Skald heap's references to UObjects.

**Why not Option A (direct mapping, no custom heap):** closures, large owned strings, and intermediate collections don't fit UE's UObject model. UE UObjects have ~64 bytes of header overhead (`UObjectBase` is 32+ bytes for `ClassPrivate`/`OuterPrivate`/`NamePrivate`/`Flags`/`InternalIndex`, plus the GUObjectArray slot). Allocating millions of small closures as UObjects would blow memory budget.

**Why not Option C (full FrankenGC dual-GC):** Verse needed it because Verse has millions of small immutable `VCell`s (option values, rational numbers, decimal types, tuples). Skald gameplay code overwhelmingly allocates UObjects or `FVector`-sized PODs. The hybrid arena handles the rest with no GC ceremony.

### 9.2 Arena allocator

Three allocation modes:

#### 9.2.1 Default heap (no annotation)
For non-reflected reference types where arena lifetime doesn't apply. Routes to `mimalloc` (chosen for cross-platform perf, header-only, MIT licensed). Examples: long-lived closures, owned strings returned from functions, persistent collections.

```skald
let s = "hello".to_upper()  // allocates on default heap
```

#### 9.2.2 `@arena` (scope-local)
Anonymous bump allocator. Freed at `}`. Equivalent to RAII arena; cannot be returned from the function. Use for temporary work within a function.

```skald
fn process_actors(actors: arr<ref<AActor>>) {
    @arena {
        let positions = arr<v3>::with_capacity(actors.len())  // arena-allocated
        for actor in actors {
            positions.push(actor.get_actor_location())
        }
        // ... use positions ...
    }  // arena freed here, positions deallocated
}
```

#### 9.2.3 `@region(name)` (named)
Named arena that lives until explicit `region::drop(name)` or end of frame (if name matches a known frame-scoped region). Captured closures inherit the region. Use for cross-function temporary state.

```skald
@region("ai_tick")
fn tick_ai(world: ref<UWorld>) {
    let buf = arr<v3>::with_capacity(1024)  // allocates in "ai_tick" region
    // ...
}

// Later, at frame end:
fn end_frame() {
    region::drop("ai_tick")  // frees all "ai_tick" allocations
}
```

### 9.3 Write barriers and `skald::Ref<T>`

**Universal handle for cross-heap references:** `skald::Ref<T>` (Skald-side) wraps a `UObject*` and is the only way to store a UObject reference in a Skald-heap object.

```rust
// skald-runtime/src/ref.rs (sketch)
pub struct Ref<T> {
    raw: *mut u8,         // UObject* — type-erased
    slot: *mut Slot,      // pointer to this ref's slot in the owning arena's slot list
    _marker: PhantomData<T>,
}

impl<T: UObject> Ref<T> {
    pub fn new(obj:NonNull<T>, arena:&Arena) -> Ref<T> {
        let slot = arena.register_uobject_slot(obj.as_ptr() as *mut u8);
        Ref { raw: obj.as_ptr() as *mut u8, slot, _marker: PhantomData }
    }
    pub fn get(&self) -> &T {
        // In dev builds: check FQuiescentScope::is_gc_active(); if so, panic
        // (Skald code should never touch UObject memory during GC stop-the-world)
        unsafe { &*(self.raw as *const T) }
    }
}

impl<T> Drop for Ref<T> {
    fn drop(&mut self) {
        // Deregister this slot from the arena's slot list
        unsafe { (*self.slot).arena.deregister_slot(self.slot); }
    }
}
```

**Write barrier:** the barrier fires only on Skald-heap → UObject assignment. The inline check is:

```rust
// Inlined at every `let x: Ref<T> = ...` site
#[inline(always)]
fn write_barrier_check(slot: *mut Slot, obj: *mut u8) {
    slot.obj.store(obj as usize, Ordering::Relaxed);
    // If GC is currently marking, we need to mark this slot dirty so the
    // collector re-visits it. Check a thread-local flag set by FSkaldRefCollector.
    if SKALD_GC_ACTIVE.load(Ordering::Relaxed) {
        write_barrier_slow(slot);  // out-of-line call
    }
}
```

UObject → UObject assignments use UE's existing `TObjectPtr` write barrier — Skald does not touch them.

### 9.4 Arena → UObject reference lifecycle

Each arena (named or anonymous) owns a `Vec<Slot>` of UObject references. When the arena is dropped, its slot list is bulk-deregistered from `FSkaldRefCollector`.

**Closure promotion.** If a closure that captures a `Ref<T>` is stored in a `pub var` slot on a UObject (i.e., it outlives its arena), it must be **promoted** to the Skald persistent heap. This triggers a heap copy of the closure and re-registration of its `Ref<T>` slots with the persistent heap's slot list.

Promotion is automatic when:
1. A closure is assigned to a `pub var` field of type `ref<SkaldClosure>` (Skald's `UScriptStruct` wrapping a closure).
2. A closure is returned from a function annotated `@persistent`.
3. A closure is captured into another closure that is itself persistent.

Promotion is a compile error if:
1. The closure captures a stack local (not arena, not persistent) — `Ref<T>` from a stack local cannot outlive the stack frame.
2. The closure captures an `@arena` value directly (the arena-allocated data) — only `Ref<T>` to UObjects can be promoted, not arena-allocated data itself.

```skald
@arena fn make_callback() -> fn() {
    let local = compute_something()  // arena-allocated
    // return || { log::info(local) }  // COMPILE ERROR: closure captures arena data
    let snapshot = local.clone_to_persistent()  // explicit copy
    return @persistent || { log::info(snapshot) }  // OK
}
```

### 9.5 Closures

Closures are non-reflected reference types. Each captures live in:
- Arena (default, if the closure doesn't escape the arena's scope).
- `@region(name)` (if the closure is stored in a region-scoped collection).
- Persistent heap (if `@persistent` or automatically promoted).

Captured `Ref<T>` registers a slot with the owning arena/persistent heap's slot list. When the arena/persistent heap is dropped, the slot is deregistered.

### 9.6 Strings

- `str` literals are static (`.rodata`), no allocation.
- Owned strings → Skald arena or persistent heap (depending on context).
- Crossing to C++ FFI → converts to `FString` (allocates UE heap copy). The `@borrow` annotation skips the copy and passes `FStringView` for read-only callees:

```skald
// Skald side:
fn log_message(@borrow msg: str) {
    ue_log(msg)  // passes FStringView, no FString allocation
}

// Shim side:
extern "C" void skald_log_message_borrow(const char* data, int32 len) {
    FStringView view(UTF8_TO_TCHAR(data), len);
    UE_LOG(LogSkald, Log, TEXT("%s"), *FString(view));
}
```

---

## 10. Reflection Integration

### 10.1 Decision: Path A (Synthetic C++ Shims) with Path C augmentation

**Why Path A:** Reusing UHT's validation, constinit emission, module-init machinery, hot-reload hooks, and cooking integration is the only sane choice. Path A is the exact trick Verse uses (`UVerseClass` is declared via a UHT-processed C++ header). Reusing UHT = zero risk of version drift across UE 5.4 / 5.5 / 5.6 / 6.x.

**Why Path C augmentation:** A `[UhtCodeGeneratorInjector]`-registered UHT plugin emits `static_assert`s into `.gen.cpp`, verifying Skald's layout assumptions match UHT's. This catches layout drift at compile time.

**Why not Path B (direct metadata generation):** highly fragile if Epic changes internal layouts of `FClassParams`, `FFunctionParams`, `FPropertyParams` in future engine updates. Path B is faster (no C++ compile step) but risks breakage on every UE release. v0.2 proposed Path B as a future fallback; v0.3 keeps it as a documented escape hatch (§17) but does not implement it.

### 10.2 Shim layout

Source `Game/AI/Sentry.skald`:
```skald
/// A stationary defensive structure that detects and engages enemies.
pub class Sentry : AActor, abstract {
    /// Maximum distance at which the sentry can detect targets.
    pub var sight_radius: f32 = 500.0, replicated, clamp(0, 5000), category="AI|Sight"

    /// The current target, if any. Replicated to clients.
    pub var target: weak<AActor>, replicated

    /// Last known position of the target.
    pub var last_seen_pos: v3 = v3::zero(), replicated

    /// Alert the sentry. Returns true if the alert was acknowledged.
    pub fn alert(intensity: f32) -> bool, reliable, category="AI" {
        // ... Skald body ...
        return true
    }

    fn begin_play() override {
        super.begin_play()
        self.patrol()
    }
}
```

Generated `Intermediate/.../UHT/Shims/Sentry.skald.gen.h`:
```cpp
// AUTOGENERATED BY skaldc — DO NOT EDIT
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sentry.skald.gen.generated.h"  // UHT will create this

UCLASS(BlueprintType, Blueprintable, Abstract, Config=Engine, meta=(SkaldGenerated))
class GAME_API ASentry : public AActor {
    GENERATED_BODY()

public:
    /// Maximum distance at which the sentry can detect targets.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="AI|Sight", meta=(ClampMin="0", ClampMax="5000"))
    float sight_radius = 500.0f;

    /// The current target, if any. Replicated to clients.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="Sentry")
    TWeakObjectPtr<AActor> target;

    /// Last known position of the target.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="Sentry")
    FVector last_seen_pos = FVector::ZeroVector;

    /// Alert the sentry. Returns true if the alert was acknowledged.
    UFUNCTION(BlueprintCallable, Reliable, Category="AI")
    bool alert(float intensity);

    virtual void BeginPlay() override;
};
```

### 10.3 Shim `.cpp` (one per module, auto-generated)

`Intermediate/.../UHT/Shims/Game_Skald_Shims.cpp`:
```cpp
// AUTOGENERATED BY skaldc — DO NOT EDIT
#include "Sentry.skald.gen.h"
#include "SkaldRuntime/SkaldThunks.h"

// ---- Virtual override thunks ----
// The C++ compiler builds the vtable; the thunk forwards to Skald's emitted body.
void ASentry::BeginPlay() {
    skald_Game_ASentry_BeginPlay_body(this);
}

// ---- @ufunc thunks ----
// These are NOT method bodies. They are FNativeFuncPtr-shaped functions
// registered via UClass::AddNativeFunction at module init.
DECLARE_SKALD_FUNCTION(skald_Game_ASentry_alert_thunk)
{
    P_GET_PROPERTY(FFloatProperty, Intensity);
    P_FINISH;
    bool RetVal = skald_Game_ASentry_alert_body(Context, Intensity);
    *static_cast<bool*>(RESULT) = RetVal;
}
```

This shim `.cpp` is added to the module's compile list via `FilesToGenerate` (this is the *correct* use of `FilesToGenerate` — it accepts C++ source, not object files).

### 10.4 Module init registration

`Intermediate/.../UHT/Shims/Game_Skald_Init.cpp`:
```cpp
// AUTOGENERATED BY skaldc — DO NOT EDIT
#include "Sentry.skald.gen.h"
#include "SkaldRuntime/SkaldThunks.h"

// Called from the module's StartupModule() — Skald's UBT plugin injects this call.
extern "C" __declspec(dllexport) void Skald_RegisterGameFunctions() {
    UClass* SentryClass = ASentry::StaticClass();
    SentryClass->AddNativeFunction(TEXT("alert"), &skald_Game_ASentry_alert_thunk);
    // ... one line per pub fn ...
}
```

`Skald_Register<Module>Functions` is called from `FGameModule::StartupModule()` via a one-line addition to the module's `Module.cpp` (the only manual C++ change required, and it's a single line — could be auto-injected by the UBT plugin in a future version).

### 10.5 Specifier validation

Two-phase:
1. **Syntax validation** at Skald parse time: typo'd `replictated` → error with suggestion. Done by `skald-modifiers` using a hardcoded list of valid modifiers.
2. **Semantic validation** delegated to UHT: e.g., `replicated` requires the class to be replicated-marked. UHT's error then surfaces in Skald's error stream via the UBT log adapter. Line numbers are remapped to `.skald` source via `#line` directives in the shim.

### 10.6 Default flag computation

The `skald-modifiers` crate computes the *effective* UE flag set for each member:

```
effective_flags = defaults[member_kind] ∪ explicit_modifiers ∪ doc_comment_overrides
```

Where:
- `defaults[member_kind]` is the table from §6.2 (e.g., `pub var` → `EditAnywhere | BlueprintReadWrite | Category=<ClassName>`).
- `explicit_modifiers` are the comma-separated modifiers from the source.
- `doc_comment_overrides` come from `///` doc comments (ToolTip, DocumentationLink, etc.).

Some modifiers *replace* defaults rather than add to them:
- `readonly` replaces `EditAnywhere` with `VisibleAnywhere` AND `BlueprintReadWrite` with `BlueprintReadOnly`.
- `pure` replaces `BlueprintCallable` with `BlueprintPure`.
- `editdefaults_only` replaces `EditAnywhere` with `EditDefaultsOnly`.
- `visibleanywhere` replaces `EditAnywhere` with `VisibleAnywhere` (but keeps `BlueprintReadWrite`).

The replacement semantics are encoded in `skald-modifiers/src/replacements.rs`:
```rust
fn apply_modifier(flags: &mut UPropertyFlags, modifier: &Modifier) {
    match modifier {
        Modifier::ReadOnly => {
            flags.remove(UPropertyFlags::EDIT_ANYWHERE);
            flags.remove(UPropertyFlags::BLUEPRINT_READWRITE);
            flags.insert(UPropertyFlags::VISIBLE_ANYWHERE);
            flags.insert(UPropertyFlags::BLUEPRINT_READ_ONLY);
        }
        Modifier::Pure => {
            flags.remove(UPropertyFlags::BLUEPRINT_CALLABLE);
            flags.insert(UPropertyFlags::BLUEPRINT_PURE);
        }
        Modifier::EditDefaultsOnly => {
            flags.remove(UPropertyFlags::EDIT_ANYWHERE);
            flags.insert(UPropertyFlags::EDIT_DEFAULTS_ONLY);
        }
        // ... etc
    }
}
```

### 10.7 Layout drift detection

The Skald UHT plugin (`SkaldUhtPlugin.cs`) registers a `[UhtCodeGeneratorInjector]` that emits `static_assert`s into each `Sentry.skald.gen.cpp`:

```cpp
// Injected by SkaldUhtPlugin into Sentry.skald.gen.cpp
static_assert(sizeof(ASentry) == 248, "Skald/UHT layout mismatch for ASentry (expected 248 bytes)");
static_assert(offsetof(ASentry, sight_radius) == 240, "Skald/UHT layout mismatch for ASentry::sight_radius");
static_assert(offsetof(ASentry, target) == 232, "Skald/UHT layout mismatch for ASentry::target");
static_assert(offsetof(ASentry, last_seen_pos) == 216, "Skald/UHT layout mismatch for ASentry::last_seen_pos");
```

The expected offsets come from `skaldc --emit-layout-json`, which runs Skald's layout pass and produces `Game.layout.json`:
```json
{
  "ASentry": {
    "size": 248,
    "fields": {
      "sight_radius": { "offset": 240, "size": 4, "type": "float" },
      "target": { "offset": 232, "size": 8, "type": "TWeakObjectPtr<AActor>" },
      "last_seen_pos": { "offset": 216, "size": 24, "type": "FVector" }
    }
  }
}
```

**Note on `offsetof`:** `offsetof` on non-standard-layout types is technically UB in C++. The shim uses `__builtin_offsetof` (GCC/Clang) or MSVC's `offsetof` (which works in practice on non-standard-layout under `/permissive-`). Both compilers emit a warning that we suppress with `#pragma warning(suppress: 4200)` or `__pragma(warning(suppress: 4116))`. This is a known trade-off; alternative is runtime validation via `FProperty::GetOffset_ForInternal()` at first-access, which is slower but standard-compliant.

### 10.8 Module init

Skald injects nothing custom into UE's module registration. UHT's existing `Z_Register_Module<Game>` picks up the shim-declared classes normally. The only Skald-specific addition is the `Skald_Register<Module>Functions` call from `StartupModule()` (§10.4).

---

## 11. FFI Strategy

### 11.1 FFI model

UFunctions are NOT called through C++ vtables. They're called through `UFunction::Func`, which is a `FNativeFuncPtr` (`Public/UObject/CoreNative.h:19`):

```cpp
typedef void (*FNativeFuncPtr)(UObject* Context, FFrame& TheStack, RESULT_DECL);
```

Verse does exactly this — `VVMVerseFunction.cs:32-53` registers `InvokeCalleeThunk` via `SetNativeFunc`. The Verse procedure body is **never** a C++ method.

**v0.3 splits the FFI model cleanly:**

| Member kind | Shim declares | Body lives in | Symbol type |
|---|---|---|---|
| `pub fn x() override` (virtual override of inherited C++ method) | C++ method declaration + `__forceinline` thunk in shim `.cpp` | Skald Cranelift IR | `extern "C"` Skald-mangled, called via vtable thunk |
| `pub fn x()` (Skald-defined UFunction) | `UFUNCTION` declaration only — no body, no symbol in shim | Skald Cranelift IR | `extern "C"` thunk (`skald_<M>_<C>_<F>_thunk`), registered via `AddNativeFunction` |
| `pub var x: T` (reflected field) | `UPROPERTY` declaration | N/A (data, not code) | Direct offset access via `static_assert`-validated layout |
| `private fn x()` (non-reflected helper) | Not in shim at all | Skald Cranelift IR | `extern "C"` Skald-internal symbol (`skald_<M>_<F>`) |
| C++-side function called from Skald | Declared in C++ normally | C++ | Skald calls via generated `extern "C"` wrapper |

**This eliminates ~90% of ABI risk.** The only place MSVC name mangling is needed is for `virtual override` thunks, and there the C++ compiler emits the symbol (Skald only emits the body, called via `extern "C"` from the thunk).

### 11.2 Binding generation

A tool `skald-bindgen` (run once per UE version, cached, checked into the Skald plugin) generates bindings for the entire UE5 API surface:

1. **Reflected mode** (primary): parses UHT's reflection JSON. Skald's UHT exporter plugin emits a `ue5-reflection.json` blob at first build describing every `UCLASS`/`USTRUCT`/`UENUM`/`UFUNCTION`/`UPROPERTY` in every loaded module. `skald-bindgen` consumes this and produces:
   - `skald-bindings.bin` — a binary blob (via `bincode`) of type/function signatures with mangled symbol names per platform. Loaded by `skald-resolve` and `skald-lsp`.
   - Skald-side `extern` declarations (e.g., `extern fn set_actor_location(self: ref<AActor>, loc: v3) -> bool`).
   - C-API shim C++ wrappers (e.g., `extern "C" bool skald_ue_AActor_SetActorLocation(AActor* Self, const FVector& Loc)`).

2. **Header mode** (fallback): for non-reflected APIs (`FMath::Sin`, math types), uses `libclang` to parse a curated header list. Generates explicit instantiations on-demand.

3. **Template instantiation**: when Skald code mentions `arr<v3>::add`, `skaldc` emits a tiny `.cpp` snippet `template void TArray<FVector>::Add(const FVector&);` via `FilesToGenerate`. The C++ compiler builds the explicit instantiation; Skald links to it.

### 11.3 Call syntax & name mapping

```skald
let s = math::sin(t)                 // FMath::Sin (header mode binding)
actor.set_actor_location(p)          // direct C++ method (reflected mode)
actor.alert(0.5)                     // Skald-defined pub fn — dispatches via UFunction
let val = actor.call("Alert", 0.5)   // late-bound UFunction::Invoke (rare, opt-in)
```

Method names map snake_case ↔ PascalCase automatically at the FFI boundary:
- `actor.set_actor_location(p)` → `AActor::SetActorLocation(p)` (C++ method call).
- `actor.alert(0.5)` → `UFunction "alert"` (registered via `AddNativeFunction`).

The mapping is configurable per binding (a `renames.toml` file in the Skald plugin), but defaults to snake_case on the Skald side.

### 11.4 Performance characteristics

- **Reflected method call** (e.g., `actor.alert(...)`): one indirection through `UFunction::Func` → `skald_<...>_thunk` → Cranelift-compiled body. ~10ns overhead vs. direct C++ call (the UFunction indirection is unavoidable; UE Blueprint has the same cost).
- **Reflected field access** (e.g., `actor.sight_radius`): direct memory load at `static_assert`-validated offset. Zero overhead vs. C++.
- **Non-reflected call** (e.g., `math::sin`): direct `extern "C"` call. Zero overhead vs. C++.
- **Late-bound `call()`**: `~200ns` per call (UFunction::Invoke + arg marshalling). For editor scripting only; do not use in hot paths.

### 11.5 Cross-module inlining

v0.3 does not support cross-module inlining (Cranelift cannot inline into C++ object files and vice versa). For the rare case where inlining matters (e.g., a tight math loop calling `FMath::Sin`), use the `@inline` annotation:

```skald
@inline(always)
fn fast_sin(x: f32) -> f32 {
    // Skald emits this function as a C++-visible .inl file
    // (via FilesToGenerate) so the C++ compiler can inline it
    // into surrounding code.
}
```

This generates a `fast_sin.inl` file in `FilesToGenerate` containing the C++-translated body. C++ code can `#include "fast_sin.inl"` to inline. Skald code calling `fast_sin` uses the Skald-emitted Cranelift body. This is a v0.3 stopgap; v0.4 may add cross-module inlining via LLVM migration (§17).

---

## 12. OOP & ECS

### 12.1 OOP-first

UE5 is OOP-first (`UObject → AActor → APawn`). Skald follows. ECS via opt-in `@mass` annotations — Skald does not force ECS, but makes it ergonomic when needed.

### 12.2 Class syntax

```skald
pub class Patroller : ACharacter {
    pub var speed: f32 = 300.0, category="AI"
    pub var route: ref<AActor>?, category="AI"

    pub fn patrol(), category="AI" {
        // ... Skald body ...
    }

    fn begin_play() override {
        super.begin_play()
        self.patrol()
    }

    // Skald-defined virtual (can be overridden by Skald subclasses)
    fn on_reached_destination() virtual {
        log::info("Reached destination")
    }
}

/// Interface for things that can take damage.
pub trait Damageable {
    /// Apply damage to this object. Returns actual damage applied.
    pub fn take_damage(amount: f32) -> f32
}
```

### 12.3 Inheritance rules

- **Single inheritance** from one C++ or Skald `class`.
- **Multiple trait implementations** (via `impl Trait for Type` blocks; trait declared `pub trait` becomes a `UInterface`).
- **`override` keyword required** for inherited virtual methods (Java/C# style; avoids C++'s silent shadowing footgun).
- **`virtual` keyword** marks a Skald-defined method as overridable by Skald subclasses. Goes through a Skald-managed vtable (a `Vec<fnptr>` in the `USkaldClass` subclass), not the C++ vtable.
- **Skald cannot define new C++-visible virtual methods** on a UClass. C++ code cannot call Skald-defined virtuals directly. This is a deliberate restriction: emitting C++ vtable entries from Cranelift is the bug farm v0.1 walked into. Skald virtuals are dispatched via Skald's own vtable.

### 12.4 Mass (ECS)

```skald
@mass_fragment
pub struct Position : POD {
    var v: v3
}

@mass_fragment
pub struct Velocity : POD {
    var v: v3
}

@mass_processor(group="movement", tick_before="PhysicsMass")
pub fn move_things(q: query<mut Position, Velocity>, dt: f32) {
    @simd
    for (mut pos, vel) in q {
        pos.v += vel.v * dt
    }
}
```

Desugars to:
- `Position` → `FPositionFragment : public FMassFragment` (shim header, UHT-processed).
- `move_things` → `class UMoveThingsProcessor : public UMassProcessor` (shim header) with `Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)` body in Skald.
- `query<mut Position, Velocity>` → `FMassEntityQuery` built at processor init, with `mut` mapping to `FMassFragmentAccess::ReadWrite` and read-only to `FMassFragmentAccess::ReadOnly`.

### 12.5 Dual world bridge

An `AActor` can spawn a Mass entity and hold its handle:

```skald
pub class Swarm : AActor {
    var entities: arr<FMassEntityHandle>

    fn spawn_one(p: v3) {
        let e = mass::spawn(self.world(), Position{v:p}, Velocity{v:v3(0,0,0)})
        self.entities.push(e)
    }
}
```

Mass processors can `query<...>` over fragments that include an `FActorFragment` to reach back to actor refs. Standard UE Mass pattern, no Skald-specific magic.

---

## 13. Concurrency

### 13.1 Threading model

- **Game thread by default.** All `pub class` methods are implicitly `!Send`.
- **`async fn`** desugars to a state machine resumable on UE's `TaskGraph` / `UE::Tasks`.
- **`await`** yields to the scheduler. Default resume thread = same as the spawner (game thread → game thread).
- **`spawn worker { ... }`** runs a `Send`-only closure on a worker thread.

### 13.2 UObject access from worker threads

`ref<UObject>` is `!Send`. Compile error to capture in `spawn worker`. To get UObject data onto a worker, snapshot it on the game thread into a POD struct, send the POD to the worker, worker returns a command struct, game thread applies the command:

```skald
// POD snapshot — can be sent to worker threads
@pod
struct ActorSnapshot {
    pos: v3
    vel: v3
    health: f32
}

@pod
struct AiCommand {
    move_to: v3
    set_health: f32
}

fn tick_ai_on_worker(actors: arr<ref<AActor>>) {
    // 1. Snapshot on game thread
    let snapshots: arr<ActorSnapshot> = actors.map(|a| ActorSnapshot{
        pos: a.get_actor_location(),
        vel: a.get_velocity(),
        health: a.health,
    })

    // 2. Send to worker
    spawn worker {
        let commands = compute_ai(snapshots)  // pure data, no UObject access

        // 3. Enqueue commands back to game thread
        game_thread::run(move || {
            for (actor, cmd) in actors.zip(commands) {
                actor.set_actor_location(cmd.move_to)
                actor.health = cmd.set_health
            }
        })
    }
}
```

**Alternative for read-only access**: a `pin<T>` type that is `Send` only if `T: Sync` and only for read-only access. Recommended against in docs — the snapshot pattern is safer and clearer.

### 13.3 Async/await

```skald
async fn fetch_save_data(save_id: i64) -> SaveData {
    let blob = await io::read_async(save_id)  // suspends, resumes on game thread
    parse_save(blob)
}

// Caller:
let data = spawn async { fetch_save_data(42) }.await
```

Desugars to a state machine:
- Each `await` is a suspension point.
- The state machine is heap-allocated on the Skald persistent heap.
- Resume is via UE's `UE::Tasks::Task` system.
- Cancellation propagates via `CancellationToken` (Skald stdlib type).

### 13.4 Parallel for

```skald
parallel for i in 0..particles.len() {
    particles[i].pos += particles[i].vel * dt
}
```

Maps to `ParallelFor`. Body must capture only `Send` data. Compile error if body captures `ref<UObject>` or `weak<T>`.

### 13.5 No data races

`mut` reference uniqueness is enforced *within* a thread (single mut borrow at a time, simple flow analysis — not a full borrow checker). Cross-thread sharing requires `Sync` + atomics or `mutex<T>`. No full borrow-checker — overkill for gameplay code, retained for `@unsafe`.

---

## 14. Hot Reload

### 14.1 Two modes

**Shipping/Development builds:** No hot reload of Skald code (same as C++). Edit → rebuild module → relaunch or use Live Coding.

**Editor dev loop:** Patch-in-place hot reload via Cranelift recompilation. (Note: this is *not* a separate Cranelift JIT — it's the same Cranelift backend, invoked incrementally on file change.)

### 14.2 Patch-in-place flow

1. File save event observed by Skald editor plugin (UE `IDirectoryWatcher`).
2. Changed file → recompiled by `skaldc --patch <Module>`:
   - Lex + parse only the changed file.
   - Type-check against the existing module's symbol table.
   - Compute **per-function signature hash** (see §14.3).
3. Recompile changed functions via Cranelift → memory image (no `.obj` file).
4. For each changed function:
   - If signature unchanged → patch `UFunction::Func` pointer in place (§14.4).
   - If signature changed → trigger UE's reinstancing path (§14.5).
5. If a removed field had live references in C++ → compile error at patch time, abort reload, keep old code running.

### 14.3 Per-function signature hash

Not a class-level hash. A per-function hash that includes:
- Parameter types (mangled).
- Parameter names (Blueprint-visible).
- Return type.
- `FunctionFlags`.
- `FProperty` chain layout (offsets, sizes, alignments).

Adding a parameter to one function changes that function's hash, even if the class shape is otherwise identical. The patch is per-function — only functions whose hash changed get their `Func` pointer patched.

### 14.4 Quiescent-state handshake

v0.1 proposed patching `UFunction::Func` pointers "in place" without waiting for in-flight calls. This is unsafe — if thread A is mid-`ProcessEvent` for `alert` (frame locals allocated, `Func` already loaded into a register), and thread B patches `Func` mid-execution, thread A returns into freed memory.

**v0.3 uses UE's existing `FGCScopeGuard` as the quiescent mechanism:**

```cpp
// In SkaldRuntime's hot-reload path
void Skald_PatchFunction(UFunction* Func, FNativeFuncPtr NewFunc) {
    // Acquire GC lock — this blocks until all async threads have left UObject code
    // (FGCScopeGuard AcquireGCLock waits for AsyncCounter == 0)
    FGCScopeGuard Guard;

    // Now no thread is inside ProcessEvent for this Func.
    // Safe to patch.
    Func->Func = NewFunc;

    // Guard releases on scope exit — async threads can resume.
}
```

This is the only safe mechanism. It's the same handshake UE's own GC uses (`Private/UObject/GCScopeLock.h:109-137`).

**Cost:** acquiring the GC lock has ~1-10ms latency depending on how many async threads are in UObject code. For typical editor scenarios (no async loading in flight), this is <1ms. Acceptable.

**Non-support for C++ member-function-pointer capture:** if C++ code captured `&ASentry::alert` as a member function pointer (rare but legal), it holds the old pointer. After patching, that pointer still works (it points at the old code, which is still in memory until unloaded), but the old code references stale captures. Documented as unsupported: "C++ code must not take pointers to `pub fn` methods."

### 14.5 Shape-change reinstancing

If a function's signature changed (new parameter, different return type), or a `pub var` was added/removed/retyped, the UClass itself needs reinstancing. This uses UE's existing `FBlueprintCompileReinstancer`:

1. `skaldc` detects shape change, emits new shim header.
2. Skald's editor plugin calls `FBlueprintCompileReinstancer::CreateForClass(OldClass)`.
3. Reinstancer creates a new `USkaldClass` (Skald's UClass subclass) with the new layout.
4. Reinstancer walks all instances of the old class and:
   - Allocates a new instance of the new class.
   - Copies field-by-field via reflection (fields that match by name and type).
   - Missing fields use `VPlaceholder`-style sentinels (default value + warning logged).
   - Extra fields in the new class get their CDO default values.
5. Old class is marked `RF_PendingKill` and GC'd later.

`USkaldClass` overrides `UClass::GetReinstancedClassPathName_Impl` (verified seam — Verse uses this at `VVMVerseClass.cpp:815-823`):

```cpp
FString USkaldClass::GetReinstancedClassPathName_Impl() const {
    return PreviousPathName;  // set when this class replaces an older version
}
```

### 14.6 UX

- File save → toast "Skald: recompiling Sentry..." → 80-300ms → toast "✓ patched 3 functions, 41 instances migrated".
- Shape change → toast "Skald: reinstancing required" → editor pauses simulation if PIE → migrates → resumes. Sub-second for <10k instances.
- Incompatible change (removed field still referenced in C++): red toast with the specific error; old code keeps running, no crash.

---

## 15. Developer Experience

### 15.1 Hello world (default surface)

```skald
pub class Greeter : AActor {
    pub var greeting: str = "Hello"
    pub var target_tag: name = "Player"

    fn begin_play() override {
        super.begin_play()
        for actor in world().actors_with_tag(self.target_tag) {
            log::info(f"{self.greeting}, {actor.name()}!")
        }
    }

    pub fn shout() {
        log::info(f"{self.greeting.to_upper()}!!!")
    }
}
```

No annotations, no modifiers. The class is Blueprintable, the fields are EditAnywhere + BlueprintReadWrite, the `shout` method is BlueprintCallable. The `begin_play` method is a C++ virtual override (the `override` keyword handles it). This is the Roblox Lua experience: **just write code, it works**.

### 15.2 Hot-path version (opt-in complexity)

```skald
pub class ParticleSim : AActor {
    // @layout(soa) restricted to private fields in v0.3 (see §5.6)
    @layout(soa)
    private var particles: arr<Particle> = arr::new()

    pub var particle_count: i32 = 0, readonly

    @hot
    fn tick(dt: f32) override {
        @region("sim_frame")
        @simd
        for mut p in self.particles.mut_iter() {
            p.vel += v3(0, 0, -9.8) * dt
            p.pos += p.vel * dt
        }

        @unsafe {
            // direct ptr write into mapped GPU buffer
            let dst: *mut v3 = gpu_buf.raw_mut()
            mem::copy_nonoverlapping(self.particles.pos_ptr(), dst, self.particles.len())
        }

        self.particle_count = self.particles.len()
    }
}
```

### 15.3 Mid-complexity version (modifiers in use)

```skald
/// A stationary defensive structure that detects and engages enemies.
pub class Sentry : AActor, abstract {
    /// Maximum distance at which the sentry can detect targets.
    pub var sight_radius: f32 = 500.0, replicated, clamp(0, 5000), category="AI|Sight"

    /// The current target, if any. Replicated to clients.
    pub var target: weak<AActor>, replicated

    /// Last known position of the target.
    pub var last_seen_pos: v3 = v3::zero(), replicated

    /// Alert the sentry. Returns true if the alert was acknowledged.
    pub fn alert(intensity: f32) -> bool, reliable, category="AI" {
        if intensity < 0.0 || intensity > 1.0 {
            return false
        }
        self.target?.alert_targeted(intensity)
        return true
    }

    /// Compute the current threat score. Pure function — no side effects.
    pub fn compute_threat() -> f32, pure, category="AI" {
        let distance = self.target?.get_distance_to(self) ?? 99999.0
        return (self.sight_radius - distance) / self.sight_radius
    }

    fn begin_play() override {
        super.begin_play()
        log::info(f"Sentry spawned at {self.get_actor_location()}")
    }
}
```

### 15.4 Error messages

**Type mismatch:**
```
error[E0101]: type mismatch
 --> Game/AI/Sentry.skald:14:23
  |
14 |     self.target = pos
  |                   ^^^ expected `weak<AActor>`, got `v3`
  |
help: did you mean to assign to `self.last_seen_pos` instead?
      (field `last_seen_pos: v3` exists on `Sentry`)
```

**Missing reflection (forgot `pub`):**
```
warning[W0420]: field is not reflected — editor and Blueprint cannot see it
 --> Game/Pawn.skald:8:9
  |
8 |     var health: f32 = 100.0
  |     ^^^ not reflected (no `pub` keyword)
  |
help: add `pub` to make it editable and Blueprint-readwrite:
      pub var health: f32 = 100.0
      or for read-only:  pub var health: f32, readonly
      or to suppress:    @allow_private
```

This is the **inverse** of v0.2's error. In v0.2, forgetting `@uprop` silently made the field invisible. In v0.3, forgetting `pub` produces a warning suggesting you make it `pub` if you wanted reflection. The default behavior is now *visible* rather than *invisible*.

**Modifier typo:**
```
error[E0301]: unknown modifier `replictated`
 --> Game/AI/Sentry.skald:5:45
  |
5 |     pub var sight_radius: f32 = 500.0, replictated, clamp(0, 5000)
  |                                         ^^^^^^^^^^^ unknown modifier
  |
help: did you mean `replicated`?
      pub var sight_radius: f32 = 500.0, replicated, clamp(0, 5000)
```

**GC violation:**
```
error[E0820]: UObject reference captured in worker thread
 --> Game/Worker.skald:22:17
  |
20 |     spawn worker {
21 |         for enemy in self.enemies {
22 |             enemy.kill()
  |             ^^^^^ `ref<AEnemy>` is `!Send`, cannot be captured in `spawn worker`
  |
help: snapshot the UObject data on the game thread before spawning:
      let snapshots = self.enemies.map(|e| EnemySnapshot::from(e))
      spawn worker {
          let commands = compute(snapshots)
          // ...
      }
```

**Hot-reload incompatible change:**
```
error[E0901]: cannot hot-reload — removed field has live C++ references
 --> Game/AI/Sentry.skald:5:5
  |
5 |     pub var sight_radius: f32 = 500.0    // (deleted in this edit)
  |     ^^^ removed, but `ASentry::sight_radius` is referenced by:
  |         - Game/AI/SentryComponent.cpp:42
  |         - Content/Blueprints/BP_Sentry.uasset (3 nodes)
  |
help: keep the field with `@deprecated` annotation, or remove the C++ references first:
      @deprecated("use `sight_range` instead")
      pub var sight_radius: f32 = 500.0
```

### 15.5 Error philosophy

- Name the file, name the line, name the intent.
- Suggest a one-line fix when possible.
- Never show template substitution chains.
- Never show Cranelift IR or LLVM IR.
- Never show UHT's internal errors directly — remap them to Skald source via `#line` directives.
- All errors include a stable error code (`E0101`, `W0420`) for documentation lookup.
- **Default-on reflection means warnings, not silent failures.** If a field is `var` without `pub` and looks like it should be reflected (e.g., it's in a `pub class` and has a default value), emit a warning suggesting `pub`. The developer can suppress with `@allow_private`.

### 15.6 IDE / Language server

- **`skald-lsp`** — Rust-based LSP server, ships with the plugin.
- Uses `salsa` for incremental computation — file edits invalidate only the affected query subgraph.
- Reads the same UHT reflection JSON that `skald-bindgen` produces → knows every UE type without parsing C++ headers.
- `libclang` fallback for non-reflected APIs.
- Features:
  - Go-to-definition (crosses Skald ↔ C++ ↔ Blueprint via UE asset registry).
  - Hover (shows type, effective UPROPERTY/UFUNCTION flags, Blueprint call cost).
  - Inlay hints (inferred types, effective reflection flags, replicated-property network cost).
  - Code completion:
    - UE types and Skald types.
    - Method names (snake_case ↔ PascalCase mapping shown in completion item detail).
    - **Modifier autocomplete**: type `, ` after a declaration and get suggestions for all valid modifiers for that declaration kind.
    - **Default-flag preview**: hover over a `pub var` to see the effective UPROPERTY flags before adding modifiers.
  - Diagnostics (live error reporting as you type).
  - Refactoring (rename — updates Skald source, shims, and Blueprint references).
  - **"What does this generate?" action**: right-click a `pub class` → "Show generated C++ shim" → opens the shim header that `skaldc` would emit, so developers can verify the reflection flags.

### 15.7 Debugger

- DWARF (Linux/Mac) and PDB (Windows) emitted by Cranelift.
- Native breakpoints work in VS, Rider, LLDB.
- No special Skald debugger required — but `skald-lsp` provides a "Skald view" in the editor showing live Skald class hierarchy, hot-reload status, and per-function signature hashes.

### 15.8 Documentation

- `///` doc comments render to Markdown.
- `skald-doc` tool (separate crate, post-v1) generates HTML docs.
- UE-integrated: Skald classes appear in UE's class picker, Blueprint node search, and details panel just like C++ classes.
- **Default-flag documentation**: every `pub class`/`pub var`/`pub fn` in the generated docs shows a "Defaults applied" section listing the effective UCLASS/UPROPERTY/UFUNCTION flags. This teaches developers what they get for free.

---

## 16. Implementation Roadmap

### 16.1 Phased plan

**Phase 0 — Validate UBT/UHT seam (4 weeks)**
- Write a trivial UE5 plugin with `.UbtPlugin.csproj` and `.Build.cs` using `GenerateHeaderFuncs` to emit a single trivial `.cpp` via `FilesToGenerate`.
- Confirm UBT compiles it and the file appears in `Intermediate/.../Gen/`.
- Write a UHT plugin DLL registering `[UhtCodeGeneratorInjector]` to inject a comment into every `.generated.h`.
- Manually write a `FClassParams` for a trivial UClass, call `ConstructUClass` from a `static FRegisterCompiledInObjects`, confirm the UClass appears in the editor.
- **Deliverable:** proof that all three seams (UBT plugin, UHT plugin, direct ConstructUClass) work without UE5 source modifications.

**Phase 1 — Frontend (10 weeks)**
- `skald-lexer` (2 wk).
- `skald-parser` with `rowan`, including modifier syntax (3 wk).
- `skald-ast` (1 wk).
- `skald-resolve` (2 wk).
- `skald-types` with `ena` unification (2 wk).
- **Deliverable:** Skald source parses, resolves, type-checks. No code generation.

**Phase 1.5 — Modifier system (3 weeks, parallel with Phase 1)**
- `skald-modifiers`: parser, validator, default-flag computation, replacement semantics.
- Unit tests for every UE5 UCLASS/UPROPERTY/UFUNCTION specifier.
- **Deliverable:** given a Skald declaration, the crate produces the exact UE flag set. No integration yet.

**Phase 2 — Cranelift backend, minimal (14 weeks)**
- `skald-hir`, `skald-tir`, `skald-mono` (4 wk).
- `skald-codegen-cranelift`: primitives, control flow, struct/class field access (5 wk).
- `skald-reflection`: shim header emission (using `skald-modifiers` for flag computation), sidecar JSON (2 wk).
- `skald-runtime-cpp`: `FSkaldRefCollector`, thunk utilities, `USkaldClass` (2 wk).
- `skald-ubt-plugin` C# project (1 wk).
- **Deliverable:** Hello-world `pub class AActor` subclass compiles and runs in a shipping build. Fields show up in Details panel, methods are Blueprint-callable.

**Phase 3 — GC bridge + arena (8 weeks)**
- `skald-runtime`: arena allocator, write barrier slow-path (3 wk).
- Closure capture semantics, arena slot lifecycle (3 wk).
- Closure promotion to persistent heap (2 wk).
- **Deliverable:** closures with `Ref<T>` captures work correctly under GC stress tests.

**Phase 4 — Generics, traits, error messages (8 weeks)**
- Monomorphization, trait dispatch (3 wk).
- `pub trait` → `UInterface` integration (2 wk).
- Error message polish, `ariadne` integration, error codes (3 wk).
- **Deliverable:** generic gameplay code compiles; error messages are helpful.

**Phase 5 — Hot reload (18 weeks)**
- Patch-in-place with quiescent-state handshake (4 wk).
- Per-function signature hashing (2 wk).
- `USkaldClass` subclass with `GetReinstancedClassPathName_Impl` (3 wk).
- Reinstancing via `FBlueprintCompileReinstancer` (4 wk).
- Edge cases: closures across reload, async state machines across reload (3 wk).
- Editor plugin: `IDirectoryWatcher`, toast notifications (2 wk).
- **Deliverable:** file save → ~200ms → patched function pointer live in editor.

**Phase 6 — Mass, async, concurrency (8 weeks)**
- `@mass_fragment`, `@mass_processor`, `query<>` desugaring (3 wk).
- `async fn` state machine, `UE::Tasks` integration (3 wk).
- `parallel for`, `Send`/`Sync` enforcement (2 wk).
- **Deliverable:** a Mass-based gameplay sample runs end-to-end.

**Phase 7 — LSP, debugger, docs (8 weeks)**
- `skald-lsp` with `tower-lsp` + `salsa` (5 wk).
- Modifier autocomplete, default-flag preview, "Show generated C++ shim" action (1 wk).
- `skald-bindgen` libclang mode for non-reflected APIs (1 wk).
- Doc generation, sample project (1 wk).
- **Deliverable:** full IDE experience in VS Code / Rider.

**Phase 8 — Bake-off & v1 (6 weeks)**
- Port one Lyra sample feature end-to-end.
- Benchmark vs C++ and Blueprint.
- Documentation, examples, blog post.
- Ship v1.0.

**Total: ~22 months to v1 with 2-3 engineers.**

Hot reload (Phase 5) is the single biggest underestimate in any plan. Verse spent *years* on hot-reload correctness. 18 weeks is aggressive; expect bugs for a year after v1.

### 16.2 Team composition

- **2-3 engineers** for Phases 0-8.
- **1 UE5 integration specialist** (familiar with UBT/UHT internals) across all phases.
- **1 Cranelift expert** for Phases 2-5 (Cranelift's documentation is sparse; an expert who's used it before saves months).
- **1 LSP/tooling engineer** for Phase 7.

### 16.3 Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Cranelift lacks AArch64-Windows maturity | Medium | Blocks console devkits | Restrict v1 to Win64-x86_64, Linux-x86_64; consoles in Phase 9 force LLVM migration (§17) |
| Cranelift lacks PDB parity with MSVC | Medium | Debugger UX in VS | Emit DWARF; recommend Rider/LLDB; VS support via `diasdk` integration in Phase 7+ |
| UE6 reflection rewrite (UHT may be replaced) | Low | Major | Path A insulates if macros stay; Path B (direct `FClassParams`) is prototyped fallback (§17) |
| Hot-reload of suspended coroutines | High | Edge case crashes | v0.3 refuses to hot-reload a function with suspended coroutines; drains or kills first |
| AutoRTFM becomes required for replicated UFunctions in UE6 | Low | Major | Skald `transaction { ... }` primitive backed by AutoRTFM; out of scope for v0.3 |
| Cooking / PSO precaching doesn't pick up Skald classes | Low | Medium | Skald classes ARE UClasses via shims; should work; validate with `-cookflavor` runs in Phase 8 |
| Module circular deps | Medium | Compile error | v0.3 is single-pass per module; two-phase resolution deferred to v0.4 |
| Default-on reflection bloats binary size unexpectedly | Low | Medium | `private` opts out; measure in Phase 8 bake-off |
| Default flags conflict with user expectations | Medium | Minor | Default-flag preview in LSP; "What does this generate?" action; clear documentation |

---

## 17. Migration Path: Cranelift → LLVM

### 17.1 When to migrate

Triggers for migrating from Cranelift to LLVM:
1. **Console support required** (PS5, Switch, Xbox Series). Cranelift lacks backends for these platforms.
2. **Cross-module inlining required** for performance-critical gameplay code (rare; usually means the algorithm needs work, not the compiler).
3. **LTO with C++ side required** for shipping binary size optimization.
4. **UE6 requires AutoRTFM-compatible codegen** for replicated functions — LLVM's pass infrastructure makes this easier.

The migration is **not** a runtime switch — it's a multi-week project to populate `skald-codegen-llvm` and validate parity.

### 17.2 Architecture for migration

`skald-codegen-cranelift` and `skald-codegen-llvm` both implement:
```rust
pub trait Backend {
    fn compile(&mut self, module: &tir::Module, target: &target_lexicon::Triple) -> PathBuf;
    fn name(&self) -> &'static str;
}
```

The TIR is backend-agnostic. Switching backends is a `cargo` feature flag:
```toml
[features]
default = ["cranelift"]
cranelift = ["skald-codegen-cranelift"]
llvm = ["skald-codegen-llvm"]
```

`skald-driver` selects the backend at startup based on the active feature.

### 17.3 LLVM backend specifics

When `skald-codegen-llvm` is populated:
- Uses `inkwell` (Rust LLVM bindings) as the LLVM-C FFI layer.
- Targets the same LLVM version UE5 ships (so Skald-emitted IR links cleanly with C++-emitted IR).
- Enables ThinLTO for cross-module inlining.
- Emits PDBs on Windows (via LLVM's PDB backend) and DWARF elsewhere.

### 17.4 Path B (direct metadata emission) as deeper fallback

If UE6 removes UHT entirely (replacing it with a unified reflection pipeline), the shim-based Path A breaks. Path B (bypass UHT, emit `FClassParams` directly) is prototyped in a separate branch:

- `skald-codegen-llvm` (or Cranelift) emits global variables of type `FClassParams`, `FFunctionParams`, etc. directly in IR.
- Module init registers them via `static FRegisterCompiledInObjects Z_Register_Module<X>{...}`.
- No C++ compilation step for reflected types.

Path B is *not* in scope for v0.3 but is documented as a research item. The TIR is designed to support both paths.

---

## 18. Open Questions

1. **Coroutine hot-reload interaction.** `async fn` state machine layout across hot-reload of partially-suspended coroutines is unsolved. v0.3 prototype: refuse to hot-reload a function while any coroutine is suspended in it; drain or kill first. May need a more sophisticated solution in v0.4.

2. **MSVC ABI for Skald-defined virtuals.** v0.3 restricts Skald-defined virtuals to a Skald-managed vtable (a `Vec<fnptr>` in `USkaldClass`). C++ code cannot call them directly. If a future use case requires C++ to dispatch Skald virtuals, we'd need either (a) Itanium-only support, (b) a vtable layout DSL feeding the shim, or (c) a hybrid where Skald virtuals with `@cpp` annotation get a real C++ vtable entry (emitted via shim). Lean toward (c) if needed; not in scope for v0.3.

3. **`@layout(soa)` on reflected fields.** v0.3 restricts `@layout(soa)` to non-reflected (private) fields. Implementing `FSoaArrayProperty` (custom FProperty subclass with full UHT/editor support) is 4-8 weeks of work and is deferred to v0.4+.

4. **AutoRTFM.** If UE6 requires AutoRTFM-compatible codegen for replicated UFunctions, Skald needs `__autortfm_*` markers in the Cranelift backend. Straightforward but unscoped for v0.3.

5. **Cooking and PSO precaching.** Skald classes need to participate in cook-time class graph traversal. Likely free since they ARE UClasses via shims, but needs validation with `-cookflavor` runs in Phase 8.

6. **Module circular deps.** UBT permits limited cycles; Skald's compiler is currently single-pass per module. May need a two-phase resolution for cross-module type references in v0.4.

7. **Cranelift Windows-ARM64 maturity.** Currently shaky. If it blocks dev-loop on ARM64 devkits, fallback is to start the LLVM migration earlier than planned (§17).

8. **Default-flag evolution.** As UE5 evolves (5.5, 5.6, 6.x), some defaults may need to change. v0.3's defaults are tied to UE5.5. The `skald-modifiers` crate's default table should be versioned per UE release; the UBT plugin detects the UE version and selects the appropriate defaults.

9. **Discoverability of `private` field warnings.** v0.3 emits a warning when a `var` (no `pub`) in a `pub class` looks like it should be reflected. Tuning the heuristic (when to warn vs. stay silent) needs real-world usage data from Phase 8.

---

## 19. Constraints (Non-negotiable)

These are the hard constraints that every design decision in this spec respects. Any future modification to the spec must not violate them without explicit re-justification.

1. **No transpiling to C++.** Skald compiles directly to native code via Cranelift (v0.3) or LLVM (future). The only C++ emitted is reflection shims (with default UE flags auto-applied) for UHT consumption.

2. **No UE5 source modifications.** Ships as a plugin (`.uplugin`) + `.UbtPlugin.csproj`. Zero edits to UBT, UHT, or engine source.

3. **No VM.** No bytecode interpreter, no JIT runtime (the "hot-reload patch" in §14 is *not* a JIT — it's the same Cranelift backend invoked incrementally).

4. **No separate GC unless justified.** v0.3 uses the hybrid model (§9.1): UE GC for reflected types, Skald arena for non-reflected. Full FrankenGC (Option C) is rejected unless Phase 6 profiling shows millions of small immutable values, which is not the expected gameplay use case.

5. **No silent ABI risk.** Wherever the spec depends on a fragile ABI assumption, a `static_assert` (or runtime check) guards it. Specifically: layout offsets (§10.7), FFI signatures (§11.1), symbol names (§10.4).

6. **The C++ compiler always owns vtables.** Skald never emits a C++ vtable. Skald supplies function *bodies* via `extern "C"` thunks; the C++ compiler builds vtables from shim declarations. This eliminates the entire class of MSVC ABI vtable-emission bugs.

7. **`ref<UObject>` is `!Send`.** Worker threads cannot directly access UObjects. The snapshot/command pattern (§13.2) is the only supported way to do work on UObjects from worker threads.

8. **Hot-reload requires a quiescent-state handshake.** Patching `UFunction::Func` pointers in place is unsafe without waiting for in-flight `ProcessEvent` calls to drain. `FGCScopeGuard` (§14.4) is the only safe mechanism.

9. **Backend isolation.** `skald-codegen-cranelift` and (future) `skald-codegen-llvm` implement the same `Backend` trait. Switching is a cargo feature flag, not a code fork. The TIR is backend-agnostic.

10. **Short keywords.** `fn`, `var`, `pub`, `mut`, `let`, `match`, `impl`, `trait`, `mod`, `use`. No `function`, `variable`, `public`, `mutable`. Reclaim horizontal space; UE's identifier bloat is a real ergonomic problem.

11. **No nulls by default.** `T?` is the only nullable form. `ref<T>` is non-null. Eliminates the majority of UE null-deref crashes.

12. **Generics cannot be reflected.** UHT has no template support. Generic classes reflect only their concrete instantiations that are explicitly aliased via `type`.

13. **Default-on reflection.** `pub` gates reflection; sensible UE defaults are auto-applied. The common case requires zero reflection annotations. Modifiers override defaults; they are never required for the common case.

14. **`@`-annotations are Skald-native only.** `@region`, `@simd`, `@unsafe`, `@layout`, `@inline`, `@hot`/`@cold`, `@borrow`, `@persistent` — these affect codegen, not reflection. UE reflection flags are ALWAYS controlled via modifiers, never via `@`-annotations.

15. **No silent reflection failures.** If a `var` looks like it should be reflected (in a `pub class`, has a default value) but isn't `pub`, emit a warning. If a modifier is typo'd, emit an error. If a default flag conflicts with an explicit modifier, emit a warning showing the resolved flag set.

---

## 20. Appendix A: Roblox Lua Comparison

This appendix shows side-by-side comparisons of Roblox Lua and Skald for common gameplay patterns, to validate the "feels like Roblox Lua" goal.

### A.1 Defining a simple class

**Roblox Lua:**
```lua
local Greeter = {}
Greeter.__index = Greeter

function Greeter.new(greeting)
    local self = setmetatable({}, Greeter)
    self.greeting = greeting or "Hello"
    return self
end

function Greeter:shout()
    print(self.greeting:upper() .. "!!!")
end

return Greeter
```

**Skald:**
```skald
pub class Greeter {
    pub var greeting: str = "Hello"

    pub fn shout() {
        log::info(f"{self.greeting.to_upper()}!!!")
    }
}
```

The Skald version is shorter, type-safe, has editor visibility (the `greeting` field appears in the Details panel), and is Blueprint-callable. The Roblox version is untyped and has no editor integration.

### A.2 A class with initialization

**Roblox Lua:**
```lua
local Sentry = {}
Sentry.__index = Sentry

function Sentry.new(sightRadius)
    local self = setmetatable({}, Sentry)
    self.sightRadius = sightRadius or 500
    self.target = nil
    return self
end

function Sentry:alert(intensity)
    if self.target then
        -- ...
    end
end
```

**Skald:**
```skald
pub class Sentry {
    pub var sight_radius: f32 = 500.0
    pub var target: weak<AActor>?

    pub fn alert(intensity: f32) {
        if let Some(t) = self.target.pin() {
            // ...
        }
    }
}
```

Note: `target` is nullable (`?`), and `pin()` returns `ref<AActor>?` (also nullable), which is unwrapped via `if let Some(t)`. This is the Rust-style null handling that eliminates 90% of UE crashes while keeping the code readable.

### A.3 Inheritance

**Roblox Lua:**
```lua
local PatrolSentry = setmetatable({}, {__index = Sentry})
PatrolSentry.__index = PatrolSentry

function PatrolSentry.new(...)
    local self = Sentry.new(...)
    setmetatable(self, PatrolSentry)
    return self
end
```

**Skald:**
```skald
pub class PatrolSentry : Sentry {
    // inherits sight_radius and target
    // can override alert():
    pub fn alert(intensity: f32) override {
        super.alert(intensity)
        // additional patrol behavior
    }
}
```

### A.4 Events / Delegates

**Roblox Lua:**
```lua
local Sentry = {}
Sentry.OnDeath = Instance.new("BindableEvent")

function Sentry:die()
    Sentry.OnDeath:Fire(self)
end
```

**Skald:**
```skald
pub class Sentry {
    pub var on_death: delegate, bp_assignable

    fn die() {
        self.on_death.broadcast(self)
    }
}
```

The Skald `delegate` type maps to `TMulticastDelegate`, Blueprint-assignable by default. The Roblox version is untyped and Blueprint-invisible.

### A.5 What Skald adds over Roblox Lua

- **Type safety.** No `attempt to index nil (a nil value)` runtime errors. Type errors caught at compile time.
- **Editor integration.** Fields show up in Details panel; methods are Blueprint-callable.
- **Performance.** Native code, no interpreter overhead.
- **UE integration.** Replication, GC, cooking, networking — all work because Skald classes ARE UClasses.
- **Tooling.** LSP, debugger, autocomplete, go-to-definition, refactoring.

### A.6 What Roblox Lua has that Skald doesn't (and shouldn't)

- **Sandboxing.** Roblox Lua scripts run untrusted user code; Skald does not sandbox.
- **Hot string eval.** Roblox lets you `loadstring()` arbitrary code at runtime; Skald does not (compile-time only).
- **Dynamic typing escape hatches.** Roblox has no type system to escape; Skald has `@unsafe` for low-level control, but no "any" type by default.

---

## 21. Appendix B: v0.2 → v0.3 Migration

This appendix summarizes what changed between v0.2 and v0.3 for developers familiar with v0.2.

### B.1 Annotation removal

**v0.2 (removed):**
- `@uclass(...)` — replaced by visibility + modifiers.
- `@ustruct` — still exists (structs are not reflected by default in v0.3).
- `@uenum` — removed; `pub enum` is reflected by default.
- `@uinterface` — removed; `pub trait` is reflected by default.
- `@ufunc(...)` — replaced by visibility + modifiers.
- `@uprop(...)` — replaced by visibility + modifiers.

**v0.3 (retained):**
- `@ustruct` — retained because structs are not reflected by default.
- All Skald-native annotations: `@region`, `@arena`, `@simd`, `@layout`, `@unsafe`, `@inline`, `@hot`, `@cold`, `@borrow`, `@persistent`.

### B.2 Migration examples

**v0.2:**
```skald
@uclass(blueprintable, abstract)
pub class Sentry : AActor {
    @uprop(editanywhere, category="AI|Sight", replicated, clamp(0, 5000))
    var sight_radius: f32 = 500.0

    @uprop(replicated)
    var target: weak<AActor>

    @ufunc(blueprintcallable, category="AI", reliable)
    fn alert(intensity: f32) -> bool { ... }
}
```

**v0.3:**
```skald
pub class Sentry : AActor, abstract {
    pub var sight_radius: f32 = 500.0, replicated, clamp(0, 5000), category="AI|Sight"
    pub var target: weak<AActor>, replicated
    pub fn alert(intensity: f32) -> bool, reliable, category="AI" { ... }
}
```

Changes:
- `@uclass(blueprintable, abstract)` → `pub class ... , abstract` (Blueprintable is default for `pub class`).
- `@uprop(editanywhere, ...)` → `pub var ..., ...` (EditAnywhere + BlueprintReadWrite is default for `pub var`).
- `@ufunc(blueprintcallable, ...)` → `pub fn ..., ...` (BlueprintCallable is default for `pub fn`).
- `var` (no `pub`) → `pub var` (to keep reflection).

### B.3 Behavioral changes

- **Forgetting `@uprop` in v0.2** silently made the field invisible. **Forgetting `pub` in v0.3** produces a warning suggesting you make it `pub`. The default behavior is now *visible* rather than *invisible*.
- **`@uenum` / `@uinterface` are gone** — `pub enum` and `pub trait` are reflected by default. No migration needed beyond removing the annotation.
- **`@ustruct` is retained** — structs are still opt-in for reflection. This asymmetry matches UE5's convention where `USTRUCT` is opt-in but `UCLASS` is the default for gameplay classes.

### B.4 What didn't change

All v0.2 architectural decisions are retained in v0.3:
- Cranelift-only backend (no LLVM, no JIT).
- Hybrid memory model (UE GC for reflected, Skald arena for non-reflected).
- Path A reflection integration (synthetic C++ shims + UHT).
- FFI model split (`pub fn` thunks via `FNativeFuncPtr`, `override` via C++ method thunks).
- Hot-reload via quiescent-state handshake (`FGCScopeGuard`).
- Worker-thread UObject access via snapshot/command pattern.
- Arena/UObject reference lifecycle with closure promotion.
- `@layout(soa)` restricted to non-reflected fields.
- Rust workspace with 18+ crates.
- 22-month timeline to v1.

---

*End of Skald Language Specification v0.3.*
