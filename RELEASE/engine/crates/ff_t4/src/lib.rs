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
//! The containers are read by the `fastfile` crate, which knows every game's
//! fastfiles: a file from another game (Call of Duty 4, Black Ops, ... which
//! share the magic) is refused here as that game's, never misread as World at
//! War's ([`Error::OtherGame`]).
//!
//! Zone (little-endian on PC, big-endian on Xbox 360): an `XFile` header
//! (`size`, `externalSize`, `blockSize[7]`) followed
//! by the streamed `XAssetList`:
//! `{ ScriptStringList { int count; const char** strings; }, int assetCount, XAsset* assets }`.
//! A pointer value of -1 (0xFFFFFFFF) means "the data follows inline in the
//! stream"; -2 means "inline, with a slot that will hold it"; other values
//! are references to data already loaded (block index in the top 3 bits,
//! offset + 1 below; `Load_XModelPtr` 0x00480840 in CoDWaW.exe 1.7).
//!
//! **Every asset of a PC zone** is read by [`Zone::read_assets`] (`zone.rs`)
//! the way the game's own loader does (blocks, inline data, references,
//! inserted slots; read from CoDWaW.exe 1.7), following a layout file
//! (`layout.rs`) that says what the loader reads, structure by structure. The
//! reader checks a zone completely: the stream ends exactly at the zone's
//! end, every block ends at the size the header gives, every reference lands
//! on something loaded before. World at War PC's layout is not part of this
//! crate (it was made with a type header that is not published; see
//! docs/formats/waw.md); with it, all 129 fastfiles of the PC game read
//! exactly. Xbox 360 zones are read up to the asset table; their raw files
//! are found by their inline layout ([`Zone::scan_raw_files`]).
//!
//! The asset type order follows OpenAssetTools' T4 definitions; see
//! docs/formats/waw.md and docs/SOURCES.md.

#![forbid(unsafe_code)]

pub mod layout;
pub mod zone;

use std::fmt;

pub use zone::{LoadedAsset, LoadedZone, ZoneError};

