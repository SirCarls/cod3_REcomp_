//! The zone layout file: what a fastfile loader reads, structure by
//! structure. [`Layout::parse`] turns it into [`StructDef`]s the zone reader
//! (`zone.rs`) follows. The reader holds the loader's rules (blocks, pointers,
//! references, aliases); a layout says which structures there are and what the
//! loader does with each of their members, so the code holds no game's
//! structure layouts. World at War PC 1.7's layout is kept with the project's
//! private notes (it was made with a type header that is not published, see
//! docs/formats/waw.md) and is given to the reader by its user (`rip --layout`).
//!
//! ```text
//! # a comment (to the end of the line)
//! asset <type number> <type name> <Structure> [name=<member path>] [block=none]
//!     an asset type: the structure the loader reads for it (into the TEMP
//!     block, then copied to its pool), the member that names it (default
//!     "name"); block=none: read where it is, without the TEMP block
//! struct <Name> <size> [align=<n>]   a structure ("union" for a union)
//!   <offset> <member>[<n>]... <kind> [options]
//! ```
//!
//! The lines under a structure are the integer fields its counts and
//! conditions read, then what the loader does, in the loader's order. Kinds:
//!
//! - `u8 i8 u16 i16 u32 i32`: an integer field (`member[n]` for an array).
//! - `string`: a `char*`, a NUL-terminated string or a reference to one.
//! - `sound`: a sound by name (a 4-byte slot holding the name's pointer, then
//!   the name).
//! - `asset:<type name>`: a pointer to an asset, inline (-1, or -2 with a slot
//!   for it) or a reference to a slot holding one.
//! - `ptr <element>`: a pointer to `count` elements: `u8` ... `i32`,
//!   `bytes:<n>`, `struct:<Name>`, `asset:<type>`, `string`, `sound`, or
//!   `ptr <element>` (a pointer to pointers).
//! - `handle struct:<Name>`: like an asset without being one.
//! - `inline <Name>`: a structure inside this one.
//! - `tail <element>`: the structure's variable-length end, read with it.
//! - `custom:<name>`: a loader written out in the reader, for shapes the
//!   kinds above cannot say ([`CUSTOM_LOADERS`]).
//!
//! Options: `count=<expr>` (a comma list: one per element of an embedded
//! array), `reusable` (the pointer may refer to data loaded before),
//! `align=<n>`, `block=temp|runtime|virtual|large`, `when=<expr>` (a union
//! member, decided in the structure holding the union), `order=<i,j,...>`
//! (the order the elements are read in). Any other `<key>=<expr>[,<expr>]` on
//! a `custom:` member is an argument for its loader, evaluated in the
//! structure holding it. Expressions are numbers, member paths (`a`, `a[2]`,
//! `a.b`, `Owner::a` for an enclosing structure), `+ - * / % << >> & | == !=
//! < <= > >= && || !` and parentheses, written without spaces.

use std::collections::HashMap;
use std::fmt;

/// The loaders a layout can name (`custom:<name>`) and the arguments each takes.
///
/// - `xanim_indices count=<n> wide=<cond>`: an index table, `n` bytes, or `n`
///   2-aligned shorts when `wide` holds.
/// - `xanim_delta_part wide=<cond>`: an animation's delta part (translation
///   and rotation, each with its frame index table; shorts when `wide` holds).
/// - `weapon_accuracy_graphs count=<n0>,<n1>,... tables=<offset>,<offset>`:
///   the member is an array of string slots; for each, its string, then from
///   each table (a pointer array at that offset in the structure) a pointer
///   to `n_i` 8-byte entries, 4-aligned.
pub const CUSTOM_LOADERS: [&str; 3] = [
    "xanim_indices",
    "xanim_delta_part",
    "weapon_accuracy_graphs",
];

/// Memory blocks, in the game's order (`XFILE_BLOCK_*`).
pub const TEMP: usize = 0;
pub const RUNTIME: usize = 1;
pub const VIRTUAL: usize = 4;
pub const LARGE: usize = 5;

/// A mistake in a layout file, with its line number.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LayoutError {
    pub line: usize,
    pub message: String,
}

impl fmt::Display for LayoutError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "layout line {}: {}", self.line, self.message)
    }
}

impl std::error::Error for LayoutError {}

/// An integer field's type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Int {
    U8,
    I8,
    U16,
    I16,
    U32,
    I32,
}

