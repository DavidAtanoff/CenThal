//! # skald-modifiers
//!
//! Spec §6, §7 — modifier validation and effective UE flag computation.
//!
//! Given a `pub var`/`pub fn`/`pub class` declaration with explicit modifiers,
//! this crate computes the *effective* UE flag set:
//!   `effective = defaults[member_kind] ∪ explicit_modifiers ∪ doc_comment_overrides`
//!
//! Some modifiers *replace* defaults rather than add to them:
//! - `readonly` → `VisibleAnywhere + BlueprintReadOnly` (replaces `EditAnywhere`/`BlueprintReadWrite`)
//! - `pure` → `BlueprintPure` (replaces `BlueprintCallable`)
//! - `editdefaults_only` → `EditDefaultsOnly` (replaces `EditAnywhere`)
//! - `visibleanywhere` → `VisibleAnywhere` (replaces `EditAnywhere`, keeps `BlueprintReadWrite`)

use rustc_hash::FxHashMap;
use serde::{Deserialize, Serialize};
use skald_ast::{FieldDecl, MethodDecl, Modifier, ClassDecl, StructDecl, EnumDecl, FreeFnDecl, Visibility};

// ---------- Effective flag sets ----------

bitflags::bitflags! {
    /// Effective UPROPERTY flags. Spec §7.2.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct UPropFlags: u64 {
        const EDIT_ANYWHERE         = 1 << 0;
        const EDIT_DEFAULTS_ONLY    = 1 << 1;
        const EDIT_INSTANCE_ONLY    = 1 << 2;
        const VISIBLE_ANYWHERE      = 1 << 3;
        const VISIBLE_DEFAULTS_ONLY = 1 << 4;
        const VISIBLE_INSTANCE_ONLY = 1 << 5;
        const BLUEPRINT_READWRITE   = 1 << 6;
        const BLUEPRINT_READ_ONLY   = 1 << 7;
        const NOT_BLUEPRINT_ASSIGNABLE = 1 << 8;
        const REPLICATED            = 1 << 9;
        const NOT_REPLICATED        = 1 << 10;
        const TRANSIENT             = 1 << 11;
        const DUPLICATE_TRANSIENT   = 1 << 12;
        const NON_TRANSACTIONAL     = 1 << 13;
        const NO_CLEAR              = 1 << 14;
        const CONFIG                = 1 << 15;
        const GLOBAL_CONFIG         = 1 << 16;
    }
}

bitflags::bitflags! {
    /// Effective UFUNCTION flags. Spec §7.3.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct UFuncFlags: u64 {
        const BLUEPRINT_CALLABLE     = 1 << 0;
        const BLUEPRINT_PURE         = 1 << 1;
        const RELIABLE               = 1 << 2;
        const UNRELIABLE             = 1 << 3;
        const WITH_VALIDATION        = 1 << 4;
        const CUSTOM_THUNK           = 1 << 5;
        const BLUEPRINT_INTERNAL_USE_ONLY = 1 << 6;
        const BLUEPRINT_AUTHORITY_ONLY = 1 << 7;
        const BLUEPRINT_COSMETIC     = 1 << 8;
        const NOT_CALLABLE           = 1 << 9;
    }
}

bitflags::bitflags! {
    /// Effective UCLASS flags. Spec §7.1.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct UClassFlags: u64 {
        const ABSTRACT              = 1 << 0;
        const DEFAULT_CONFIG        = 1 << 1;
        const GLOBAL_CONFIG         = 1 << 2;
        const NOT_BLUEPRINTABLE     = 1 << 3;
        const BLUEPRINT_TYPE        = 1 << 4;
        const NOT_BLUEPRINT_TYPE    = 1 << 5;
        const EDIT_INLINE_NEW       = 1 << 6;
        const NOT_EDIT_INLINE_NEW   = 1 << 7;
        const PLACEABLE             = 1 << 8;
        const NOT_PLACEABLE         = 1 << 9;
        const TRANSIENT             = 1 << 10;
        const NON_TRANSIENT         = 1 << 11;
        const MINIMAL_API           = 1 << 12;
        const CONST                 = 1 << 13;
        const CONVERSION_ROOT       = 1 << 14;
        const CUSTOM_CONSTRUCTOR    = 1 << 15;
        const DEPRECATED            = 1 << 16;
        const HIDE_DROPDOWN         = 1 << 17;
        const SPAWNABLE             = 1 << 18;
        const DEFAULT_TO_INSTANCED  = 1 << 19;
        const COLLAPSE_CATEGORIES   = 1 << 20;
        const DONT_COLLAPSE_CATEGORIES = 1 << 21;
    }
}

