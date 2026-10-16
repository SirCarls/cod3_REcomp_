//! The directory at the start of every unpacked main chunk ("APKF"), the data
//! it describes, and the pointer and import tables at the chunk's end.
//!
//! Worked out from the retail data (see docs/formats/cod3.md). All values are
//! big-endian. "Self-relative" pointers in the directory are added to the
//! address of the field that holds them.
//!
//! ```text
//! 0x00  "APKF"
//! 0x04  u32 0x04010000, u32 0x00000004, u32 0xFFFFFFFF   (constant)
//! 0x10  u32 data area count       2 ("image", "physical"); 1 in small chunks
//! 0x14  u32 directory end
//! 0x18  u32 0
//! 0x1C  type records, 0x1C bytes each, up to the first item table:
//!         u32 type        a four-character code stored byte-reversed
//!                         ('TEX', 'MESH', 'MAT', 'SKEL', 'MLTI', 'FX', 'FONT')
//!                         or the hash of a file extension (.cgb, .csv, ...)
//!         u32 version     TEX 3, MESH 2, MAT 2, SKEL 2, MLTI 100, others 0/1
//!         u32 part count  1 = object data only, 2 = also GPU data
//!         u32 items       self-relative pointer to the item table
//!         u32 item count
//!         u32 alignment   of each item's object data
//!         u32 flags       0xFF000000 without GPU data; otherwise 0x01 in the
//!                         top byte and the GPU data's alignment below
//! item tables, 4 + 4 * part count bytes per item:
//!         u32 name        self-relative pointer to a name record
//!         u32 size[part count]
//! name pool               right after the last item table. Records are a
//!                         u32 hash then the zero-terminated name.
//! directory end + 0x14: data area descriptors, 0x18 bytes each:
//!         u32 name        self-relative pointer to a name record
//!         u32 0
//!         u32 alignment   "image" 0x10 (or the largest item alignment),
//!                         "physical" 0x10000
//!         u32 unknown     0 or 0x10000
//!         u32 size        bytes of data in the area
//!         u32 unknown
//! ```
//!
//! The item data is not pointed to from the directory. It is packed in
//! directory order, each item aligned as its type record says:
//!
//! 1. **"image"**: object data (headers, tables, scripts). It starts right
//!    after the area descriptors, aligned to the area's alignment.
//! 2. **"physical"**: GPU data (texture pixels, vertex and index buffers). It
//!    starts at the next multiple of its alignment (64 KB) after the image.
//!
//! The chunk ends with two tables, each closed by `0xFFFFFFFF`:
//!
//! 1. **Pointer fields**: one u32 per field in the object data that holds a
//!    pointer, as a dword index from the start of the image area. On load the
//!    game turns each into an address. The field's own value is either:
//!    - `0xFC0000nn`: a reference to the name record at
//!      `name pool + 4 * nn`;
//!    - otherwise a target, as a dword index from the start of the image area.
//! 2. **Imports**, 12 bytes each: u32 field (dword index from the image start),
//!    u32 type (four-character code), u32 name (byte offset of a name record
//!    in the name pool). The field gets the address of the named resource of
//!    that type, loaded from another chunk or archive. In the file the field
//!    holds a placeholder (`0xCDCDCDCD` or a count).
//!
//! **Checks:** on 32 chunks from saint_lo, weapons, chars, start and global:
//! - every texture object's D3D header lands where this layout puts it;
//! - every material and mesh object starts with a reference to its own name;
//! - every pointer target lands inside an item;
//! - the two tables end exactly at the end of the chunk.

use std::collections::HashMap;
use std::fmt;

pub const MAGIC: &[u8; 4] = b"APKF";
const TYPE_RECORD_SIZE: usize = 0x1C;
/// The area descriptors start this far past the directory end.
const AREA_DESCRIPTORS_OFFSET: usize = 0x14;
const AREA_DESCRIPTOR_SIZE: usize = 0x18;
/// Tables at the chunk's end are closed by this value.
const TABLE_END: u32 = 0xFFFF_FFFF;
const IMPORT_SIZE: usize = 12;
/// Top byte of a field value that refers to a name record.
const NAME_REF_TAG: u32 = 0xFC;

#[derive(Debug)]
pub enum Error {
    NotApkf,
    Truncated(&'static str),
    NoImageArea,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotApkf => write!(f, "no APKF directory at the start of the chunk"),
            Error::Truncated(what) => write!(f, "chunk ends inside the {what}"),
            Error::NoImageArea => write!(f, "the chunk has no data area descriptors"),
        }
    }
}