impl Int {
    fn parse(word: &str) -> Option<Self> {
        Some(match word {
            "u8" => Int::U8,
            "i8" => Int::I8,
            "u16" => Int::U16,
            "i16" => Int::I16,
            "u32" => Int::U32,
            "i32" => Int::I32,
            _ => return None,
        })
    }

    pub fn size(self) -> u32 {
        match self {
            Int::U8 | Int::I8 => 1,
            Int::U16 | Int::I16 => 2,
            Int::U32 | Int::I32 => 4,
        }
    }

    /// The value of `bytes` (little-endian, `size()` long).
    pub fn value(self, bytes: &[u8]) -> i64 {
        match self {
            Int::U8 => bytes[0] as i64,
            Int::I8 => bytes[0] as i8 as i64,
            Int::U16 => u16::from_le_bytes([bytes[0], bytes[1]]) as i64,
            Int::I16 => i16::from_le_bytes([bytes[0], bytes[1]]) as i64,
            Int::U32 => u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as i64,
            Int::I32 => i32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as i64,
        }
    }
}

/// A field the counts and conditions read.
#[derive(Debug, Clone)]
pub struct Field {
    pub offset: u32,
    pub int: Int,
}

/// What a pointer points at, or what a slot holds.
#[derive(Debug, Clone)]
pub enum Element {
    Int(Int),
    Bytes(u32),
    Struct(String),
    Asset(u32),
    Str,
    Sound,
    /// A pointer to one such element (a pointer array's entries).
    Ptr(Box<Element>),
}

/// What the loader does with a member.
#[derive(Debug, Clone)]
pub enum Kind {
    /// A `char*`: a string, or a reference to one.
    Str,
    /// A sound by name (`Load_snd_alias_list_tPtr`).
    Sound,
    /// A pointer to an asset of this type.
    Asset(u32),
    /// A pointer to `count` elements.
    Ptr(Element),
    /// An asset-like pointer that is not an asset (an image's load definition).
    Handle(String),
    /// A structure inside this one.
    Inline(String),
    /// The structure's variable-length end, read with it.
    Tail(Element),
    /// A loader written out in the reader.
    Custom(String),
}

/// One thing the loader does, in order.
#[derive(Debug, Clone)]
pub struct Action {
    pub name: String,
    pub offset: u32,
    /// Embedded array length (1 for a single slot or structure).
    pub count: u32,
    pub kind: Kind,
    /// Count expressions: none (one element), one, or one per embedded element.
    pub counts: Vec<Expr>,
    pub reusable: bool,
    pub align: u32,
    pub block: Option<usize>,
    pub when: Option<Expr>,
    pub order: Option<Vec<u32>>,
    /// A custom loader's arguments, by name.
    pub args: Vec<(String, Vec<Expr>)>,
}

/// A structure (or union) of the zone.
#[derive(Debug, Clone)]
pub struct StructDef {
    pub name: String,
    pub size: u32,
    pub align: u32,
    pub union: bool,
    pub fields: HashMap<String, Field>,
    pub actions: Vec<Action>,
    /// Index into `actions` of the variable-length tail, if any.
    pub tail: Option<usize>,
}

/// An asset type: its structure and the member naming it.
#[derive(Debug, Clone)]
pub struct AssetDef {
    pub type_number: u32,
    pub name: String,
    pub structure: String,
    pub name_path: String,
    /// Loaded where it is, without the TEMP block (`Load_StringTablePtr`).
    pub in_place: bool,
}

/// A parsed layout file.
#[derive(Debug, Clone)]
pub struct Layout {
    pub structs: HashMap<String, StructDef>,
    pub assets: HashMap<u32, AssetDef>,
}

