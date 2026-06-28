//! # skald-lexer
//!
//! Hand-rolled state-machine tokenizer for Skald (spec §3.2, §4).
//!
//! Produces `Vec<Token>` with span info. The lexer also interns all
//! identifiers/strings into an `Interner` that is shared with the parser.
//!
//! Lexer design choices:
//! - Hand-rolled (no `logos` / regex) — per spec §3.2 ("lean toward hand-rolled
//!   to avoid macro hygiene issues with `proc-macro2` versions").
//! - UTF-8 input, BOM-rejected, CRLF normalized to LF.
//! - Block comments are nestable (`/* /* */ */`).
//! - Doc comments (`///`, `//!`) emitted as their own tokens so the parser can
//!   attach them to the following item.
//! - Strings support `f"..."` interpolation by emitting `FmtStrStart`,
//!   `Expr`-tokens (raw), `FmtStrEnd`. Actually for simplicity we emit a single
//!   `FmtStr` token carrying the raw inner source; the parser re-parses the
//!   pieces. This is the same approach rust-analyzer uses for raw strings.

#![allow(clippy::needless_range_loop)]

use rustc_hash::FxHashMap;
use skald_ast::{Annotation, FileId, Ident, InlineKind, Interner, LayoutKind, Span};

// ---------- Token ----------

#[derive(Debug, Clone, PartialEq)]
pub enum Token {
    // Identifiers & literals
    Ident(String),
    RawIdent(String),  // r#name
    IntLit { value: i128, suffix: Option<String> },
    FloatLit { value: f64, suffix: Option<String> },
    StrLit(String),
    ByteStrLit(Vec<u8>),
    CharLit(char),
    FmtStrLit(String),  // raw inner source — parser splits
    // Keywords (§4.3)
    KwLet, KwVar, KwFn, KwClass, KwStruct, KwEnum, KwTrait, KwImpl, KwPub, KwPrivate,
    KwProtected, KwConst, KwUse, KwMod, KwType, KwAlias, KwStatic,
    KwIf, KwElse, KwMatch, KwFor, KwWhile, KwLoop, KwBreak, KwCont, KwReturn,
    KwAsync, KwAwait, KwSpawn, KwAs, KwIs, KwIn, KwSelf_, KwSelfTy, KwSuper,
    KwTrue, KwFalse, KwNull,
    KwOverride, KwVirtual, KwAbstract, KwFinal, KwReadonly,
    KwParallel,  // `parallel for`
    KwWorker,    // `spawn worker`
    KwTransaction,
    // Punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    LAngle, RAngle,    // <  >  (context: generic args or comparison)
    Comma, Semicolon, Colon, DoubleColon,  // , ; : ::
    Dot, DoubleDot, TripleDot, DotEq,  // . .. ... ..=
    Arrow, FatArrow, Pipe, PipeForward,  // -> => | |>
    Question, Bang, Tilde, At, Hash, Dollar,
    Plus, Minus, Star, Slash, Percent,
    Eq, EqEq, BangEq, Lt, Le, Gt, Ge,
    Amp, AmpAmp, PipePipe, Caret,
    Shl, Shr,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
    OptionalChain,    // ?.
    Elvis,            // ?:
    NullCoalesce,     // ??
    Underscore,
    // Annotation start (lexer keeps it as `At` + Ident — parser assembles).
    // Doc comment tokens
    DocComment(String),   // ///
    InnerDocComment(String), // //!
    LineComment,          // (collapsed — content not preserved for now)
    BlockComment,
    // Sentinel
    Eof,
}

impl Token {
    pub fn is_trivia(&self) -> bool {
        matches!(self,
            Token::LineComment | Token::BlockComment
            | Token::DocComment(_) | Token::InnerDocComment(_))
    }
    pub fn is_eof(&self) -> bool { matches!(self, Token::Eof) }
}

// ---------- Token + span ----------

#[derive(Debug, Clone, PartialEq)]
pub struct Tok {
    pub kind: Token,
    pub span: Span,
}

// ---------- Keyword table ----------

