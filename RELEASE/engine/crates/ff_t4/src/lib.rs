//! Call of Duty: World at War (T4) fastfile reader, for the PC and Xbox 360
//! versions.
//!
//! Containers (version 387 in both games):
//!
//! - **PC:** `"IWffu100"`, a little-endian u32 version, then one zlib stream
//!   holding the zone.
//! - **Xbox 360, unsigned** (campaign and zombies): the same, with a
//!   big-endian version.
//! - **Xbox 360, signed** (multiplayer): `"IWff0100"` and the big-endian
//!   version, then 0x2000-byte chunks counted from byte 12:
//!   - chunk 0 is the `"IWffs100"` authentication header: a 32-byte hash, a
//!     256-byte signature, the zone name (32 bytes), a u32, and one SHA-256
//!     per group (the hash of that group's hash chunk);
//!   - then groups of one hash chunk (a SHA-256 for each of the next 256
//!     chunks) and up to 256 data chunks (2 MB).
//!
//!   Joining the data chunks gives an ordinary zlib stream.
//!
//!   Found by us in the owner's disc (2026-10-07). Checked on `ui_mp.ff` and
//!   `mp_seelow.ff`: the Adler-32 matches, the zone is exactly its declared
//!   size plus the 36-byte `XFile` header, and the SHA-256 of the first data
//!   chunk and of the first hash chunk are where this layout says.
//!
//! Zone (little-endian on PC, big-endian on Xbox 360): an `XFile` header
//! (`size`, `externalSize`, `blockSize[7]`) followed
//! by the streamed `XAssetList`:
//! `{ ScriptStringList { int count; const char** strings; }, int assetCount, XAsset* assets }`.
//! A pointer value of -1 (0xFFFFFFFF) means "the data follows inline in the
//! stream"; -2 means "inline and registered for later reuse"; other values
//! are references to data already loaded (block index in the top 4 bits,
//! offset + 1 below, as in the other IW-engine zone formats).
//!
//! Structures and the asset type order follow OpenAssetTools' T4 definitions
//! (`src/Common/Game/T4/T4.h`, `T4_Assets.h`); see docs/SOURCES.md.
//!
//! Status: container, header, script strings and the asset table are read.
//! Raw files (GSC/CSC scripts, configs) are located by their inline layout.
//! The full streaming loader for every asset type is the next step.

#![forbid(unsafe_code)]

use std::fmt;

pub const MAGIC_UNSIGNED: &[u8; 8] = b"IWffu100";
pub const MAGIC_SIGNED: &[u8; 8] = b"IWff0100";
pub const MAGIC_AUTH: &[u8; 8] = b"IWffs100";
/// Retail World at War, PC and Xbox 360.
pub const VERSION: u32 = 387;
/// Signed Xbox 360 fastfiles are cut into chunks counted from the end of the
/// 12-byte container header: the authentication header takes one chunk, and
/// each group of data chunks is preceded by one chunk of their hashes.
const SIGNED_CHUNK_SIZE: usize = 0x2000;
const SIGNED_CHUNKS_PER_GROUP: usize = 256;
const SIGNED_GROUP_DATA_SIZE: usize = SIGNED_CHUNK_SIZE * SIGNED_CHUNKS_PER_GROUP;
const CONTAINER_HEADER_SIZE: usize = 12;
/// The Xbox 360 asset type with no PC equivalent (see `AssetType::from_raw`).
const XBOX360_EXTRA_TYPE: u32 = 7;
pub const BLOCK_COUNT: usize = 7;
pub const PTR_FOLLOWING: u32 = 0xFFFF_FFFF;
pub const PTR_INSERT: u32 = 0xFFFF_FFFE;
/// Most entries reserved up front for a count read from a file.
const MAX_PREALLOC: usize = 1 << 16;

/// T4 asset types, in the game's order.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum AssetType {
    XModelPieces = 0,
    PhysPreset,
    PhysConstraints,
    DestructibleDef,
    XAnimParts,
    XModel,
    Material,
    TechniqueSet,
    Image,
    Sound,
    LoadedSound,
    ClipMap,
    ClipMapPvs,
    ComWorld,
    GameWorldSp,
    GameWorldMp,
    MapEnts,
    GfxWorld,
    LightDef,
    UiMap,
    Font,
    MenuList,
    Menu,
    LocalizeEntry,
    Weapon,
    SndDriverGlobals,
    Fx,
    ImpactFx,
    AiType,
    MpType,
    Character,
    XModelAlias,
    RawFile,
    StringTable,
    PackIndex,
}