/// A count or condition: numbers, field paths and C operators.
#[derive(Debug, Clone, PartialEq)]
pub enum Expr {
    Num(i64),
    /// `Owner::a[i].b`: the owner structure (if named) and the segments.
    Path(Option<String>, Vec<(String, Option<Box<Expr>>)>),
    Not(Box<Expr>),
    Neg(Box<Expr>),
    Binary(Op, Box<Expr>, Box<Expr>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Op {
    Or,
    And,
    BitOr,
    BitAnd,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Shl,
    Shr,
    Add,
    Sub,
    Mul,
    Div,
    Rem,
}

impl Op {
    pub fn apply(self, a: i64, b: i64) -> i64 {
        match self {
            Op::Or => ((a != 0) || (b != 0)) as i64,
            Op::And => ((a != 0) && (b != 0)) as i64,
            Op::BitOr => a | b,
            Op::BitAnd => a & b,
            Op::Eq => (a == b) as i64,
            Op::Ne => (a != b) as i64,
            Op::Lt => (a < b) as i64,
            Op::Le => (a <= b) as i64,
            Op::Gt => (a > b) as i64,
            Op::Ge => (a >= b) as i64,
            Op::Shl => a.checked_shl(b.clamp(0, 63) as u32).unwrap_or(0),
            Op::Shr => a.checked_shr(b.clamp(0, 63) as u32).unwrap_or(0),
            Op::Add => a.wrapping_add(b),
            Op::Sub => a.wrapping_sub(b),
            Op::Mul => a.wrapping_mul(b),
            Op::Div => a.checked_div(b).unwrap_or(0),
            Op::Rem => a.checked_rem(b).unwrap_or(0),
        }
    }
}

/// Operators by precedence, lowest first.
const LEVELS: &[&[(&str, Op)]] = &[
    &[("||", Op::Or)],
    &[("&&", Op::And)],
    &[("|", Op::BitOr)],
    &[("&", Op::BitAnd)],
    &[("==", Op::Eq), ("!=", Op::Ne)],
    &[("<", Op::Lt), ("<=", Op::Le), (">", Op::Gt), (">=", Op::Ge)],
    &[("<<", Op::Shl), (">>", Op::Shr)],
    &[("+", Op::Add), ("-", Op::Sub)],
    &[("*", Op::Mul), ("/", Op::Div), ("%", Op::Rem)],
];

impl Expr {
    /// Parses an expression written without spaces (`(numBones-numRootBones)*4`).
    pub fn parse(text: &str) -> Result<Expr, String> {
        let tokens = tokenize(text)?;
        let (e, used) =
            parse_level(&tokens, 0, 0).ok_or_else(|| format!("bad expression {text}"))?;
        if used != tokens.len() {
            return Err(format!("bad expression {text}"));
        }
        Ok(e)
    }
}

fn tokenize(text: &str) -> Result<Vec<String>, String> {
    let b = text.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        let start = i;
        if c.is_ascii_digit() {
            i += 1;
            while i < b.len() && b[i].is_ascii_alphanumeric() {
                i += 1;
            }
        } else if c.is_ascii_alphabetic() || c == b'_' {
            while i < b.len() && (b[i].is_ascii_alphanumeric() || b[i] == b'_' || b[i] == b':') {
                i += 1;
            }
        } else {
            let two = text.get(i..i + 2).unwrap_or("");
            i += if ["<<", ">>", "<=", ">=", "==", "!=", "&&", "||"].contains(&two) {
                2
            } else if b"+-*/%&|<>!()[].".contains(&c) {
                1
            } else {
                return Err(format!("bad character in expression {text}"));
            };
        }
        out.push(text[start..i].to_string());
    }
    Ok(out)
}

fn parse_level(t: &[String], i: usize, level: usize) -> Option<(Expr, usize)> {
    if level == LEVELS.len() {
        return parse_unary(t, i);
    }
    let (mut left, mut i) = parse_level(t, i, level + 1)?;
    while let Some(&(_, op)) = t
        .get(i)
        .and_then(|tok| LEVELS[level].iter().find(|(s, _)| s == tok))
    {
        let (right, next) = parse_level(t, i + 1, level + 1)?;
        left = Expr::Binary(op, Box::new(left), Box::new(right));
        i = next;
    }
    Some((left, i))
}

fn parse_unary(t: &[String], i: usize) -> Option<(Expr, usize)> {
    let tok = t.get(i)?;
    match tok.as_str() {
        "(" => {
            let (e, i) = parse_level(t, i + 1, 0)?;
            (t.get(i)? == ")").then_some((e, i + 1))
        }
        "!" => parse_unary(t, i + 1).map(|(e, i)| (Expr::Not(Box::new(e)), i)),
        "-" => parse_unary(t, i + 1).map(|(e, i)| (Expr::Neg(Box::new(e)), i)),
        _ if tok.as_bytes()[0].is_ascii_digit() => {
            let n = match tok.strip_prefix("0x") {
                Some(hex) => i64::from_str_radix(hex, 16).ok()?,
                None => tok.parse().ok()?,
            };
            Some((Expr::Num(n), i + 1))
        }
        _ => {
            let (owner, mut name) = match tok.split_once("::") {
                Some((o, n)) => (Some(o.to_string()), n.to_string()),
                None => (None, tok.clone()),
            };
            let mut segments = Vec::new();
            let mut i = i + 1;
            loop {
                let mut index = None;
                if t.get(i).map(String::as_str) == Some("[") {
                    let (e, next) = parse_level(t, i + 1, 0)?;
                    (t.get(next)? == "]").then_some(())?;
                    index = Some(Box::new(e));
                    i = next + 1;
                }
                segments.push((name, index));
                if t.get(i).map(String::as_str) == Some(".") {
                    name = t.get(i + 1)?.clone();
                    i += 2;
                    continue;
                }
                return Some((Expr::Path(owner, segments), i));
            }
        }
    }
}

fn number(word: &str) -> Result<u32, String> {
    let n = match word.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16),
        None => word.parse(),
    };
    n.map_err(|_| format!("not a number: {word}"))
}