// ---------- Meta (string-keyed) ----------

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Meta {
    pub category: Option<String>,
    pub tooltip: Option<String>,
    pub display_name: Option<String>,
    pub config: Option<String>,
    pub within: Option<String>,
    pub asset_bundle: Option<String>,
    pub replicated_using: Option<String>,
    pub hide_functions: Option<String>,
    pub show_functions: Option<String>,
    pub return_display_name: Option<String>,
    pub auto_create_ref_term: Option<String>,
    pub clamp_min: Option<String>,
    pub clamp_max: Option<String>,
    pub ui_min: Option<String>,
    pub ui_max: Option<String>,
    pub advanced_view: Option<u32>,
    pub advanced_display: Option<u32>,
    pub array_index: Option<u32>,
    pub extra: Vec<(String, String)>,  // raw meta=(...) escape
}

// ---------- Effective sets ----------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EffectiveField {
    pub flags: UPropFlags,
    pub meta: Meta,
    pub readonly: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EffectiveFunc {
    pub flags: UFuncFlags,
    pub meta: Meta,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EffectiveClass {
    pub flags: UClassFlags,
    pub meta: Meta,
}

// ---------- Default computation (spec §6.2) ----------

pub fn default_field_flags(vis: Visibility, class_name: &str) -> EffectiveField {
    let mut flags = UPropFlags::empty();
    let mut meta = Meta::default();
    if vis.is_reflected() {
        flags |= UPropFlags::EDIT_ANYWHERE | UPropFlags::BLUEPRINT_READWRITE;
        meta.category = Some(class_name.to_string());
    }
    EffectiveField { flags, meta, readonly: false }
}

pub fn default_func_flags(vis: Visibility, class_name: &str) -> EffectiveFunc {
    let mut flags = UFuncFlags::empty();
    let mut meta = Meta::default();
    if vis.is_reflected() {
        flags |= UFuncFlags::BLUEPRINT_CALLABLE;
        meta.category = Some(class_name.to_string());
    }
    EffectiveFunc { flags, meta }
}

pub fn default_class_flags() -> EffectiveClass {
    EffectiveClass {
        flags: UClassFlags::BLUEPRINT_TYPE | UClassFlags::NOT_EDIT_INLINE_NEW,
        meta: Meta { config: Some("Engine".to_string()), ..Default::default() },
    }
}

// ---------- Apply modifiers ----------

pub fn apply_field_modifier(ef: &mut EffectiveField, m: &Modifier) -> Result<(), ModifierError> {
    match m {
        // Dispatch keywords (§7.4) — not UProperty flags, ignore here.
        Modifier::Override | Modifier::Virtual | Modifier::Final | Modifier::Static => {
            // No-op — these are method dispatch keywords, not field flags.
        }
        // Edit modes (replacements)
        Modifier::EditAnywhere => {
            ef.flags.remove(UPropFlags::EDIT_DEFAULTS_ONLY | UPropFlags::EDIT_INSTANCE_ONLY | UPropFlags::VISIBLE_ANYWHERE | UPropFlags::VISIBLE_DEFAULTS_ONLY | UPropFlags::VISIBLE_INSTANCE_ONLY);
            ef.flags.insert(UPropFlags::EDIT_ANYWHERE);
        }
        Modifier::EditDefaultsOnly => {
            ef.flags.remove(UPropFlags::EDIT_ANYWHERE | UPropFlags::EDIT_INSTANCE_ONLY);
            ef.flags.insert(UPropFlags::EDIT_DEFAULTS_ONLY);
        }
        Modifier::EditInstanceOnly => {
            ef.flags.remove(UPropFlags::EDIT_ANYWHERE | UPropFlags::EDIT_DEFAULTS_ONLY);
            ef.flags.insert(UPropFlags::EDIT_INSTANCE_ONLY);
        }
        Modifier::NotEditable => {
            ef.flags.remove(UPropFlags::EDIT_ANYWHERE | UPropFlags::EDIT_DEFAULTS_ONLY | UPropFlags::EDIT_INSTANCE_ONLY);
        }
        Modifier::VisibleAnywhere => {
            ef.flags.remove(UPropFlags::EDIT_ANYWHERE | UPropFlags::EDIT_DEFAULTS_ONLY | UPropFlags::EDIT_INSTANCE_ONLY | UPropFlags::VISIBLE_DEFAULTS_ONLY | UPropFlags::VISIBLE_INSTANCE_ONLY);
            ef.flags.insert(UPropFlags::VISIBLE_ANYWHERE);
        }
        Modifier::VisibleDefaultsOnly => {
            ef.flags.remove(UPropFlags::EDIT_ANYWHERE);
            ef.flags.insert(UPropFlags::VISIBLE_DEFAULTS_ONLY);
        }
        Modifier::VisibleInstanceOnly => {
            ef.flags.remove(UPropFlags::EDIT_ANYWHERE);
            ef.flags.insert(UPropFlags::VISIBLE_INSTANCE_ONLY);
        }
        // Blueprint access
        Modifier::BlueprintReadWrite => {
            ef.flags.remove(UPropFlags::BLUEPRINT_READ_ONLY | UPropFlags::NOT_BLUEPRINT_ASSIGNABLE);
            ef.flags.insert(UPropFlags::BLUEPRINT_READWRITE);
        }
        Modifier::BlueprintReadOnly => {
            ef.flags.remove(UPropFlags::BLUEPRINT_READWRITE);
            ef.flags.insert(UPropFlags::BLUEPRINT_READ_ONLY);
            ef.readonly = true;
        }
        Modifier::NotBlueprintAssignable => { ef.flags.insert(UPropFlags::NOT_BLUEPRINT_ASSIGNABLE); }
        Modifier::Replicated => { ef.flags.insert(UPropFlags::REPLICATED); }
        Modifier::NotReplicated => {
            ef.flags.remove(UPropFlags::REPLICATED);
            ef.flags.insert(UPropFlags::NOT_REPLICATED);
        }
        Modifier::ReplicatedUsing(s) => { ef.meta.replicated_using = Some(s.clone()); }
        Modifier::Transient => { ef.flags.insert(UPropFlags::TRANSIENT); }
        Modifier::DuplicateTransient => { ef.flags.insert(UPropFlags::DUPLICATE_TRANSIENT); }
        Modifier::NonTransactional => { ef.flags.insert(UPropFlags::NON_TRANSACTIONAL); }
        Modifier::NoClear => { ef.flags.insert(UPropFlags::NO_CLEAR); }
        Modifier::ConfigField => { ef.flags.insert(UPropFlags::CONFIG); }
        Modifier::GlobalConfigField => { ef.flags.insert(UPropFlags::GLOBAL_CONFIG); }
        Modifier::AssetBundle(s) => { ef.meta.asset_bundle = Some(s.clone()); }
        Modifier::Category(s) => { ef.meta.category = Some(s.clone()); }
        Modifier::Tooltip(s) => { ef.meta.tooltip = Some(s.clone()); }
        Modifier::DisplayName(s) => { ef.meta.display_name = Some(s.clone()); }
        Modifier::Clamp(a, b) => { ef.meta.clamp_min = Some(a.clone()); ef.meta.clamp_max = Some(b.clone()); }
        Modifier::Range(a, b) => { ef.meta.ui_min = Some(a.clone()); ef.meta.ui_max = Some(b.clone()); }
        Modifier::AdvancedView(n) => { ef.meta.advanced_view = Some(*n); }
        Modifier::ArrayIndex(n) => { ef.meta.array_index = Some(*n); }
        Modifier::Meta(s) => { ef.meta.extra.push(("meta".to_string(), s.clone())); }
        // Unknown / non-field modifier — caller should warn.
        Modifier::Unknown { name, .. } => {
            return Err(ModifierError::UnknownForField { name: name.clone() });
        }
        _ => {
            return Err(ModifierError::NotApplicable { mod_kind: format!("{:?}", m), target: "field" });
        }
    }
    Ok(())
}

pub fn apply_func_modifier(ef: &mut EffectiveFunc, m: &Modifier) -> Result<(), ModifierError> {
    match m {
        // Dispatch keywords (§7.4) — not UFunction flags, ignore here.
        Modifier::Override | Modifier::Virtual | Modifier::Final | Modifier::Static => Ok(()),
        Modifier::Callable | Modifier::BlueprintCallable => { ef.flags.insert(UFuncFlags::BLUEPRINT_CALLABLE); Ok(()) }
        Modifier::Pure => {
            ef.flags.remove(UFuncFlags::BLUEPRINT_CALLABLE);
            ef.flags.insert(UFuncFlags::BLUEPRINT_PURE);
            Ok(())
        }
        Modifier::NotCallable => {
            ef.flags.remove(UFuncFlags::BLUEPRINT_CALLABLE | UFuncFlags::BLUEPRINT_PURE);
            ef.flags.insert(UFuncFlags::NOT_CALLABLE);
            Ok(())
        }
        Modifier::Reliable => { ef.flags.insert(UFuncFlags::RELIABLE); Ok(()) }
        Modifier::Unreliable => { ef.flags.insert(UFuncFlags::UNRELIABLE); Ok(()) }
        Modifier::WithValidation => { ef.flags.insert(UFuncFlags::WITH_VALIDATION); Ok(()) }
        Modifier::CustomThunk => { ef.flags.insert(UFuncFlags::CUSTOM_THUNK); Ok(()) }
        Modifier::BlueprintInternal => { ef.flags.insert(UFuncFlags::BLUEPRINT_INTERNAL_USE_ONLY); Ok(()) }
        Modifier::BlueprintAuthorityOnly => { ef.flags.insert(UFuncFlags::BLUEPRINT_AUTHORITY_ONLY); Ok(()) }
        Modifier::BlueprintCosmetic => { ef.flags.insert(UFuncFlags::BLUEPRINT_COSMETIC); Ok(()) }
        Modifier::Category(s) => { ef.meta.category = Some(s.clone()); Ok(()) }
        Modifier::Tooltip(s) => { ef.meta.tooltip = Some(s.clone()); Ok(()) }
        Modifier::DisplayName(s) => { ef.meta.display_name = Some(s.clone()); Ok(()) }
        Modifier::ReturnDisplayName(s) => { ef.meta.return_display_name = Some(s.clone()); Ok(()) }
        Modifier::AutoCreateRefTerm(s) => { ef.meta.auto_create_ref_term = Some(s.clone()); Ok(()) }
        Modifier::AdvancedDisplay(n) => { ef.meta.advanced_display = Some(*n); Ok(()) }
        Modifier::Meta(s) => { ef.meta.extra.push(("meta".to_string(), s.clone())); Ok(()) }
        Modifier::Unknown { name, .. } => Err(ModifierError::UnknownForFunc { name: name.clone() }),
        _ => Err(ModifierError::NotApplicable { mod_kind: format!("{:?}", m), target: "function" }),
    }
}

pub fn apply_class_modifier(ef: &mut EffectiveClass, m: &Modifier) -> Result<(), ModifierError> {
    match m {
        Modifier::Abstract => { ef.flags.insert(UClassFlags::ABSTRACT); }
        Modifier::Config(s) => { ef.meta.config = Some(s.clone()); }
        Modifier::DefaultConfig => { ef.flags.insert(UClassFlags::DEFAULT_CONFIG); }
        Modifier::GlobalConfig => { ef.flags.insert(UClassFlags::GLOBAL_CONFIG); }
        Modifier::NotBlueprintable => { ef.flags.insert(UClassFlags::NOT_BLUEPRINTABLE); }
        Modifier::BlueprintType => { ef.flags.insert(UClassFlags::BLUEPRINT_TYPE); }
        Modifier::NotBlueprintType => { ef.flags.insert(UClassFlags::NOT_BLUEPRINT_TYPE); }
        Modifier::EditInlineNew => { ef.flags.insert(UClassFlags::EDIT_INLINE_NEW); }
        Modifier::NotEditInlineNew => { ef.flags.insert(UClassFlags::NOT_EDIT_INLINE_NEW); }
        Modifier::Placeable => { ef.flags.insert(UClassFlags::PLACEABLE); }
        Modifier::NotPlaceable => { ef.flags.insert(UClassFlags::NOT_PLACEABLE); }
        Modifier::Within(s) => { ef.meta.within = Some(s.clone()); }
        Modifier::Transient => { ef.flags.insert(UClassFlags::TRANSIENT); }
        Modifier::NonTransient => { ef.flags.insert(UClassFlags::NON_TRANSIENT); }
        Modifier::MinimalApi => { ef.flags.insert(UClassFlags::MINIMAL_API); }
        Modifier::Const => { ef.flags.insert(UClassFlags::CONST); }
        Modifier::ConversionRoot => { ef.flags.insert(UClassFlags::CONVERSION_ROOT); }
        Modifier::CustomConstructor => { ef.flags.insert(UClassFlags::CUSTOM_CONSTRUCTOR); }
        Modifier::Deprecated => { ef.flags.insert(UClassFlags::DEPRECATED); }
        Modifier::HideDropdown => { ef.flags.insert(UClassFlags::HIDE_DROPDOWN); }
        Modifier::HideFunctions(s) => { ef.meta.hide_functions = Some(s.clone()); }
        Modifier::ShowFunctions(s) => { ef.meta.show_functions = Some(s.clone()); }
        Modifier::Spawnable => { ef.flags.insert(UClassFlags::SPAWNABLE); }
        Modifier::DefaultToInstanced => { ef.flags.insert(UClassFlags::DEFAULT_TO_INSTANCED); }
        Modifier::CollapseCategories => { ef.flags.insert(UClassFlags::COLLAPSE_CATEGORIES); }
        Modifier::DontCollapseCategories => { ef.flags.insert(UClassFlags::DONT_COLLAPSE_CATEGORIES); }
        Modifier::Meta(s) => { ef.meta.extra.push(("meta".to_string(), s.clone())); }
        Modifier::Unknown { name, .. } => return Err(ModifierError::UnknownForClass { name: name.clone() }),
        _ => return Err(ModifierError::NotApplicable { mod_kind: format!("{:?}", m), target: "class" }),
    }
    Ok(())
}

// ---------- Errors ----------

#[derive(Debug, Clone, PartialEq)]
pub enum ModifierError {
    UnknownForField { name: String },
    UnknownForFunc { name: String },
    UnknownForClass { name: String },
    NotApplicable { mod_kind: String, target: &'static str },
}

// ---------- Top-level compute functions ----------

pub fn compute_field(field: &FieldDecl, class_name: &str) -> (EffectiveField, Vec<ModifierError>) {
    let mut ef = default_field_flags(field.vis, class_name);
    if field.readonly {
        ef.flags.remove(UPropFlags::EDIT_ANYWHERE | UPropFlags::EDIT_DEFAULTS_ONLY | UPropFlags::EDIT_INSTANCE_ONLY);
        ef.flags.remove(UPropFlags::BLUEPRINT_READWRITE);
        ef.flags.insert(UPropFlags::VISIBLE_ANYWHERE | UPropFlags::BLUEPRINT_READ_ONLY);
        ef.readonly = true;
    }
    let mut errs = vec![];
    for m in &field.modifiers {
        if let Err(e) = apply_field_modifier(&mut ef, m) { errs.push(e); }
    }
    (ef, errs)
}

pub fn compute_method(method: &MethodDecl, class_name: &str) -> (EffectiveFunc, Vec<ModifierError>) {
    let mut ef = default_func_flags(method.vis, class_name);
    let mut errs = vec![];
    for m in &method.modifiers {
        if let Err(e) = apply_func_modifier(&mut ef, m) { errs.push(e); }
    }
    (ef, errs)
}

pub fn compute_free_fn(f: &FreeFnDecl) -> (EffectiveFunc, Vec<ModifierError>) {
    let mut ef = default_func_flags(f.vis, "<module>");
    let mut errs = vec![];
    for m in &f.modifiers {
        if let Err(e) = apply_func_modifier(&mut ef, m) { errs.push(e); }
    }
    (ef, errs)
}

pub fn compute_class(c: &ClassDecl) -> (EffectiveClass, Vec<ModifierError>) {
    let mut ef = default_class_flags();
    let mut errs = vec![];
    for m in &c.modifiers {
        if let Err(e) = apply_class_modifier(&mut ef, m) { errs.push(e); }
    }
    (ef, errs)
}

pub fn compute_struct(s: &StructDecl) -> (EffectiveClass, Vec<ModifierError>) {
    // Structs have a smaller flag set (mostly BlueprintType). Reuse UClass for simplicity.
    let mut ef = EffectiveClass { flags: UClassFlags::BLUEPRINT_TYPE, meta: Meta::default() };
    let mut errs = vec![];
    for m in &s.modifiers {
        if let Err(e) = apply_class_modifier(&mut ef, m) { errs.push(e); }
    }
    (ef, errs)
}

pub fn compute_enum(e: &EnumDecl) -> (EffectiveClass, Vec<ModifierError>) {
    let mut ef = EffectiveClass { flags: UClassFlags::BLUEPRINT_TYPE, meta: Meta::default() };
    let mut errs = vec![];
    for m in &e.modifiers {
        if let Err(e) = apply_class_modifier(&mut ef, m) { errs.push(e); }
    }
    (ef, errs)
}

// ---------- Serialization (for the sidecar reflection JSON, spec §8.3) ----------

pub fn to_json(ef: &EffectiveField) -> serde_json::Value {
    serde_json::json!({
        "flags": format!("{:?}", ef.flags),
        "meta": ef.meta,
        "readonly": ef.readonly,
    })
}

pub fn func_to_json(ef: &EffectiveFunc) -> serde_json::Value {
    serde_json::json!({
        "flags": format!("{:?}", ef.flags),
        "meta": ef.meta,
    })
}

pub fn class_to_json(ef: &EffectiveClass) -> serde_json::Value {
    serde_json::json!({
        "flags": format!("{:?}", ef.flags),
        "meta": ef.meta,
    })
}

// ---------- Suggest (for LSP autocomplete — list valid modifiers per kind) ----------

pub fn valid_field_modifiers() -> Vec<&'static str> {
    vec![
        "editanywhere", "editdefaults_only", "editinstance_only", "not_editable",
        "visibleanywhere", "visible_defaults_only", "visible_instance_only",
        "blueprint_readwrite", "blueprint_read_only", "not_blueprint_assignable",
        "replicated", "not_replicated", "replicated_using=fn",
        "transient", "duplicate_transient", "non_transactional", "no_clear",
        "config", "global_config", "asset_bundle=\"X\"",
        "category=\"X\"", "tooltip=\"...\"", "display_name=\"...\"",
        "clamp(min, max)", "range(min, max)", "advanced_view=N", "array_index=N",
        "meta=\"...\"",
    ]
}

pub fn valid_func_modifiers() -> Vec<&'static str> {
    vec![
        "callable", "pure", "not_callable",
        "reliable", "unreliable", "with_validation", "custom_thunk",
        "blueprint_internal", "blueprint_callable", "blueprint_authority_only", "blueprint_cosmetic",
        "category=\"X\"", "tooltip=\"...\"", "display_name=\"...\"",
        "advanced_display=N", "return_display_name=\"...\"", "auto_create_ref_term=\"...\"",
        "meta=\"...\"",
    ]
}

