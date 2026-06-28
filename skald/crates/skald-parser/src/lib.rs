//! # skald-parser
//!
//! Recursive-descent + Pratt parser for Skald (spec §3.2, §4).
//!
//! Produces `skald_ast::Module`. The parser is structured as:
//! - Top-level `Parser` struct with token cursor + error list.
//! - Item-level functions: `parse_module`, `parse_class`, `parse_fn`, etc.
//! - Statement-level functions: `parse_stmt`, `parse_let`, etc.
//! - Expression parsing: `parse_expr` (entry) → Pratt loop in `parse_expr_with_prec`.
//!
//! Error recovery strategy: on a parse error, the parser records the error,
//! advances to the next `;`, `}`, or top-level keyword, and continues. This
//! lets a single file produce multiple errors rather than aborting on the
//! first one.

#![allow(clippy::needless_borrow)]
#![allow(clippy::collapsible_if)]
#![allow(clippy::needless_range_loop)]

use rustc_hash::FxHashMap;
use skald_ast::*;
use skald_lexer::{Token, Tok};

// ---------- Errors ----------

#[derive(Debug, Clone, PartialEq)]
pub struct ParseError {
    pub span: Span,
    pub msg: String,
    /// Expected token kinds (for "expected one of X, Y" messages).
    pub expected: Vec<&'static str>,
}

impl ParseError {
    pub fn new(span: Span, msg: impl Into<String>) -> Self {
        ParseError { span, msg: msg.into(), expected: vec![] }
    }
    pub fn expected_one(span: Span, expected: Vec<&'static str>, got: &Token) -> Self {
        let got_s = token_name(got);
        ParseError {
            span,
            msg: format!("expected one of {:?}, got {}", expected, got_s),
            expected,
        }
    }
}

// ---------- Parser ----------

pub struct Parser {
    toks: Vec<Tok>,
    pos: usize,
    file: FileId,
    interner: Interner,
    pub errors: Vec<ParseError>,
    /// Doc comments pending attachment to next item.
    pending_doc: Vec<String>,
    pending_inner_doc: Vec<String>,
}

impl Parser {
    pub fn new(toks: Vec<Tok>, file: FileId, interner: Interner) -> Self {
        Self {
            toks,
            pos: 0,
            file,
            interner,
            errors: vec![],
            pending_doc: vec![],
            pending_inner_doc: vec![],
        }
    }

    // ----- Token cursor helpers -----

    fn cur(&self) -> &Token {
        self.toks.get(self.pos).map(|t| &t.kind).unwrap_or(&Token::Eof)
    }
    fn cur_tok(&self) -> &Tok {
        self.toks.get(self.pos).unwrap_or_else(|| self.toks.last().unwrap())
    }
    fn peek(&self, n: usize) -> &Token {
        self.toks.get(self.pos + n).map(|t| &t.kind).unwrap_or(&Token::Eof)
    }
    fn bump(&mut self) -> Tok {
        let t = self.toks.get(self.pos).cloned();
        if t.is_some() { self.pos += 1; }
        t.unwrap_or_else(|| Tok { kind: Token::Eof, span: Span::DUMMY })
    }
    fn at(&self, t: &Token) -> bool {
        std::mem::discriminant(self.cur()) == std::mem::discriminant(t)
    }
    fn eat(&mut self, t: &Token) -> bool {
        if self.at(t) { self.bump(); true } else { false }
    }
    fn expect(&mut self, t: Token, what: &str) -> bool {
        if self.at(&t) {
            self.bump();
            true
        } else {
            let span = self.cur_tok().span;
            self.errors.push(ParseError::new(span, format!("expected {}, got {}", what, token_name(self.cur()))));
            false
        }
    }
    fn skip_trivia(&mut self) {
        while matches!(self.cur(), Token::LineComment | Token::BlockComment) {
            self.bump();
        }
        // Doc comments are captured separately via the token stream — we
        // gather all consecutive `DocComment` and `InnerDocComment` tokens
        // into pending lists, then attach them to the next item.
        while let Token::DocComment(s) = self.cur() {
            let s = s.clone();
            self.bump();
            self.pending_doc.push(s);
        }
        while let Token::InnerDocComment(s) = self.cur() {
            let s = s.clone();
            self.bump();
            self.pending_inner_doc.push(s);
        }
    }

    /// Like `skip_trivia` but discards any doc comments instead of saving them
    /// (used inside function bodies where `///` doesn't attach to anything).
    fn skip_trivia_discard(&mut self) {
        while matches!(self.cur(),
            Token::LineComment | Token::BlockComment
            | Token::DocComment(_) | Token::InnerDocComment(_))
        {
            self.bump();
        }
    }

    fn intern(&mut self, s: &str) -> Ident { self.interner.intern(s) }

    fn name_from_token(&mut self) -> Option<Name> {
        match self.cur().clone() {
            Token::Ident(s) => {
                let span = self.cur_tok().span;
                self.bump();
                let id = self.intern(&s);
                Some(Name { ident: id, span })
            }
            Token::RawIdent(s) => {
                let span = self.cur_tok().span;
                self.bump();
                let id = self.intern(s.trim_start_matches("r#"));
                Some(Name { ident: id, span })
            }
            _ => None,
        }
    }

    // ----- Module entry -----

    pub fn parse_module(&mut self, name: String) -> Module {
        let mut items = vec![];
        let mut module_doc: Option<String> = None;
        loop {
            self.skip_trivia();
            if self.at(&Token::Eof) { break; }
            // Inner doc comments before any item become module-level docs.
            if !self.pending_inner_doc.is_empty() {
                let doc = std::mem::take(&mut self.pending_inner_doc).join("\n");
                module_doc = Some(doc);
                continue;
            }
            // Skip stray semicolons (between items).
            if self.at(&Token::Semicolon) { self.bump(); continue; }
            match self.parse_item() {
                Some(item) => items.push(item),
                None => {
                    // Error already recorded; advance to a safe token.
                    self.recover_to_item();
                }
            }
        }
        Module {
            file: self.file,
            name,
            items,
            doc: module_doc,
            interner: std::mem::take(&mut self.interner),
        }
    }

    fn recover_to_item(&mut self) {
        // Advance until we hit a top-level keyword or `}` or EOF.
        let mut last_pos = self.pos;
        loop {
            self.skip_trivia();
            match self.cur() {
                Token::Eof | Token::RBrace => return,
                Token::KwPub | Token::KwPrivate | Token::KwProtected
                | Token::KwUse | Token::KwMod | Token::KwFn | Token::KwClass
                | Token::KwStruct | Token::KwEnum | Token::KwTrait | Token::KwImpl
                | Token::KwConst | Token::KwStatic | Token::KwType | Token::KwAlias
                | Token::At => return,
                _ => { self.bump(); }
            }
            // Safety: force-advance if no progress.
            if self.pos == last_pos { self.bump(); }
            last_pos = self.pos;
        }
    }

    // ----- Attributes / Annotations -----

    /// Parse zero or more `@annotation` blocks. Returns the collected list.
    fn parse_attrs(&mut self) -> Vec<Annotation> {
        let mut attrs = vec![];
        loop {
            self.skip_trivia();
            if !self.at(&Token::At) { break; }
            self.bump(); // @
            // Parse `name` (possibly `name(args)`)
            let name = match self.cur().clone() {
                Token::Ident(s) => { self.bump(); s }
                _ => {
                    self.errors.push(ParseError::new(self.cur_tok().span, "expected annotation name after `@`"));
                    break;
                }
            };
            // Optional args: `(arg1, arg2, ...)` — args are raw tokens until matching `)`.
            let args: Vec<String> = if self.at(&Token::LParen) {
                self.bump();
                let mut args = vec![];
                let mut depth = 1;
                let mut cur = String::new();
                while depth > 0 {
                    match self.cur().clone() {
                        Token::LParen => { depth += 1; cur.push('('); self.bump(); }
                        Token::RParen => {
                            depth -= 1;
                            if depth == 0 {
                                self.bump();
                                if !cur.is_empty() { args.push(cur); }
                                break;
                            } else { cur.push(')'); self.bump(); }
                        }
                        Token::Comma if depth == 1 => {
                            args.push(std::mem::take(&mut cur));
                            self.bump();
                        }
                        Token::Eof => break,
                        other => {
                            cur.push_str(&token_to_src(&other));
                            self.bump();
                        }
                    }
                }
                args
            } else { vec![] };
            attrs.push(skald_lexer::parse_annotation(&name, &args));
        }
        attrs
    }

    // ----- Visibility + modifiers keyword prefix -----

    fn parse_vis(&mut self) -> Visibility {
        match self.cur() {
            Token::KwPub => { self.bump(); Visibility::Pub }
            Token::KwPrivate => { self.bump(); Visibility::Private }
            Token::KwProtected => { self.bump(); Visibility::Protected }
            _ => Visibility::Private,
        }
    }

    // ----- Top-level items -----

    fn parse_item(&mut self) -> Option<Item> {
        let attrs = self.parse_attrs();
        let doc = if !self.pending_doc.is_empty() {
            let s = std::mem::take(&mut self.pending_doc).join("\n");
            Some(s)
        } else { None };
        let vis = self.parse_vis();
        // `async fn` — `async` is a keyword. Consume it before `fn`.
        if self.at(&Token::KwAsync) { self.bump(); }

        match self.cur() {
            Token::KwUse => self.parse_use().map(Item::Use),
            Token::KwFn => self.parse_free_fn(attrs, vis, doc).map(Item::Fn),
            Token::KwClass => self.parse_class(attrs, vis, doc).map(Item::Class),
            Token::KwStruct => self.parse_struct(attrs, vis, doc).map(Item::Struct),
            Token::KwEnum => self.parse_enum(attrs, vis, doc).map(Item::Enum),
            Token::KwTrait => self.parse_trait(attrs, vis, doc).map(Item::Trait),
            Token::KwImpl => self.parse_impl().map(Item::Impl),
            Token::KwConst => self.parse_const(vis).map(|(n, t, e)| Item::Const { name: n, ty: t, init: e, vis }),
            Token::KwStatic => self.parse_static(attrs, vis).map(Item::Static),
            Token::KwType => self.parse_type_alias(vis).map(|(n, p, a)| Item::TypeAlias { name: n, params: p, alias: a, vis }),
            Token::KwAlias => {
                // `alias Name = Type;` — synonym for `type`
                self.bump();
                self.parse_type_alias(vis).map(|(n, p, a)| Item::TypeAlias { name: n, params: p, alias: a, vis })
            }
            Token::KwMod => self.parse_mod(vis),
            Token::Ident(s) if s == "extern" => self.parse_extern(vis),
            _ => {
                // Maybe an un-keyworded `extern fn ...`?
                self.errors.push(ParseError::new(
                    self.cur_tok().span,
                    format!("expected item, got {}", token_name(self.cur())),
                ));
                None
            }
        }
    }

    fn parse_use(&mut self) -> Option<UseTree> {
        self.expect(Token::KwUse, "`use`");
        let tree = self.parse_use_tree();
        self.expect(Token::Semicolon, "`;`");
        tree
    }