fn block(word: &str) -> Result<usize, String> {
    Ok(match word {
        "temp" => TEMP,
        "runtime" => RUNTIME,
        "virtual" => VIRTUAL,
        "large" => LARGE,
        _ => return Err(format!("unknown block {word}")),
    })
}

impl Layout {
    pub fn parse(text: &str) -> Result<Layout, LayoutError> {
        let mut layout = Layout {
            structs: HashMap::new(),
            assets: HashMap::new(),
        };
        let mut type_numbers = HashMap::new();
        let mut current: Option<String> = None;
        // Asset lines first (members name asset types).
        for (n, raw) in text.lines().enumerate() {
            let line = raw.split('#').next().unwrap_or("").trim_end();
            let words: Vec<&str> = line.split_whitespace().collect();
            if words.first() == Some(&"asset") && words.len() >= 4 {
                let at = |message: String| LayoutError {
                    line: n + 1,
                    message,
                };
                let type_number = number(words[1]).map_err(at)?;
                let mut asset = AssetDef {
                    type_number,
                    name: words[2].to_string(),
                    structure: words[3].to_string(),
                    name_path: "name".to_string(),
                    in_place: false,
                };
                for w in &words[4..] {
                    match w.split_once('=') {
                        Some(("name", v)) => asset.name_path = v.to_string(),
                        Some(("block", "none")) => asset.in_place = true,
                        _ => return Err(at(format!("unknown option {w}"))),
                    }
                }
                type_numbers.insert(asset.name.clone(), type_number);
                layout.assets.insert(type_number, asset);
            }
        }
        for (n, raw) in text.lines().enumerate() {
            let line = raw.split('#').next().unwrap_or("").trim_end();
            if line.trim().is_empty() {
                continue;
            }
            let at = |message: String| LayoutError {
                line: n + 1,
                message,
            };
            let words: Vec<&str> = line.split_whitespace().collect();
            match words[0] {
                "asset" => current = None,
                "struct" | "union" if !raw.starts_with(' ') => {
                    let (Some(name), Some(size)) = (words.get(1), words.get(2)) else {
                        return Err(at("a structure needs a name and a size".into()));
                    };
                    let mut align = 4;
                    for w in &words[3..] {
                        match w.split_once('=') {
                            Some(("align", v)) => align = number(v).map_err(at)?,
                            _ => return Err(at(format!("unknown option {w}"))),
                        }
                    }
                    layout.structs.insert(
                        name.to_string(),
                        StructDef {
                            name: name.to_string(),
                            size: number(size).map_err(at)?,
                            align,
                            union: words[0] == "union",
                            fields: HashMap::new(),
                            actions: Vec::new(),
                            tail: None,
                        },
                    );
                    current = Some(name.to_string());
                }
                _ if raw.starts_with(' ') => {
                    let Some(owner) = &current else {
                        return Err(at("a member outside a structure".into()));
                    };
                    let st = layout.structs.get_mut(owner).expect("current structure");
                    member(st, &words, &type_numbers).map_err(at)?;
                }
                _ => return Err(at(format!("not a layout line: {line}"))),
            }
        }
        // Every structure an action names must exist.
        for st in layout.structs.values() {
            for a in &st.actions {
                let named = match &a.kind {
                    Kind::Inline(s) | Kind::Handle(s) => Some(s),
                    Kind::Ptr(e) | Kind::Tail(e) => element_struct(e),
                    _ => None,
                };
                if let Some(s) = named {
                    if !layout.structs.contains_key(s) {
                        return Err(LayoutError {
                            line: 0,
                            message: format!(
                                "{}.{} names {s}, which is not described",
                                st.name, a.name
                            ),
                        });
                    }
                }
            }
        }
        for a in layout.assets.values() {
            if !layout.structs.contains_key(&a.structure) {
                return Err(LayoutError {
                    line: 0,
                    message: format!(
                        "asset {} names {}, which is not described",
                        a.name, a.structure
                    ),
                });
            }
        }
        Ok(layout)
    }

