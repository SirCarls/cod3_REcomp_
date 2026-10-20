//! Call of Duty fastfiles (`.ff`) of every game we know: which game and
//! platform a file comes from ([`identify`]), and its zone ([`read`]), the
//! decompressed data the game's own loader reads.
//!
//! Call of Duty 2 on the Xbox 360 and every IW-engine game from Call of Duty 4
//! on keep their assets in fastfiles. From Call of Duty 4 on they start the
//! same way: an 8-byte magic, then a u32 version. The magic says how the file
//! is packed (`IWffu100` unsigned, `IWff0100` signed, `TAff0100` Black Ops II,
//! `S1ff0100` Advanced Warfare); the version says which game and platform it
//! was built for. Consoles store the version big-endian and PCs
//! little-endian, so the byte order tells them apart even where two numbers
//! are equal (1 is Call of Duty 4 on a console and Modern Warfare 3 on PC).
//! Call of Duty 2's fastfiles have no magic: a big-endian u32 0x68 comes first.
//!
//! After the header, each game packs its zone one of a few ways (the zone is
//! an `XFile` header, then the streamed asset list and assets):
//!
//! - one zlib stream, right after the 12-byte header or after a few more
//!   header bytes (Modern Warfare 2 and 3 on PC: 9 more; Ghosts on PC: 32;
//!   Modern Warfare 2 on the Xbox 360: 25 more and 12 per entry it lists;
//!   Call of Duty 2 on the Xbox 360: from byte 20);
//! - signed: an authentication header (`IWffs100`) where the zlib stream would
//!   start, then 8 KB chunks: an authentication chunk, then groups of one hash
//!   chunk and up to 256 data chunks; the data chunks joined are one zlib stream;
//! - encrypted (Black Ops II on PC, Black Ops on the Xbox 360): `PHEEBs71` and
//!   the zone name. These are not read.
//!
//! The `XFile` header has a size, an external size and one size per memory
//! block; how many blocks (and whether the fields are 32 or 64 bits) differs by
//! game and platform. The zone's size field plus the header's length is the
//! zone's length, which checks the table below on every file
//! ([`Zone::size_matches`]).
//!
//! Every number here was read from the owner's own files (PC copies of Call of
//! Duty 4, World at War, Modern Warfare 2 and 3, Black Ops, Black Ops II and
//! Ghosts; Xbox 360 discs of Call of Duty 2, Call of Duty 4, World at War,
//! Modern Warfare 2 and Black Ops), except the other consoles' version numbers,
//! which come from the COD Engine Research wiki's FastFiles page. Sources and
//! what was checked on how many files: `docs/formats/fastfiles.md`. The setup
//! tool has the same detection in Python (`c3w/fastfiles.py`).

#![forbid(unsafe_code)]

use std::fmt;

/// Unsigned fastfiles (every IW-engine game from Call of Duty 4 on).
pub const MAGIC_UNSIGNED: &[u8; 8] = b"IWffu100";
/// Signed fastfiles, read through their hash chunks.
pub const MAGIC_SIGNED: &[u8; 8] = b"IWff0100";
/// Treyarch's own magic from Black Ops II on.
pub const MAGIC_TREYARCH: &[u8; 8] = b"TAff0100";
/// Sledgehammer's magic (Advanced Warfare).
pub const MAGIC_SLEDGEHAMMER: &[u8; 8] = b"S1ff0100";
/// The authentication header of a signed file.
pub const MAGIC_AUTH: &[u8; 8] = b"IWffs100";
/// An encrypted zone (Black Ops II on PC at byte 12, Black Ops on the Xbox
/// 360 at byte 16), followed by the zone name.
pub const MAGIC_ENCRYPTED: &[u8; 8] = b"PHEEBs71";
/// The magic and the version.
pub const HEADER_SIZE: usize = 12;
/// How much of a file [`identify`] looks at.
pub const IDENTIFY_SIZE: usize = 64;