    fn parse_use_tree(&mut self) -> Option<UseTree> {
        let mut prefix = vec![];
        while let Some(n) = self.name_from_token_or_keyword() {
            prefix.push(n);
            if self.eat(&Token::DoubleColon) { continue; }
            break;
        }
        let kind = if self.eat(&Token::Star) {
            UseTreeKind::Glob
        } else if self.eat(&Token::LBrace) {
            let mut nested = vec![];
            loop {
                if self.eat(&Token::RBrace) { break; }
                if let Some(t) = self.parse_use_tree() { nested.push(t); }
                if !self.eat(&Token::Comma) {
                    self.expect(Token::RBrace, "`}`");
                    break;
                }
            }
            UseTreeKind::Nested(nested)
        } else if let Some(last) = prefix.pop() {
            UseTreeKind::Single(last)
        } else {
            self.errors.push(ParseError::new(self.cur_tok().span, "expected name in use tree"));
            return None;
        };
        Some(UseTree { prefix, kind })
    }

    fn parse_const(&mut self, vis: Visibility) -> Option<(Name, Type, Expr)> {
        let _ = vis;
        self.expect(Token::KwConst, "`const`");
        let name = self.name_from_token()?;
        self.expect(Token::Colon, "`:`");
        let ty = self.parse_type()?;
        self.expect(Token::Eq, "`=`");
        let init = self.parse_expr()?;
        // Semicolon is optional (newline-terminated).
        self.eat(&Token::Semicolon);
        Some((name, ty, init))
    }

    fn parse_static(&mut self, attrs: Vec<Annotation>, vis: Visibility) -> Option<GlobalDecl> {
        self.expect(Token::KwStatic, "`static`");
        // `mut` is a contextual keyword (not in the keyword table) — comes
        // through as Ident("mut").
        let mutable = self.cur_is_kw("mut") && { self.bump(); true };
        // Optional `var`/`let` keyword (Skald allows `static var x: T`).
        if self.at(&Token::KwVar) || self.at(&Token::KwLet) { self.bump(); }
        let name = self.name_from_token()?;
        self.expect(Token::Colon, "`:`");
        let ty = self.parse_type()?;
        let init = if self.eat(&Token::Eq) {
            Some(self.parse_expr()?)
        } else { None };
        // Semicolon is optional (newline-terminated).
        self.eat(&Token::Semicolon);
        Some(GlobalDecl { attrs, vis, name, ty, init, mutable })
    }

    fn parse_type_alias(&mut self, vis: Visibility) -> Option<(Name, Vec<TypeParam>, Type)> {
        let _ = vis;
        self.expect(Token::KwType, "`type`");
        let name = self.name_from_token()?;
        let params = self.parse_generic_params();
        self.expect(Token::Eq, "`=`");
        let alias = self.parse_type()?;
        // Semicolon is optional (newline-terminated).
        self.eat(&Token::Semicolon);
        Some((name, params, alias))
    }

    fn parse_mod(&mut self, vis: Visibility) -> Option<Item> {
        self.expect(Token::KwMod, "`mod`");
        let name = self.name_from_token()?;
        if self.eat(&Token::Semicolon) {
            return Some(Item::ModRef { name, vis });
        }
        self.expect(Token::LBrace, "`{`");
        let mut items = vec![];
        loop {
            self.skip_trivia();
            if self.eat(&Token::RBrace) { break; }
            if self.at(&Token::Eof) { break; }
            if let Some(item) = self.parse_item() { items.push(item); }
            else { self.recover_to_item(); }
        }
        Some(Item::Mod { name, items, vis })
    }

    fn parse_extern(&mut self, vis: Visibility) -> Option<Item> {
        // `extern "C" fn name(...)` or `extern { ... }`
        let _ = self.bump(); // extern (ident)
        let abi = if let Token::StrLit(s) = self.cur().clone() {
            self.bump();
            s
        } else { "C".to_string() };
        if self.at(&Token::LBrace) {
            // extern block — not common in Skald, parse items as `extern fn`s
            self.bump();
            // For now, just consume until `}`.
            // TODO: full extern block parsing.
            let mut depth = 1;
            while depth > 0 {
                match self.cur() {
                    Token::LBrace => { depth += 1; self.bump(); }
                    Token::RBrace => { depth -= 1; self.bump(); }
                    Token::Eof => break,
                    _ => { self.bump(); }
                }
            }
            return None;
        }
        self.expect(Token::KwFn, "`fn`");
        let name = self.name_from_token()?;
        self.expect(Token::LParen, "`(`");
        let params = self.parse_params()?;
        let ret = if self.eat(&Token::Arrow) {
            Some(self.parse_type()?)
        } else { None };
        self.expect(Token::Semicolon, "`;`");
        Some(Item::Extern(ExternDecl { vis, name, params, ret, abi }))
    }

    // ----- Generic params -----

    fn parse_generic_params(&mut self) -> Vec<TypeParam> {
        let mut out = vec![];
        if !self.eat(&Token::Lt) { return out; }
        loop {
            if self.eat(&Token::Gt) { return out; }
            if self.at(&Token::Ge) {
                // `>=` — but we are in generic args, so this is ambiguous.
                // Re-interpret as `>` followed by `=`. We don't split tokens
                // here; report an error.
                self.errors.push(ParseError::new(self.cur_tok().span,
                    "ambiguous `>=` in generic args; add space between `>` and `=`"));
                return out;
            }
            let name = match self.name_from_token() {
                Some(n) => n,
                None => { self.recover_to_gt(); return out; }
            };
            let mut bounds = vec![];
            if self.eat(&Token::Colon) {
                while !matches!(self.cur(), Token::Comma | Token::Gt | Token::Eq | Token::Eof) {
                    let p = self.parse_path();
                    if let Some(path) = p {
                        bounds.push(TraitBound { path });
                    }
                    if !self.eat(&Token::Plus) { break; }
                }
            }
            let default = if self.eat(&Token::Eq) {
                Some(self.parse_type().unwrap_or(Type::Infer))
            } else { None };
            out.push(TypeParam { name, bounds, default });
            if !self.eat(&Token::Comma) { break; }
        }
        let _ = self.eat(&Token::Gt);
        out
    }

    fn recover_to_gt(&mut self) {
        loop {
            match self.cur() {
                Token::Gt | Token::Comma | Token::Eof => return,
                _ => { self.bump(); }
            }
        }
    }

    // ----- Function -----

    fn parse_free_fn(&mut self, attrs: Vec<Annotation>, vis: Visibility, doc: Option<String>) -> Option<FreeFnDecl> {
        self.expect(Token::KwFn, "`fn`");
        let name = self.name_from_token()?;
        let generics = self.parse_generic_params();
        self.expect(Token::LParen, "`(`");
        let params = self.parse_params()?;
        let ret = if self.eat(&Token::Arrow) {
            Some(self.parse_type()?)
        } else { None };
        let modifiers = self.parse_modifiers();
        let dispatch = self.dispatch_from_modifiers(&modifiers);

        // Body: `=> expr` (arrow-body) or `{ ... }` (block) or `;` (decl-only).
        let (body, arrow_body) = if self.eat(&Token::FatArrow) {
            let e = self.parse_expr()?;
            // Semicolon is optional for arrow-body form (newline is enough).
            self.eat(&Token::Semicolon);
            (None, Some(Box::new(e)))
        } else if self.at(&Token::LBrace) {
            (Some(self.parse_block()?), None)
        } else {
            self.expect(Token::Semicolon, "`;`");
            (None, None)
        };

        Some(FreeFnDecl {
            attrs, vis, name, generics, params, ret, modifiers,
            dispatch, body, arrow_body, doc,
        })
    }

    fn dispatch_from_modifiers(&self, mods: &[Modifier]) -> MethodDispatch {
        for m in mods {
            match m {
                Modifier::Override => return MethodDispatch::Override,
                Modifier::Virtual => return MethodDispatch::Virtual,
                Modifier::Final => return MethodDispatch::Final,
                Modifier::Static => return MethodDispatch::Static,
                _ => {}
            }
        }
        MethodDispatch::Instance
    }

    // ----- Parameters -----

    fn parse_params(&mut self) -> Option<Vec<Param>> {
        let mut params = vec![];
        loop {
            if self.eat(&Token::RParen) { return Some(params); }
            // Each param: `@borrow? name: Type` or `name: Type = default`.
            let mut borrow = false;
            // `@borrow` could come via parse_attrs but params don't typically have multi-attr.
            if self.at(&Token::At) {
                let attrs = self.parse_attrs();
                if attrs.iter().any(|a| matches!(a, Annotation::Borrow)) {
                    borrow = true;
                }
            }
            // `mut name: T`? — `mut` is a contextual keyword, not reserved.
            if self.cur_is_kw("mut") { self.bump(); }
            let name = match self.name_from_token() {
                Some(n) => n,
                None => {
                    // `self` or `self: Type`?
                    if matches!(self.cur(), Token::KwSelf_) {
                        let span = self.cur_tok().span;
                        self.bump();
                        let id = self.intern("self");
                        Name { ident: id, span }
                    } else {
                        self.errors.push(ParseError::new(self.cur_tok().span, "expected parameter name"));
                        return Some(params);
                    }
                }
            };
            self.expect(Token::Colon, "`:`");
            let ty = match self.parse_type() {
                Some(t) => t,
                None => return Some(params),
            };
            let default = if self.eat(&Token::Eq) {
                Some(self.parse_expr()?)
            } else { None };
            params.push(Param { name, ty, borrow, default });
            if !self.eat(&Token::Comma) {
                self.expect(Token::RParen, "`)`");
                return Some(params);
            }
        }
    }

    // ----- Class / Struct / Enum / Trait -----

    fn parse_class(&mut self, attrs: Vec<Annotation>, vis: Visibility, doc: Option<String>) -> Option<ClassDecl> {
        self.expect(Token::KwClass, "`class`");
        let name = self.name_from_token()?;
        let generics = self.parse_generic_params();
        let parent = if self.eat(&Token::Colon) {
            Some(self.parse_type()?)
        } else { None };
        let mut traits = vec![];
        // `, Trait1, Trait2` after parent — but stop if we hit a modifier keyword
        // (e.g. `pub class X : A, abstract { }`).
        while self.eat(&Token::Comma) {
            // If the next token is a modifier keyword, stop and let parse_modifiers handle it.
            if matches!(self.cur(),
                Token::KwAbstract | Token::KwOverride | Token::KwVirtual
                | Token::KwFinal | Token::KwStatic | Token::KwReadonly)
            {
                break;
            }
            if let Some(t) = self.parse_type() { traits.push(t); }
            else { break; }
        }
        let modifiers = self.parse_modifiers();
        self.expect(Token::LBrace, "`{`");
        let members = self.parse_class_members();
        self.expect(Token::RBrace, "`}`");
        Some(ClassDecl {
            attrs, vis, name, generics, parent, traits, modifiers, members, doc,
        })
    }

    fn parse_struct(&mut self, attrs: Vec<Annotation>, vis: Visibility, doc: Option<String>) -> Option<StructDecl> {
        self.expect(Token::KwStruct, "`struct`");
        let name = self.name_from_token()?;
        let generics = self.parse_generic_params();
        // `: POD` parent marker.
        let parent = if self.eat(&Token::Colon) {
            Some(self.parse_type()?)
        } else { None };
        let modifiers = self.parse_modifiers();
        self.expect(Token::LBrace, "`{`");
        let (fields, methods) = self.parse_struct_members();
        self.expect(Token::RBrace, "`}`");
        Some(StructDecl {
            attrs, vis, name, generics, parent, modifiers, fields, methods, doc,
        })
    }