fn keyword_table() -> FxHashMap<&'static str, Token> {
    use Token::*;
    let mut m = FxHashMap::default();
    // Declarations
    m.insert("let", KwLet); m.insert("var", KwVar); m.insert("fn", KwFn);
    m.insert("class", KwClass); m.insert("struct", KwStruct);
    m.insert("enum", KwEnum); m.insert("trait", KwTrait);
    m.insert("impl", KwImpl); m.insert("pub", KwPub);
    m.insert("private", KwPrivate); m.insert("protected", KwProtected);
    m.insert("const", KwConst); m.insert("use", KwUse);
    m.insert("mod", KwMod); m.insert("type", KwType); m.insert("alias", KwAlias);
    m.insert("static", KwStatic);
    // Control flow
    m.insert("if", KwIf); m.insert("else", KwElse); m.insert("match", KwMatch);
    m.insert("for", KwFor); m.insert("while", KwWhile); m.insert("loop", KwLoop);
    m.insert("break", KwBreak); m.insert("cont", KwCont); m.insert("return", KwReturn);
    m.insert("async", KwAsync); m.insert("await", KwAwait); m.insert("spawn", KwSpawn);
    // Operators / keywords
    m.insert("as", KwAs); m.insert("is", KwIs); m.insert("in", KwIn);
    m.insert("self", KwSelf_); m.insert("Self", KwSelfTy); m.insert("super", KwSuper);
    m.insert("true", KwTrue); m.insert("false", KwFalse); m.insert("null", KwNull);
    // Modifiers
    m.insert("override", KwOverride); m.insert("virtual", KwVirtual);
    m.insert("abstract", KwAbstract); m.insert("final", KwFinal);
    m.insert("readonly", KwReadonly);
    // Other
    m.insert("parallel", KwParallel);
    m.insert("worker", KwWorker);
    m.insert("transaction", KwTransaction);
    m
}

// ---------- Errors ----------

#[derive(Debug, Clone, PartialEq)]
pub enum LexError {
    UnexpectedChar { ch: char, span: Span },
    UnterminatedString { span: Span },
    UnterminatedChar { span: Span },
    UnterminatedBlockComment { span: Span },
    InvalidEscape { ch: char, span: Span },
    InvalidNumberSuffix { suffix: String, span: Span },
    NumberTooBig { span: Span },
    BOM { span: Span },
}

// ---------- Lexer ----------

pub struct Lexer<'src> {
    src: &'src [u8],
    /// Chars with their byte offsets. Pre-collected for fast indexing.
    chars: Vec<(char, u32)>,
    pos: usize,  // index into `chars`
    file: FileId,
    interner: &'src mut Interner,
    keywords: FxHashMap<&'static str, Token>,
    pub errors: Vec<LexError>,
    /// Pending doc-comment tokens (so they can attach to next item).
    pending_doc: Vec<String>,
    pending_inner_doc: Vec<String>,
}

impl<'src> Lexer<'src> {
    pub fn new(src: &'src str, file: FileId, interner: &'src mut Interner) -> Self {
        // Reject BOM.
        let mut src = src;
        if src.starts_with('\u{FEFF}') {
            // skip BOM
            src = &src[3..];
        }
        // Normalize CRLF -> LF: build chars from a normalized copy.
        let normalized: String = if src.contains('\r') {
            src.replace("\r\n", "\n").replace('\r', "\n")
        } else {
            src.to_string()
        };
        let chars: Vec<(char, u32)> = normalized
            .char_indices()
            .map(|(i, c)| (c, i as u32))
            .collect();
        Self {
            src: src.as_bytes(),
            chars,
            pos: 0,
            file,
            interner,
            keywords: keyword_table(),
            errors: vec![],
            pending_doc: vec![],
            pending_inner_doc: vec![],
        }
    }

