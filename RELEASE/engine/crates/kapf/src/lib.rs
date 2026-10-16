//! Call of Duty 3 (Xbox 360) `.cod` archive reader.
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

#![forbid(unsafe_code)]

pub mod apkf;

use std::fmt;

pub const MAGIC: &[u8; 4] = b"KAPF";
pub const NCH_MAGIC: &[u8; 4] = b"NCH\0";
const SECTION_RECORD_SIZE: usize = 0x18;
const CHUNK_ENTRY_SIZE: usize = 0x20;
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
            Error::NotKapf => write!(f, "not a Call of Duty 3 .cod archive (no KAPF header)"),
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
        if file.len() < 0x30 || &file[..4] != MAGIC {
            return Err(Error::NotKapf);
        }
        let version = le32(file, 0x04, "header")?;
        let header_size = le32(file, 0x08, "header")? as usize;
        let section_count = le32(file, 0x0C, "header")? as usize;
        let builder = cstr(&file[0x1C..0x30]);

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
                let e = table + c * CHUNK_ENTRY_SIZE;
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