    fn parse_enum(&mut self, attrs: Vec<Annotation>, vis: Visibility, doc: Option<String>) -> Option<EnumDecl> {
        self.expect(Token::KwEnum, "`enum`");
        let name = self.name_from_token()?;
        let base = if self.eat(&Token::Colon) {
            if let Some(Type::Prim(p)) = self.parse_type() {
                Some(p)
            } else { None }
        } else { None };
        let modifiers = self.parse_modifiers();
        self.expect(Token::LBrace, "`{`");
        let mut variants = vec![];
        let mut last_pos = self.pos;
        loop {
            self.skip_trivia();
            if self.eat(&Token::RBrace) { break; }
            // Variants can be newline-separated OR comma-separated. If the
            // current token isn't an identifier, recover.
            let var_name = match self.name_from_token() {
                Some(n) => n,
                None => {
                    self.errors.push(ParseError::new(self.cur_tok().span,
                        format!("expected variant name, got {}", token_name(self.cur()))));
                    self.bump();
                    continue;
                }
            };
            let payload = if self.eat(&Token::LParen) {
                let mut ts = vec![];
                loop {
                    if self.eat(&Token::RParen) { break; }
                    if let Some(t) = self.parse_type() { ts.push(t); }
                    if !self.eat(&Token::Comma) { self.expect(Token::RParen, "`)`"); break; }
                }
                Some(ts)
            } else { None };
            let discriminant = if self.eat(&Token::Eq) {
                Some(self.parse_expr()?)
            } else { None };
            variants.push(EnumVariant { name: var_name, payload, discriminant });
            // Comma is OPTIONAL — newline alone is enough to separate variants.
            self.eat(&Token::Comma);
            // Safety: force-advance if no progress.
            if self.pos == last_pos { self.bump(); }
            last_pos = self.pos;
        }
        Some(EnumDecl { attrs, vis, name, base, variants, modifiers, doc })
    }

    fn parse_trait(&mut self, attrs: Vec<Annotation>, vis: Visibility, doc: Option<String>) -> Option<TraitDecl> {
        self.expect(Token::KwTrait, "`trait`");
        let name = self.name_from_token()?;
        let generics = self.parse_generic_params();
        let mut supertraits = vec![];
        if self.eat(&Token::Colon) {
            while !matches!(self.cur(), Token::LBrace | Token::Eof) {
                if let Some(p) = self.parse_path() {
                    supertraits.push(TraitBound { path: p });
                }
                if !self.eat(&Token::Plus) { break; }
            }
        }
        self.expect(Token::LBrace, "`{`");
        let mut methods = vec![];
        let mut last_pos = self.pos;
        loop {
            self.skip_trivia();
            if self.eat(&Token::RBrace) { break; }
            // Skip modifiers — traits use method sigs without modifiers.
            let _vis = self.parse_vis();
            // `fn`
            if !self.at(&Token::KwFn) {
                // Skip stray tokens
                if matches!(self.cur(), Token::Eof) { break; }
                self.bump();
                if self.pos == last_pos { self.bump(); }
                last_pos = self.pos;
                continue;
            }
            self.bump(); // fn
            let mname = match self.name_from_token() {
                Some(n) => n,
                None => {
                    if self.pos == last_pos { self.bump(); }
                    last_pos = self.pos;
                    continue;
                }
            };
            self.expect(Token::LParen, "`(`");
            let params = self.parse_params()?;
            let ret = if self.eat(&Token::Arrow) {
                Some(self.parse_type()?)
            } else { None };
            let default_body = if self.at(&Token::LBrace) {
                Some(self.parse_block()?)
            } else {
                self.expect(Token::Semicolon, "`;` or `{`");
                None
            };
            // Doc capture
            let mdoc = if !self.pending_doc.is_empty() {
                Some(std::mem::take(&mut self.pending_doc).join("\n"))
            } else { None };
            methods.push(TraitMethod { name: mname, params, ret, default_body, doc: mdoc });
            if self.pos == last_pos { self.bump(); }
            last_pos = self.pos;
        }
        Some(TraitDecl { attrs, vis, name, generics, supertraits, methods, doc })
    }

    fn parse_impl(&mut self) -> Option<ImplBlock> {
        self.expect(Token::KwImpl, "`impl`");
        let generics = self.parse_generic_params();
        // `impl Trait for Type { ... }` or `impl Type { ... }`
        let first_path = self.parse_path().unwrap_or_default();
        let (trait_path, target) = if self.at(&Token::KwFor) {
            self.bump();
            let target = self.parse_type()?;
            (Some(first_path), target)
        } else {
            // Reconstruct Type from path
            let target = path_to_type(first_path);
            (None, target)
        };
        self.expect(Token::LBrace, "`{`");
        let mut methods = vec![];
        let mut last_pos = self.pos;
        loop {
            self.skip_trivia();
            if self.eat(&Token::RBrace) { break; }
            let attrs = self.parse_attrs();
            let _vis = self.parse_vis();
            if !self.at(&Token::KwFn) {
                if matches!(self.cur(), Token::Eof) { break; }
                self.bump();
                if self.pos == last_pos { self.bump(); }
                last_pos = self.pos;
                continue;
            }
            self.bump();
            let name = match self.name_from_token() {
                Some(n) => n,
                None => {
                    if self.pos == last_pos { self.bump(); }
                    last_pos = self.pos;
                    continue;
                }
            };
            let _generics = self.parse_generic_params();
            self.expect(Token::LParen, "`(`");
            let params = self.parse_params()?;
            let ret = if self.eat(&Token::Arrow) {
                Some(self.parse_type()?)
            } else { None };
            let modifiers = self.parse_modifiers();
            let dispatch = self.dispatch_from_modifiers(&modifiers);
            let body = if self.at(&Token::LBrace) {
                Some(self.parse_block()?)
            } else {
                self.expect(Token::Semicolon, "`;` or `{`");
                None
            };
            methods.push(MethodDecl {
                attrs, vis: Visibility::Private, name, generics: vec![], params, ret,
                modifiers, dispatch, body,
            });
            if self.pos == last_pos { self.bump(); }
            last_pos = self.pos;
        }
        Some(ImplBlock { generics, trait_path, target, methods })
    }

    fn cur_is_kw(&self, s: &str) -> bool {
        matches!(self.cur(), Token::Ident(t) if t == s)
    }

    /// Eat a `>` token, splitting `>>` (Shr) or `>=` (Ge) if necessary.
    /// Used for closing generic argument lists in type contexts.
    fn eat_gt_splitting(&mut self) -> bool {
        if self.eat(&Token::Gt) {
            return true;
        }
        if matches!(self.cur(), Token::Shr) {
            let span = self.cur_tok().span;
            let half = Span::new(span.file, span.start, span.start + 1);
            let rest = Span::new(span.file, span.start + 1, span.end);
            self.toks[self.pos] = Tok { kind: Token::Gt, span: half };
            self.toks.insert(self.pos + 1, Tok { kind: Token::Gt, span: rest });
            self.bump();
            return true;
        }
        if matches!(self.cur(), Token::Ge) {
            let span = self.cur_tok().span;
            let half = Span::new(span.file, span.start, span.start + 1);
            let rest = Span::new(span.file, span.start + 1, span.end);
            self.toks[self.pos] = Tok { kind: Token::Gt, span: half };
            self.toks.insert(self.pos + 1, Tok { kind: Token::Eq, span: rest });
            self.bump();
            return true;
        }
        false
    }

    // ----- Members -----

    fn parse_class_members(&mut self) -> Vec<ClassMember> {
        let mut out = vec![];
        let mut last_pos = self.pos;
        loop {
            self.skip_trivia();
            if matches!(self.cur(), Token::RBrace | Token::Eof) { return out; }
            let attrs = self.parse_attrs();
            let doc = if !self.pending_doc.is_empty() {
                Some(std::mem::take(&mut self.pending_doc).join("\n"))
            } else { None };
            let vis = self.parse_vis();
            // `static var`, `static fn`? — `static` is a keyword (KwStatic).
            let is_static = self.at(&Token::KwStatic) && {
                self.bump();
                true
            };
            // `async fn` — `async` is a keyword. Consume it before `fn`.
            if self.at(&Token::KwAsync) { self.bump(); }

            if self.at(&Token::KwFn) {
                if let Some(mut m) = self.parse_method(attrs.clone(), vis, doc.clone()) {
                    m.dispatch = if is_static { MethodDispatch::Static } else { m.dispatch };
                    out.push(ClassMember::Method(m));
                }
            } else if self.at(&Token::KwVar) || self.at(&Token::KwLet) {
                if let Some(mut f) = self.parse_field(attrs.clone(), vis, doc.clone()) {
                    f.is_static = is_static;
                    out.push(ClassMember::Field(f));
                }
            } else {
                self.errors.push(ParseError::new(
                    self.cur_tok().span,
                    format!("expected class member (`fn`/`var`/`let`), got {}", token_name(self.cur())),
                ));
                self.bump();
            }
            // Safety: if no progress was made, force-advance to avoid infinite loop.
            if self.pos == last_pos {
                self.bump();
            }
            last_pos = self.pos;
        }
    }

    fn parse_struct_members(&mut self) -> (Vec<FieldDecl>, Vec<MethodDecl>) {
        let mut fields = vec![];
        let mut methods = vec![];
        loop {
            self.skip_trivia();
            if matches!(self.cur(), Token::RBrace | Token::Eof) { return (fields, methods); }
            let attrs = self.parse_attrs();
            let doc = if !self.pending_doc.is_empty() {
                Some(std::mem::take(&mut self.pending_doc).join("\n"))
            } else { None };
            let vis = self.parse_vis();
            if self.at(&Token::KwFn) {
                if let Some(m) = self.parse_method(attrs, vis, doc) {
                    methods.push(m);
                }
            } else if self.at(&Token::KwVar) || self.at(&Token::KwLet) {
                if let Some(f) = self.parse_field(attrs, vis, doc) {
                    fields.push(f);
                }
            } else {
                self.errors.push(ParseError::new(
                    self.cur_tok().span,
                    format!("expected `fn`/`var`, got {}", token_name(self.cur())),
                ));
                self.bump();
            }
        }
    }

    fn parse_field(&mut self, attrs: Vec<Annotation>, vis: Visibility, doc: Option<String>) -> Option<FieldDecl> {
        // `var` or `let`
        self.bump(); // var/let
        let name = self.name_from_token()?;
        self.expect(Token::Colon, "`:`");
        let ty = self.parse_type()?;
        let init = if self.eat(&Token::Eq) {
            Some(self.parse_expr()?)
        } else { None };
        let modifiers = self.parse_modifiers();
        let readonly = modifiers.iter().any(|m| matches!(m, Modifier::VisibleAnywhere))
            || modifiers.iter().any(|m| matches!(m, Modifier::BlueprintReadOnly));
        // Semicolon is OPTIONAL — Skald class members are newline-terminated.
        self.eat(&Token::Semicolon);
        let _ = doc;
        Some(FieldDecl {
            attrs, vis, name, ty, init, modifiers, readonly, is_static: false,
        })
    }

    fn parse_method(&mut self, attrs: Vec<Annotation>, vis: Visibility, doc: Option<String>) -> Option<MethodDecl> {
        self.bump(); // fn
        let name = self.name_from_token()?;
        let generics = self.parse_generic_params();
        self.expect(Token::LParen, "`(`");
        let params = self.parse_params()?;
        let ret = if self.eat(&Token::Arrow) {
            Some(self.parse_type()?)
        } else { None };
        let modifiers = self.parse_modifiers();
        let dispatch = self.dispatch_from_modifiers(&modifiers);
        let body = if self.at(&Token::LBrace) {
            Some(self.parse_block()?)
        } else if self.eat(&Token::FatArrow) {
            // Arrow body — wrap as block with tail expr.
            let e = self.parse_expr()?;
            self.expect(Token::Semicolon, "`;`");
            Some(Block::new(vec![], Some(e)))
        } else {
            self.expect(Token::Semicolon, "`;` or `{`");
            None
        };
        let _ = doc;
        Some(MethodDecl {
            attrs, vis, name, generics, params, ret, modifiers, dispatch, body,
        })
    }