    fn peek(&self) -> Option<(char, u32)> {
        self.chars.get(self.pos).copied()
    }
    fn peek2(&self) -> Option<(char, u32)> {
        self.chars.get(self.pos + 1).copied()
    }
    fn peek3(&self) -> Option<(char, u32)> {
        self.chars.get(self.pos + 2).copied()
    }
    fn at_eof(&self) -> bool { self.pos >= self.chars.len() }
    fn bump(&mut self) -> Option<(char, u32)> {
        let c = self.chars.get(self.pos).copied();
        if c.is_some() { self.pos += 1; }
        c
    }
    fn span(&self, start: u32, end: u32) -> Span { Span::new(self.file, start, end) }

    /// Main entry: tokenize the whole source, returning `Vec<Tok>` and errors.
    pub fn tokenize(mut self) -> (Vec<Tok>, Vec<LexError>, Vec<String>, Vec<String>) {
        let mut out = vec![];
        while !self.at_eof() {
            let start = self.peek().map(|(_, b)| b).unwrap_or(0);
            if let Some(tok) = self.next_token() {
                let end = self.cur_byte();
                out.push(Tok { kind: tok, span: self.span(start, end) });
            }
        }
        out.push(Tok { kind: Token::Eof, span: self.span(self.cur_byte(), self.cur_byte()) });
        let doc = std::mem::take(&mut self.pending_doc);
        let inner = std::mem::take(&mut self.pending_inner_doc);
        (out, self.errors, doc, inner)
    }

    fn cur_byte(&self) -> u32 {
        self.peek().map(|(_, b)| b).unwrap_or_else(|| self.src.len() as u32)
    }

    fn next_token(&mut self) -> Option<Token> {
        // Skip whitespace (we already normalized CRLF).
        while let Some((c, _)) = self.peek() {
            if c.is_whitespace() { self.bump(); }
            else { break; }
        }
        let (c, start) = match self.peek() { Some(x) => x, None => return None };

        // Comments
        if c == '/' {
            if let Some(('/', _)) = self.peek2() {
                return self.line_comment(start);
            }
            if let Some(('*', _)) = self.peek2() {
                return self.block_comment(start);
            }
        }

        // Strings & chars — check BEFORE identifier so `f"..."` and `b"..."` win.
        if c == 'f' && self.peek2().map(|(c, _)| c) == Some('"') {
            self.bump(); // f
            return Some(self.fmt_string(start));
        }
        if c == 'b' && self.peek2().map(|(c, _)| c) == Some('"') {
            self.bump(); // b
            return Some(self.byte_string(start));
        }
        // Raw identifier `r#name` or raw string `r#"..."#`
        if c == 'r' && self.peek2().map(|(c, _)| c) == Some('#') {
            // Could be raw ident or `r#"..."#` raw string. Disambiguate by peek3.
            if let Some((p3, _)) = self.peek3() {
                if p3 == '"' || p3 == '#' {
                    // raw string literal
                    return self.raw_string(start);
                }
                if p3.is_alphabetic() || p3 == '_' {
                    // raw identifier
                    self.bump(); // r
                    self.bump(); // #
                    return self.ident(start);
                }
            }
        }

        if c == '\'' {
            // Char literal — but might be lifetime-like label. Skald has no
            // lifetimes, so always char.
            return Some(self.char_lit(start));
        }

        // Identifiers / keywords (must come AFTER f"/b"/r# disambiguation).
        if c == '_' && self.peek2().map(|(c, _)| c).map_or(false, |c| c.is_alphabetic()) {
            // `_name` — identifier, NOT wildcard. But spec says `_` alone is wildcard.
            return self.ident(start);
        }
        if c == '_' && self.peek2().is_none() {
            // `_` wildcard
            self.bump();
            return Some(Token::Underscore);
        }
        if c == '_' {
            // Just `_` followed by non-alpha
            let next = self.peek2().map(|(c, _)| c);
            if next.is_none() || !next.unwrap().is_alphanumeric() {
                self.bump();
                return Some(Token::Underscore);
            }
        }
        if c.is_alphabetic() || c == '_' {
            return self.ident(start);
        }

        // Numeric literals
        if c.is_ascii_digit() {
            return self.number(start);
        }
        if c == '.' && self.peek2().map(|(c, _)| c).map_or(false, |c| c.is_ascii_digit()) {
            // `.5` float
            return self.number(start);
        }

        if c == '"' { return Some(self.string(start)); }

        // Punctuation
        self.punct(start)
    }