/// How far after the 12-byte header the encryption magic may start.
const ENCRYPTED_MAGIC_WITHIN: usize = 20;
/// Call of Duty 2 on the Xbox 360: no magic; a big-endian u32 0x68, the
/// zone's size, three block sizes, then one zlib stream from byte 20 (all 111
/// fastfiles of the owner's disc: each stream inflates to exactly that size).
const COD2_XBOX360_VERSION: u32 = 0x68;
const COD2_XBOX360_STREAM_START: usize = 20;
const COD2_XBOX360_BLOCKS: usize = 3;
/// Where an unsigned Xbox 360 file's zlib stream starts: right after the
/// 12-byte header (Call of Duty 4, World at War), or after Modern Warfare 2's
/// 25 more bytes when its entry list (below) is empty.
const CONSOLE_STREAM_STARTS: [usize; 2] = [HEADER_SIZE, MW2_XBOX360_STREAM_BASE];
/// Modern Warfare 2 on the Xbox 360, after the 12-byte header: an online
/// update flag (1 byte), a file time (8), a region (4), an entry count (4),
/// that many 12-byte entries, two sizes (8), then the zlib stream (or, in a
/// signed file, the authentication header). Checked on all 84 fastfiles of
/// the owner's disc: the 49 unsigned ones have their zlib stream there, the
/// 35 signed ones their authentication header, and the small ones of both
/// inflate to their zone.
const MW2_XBOX360_COUNT_AT: usize = 25;
const MW2_XBOX360_ENTRY_SIZE: usize = 12;
const MW2_XBOX360_STREAM_BASE: usize = 37;
/// Modern Warfare 2 and 3 on PC: an online update flag and a file time after
/// the version (9 bytes), then the zlib stream or the authentication header.
const IW4_PC_START: usize = 21;
/// Ghosts on PC: 32 more bytes after the version (flags, a file time, the
/// file's size twice), then the zlib stream.
const GHOSTS_PC_START: usize = 44;
/// Signed files are cut into 8 KB chunks from where the authentication
/// header starts: that chunk, then groups of one hash chunk and up to 256
/// data chunks.
const SIGNED_CHUNK_SIZE: usize = 0x2000;
const SIGNED_CHUNKS_PER_GROUP: usize = 256;

/// The games with fastfiles.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Game {
    CallOfDuty2,
    CallOfDuty4,
    WorldAtWar,
    ModernWarfare2,
    BlackOps,
    ModernWarfare3,
    BlackOps2,
    Ghosts,
    AdvancedWarfare,
    BlackOps3,
}

impl Game {
    /// The short key the setup tool uses for the game (`c3w/fastfiles.py`).
    pub fn key(self) -> &'static str {
        match self {
            Game::CallOfDuty2 => "cod2",
            Game::CallOfDuty4 => "cod4",
            Game::WorldAtWar => "waw",
            Game::ModernWarfare2 => "mw2",
            Game::BlackOps => "bo1",
            Game::ModernWarfare3 => "mw3",
            Game::BlackOps2 => "bo2",
            Game::Ghosts => "ghosts",
            Game::AdvancedWarfare => "aw",
            Game::BlackOps3 => "bo3",
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Game::CallOfDuty2 => "Call of Duty 2",
            Game::CallOfDuty4 => "Call of Duty 4: Modern Warfare",
            Game::WorldAtWar => "Call of Duty: World at War",
            Game::ModernWarfare2 => "Call of Duty: Modern Warfare 2",
            Game::BlackOps => "Call of Duty: Black Ops",
            Game::ModernWarfare3 => "Call of Duty: Modern Warfare 3",
            Game::BlackOps2 => "Call of Duty: Black Ops II",
            Game::Ghosts => "Call of Duty: Ghosts",
            Game::AdvancedWarfare => "Call of Duty: Advanced Warfare",
            Game::BlackOps3 => "Call of Duty: Black Ops III",
        }
    }

    /// Whether the game's fastfiles use this magic (a version number under
    /// another game's magic is not that game).
    fn uses_magic(self, magic: &[u8]) -> bool {
        match self {
            Game::BlackOps2 | Game::BlackOps3 => magic == MAGIC_TREYARCH,
            Game::AdvancedWarfare => magic == MAGIC_SLEDGEHAMMER,
            _ => magic == MAGIC_UNSIGNED || magic == MAGIC_SIGNED,
        }
    }
}

/// The platform a fastfile was built for.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Platform {
    Pc,
    Xbox360,
    Ps3,
    /// The Xbox 360 and PS3 builds share their version numbers; this is a
    /// console file whose header doesn't say which.
    Xbox360OrPs3,
    Wii,
    WiiU,
}

impl Platform {
    pub fn name(self) -> &'static str {
        match self {
            Platform::Pc => "PC",
            Platform::Xbox360 => "Xbox 360",
            Platform::Ps3 => "PS3",
            Platform::Xbox360OrPs3 => "Xbox 360 or PS3",
            Platform::Wii => "Wii",
            Platform::WiiU => "Wii U",
        }
    }
}

/// What a fastfile's header says about it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Identity {
    pub game: Game,
    pub platform: Platform,
    pub version: u32,
    /// Consoles store the header (and the zone) big-endian.
    pub big_endian: bool,
    /// RSA-signed: the zone is read through the file's hash chunks.
    pub signed: bool,
    /// The zone is encrypted; [`read`] doesn't read those.
    pub encrypted: bool,
}