    // ----- Modifiers (comma-separated, after decl) -----
    //
    // Spec §4.4: modifiers appear after the declaration's basic form.
    // The first modifier may be either:
    //   (a) a comma-prefixed value/flag modifier: `, replicated`, `, category="X"`
    //   (b) a bare modifier-keyword with no comma: `override`, `virtual`, `final`,
    //       `abstract`, `readonly`, `static`
    // Subsequent modifiers are always comma-separated.

    fn parse_modifiers(&mut self) -> Vec<Modifier> {
        let mut out = vec![];
        // Check for a bare keyword-modifier (no leading comma).
        if let Some(m) = self.try_parse_keyword_modifier() {
            out.push(m);
            // After the first keyword modifier, subsequent modifiers need commas.
            while self.eat(&Token::Comma) {
                if let Some(m) = self.try_parse_modifier_item() {
                    out.push(m);
                } else {
                    break;
                }
            }
            return out;
        }
        // Otherwise: comma-prefixed list.
        while self.eat(&Token::Comma) {
            if let Some(m) = self.try_parse_modifier_item() {
                out.push(m);
            } else {
                break;
            }
        }
        out
    }

    /// Try to parse a single modifier item — either a bare keyword or a
    /// name/value/args modifier. Returns None if the current token isn't a
    /// valid modifier start.
    fn try_parse_modifier_item(&mut self) -> Option<Modifier> {
        // `readonly` keyword
        if self.at(&Token::KwReadonly) {
            self.bump();
            return Some(Modifier::VisibleAnywhere);
        }
        // Keyword modifiers (§7.4)
        if self.at(&Token::KwOverride) { self.bump(); return Some(Modifier::Override); }
        if self.at(&Token::KwVirtual) { self.bump(); return Some(Modifier::Virtual); }
        if self.at(&Token::KwFinal) { self.bump(); return Some(Modifier::Final); }
        if self.at(&Token::KwAbstract) { self.bump(); return Some(Modifier::Abstract); }
        if self.at(&Token::KwStatic) { self.bump(); return Some(Modifier::Static); }

        // `name`, `name=value`, or `name(args)` form.
        let name = match self.cur().clone() {
            Token::Ident(s) => { self.bump(); s }
            Token::KwConst => { self.bump(); "const".to_string() }
            _ => return None,
        };

        if self.eat(&Token::Eq) {
            let val = match self.cur().clone() {
                Token::StrLit(s) => { self.bump(); ModifierArg::Str(s) }
                Token::IntLit { value, .. } => { self.bump(); ModifierArg::Int(value as i64) }
                Token::FloatLit { value, .. } => { self.bump(); ModifierArg::Float(value) }
                Token::Ident(s) => { self.bump(); ModifierArg::Ident(s) }
                _ => return None,
            };
            return Some(self.modifier_with_value(&name, val));
        }

        if self.at(&Token::LParen) {
            self.bump();
            let mut args = vec![];
            loop {
                if self.eat(&Token::RParen) { break; }
                let v = match self.cur().clone() {
                    Token::StrLit(s) => { self.bump(); ModifierArg::Str(s) }
                    Token::IntLit { value, .. } => { self.bump(); ModifierArg::Int(value as i64) }
                    Token::FloatLit { value, .. } => { self.bump(); ModifierArg::Float(value) }
                    Token::Ident(s) => { self.bump(); ModifierArg::Ident(s) }
                    other => {
                        self.errors.push(ParseError::new(self.cur_tok().span,
                            format!("expected modifier arg, got {}", token_name(&other))));
                        self.bump();
                        ModifierArg::Ident(String::new())
                    }
                };
                args.push(v);
                if !self.eat(&Token::Comma) {
                    self.expect(Token::RParen, "`)`");
                    break;
                }
            }
            return Some(self.modifier_with_args(&name, args));
        }

        Some(self.modifier_bare(&name))
    }

    /// Try to parse a leading bare keyword modifier (no comma prefix).
    fn try_parse_keyword_modifier(&mut self) -> Option<Modifier> {
        if self.at(&Token::KwReadonly) {
            self.bump();
            return Some(Modifier::VisibleAnywhere);
        }
        if self.at(&Token::KwOverride) { self.bump(); return Some(Modifier::Override); }
        if self.at(&Token::KwVirtual) { self.bump(); return Some(Modifier::Virtual); }
        if self.at(&Token::KwFinal) { self.bump(); return Some(Modifier::Final); }
        if self.at(&Token::KwAbstract) { self.bump(); return Some(Modifier::Abstract); }
        if self.at(&Token::KwStatic) { self.bump(); return Some(Modifier::Static); }
        None
    }

    fn modifier_bare(&mut self, name: &str) -> Modifier {
        match name {
            // Class
            "abstract" => Modifier::Abstract,
            "default_config" => Modifier::DefaultConfig,
            "global_config" => Modifier::GlobalConfig,
            "not_blueprintable" => Modifier::NotBlueprintable,
            "blueprint_type" => Modifier::BlueprintType,
            "not_blueprint_type" => Modifier::NotBlueprintType,
            "editinline_new" => Modifier::EditInlineNew,
            "not_editinline_new" => Modifier::NotEditInlineNew,
            "placeable" => Modifier::Placeable,
            "not_placeable" => Modifier::NotPlaceable,
            "transient" => Modifier::Transient,
            "non_transient" => Modifier::NonTransient,
            "minimal_api" => Modifier::MinimalApi,
            "const" => Modifier::Const,
            "conversion_root" => Modifier::ConversionRoot,
            "custom_constructor" => Modifier::CustomConstructor,
            "deprecated" => Modifier::Deprecated,
            "hide_dropdown" => Modifier::HideDropdown,
            "spawnable" => Modifier::Spawnable,
            "default_to_instanced" => Modifier::DefaultToInstanced,
            "collapse_categories" => Modifier::CollapseCategories,
            "dont_collapse_categories" => Modifier::DontCollapseCategories,
            // Field
            "editanywhere" => Modifier::EditAnywhere,
            "editdefaults_only" => Modifier::EditDefaultsOnly,
            "editinstance_only" => Modifier::EditInstanceOnly,
            "not_editable" => Modifier::NotEditable,
            "visibleanywhere" => Modifier::VisibleAnywhere,
            "visible_defaults_only" => Modifier::VisibleDefaultsOnly,
            "visible_instance_only" => Modifier::VisibleInstanceOnly,
            "blueprint_readwrite" => Modifier::BlueprintReadWrite,
            "blueprint_read_only" => Modifier::BlueprintReadOnly,
            "not_blueprint_assignable" => Modifier::NotBlueprintAssignable,
            "replicated" => Modifier::Replicated,
            "not_replicated" => Modifier::NotReplicated,
            "duplicate_transient" => Modifier::DuplicateTransient,
            "non_transactional" => Modifier::NonTransactional,
            "no_clear" => Modifier::NoClear,
            "config" => Modifier::ConfigField,
            "bp_assignable" => Modifier::BpAssignable,
            // Function
            "callable" => Modifier::Callable,
            "pure" => Modifier::Pure,
            "not_callable" => Modifier::NotCallable,
            "reliable" => Modifier::Reliable,
            "unreliable" => Modifier::Unreliable,
            "with_validation" => Modifier::WithValidation,
            "custom_thunk" => Modifier::CustomThunk,
            "blueprint_internal" => Modifier::BlueprintInternal,
            "blueprint_callable" => Modifier::BlueprintCallable,
            "blueprint_authority_only" => Modifier::BlueprintAuthorityOnly,
            "blueprint_cosmetic" => Modifier::BlueprintCosmetic,
            other => {
                self.errors.push(ParseError::new(self.cur_tok().span,
                    format!("unknown modifier `{}`", other)));
                Modifier::Unknown { name: other.to_string(), args: vec![] }
            }
        }
    }

    fn modifier_with_value(&mut self, name: &str, val: ModifierArg) -> Modifier {
        let s = match &val {
            ModifierArg::Str(s) | ModifierArg::Ident(s) => s.clone(),
            ModifierArg::Int(i) => i.to_string(),
            ModifierArg::Float(f) => f.to_string(),
        };
        match name {
            "config" => Modifier::Config(s),
            "within" => Modifier::Within(s),
            "hide_functions" => Modifier::HideFunctions(s),
            "show_functions" => Modifier::ShowFunctions(s),
            "category" => Modifier::Category(s),
            "tooltip" => Modifier::Tooltip(s),
            "display_name" => Modifier::DisplayName(s),
            "meta" => Modifier::Meta(s),
            "asset_bundle" => Modifier::AssetBundle(s),
            "replicated_using" => Modifier::ReplicatedUsing(s),
            "return_display_name" => Modifier::ReturnDisplayName(s),
            "auto_create_ref_term" => Modifier::AutoCreateRefTerm(s),
            other => Modifier::Unknown { name: other.to_string(), args: vec![val] },
        }
    }

    fn modifier_with_args(&mut self, name: &str, args: Vec<ModifierArg>) -> Modifier {
        let strs: Vec<String> = args.iter().map(|a| match a {
            ModifierArg::Str(s) | ModifierArg::Ident(s) => s.clone(),
            ModifierArg::Int(i) => i.to_string(),
            ModifierArg::Float(f) => f.to_string(),
        }).collect();
        match name {
            "clamp" if strs.len() == 2 => Modifier::Clamp(strs[0].clone(), strs[1].clone()),
            "range" if strs.len() == 2 => Modifier::Range(strs[0].clone(), strs[1].clone()),
            "advanced_view" if !strs.is_empty() => {
                let n = strs[0].parse().unwrap_or(0);
                Modifier::AdvancedView(n)
            }
            "advanced_display" if !strs.is_empty() => {
                let n = strs[0].parse().unwrap_or(0);
                Modifier::AdvancedDisplay(n)
            }
            "array_index" if !strs.is_empty() => {
                let n = strs[0].parse().unwrap_or(0);
                Modifier::ArrayIndex(n)
            }
            other => Modifier::Unknown { name: other.to_string(), args },
        }
    }

    // ----- Types -----

    fn parse_path(&mut self) -> Option<Vec<Name>> {
        let mut path = vec![];
        let first = self.name_from_token_or_keyword()?;
        path.push(first);
        while self.eat(&Token::DoubleColon) {
            if let Some(n) = self.name_from_token_or_keyword() { path.push(n); }
            else { break; }
        }
        Some(path)
    }

    /// Like `name_from_token`, but also accepts keywords as identifiers
    /// (so `ue::spawn`, `ue::async`, etc. work as path components).
    fn name_from_token_or_keyword(&mut self) -> Option<Name> {
        // First try a normal identifier.
        if let Some(n) = self.name_from_token() {
            return Some(n);
        }
        // Otherwise, if cur is a keyword, treat it as an identifier.
        let span = self.cur_tok().span;
        let s = match self.cur().clone() {
            Token::KwSpawn => "spawn".to_string(),
            Token::KwAsync => "async".to_string(),
            Token::KwAwait => "await".to_string(),
            Token::KwType => "type".to_string(),
            Token::KwConst => "const".to_string(),
            Token::KwStatic => "static".to_string(),
            Token::KwSelf_ => "self".to_string(),
            Token::KwSuper => "super".to_string(),
            Token::KwIn => "in".to_string(),
            Token::KwAs => "as".to_string(),
            Token::KwIs => "is".to_string(),
            Token::KwMatch => "match".to_string(),
            _ => return None,
        };
        self.bump();
        let id = self.intern(&s);
        Some(Name { ident: id, span })
    }