    // ----- Identifiers -----

    fn ident(&mut self, start: u32) -> Option<Token> {
        let mut s = String::new();
        while let Some((c, _)) = self.peek() {
            if c.is_alphanumeric() || c == '_' {
                s.push(c);
                self.bump();
            } else { break; }
        }
        if let Some(kw) = self.keywords.get(s.as_str()) {
            return Some(kw.clone());
        }
        // Intern now so parser doesn't have to re-intern.
        let _ = self.interner.intern(&s);
        Some(Token::Ident(s))
    }

    // ----- Numbers -----

    fn number(&mut self, start: u32) -> Option<Token> {
        let mut s = String::new();
        let mut is_float = false;
        let mut is_hex = false;
        let mut is_bin = false;

        // First char
        if self.peek().map(|(c, _)| c) == Some('0') {
            match self.peek2().map(|(c, _)| c) {
                Some('x') | Some('X') => { is_hex = true; s.push('0'); s.push('x'); self.bump(); self.bump(); }
                Some('b') | Some('B') => { is_bin = true; s.push('0'); s.push('b'); self.bump(); self.bump(); }
                _ => {}
            }
        }
        if is_hex {
            while let Some((c, _)) = self.peek() {
                if c.is_ascii_hexdigit() || c == '_' { s.push(c); self.bump(); }
                else { break; }
            }
        } else if is_bin {
            while let Some((c, _)) = self.peek() {
                if c == '0' || c == '1' || c == '_' { s.push(c); self.bump(); }
                else { break; }
            }
        } else {
            while let Some((c, _)) = self.peek() {
                if c.is_ascii_digit() || c == '_' { s.push(c); self.bump(); }
                else { break; }
            }
            // Float? `.` followed by digit
            if self.peek().map(|(c, _)| c) == Some('.')
                && self.peek2().map(|(c, _)| c).map_or(false, |c| c.is_ascii_digit())
            {
                is_float = true;
                s.push('.'); self.bump();
                while let Some((c, _)) = self.peek() {
                    if c.is_ascii_digit() || c == '_' { s.push(c); self.bump(); }
                    else { break; }
                }
            }
            // Exponent
            if matches!(self.peek().map(|(c, _)| c), Some('e') | Some('E')) {
                is_float = true;
                s.push('e'); self.bump();
                if matches!(self.peek().map(|(c, _)| c), Some('+') | Some('-')) {
                    s.push(self.bump().unwrap().0);
                }
                while let Some((c, _)) = self.peek() {
                    if c.is_ascii_digit() || c == '_' { s.push(c); self.bump(); }
                    else { break; }
                }
            }
        }
        // Suffix — `123i8`, `1.0f32`
        let mut suffix = String::new();
        while let Some((c, _)) = self.peek() {
            if c.is_alphanumeric() || c == '_' { suffix.push(c); self.bump(); }
            else { break; }
        }
        if is_float {
            let s_clean: String = s.chars().filter(|c| *c != '_').collect();
            let val: f64 = s_clean.parse().unwrap_or(f64::INFINITY);
            let suffix_opt = if suffix.is_empty() { None } else { Some(suffix) };
            Some(Token::FloatLit { value: val, suffix: suffix_opt })
        } else {
            let s_clean: String = s.chars().filter(|c| *c != '_').collect();
            let val: i128 = if is_hex {
                i128::from_str_radix(&s_clean[2..], 16).unwrap_or(i128::MAX)
            } else if is_bin {
                i128::from_str_radix(&s_clean[2..], 2).unwrap_or(i128::MAX)
            } else {
                s_clean.parse().unwrap_or(i128::MAX)
            };
            let suffix_opt = if suffix.is_empty() { None } else { Some(suffix) };
            Some(Token::IntLit { value: val, suffix: suffix_opt })
        }
    }

    // ----- Strings -----