    /// Bytes one element takes.
    pub fn element_size(&self, e: &Element) -> u32 {
        match e {
            Element::Int(i) => i.size(),
            Element::Bytes(n) => *n,
            Element::Struct(s) => self.structs[s].size,
            Element::Asset(_) | Element::Str | Element::Sound | Element::Ptr(_) => 4,
        }
    }

    /// The natural alignment of an element (pointer arrays' entries).
    pub fn element_align(&self, e: &Element) -> u32 {
        match e {
            Element::Int(i) => i.size(),
            Element::Struct(s) => self.structs[s].align,
            _ => 4,
        }
    }
}

fn element_struct(e: &Element) -> Option<&String> {
    match e {
        Element::Struct(s) => Some(s),
        Element::Ptr(inner) => element_struct(inner),
        _ => None,
    }
}

fn element(words: &[&str], types: &HashMap<String, u32>) -> Result<(Element, usize), String> {
    let w = *words.first().ok_or("missing element")?;
    if w == "ptr" {
        let (inner, used) = element(&words[1..], types)?;
        return Ok((Element::Ptr(Box::new(inner)), used + 1));
    }
    let e = if let Some(int) = Int::parse(w) {
        Element::Int(int)
    } else if let Some(n) = w.strip_prefix("bytes:") {
        Element::Bytes(number(n)?)
    } else if let Some(s) = w.strip_prefix("struct:") {
        Element::Struct(s.to_string())
    } else if let Some(t) = w.strip_prefix("asset:") {
        Element::Asset(*types.get(t).ok_or(format!("unknown asset type {t}"))?)
    } else if w == "string" {
        Element::Str
    } else if w == "sound" {
        Element::Sound
    } else {
        return Err(format!("unknown element {w}"));
    };
    Ok((e, 1))
}