    fn parse_type(&mut self) -> Option<Type> {
        self.parse_type_prec(0)
    }

    fn parse_type_prec(&mut self, _prec: u8) -> Option<Type> {
        let mut base = self.parse_type_atom()?;
        // Suffixes: `?`, `!`, `mut`, generic args `<...>`.
        loop {
            match self.cur() {
                Token::Question => {
                    self.bump();
                    base = Type::Optional(Box::new(base));
                }
                Token::Bang => {
                    // `!` on types isn't standard. Skip.
                    self.bump();
                }
                Token::Lt => {
                    // Generic args. Only valid for path-based types.
                    let generics = self.parse_generic_args();
                    base = Type::App { base: Box::new(base), args: generics };
                }
                _ => break,
            }
        }
        Some(base)
    }

    fn parse_generic_args(&mut self) -> Vec<Type> {
        let mut out = vec![];
        if !self.eat(&Token::Lt) { return out; }
        loop {
            // Split `>>` into two `>` for nested generics.
            if matches!(self.cur(), Token::Shr) {
                let span = self.cur_tok().span;
                let half_span = Span::new(span.file, span.start, span.start + 1);
                let rest_span = Span::new(span.file, span.start + 1, span.end);
                self.toks[self.pos] = Tok { kind: Token::Gt, span: half_span };
                self.toks.insert(self.pos + 1, Tok { kind: Token::Gt, span: rest_span });
                self.bump();
                return out;
            }
            if self.eat(&Token::Gt) { return out; }
            if self.at(&Token::Ge) {
                let span = self.cur_tok().span;
                let half_span = Span::new(span.file, span.start, span.start + 1);
                let rest_span = Span::new(span.file, span.start + 1, span.end);
                self.toks[self.pos] = Tok { kind: Token::Gt, span: half_span };
                self.toks.insert(self.pos + 1, Tok { kind: Token::Eq, span: rest_span });
                self.bump();
                return out;
            }
            if let Some(t) = self.parse_type() {
                out.push(t);
            } else { break; }
            if !self.eat(&Token::Comma) {
                // Try `>` (Gt) or split `>>` (Shr) or `>=` (Ge).
                self.eat_gt_splitting();
                break;
            }
        }
        out
    }

    fn parse_type_atom(&mut self) -> Option<Type> {
        // Prefix `mut`, `ref`, `weak`, `soft`, `subclass`, `*`, `*mut`, `arr`/etc with `<...>`.
        if matches!(self.cur(), Token::Ident(s) if s == "mut") {
            self.bump();
            let inner = self.parse_type_atom()?;
            return Some(Type::Mut(Box::new(inner)));
        }
        if matches!(self.cur(), Token::Ident(s) if s == "ref") {
            self.bump();
            self.expect(Token::Lt, "`<`");
            let inner = self.parse_type()?;
            if !self.eat_gt_splitting() {
                self.errors.push(ParseError::new(self.cur_tok().span, "expected `>`"));
            }
            return Some(Type::Ref(Box::new(inner)));
        }
        if matches!(self.cur(), Token::Ident(s) if s == "weak") {
            self.bump();
            self.expect(Token::Lt, "`<`");
            let inner = self.parse_type()?;
            if !self.eat_gt_splitting() {
                self.errors.push(ParseError::new(self.cur_tok().span, "expected `>`"));
            }
            return Some(Type::Weak(Box::new(inner)));
        }
        if matches!(self.cur(), Token::Ident(s) if s == "soft") {
            self.bump();
            self.expect(Token::Lt, "`<`");
            let inner = self.parse_type()?;
            if !self.eat_gt_splitting() {
                self.errors.push(ParseError::new(self.cur_tok().span, "expected `>`"));
            }
            return Some(Type::Soft(Box::new(inner)));
        }
        if matches!(self.cur(), Token::Ident(s) if s == "subclass") {
            self.bump();
            self.expect(Token::Lt, "`<`");
            let inner = self.parse_type()?;
            if !self.eat_gt_splitting() {
                self.errors.push(ParseError::new(self.cur_tok().span, "expected `>`"));
            }
            return Some(Type::Subclass(Box::new(inner)));
        }
        if matches!(self.cur(), Token::Ident(s) if s == "opt") {
            self.bump();
            self.expect(Token::Lt, "`<`");
            let inner = self.parse_type()?;
            if !self.eat_gt_splitting() {
                self.errors.push(ParseError::new(self.cur_tok().span, "expected `>`"));
            }
            return Some(Type::Optional(Box::new(inner)));
        }
        if matches!(self.cur(), Token::Ident(s) if s == "arr" || s == "map" || s == "set") {
            // Generic containers — handle as Named + App.
            let name = self.name_from_token()?;
            let args = self.parse_generic_args();
            let base = Type::Named(name);
            if args.is_empty() { return Some(base); }
            return Some(Type::App { base: Box::new(base), args });
        }
        if matches!(self.cur(), Token::Ident(s) if s == "query") {
            self.bump();
            self.expect(Token::Lt, "`<`");
            let mut args = vec![];
            loop {
                if self.eat_gt_splitting() { break; }
                if let Some(t) = self.parse_type() { args.push(t); }
                if !self.eat(&Token::Comma) {
                    let _ = self.eat_gt_splitting();
                    break;
                }
            }
            return Some(Type::Query(args));
        }
        if matches!(self.cur(), Token::Ident(s) if s == "delegate") {
            self.bump();
            return Some(Type::Delegate);
        }
        if matches!(self.cur(), Token::Star) {
            self.bump();
            let mutable = self.cur_is_kw("mut") && { self.bump(); true };
            let inner = self.parse_type_atom()?;
            return Some(Type::Ptr { ty: Box::new(inner), mutable });
        }
        // Primitives
        if let Token::Ident(s) = self.cur().clone() {
            if let Some(p) = parse_prim(&s) {
                self.bump();
                return Some(Type::Prim(p));
            }
            if let Some(m) = parse_math(&s) {
                self.bump();
                return Some(Type::Math(m));
            }
        }
        // `()` unit
        if self.eat(&Token::LParen) {
            if self.eat(&Token::RParen) {
                return Some(Type::Unit);
            }
            let mut ts = vec![];
            loop {
                if let Some(t) = self.parse_type() { ts.push(t); }
                if !self.eat(&Token::Comma) { break; }
            }
            self.expect(Token::RParen, "`)`");
            return Some(Type::Tuple(ts));
        }
        // `Self`
        if self.at(&Token::KwSelfTy) {
            self.bump();
            return Some(Type::SelfTy);
        }
        // `[u8]` byte slice
        if self.eat(&Token::LBracket) {
            self.expect(Token::RBracket, "`]`");
            return Some(Type::ByteSlice);
        }
        // `fn(Args) -> Ret`
        if self.at(&Token::KwFn) {
            self.bump();
            self.expect(Token::LParen, "`(`");
            let mut params = vec![];
            loop {
                if self.eat(&Token::RParen) { break; }
                if let Some(t) = self.parse_type() { params.push(t); }
                if !self.eat(&Token::Comma) { self.expect(Token::RParen, "`)`"); break; }
            }
            let ret = if self.eat(&Token::Arrow) {
                self.parse_type()?
            } else { Type::Unit };
            return Some(Type::Fn { params, ret: Box::new(ret) });
        }
        // Named type — possibly path-qualified `Module::Type`.
        if let Some(path) = self.parse_path() {
            return Some(path_to_type(path));
        }
        // `_` infer
        if self.eat(&Token::Underscore) {
            return Some(Type::Infer);
        }
        self.errors.push(ParseError::new(self.cur_tok().span,
            format!("expected type, got {}", token_name(self.cur()))));
        None
    }

    // ----- Statements -----

    fn parse_block(&mut self) -> Option<Block> {
        self.expect(Token::LBrace, "`{`");
        let mut stmts = vec![];
        let mut tail = None;
        let mut last_pos = self.pos;
        loop {
            self.skip_trivia_discard();
            if self.eat(&Token::RBrace) { break; }
            if self.at(&Token::Eof) { break; }
            // Try statement. If it's actually a trailing expression, capture as tail.
            if self.is_expr_start() && self.could_be_tail_expr() {
                let e = self.parse_expr()?;
                if self.eat(&Token::Semicolon) {
                    stmts.push(Stmt::Expr(e));
                    continue;
                }
                if self.at(&Token::RBrace) {
                    tail = Some(e);
                    self.bump();
                    break;
                }
                // Else, treat as statement.
                stmts.push(Stmt::Expr(e));
                let _ = self.eat(&Token::Semicolon);
                continue;
            }
            if let Some(s) = self.parse_stmt() {
                stmts.push(s);
            } else {
                self.recover_to_stmt();
            }
            // Safety: if no progress was made, force-advance to avoid infinite loop.
            if self.pos == last_pos {
                self.bump();
            }
            last_pos = self.pos;
        }
        Some(Block { stmts, tail: tail.map(Box::new) })
    }

    fn could_be_tail_expr(&self) -> bool {
        // Conservative: any expression-starting token that isn't a statement keyword.
        !matches!(self.cur(),
            Token::KwLet | Token::KwVar | Token::KwUse | Token::KwConst
            | Token::KwStatic | Token::KwType | Token::KwAlias | Token::KwMod)
    }

    fn is_expr_start(&self) -> bool {
        matches!(self.cur(),
            Token::IntLit { .. } | Token::FloatLit { .. } | Token::StrLit(_)
            | Token::CharLit(_) | Token::FmtStrLit(_) | Token::ByteStrLit(_)
            | Token::KwTrue | Token::KwFalse
            | Token::KwSelf_ | Token::KwSelfTy | Token::KwSuper
            | Token::KwIf | Token::KwMatch | Token::KwFor | Token::KwWhile
            | Token::KwLoop | Token::KwReturn | Token::KwBreak | Token::KwCont
            | Token::KwSpawn | Token::KwAwait | Token::KwParallel
            | Token::KwAsync | Token::KwNull | Token::KwTransaction
            | Token::LParen | Token::LBracket | Token::LBrace
            | Token::Minus | Token::Bang | Token::Star | Token::Amp | Token::Pipe
            | Token::At | Token::Ident(_) | Token::RawIdent(_)
        )
    }

    fn recover_to_stmt(&mut self) {
        loop {
            match self.cur() {
                Token::Semicolon | Token::RBrace | Token::Eof => { let _ = self.eat(&Token::Semicolon); return; }
                _ => { self.bump(); }
            }
        }
    }

    fn parse_stmt(&mut self) -> Option<Stmt> {
        // Skip leading doc/comments.
        self.skip_trivia();
        match self.cur() {
            Token::KwLet | Token::KwVar => self.parse_let(),
            Token::KwUse => self.parse_use().map(Stmt::Use),
            Token::KwConst => {
                let vis = Visibility::Private;
                self.parse_const(vis).map(|(n, t, e)| Stmt::Const { name: n, ty: t, init: e })
            }
            Token::KwStatic => {
                let attrs = self.parse_attrs();
                let vis = Visibility::Private;
                let gd = self.parse_static(attrs, vis)?;
                Some(Stmt::Static { name: gd.name, ty: gd.ty, init: gd.init.unwrap_or(Expr::UnitLit), mutable: gd.mutable })
            }
            Token::KwType => self.parse_type_alias(Visibility::Private).map(|(n, p, a)| Stmt::TypeAlias { name: n, params: p, alias: a }),
            Token::Semicolon => { self.bump(); Some(Stmt::Empty) }
            _ => {
                // Expression statement
                let e = self.parse_expr()?;
                self.eat(&Token::Semicolon);
                Some(Stmt::Expr(e))
            }
        }
    }

