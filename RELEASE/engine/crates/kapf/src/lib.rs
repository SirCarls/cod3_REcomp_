//! Call of Duty 3 (Xbox 360) `.cod` archive reader, which also reads the
//! archives of Call of Duty 2: Big Red One (Xbox, 2005), the Treyarch game
//! whose engine Call of Duty 3 grew from (see "Big Red One" below).
//!
//! Every level folder (`sp/<level>/`) holds several of these: `<level>.cod`,
//! `chars.cod`, `weapons.cod`, `fx.cod`, `vehicles.cod`, the streamed zones
//! `znNN.cod`, and so on. `sp/global.cod` and `sp/anim.cod` hold shared data.
//!
//! Layout, worked out from the retail files (fields marked "unknown" are
//! read and kept but not yet understood):
//!
//! ```text
//! 0x00  "KAPF"
//! 0x04  u32 version            0x4003D70A in every retail archive
//! 0x08  u32 header size        0x30; the section records follow
//! 0x0C  u32 section count
//! 0x10  u32 end of the chunk tables
//! 0x14  u32 unknown, 0x18 u32 unknown
//! 0x1C  char builder[20]       "CODAUTO30" / "CODAUTO32"
//! 0x30  section records, 0x18 bytes each:
//!         char name[12]        "", "ANIM", "AUDIO", "FLI", "STRING"
//!         u16  chunk count
//!         u16  unknown (an item count?)
//!         u32  offset of the section's chunk table
//!         u32  unknown (offsets that grow from record to record)
//! chunk table entries, 0x20 bytes each:
//!         u32  file offset of the chunk (64 KB aligned)
//!         u32  data size           bytes the chunk holds once unpacked
//!         u32  slot size           0x300000: the size reserved for it
//!         u32  flags               0x82 seen everywhere
//!         u32  stored size         0 = stored as is, else NCH blocks
//!         u32  reserved[3]
//! ```
//!
//! All header integers are little-endian. A packed chunk is a series of "NCH"
//! blocks, each 0x20 bytes of big-endian header and an LZO1X stream:
//!
//! ```text
//! "NCH\0", u32 packed size, u32 checksum, u32 unpacked size,
//! u32 output offset, u32 checksum, u32 block size (header + packed), u32 1
//! ```
//!
//! Blocks start every 512 KB (0x80000) inside the chunk; the unpacked parts
//! are placed at their output offsets. The two checksums are not checked yet.
//! The unpacked data of a level archive starts with an "APKF" header (the
//! asset banks, big-endian; see docs/formats/cod3.md).
//!
//! **Big Red One** (found by us in the owner's disc, 2026-10-20; 389 archives
//! with the same level layout: `sp/<level>/<level>.cod`, `chars.cod`,
//! `weapons.cod`, streamed zones, `sp/global.cod`, `mp/global.cod`). The
//! version field is a float: 2.02 (0x400147AE) there, 2.06 (0x4003D70A) in
//! Call of Duty 3. The older layout differs in three places:
//!
//! ```text
//! 0x08  u32 header size        0x1C: no builder name; the section records follow
//! chunk table entries, 0x1C bytes each:
//!         u32  file offset of the chunk (32 KB aligned)
//!         u32  data size, u32 slot size
//!         u32  flags               0, or 1 on some sections' last chunk
//!         u32  reserved[3]         0: every chunk is stored as is (no NCH blocks)
//! ```
//!
//! Its chunks hold `SBII` banks (not APKF; not decoded yet), DDS textures,
//! `XBXM` data, `WAVE` sound and `TRTS` string tables; the sections are named as
//! in Call of Duty 3 ("", `ANIM`, `AUDIO`, `STRING`). [`Archive::game`] says
//! which game an archive is from.

#![forbid(unsafe_code)]

pub mod apkf;

use std::fmt;