impl fmt::Display for Identity {
    /// "Call of Duty: World at War (PC, version 387)".
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{} ({}, version {}",
            self.game.name(),
            self.platform.name(),
            self.version
        )?;
        if self.signed {
            write!(f, ", signed")?;
        }
        if self.encrypted {
            write!(f, ", encrypted")?;
        }
        write!(f, ")")
    }
}

/// One game build's version number.
struct KnownVersion {
    game: Game,
    platform: Platform,
    version: u32,
    big_endian: bool,
}

const fn known(game: Game, platform: Platform, version: u32, big_endian: bool) -> KnownVersion {
    KnownVersion {
        game,
        platform,
        version,
        big_endian,
    }
}

/// Every version number we know. Where each comes from, and how many of the
/// owner's files were checked: docs/formats/fastfiles.md.
const VERSIONS: &[KnownVersion] = &[
    known(Game::CallOfDuty4, Platform::Pc, 0x5, false),
    known(Game::CallOfDuty4, Platform::Xbox360OrPs3, 0x1, true),
    known(Game::CallOfDuty4, Platform::Wii, 0x1A2, true),
    known(Game::WorldAtWar, Platform::Pc, 0x183, false),
    known(Game::WorldAtWar, Platform::Xbox360OrPs3, 0x183, true),
    known(Game::WorldAtWar, Platform::Wii, 0x19B, true),
    known(Game::ModernWarfare2, Platform::Pc, 0x114, false),
    known(Game::ModernWarfare2, Platform::Xbox360OrPs3, 0x10D, true),
    known(Game::BlackOps, Platform::Pc, 0x1D9, false),
    known(Game::BlackOps, Platform::Xbox360OrPs3, 0x1D9, true),
    known(Game::BlackOps, Platform::Wii, 0x1DD, true),
    known(Game::ModernWarfare3, Platform::Pc, 0x1, false),
    known(Game::ModernWarfare3, Platform::Xbox360OrPs3, 0x70, true),
    known(Game::ModernWarfare3, Platform::Wii, 0x6B, true),
    known(Game::BlackOps2, Platform::Pc, 0x93, false),
    known(Game::BlackOps2, Platform::Xbox360OrPs3, 0x92, true),
    known(Game::BlackOps2, Platform::WiiU, 0x94, true),
    known(Game::Ghosts, Platform::Pc, 0x235, false),
    known(Game::Ghosts, Platform::Xbox360OrPs3, 0x22E, true),
    known(Game::AdvancedWarfare, Platform::Pc, 0x72E, false),
    known(Game::AdvancedWarfare, Platform::Xbox360OrPs3, 0x72E, true),
    known(Game::BlackOps3, Platform::Pc, 0x1FB, false),
    known(Game::BlackOps3, Platform::Xbox360, 0x253, true),
    known(Game::BlackOps3, Platform::Ps3, 0x25E, true),
];

fn is_magic(magic: &[u8]) -> bool {
    [
        MAGIC_UNSIGNED,
        MAGIC_SIGNED,
        MAGIC_TREYARCH,
        MAGIC_SLEDGEHAMMER,
    ]
    .iter()
    .any(|m| magic == *m)
}

/// A zlib stream's first two bytes: deflate with a window of at most 32 KB,
/// and the pair a multiple of 31 (RFC 1950, section 2.2).
fn is_zlib_header(bytes: &[u8]) -> bool {
    match bytes {
        [cmf, flg, ..] => {
            cmf & 0x0F == 8 && cmf >> 4 <= 7 && ((u16::from(*cmf) << 8) | u16::from(*flg)) % 31 == 0
        }
        _ => false,
    }
}