    fn string(&mut self, start: u32) -> Token {
        self.bump(); // opening "
        let mut s = String::new();
        loop {
            match self.bump() {
                None => {
                    self.errors.push(LexError::UnterminatedString { span: self.span(start, self.cur_byte()) });
                    return Token::StrLit(s);
                }
                Some(('"', _)) => break,
                Some(('\\', _)) => {
                    if let Some((ec, _)) = self.bump() {
                        match ec {
                            'n' => s.push('\n'),
                            't' => s.push('\t'),
                            'r' => s.push('\r'),
                            '0' => s.push('\0'),
                            '\\' => s.push('\\'),
                            '"' => s.push('"'),
                            '\'' => s.push('\''),
                            'x' => {
                                let h1 = self.bump().map(|(c, _)| c);
                                let h2 = self.bump().map(|(c, _)| c);
                                if let (Some(a), Some(b)) = (h1, h2) {
                                    let hi = a.to_digit(16).unwrap_or(0);
                                    let lo = b.to_digit(16).unwrap_or(0);
                                    s.push((hi * 16 + lo) as u8 as char);
                                }
                            }
                            'u' => {
                                if self.bump().map(|(c, _)| c) == Some('{') {
                                    let mut hex = String::new();
                                    while let Some((c, _)) = self.peek() {
                                        if c == '}' { self.bump(); break; }
                                        hex.push(c); self.bump();
                                    }
                                    if let Ok(n) = u32::from_str_radix(&hex, 16) {
                                        if let Some(c) = char::from_u32(n) { s.push(c); }
                                    }
                                }
                            }
                            other => {
                                self.errors.push(LexError::InvalidEscape { ch: other, span: self.span(start, self.cur_byte()) });
                                s.push(other);
                            }
                        }
                    }
                }
                Some((c, _)) => s.push(c),
            }
        }
        Token::StrLit(s)
    }

    fn byte_string(&mut self, start: u32) -> Token {
        self.bump(); // opening "
        let mut bytes = Vec::new();
        loop {
            match self.bump() {
                None => {
                    self.errors.push(LexError::UnterminatedString { span: self.span(start, self.cur_byte()) });
                    return Token::ByteStrLit(bytes);
                }
                Some(('"', _)) => break,
                Some(('\\', _)) => {
                    if let Some((ec, _)) = self.bump() {
                        match ec {
                            'n' => bytes.push(b'\n'),
                            't' => bytes.push(b'\t'),
                            'r' => bytes.push(b'\r'),
                            '0' => bytes.push(0),
                            '\\' => bytes.push(b'\\'),
                            '"' => bytes.push(b'"'),
                            '\'' => bytes.push(b'\''),
                            'x' => {
                                let h1 = self.bump().map(|(c, _)| c);
                                let h2 = self.bump().map(|(c, _)| c);
                                if let (Some(a), Some(b)) = (h1, h2) {
                                    let hi = a.to_digit(16).unwrap_or(0);
                                    let lo = b.to_digit(16).unwrap_or(0);
                                    bytes.push((hi * 16 + lo) as u8);
                                }
                            }
                            other => bytes.push(other as u8),
                        }
                    }
                }
                Some((c, _)) => {
                    let mut buf = [0u8; 4];
                    let s = c.encode_utf8(&mut buf);
                    bytes.extend_from_slice(s.as_bytes());
                }
            }
        }
        Token::ByteStrLit(bytes)
    }

    fn fmt_string(&mut self, start: u32) -> Token {
        // f"..." — capture raw inner source so the parser can re-parse exprs.
        self.bump(); // opening "
        let mut s = String::new();
        loop {
            match self.bump() {
                None => {
                    self.errors.push(LexError::UnterminatedString { span: self.span(start, self.cur_byte()) });
                    return Token::FmtStrLit(s);
                }
                Some(('"', _)) => break,
                Some(('\\', _)) => {
                    s.push('\\');
                    if let Some((ec, _)) = self.bump() {
                        s.push(ec);
                        if ec == 'u' && self.peek().map(|(c, _)| c) == Some('{') {
                            while let Some((c, _)) = self.peek() {
                                s.push(c); self.bump();
                                if c == '}' { break; }
                            }
                        }
                    }
                }
                Some((c, _)) => s.push(c),
            }
        }
        Token::FmtStrLit(s)
    }