    fn parse_let(&mut self) -> Option<Stmt> {
        let kw = self.bump().kind;
        let pat = self.parse_pat()?;
        let ty = if self.eat(&Token::Colon) {
            Some(self.parse_type()?)
        } else { None };
        let init = if self.eat(&Token::Eq) {
            Some(self.parse_expr()?)
        } else { None };
        self.eat(&Token::Semicolon);
        if matches!(kw, Token::KwVar) {
            Some(Stmt::VarLet { pat, ty, init })
        } else {
            Some(Stmt::Let { pat, ty, init })
        }
    }

    // ----- Patterns -----

    fn parse_pat(&mut self) -> Option<Pat> {
        self.parse_or_pat()
    }

    fn parse_or_pat(&mut self) -> Option<Pat> {
        let mut first = self.parse_pat_atom()?;
        while self.eat(&Token::Pipe) {
            let mut parts = vec![first];
            while let Some(next) = self.parse_pat_atom() {
                parts.push(next);
                if !self.eat(&Token::Pipe) { break; }
            }
            first = Pat::Or(parts);
        }
        // Optional `: Type`
        if self.eat(&Token::Colon) {
            let ty = self.parse_type()?;
            return Some(Pat::Typed { pat: Box::new(first), ty });
        }
        // `@` binding
        if let Some(n) = self.peek_at_name() {
            // Already handled in atom; skip.
        }
        Some(first)
    }

    fn peek_at_name(&self) -> Option<Name> { None }  // placeholder

    fn parse_pat_atom(&mut self) -> Option<Pat> {
        self.skip_trivia();
        match self.cur().clone() {
            Token::Underscore => { self.bump(); Some(Pat::Wildcard) }
            Token::Ident(s) => {
                // Could be bind, variant, or struct pattern.
                let _ = self.bump();
                let id = self.intern(&s);
                let name = Name { ident: id, span: self.cur_tok().span };
                // Check for path
                let mut path = vec![name];
                while self.eat(&Token::DoubleColon) {
                    if let Some(n) = self.name_from_token() { path.push(n); }
                }
                // Variant `Foo(a, b)` or struct `Foo { x, y }`
                if self.at(&Token::LParen) {
                    self.bump();
                    let mut args = vec![];
                    loop {
                        if self.eat(&Token::RParen) { break; }
                        if let Some(p) = self.parse_pat() { args.push(p); }
                        if !self.eat(&Token::Comma) { self.expect(Token::RParen, "`)`"); break; }
                    }
                    return Some(Pat::TupleStruct { path, args });
                }
                if self.at(&Token::LBrace) {
                    self.bump();
                    let mut fields = vec![];
                    let mut rest = false;
                    loop {
                        if self.eat(&Token::RBrace) { break; }
                        if self.eat(&Token::DoubleDot) { rest = true; self.expect(Token::RBrace, "`}`"); break; }
                        let fname = self.name_from_token()?;
                        let pat = if self.eat(&Token::Colon) {
                            self.parse_pat()?
                        } else {
                            Pat::Bind(fname)
                        };
                        fields.push(StructPatField { name: fname, pat });
                        if !self.eat(&Token::Comma) { self.expect(Token::RBrace, "`}`"); break; }
                    }
                    return Some(Pat::Struct { path, fields, rest });
                }
                // `name @ pat`
                if self.eat(&Token::At) {
                    let sub = self.parse_pat_atom()?;
                    return Some(Pat::At { name: path.into_iter().next().unwrap(), sub: Box::new(sub) });
                }
                // Just bind
                if path.len() == 1 {
                    if s == "mut" {
                        // `mut name`
                        if let Some(n) = self.name_from_token() {
                            return Some(Pat::MutBind(n));
                        }
                    }
                    Some(Pat::Bind(path.into_iter().next().unwrap()))
                } else {
                    // Path pattern — treat as a constant. Wrap as TupleStruct with no args.
                    Some(Pat::TupleStruct { path, args: vec![] })
                }
            }
            Token::IntLit { value, .. } => {
                self.bump();
                // Check for range pattern: `N..M` or `N..=M`.
                if self.at(&Token::DoubleDot) || self.at(&Token::DotEq) {
                    let inclusive = self.at(&Token::DotEq);
                    self.bump();
                    let hi = match self.cur().clone() {
                        Token::IntLit { value, .. } => { self.bump(); Some(Box::new(Expr::IntLit { value, ty_hint: None })) }
                        _ => None,
                    };
                    return Some(Pat::Range {
                        lo: Some(Box::new(Expr::IntLit { value, ty_hint: None })),
                        hi,
                        inclusive,
                    });
                }
                Some(Pat::Lit(Box::new(Expr::IntLit { value, ty_hint: None })))
            }
            Token::FloatLit { value, .. } => {
                self.bump();
                if self.at(&Token::DoubleDot) || self.at(&Token::DotEq) {
                    let inclusive = self.at(&Token::DotEq);
                    self.bump();
                    let hi = match self.cur().clone() {
                        Token::FloatLit { value, .. } => { self.bump(); Some(Box::new(Expr::FloatLit { value, ty_hint: None })) }
                        _ => None,
                    };
                    return Some(Pat::Range {
                        lo: Some(Box::new(Expr::FloatLit { value, ty_hint: None })),
                        hi,
                        inclusive,
                    });
                }
                Some(Pat::Lit(Box::new(Expr::FloatLit { value, ty_hint: None })))
            }
            Token::StrLit(s) => {
                self.bump();
                Some(Pat::Lit(Box::new(Expr::StrLit(s))))
            }
            Token::KwTrue => { self.bump(); Some(Pat::Lit(Box::new(Expr::BoolLit(true)))) }
            Token::KwFalse => { self.bump(); Some(Pat::Lit(Box::new(Expr::BoolLit(false)))) }
            Token::KwNull => { self.bump(); Some(Pat::Lit(Box::new(Expr::NullLit))) }
            Token::Minus => {
                // Negative number pattern: `-1`, `-3.14`
                self.bump();
                match self.cur().clone() {
                    Token::IntLit { value, .. } => {
                        self.bump();
                        Some(Pat::Lit(Box::new(Expr::IntLit { value: -value, ty_hint: None })))
                    }
                    Token::FloatLit { value, .. } => {
                        self.bump();
                        Some(Pat::Lit(Box::new(Expr::FloatLit { value: -value, ty_hint: None })))
                    }
                    _ => None,
                }
            }
            Token::LParen => {
                self.bump();
                let mut pats = vec![];
                loop {
                    if self.eat(&Token::RParen) { break; }
                    if let Some(p) = self.parse_pat() { pats.push(p); }
                    if !self.eat(&Token::Comma) { self.expect(Token::RParen, "`)`"); break; }
                }
                Some(Pat::Tuple(pats))
            }
            Token::LBracket => {
                self.bump();
                let mut pats = vec![];
                loop {
                    if self.eat(&Token::RBracket) { break; }
                    if let Some(p) = self.parse_pat() { pats.push(p); }
                    if !self.eat(&Token::Comma) { self.expect(Token::RBracket, "`]`"); break; }
                }
                Some(Pat::Slice(pats))
            }
            other => {
                self.errors.push(ParseError::new(self.cur_tok().span,
                    format!("expected pattern, got {}", token_name(&other))));
                None
            }
        }
    }

    // ----- Expressions (Pratt) -----

    pub fn parse_expr(&mut self) -> Option<Expr> {
        self.parse_expr_min_prec(0)
    }

