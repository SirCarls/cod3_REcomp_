//! Reads every asset of a World at War zone the way the game's loader does
//! (`DB_LoadXFileInternal` 0x0047B300 in CoDWaW.exe 1.7 and the generated
//! `Load_*` functions), following the layout file (`layout.rs`).
//!
//! Memory works like the game's: seven blocks (TEMP, RUNTIME, LARGE_RUNTIME,
//! PHYSICAL_RUNTIME, VIRTUAL, LARGE, PHYSICAL) and the asset pools. A
//! pointer is `((block << 29) | offset) + 1`, the file's own encoding of a
//! reference, so a reference in the file is already a pointer here:
//!
//! - `Load_Stream` (0x00490310) reads into the current block, leaves RUNTIME
//!   memory zero without reading, and reads blocks 2 and 3 at the end (none
//!   of the PC game's zones uses them);
//! - `DB_PushStreamPos` / `DB_PopStreamPos` (0x00490210, 0x00490280) switch
//!   blocks; popping out of TEMP frees what was put there;
//! - an asset (`Load_<T>Ptr`, 0x00480840 `Load_XModelPtr` is the pattern) goes
//!   to TEMP and is copied to its pool; -2 also reserves a VIRTUAL slot that
//!   will hold it (`DB_InsertPointer` 0x004902E0); any other value is a
//!   reference to a slot holding one.
//!
//! [`Reader::run`] checks what the zone allows: the stream ends at the zone's
//! end, each block ends at the size the zone header gives, and every
//! reference lands on something loaded before.

use std::collections::{BTreeMap, HashSet};
use std::fmt;

use crate::layout::{
    Action, AssetDef, Element, Expr, Kind, Layout, StructDef, LARGE, RUNTIME, TEMP, VIRTUAL,
};

const BLOCKS: usize = 7;
const POOL: usize = 7;
const INLINE: u32 = 0xFFFF_FFFF;
const INSERT: u32 = 0xFFFF_FFFE;
/// The zone's header: size, external size, seven block sizes.
const HEADER_SIZE: usize = 36;

/// Why a zone can't be read on.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ZoneError {
    /// The stream ends before what the loader reads.
    StreamEnds { wanted: u64, at: usize, size: usize },
    /// Memory beyond a block's size.
    BlockTooSmall {
        block: usize,
        end: u64,
        size: usize,
        at: String,
    },
    /// An asset type the game has no loader for.
    NoLoader { type_number: u32, at: String },
    /// A field or structure the layout doesn't describe.
    Layout(String),
}

impl fmt::Display for ZoneError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ZoneError::StreamEnds { wanted, at, size } => {
                write!(
                    f,
                    "the stream ends early: {wanted:#x} bytes wanted at {at:#x} of {size:#x}"
                )
            }
            ZoneError::BlockTooSmall {
                block,
                end,
                size,
                at,
            } => {
                write!(
                    f,
                    "block {block} too small: {end:#x} past its {size:#x} bytes (at {at})"
                )
            }
            ZoneError::NoLoader { type_number, at } => {
                write!(
                    f,
                    "asset type {type_number} has no loader in World at War (at {at})"
                )
            }
            ZoneError::Layout(message) => write!(f, "{message}"),
        }
    }
}

impl std::error::Error for ZoneError {}

type Result<T> = std::result::Result<T, ZoneError>;

/// One loaded asset.
#[derive(Debug, Clone)]
pub struct LoadedAsset {
    pub type_number: u32,
    pub name: String,
    /// Where its structure is (in the pools).
    pub address: u32,
}

/// What reading a zone found.
#[derive(Debug)]
pub struct LoadedZone {
    pub assets: Vec<LoadedAsset>,
    /// What the checks found (empty when the zone reads exactly).
    pub problems: Vec<String>,
    pub script_strings: Vec<Option<String>>,
    /// Bytes of the zone read.
    pub stream_read: usize,
    memory: Vec<Vec<u8>>,
}

impl LoadedZone {
    /// `size` bytes at a pointer (`None` past the memory loaded).
    pub fn bytes(&self, address: u32, size: usize) -> Option<&[u8]> {
        let (block, offset) = unptr(address);
        self.memory.get(block)?.get(offset..offset + size)
    }