pub fn valid_class_modifiers() -> Vec<&'static str> {
    vec![
        "abstract", "config=\"X\"", "default_config", "global_config",
        "not_blueprintable", "blueprint_type", "not_blueprint_type",
        "editinline_new", "not_editinline_new", "placeable", "not_placeable",
        "within=\"X\"", "transient", "non_transient", "minimal_api", "const",
        "conversion_root", "custom_constructor", "deprecated", "hide_dropdown",
        "hide_functions=\"...\"", "show_functions=\"...\"", "spawnable",
        "default_to_instanced", "collapse_categories", "dont_collapse_categories",
        "meta=\"...\"",
    ]
}

// ---------- Suggestion (Levenshtein) for typo correction ----------

pub fn suggest(input: &str, candidates: &[&'static str]) -> Option<String> {
    let mut best: Option<(usize, &str)> = None;
    for c in candidates {
        let d = levenshtein(input, c);
        if d <= 3 && (best.is_none() || d < best.unwrap().0) {
            best = Some((d, c));
        }
    }
    best.map(|(_, s)| s.to_string())
}

fn levenshtein(a: &str, b: &str) -> usize {
    let a: Vec<char> = a.chars().collect();
    let b: Vec<char> = b.chars().collect();
    let m = a.len();
    let n = b.len();
    if m == 0 { return n; }
    if n == 0 { return m; }
    let mut prev = (0..=n).collect::<Vec<_>>();
    let mut cur = vec![0; n + 1];
    for i in 1..=m {
        cur[0] = i;
        for j in 1..=n {
            let cost = if a[i-1] == b[j-1] { 0 } else { 1 };
            cur[j] = (prev[j] + 1).min(cur[j-1] + 1).min(prev[j-1] + cost);
        }
        std::mem::swap(&mut prev, &mut cur);
    }
    prev[n]
}

// Stub map for completeness.
#[allow(dead_code)]
fn _stub_map() -> FxHashMap<String, String> { FxHashMap::default() }