    fn parse_expr_min_prec(&mut self, min_prec: u8) -> Option<Expr> {
        let mut lhs = self.parse_unary()?;
        loop {
            // Postfix operators: `.`, `?.`, `(...)`, `[...]`, `!`, `?`.
            lhs = self.parse_postfix(lhs)?;

            // Pipe forward `|>` — left-associative, very low precedence (just
            // above assignment). Spec §4.6.
            if self.at(&Token::PipeForward) && min_prec <= 1 {
                self.bump();
                let rhs = self.parse_expr_min_prec(2)?;
                lhs = Expr::Pipe { lhs: Box::new(lhs), rhs: Box::new(rhs) };
                continue;
            }

            // Elvis `?:` — like NullCoalesce, precedence 3. Spec §4.6.
            if self.at(&Token::Elvis) && min_prec <= 3 {
                self.bump();
                let rhs = self.parse_expr_min_prec(4)?;
                lhs = Expr::Elvis { cond: Box::new(lhs), default: Box::new(rhs) };
                continue;
            }

            // Range `..` and `..=` (§4.6). Low precedence, just above comparison.
            // Both bounds are required in expression context (open bounds are
            // only valid in `for`/match patterns).
            if (self.at(&Token::DoubleDot) || self.at(&Token::DotEq)) && min_prec <= 8 {
                let inclusive = self.at(&Token::DotEq);
                self.bump();
                // If we're followed by something that can't start an expression
                // (e.g. `{` for a for-loop body), leave `hi` as None.
                let hi = if self.is_expr_start() {
                    Some(Box::new(self.parse_expr_min_prec(9)?))
                } else { None };
                lhs = Expr::Range { lo: Some(Box::new(lhs)), hi, inclusive };
                continue;
            }

            // Binary operator
            let op = match self.classify_binop() {
                Some(op) => op,
                None => break,
            };
            let prec = op.prec();
            if prec < min_prec { break; }
            self.bump_binop();
            // Assignment is right-assoc.
            let next_min = if op.is_assign() { prec } else { prec + 1 };
            let rhs = self.parse_expr_min_prec(next_min)?;
            lhs = Expr::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs) };
        }
        Some(lhs)
    }

    fn parse_unary(&mut self) -> Option<Expr> {
        self.skip_trivia();
        match self.cur() {
            Token::Minus => {
                self.bump();
                let e = self.parse_unary()?;
                Some(Expr::Unary { op: UnOp::Neg, expr: Box::new(e) })
            }
            Token::Bang => {
                self.bump();
                let e = self.parse_unary()?;
                Some(Expr::Unary { op: UnOp::Not, expr: Box::new(e) })
            }
            Token::Star => {
                self.bump();
                let e = self.parse_unary()?;
                Some(Expr::Unary { op: UnOp::Deref, expr: Box::new(e) })
            }
            Token::Amp => {
                self.bump();
                // Reference — Skald doesn't have explicit references except `ref<T>`,
                // but allow `&` for borrow expressions on UObjects (rare).
                let _mutable = self.cur_is_kw("mut") && { self.bump(); true };
                let e = self.parse_unary()?;
                Some(Expr::Unary { op: UnOp::Deref, expr: Box::new(e) }) // approx
            }
            _ => self.parse_primary(),
        }
    }

    fn parse_postfix(&mut self, mut lhs: Expr) -> Option<Expr> {
        loop {
            match self.cur() {
                Token::Dot => {
                    self.bump();
                    let field = self.name_from_token()?;
                    if self.at(&Token::LParen) {
                        self.bump();
                        let args = self.parse_call_args()?;
                        lhs = Expr::MethodCall { receiver: Box::new(lhs), method: field, args };
                    } else {
                        lhs = Expr::Field { base: Box::new(lhs), field };
                    }
                }
                Token::OptionalChain => {
                    self.bump();
                    let field = self.name_from_token()?;
                    if self.at(&Token::LParen) {
                        self.bump();
                        let args = self.parse_call_args()?;
                        lhs = Expr::OptionalMethodCall { receiver: Box::new(lhs), method: field, args };
                    } else {
                        lhs = Expr::OptionalField { base: Box::new(lhs), field };
                    }
                }
                Token::LParen => {
                    self.bump();
                    let args = self.parse_call_args()?;
                    lhs = Expr::Call { callee: Box::new(lhs), args };
                }
                Token::LBracket => {
                    self.bump();
                    let idx = self.parse_expr()?;
                    self.expect(Token::RBracket, "`]`");
                    lhs = Expr::Index { base: Box::new(lhs), idx: Box::new(idx) };
                }
                Token::Bang => {
                    // `expr!` unwrap
                    self.bump();
                    lhs = Expr::Unwrap(Box::new(lhs));
                }
                Token::KwAs => {
                    self.bump();
                    let ty = self.parse_type()?;
                    lhs = Expr::Cast { expr: Box::new(lhs), ty };
                }
                Token::KwIs => {
                    self.bump();
                    let ty = self.parse_type()?;
                    lhs = Expr::Is { expr: Box::new(lhs), ty };
                }
                Token::KwAwait => {
                    self.bump();
                    lhs = Expr::Await(Box::new(lhs));
                }
                _ => break,
            }
        }
        Some(lhs)
    }

    fn parse_call_args(&mut self) -> Option<Vec<Expr>> {
        let mut args = vec![];
        loop {
            if self.eat(&Token::RParen) { return Some(args); }
            if let Some(e) = self.parse_expr() { args.push(e); }
            if !self.eat(&Token::Comma) {
                self.expect(Token::RParen, "`)`");
                return Some(args);
            }
        }
    }

    fn classify_binop(&self) -> Option<BinOp> {
        use BinOp::*;
        Some(match self.cur() {
            Token::Plus => Add,
            Token::Minus => Sub,
            Token::Star => Mul,
            Token::Slash => Div,
            Token::Percent => Mod,
            Token::Eq => Assign,
            Token::EqEq => Eq,
            Token::BangEq => Ne,
            Token::Lt => Lt,
            Token::Le => Le,
            Token::Gt => Gt,
            Token::Ge => Ge,
            Token::AmpAmp => And,
            Token::PipePipe => Or,
            Token::Amp => BitAnd,
            Token::Pipe => BitOr,
            Token::Caret => BitXor,
            Token::Shl => Shl,
            Token::Shr => Shr,
            Token::PlusEq => AddAssign,
            Token::MinusEq => SubAssign,
            Token::StarEq => MulAssign,
            Token::SlashEq => DivAssign,
            Token::PercentEq => ModAssign,
            Token::AmpEq => BitAndAssign,
            Token::PipeEq => BitOrAssign,
            Token::CaretEq => BitXorAssign,
            Token::ShlEq => ShlAssign,
            Token::ShrEq => ShrAssign,
            Token::NullCoalesce => NullCoalesce,
            _ => return None,
        })
    }

    fn bump_binop(&mut self) {
        self.bump();
    }

    fn parse_primary(&mut self) -> Option<Expr> {
        self.skip_trivia();
        match self.cur().clone() {
            Token::IntLit { value, suffix } => {
                self.bump();
                let ty_hint = suffix.as_deref().and_then(parse_prim);
                Some(Expr::IntLit { value, ty_hint })
            }
            Token::FloatLit { value, suffix } => {
                self.bump();
                let ty_hint = suffix.as_deref().and_then(parse_prim);
                Some(Expr::FloatLit { value, ty_hint })
            }
            Token::StrLit(s) => { self.bump(); Some(Expr::StrLit(s)) }
            Token::ByteStrLit(b) => { self.bump(); Some(Expr::ByteStrLit(b)) }
            Token::CharLit(c) => { self.bump(); Some(Expr::CharLit(c)) }
            Token::FmtStrLit(s) => { self.bump(); parse_fmt_string(&s) }
            Token::KwTrue => { self.bump(); Some(Expr::BoolLit(true)) }
            Token::KwFalse => { self.bump(); Some(Expr::BoolLit(false)) }
            Token::KwNull => { self.bump(); Some(Expr::NullLit) }
            Token::KwSelf_ => { self.bump(); Some(Expr::SelfRef) }
            Token::KwSelfTy => { self.bump(); Some(Expr::Ident(Name::dummy())) } // Self type used in expr context is rare; treat as ident
            Token::KwSuper => { self.bump(); Some(Expr::SuperRef) }
            Token::KwReturn => {
                self.bump();
                let e = if self.is_expr_start() { Some(Box::new(self.parse_expr()?)) } else { None };
                Some(Expr::Return(e))
            }
            Token::KwBreak => {
                self.bump();
                let e = if self.is_expr_start() && !matches!(self.cur(), Token::Semicolon | Token::RBrace | Token::Eof) {
                    Some(Box::new(self.parse_expr()?))
                } else { None };
                Some(Expr::Break(e))
            }
            Token::KwCont => { self.bump(); Some(Expr::Cont) }
            Token::KwIf => self.parse_if(),
            Token::KwMatch => self.parse_match(),
            Token::KwFor => self.parse_for(false),
            Token::KwWhile => self.parse_while(),
            Token::KwLoop => {
                self.bump();
                let body = self.parse_block()?;
                Some(Expr::Loop(Box::new(body)))
            }
            Token::KwSpawn => self.parse_spawn(),
            Token::KwAwait => {
                self.bump();
                let e = self.parse_unary()?;
                Some(Expr::Await(Box::new(e)))
            }
            Token::KwParallel => {
                self.bump();
                self.parse_for(true)
            }
            Token::KwTransaction => {
                self.bump();
                let b = self.parse_block()?;
                Some(Expr::Transaction(Box::new(b)))
            }
            Token::At => {
                // `@unsafe { ... }` block, `@region(...)`, `@simd ...`, etc.
                self.bump();
                let name = match self.cur().clone() {
                    Token::Ident(s) => { self.bump(); s }
                    _ => return None,
                };
                let args: Vec<String> = if self.at(&Token::LParen) {
                    self.bump();
                    let mut args = vec![];
                    let mut depth = 1;
                    let mut cur = String::new();
                    while depth > 0 {
                        match self.cur().clone() {
                            Token::LParen => { depth += 1; cur.push('('); self.bump(); }
                            Token::RParen => {
                                depth -= 1;
                                if depth == 0 { self.bump(); args.push(std::mem::take(&mut cur)); break; }
                                else { cur.push(')'); self.bump(); }
                            }
                            Token::Comma if depth == 1 => { args.push(std::mem::take(&mut cur)); self.bump(); }
                            Token::Eof => break,
                            other => { cur.push_str(&token_to_src(&other)); self.bump(); }
                        }
                    }
                    args
                } else { vec![] };
                let ann = skald_lexer::parse_annotation(&name, &args);
                match ann {
                    Annotation::Unsafe => {
                        let b = self.parse_block()?;
                        Some(Expr::UnsafeBlock(Box::new(b)))
                    }
                    Annotation::Region(_) | Annotation::Arena | Annotation::Simd => {
                        // These wrap a block or a single statement (e.g., `@simd for ...`).
                        if self.at(&Token::LBrace) {
                            let b = self.parse_block()?;
                            Some(Expr::Block(Box::new(b)))
                        } else {
                            // Wrap next statement/expr — for now, parse as expr.
                            let e = self.parse_expr()?;
                            Some(e)
                        }
                    }
                    _ => {
                        // Other annotations on expressions — just parse next.
                        let e = self.parse_expr()?;
                        Some(e)
                    }
                }
            }
            Token::LParen => {
                self.bump();
                if self.eat(&Token::RParen) {
                    return Some(Expr::UnitLit);
                }
                let e = self.parse_expr()?;
                if self.eat(&Token::Comma) {
                    // Tuple
                    let mut items = vec![e];
                    loop {
                        if self.eat(&Token::RParen) { break; }
                        if let Some(e2) = self.parse_expr() { items.push(e2); }
                        if !self.eat(&Token::Comma) { self.expect(Token::RParen, "`)`"); break; }
                    }
                    return Some(Expr::TupleLit(items));
                }
                self.expect(Token::RParen, "`)`");
                Some(Expr::Paren(Box::new(e)))
            }
            Token::LBracket => {
                self.bump();
                let mut items = vec![];
                loop {
                    if self.eat(&Token::RBracket) { break; }
                    if let Some(e) = self.parse_expr() { items.push(e); }
                    if !self.eat(&Token::Comma) { self.expect(Token::RBracket, "`]`"); break; }
                }
                Some(Expr::ArrayLit(items))
            }
            Token::LBrace => {
                let b = self.parse_block()?;
                Some(Expr::Block(Box::new(b)))
            }
            Token::Pipe => {
                // Lambda `|...| expr`
                self.bump();
                let mut params = vec![];
                loop {
                    if self.eat(&Token::Pipe) { break; }
                    // Lambda params don't support or-patterns — use parse_pat_atom
                    // directly so the closing `|` isn't misread as an or-separator.
                    let pat = self.parse_pat_atom()?;
                    let ty = if self.eat(&Token::Colon) {
                        Some(self.parse_type()?)
                    } else { None };
                    params.push(LambdaParam { pat, ty });
                    if !self.eat(&Token::Comma) {
                        self.expect(Token::Pipe, "`|`");
                        break;
                    }
                }
                let body = self.parse_expr()?;
                Some(Expr::Lambda { params, body: Box::new(body) })
            }
            Token::PipePipe => {
                // Empty lambda `|| expr` — disambiguated from logical OR by
                // position (only at expression start, which is how we got here).
                self.bump();
                let body = self.parse_expr()?;
                Some(Expr::Lambda { params: vec![], body: Box::new(body) })
            }
            Token::Ident(s) => {
                // v3(1,2,3) / quat(...) — vector literals
                if matches!(self.peek(1), Token::LParen) {
                    if let Some(m) = parse_math(&s) {
                        self.bump(); // ident
                        self.bump(); // (
                        let args = self.parse_call_args()?;
                        return Some(Expr::VectorLit { kind: m, args });
                    }
                }
                // Path expression `Module::item::name`
                let _ = self.bump();
                let id = self.intern(&s);
                let name = Name { ident: id, span: self.cur_tok().span };
                let mut path = vec![name];
                while self.eat(&Token::DoubleColon) {
                    if let Some(n) = self.name_from_token() { path.push(n); }
                    else { break; }
                }
                if path.len() > 1 {
                    if self.at(&Token::LParen) {
                        self.bump();
                        let args = self.parse_call_args()?;
                        return Some(Expr::PathCall { path, args });
                    }
                    // Struct literal? `Type { field: val }`
                    if self.at(&Token::LBrace) && self.is_struct_lit_start() {
                        self.bump();
                        let fields = self.parse_struct_lit_fields()?;
                        let ty = path_to_type(path);
                        return Some(Expr::StructLit { ty, fields });
                    }
                    return Some(Expr::Path(path));
                }
                // Single name — could be struct literal `Foo { ... }` or call.
                if self.at(&Token::LBrace) && self.is_struct_lit_start() {
                    self.bump();
                    let fields = self.parse_struct_lit_fields()?;
                    let ty = Type::Named(name);
                    return Some(Expr::StructLit { ty, fields });
                }
                Some(Expr::Ident(name))
            }
            other => {
                self.errors.push(ParseError::new(self.cur_tok().span,
                    format!("expected expression, got {}", token_name(&other))));
                None
            }
        }
    }

    fn is_struct_lit_start(&self) -> bool {
        // Look ahead: `{` followed by ident `:` or `}` or `..`
        if !self.at(&Token::LBrace) { return false; }
        let next = self.peek(1);
        matches!(next, Token::RBrace) ||
        matches!(next, Token::DoubleDot) ||
        matches!(next, Token::Ident(_) | Token::RawIdent(_)) && matches!(self.peek(2), Token::Colon | Token::Comma | Token::RBrace)
    }

    fn parse_struct_lit_fields(&mut self) -> Option<Vec<StructLitField>> {
        let mut fields = vec![];
        loop {
            if self.eat(&Token::RBrace) { break; }
            if self.eat(&Token::DoubleDot) {
                // base expr
                let _ = self.parse_expr()?;
                self.expect(Token::RBrace, "`}`");
                break;
            }
            let name = self.name_from_token()?;
            let value = if self.eat(&Token::Colon) {
                Some(self.parse_expr()?)
            } else { None };
            fields.push(StructLitField { name, value });
            if !self.eat(&Token::Comma) {
                self.expect(Token::RBrace, "`}`");
                break;
            }
        }
        Some(fields)
    }

    fn parse_if(&mut self) -> Option<Expr> {
        self.expect(Token::KwIf, "`if`");
        let cond = self.parse_expr()?;
        let then = self.parse_block()?;
        let else_ = if self.eat(&Token::KwElse) {
            if self.at(&Token::KwIf) {
                Some(Box::new(self.parse_if()?))
            } else {
                Some(Box::new(Expr::Block(Box::new(self.parse_block()?))))
            }
        } else { None };
        Some(Expr::If { cond: Box::new(cond), then: Box::new(then), else_ })
    }

    fn parse_match(&mut self) -> Option<Expr> {
        self.expect(Token::KwMatch, "`match`");
        let scrutinee = self.parse_expr()?;
        self.expect(Token::LBrace, "`{`");
        let mut arms = vec![];
        let mut last_pos = self.pos;
        loop {
            self.skip_trivia();
            if self.eat(&Token::RBrace) { break; }
            let pat = self.parse_pat()?;
            let guard = if self.at(&Token::KwIf) {
                self.bump();
                Some(self.parse_expr()?)
            } else { None };
            self.expect(Token::FatArrow, "`=>`");
            let body = self.parse_expr()?;
            arms.push(MatchArm { pat, guard, body });
            self.eat(&Token::Comma);
            // Safety: force-advance if no progress.
            if self.pos == last_pos { self.bump(); }
            last_pos = self.pos;
        }
        Some(Expr::Match { scrutinee: Box::new(scrutinee), arms })
    }

    fn parse_for(&mut self, parallel: bool) -> Option<Expr> {
        self.expect(Token::KwFor, "`for`");
        let pat = self.parse_pat()?;
        self.expect(Token::KwIn, "`in`");
        let iter = self.parse_expr()?;
        // Disable range parsing ambiguity: `for i in 0..10 { ... }` — `..` is part of iter expr.
        let body = self.parse_block()?;
        if parallel {
            Some(Expr::ParallelFor { pat: Box::new(pat), iter: Box::new(iter), body: Box::new(body) })
        } else {
            Some(Expr::For { pat: Box::new(pat), iter: Box::new(iter), body: Box::new(body) })
        }
    }

    fn parse_while(&mut self) -> Option<Expr> {
        self.expect(Token::KwWhile, "`while`");
        let cond = self.parse_expr()?;
        let body = self.parse_block()?;
        Some(Expr::While { cond: Box::new(cond), body: Box::new(body) })
    }

    fn parse_spawn(&mut self) -> Option<Expr> {
        self.expect(Token::KwSpawn, "`spawn`");
        let kind = if self.eat(&Token::KwWorker) {
            SpawnKind::Worker
        } else if self.eat(&Token::KwAsync) {
            SpawnKind::Async
        } else {
            SpawnKind::Async
        };
        // Optional `move` keyword
        let _ = self.cur_is_kw("move") && { self.bump(); true };
        let body = if self.at(&Token::LBrace) {
            self.parse_block()?
        } else {
            // `spawn expr` — wrap as block tail.
            let e = self.parse_expr()?;
            Block::new(vec![], Some(e))
        };
        Some(Expr::Spawn { kind, body: Box::new(body) })
    }
}