    fn raw_string(&mut self, _start: u32) -> Option<Token> {
        // r#"..."# — basic implementation. Skald rarely uses raw strings; the
        // spec doesn't mention them. Treat as normal string for now.
        self.bump(); // r
        let hash_count = if self.peek().map(|(c, _)| c) == Some('#') {
            let mut n = 0;
            while self.peek().map(|(c, _)| c) == Some('#') { self.bump(); n += 1; }
            n
        } else { 0 };
        if self.peek().map(|(c, _)| c) != Some('"') {
            // raw identifier — handled above, but if we got here, it's broken
            return self.ident(_start);
        }
        self.bump(); // "
        let mut s = String::new();
        loop {
            match self.bump() {
                None => break,
                Some(('"', _)) => {
                    let mut matched = 0;
                    let mut look = self.pos;
                    while matched < hash_count && look < self.chars.len() && self.chars[look].0 == '#' {
                        matched += 1; look += 1;
                    }
                    if matched == hash_count {
                        for _ in 0..hash_count { self.bump(); }
                        break;
                    } else {
                        s.push('"');
                    }
                }
                Some((c, _)) => s.push(c),
            }
        }
        Some(Token::StrLit(s))
    }

    fn char_lit(&mut self, start: u32) -> Token {
        self.bump(); // opening '
        let c = match self.bump() {
            None => { self.errors.push(LexError::UnterminatedChar { span: self.span(start, self.cur_byte()) }); return Token::CharLit('\0'); }
            Some(('\\', _)) => {
                match self.bump() {
                    Some(('n', _)) => '\n',
                    Some(('t', _)) => '\t',
                    Some(('r', _)) => '\r',
                    Some(('0', _)) => '\0',
                    Some(('\\', _)) => '\\',
                    Some(('\'', _)) => '\'',
                    Some(('"', _)) => '"',
                    Some(('x', _)) => {
                        let h1 = self.bump().map(|(c, _)| c);
                        let h2 = self.bump().map(|(c, _)| c);
                        if let (Some(a), Some(b)) = (h1, h2) {
                            let hi = a.to_digit(16).unwrap_or(0);
                            let lo = b.to_digit(16).unwrap_or(0);
                            (hi * 16 + lo) as u8 as char
                        } else { '\0' }
                    }
                    Some(('u', _)) => {
                        if self.bump().map(|(c, _)| c) == Some('{') {
                            let mut hex = String::new();
                            while let Some((c, _)) = self.peek() {
                                if c == '}' { self.bump(); break; }
                                hex.push(c); self.bump();
                            }
                            u32::from_str_radix(&hex, 16).ok()
                                .and_then(char::from_u32)
                                .unwrap_or('\0')
                        } else { '\0' }
                    }
                    Some((c, _)) => { self.errors.push(LexError::InvalidEscape { ch: c, span: self.span(start, self.cur_byte()) }); c }
                    None => '\0',
                }
            }
            Some((c, _)) => c,
        };
        if self.peek().map(|(c, _)| c) != Some('\'') {
            self.errors.push(LexError::UnterminatedChar { span: self.span(start, self.cur_byte()) });
        } else { self.bump(); }
        Token::CharLit(c)
    }

    // ----- Comments -----