impl AssetType {
    pub const COUNT: u32 = 35;

    /// The asset type a zone's type number means on `platform`.
    ///
    /// The Xbox 360 list has one extra type at 7 (not seen in any zone so
    /// far; probably the pixel shaders, which consoles keep apart), so its
    /// numbers from 8 on are one higher than on PC. Checked on
    /// `code_post_gfx.ff`: every type count matches the PC zone shifted by one.
    pub fn from_raw(v: u32, platform: Platform) -> Option<Self> {
        match platform {
            Platform::Pc => Self::from_u32(v),
            Platform::Xbox360 if v < XBOX360_EXTRA_TYPE => Self::from_u32(v),
            Platform::Xbox360 if v == XBOX360_EXTRA_TYPE => None,
            Platform::Xbox360 => Self::from_u32(v - 1),
        }
    }

    /// Every asset type, in number order (index = type number).
    const ALL: [AssetType; Self::COUNT as usize] = [
        Self::XModelPieces,
        Self::PhysPreset,
        Self::PhysConstraints,
        Self::DestructibleDef,
        Self::XAnimParts,
        Self::XModel,
        Self::Material,
        Self::TechniqueSet,
        Self::Image,
        Self::Sound,
        Self::LoadedSound,
        Self::ClipMap,
        Self::ClipMapPvs,
        Self::ComWorld,
        Self::GameWorldSp,
        Self::GameWorldMp,
        Self::MapEnts,
        Self::GfxWorld,
        Self::LightDef,
        Self::UiMap,
        Self::Font,
        Self::MenuList,
        Self::Menu,
        Self::LocalizeEntry,
        Self::Weapon,
        Self::SndDriverGlobals,
        Self::Fx,
        Self::ImpactFx,
        Self::AiType,
        Self::MpType,
        Self::Character,
        Self::XModelAlias,
        Self::RawFile,
        Self::StringTable,
        Self::PackIndex,
    ];

    /// The asset type a PC zone's type number means (a table lookup: no
    /// unsafe code reads the number from the file).
    pub fn from_u32(v: u32) -> Option<Self> {
        Self::ALL.get(usize::try_from(v).ok()?).copied()
    }

    pub fn name(self) -> &'static str {
        const NAMES: [&str; 35] = [
            "xmodelpieces",
            "physpreset",
            "physconstraints",
            "destructibledef",
            "xanim",
            "xmodel",
            "material",
            "techniqueset",
            "image",
            "sound",
            "loaded_sound",
            "clipmap",
            "clipmap_pvs",
            "comworld",
            "gameworld_sp",
            "gameworld_mp",
            "map_ents",
            "gfxworld",
            "lightdef",
            "ui_map",
            "font",
            "menulist",
            "menu",
            "localize",
            "weapon",
            "snddriverglobals",
            "fx",
            "impactfx",
            "aitype",
            "mptype",
            "character",
            "xmodelalias",
            "rawfile",
            "stringtable",
            "packindex",
        ];
        NAMES[self as usize]
    }
}

/// Which game build a fastfile comes from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Platform {
    Pc,
    Xbox360,
}

impl Platform {
    pub fn name(self) -> &'static str {
        match self {
            Platform::Pc => "PC",
            Platform::Xbox360 => "Xbox 360",
        }
    }
}

#[derive(Debug)]
pub enum Error {
    NotAFastfile,
    UnsupportedVersion(u32),
    Inflate(inflate::Error),
    Truncated(&'static str),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotAFastfile => write!(
                f,
                "not a World at War fastfile (expected IWffu100 or IWff0100)"
            ),
            Error::UnsupportedVersion(v) => {
                write!(f, "unsupported fastfile version {v} (expected {VERSION})")
            }
            Error::Inflate(e) => write!(f, "zlib: {e}"),
            Error::Truncated(what) => write!(f, "zone truncated while reading {what}"),
        }
    }
}