impl std::error::Error for Error {}

/// One resource: its name and where its parts lie in the chunk.
#[derive(Debug, Clone)]
pub struct Item {
    pub name: String,
    pub name_hash: u32,
    /// Object data (headers, tables): offset and size in the chunk.
    pub object: (usize, usize),
    /// GPU data (texture pixels, vertex and index buffers), when the type has
    /// it: offset and size in the chunk.
    pub gpu: Option<(usize, usize)>,
}

#[derive(Debug, Clone)]
pub struct TypeRecord {
    pub type_code: u32,
    pub version: u32,
    pub part_count: u32,
    pub alignment: u32,
    pub flags: u32,
    pub items: Vec<Item>,
}

impl TypeRecord {
    /// "TEX", "MESH", ... for four-character codes, else the hex hash.
    pub fn type_name(&self) -> String {
        four_cc_name(self.type_code)
    }

    fn gpu_alignment(&self) -> usize {
        ((self.flags & 0x00FF_FFFF) as usize).max(1)
    }
}

/// "TEX", "MESH", ... for a byte-reversed four-character code, else the hex value.
pub fn four_cc_name(code: u32) -> String {
    let bytes = code.to_le_bytes();
    let trimmed: Vec<u8> = bytes.iter().copied().filter(|&b| b != 0).collect();
    if !trimmed.is_empty() && trimmed.iter().all(|b| b.is_ascii_uppercase()) {
        String::from_utf8(trimmed).unwrap()
    } else {
        format!("{code:08x}")
    }
}

/// A data area: "image" (object data) or "physical" (GPU data).
#[derive(Debug, Clone)]
pub struct DataArea {
    pub name: String,
    pub alignment: usize,
    /// Where the area starts in the chunk, and its size.
    pub offset: usize,
    pub size: usize,
}

/// A field that the game fills with the address of a resource loaded
/// elsewhere, found by type and name hash.
#[derive(Debug, Clone)]
pub struct Import {
    /// Offset of the field in the chunk.
    pub field: usize,
    pub type_code: u32,
    pub name: String,
    /// The hash of the full name (`name_hash`), which the game matches:
    /// names are cut to 27 characters in the files, hashes are not.
    pub name_hash: u32,
}

#[derive(Debug, Clone)]
pub struct Directory {
    pub directory_end: usize,
    pub areas: Vec<DataArea>,
    pub types: Vec<TypeRecord>,
    /// Start of the name pool; `0xFC0000nn` values refer to `name_pool + 4 * nn`.
    pub name_pool: usize,
    /// Start of the "image" area. Pointers count dwords from here.
    pub object_base: usize,
    /// Where the data areas end; the tables start at the next dword.
    pub data_end: usize,
    /// Offsets of the fields that hold pointers or name references.
    pub pointer_fields: Vec<usize>,
    pub imports: Vec<Import>,
    /// Just past the import table's end marker. Equals the chunk size in
    /// every retail chunk checked.
    pub end_of_tables: usize,
}

/// What a field in an object refers to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Link {
    /// The field is not a pointer, or it is empty.
    None,
    /// An offset in this chunk.
    Local(usize),
    /// A resource found by type and name hash, in another chunk or archive.
    Import {
        type_name: String,
        name: String,
        name_hash: u32,
    },
    /// A name record (`0xFC0000nn` reference).
    Name(String),
}

/// An unpacked main chunk with its directory, for following links.
pub struct Chunk<'a> {
    pub data: &'a [u8],
    pub directory: Directory,
    is_pointer: std::collections::HashSet<usize>,
    import_at: HashMap<usize, usize>,
}

fn be32(d: &[u8], at: usize, what: &'static str) -> Result<u32, Error> {
    let b = d.get(at..at + 4).ok_or(Error::Truncated(what))?;
    Ok(u32::from_be_bytes(b.try_into().unwrap()))
}

fn align(x: usize, a: usize) -> usize {
    if a <= 1 {
        x
    } else {
        x.div_ceil(a) * a
    }
}

/// Longest name read from a name record; names in the retail data are shorter.
const MAX_NAME: usize = 128;