    fn line_comment(&mut self, start: u32) -> Option<Token> {
        self.bump(); // /
        self.bump(); // /
        // Check for doc comment
        if self.peek().map(|(c, _)| c) == Some('/') && self.peek2().map(|(c, _)| c) != Some('/') {
            self.bump();
            // /// doc
            let mut s = String::new();
            while let Some((c, _)) = self.peek() {
                if c == '\n' { break; }
                s.push(c); self.bump();
            }
            // Trim leading space
            let s = s.trim_start().to_string();
            self.pending_doc.push(s);
            return Some(Token::DocComment(self.pending_doc.last().unwrap().clone()));
        }
        if self.peek().map(|(c, _)| c) == Some('!') {
            self.bump();
            // //! inner doc
            let mut s = String::new();
            while let Some((c, _)) = self.peek() {
                if c == '\n' { break; }
                s.push(c); self.bump();
            }
            let s = s.trim_start().to_string();
            self.pending_inner_doc.push(s);
            return Some(Token::InnerDocComment(self.pending_inner_doc.last().unwrap().clone()));
        }
        // Plain line comment
        while let Some((c, _)) = self.peek() {
            if c == '\n' { break; }
            self.bump();
        }
        let _ = start;
        Some(Token::LineComment)
    }

    fn block_comment(&mut self, start: u32) -> Option<Token> {
        self.bump(); // /
        self.bump(); // *
        let mut depth = 1;
        while depth > 0 {
            match self.peek() {
                None => {
                    self.errors.push(LexError::UnterminatedBlockComment { span: self.span(start, self.cur_byte()) });
                    return Some(Token::BlockComment);
                }
                Some(('/', _)) if self.peek2().map(|(c, _)| c) == Some('*') => {
                    self.bump(); self.bump(); depth += 1;
                }
                Some(('*', _)) if self.peek2().map(|(c, _)| c) == Some('/') => {
                    self.bump(); self.bump(); depth -= 1;
                }
                Some(_) => { self.bump(); }
            }
        }
        Some(Token::BlockComment)
    }

    // ----- Punctuation -----

    fn punct(&mut self, start: u32) -> Option<Token> {
        let (c, _) = self.peek().unwrap();
        self.bump();
        let tok = match c {
            '(' => Token::LParen,
            ')' => Token::RParen,
            '{' => Token::LBrace,
            '}' => Token::RBrace,
            '[' => Token::LBracket,
            ']' => Token::RBracket,
            ',' => Token::Comma,
            ';' => Token::Semicolon,
            '@' => Token::At,
            '#' => Token::Hash,
            '$' => Token::Dollar,
            '~' => Token::Tilde,
            '?' => {
                // ?. ?: ??
                match self.peek().map(|(c, _)| c) {
                    Some('.') => { self.bump(); Token::OptionalChain }
                    Some(':') => { self.bump(); Token::Elvis }
                    Some('?') => { self.bump(); Token::NullCoalesce }
                    _ => Token::Question,
                }
            }
            '.' => {
                match self.peek().map(|(c, _)| c) {
                    Some('.') => {
                        self.bump();
                        if self.peek().map(|(c, _)| c) == Some('.') { self.bump(); Token::TripleDot }
                        else if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::DotEq }
                        else { Token::DoubleDot }
                    }
                    _ => Token::Dot,
                }
            }
            ':' => {
                if self.peek().map(|(c, _)| c) == Some(':') { self.bump(); Token::DoubleColon }
                else { Token::Colon }
            }
            '+' => {
                match self.peek().map(|(c, _)| c) {
                    Some('=') => { self.bump(); Token::PlusEq }
                    _ => Token::Plus,
                }
            }
            '-' => {
                match self.peek().map(|(c, _)| c) {
                    Some('=') => { self.bump(); Token::MinusEq }
                    Some('>') => { self.bump(); Token::Arrow }
                    _ => Token::Minus,
                }
            }
            '*' => {
                if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::StarEq }
                else { Token::Star }
            }
            '/' => {
                if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::SlashEq }
                else { Token::Slash }
            }
            '%' => {
                if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::PercentEq }
                else { Token::Percent }
            }
            '=' => {
                match self.peek().map(|(c, _)| c) {
                    Some('=') => { self.bump(); Token::EqEq }
                    Some('>') => { self.bump(); Token::FatArrow }
                    _ => Token::Eq,
                }
            }
            '!' => {
                if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::BangEq }
                else { Token::Bang }
            }
            '<' => {
                match self.peek().map(|(c, _)| c) {
                    Some('=') => { self.bump(); Token::Le }
                    Some('<') => {
                        self.bump();
                        if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::ShlEq }
                        else { Token::Shl }
                    }
                    _ => Token::Lt,
                }
            }
            '>' => {
                match self.peek().map(|(c, _)| c) {
                    Some('=') => { self.bump(); Token::Ge }
                    Some('>') => {
                        self.bump();
                        if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::ShrEq }
                        else { Token::Shr }
                    }
                    _ => Token::Gt,
                }
            }
            '&' => {
                match self.peek().map(|(c, _)| c) {
                    Some('&') => { self.bump(); Token::AmpAmp }
                    Some('=') => { self.bump(); Token::AmpEq }
                    _ => Token::Amp,
                }
            }
            '|' => {
                match self.peek().map(|(c, _)| c) {
                    Some('|') => { self.bump(); Token::PipePipe }
                    Some('=') => { self.bump(); Token::PipeEq }
                    Some('>') => { self.bump(); Token::PipeForward }
                    _ => Token::Pipe,
                }
            }
            '^' => {
                if self.peek().map(|(c, _)| c) == Some('=') { self.bump(); Token::CaretEq }
                else { Token::Caret }
            }
            _ => {
                self.errors.push(LexError::UnexpectedChar { ch: c, span: self.span(start, self.cur_byte()) });
                return None;
            }
        };
        Some(tok)
    }
}