impl std::error::Error for Error {}

/// A fastfile's zone bytes and where they come from.
pub struct Container {
    pub platform: Platform,
    /// Xbox 360 multiplayer fastfiles are signed.
    pub signed: bool,
    pub zone: Vec<u8>,
}

/// Which platform the version field's byte order says, if it is 387.
fn platform_of(version: [u8; 4]) -> Result<Platform, Error> {
    if u32::from_le_bytes(version) == VERSION {
        Ok(Platform::Pc)
    } else if u32::from_be_bytes(version) == VERSION {
        Ok(Platform::Xbox360)
    } else {
        Err(Error::UnsupportedVersion(u32::from_le_bytes(version)))
    }
}

/// The data of a signed fastfile: its data chunks joined, without the
/// authentication header and the hash chunks.
fn signed_data(file: &[u8]) -> Vec<u8> {
    let mut data = Vec::with_capacity(file.len());
    // Skip the container header and the authentication header chunk.
    let mut hash_chunk = CONTAINER_HEADER_SIZE + SIGNED_CHUNK_SIZE;
    loop {
        let start = hash_chunk + SIGNED_CHUNK_SIZE;
        if start >= file.len() {
            break;
        }
        let end = (start + SIGNED_GROUP_DATA_SIZE).min(file.len());
        data.extend_from_slice(&file[start..end]);
        hash_chunk = end;
    }
    data
}

/// Decompresses a PC or Xbox 360 fastfile into its zone bytes.
pub fn read_container(file: &[u8]) -> Result<Container, Error> {
    if file.len() < CONTAINER_HEADER_SIZE {
        return Err(Error::NotAFastfile);
    }
    let magic = &file[..8];
    let platform = platform_of(file[8..12].try_into().unwrap())?;
    if magic == MAGIC_UNSIGNED {
        let zone = inflate::inflate_zlib(&file[CONTAINER_HEADER_SIZE..], file.len() * 3)
            .map_err(Error::Inflate)?;
        return Ok(Container {
            platform,
            signed: false,
            zone,
        });
    }
    if magic == MAGIC_SIGNED && file.get(12..20) == Some(MAGIC_AUTH.as_slice()) {
        let data = signed_data(file);
        if data.is_empty() {
            return Err(Error::Truncated("signed data"));
        }
        let zone = inflate::inflate_zlib(&data, file.len() * 3).map_err(Error::Inflate)?;
        return Ok(Container {
            platform,
            signed: true,
            zone,
        });
    }
    Err(Error::NotAFastfile)
}

#[derive(Debug, Clone)]
pub struct ZoneHeader {
    pub size: u32,
    pub external_size: u32,
    pub block_size: [u32; BLOCK_COUNT],
}

#[derive(Debug, Clone)]
pub struct AssetEntry {
    pub kind: Option<AssetType>,
    pub raw_type: u32,
    pub header_ptr: u32,
}

#[derive(Debug, Clone)]
pub struct RawFile {
    pub name: String,
    pub data: Vec<u8>,
}

/// A decompressed zone with its header, script strings and asset table.
pub struct Zone {
    pub platform: Platform,
    pub data: Vec<u8>,
    pub header: ZoneHeader,
    pub script_strings: Vec<Option<String>>,
    pub assets: Vec<AssetEntry>,
    /// Stream offset of the first asset's data (right after the asset table).
    pub assets_offset: usize,
}

struct Cursor<'a> {
    d: &'a [u8],
    p: usize,
    big_endian: bool,
}

/// A u32 in the zone's byte order.
fn read_u32(bytes: [u8; 4], big_endian: bool) -> u32 {
    if big_endian {
        u32::from_be_bytes(bytes)
    } else {
        u32::from_le_bytes(bytes)
    }
}

impl<'a> Cursor<'a> {
    fn u32(&mut self, what: &'static str) -> Result<u32, Error> {
        let b = self
            .d
            .get(self.p..self.p + 4)
            .ok_or(Error::Truncated(what))?;
        self.p += 4;
        Ok(read_u32(b.try_into().unwrap(), self.big_endian))
    }
    fn cstr(&mut self, what: &'static str) -> Result<String, Error> {
        let rest = self.d.get(self.p..).ok_or(Error::Truncated(what))?;
        let n = rest
            .iter()
            .position(|&b| b == 0)
            .ok_or(Error::Truncated(what))?;
        let s = String::from_utf8_lossy(&rest[..n]).into_owned();
        self.p += n + 1;
        Ok(s)
    }
}