/// The hash the game stores with every name and matches imports by: over
/// the lower-cased full name, `hash = hash * 33 + byte` from 0 (the djb2
/// step without its seed). Names in the files are cut to 27 characters, so
/// `brick$us_dirty_broken_wall_c` and `..._n` share a text but not a hash.
pub fn name_hash(name: &str) -> u32 {
    name.bytes()
        .map(|b| b.to_ascii_lowercase() as u32)
        .fold(0u32, |hash, b| hash.wrapping_mul(33).wrapping_add(b))
}

/// Reads a name record (u32 hash, then a zero-terminated name) at `at`.
fn name_record(d: &[u8], at: usize) -> Result<(u32, String), Error> {
    let hash = be32(d, at, "name record")?;
    let rest = d.get(at + 4..).ok_or(Error::Truncated("name record"))?;
    let len = rest
        .iter()
        .take(MAX_NAME)
        .position(|&b| b == 0)
        .unwrap_or(0);
    Ok((hash, String::from_utf8_lossy(&rest[..len]).into_owned()))
}

/// Follows the self-relative pointer in the field at `field`.
fn self_relative(d: &[u8], field: usize, what: &'static str) -> Result<usize, Error> {
    let delta = be32(d, field, what)? as i32 as isize;
    field
        .checked_add_signed(delta)
        .ok_or(Error::Truncated(what))
}

pub fn parse(chunk: &[u8]) -> Result<Directory, Error> {
    if chunk.get(..4) != Some(MAGIC.as_slice()) {
        return Err(Error::NotApkf);
    }
    let area_count = be32(chunk, 0x10, "header")? as usize;
    let directory_end = be32(chunk, 0x14, "header")? as usize;

    // Type records run until the first item table begins.
    let mut types = Vec::new();
    let mut at = 0x1C;
    let mut first_table = directory_end;
    let mut tables_end = 0;
    while at + TYPE_RECORD_SIZE <= first_table {
        let field = |k: usize| be32(chunk, at + 4 * k, "type records");
        let (type_code, version, part_count) = (field(0)?, field(1)?, field(2)?);
        let table = at + 12 + field(3)? as usize;
        let (count, alignment, flags) = (field(4)? as usize, field(5)?, field(6)?);
        if table >= chunk.len() || !(1..=2).contains(&part_count) {
            break;
        }
        first_table = first_table.min(table);

        let entry_size = 4 + 4 * part_count as usize;
        tables_end = tables_end.max(table + count * entry_size);
        let mut items = Vec::with_capacity(count);
        for i in 0..count {
            let entry = table + i * entry_size;
            let (name_hash, name) = name_record(chunk, self_relative(chunk, entry, "item table")?)?;
            let object_size = be32(chunk, entry + 4, "item table")? as usize;
            let gpu_size = if part_count == 2 {
                Some(be32(chunk, entry + 8, "item table")? as usize)
            } else {
                None
            };
            items.push(Item {
                name,
                name_hash,
                object: (0, object_size),
                gpu: gpu_size.map(|s| (0, s)),
            });
        }
        types.push(TypeRecord {
            type_code,
            version,
            part_count,
            alignment,
            flags,
            items,
        });
        at += TYPE_RECORD_SIZE;
    }

    // Data area descriptors, then the areas themselves.
    let descriptors = directory_end + AREA_DESCRIPTORS_OFFSET;
    let mut pos = descriptors + AREA_DESCRIPTOR_SIZE * area_count;
    let mut areas = Vec::with_capacity(area_count);
    for i in 0..area_count {
        let d = descriptors + AREA_DESCRIPTOR_SIZE * i;
        let (_, name) = name_record(chunk, self_relative(chunk, d, "area descriptors")?)?;
        let alignment = be32(chunk, d + 8, "area descriptors")? as usize;
        let size = be32(chunk, d + 16, "area descriptors")? as usize;
        pos = align(pos, alignment);
        areas.push(DataArea {
            name,
            alignment,
            offset: pos,
            size,
        });
        pos += size;
    }
    let data_end = pos;
    let object_base = areas.first().ok_or(Error::NoImageArea)?.offset;

    // Lay the items out: object data in the image area, GPU data in the
    // physical area.
    let mut pos = object_base;
    for t in &mut types {
        for item in &mut t.items {
            pos = align(pos, t.alignment as usize);
            item.object.0 = pos;
            pos += item.object.1;
        }
    }
    if let Some(physical) = areas.get(1) {
        let mut pos = physical.offset;
        for t in &mut types {
            let a = t.gpu_alignment();
            for item in &mut t.items {
                if let Some(gpu) = &mut item.gpu {
                    pos = align(pos, a);
                    gpu.0 = pos;
                    pos += gpu.1;
                }
            }
        }
    }

    // The two tables after the data.
    let name_pool = tables_end;
    let mut at = align(data_end, 4);
    let mut pointer_fields = Vec::new();
    loop {
        let index = be32(chunk, at, "pointer table")?;
        at += 4;
        if index == TABLE_END {
            break;
        }
        pointer_fields.push(object_base + 4 * index as usize);
    }
    let mut imports = Vec::new();
    loop {
        let index = be32(chunk, at, "import table")?;
        if index == TABLE_END {
            at += 4;
            break;
        }
        let type_code = be32(chunk, at + 4, "import table")?;
        let name_offset = be32(chunk, at + 8, "import table")? as usize;
        let (name_hash, name) = name_record(chunk, name_pool + name_offset)?;
        imports.push(Import {
            field: object_base + 4 * index as usize,
            type_code,
            name,
            name_hash,
        });
        at += IMPORT_SIZE;
    }

    Ok(Directory {
        directory_end,
        areas,
        types,
        name_pool,
        object_base,
        data_end,
        pointer_fields,
        imports,
        end_of_tables: at,
    })
}