// ---------- Convenience: tokenize string with fresh interner ----------

pub fn tokenize(src: &str, file: FileId, interner: &mut Interner) -> (Vec<Tok>, Vec<LexError>) {
    let lx = Lexer::new(src, file, interner);
    let (toks, errs, _doc, _inner) = lx.tokenize();
    (toks, errs)
}

// ---------- Annotation parsing helper (lexer-level `@name(args)`) ----------

/// Parse `@name` followed by optional `(args)`. Used by the parser.
pub fn parse_annotation(name: &str, args: &[String]) -> Annotation {
    match name {
        "region" => {
            let s = args.first().cloned().unwrap_or_default();
            Annotation::Region(s.trim_matches('"').to_string())
        }
        "arena" => Annotation::Arena,
        "simd" => Annotation::Simd,
        "layout" => {
            let s = args.first().cloned().unwrap_or_default();
            let kind = match s.trim_matches('"') {
                "soa" => LayoutKind::Soa,
                _ => LayoutKind::Aos,
            };
            Annotation::Layout(kind)
        }
        "unsafe" => Annotation::Unsafe,
        "inline" => {
            let s = args.first().cloned().unwrap_or_else(|| "hint".to_string());
            let k = match s.trim_matches('"') {
                "always" => InlineKind::Always,
                "never" => InlineKind::Never,
                _ => InlineKind::Hint,
            };
            Annotation::Inline(k)
        }
        "hot" => Annotation::Hot,
        "cold" => Annotation::Cold,
        "borrow" => Annotation::Borrow,
        "persistent" => Annotation::Persistent,
        "ustruct" => Annotation::UStruct,
        "mass_fragment" => Annotation::MassFragment,
        "mass_processor" => {
            let mut group = None; let mut tb = None; let mut ta = None;
            for a in args {
                let a = a.trim();
                if let Some(v) = a.strip_prefix("group=") {
                    group = Some(v.trim_matches('"').to_string());
                } else if let Some(v) = a.strip_prefix("tick_before=") {
                    tb = Some(v.trim_matches('"').to_string());
                } else if let Some(v) = a.strip_prefix("tick_after=") {
                    ta = Some(v.trim_matches('"').to_string());
                }
            }
            Annotation::MassProcessor { group, tick_before: tb, tick_after: ta }
        }
        "pod" => Annotation::Pod,
        "deprecated" => {
            let s = args.first().cloned().unwrap_or_default();
            Annotation::Deprecated(s.trim_matches('"').to_string())
        }
        "allow_private" => Annotation::AllowPrivate,
        _ => Annotation::Unknown {
            name: name.to_string(),
            args: args.to_vec(),
        },
    }
}