impl Zone {
    pub fn from_fastfile(file: &[u8]) -> Result<Self, Error> {
        let container = read_container(file)?;
        Self::from_zone(container.zone, container.platform)
    }

    pub fn from_zone(data: Vec<u8>, platform: Platform) -> Result<Self, Error> {
        let mut c = Cursor {
            d: &data,
            p: 0,
            big_endian: platform == Platform::Xbox360,
        };
        let size = c.u32("header")?;
        let external_size = c.u32("header")?;
        let mut block_size = [0u32; BLOCK_COUNT];
        for b in &mut block_size {
            *b = c.u32("header")?;
        }
        // XAssetList
        let string_count = c.u32("asset list")? as usize;
        let strings_ptr = c.u32("asset list")?;
        let asset_count = c.u32("asset list")? as usize;
        let assets_ptr = c.u32("asset list")?;
        // Counts come from the file: the vectors grow as entries are read,
        // so a bogus count fails on the data instead of a huge allocation.
        let mut script_strings = Vec::with_capacity(string_count.min(MAX_PREALLOC));
        if strings_ptr == PTR_FOLLOWING {
            let mut ptrs = Vec::with_capacity(string_count.min(MAX_PREALLOC));
            for _ in 0..string_count {
                ptrs.push(c.u32("script strings")?);
            }
            for p in ptrs {
                script_strings.push(if p == PTR_FOLLOWING {
                    Some(c.cstr("script string")?)
                } else {
                    None
                });
            }
        }
        let mut assets = Vec::with_capacity(asset_count.min(MAX_PREALLOC));
        if assets_ptr == PTR_FOLLOWING {
            for _ in 0..asset_count {
                let raw_type = c.u32("asset table")?;
                let header_ptr = c.u32("asset table")?;
                assets.push(AssetEntry {
                    kind: AssetType::from_raw(raw_type, platform),
                    raw_type,
                    header_ptr,
                });
            }
        }
        let assets_offset = c.p;
        Ok(Self {
            platform,
            header: ZoneHeader {
                size,
                external_size,
                block_size,
            },
            script_strings,
            assets,
            assets_offset,
            data,
        })
    }