fn be_u32(bytes: &[u8], at: usize) -> Option<u32> {
    let b = bytes.get(at..at.checked_add(4)?)?;
    Some(u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}

/// A console file's platform. The Xbox 360 and PS3 builds share their
/// numbers. A signed file with its authentication header right after the
/// 12-byte header, or an unsigned one with its zlib stream there (or where
/// Modern Warfare 2 starts it), is the Xbox 360's: checked on the owner's
/// discs. (CoD-FF-Tools describes PS3 files as raw deflate blocks, each after
/// a 2-byte size.) Anything else stays "Xbox 360 or PS3".
fn console_platform(header: &[u8], signed: bool) -> Platform {
    if signed
        && header.get(HEADER_SIZE..HEADER_SIZE + MAGIC_AUTH.len()) == Some(MAGIC_AUTH.as_slice())
    {
        return Platform::Xbox360;
    }
    if !signed
        && CONSOLE_STREAM_STARTS
            .iter()
            .any(|&at| header.get(at..).is_some_and(is_zlib_header))
    {
        return Platform::Xbox360;
    }
    Platform::Xbox360OrPs3
}

/// The game and platform of a fastfile, from its first bytes
/// ([`IDENTIFY_SIZE`] is enough); `None` when it isn't a fastfile we know.
pub fn identify(header: &[u8]) -> Option<Identity> {
    let first = be_u32(header, 0)?;
    let magic = header.get(..8)?;
    if !is_magic(magic) {
        let stream = header.get(COD2_XBOX360_STREAM_START..)?;
        if first == COD2_XBOX360_VERSION && is_zlib_header(stream) {
            return Some(Identity {
                game: Game::CallOfDuty2,
                platform: Platform::Xbox360,
                version: COD2_XBOX360_VERSION,
                big_endian: true,
                signed: false,
                encrypted: false,
            });
        }
        return None;
    }
    let raw = header.get(8..HEADER_SIZE)?;
    let raw = [raw[0], raw[1], raw[2], raw[3]];
    let signed = magic == MAGIC_SIGNED;
    let after = header.get(HEADER_SIZE..).unwrap_or(&[]);
    let near = &after[..after.len().min(ENCRYPTED_MAGIC_WITHIN)];
    let encrypted = near
        .windows(MAGIC_ENCRYPTED.len())
        .any(|w| w == MAGIC_ENCRYPTED);
    for known in VERSIONS {
        let version = if known.big_endian {
            u32::from_be_bytes(raw)
        } else {
            u32::from_le_bytes(raw)
        };
        if version != known.version || !known.game.uses_magic(magic) {
            continue;
        }
        let platform = if known.platform == Platform::Xbox360OrPs3 {
            console_platform(header, signed)
        } else {
            known.platform
        };
        return Some(Identity {
            game: known.game,
            platform,
            version,
            big_endian: known.big_endian,
            signed,
            encrypted,
        });
    }
    None
}

/// Why a fastfile wasn't read.
#[derive(Debug)]
pub enum Error {
    /// Not a fastfile of any game we know.
    NotAFastfile,
    /// A fastfile magic with a version no game here uses.
    UnknownVersion {
        magic: String,
        little_endian: u32,
        big_endian: u32,
    },
    /// The zone is encrypted; we don't read those.
    Encrypted(Identity),
    /// A game or platform whose packing isn't decoded yet (the reason says what).
    NotReadYet(Identity, &'static str),
    Truncated(&'static str),
    Inflate(inflate::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotAFastfile => write!(f, "not a Call of Duty fastfile"),
            Error::UnknownVersion {
                magic,
                little_endian,
                big_endian,
            } => write!(
                f,
                "a {magic} fastfile of a version no game here uses ({little_endian:#x} \
                 little-endian, {big_endian:#x} big-endian)"
            ),
            Error::Encrypted(identity) => {
                write!(f, "{identity}: the zone is encrypted, which we don't read")
            }
            Error::NotReadYet(identity, why) => write!(f, "{identity}: not read yet ({why})"),
            Error::Truncated(what) => write!(f, "file truncated while reading {what}"),
            Error::Inflate(e) => write!(f, "zlib: {e}"),
        }
    }
}

impl std::error::Error for Error {}

/// A zone's `XFile` header.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ZoneHeader {
    /// The streamed data's size (what follows the header).
    pub size: u64,
    pub external_size: u64,
    /// One size per memory block, in the game's block order.
    pub block_sizes: Vec<u64>,
    /// How many bytes the header takes at the start of [`Zone::data`]: 0 when
    /// the file keeps it in front of the compressed data (Call of Duty 2).
    pub length: usize,
}

/// A fastfile's zone, decompressed.
pub struct Zone {
    pub identity: Identity,
    pub header: ZoneHeader,
    /// The `XFile` header (unless the file keeps it outside), then the stream.
    pub data: Vec<u8>,
}

impl Zone {
    /// The streamed data after the header: the asset list, then the assets.
    pub fn stream(&self) -> &[u8] {
        &self.data[self.header.length..]
    }

    /// Whether the header's size is the stream's length. True for every one of
    /// the owner's files of the layouts here; false would mean the layout
    /// table is wrong for this file (or the file is damaged).
    pub fn size_matches(&self) -> bool {
        u64::try_from(self.stream().len()).ok() == Some(self.header.size)
    }
}

/// How a game build packs its zone.
struct Layout {
    /// Where the zlib stream, or a signed file's authentication header, starts.
    start: usize,
    /// The header's fields are 4 or 8 bytes.
    word_size: usize,
    /// How many block sizes the `XFile` header has.
    blocks: usize,
    /// The header sits in front of the compressed data (Call of Duty 2).
    header_in_front: bool,
}

const fn layout(start: usize, word_size: usize, blocks: usize) -> Layout {
    Layout {
        start,
        word_size,
        blocks,
        header_in_front: false,
    }
}

/// Where Modern Warfare 2's Xbox 360 zlib stream (or a signed file's
/// authentication header) starts, if it is where its entry count says.
fn mw2_xbox360_start(file: &[u8], signed: bool) -> Option<usize> {
    let count = usize::try_from(be_u32(file, MW2_XBOX360_COUNT_AT)?).ok()?;
    let start = MW2_XBOX360_STREAM_BASE.checked_add(count.checked_mul(MW2_XBOX360_ENTRY_SIZE)?)?;
    let there = file.get(start..)?;
    let found = if signed {
        there.starts_with(MAGIC_AUTH)
    } else {
        is_zlib_header(there)
    };
    if found {
        Some(start)
    } else {
        None
    }
}

/// The layout of `identity`'s files (block counts measured on the owner's
/// files: the zone's size plus the header's length is the zone's length).
fn layout_of(identity: &mut Identity, file: &[u8]) -> Result<Layout, Error> {
    use Game::*;
    use Platform::*;
    let found = match (identity.game, identity.platform) {
        (CallOfDuty2, Xbox360) => Layout {
            start: COD2_XBOX360_STREAM_START,
            word_size: 4,
            blocks: COD2_XBOX360_BLOCKS,
            header_in_front: true,
        },
        (CallOfDuty4, Pc) => layout(HEADER_SIZE, 4, 9),
        (CallOfDuty4, Xbox360) => layout(HEADER_SIZE, 4, 7),
        (WorldAtWar, Pc) | (WorldAtWar, Xbox360) => layout(HEADER_SIZE, 4, 7),
        (ModernWarfare2, Pc) => layout(IW4_PC_START, 4, 8),
        (ModernWarfare2, Xbox360) | (ModernWarfare2, Xbox360OrPs3) => {
            match mw2_xbox360_start(file, identity.signed) {
                Some(start) => {
                    identity.platform = Xbox360;
                    layout(start, 4, 6)
                }
                None => {
                    return Err(Error::NotReadYet(
                        *identity,
                        "not packed the way the Xbox 360 files are",
                    ))
                }
            }
        }
        (BlackOps, Pc) => layout(HEADER_SIZE, 4, 7),
        (ModernWarfare3, Pc) => layout(IW4_PC_START, 4, 9),
        (Ghosts, Pc) => layout(GHOSTS_PC_START, 8, 7),
        _ => {
            return Err(Error::NotReadYet(
                *identity,
                "this game and platform's packing isn't decoded yet",
            ))
        }
    };
    Ok(found)
}

/// The data chunks of a signed file joined: everything after the
/// authentication chunk at `auth_start` except the hash chunks.
fn signed_data(file: &[u8], auth_start: usize) -> Vec<u8> {
    let mut data = Vec::with_capacity(file.len());
    let mut hash_chunk = auth_start + SIGNED_CHUNK_SIZE;
    loop {
        let start = hash_chunk + SIGNED_CHUNK_SIZE;
        if start >= file.len() {
            break;
        }
        let end = (start + SIGNED_CHUNK_SIZE * SIGNED_CHUNKS_PER_GROUP).min(file.len());
        data.extend_from_slice(&file[start..end]);
        hash_chunk = end;
    }
    data
}

/// A header field of `word_size` bytes in the zone's byte order.
fn read_word(data: &[u8], at: usize, word_size: usize, big_endian: bool) -> Option<u64> {
    let bytes = data.get(at..at.checked_add(word_size)?)?;
    let mut value = 0u64;
    if big_endian {
        for &b in bytes {
            value = (value << 8) | u64::from(b);
        }
    } else {
        for &b in bytes.iter().rev() {
            value = (value << 8) | u64::from(b);
        }
    }
    Some(value)
}

/// The `XFile` header at the start of `data`: size, external size, then the
/// block sizes.
fn xfile_header(data: &[u8], found: &Layout, big_endian: bool) -> Result<ZoneHeader, Error> {
    let word = |i: usize| {
        read_word(data, i * found.word_size, found.word_size, big_endian)
            .ok_or(Error::Truncated("zone header"))
    };
    let size = word(0)?;
    let external_size = word(1)?;
    let mut block_sizes = Vec::with_capacity(found.blocks);
    for i in 0..found.blocks {
        block_sizes.push(word(2 + i)?);
    }
    Ok(ZoneHeader {
        size,
        external_size,
        block_sizes,
        length: (2 + found.blocks) * found.word_size,
    })
}

/// Call of Duty 2's header, in front of the compressed data: the version,
/// the zone's size, then the block sizes (big-endian u32s).
fn front_header(file: &[u8], found: &Layout) -> Result<ZoneHeader, Error> {
    let word = |i: usize| {
        be_u32(file, 4 * i)
            .map(u64::from)
            .ok_or(Error::Truncated("header"))
    };
    let size = word(1)?;
    let mut block_sizes = Vec::with_capacity(found.blocks);
    for i in 0..found.blocks {
        block_sizes.push(word(2 + i)?);
    }
    Ok(ZoneHeader {
        size,
        external_size: 0,
        block_sizes,
        length: 0,
    })
}

/// The error for a file [`identify`] doesn't know.
fn not_known(file: &[u8]) -> Error {
    match (file.get(..8), file.get(8..HEADER_SIZE)) {
        (Some(magic), Some(raw)) if is_magic(magic) => {
            let raw = [raw[0], raw[1], raw[2], raw[3]];
            Error::UnknownVersion {
                magic: String::from_utf8_lossy(magic).into_owned(),
                little_endian: u32::from_le_bytes(raw),
                big_endian: u32::from_be_bytes(raw),
            }
        }
        _ => Error::NotAFastfile,
    }
}

/// Reads a fastfile of any game whose packing is decoded (see the module
/// documentation) into its zone.
pub fn read(file: &[u8]) -> Result<Zone, Error> {
    let head = &file[..file.len().min(IDENTIFY_SIZE)];
    let mut identity = identify(head).ok_or_else(|| not_known(file))?;
    if identity.encrypted {
        return Err(Error::Encrypted(identity));
    }
    let found = layout_of(&mut identity, file)?;
    // A zone is rarely more than a few times its packed size; the vector grows
    // past the hint when it is.
    let size_hint = file.len().saturating_mul(3);
    let data = if identity.signed {
        if file.get(found.start..found.start + MAGIC_AUTH.len()) != Some(MAGIC_AUTH.as_slice()) {
            return Err(Error::NotReadYet(
                identity,
                "signed, but not the authentication header the other games have",
            ));
        }
        let joined = signed_data(file, found.start);
        if joined.is_empty() {
            return Err(Error::Truncated("signed data"));
        }
        inflate::inflate_zlib(&joined, size_hint).map_err(Error::Inflate)?
    } else {
        let stream = file
            .get(found.start..)
            .ok_or(Error::Truncated("zlib stream"))?;
        if !is_zlib_header(stream) {
            return Err(Error::NotReadYet(
                identity,
                "no zlib stream where this game's files have one",
            ));
        }
        inflate::inflate_zlib(stream, size_hint).map_err(Error::Inflate)?
    };
    let header = if found.header_in_front {
        front_header(file, &found)?
    } else {
        xfile_header(&data, &found, identity.big_endian)?
    };
    if header.length > data.len() {
        return Err(Error::Truncated("zone header"));
    }
    Ok(Zone {
        identity,
        header,
        data,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// zlib with one stored (uncompressed) block, so the tests need no compressor.
    fn zlib_stored(data: &[u8]) -> Vec<u8> {
        let mut out = vec![0x78, 0x01, 0x01];
        out.extend_from_slice(&(data.len() as u16).to_le_bytes());
        out.extend_from_slice(&(!(data.len() as u16)).to_le_bytes());
        out.extend_from_slice(data);
        let (mut a, mut b) = (1u32, 0u32);
        for &x in data {
            a = (a + x as u32) % 65521;
            b = (b + a) % 65521;
        }
        out.extend_from_slice(&((b << 16) | a).to_be_bytes());
        out
    }

    fn word(value: u64, size: usize, big_endian: bool) -> Vec<u8> {
        let bytes = value.to_be_bytes();
        let mut out = bytes[8 - size..].to_vec();
        if !big_endian {
            out.reverse();
        }
        out
    }

    /// A zone: its XFile header (`blocks` sizes of `word_size` bytes) and `stream`.
    fn zone(stream: &[u8], word_size: usize, blocks: usize, big_endian: bool) -> Vec<u8> {
        let mut z = word(stream.len() as u64, word_size, big_endian);
        z.extend(word(0, word_size, big_endian));
        for i in 0..blocks {
            z.extend(word(i as u64 + 1, word_size, big_endian));
        }
        z.extend_from_slice(stream);
        z
    }

    fn version(magic: &[u8; 8], version: u32, big_endian: bool) -> Vec<u8> {
        let mut file = magic.to_vec();
        file.extend(word(u64::from(version), 4, big_endian));
        file
    }

    /// Reads `file` and checks the game, platform, block count and stream.
    fn check(file: &[u8], game: Game, platform: Platform, blocks: usize, stream: &[u8]) {
        let read_zone = read(file).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(read_zone.identity.game, game);
        assert_eq!(read_zone.identity.platform, platform);
        assert_eq!(read_zone.header.block_sizes.len(), blocks);
        assert_eq!(read_zone.header.block_sizes[0], 1);
        assert_eq!(read_zone.stream(), stream);
        assert!(read_zone.size_matches());
    }

    const STREAM: &[u8] = b"asset list and assets";

    #[test]
    fn pc_games() {
        let mut cod4 = version(MAGIC_UNSIGNED, 5, false);
        cod4.extend(zlib_stored(&zone(STREAM, 4, 9, false)));
        check(&cod4, Game::CallOfDuty4, Platform::Pc, 9, STREAM);

        let mut waw = version(MAGIC_UNSIGNED, 387, false);
        waw.extend(zlib_stored(&zone(STREAM, 4, 7, false)));
        check(&waw, Game::WorldAtWar, Platform::Pc, 7, STREAM);
        let identity = identify(&waw).unwrap();
        assert_eq!(
            identity.to_string(),
            "Call of Duty: World at War (PC, version 387)"
        );

        for (v, game, blocks) in [
            (0x114, Game::ModernWarfare2, 8),
            (0x1, Game::ModernWarfare3, 9),
        ] {
            let mut file = version(MAGIC_UNSIGNED, v, false);
            file.push(1); // online update flag
            file.extend([0u8; 8]); // file time
            file.extend(zlib_stored(&zone(STREAM, 4, blocks, false)));
            check(&file, game, Platform::Pc, blocks, STREAM);
        }

        let mut bo1 = version(MAGIC_UNSIGNED, 0x1D9, false);
        bo1.extend(zlib_stored(&zone(STREAM, 4, 7, false)));
        check(&bo1, Game::BlackOps, Platform::Pc, 7, STREAM);

        let mut ghosts = version(MAGIC_UNSIGNED, 0x235, false);
        ghosts.extend([1u8, 0, 8, 4]);
        ghosts.extend([0u8; 28]);
        ghosts.extend(zlib_stored(&zone(STREAM, 8, 7, false)));
        check(&ghosts, Game::Ghosts, Platform::Pc, 7, STREAM);
    }

    #[test]
    fn byte_order_tells_cod4_console_from_mw3_pc() {
        let mut cod4 = version(MAGIC_UNSIGNED, 1, true);
        cod4.extend(zlib_stored(&zone(STREAM, 4, 7, true)));
        check(&cod4, Game::CallOfDuty4, Platform::Xbox360, 7, STREAM);
        let mw3 = version(MAGIC_UNSIGNED, 1, false);
        assert_eq!(identify(&mw3).map(|i| i.game), Some(Game::ModernWarfare3));
    }

    /// A signed file: `before` bytes after the version, the authentication
    /// chunk, then the zlib stream in groups behind hash chunks.
    fn signed(magic_version: Vec<u8>, before: &[u8], stream: &[u8]) -> Vec<u8> {
        let mut file = magic_version;
        file.extend_from_slice(before);
        let auth_start = file.len();
        file.extend_from_slice(MAGIC_AUTH);
        file.resize(auth_start + SIGNED_CHUNK_SIZE, 0xAA);
        let group = SIGNED_CHUNK_SIZE * SIGNED_CHUNKS_PER_GROUP;
        for part in stream.chunks(group) {
            file.resize(file.len() + SIGNED_CHUNK_SIZE, 0xEE); // hash chunk
            file.extend_from_slice(part);
        }
        file
    }

    #[test]
    fn signed_files() {
        // World at War's Xbox 360 multiplayer files: the authentication header at byte 12.
        let packed = zlib_stored(&zone(STREAM, 4, 7, true));
        let waw = signed(version(MAGIC_SIGNED, 387, true), &[], &packed);
        check(&waw, Game::WorldAtWar, Platform::Xbox360, 7, STREAM);
        assert!(read(&waw).unwrap().identity.signed);

        // Modern Warfare 2 on PC: at byte 21.
        let packed = zlib_stored(&zone(STREAM, 4, 8, false));
        let mw2 = signed(version(MAGIC_SIGNED, 0x114, false), &[1u8; 9], &packed);
        check(&mw2, Game::ModernWarfare2, Platform::Pc, 8, STREAM);
    }

    #[test]
    fn signed_data_skips_hash_chunks() {
        let group = SIGNED_CHUNK_SIZE * SIGNED_CHUNKS_PER_GROUP;
        let payload: Vec<u8> = (0..group + 100).map(|i| (i % 251) as u8).collect();
        let file = signed(version(MAGIC_SIGNED, 387, true), &[], &payload);
        assert_eq!(signed_data(&file, HEADER_SIZE), payload);
    }

    #[test]
    fn mw2_xbox360_entries() {
        for entries in [0u32, 3] {
            let mut file = version(MAGIC_UNSIGNED, 0x10D, true);
            file.push(1); // online update flag
            file.extend([0u8; 8]); // file time
            file.extend(1u32.to_be_bytes()); // region
            file.extend(entries.to_be_bytes());
            for _ in 0..entries {
                file.extend([7u8; MW2_XBOX360_ENTRY_SIZE]);
            }
            file.extend([0u8; 8]); // two sizes
            file.extend(zlib_stored(&zone(STREAM, 4, 6, true)));
            check(&file, Game::ModernWarfare2, Platform::Xbox360, 6, STREAM);
        }
    }

    #[test]
    fn mw2_xbox360_signed() {
        let mut before = vec![1u8]; // online update flag
        before.extend([0u8; 8]); // file time
        before.extend(1u32.to_be_bytes()); // region
        before.extend(2u32.to_be_bytes()); // two entries
        before.extend([7u8; 2 * MW2_XBOX360_ENTRY_SIZE]);
        before.extend([0u8; 8]); // two sizes
        let packed = zlib_stored(&zone(STREAM, 4, 6, true));
        let file = signed(version(MAGIC_SIGNED, 0x10D, true), &before, &packed);
        check(&file, Game::ModernWarfare2, Platform::Xbox360, 6, STREAM);
    }

    #[test]
    fn cod2_xbox360_keeps_its_header_in_front() {
        let mut file = COD2_XBOX360_VERSION.to_be_bytes().to_vec();
        file.extend((STREAM.len() as u32).to_be_bytes());
        for size in [1u32, 2, 3] {
            file.extend(size.to_be_bytes());
        }
        file.extend(zlib_stored(STREAM));
        let read_zone = read(&file).unwrap();
        assert_eq!(read_zone.identity.game, Game::CallOfDuty2);
        assert_eq!(read_zone.header.block_sizes, vec![1, 2, 3]);
        assert_eq!(read_zone.stream(), STREAM);
        assert!(read_zone.size_matches());
        // Without its zlib stream at byte 20 it isn't taken for one.
        let mut other = COD2_XBOX360_VERSION.to_be_bytes().to_vec();
        other.extend([0u8; 40]);
        assert!(identify(&other).is_none());
    }

    #[test]
    fn encrypted_and_unknown() {
        let mut bo2 = version(MAGIC_TREYARCH, 0x93, false);
        bo2.extend_from_slice(MAGIC_ENCRYPTED);
        bo2.extend_from_slice(b"\0\0\0\0weapons\0");
        let identity = identify(&bo2).unwrap();
        assert_eq!(identity.game, Game::BlackOps2);
        assert!(identity.encrypted);
        assert!(matches!(read(&bo2), Err(Error::Encrypted(_))));

        // Black Ops II's version under the IW magic is nothing we know.
        let odd = version(MAGIC_UNSIGNED, 0x93, false);
        assert!(identify(&odd).is_none());
        assert!(matches!(
            read(&odd),
            Err(Error::UnknownVersion {
                little_endian: 0x93,
                ..
            })
        ));
        assert!(matches!(read(b"KAPF and more"), Err(Error::NotAFastfile)));
        assert!(matches!(read(b""), Err(Error::NotAFastfile)));
    }

    #[test]
    fn consoles_without_a_zlib_stream_stay_unsure() {
        // PS3 files are packed in blocks; nothing here says which console.
        let mut waw = version(MAGIC_UNSIGNED, 387, true);
        waw.extend([0x40u8, 0x00]);
        waw.extend([0u8; 30]);
        let identity = identify(&waw).unwrap();
        assert_eq!(identity.platform, Platform::Xbox360OrPs3);
        assert!(matches!(read(&waw), Err(Error::NotReadYet(..))));
    }

    #[test]
    fn every_version_is_found() {
        for known in VERSIONS {
            let magic = match known.game {
                Game::BlackOps2 | Game::BlackOps3 => MAGIC_TREYARCH,
                Game::AdvancedWarfare => MAGIC_SLEDGEHAMMER,
                _ => MAGIC_UNSIGNED,
            };
            let file = version(magic, known.version, known.big_endian);
            let identity = identify(&file).unwrap();
            assert_eq!(
                (identity.game, identity.version),
                (known.game, known.version)
            );
        }
    }
}