impl<'a> Chunk<'a> {
    pub fn parse(data: &'a [u8]) -> Result<Self, Error> {
        let directory = parse(data)?;
        let is_pointer = directory.pointer_fields.iter().copied().collect();
        let import_at = directory
            .imports
            .iter()
            .enumerate()
            .map(|(i, imp)| (imp.field, i))
            .collect();
        Ok(Chunk {
            data,
            directory,
            is_pointer,
            import_at,
        })
    }

    /// The big-endian u32 at `at`, if it is inside the chunk.
    pub fn u32_at(&self, at: usize) -> Option<u32> {
        be32(self.data, at, "").ok()
    }

    pub fn f32_at(&self, at: usize) -> Option<f32> {
        self.u32_at(at).map(f32::from_bits)
    }

    /// The name a `0xFC0000nn` value refers to.
    pub fn name_ref(&self, value: u32) -> Option<String> {
        if value >> 24 != NAME_REF_TAG {
            return None;
        }
        let record = self.directory.name_pool + 4 * (value & 0x00FF_FFFF) as usize;
        name_record(self.data, record).ok().map(|(_, name)| name)
    }

    /// What the field at `field` refers to.
    pub fn link(&self, field: usize) -> Link {
        if let Some(&i) = self.import_at.get(&field) {
            let import = &self.directory.imports[i];
            return Link::Import {
                type_name: four_cc_name(import.type_code),
                name: import.name.clone(),
                name_hash: import.name_hash,
            };
        }
        if !self.is_pointer.contains(&field) {
            return Link::None;
        }
        let Some(value) = self.u32_at(field) else {
            return Link::None;
        };
        if let Some(name) = self.name_ref(value) {
            return Link::Name(name);
        }
        let target = self.directory.object_base + 4 * value as usize;
        if target < self.data.len() {
            Link::Local(target)
        } else {
            Link::None
        }
    }

    /// The local offset the field points to, if it holds a pointer.
    pub fn pointer(&self, field: usize) -> Option<usize> {
        match self.link(field) {
            Link::Local(at) => Some(at),
            _ => None,
        }
    }

    /// The item whose object data contains `offset`: (type, item).
    pub fn item_at(&self, offset: usize) -> Option<(&TypeRecord, &Item)> {
        self.directory.types.iter().find_map(|t| {
            t.items
                .iter()
                .find(|i| (i.object.0..i.object.0 + i.object.1).contains(&offset))
                .map(|i| (t, i))
        })
    }