fn member(st: &mut StructDef, words: &[&str], types: &HashMap<String, u32>) -> Result<(), String> {
    if words.len() < 3 {
        return Err("a member needs an offset, a name and a kind".into());
    }
    let offset = number(words[0])?;
    let (name, dims) = match words[1].find('[') {
        Some(i) => (&words[1][..i], &words[1][i..]),
        None => (words[1], ""),
    };
    let mut count = 1u32;
    for d in dims.split(['[', ']']).filter(|s| !s.is_empty()) {
        count = count.saturating_mul(number(d)?);
    }
    if let Some(int) = Int::parse(words[2]) {
        st.fields.insert(name.to_string(), Field { offset, int });
        return Ok(());
    }
    let mut rest = &words[3..];
    let kind = match words[2] {
        "string" => Kind::Str,
        "sound" => Kind::Sound,
        "ptr" | "tail" => {
            let (e, used) = element(rest, types)?;
            rest = &rest[used..];
            if words[2] == "ptr" {
                Kind::Ptr(e)
            } else {
                Kind::Tail(e)
            }
        }
        "inline" | "handle" => {
            let s = rest
                .first()
                .ok_or("missing structure")?
                .trim_start_matches("struct:")
                .to_string();
            rest = &rest[1..];
            if words[2] == "inline" {
                Kind::Inline(s)
            } else {
                Kind::Handle(s)
            }
        }
        k => {
            if let Some(t) = k.strip_prefix("asset:") {
                Kind::Asset(*types.get(t).ok_or(format!("unknown asset type {t}"))?)
            } else if let Some(c) = k.strip_prefix("custom:") {
                if !CUSTOM_LOADERS.contains(&c) {
                    return Err(format!("no custom loader {c}"));
                }
                Kind::Custom(c.to_string())
            } else {
                return Err(format!("unknown kind {k}"));
            }
        }
    };
    let mut action = Action {
        name: name.to_string(),
        offset,
        count,
        kind,
        counts: Vec::new(),
        reusable: false,
        align: 1,
        block: None,
        when: None,
        order: None,
        args: Vec::new(),
    };
    for w in rest {
        match w.split_once('=') {
            Some(("count", v)) => {
                action.counts = v.split(',').map(Expr::parse).collect::<Result<_, _>>()?;
            }
            Some(("align", v)) => action.align = number(v)?,
            Some(("block", v)) => action.block = Some(block(v)?),
            Some(("when", v)) => action.when = Some(Expr::parse(v)?),
            Some(("order", v)) => {
                action.order = Some(v.split(',').map(number).collect::<Result<_, _>>()?);
            }
            None if *w == "reusable" => action.reusable = true,
            Some((key, v)) if matches!(action.kind, Kind::Custom(_)) => {
                let values = v.split(',').map(Expr::parse).collect::<Result<_, _>>()?;
                action.args.push((key.to_string(), values));
            }
            _ => return Err(format!("unknown option {w}")),
        }
    }
    if matches!(action.kind, Kind::Tail(_)) {
        st.tail = Some(st.actions.len());
    }
    st.actions.push(action);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expressions() {
        let e = Expr::parse("(count-skipped)*4").unwrap();
        assert!(matches!(e, Expr::Binary(Op::Mul, _, _)));
        let e = Expr::parse("high[axis]-low[axis]+1").unwrap();
        let Expr::Binary(Op::Add, left, _) = e else {
            panic!()
        };
        assert!(matches!(*left, Expr::Binary(Op::Sub, _, _)));
        let e = Expr::parse("Outer::inner.total").unwrap();
        assert_eq!(
            e,
            Expr::Path(
                Some("Outer".into()),
                vec![("inner".into(), None), ("total".into(), None)]
            )
        );
        assert_eq!(Expr::parse("0x2000").unwrap(), Expr::Num(0x2000));
        assert!(Expr::parse("a+").is_err());
        assert!(Expr::parse("a b").is_err());
        // precedence: comparisons below &&, shifts above comparisons
        let e = Expr::parse("type==1||type==7").unwrap();
        assert!(matches!(e, Expr::Binary(Op::Or, _, _)));
    }

    /// Every kind of line the format has.
    const SAMPLE: &str = "\
# a sample, not a game's
asset 1 thing Thing name=info.label
asset 2 table Table block=none
struct Info 0x4
  0x0000 label string
struct Thing 0x20
  0x0004 count u16
  0x0006 flags u8
  0x0008 total[2] i32
  0x0000 info inline Info
  0x0010 parts ptr struct:Part count=count align=4 reusable
  0x0014 names[2] ptr string count=total[0],total[1] order=1,0
  0x0018 extra ptr u32 count=count block=runtime align=4
  0x001c table asset:table
  0x0010 graphs[2] custom:weapon_accuracy_graphs count=total[0],total[1] tables=0x18,0x1c
union Choice 0x4
  0x0000 thing asset:thing when=Holder::kind==1
  0x0000 bytes ptr bytes:8 when=Holder::kind!=1 align=4
struct Part 0x8
  0x0004 size u16
  0x0008 entries tail struct:Info count=size
struct Table 0x4
  0x0000 name string
";

    #[test]
    fn every_kind_of_line_parses() {
        let layout = Layout::parse(SAMPLE).unwrap();
        assert_eq!(layout.assets[&1].structure, "Thing");
        assert_eq!(layout.assets[&1].name_path, "info.label");
        assert!(layout.assets[&2].in_place);
        let thing = &layout.structs["Thing"];
        assert_eq!(thing.size, 0x20);
        assert_eq!(thing.fields["total"].offset, 8);
        assert_eq!(thing.actions.len(), 6);
        let graphs = &thing.actions[5];
        assert!(matches!(&graphs.kind, Kind::Custom(name) if name == "weapon_accuracy_graphs"));
        assert_eq!((graphs.count, graphs.counts.len()), (2, 2));
        assert_eq!(graphs.args[0].0, "tables");
        assert_eq!(graphs.args[0].1, vec![Expr::Num(0x18), Expr::Num(0x1c)]);
        assert!(thing.actions[1].reusable);
        assert_eq!(thing.actions[2].order, Some(vec![1, 0]));
        assert_eq!(thing.actions[3].block, Some(RUNTIME));
        assert!(layout.structs["Choice"].union);
        assert!(layout.structs["Part"].tail.is_some());
    }

    #[test]
    fn mistakes_name_their_line() {
        let err = Layout::parse("struct A 0x4\n  0x0 x ptr nonsense\n").unwrap_err();
        assert_eq!(err.line, 2);
        let err = Layout::parse("asset 1 a Missing\n").unwrap_err();
        assert!(err.message.contains("Missing"));
        // arguments are for custom loaders only, and the loader must exist
        let err = Layout::parse("struct A 0x4\n  0x0 x string tables=1\n").unwrap_err();
        assert!(err.message.contains("tables=1"));
        let err = Layout::parse("struct A 0x4\n  0x0 x custom:nothing\n").unwrap_err();
        assert!(err.message.contains("nothing"));
    }
}