    pub fn u32(&self, address: u32) -> Option<u32> {
        self.bytes(address, 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    /// The NUL-terminated string at a pointer.
    pub fn string(&self, address: u32) -> Option<String> {
        let (block, offset) = unptr(address);
        let mem = self.memory.get(block)?.get(offset..)?;
        let end = mem.iter().position(|&b| b == 0)?;
        Some(String::from_utf8_lossy(&mem[..end]).into_owned())
    }
}

fn ptr(block: usize, offset: usize) -> u32 {
    (((block as u32) << 29) | offset as u32).wrapping_add(1)
}

fn unptr(value: u32) -> (usize, usize) {
    let v = value.wrapping_sub(1);
    ((v >> 29) as usize, (v & 0x1FFF_FFFF) as usize)
}

/// Reads a zone (PC, little-endian) by its layout.
pub struct Reader<'a> {
    layout: &'a Layout,
    data: &'a [u8],
    read_pos: usize,
    block_sizes: [u32; BLOCKS],
    memory: Vec<Vec<u8>>,
    index: usize,
    pos: usize,
    saved: [usize; BLOCKS],
    stack: Vec<(usize, usize)>,
    delayed: Vec<(u32, usize)>,
    temp_high: usize,
    strings: HashSet<u32>,
    slots: HashSet<u32>,
    starts: HashSet<u32>,
    /// Loaded arrays: start -> (bytes, element size).
    regions: BTreeMap<u32, (u32, u32)>,
    assets: Vec<LoadedAsset>,
    script_strings: Vec<Option<String>>,
    problems: Vec<String>,
    ctx: Vec<(&'a StructDef, u32)>,
}

impl<'a> Reader<'a> {
    pub fn new(layout: &'a Layout, data: &'a [u8]) -> Result<Self> {
        let mut r = Reader {
            layout,
            data,
            read_pos: 0,
            block_sizes: [0; BLOCKS],
            memory: Vec::new(),
            index: TEMP,
            pos: 0,
            saved: [0; BLOCKS],
            stack: Vec::new(),
            delayed: Vec::new(),
            temp_high: 0,
            strings: HashSet::new(),
            slots: HashSet::new(),
            starts: HashSet::new(),
            regions: BTreeMap::new(),
            assets: Vec::new(),
            script_strings: Vec::new(),
            problems: Vec::new(),
            ctx: Vec::new(),
        };
        let header = r.file_read(HEADER_SIZE)?;
        for (i, size) in r.block_sizes.iter_mut().enumerate() {
            let at = 8 + 4 * i;
            *size =
                u32::from_le_bytes([header[at], header[at + 1], header[at + 2], header[at + 3]]);
        }
        r.memory = r.block_sizes.iter().map(|&s| vec![0; s as usize]).collect();
        r.memory.push(Vec::new()); // the pools
        Ok(r)
    }

    // ---- memory -----------------------------------------------------------------------------

    fn span(&mut self, address: u32, size: usize) -> Result<(usize, usize)> {
        let (block, offset) = unptr(address);
        let end = offset as u64 + size as u64;
        let len = self.memory.get(block).map(Vec::len).unwrap_or(0);
        if end > len as u64 {
            if block == POOL {
                self.memory[POOL].resize(end as usize, 0);
            } else {
                return Err(ZoneError::BlockTooSmall {
                    block,
                    end,
                    size: len,
                    at: self.at(),
                });
            }
        }
        Ok((block, offset))
    }

    fn read(&mut self, address: u32, size: usize) -> Result<&[u8]> {
        let (block, offset) = self.span(address, size)?;
        Ok(&self.memory[block][offset..offset + size])
    }

    fn u32(&mut self, address: u32) -> Result<u32> {
        let b = self.read(address, 4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn set_u32(&mut self, address: u32, value: u32) -> Result<()> {
        let (block, offset) = self.span(address, 4)?;
        self.memory[block][offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    // ---- the stream -------------------------------------------------------------------------

    fn file_read(&mut self, size: usize) -> Result<&'a [u8]> {
        let data: &'a [u8] = self.data;
        let end = self.read_pos.checked_add(size).filter(|&e| e <= data.len());
        let Some(end) = end else {
            return Err(ZoneError::StreamEnds {
                wanted: size as u64,
                at: self.read_pos,
                size: data.len(),
            });
        };
        let chunk = &data[self.read_pos..end];
        self.read_pos = end;
        Ok(chunk)
    }

    fn set_index(&mut self, index: usize) {
        if index != self.index {
            self.saved[self.index] = self.pos;
            self.pos = self.saved[index];
            self.index = index;
        }
    }

    fn push(&mut self, index: usize) {
        let old = self.index;
        self.set_index(index);
        self.stack.push((old, self.pos));
    }

    fn pop(&mut self) {
        let (old, pos) = self.stack.pop().expect("pop without push");
        if self.index == TEMP {
            self.temp_high = self.temp_high.max(self.pos);
            self.pos = pos;
        }
        self.set_index(old);
    }

    fn here(&self) -> u32 {
        ptr(self.index, self.pos)
    }

    fn align(&mut self, n: u32) -> u32 {
        if n > 1 {
            let n = n as usize;
            self.pos = (self.pos + n - 1) & !(n - 1);
        }
        self.here()
    }

    /// `Load_Stream`: `size` bytes into the current block.
    fn load(&mut self, address: u32, size: usize) -> Result<()> {
        if size == 0 {
            return Ok(());
        }
        match self.index {
            RUNTIME => {}
            2 | 3 => self.delayed.push((address, size)),
            _ => {
                let (block, offset) = self.span(address, size)?;
                let chunk = self.file_read(size)?;
                self.memory[block][offset..offset + size].copy_from_slice(chunk);
            }
        }
        self.pos += size;
        if self.index == TEMP {
            self.temp_high = self.temp_high.max(self.pos);
        }
        Ok(())
    }

    fn at(&self) -> String {
        let names: Vec<&str> = self.ctx.iter().map(|(s, _)| s.name.as_str()).collect();
        if names.is_empty() {
            "zone".into()
        } else {
            names.join("/")
        }
    }

    // ---- references -------------------------------------------------------------------------

    fn region(&mut self, start: u32, size: u32, elem: u32) {
        self.regions.insert(start, (size, elem.max(1)));
    }

    /// A reference is good when it is the start of something loaded before, or
    /// an element of an array loaded before.
    fn lands(&self, value: u32) -> bool {
        if self.starts.contains(&value) || self.strings.contains(&value) {
            return true;
        }
        match self.regions.range(..=value).next_back() {
            Some((&start, &(size, elem))) => value < start + size && (value - start) % elem == 0,
            None => false,
        }
    }

    fn bad(&mut self, what: &str, value: u32) {
        let at = self.at();
        self.problems.push(format!(
            "{what} reference {value:#x} at {at} lands on nothing loaded"
        ));
    }

    // ---- expressions ------------------------------------------------------------------------

    /// The nearest enclosing structure that is not a union (union conditions read it).
    fn holder(&self) -> Result<(&'a StructDef, u32)> {
        self.ctx
            .iter()
            .rev()
            .find(|(s, _)| !s.union)
            .copied()
            .ok_or_else(|| ZoneError::Layout("no structure around a union".into()))
    }

    fn eval(&mut self, e: &Expr, st: &'a StructDef, address: u32) -> Result<i64> {
        Ok(match e {
            Expr::Num(n) => *n,
            Expr::Path(owner, segments) => self.path(owner.as_deref(), segments, st, address)?,
            Expr::Not(a) => (self.eval(a, st, address)? == 0) as i64,
            Expr::Neg(a) => self.eval(a, st, address)?.wrapping_neg(),
            Expr::Binary(op, a, b) => {
                let a = self.eval(a, st, address)?;
                let b = self.eval(b, st, address)?;
                op.apply(a, b)
            }
        })
    }

    fn path(
        &mut self,
        owner: Option<&str>,
        segments: &[(String, Option<Box<Expr>>)],
        mut st: &'a StructDef,
        mut address: u32,
    ) -> Result<i64> {
        if let Some(owner) = owner {
            let found = self
                .ctx
                .iter()
                .rev()
                .find(|(s, _)| s.name == owner)
                .copied();
            let Some(found) = found else {
                return Err(ZoneError::Layout(format!(
                    "no {owner} around {}",
                    self.at()
                )));
            };
            (st, address) = found;
        }
        let layout = self.layout;
        let mut n = 0;
        while n < segments.len() {
            let (name, index) = &segments[n];
            // a dotted field of a structure the loader doesn't enter ("header.count")
            let dotted: Vec<&str> = segments[n..].iter().map(|(s, _)| s.as_str()).collect();
            let dotted = dotted.join(".");
            let (name, index, last) = if segments.len() - n > 1 && st.fields.contains_key(&dotted) {
                (&dotted, &segments[segments.len() - 1].1, true)
            } else {
                (name, index, n == segments.len() - 1)
            };
            let i = match index {
                Some(e) => self.eval(e, st, address)?,
                None => 0,
            };
            if !last {
                let inline = st
                    .actions
                    .iter()
                    .find(|a| &a.name == name && matches!(a.kind, Kind::Inline(_)));
                let Some(Action {
                    kind: Kind::Inline(inner),
                    offset,
                    ..
                }) = inline
                else {
                    return Err(ZoneError::Layout(format!(
                        "{} has no structure {name}",
                        st.name
                    )));
                };
                let inner = &layout.structs[inner];
                address = address
                    .wrapping_add(*offset)
                    .wrapping_add((i as u32).wrapping_mul(inner.size));
                st = inner;
                n += 1;
                continue;
            }
            let Some(f) = st.fields.get(name) else {
                return Err(ZoneError::Layout(format!(
                    "{} has no field {name} (at {})",
                    st.name,
                    self.at()
                )));
            };
            let size = f.int.size();
            let at = address
                .wrapping_add(f.offset)
                .wrapping_add((i as u32).wrapping_mul(size));
            let int = f.int;
            let bytes = self.read(at, size as usize)?;
            return Ok(int.value(bytes));
        }
        Err(ZoneError::Layout("an empty path".into()))
    }

    fn count(&mut self, e: &Expr, st: &'a StructDef, address: u32) -> Result<usize> {
        // A negative count (a broken file) loads nothing, as the game's signed loop does.
        Ok(self.eval(e, st, address)?.max(0) as usize)
    }

    // ---- the zone ---------------------------------------------------------------------------

    /// Reads the asset list and every asset, then makes the checks.
    pub fn run(mut self) -> Result<LoadedZone> {
        // Load_XAssetListCustom 0x0047B1F0
        let list = self.file_read(16)?;
        let word = |i: usize| u32::from_le_bytes([list[i], list[i + 1], list[i + 2], list[i + 3]]);
        let (script_count, strings, asset_count, assets) = (word(0), word(4), word(8), word(12));
        self.push(VIRTUAL);
        self.push(VIRTUAL);
        if strings != 0 {
            let array = self.align(4);
            self.load(array, 4 * script_count as usize)?;
            for i in 0..script_count {
                let slot = array + 4 * i;
                self.string(slot)?;
                let v = self.u32(slot)?;
                let s = if v != 0 { self.c_string(v) } else { None };
                self.script_strings.push(s);
            }
        }
        self.pop();
        self.pop();
        // DB_LoadXFileInternal 0x0047B300: the asset array, then each asset
        self.push(VIRTUAL);
        if assets != 0 {
            let array = self.align(4);
            self.load(array, 8 * asset_count as usize)?;
            for i in 0..asset_count {
                let kind = self.u32(array + 8 * i)?;
                self.asset(kind, array + 8 * i + 4)?;
            }
        }
        self.pop();
        // FUN_00490380: the delayed blocks' data, in the order it was met
        for (address, size) in std::mem::take(&mut self.delayed) {
            let (block, offset) = self.span(address, size)?;
            let chunk = self.file_read(size)?;
            self.memory[block][offset..offset + size].copy_from_slice(chunk);
        }
        if self.read_pos != self.data.len() {
            self.problems.push(format!(
                "the stream ended at {:#x}, the zone is {:#x}",
                self.read_pos,
                self.data.len()
            ));
        }
        self.saved[self.index] = self.pos;
        for block in [RUNTIME, VIRTUAL, LARGE, 6] {
            if self.saved[block] != self.block_sizes[block] as usize {
                self.problems.push(format!(
                    "block {block} ends at {:#x}, the header says {:#x}",
                    self.saved[block], self.block_sizes[block]
                ));
            }
        }
        if self.temp_high > self.block_sizes[TEMP] as usize {
            self.problems.push(format!(
                "the TEMP block reached {:#x}, the header says {:#x}",
                self.temp_high, self.block_sizes[TEMP]
            ));
        }
        Ok(LoadedZone {
            assets: self.assets,
            problems: self.problems,
            script_strings: self.script_strings,
            stream_read: self.read_pos,
            memory: self.memory,
        })
    }

    fn c_string(&self, address: u32) -> Option<String> {
        let (block, offset) = unptr(address);
        let mem = self.memory.get(block)?.get(offset..)?;
        let end = mem.iter().position(|&b| b == 0)?;
        Some(String::from_utf8_lossy(&mem[..end]).into_owned())
    }

    /// `Load_XString`: NUL-terminated and unaligned, or a reference to one.
    fn string(&mut self, slot: u32) -> Result<()> {
        let value = self.u32(slot)?;
        if value == 0 {
            return Ok(());
        }
        if value != INLINE {
            if !self.strings.contains(&value) {
                self.bad("string", value);
            }
            return Ok(());
        }
        let address = self.here();
        self.set_u32(slot, address)?;
        let rest = &self.data[self.read_pos.min(self.data.len())..];
        let Some(len) = rest.iter().position(|&b| b == 0) else {
            return Err(ZoneError::StreamEnds {
                wanted: rest.len() as u64 + 1,
                at: self.read_pos,
                size: self.data.len(),
            });
        };
        self.load(address, len + 1)?;
        self.strings.insert(address);
        Ok(())
    }

    /// `Load_snd_alias_list_tPtr` 0x0047BAD0: a sound by name, a 4-aligned
    /// slot holding the name's pointer and then the name.
    fn sound(&mut self, slot: u32) -> Result<()> {
        let value = self.u32(slot)?;
        if value == 0 {
            return Ok(());
        }
        if value != INLINE {
            if !self.lands(value) {
                self.bad("sound name", value);
            }
            return Ok(());
        }
        let target = self.align(4);
        self.set_u32(slot, target)?;
        self.starts.insert(target);
        self.load(target, 4)?;
        self.string(target)
    }

    fn insert_pointer(&mut self) -> u32 {
        self.push(VIRTUAL);
        let slot = self.align(4);
        self.pos += 4;
        self.pop();
        slot
    }

    fn asset(&mut self, type_number: u32, slot: u32) -> Result<()> {
        let layout = self.layout;
        let Some(def) = layout.assets.get(&type_number) else {
            return Err(ZoneError::NoLoader {
                type_number,
                at: self.at(),
            });
        };
        let st = &layout.structs[&def.structure];
        if def.in_place {
            // Load_StringTablePtr 0x004896E0: where it is, a plain reference
            let value = self.u32(slot)?;
            if value != INLINE {
                if !self.lands(value) {
                    self.bad(&def.name, value);
                }
                return Ok(());
            }
            let address = self.align(st.align);
            self.set_u32(slot, address)?;
            self.starts.insert(address);
            self.structure(st, address, None)?;
            let pool = self.add_asset(def, st, address)?;
            return self.set_u32(slot, pool);
        }
        self.push(TEMP);
        let value = self.u32(slot)?;
        if value == 0 {
            self.pop();
            return Ok(());
        }
        if value != INLINE && value != INSERT {
            if self.slots.contains(&value) {
                let target = self.u32(value)?;
                self.set_u32(slot, target)?;
            } else {
                self.bad(&def.name, value);
            }
            self.pop();
            return Ok(());
        }
        let address = self.align(st.align);
        self.set_u32(slot, address)?;
        let inserted = if value == INSERT {
            Some(self.insert_pointer())
        } else {
            None
        };
        self.starts.insert(address);
        self.structure(st, address, Some(VIRTUAL))?;
        let pool = self.add_asset(def, st, address)?;
        self.set_u32(slot, pool)?;
        self.slots.insert(slot);
        if let Some(inserted) = inserted {
            self.set_u32(inserted, pool)?;
            self.slots.insert(inserted);
        }
        self.pop();
        Ok(())
    }

    /// `DB_AddXAsset` 0x0048E300: the asset's structure is copied to its pool.
    fn add_asset(&mut self, def: &AssetDef, st: &'a StructDef, address: u32) -> Result<u32> {
        let bytes = self.read(address, st.size as usize)?.to_vec();
        let pool = ptr(POOL, self.memory[POOL].len());
        self.memory[POOL].extend_from_slice(&bytes);
        // the name: an inline path ("info.name", "window.name") ending at a string
        let layout = self.layout;
        let mut owner = st;
        let mut at = pool;
        let parts: Vec<&str> = def.name_path.split('.').collect();
        for part in &parts[..parts.len() - 1] {
            match owner.actions.iter().find(|a| a.name == *part) {
                Some(Action {
                    kind: Kind::Inline(inner),
                    offset,
                    ..
                }) => {
                    owner = &layout.structs[inner];
                    at += offset;
                }
                _ => {
                    return Err(ZoneError::Layout(format!(
                        "{} has no structure {part}",
                        owner.name
                    )))
                }
            }
        }
        let last = parts[parts.len() - 1];
        let name = match owner.actions.iter().find(|a| a.name == last) {
            Some(a) => {
                let v = self.u32(at + a.offset)?;
                if v != 0 {
                    self.c_string(v).unwrap_or_default()
                } else {
                    String::new()
                }
            }
            None => String::new(),
        };
        self.assets.push(LoadedAsset {
            type_number: def.type_number,
            name,
            address: pool,
        });
        Ok(pool)
    }

    // ---- structures -------------------------------------------------------------------------

    /// `Load_<T>`: the structure's bytes (its tail after the fixed part), then
    /// what it follows (an asset's in VIRTUAL).
    fn structure(&mut self, st: &'a StructDef, address: u32, push: Option<usize>) -> Result<()> {
        self.ctx.push((st, address));
        self.load(address, st.size as usize)?;
        if let Some(t) = st.tail {
            let tail = &st.actions[t];
            let Kind::Tail(e) = &tail.kind else {
                unreachable!()
            };
            let n = match tail.counts.first() {
                Some(c) => self.count(c, st, address)?,
                None => 1,
            };
            let size = self.layout.element_size(e) as usize;
            self.load(address + tail.offset, n * size)?;
        }
        if let Some(block) = push {
            self.push(block);
        }
        self.members(st, address)?;
        if push.is_some() {
            self.pop();
        }
        self.ctx.pop();
        Ok(())
    }

    fn members(&mut self, st: &'a StructDef, address: u32) -> Result<()> {
        for a in &st.actions {
            if let Some(when) = &a.when {
                let (hs, ha) = if st.union {
                    self.holder()?
                } else {
                    (st, address)
                };
                if self.eval(when, hs, ha)? == 0 {
                    continue;
                }
            }
            self.action(st, address, a)?;
        }
        Ok(())
    }

    fn elements(&mut self, inner: &'a StructDef, at: u32, n: usize) -> Result<()> {
        for i in 0..n {
            let address = at + (i as u32) * inner.size;
            self.ctx.push((inner, address));
            self.members(inner, address)?;
            self.ctx.pop();
        }
        Ok(())
    }

    fn action(&mut self, st: &'a StructDef, address: u32, a: &'a Action) -> Result<()> {
        let layout = self.layout;
        let at = address + a.offset;
        match &a.kind {
            Kind::Custom(name) => return self.custom(name, st, address, a),
            Kind::Tail(e) => {
                // its bytes came with the structure
                if let Element::Struct(inner) = e {
                    let n = match a.counts.first() {
                        Some(c) => self.count(c, st, address)?,
                        None => 1,
                    };
                    self.elements(&layout.structs[inner], at, n)?;
                }
                return Ok(());
            }
            Kind::Inline(inner) => {
                return self.elements(&layout.structs[inner], at, a.count as usize)
            }
            _ => {}
        }
        let indices: Vec<u32> = match &a.order {
            Some(order) => order.clone(),
            None => (0..a.count).collect(),
        };
        for i in indices {
            let slot = at + 4 * i;
            match &a.kind {
                Kind::Str => self.string(slot)?,
                Kind::Sound => self.sound(slot)?,
                Kind::Asset(t) => self.asset(*t, slot)?,
                Kind::Handle(inner) => self.handle(a, &layout.structs[inner], slot)?,
                Kind::Ptr(e) => {
                    let count = match a.counts.len() {
                        0 => None,
                        1 => Some(&a.counts[0]),
                        _ => a.counts.get(i as usize),
                    };
                    self.pointer(st, address, a, e, slot, count, a.reusable, a.align, a.block)?;
                }
                _ => unreachable!(),
            }
        }
        Ok(())
    }

    /// `Load_GfxTexture` 0x0047DD30: an asset-like pointer that is not an asset.
    fn handle(&mut self, a: &Action, inner: &'a StructDef, slot: u32) -> Result<()> {
        let value = self.u32(slot)?;
        if value == 0 {
            return Ok(());
        }
        if let Some(block) = a.block {
            self.push(block);
        }
        if value != INLINE && value != INSERT {
            if self.slots.contains(&value) {
                let target = self.u32(value)?;
                self.set_u32(slot, target)?;
            } else {
                self.bad(&a.name, value);
            }
        } else {
            let target = self.align(inner.align);
            self.set_u32(slot, target)?;
            let inserted = if value == INSERT {
                Some(self.insert_pointer())
            } else {
                None
            };
            self.starts.insert(target);
            self.structure(inner, target, None)?;
            self.slots.insert(slot);
            if let Some(inserted) = inserted {
                self.set_u32(inserted, target)?;
                self.slots.insert(inserted);
            }
        }
        if a.block.is_some() {
            self.pop();
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn pointer(
        &mut self,
        st: &'a StructDef,
        address: u32,
        a: &Action,
        e: &'a Element,
        slot: u32,
        count: Option<&Expr>,
        reusable: bool,
        align: u32,
        block: Option<usize>,
    ) -> Result<()> {
        let value = self.u32(slot)?;
        if value == 0 {
            return Ok(());
        }
        if value != INLINE {
            if reusable {
                if !self.lands(value) {
                    self.bad(&format!("{}.{}", st.name, a.name), value);
                }
                return Ok(());
            }
            let at = self.at();
            self.problems.push(format!(
                "{}.{} = {value:#x}: not inline and not reusable (at {at})",
                st.name, a.name
            ));
        }
        let n = match count {
            Some(c) => self.count(c, st, address)?,
            None => 1,
        };
        if let Some(block) = block {
            self.push(block);
        }
        let layout = self.layout;
        let target = self.align(align);
        self.set_u32(slot, target)?;
        self.starts.insert(target);
        match e {
            Element::Struct(name) if layout.structs[name].tail.is_some() => {
                if n != 1 {
                    return Err(ZoneError::Layout(format!(
                        "{n} of {name}, which has a tail"
                    )));
                }
                self.structure(&layout.structs[name], target, None)?;
            }
            _ => {
                let size = layout.element_size(e) as usize;
                let total = n.checked_mul(size).ok_or(ZoneError::StreamEnds {
                    wanted: u64::MAX,
                    at: self.read_pos,
                    size: self.data.len(),
                })?;
                self.load(target, total)?;
                self.region(target, total as u32, size as u32);
                for i in 0..n {
                    let s = target + (i * size) as u32;
                    match e {
                        Element::Struct(name) => {
                            let inner = &layout.structs[name];
                            self.ctx.push((inner, s));
                            self.members(inner, s)?;
                            self.ctx.pop();
                        }
                        Element::Asset(t) => self.asset(*t, s)?,
                        Element::Str => self.string(s)?,
                        Element::Sound => self.sound(s)?,
                        Element::Ptr(inner) => {
                            let inner_align = layout.element_align(inner);
                            self.pointer(st, address, a, inner, s, None, false, inner_align, None)?;
                        }
                        Element::Int(_) | Element::Bytes(_) => break,
                    }
                }
            }
        }
        if block.is_some() {
            self.pop();
        }
        Ok(())
    }

    // ---- loaders the layout names (custom:<name>), written out from the game's code ----

    fn custom(&mut self, name: &str, st: &'a StructDef, address: u32, a: &Action) -> Result<()> {
        let slot = address + a.offset;
        match name {
            "xanim_indices" => {
                let wide = self.arg(a, "wide", st, address)?[0] != 0;
                let count = match a.counts.first() {
                    Some(e) => self.count(e, st, address)?,
                    None => return Err(ZoneError::Layout(format!("{} needs count=", a.name))),
                };
                self.xanim_indices(slot, count, wide)
            }
            "xanim_delta_part" => {
                let wide = self.arg(a, "wide", st, address)?[0] != 0;
                self.xanim_delta_part(slot, wide)
            }
            "weapon_accuracy_graphs" => {
                let tables = self.arg(a, "tables", st, address)?;
                let mut counts = Vec::with_capacity(a.counts.len());
                for e in &a.counts {
                    counts.push(self.count(e, st, address)?);
                }
                if counts.len() != a.count as usize {
                    return Err(ZoneError::Layout(format!(
                        "{} needs one count per element",
                        a.name
                    )));
                }
                let tables: Vec<u32> = tables.iter().map(|&t| address + t as u32).collect();
                self.weapon_accuracy_graphs(slot, &counts, &tables)
            }
            _ => Err(ZoneError::Layout(format!("no custom loader {name}"))),
        }
    }

    /// A custom loader's argument `key`, evaluated in the structure holding it.
    fn arg(&mut self, a: &Action, key: &str, st: &'a StructDef, address: u32) -> Result<Vec<i64>> {
        let Some((_, exprs)) = a.args.iter().find(|(k, _)| k == key) else {
            return Err(ZoneError::Layout(format!("{} needs {key}=", a.name)));
        };
        let mut values = Vec::with_capacity(exprs.len());
        for e in exprs {
            values.push(self.eval(e, st, address)?);
        }
        Ok(values)
    }

    /// `Load_XAnimIndices` 0x0047BB80: `count` bytes, or `count` 2-aligned
    /// shorts when the animation is `wide` (256 frames or more).
    fn xanim_indices(&mut self, slot: u32, mut count: usize, wide: bool) -> Result<()> {
        if self.u32(slot)? == 0 {
            return Ok(());
        }
        let target = if wide {
            count *= 2;
            self.align(2)
        } else {
            self.here()
        };
        self.set_u32(slot, target)?;
        self.load(target, count)
    }

    /// The frame index table after an animation part's header: size + 1
    /// entries, bytes, or shorts when the animation is `wide`; unaligned
    /// (`Load_XAnimPartTransFrames` 0x0047BF70, `Load_XAnimDeltaPartQuatData` 0x0047BC70).
    fn frame_indices(&mut self, size: usize, wide: bool) -> Result<()> {
        let n = if wide { 2 * size + 2 } else { size + 1 };
        let here = self.here();
        self.load(here, n)
    }

    /// `Load_XAnimDeltaPart` 0x0047BEC0 and what it calls: the translation
    /// (0x0047C1B0, 0x0047C110, 0x0047C000, 0x0047B750) and the rotation
    /// (0x0047BE30, 0x0047BD00, 0x0047B850).
    fn xanim_delta_part(&mut self, slot: u32, wide: bool) -> Result<()> {
        if self.u32(slot)? == 0 {
            return Ok(());
        }
        let part = self.align(4);
        self.set_u32(slot, part)?;
        self.load(part, 8)?;
        if self.u32(part)? != 0 {
            let trans = self.align(4);
            self.set_u32(part, trans)?;
            self.load(trans, 4)?;
            let head = self.read(trans, 3)?;
            let (size, small) = (
                u16::from_le_bytes([head[0], head[1]]) as usize,
                head[2] != 0,
            );
            if size != 0 {
                let data = self.here();
                self.load(data, 0x1C)?;
                self.frame_indices(size, wide)?;
                if self.u32(data + 0x18)? != 0 {
                    let frames = if small { self.here() } else { self.align(4) };
                    self.set_u32(data + 0x18, frames)?;
                    self.load(frames, (size + 1) * if small { 3 } else { 6 })?;
                }
            } else {
                let here = self.here();
                self.load(here, 0xC)?; // frame0
            }
        }
        if self.u32(part + 4)? != 0 {
            let quat = self.align(4);
            self.set_u32(part + 4, quat)?;
            self.load(quat, 4)?;
            let head = self.read(quat, 2)?;
            let size = u16::from_le_bytes([head[0], head[1]]) as usize;
            if size != 0 {
                let data = self.here();
                self.load(data, 4)?;
                self.frame_indices(size, wide)?;
                if self.u32(data)? != 0 {
                    let frames = self.align(4);
                    self.set_u32(data, frames)?;
                    self.load(frames, (size + 1) * 4)?;
                }
            } else {
                let here = self.here();
                self.load(here, 4)?; // frame0
            }
        }
        Ok(())
    }

    /// `Load_WeaponDef` 0x00486780 at 0x00487700: for each of its string slots,
    /// the string, then from each table a pointer to `counts[i]` 8-byte
    /// entries, 4-aligned (the accuracy graphs: name, knots, original knots).
    fn weapon_accuracy_graphs(
        &mut self,
        names: u32,
        counts: &[usize],
        tables: &[u32],
    ) -> Result<()> {
        for (i, &count) in counts.iter().enumerate() {
            let i = i as u32;
            self.string(names + 4 * i)?;
            for &table in tables {
                let at = table + 4 * i;
                let value = self.u32(at)?;
                if value == 0 {
                    continue;
                }
                if value != INLINE {
                    if !self.lands(value) {
                        self.bad("weapon accuracy graph", value);
                    }
                    continue;
                }
                let target = self.align(4);
                self.set_u32(at, target)?;
                self.starts.insert(target);
                self.load(target, count * 8)?;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A layout for the tests: a raw file, a material-like asset with a
    /// texture table, a referenced image and a runtime block, and an
    /// animation-like asset whose index table is read by a custom loader.
    const LAYOUT: &str = "\
asset 32 rawfile RawFile
asset 8 image Image
asset 6 material Mat name=info.name
struct RawFile 0xc
  0x0004 len i32
  0x0000 name string
  0x0008 buffer ptr u8 count=len+1
struct Image 0x8
  0x0000 name string
struct Info 0x4
  0x0000 name string
struct Mat 0x14
  0x000c count u8
  0x0000 info inline Info
  0x0004 images ptr asset:image count=count align=4
  0x0008 scratch ptr u32 count=count block=runtime align=4
  0x0010 table ptr u16 count=count reusable align=2
asset 4 anim Anim
struct Anim 0x10
  0x0004 frames u16
  0x0008 count u32
  0x0000 name string
  0x000c indices custom:xanim_indices count=count wide=frames>=256
";

    fn le(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    /// Builds a zone: the header (with block sizes), the asset list and the
    /// streamed bytes.
    fn zone(blocks: [u32; 7], assets: &[(u32, u32)], stream: &[u8]) -> Vec<u8> {
        let mut z = le(&[0, 0]);
        z.extend(le(&blocks));
        z.extend(le(&[
            0,
            0,
            assets.len() as u32,
            if assets.is_empty() { 0 } else { INLINE },
        ]));
        for &(t, h) in assets {
            z.extend(le(&[t, h]));
        }
        z.extend_from_slice(stream);
        z
    }

    fn read(z: &[u8]) -> LoadedZone {
        let layout = Layout::parse(LAYOUT).unwrap();
        Reader::new(&layout, z).unwrap().run().unwrap()
    }

    #[test]
    fn a_raw_file_reads_exactly() {
        let mut stream = le(&[INLINE, 5, INLINE]);
        stream.extend_from_slice(b"maps/x.gsc\0hello\0");
        // VIRTUAL: the asset array (8), the name (11), the buffer (6)
        let z = zone([0xc, 0, 0, 0, 8 + 11 + 6, 0, 0], &[(32, INLINE)], &stream);
        let loaded = read(&z);
        assert_eq!(loaded.problems, Vec::<String>::new());
        assert_eq!(loaded.assets.len(), 1);
        assert_eq!(loaded.assets[0].name, "maps/x.gsc");
        assert_eq!(loaded.stream_read, z.len());
        let buffer = loaded.u32(loaded.assets[0].address + 8).unwrap();
        assert_eq!(loaded.string(buffer).unwrap(), "hello");
    }

    #[test]
    fn references_runtime_blocks_and_inserted_slots() {
        // A material with two images: the first inline with a slot (-2), the
        // second a reference to that slot; a runtime array (not in the
        // stream) and a reference to its own image array.
        let mut stream = le(&[INLINE, INLINE, INLINE, 0, INLINE]); // Mat (count at 0xc = 0, set below)
        stream[0xc] = 2;
        stream.extend_from_slice(b"mat\0");
        // images array: 2 handles
        // VIRTUAL so far: asset array 8, "mat" 4 -> images at 12
        let slot_of_insert = ptr(VIRTUAL, 20); // after the images array (12..20)
        stream.extend(le(&[INSERT, slot_of_insert]));
        stream.extend(le(&[INLINE, 0])); // image 1: name = -1
        stream.extend_from_slice(b"img\0");
        // table: a reference to the images array (2-aligned u16 view of it)
        let images = ptr(VIRTUAL, 12);
        // patch the table pointer to the reference
        stream[0x10..0x14].copy_from_slice(&images.to_le_bytes());
        // VIRTUAL: 8 + 4 + 8 (images) + 4 (slot) + 4 ("img") = 28; RUNTIME: 8
        let z = zone([0x14 + 0x8, 8, 0, 0, 28, 0, 0], &[(6, INLINE)], &stream);
        let loaded = read(&z);
        assert_eq!(loaded.problems, Vec::<String>::new());
        let names: Vec<_> = loaded.assets.iter().map(|a| a.name.as_str()).collect();
        assert_eq!(names, ["img", "mat"]);
        assert_eq!(loaded.stream_read, z.len());
    }

    #[test]
    fn a_custom_loader_takes_its_arguments_from_the_layout() {
        // 3 indices: bytes below 256 frames, 2-aligned shorts from 256 on.
        for (frames, indices) in [(10u32, 3u32), (300, 6)] {
            let mut stream = le(&[INLINE, frames, 3, INLINE]);
            stream.extend_from_slice(b"a\0");
            stream.resize(stream.len() + indices as usize, 7);
            // VIRTUAL: the asset array (8), the name (2), the indices (at 10, already 2-aligned)
            let z = zone(
                [0x10, 0, 0, 0, 8 + 2 + indices, 0, 0],
                &[(4, INLINE)],
                &stream,
            );
            let loaded = read(&z);
            assert_eq!(loaded.problems, Vec::<String>::new(), "{frames} frames");
            assert_eq!(loaded.assets[0].name, "a");
            assert_eq!(loaded.stream_read, z.len());
        }
        // A custom loader without its argument is a mistake in the layout.
        let layout = Layout::parse(
            "asset 4 anim Anim\nstruct Anim 0x10\n  0x0008 count u32\n  0x0000 name string\n  \
             0x000c indices custom:xanim_indices count=count\n",
        )
        .unwrap();
        let mut stream = le(&[INLINE, 0, 0, INLINE]);
        stream.extend_from_slice(b"a\0");
        let z = zone([0x10, 0, 0, 0, 10, 0, 0], &[(4, INLINE)], &stream);
        let err = Reader::new(&layout, &z).unwrap().run().unwrap_err();
        assert!(
            matches!(&err, ZoneError::Layout(m) if m.contains("wide=")),
            "{err}"
        );
    }

    #[test]
    fn a_reference_to_nothing_is_reported() {
        let mut stream = le(&[ptr(VIRTUAL, 0x100), 5, INLINE]);
        stream.extend_from_slice(b"hello\0");
        let z = zone([0xc, 0, 0, 0, 8 + 6, 0, 0], &[(32, INLINE)], &stream);
        let loaded = read(&z);
        assert_eq!(loaded.problems.len(), 1, "{:?}", loaded.problems);
        assert!(loaded.problems[0].contains("string reference"));
    }

    #[test]
    fn a_short_stream_is_an_error() {
        let z = zone([0xc, 0, 0, 0, 8, 0, 0], &[(32, INLINE)], &le(&[INLINE, 5]));
        let layout = Layout::parse(LAYOUT).unwrap();
        let err = Reader::new(&layout, &z).unwrap().run().unwrap_err();
        assert!(matches!(err, ZoneError::StreamEnds { .. }), "{err}");
    }

    #[test]
    fn a_type_without_a_loader_is_an_error() {
        let z = zone([0, 0, 0, 0, 8, 0, 0], &[(19, INLINE)], &[]);
        let layout = Layout::parse(LAYOUT).unwrap();
        let err = Reader::new(&layout, &z).unwrap().run().unwrap_err();
        assert!(
            matches!(
                err,
                ZoneError::NoLoader {
                    type_number: 19,
                    ..
                }
            ),
            "{err}"
        );
    }
}