pub use fastfile::{MAGIC_AUTH, MAGIC_SIGNED, MAGIC_UNSIGNED};
/// Retail World at War, PC and Xbox 360.
pub const VERSION: u32 = 387;
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
    /// Not a fastfile we can read (`fastfile` says why).
    Fastfile(fastfile::Error),
    /// Another game's fastfile, or World at War's for a platform this reader
    /// doesn't know (Wii, PS3).
    OtherGame(fastfile::Identity),
    Truncated(&'static str),
    /// Reading the assets failed (the zone doesn't fit the layout).
    Zone(ZoneError),
    /// Only PC zones are read asset by asset so far.
    NotPc,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Fastfile(e) => write!(f, "{e}"),
            Error::OtherGame(identity) => {
                write!(f, "{identity}, not a World at War PC or Xbox 360 fastfile")
            }
            Error::Truncated(what) => write!(f, "zone truncated while reading {what}"),
            Error::Zone(e) => write!(f, "{e}"),
            Error::NotPc => write!(f, "only PC zones are read asset by asset so far"),
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

/// Decompresses a PC or Xbox 360 World at War fastfile into its zone bytes
/// (the `fastfile` crate reads the container and says which game it is).
pub fn read_container(file: &[u8]) -> Result<Container, Error> {
    let head = &file[..file.len().min(fastfile::IDENTIFY_SIZE)];
    let Some(identity) = fastfile::identify(head) else {
        // Not a fastfile we know: `fastfile::read` says why (not a fastfile at
        // all, or a version no game uses).
        let why = fastfile::read(head).err();
        return Err(Error::Fastfile(
            why.unwrap_or(fastfile::Error::NotAFastfile),
        ));
    };
    let platform = match (identity.game, identity.platform) {
        (fastfile::Game::WorldAtWar, fastfile::Platform::Pc) => Platform::Pc,
        (fastfile::Game::WorldAtWar, fastfile::Platform::Xbox360) => Platform::Xbox360,
        _ => return Err(Error::OtherGame(identity)),
    };
    let zone = fastfile::read(file).map_err(Error::Fastfile)?;
    Ok(Container {
        platform,
        signed: zone.identity.signed,
        zone: zone.data,
    })
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

    /// Reads every asset of a PC zone the way the game's loader does
    /// (`zone.rs`), following `layout` (`layout.rs`). The result's
    /// `problems` are empty when the zone reads exactly; an error means the
    /// reader could not go on (or the layout lacks a structure it needs).
    pub fn read_assets(&self, layout: &layout::Layout) -> Result<LoadedZone, Error> {
        if self.platform != Platform::Pc {
            return Err(Error::NotPc);
        }
        zone::Reader::new(layout, &self.data)
            .and_then(zone::Reader::run)
            .map_err(Error::Zone)
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

impl LoadedZone {
    /// The raw files (scripts, configs) among the assets read, exactly as
    /// stored: `RawFile { name, len, buffer }`.
    pub fn raw_files(&self) -> Vec<RawFile> {
        let mut out = Vec::new();
        for a in &self.assets {
            if a.type_number != AssetType::RawFile as u32 {
                continue;
            }
            let (Some(len), Some(buffer)) = (self.u32(a.address + 4), self.u32(a.address + 8))
            else {
                continue;
            };
            let data = if buffer == 0 {
                Vec::new()
            } else {
                match self.bytes(buffer, len as usize) {
                    Some(b) => b.to_vec(),
                    None => continue,
                }
            };
            out.push(RawFile {
                name: a.name.clone(),
                data,
            });
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

    /// A layout with the one structure the raw file zone needs.
    const RAWFILE_LAYOUT: &str = "\
asset 32 rawfile RawFile
struct RawFile 0xc
  0x0004 len i32
  0x0000 name string
  0x0008 buffer ptr u8 count=len+1
";

    #[test]
    fn a_pc_zone_is_read_asset_by_asset() {
        let layout = layout::Layout::parse(RAWFILE_LAYOUT).unwrap();
        let mut zone = zone_with_rawfile(false);
        // block sizes: TEMP holds the 12-byte RawFile, VIRTUAL the asset
        // array (8), the name (11) and the buffer (6)
        zone[8..12].copy_from_slice(&12u32.to_le_bytes());
        zone[24..28].copy_from_slice(&25u32.to_le_bytes());
        let zone = Zone::from_zone(zone, Platform::Pc).unwrap();
        let loaded = zone.read_assets(&layout).unwrap();
        assert_eq!(loaded.problems, Vec::<String>::new());
        let raw = loaded.raw_files();
        assert_eq!(
            (raw[0].name.as_str(), raw[0].data.as_slice()),
            ("maps/x.gsc", &b"hello"[..])
        );
        let xbox = Zone::from_zone(zone_with_rawfile(true), Platform::Xbox360).unwrap();
        assert!(matches!(xbox.read_assets(&layout), Err(Error::NotPc)));
    }

    /// The container header, then the 8 KB authentication chunk and one
    /// hash chunk (the `fastfile` crate tests the chunk groups in full).
    const SIGNED_HEADER_SIZE: usize = 12 + 2 * 0x2000;

    #[test]
    fn xbox_360_signed() {
        let mut small = MAGIC_SIGNED.to_vec();
        small.extend_from_slice(&VERSION.to_be_bytes());
        small.extend_from_slice(MAGIC_AUTH);
        small.resize(SIGNED_HEADER_SIZE, 0xAA);
        small.extend(zlib_stored(&zone_with_rawfile(true)));
        let container = read_container(&small).unwrap();
        assert!(container.signed);
        assert_eq!(container.platform, Platform::Xbox360);
        assert_eq!(container.zone, zone_with_rawfile(true));
    }

    #[test]
    fn other_games_are_refused_as_theirs() {
        // Call of Duty 4 on PC: the same magic, version 5.
        let mut cod4 = MAGIC_UNSIGNED.to_vec();
        cod4.extend_from_slice(&5u32.to_le_bytes());
        cod4.extend(zlib_stored(&zone_with_rawfile(false)));
        match read_container(&cod4) {
            Err(Error::OtherGame(identity)) => {
                assert_eq!(identity.game, fastfile::Game::CallOfDuty4)
            }
            other => panic!("{:?}", other.err()),
        }
        assert!(matches!(
            read_container(b"not a fastfile at all"),
            Err(Error::Fastfile(fastfile::Error::NotAFastfile))
        ));
    }
}