pub const MAGIC: &[u8; 4] = b"KAPF";
pub const NCH_MAGIC: &[u8; 4] = b"NCH\0";
/// The version field is the float 2.06 in every Call of Duty 3 archive.
pub const VERSION_COD3: u32 = 0x4003_D70A;
/// ... and 2.02 in every Big Red One archive.
pub const VERSION_BIG_RED_ONE: u32 = 0x4001_47AE;
const SECTION_RECORD_SIZE: usize = 0x18;
const CHUNK_ENTRY_SIZE: usize = 0x20;
const BIG_RED_ONE_CHUNK_ENTRY_SIZE: usize = 0x1C;
/// Call of Duty 3's header, up to the end of the builder name.
const COD3_HEADER_SIZE: usize = 0x30;
const BUILDER_NAME: std::ops::Range<usize> = 0x1C..0x30;

/// The game a `.cod` archive comes from, by its version.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Game {
    /// Call of Duty 3 (Xbox 360).
    CallOfDuty3,
    /// Call of Duty 2: Big Red One (Xbox).
    BigRedOne,
}

impl Game {
    pub fn from_version(version: u32) -> Option<Self> {
        match version {
            VERSION_COD3 => Some(Game::CallOfDuty3),
            VERSION_BIG_RED_ONE => Some(Game::BigRedOne),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Game::CallOfDuty3 => "Call of Duty 3",
            Game::BigRedOne => "Call of Duty 2: Big Red One",
        }
    }

    /// The platform of the archives we have (the other consoles' versions of
    /// either game haven't been looked at).
    pub fn platform(self) -> &'static str {
        match self {
            Game::CallOfDuty3 => "Xbox 360",
            Game::BigRedOne => "Xbox",
        }
    }
}
const NCH_HEADER_SIZE: usize = 0x20;
const NCH_BLOCK_ALIGN: usize = 0x80000;

#[derive(Debug)]
pub enum Error {
    NotKapf,
    Truncated(&'static str),
    BadNchBlock {
        chunk_offset: u32,
        at: usize,
    },
    Lzo {
        chunk_offset: u32,
        at: usize,
        error: lzo::Error,
    },
    SizeMismatch {
        chunk_offset: u32,
        expected: usize,
        got: usize,
    },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotKapf => write!(f, "not a .cod archive (no KAPF header)"),
            Error::Truncated(what) => write!(f, "file ends inside the {what}"),
            Error::BadNchBlock { chunk_offset, at } => {
                write!(f, "chunk at {chunk_offset:#x}: no NCH block at +{at:#x}")
            }
            Error::Lzo {
                chunk_offset,
                at,
                error,
            } => {
                write!(
                    f,
                    "chunk at {chunk_offset:#x}: LZO1X error {error} in the block at +{at:#x}"
                )
            }
            Error::SizeMismatch {
                chunk_offset,
                expected,
                got,
            } => write!(
                f,
                "chunk at {chunk_offset:#x}: unpacked {got:#x} bytes, the table says {expected:#x}"
            ),
        }
    }
}

impl std::error::Error for Error {}

#[derive(Debug, Clone)]
pub struct Chunk {
    pub file_offset: u32,
    pub data_size: u32,
    pub slot_size: u32,
    pub flags: u32,
    /// 0 when the chunk is stored unpacked.
    pub stored_size: u32,
}

impl Chunk {
    pub fn is_packed(&self) -> bool {
        self.stored_size != 0
    }
}

#[derive(Debug, Clone)]
pub struct Section {
    pub name: String,
    pub unknown_count: u16,
    pub unknown_offset: u32,
    pub chunks: Vec<Chunk>,
}

#[derive(Debug)]
pub struct Archive<'a> {
    pub file: &'a [u8],
    pub version: u32,
    pub builder: String,
    pub sections: Vec<Section>,
}

fn le32(d: &[u8], at: usize, what: &'static str) -> Result<u32, Error> {
    let b = d.get(at..at + 4).ok_or(Error::Truncated(what))?;
    Ok(u32::from_le_bytes(b.try_into().unwrap()))
}

fn be32(d: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(d[at..at + 4].try_into().unwrap())
}