// ---------- Helpers ----------

fn parse_prim(s: &str) -> Option<PrimType> {
    Some(match s {
        "i8" => PrimType::I8,
        "i16" => PrimType::I16,
        "i32" => PrimType::I32,
        "i64" => PrimType::I64,
        "i128" => PrimType::I128,
        "u8" => PrimType::U8,
        "u16" => PrimType::U16,
        "u32" => PrimType::U32,
        "u64" => PrimType::U64,
        "u128" => PrimType::U128,
        "f32" => PrimType::F32,
        "f64" => PrimType::F64,
        "bool" => PrimType::Bool,
        "char" => PrimType::Char,
        "str" => PrimType::Str,
        "name" => PrimType::Name,
        "text" => PrimType::Text,
        "void" => PrimType::Void,
        "never" => PrimType::Never,
        _ => return None,
    })
}

fn parse_math(s: &str) -> Option<MathType> {
    Some(match s {
        "v2" => MathType::V2,
        "v3" => MathType::V3,
        "v4" => MathType::V4,
        "quat" => MathType::Quat,
        "rot" => MathType::Rot,
        "mat4" => MathType::Mat4,
        _ => return None,
    })
}

fn path_to_type(path: Vec<Name>) -> Type {
    if path.len() == 1 {
        Type::Named(path.into_iter().next().unwrap())
    } else {
        // Use App with Named(path[0]) and treat rest as ... actually path is just a name.
        // Encode as Named(last) for now (lose module prefix in type — types module handles resolution).
        Type::Named(path.into_iter().last().unwrap())
    }
}

fn token_name(t: &Token) -> String {
    match t {
        Token::Ident(s) => format!("identifier `{}`", s),
        Token::RawIdent(s) => format!("raw identifier `{}`", s),
        Token::IntLit { .. } => "integer literal".to_string(),
        Token::FloatLit { .. } => "float literal".to_string(),
        Token::StrLit(_) => "string literal".to_string(),
        Token::ByteStrLit(_) => "byte string literal".to_string(),
        Token::CharLit(_) => "char literal".to_string(),
        Token::FmtStrLit(_) => "format string literal".to_string(),
        Token::Eof => "end of file".to_string(),
        other => format!("{:?}", other).to_lowercase().replace('_', " "),
    }
}

fn token_to_src(t: &Token) -> String {
    match t {
        Token::Ident(s) | Token::RawIdent(s) => s.clone(),
        Token::IntLit { value, suffix } => format!("{}{}", value, suffix.as_deref().unwrap_or("")),
        Token::FloatLit { value, suffix } => format!("{}{}", value, suffix.as_deref().unwrap_or("")),
        Token::StrLit(s) => format!("\"{}\"", s),
        Token::CharLit(c) => format!("'{}'", c),
        Token::LParen => "(".into(),
        Token::RParen => ")".into(),
        Token::LBrace => "{".into(),
        Token::RBrace => "}".into(),
        Token::LBracket => "[".into(),
        Token::RBracket => "]".into(),
        Token::Comma => ",".into(),
        Token::Semicolon => ";".into(),
        Token::Colon => ":".into(),
        Token::DoubleColon => "::".into(),
        Token::Dot => ".".into(),
        Token::DoubleDot => "..".into(),
        Token::TripleDot => "...".into(),
        Token::DotEq => "..=".into(),
        Token::Arrow => "->".into(),
        Token::FatArrow => "=>".into(),
        Token::Pipe => "|".into(),
        Token::PipeForward => "|>".into(),
        Token::Plus => "+".into(),
        Token::Minus => "-".into(),
        Token::Star => "*".into(),
        Token::Slash => "/".into(),
        Token::Percent => "%".into(),
        Token::Eq => "=".into(),
        Token::EqEq => "==".into(),
        Token::BangEq => "!=".into(),
        Token::Lt => "<".into(),
        Token::Le => "<=".into(),
        Token::Gt => ">".into(),
        Token::Ge => ">=".into(),
        Token::Amp => "&".into(),
        Token::AmpAmp => "&&".into(),
        Token::PipePipe => "||".into(),
        Token::Caret => "^".into(),
        Token::Tilde => "~".into(),
        Token::At => "@".into(),
        Token::Hash => "#".into(),
        Token::Dollar => "$".into(),
        Token::Question => "?".into(),
        Token::Bang => "!".into(),
        Token::Shl => "<<".into(),
        Token::Shr => ">>".into(),
        other => format!("{:?}", other),
    }
}

/// Parse `f"..."` raw inner source into pieces.
fn parse_fmt_string(s: &str) -> Option<Expr> {
    let mut pieces = vec![];
    let mut text = String::new();
    let chars: Vec<char> = s.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c == '{' {
            if i + 1 < chars.len() && chars[i + 1] == '{' {
                text.push('{');
                i += 2;
                continue;
            }
            // Push text piece
            if !text.is_empty() {
                pieces.push(FmtPiece::Text(std::mem::take(&mut text)));
            }
            // Read until `}`
            i += 1;
            let mut expr_s = String::new();
            let mut fmt = String::new();
            let mut in_fmt = false;
            while i < chars.len() && chars[i] != '}' {
                if chars[i] == ':' { in_fmt = true; i += 1; continue; }
                if in_fmt { fmt.push(chars[i]); }
                else { expr_s.push(chars[i]); }
                i += 1;
            }
            if i < chars.len() && chars[i] == '}' { i += 1; }
            // Re-parse expr_s as a Skald expression. We use a sub-parser.
            let mut sub_interner = Interner::new();
            let (toks, errs) = skald_lexer::tokenize(&expr_s, 0, &mut sub_interner);
            if !errs.is_empty() {
                // Fallback — embed as text.
                pieces.push(FmtPiece::Text(format!("{{{}}}", expr_s)));
                continue;
            }
            let mut p = Parser::new(toks, 0, sub_interner);
            if let Some(e) = p.parse_expr() {
                pieces.push(FmtPiece::Expr { expr: Box::new(e), fmt: if fmt.is_empty() { None } else { Some(fmt) } });
            } else {
                pieces.push(FmtPiece::Text(format!("{{{}}}", expr_s)));
            }
        } else if c == '}' && i + 1 < chars.len() && chars[i + 1] == '}' {
            text.push('}');
            i += 2;
        } else {
            text.push(c);
            i += 1;
        }
    }
    if !text.is_empty() { pieces.push(FmtPiece::Text(text)); }
    Some(Expr::FmtStrLit(pieces))
}

// ---------- Public entry ----------

/// Parse a Skald source file into a `Module`.
pub fn parse(src: &str, file: FileId, module_name: String) -> (Module, Vec<ParseError>, Vec<skald_lexer::LexError>) {
    let mut interner = Interner::new();
    let (toks, lex_errs) = skald_lexer::tokenize(src, file, &mut interner);
    let mut p = Parser::new(toks, file, interner);
    let module = p.parse_module(module_name);
    (module, p.errors, lex_errs)
}