    /// The name of a zone's asset type number ("?" if unknown).
    pub fn type_name(&self, raw_type: u32) -> &'static str {
        AssetType::from_raw(raw_type, self.platform)
            .map(|t| t.name())
            .unwrap_or("?")
    }

    /// Asset counts per raw type number, in type order.
    pub fn census(&self) -> Vec<(u32, usize)> {
        let mut counts = std::collections::BTreeMap::new();
        for a in &self.assets {
            *counts.entry(a.raw_type).or_insert(0usize) += 1;
        }
        counts.into_iter().collect()
    }

    /// Finds raw files by their inline layout: `{ name = -1, len, buffer = -1 }`,
    /// the name string, then `len` bytes and a terminating zero. Only used until
    /// the full streaming loader exists; every hit is validated (printable path
    /// name with an extension, NUL right after the data).
    pub fn scan_raw_files(&self) -> Vec<RawFile> {
        let d = &self.data;
        let mut out = Vec::new();
        let mut seen = std::collections::HashSet::new();
        let mut i = self.assets_offset;
        while i + 13 <= d.len() {
            // Structs are aligned to their block offset, not the stream offset,
            // so every byte position is a candidate.
            if d[i] != 0xFF || d[i..i + 4] != [0xFF; 4] || d[i + 8..i + 12] != [0xFF; 4] {
                i += 1;
                continue;
            }
            let len = read_u32(
                d[i + 4..i + 8].try_into().unwrap(),
                self.platform == Platform::Xbox360,
            ) as usize;
            let name_start = i + 12;
            let Some(nlen) = d[name_start..d.len().min(name_start + 256)]
                .iter()
                .position(|&b| b == 0)
            else {
                i += 1;
                continue;
            };
            let name = &d[name_start..name_start + nlen];
            let valid_name = nlen >= 3
                && name.iter().all(|&b| b.is_ascii_graphic())
                && name.contains(&b'.')
                && !name.contains(&b'\\');
            let data_start = name_start + nlen + 1;
            if !valid_name || len == 0 || data_start + len >= d.len() || d[data_start + len] != 0 {
                i += 1;
                continue;
            }
            let name = String::from_utf8_lossy(name).into_owned();
            if seen.insert(name.clone()) {
                out.push(RawFile {
                    name,
                    data: d[data_start..data_start + len].to_vec(),
                });
            }
            i = data_start + len + 1;
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn asset_type_numbers_match_the_table() {
        for v in 0..AssetType::COUNT {
            assert_eq!(AssetType::from_u32(v).map(|t| t as u32), Some(v));
        }
        assert_eq!(AssetType::from_u32(AssetType::COUNT), None);
        assert_eq!(AssetType::from_u32(u32::MAX), None);
    }

    /// A tiny zone holding one raw file, in the given byte order.
    fn zone_with_rawfile(big_endian: bool) -> Vec<u8> {
        let word = |v: u32| {
            if big_endian {
                v.to_be_bytes()
            } else {
                v.to_le_bytes()
            }
        };
        let mut z = Vec::new();
        for v in [0u32; 9] {
            z.extend_from_slice(&word(v)); // XFile header
        }
        for v in [0, 0, 1, PTR_FOLLOWING] {
            z.extend_from_slice(&word(v)); // no script strings, one asset
        }
        for v in [AssetType::RawFile as u32, PTR_FOLLOWING] {
            z.extend_from_slice(&word(v));
        }
        for v in [PTR_FOLLOWING, 5, PTR_FOLLOWING] {
            z.extend_from_slice(&word(v)); // RawFile { name, len, buffer }
        }
        z.extend_from_slice(b"maps/x.gsc\0hello\0");
        z
    }

    /// zlib with one stored (uncompressed) block, so the test needs no compressor.
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

    #[test]
    fn pc_and_xbox_360_unsigned() {
        for (platform, big_endian) in [(Platform::Pc, false), (Platform::Xbox360, true)] {
            let mut file = MAGIC_UNSIGNED.to_vec();
            file.extend_from_slice(&if big_endian {
                VERSION.to_be_bytes()
            } else {
                VERSION.to_le_bytes()
            });
            file.extend(zlib_stored(&zone_with_rawfile(big_endian)));
            let zone = Zone::from_fastfile(&file).unwrap();
            assert_eq!(zone.platform, platform);
            assert_eq!(zone.assets.len(), 1);
            let raw = zone.scan_raw_files();
            assert_eq!(raw[0].name, "maps/x.gsc");
            assert_eq!(raw[0].data, b"hello");
        }
    }

    #[test]
    fn xbox_360_signed_skips_hash_chunks() {
        // Data that spans two groups, so a hash chunk sits in the middle.
        let payload: Vec<u8> = (0..SIGNED_GROUP_DATA_SIZE + 100)
            .map(|i| (i % 251) as u8)
            .collect();
        let header = |file: &mut Vec<u8>| {
            file.extend_from_slice(MAGIC_SIGNED);
            file.extend_from_slice(&VERSION.to_be_bytes());
            file.extend_from_slice(MAGIC_AUTH);
            file.resize(CONTAINER_HEADER_SIZE + 2 * SIGNED_CHUNK_SIZE, 0xAA);
        };
        let mut file = Vec::new();
        header(&mut file);
        file.extend_from_slice(&payload[..SIGNED_GROUP_DATA_SIZE]);
        file.extend(std::iter::repeat(0xEE).take(SIGNED_CHUNK_SIZE));
        file.extend_from_slice(&payload[SIGNED_GROUP_DATA_SIZE..]);
        assert_eq!(signed_data(&file), payload);

        // A whole signed file with a small zone.
        let mut small = Vec::new();
        header(&mut small);
        small.extend(zlib_stored(&zone_with_rawfile(true)));
        let container = read_container(&small).unwrap();
        assert!(container.signed);
        assert_eq!(container.platform, Platform::Xbox360);
        assert_eq!(container.zone, zone_with_rawfile(true));
    }
}