    /// All items of one type ("TEX", "MAT", ...).
    pub fn items_of<'s>(&'s self, type_name: &'s str) -> impl Iterator<Item = &'s Item> + 's {
        self.directory
            .types
            .iter()
            .filter(move |t| t.type_name() == type_name)
            .flat_map(|t| t.items.iter())
    }

    pub fn object(&self, item: &Item) -> Option<&'a [u8]> {
        let (at, len) = item.object;
        self.data.get(at..at + len)
    }

    pub fn gpu(&self, item: &Item) -> Option<&'a [u8]> {
        let (at, len) = item.gpu?;
        self.data.get(at..at + len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a tiny chunk by hand: one type ("MAT", 1 item of 16 bytes) and
    /// one data area, with a pointer field and an import.
    fn sample_chunk() -> Vec<u8> {
        let mut d = vec![0u8; 0x200];
        let put =
            |d: &mut Vec<u8>, at: usize, v: u32| d[at..at + 4].copy_from_slice(&v.to_be_bytes());
        d[..4].copy_from_slice(MAGIC);
        put(&mut d, 0x10, 1); // one area
                              // Type record at 0x1C; its item table follows at 0x38.
        put(&mut d, 0x1C, u32::from_le_bytes(*b"MAT\0"));
        put(&mut d, 0x20, 2); // version
        put(&mut d, 0x24, 1); // part count
        put(&mut d, 0x28, 0x38 - 0x28); // self-relative pointer to the table
        put(&mut d, 0x2C, 1); // item count
        put(&mut d, 0x30, 16); // alignment
        put(&mut d, 0x34, 0xFF00_0000);
        // Item table entry at 0x38: name pointer, size.
        put(&mut d, 0x38, 0x40 - 0x38);
        put(&mut d, 0x3C, 16);
        // Name pool at 0x40: "mat_a" record, "image" record, "tex_b" record.
        d[0x44..0x49].copy_from_slice(b"mat_a");
        d[0x54..0x59].copy_from_slice(b"image");
        d[0x64..0x69].copy_from_slice(b"tex_b");
        let directory_end = 0x60;
        put(&mut d, 0x14, directory_end as u32);
        // Area descriptor at directory end + 0x14 = 0x74: name -> 0x50.
        put(&mut d, 0x74, (0x50i32 - 0x74) as u32);
        put(&mut d, 0x74 + 8, 16); // alignment
        put(&mut d, 0x74 + 16, 16); // size
                                    // Image area at align(0x74 + 0x18, 16) = 0x90; the item lives there.
        put(&mut d, 0x90, 0xFC00_0000); // name ref -> pool + 0 ("mat_a")
        put(&mut d, 0x94, 0); // pointer to dword 0 of the image = the item itself
        put(&mut d, 0x98, 0xCDCD_CDCD); // import placeholder
                                        // Tables at 0xA0: pointer fields 0 and 1, then one import of field 2.
        put(&mut d, 0xA0, 0);
        put(&mut d, 0xA4, 1);
        put(&mut d, 0xA8, TABLE_END);
        put(&mut d, 0xAC, 2);
        put(&mut d, 0xB0, u32::from_le_bytes(*b"TEX\0"));
        put(&mut d, 0xB4, 0x20); // "tex_b" record is pool + 0x20
        put(&mut d, 0xB8, TABLE_END);
        d.truncate(0xBC);
        d
    }

    #[test]
    fn layout_and_links() {
        let data = sample_chunk();
        let chunk = Chunk::parse(&data).unwrap();
        let dir = &chunk.directory;
        assert_eq!(dir.name_pool, 0x40);
        assert_eq!(dir.areas[0].name, "image");
        assert_eq!(dir.object_base, 0x90);
        let item = &dir.types[0].items[0];
        assert_eq!(item.name, "mat_a");
        assert_eq!(item.object, (0x90, 16));
        assert_eq!(chunk.link(0x90), Link::Name("mat_a".into()));
        assert_eq!(chunk.link(0x94), Link::Local(0x90));
        assert_eq!(
            chunk.link(0x98),
            Link::Import {
                type_name: "TEX".into(),
                name: "tex_b".into(),
                name_hash: 0,
            }
        );
        assert_eq!(chunk.link(0x9C), Link::None);
        assert_eq!(chunk.item_at(0x94).unwrap().1.name, "mat_a");
        assert_eq!(dir.end_of_tables, data.len());
    }

    #[test]
    fn name_hashes_match_the_files() {
        // Hashes read from global.cod and saint_lo.cod name records.
        assert_eq!(name_hash("garamond"), 0x4a52_b849);
        assert_eq!(name_hash("GARAMOND"), 0x4a52_b849);
        assert_eq!(name_hash("brick$us_dirty_broken_wall_c"), 0x71e6_ddf3);
        assert_eq!(name_hash("brick$us_dirty_broken_wall_n"), 0x71e6_ddfe);
    }
}