fn cstr(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

impl<'a> Archive<'a> {
    pub fn parse(file: &'a [u8]) -> Result<Self, Error> {
        if file.get(..4) != Some(MAGIC.as_slice()) {
            return Err(Error::NotKapf);
        }
        let version = le32(file, 0x04, "header")?;
        let big_red_one = Game::from_version(version) == Some(Game::BigRedOne);
        if !big_red_one && file.len() < COD3_HEADER_SIZE {
            return Err(Error::NotKapf);
        }
        let header_size = le32(file, 0x08, "header")? as usize;
        let section_count = le32(file, 0x0C, "header")? as usize;
        // Big Red One's header ends where Call of Duty 3's builder name starts.
        let (builder, entry_size) = if big_red_one {
            (String::new(), BIG_RED_ONE_CHUNK_ENTRY_SIZE)
        } else {
            (cstr(&file[BUILDER_NAME]), CHUNK_ENTRY_SIZE)
        };

        let mut sections = Vec::with_capacity(section_count);
        for i in 0..section_count {
            let rec = header_size + i * SECTION_RECORD_SIZE;
            let raw = file
                .get(rec..rec + SECTION_RECORD_SIZE)
                .ok_or(Error::Truncated("section table"))?;
            let name = cstr(&raw[..12]);
            let chunk_count = u16::from_le_bytes([raw[12], raw[13]]) as usize;
            let unknown_count = u16::from_le_bytes([raw[14], raw[15]]);
            let table = u32::from_le_bytes(raw[16..20].try_into().unwrap()) as usize;
            let unknown_offset = u32::from_le_bytes(raw[20..24].try_into().unwrap());

            let mut chunks = Vec::with_capacity(chunk_count);
            for c in 0..chunk_count {
                let e = table + c * entry_size;
                chunks.push(Chunk {
                    file_offset: le32(file, e, "chunk table")?,
                    data_size: le32(file, e + 4, "chunk table")?,
                    slot_size: le32(file, e + 8, "chunk table")?,
                    flags: le32(file, e + 12, "chunk table")?,
                    stored_size: le32(file, e + 16, "chunk table")?,
                });
            }
            sections.push(Section {
                name,
                unknown_count,
                unknown_offset,
                chunks,
            });
        }
        Ok(Self {
            file,
            version,
            builder,
            sections,
        })
    }

    /// The game the archive comes from (`None`: a version we don't know).
    pub fn game(&self) -> Option<Game> {
        Game::from_version(self.version)
    }

    /// The unpacked bytes of one chunk.
    pub fn read_chunk(&self, chunk: &Chunk) -> Result<Vec<u8>, Error> {
        let start = chunk.file_offset as usize;
        let size = chunk.data_size as usize;
        if !chunk.is_packed() {
            return Ok(self
                .file
                .get(start..start + size)
                .ok_or(Error::Truncated("chunk data"))?
                .to_vec());
        }
        let stored = self
            .file
            .get(start..start + chunk.stored_size as usize)
            .ok_or(Error::Truncated("packed chunk"))?;

        let mut out = vec![0u8; size];
        let mut filled = 0usize;
        let mut at = 0usize;
        while filled < size {
            if stored.get(at..at + 4) != Some(NCH_MAGIC.as_slice())
                || at + NCH_HEADER_SIZE > stored.len()
            {
                return Err(Error::BadNchBlock {
                    chunk_offset: chunk.file_offset,
                    at,
                });
            }
            let packed_size = be32(stored, at + 4) as usize;
            let unpacked_size = be32(stored, at + 12) as usize;
            let output_offset = be32(stored, at + 16) as usize;
            let block_size = be32(stored, at + 24) as usize;

            let payload_start = at + NCH_HEADER_SIZE;
            let payload = stored
                .get(payload_start..payload_start + packed_size)
                .ok_or(Error::BadNchBlock {
                    chunk_offset: chunk.file_offset,
                    at,
                })?;
            let (data, _) =
                lzo::decompress(payload, unpacked_size).map_err(|error| Error::Lzo {
                    chunk_offset: chunk.file_offset,
                    at,
                    error,
                })?;
            let dst = out
                .get_mut(output_offset..output_offset + data.len())
                .ok_or(Error::SizeMismatch {
                    chunk_offset: chunk.file_offset,
                    expected: size,
                    got: output_offset + data.len(),
                })?;
            dst.copy_from_slice(&data);
            filled += data.len();

            // The next block starts at the next 512 KB boundary. A block that
            // adds nothing or doesn't move forward would loop forever on a
            // damaged file, so it ends the chunk with an error instead.
            let next = (at + block_size).div_ceil(NCH_BLOCK_ALIGN) * NCH_BLOCK_ALIGN;
            if data.is_empty() || next <= at {
                return Err(Error::BadNchBlock {
                    chunk_offset: chunk.file_offset,
                    at,
                });
            }
            at = next;
        }
        if filled != size {
            return Err(Error::SizeMismatch {
                chunk_offset: chunk.file_offset,
                expected: size,
                got: filled,
            });
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// An archive with one section of one stored chunk holding `data`.
    fn archive(version: u32, header_size: usize, entry_size: usize, data: &[u8]) -> Vec<u8> {
        let mut file = vec![0u8; header_size];
        file[..4].copy_from_slice(MAGIC);
        file[4..8].copy_from_slice(&version.to_le_bytes());
        file[8..12].copy_from_slice(&(header_size as u32).to_le_bytes());
        file[12..16].copy_from_slice(&1u32.to_le_bytes());
        if header_size >= COD3_HEADER_SIZE {
            file[BUILDER_NAME][..9].copy_from_slice(b"CODAUTO32");
        }
        let table = header_size + SECTION_RECORD_SIZE;
        let mut record = vec![0u8; SECTION_RECORD_SIZE];
        record[12..14].copy_from_slice(&1u16.to_le_bytes());
        record[16..20].copy_from_slice(&(table as u32).to_le_bytes());
        file.extend(record);
        let chunk_at = table + entry_size;
        let mut entry = vec![0u8; entry_size];
        entry[0..4].copy_from_slice(&(chunk_at as u32).to_le_bytes());
        entry[4..8].copy_from_slice(&(data.len() as u32).to_le_bytes());
        entry[8..12].copy_from_slice(&(data.len() as u32).to_le_bytes());
        file.extend(entry);
        file.extend_from_slice(data);
        file
    }

    #[test]
    fn call_of_duty_3() {
        let file = archive(
            VERSION_COD3,
            COD3_HEADER_SIZE,
            CHUNK_ENTRY_SIZE,
            b"APKF bank",
        );
        let parsed = Archive::parse(&file).unwrap();
        assert_eq!(parsed.game(), Some(Game::CallOfDuty3));
        assert_eq!(parsed.builder, "CODAUTO32");
        let chunk = &parsed.sections[0].chunks[0];
        assert_eq!(parsed.read_chunk(chunk).unwrap(), b"APKF bank");
    }

    #[test]
    fn big_red_one() {
        // A 0x1C-byte header and 0x1C-byte chunk entries; chunks stored as is.
        let file = archive(
            VERSION_BIG_RED_ONE,
            0x1C,
            BIG_RED_ONE_CHUNK_ENTRY_SIZE,
            b"SBII bank",
        );
        let parsed = Archive::parse(&file).unwrap();
        assert_eq!(parsed.game(), Some(Game::BigRedOne));
        assert_eq!(parsed.game().unwrap().name(), "Call of Duty 2: Big Red One");
        assert_eq!(parsed.builder, "");
        let chunk = &parsed.sections[0].chunks[0];
        assert!(!chunk.is_packed());
        assert_eq!(parsed.read_chunk(chunk).unwrap(), b"SBII bank");
    }

    #[test]
    fn not_an_archive() {
        assert!(matches!(Archive::parse(b"KAP"), Err(Error::NotKapf)));
        assert!(matches!(Archive::parse(b"IWffu100"), Err(Error::NotKapf)));
        // A Call of Duty 3 version needs the whole 0x30-byte header.
        let mut short = MAGIC.to_vec();
        short.extend(VERSION_COD3.to_le_bytes());
        assert!(matches!(Archive::parse(&short), Err(Error::NotKapf)));
        assert_eq!(Game::from_version(0x1234), None);
    }
}
