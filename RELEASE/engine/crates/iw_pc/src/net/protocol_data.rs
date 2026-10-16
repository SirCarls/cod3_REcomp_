//! The tables the network protocol needs that are game data, read at run
//! time from the player's own executable.
//!
//! Speaking the PC protocol byte for byte takes a few tables that live in
//! the games' executables: the Huffman frequency table every message is
//! compressed with (256 numbers, the same as Quake III's, so also GPL
//! source) and the delta field tables (which fields of an entity, player,
//! client, HUD element or objective are sent, in which order, with how many
//! bits). They are game data, so they are not part of this code: they are
//! read from the player's `CoD2MP_s.exe` or `CoDUOMP.exe` when the program
//! starts. This module is the one place that knows where they are.
//!
//! The executable is only read as a file: a small PE reader maps a table's
//! address to its bytes in the file. Nothing is run, patched or decrypted.
//! Every table read is checked against the SHA-256 of the same table in the
//! supported build (Call of Duty 2 1.3, United Offensive 1.51), so another
//! build (whose addresses differ) is refused with an error naming the build
//! expected, instead of giving wrong tables.
//!
//! [`table`](GameExecutable::table) gives a table's raw bytes; turning the
//! field tables into fields is the delta code's job. The field tables are
//! arrays of the games' `netField_t` (12 bytes: a pointer to the field's
//! name, its offset in the structure, its bit count; little-endian).
//!
//! SHA-256 comes from the workspace's `game_sources` crate.

use std::fmt;
use std::io;
use std::path::{Path, PathBuf};

use game_sources::sha256;

use crate::Game;

/// A table of protocol data in the executables.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ProtocolTable {
    /// `msg_hData`: how often each byte value occurs (256 `int32`s), the
    /// weights the message Huffman tree is built from
    /// ([`MessageTree::from_frequencies`](super::huffman::MessageTree::from_frequencies)).
    HuffmanFrequencies,
    /// The entity state fields (`entityStateFields`).
    EntityStateFields,
    /// United Offensive's second entity table, used for entities of type 12.
    EntityStateFieldsAlternate,
    /// The player state fields (`playerStateFields`).
    PlayerStateFields,
    /// The client state fields (`clientStateFields`).
    ClientStateFields,
    /// The archived entity fields (`archivedEntityFields`).
    ArchivedEntityFields,
    /// The HUD element fields.
    HudElemFields,
    /// The objective fields.
    ObjectiveFields,
}

impl ProtocolTable {
    /// Every table, in a fixed order.
    pub const ALL: [ProtocolTable; 8] = [
        ProtocolTable::HuffmanFrequencies,
        ProtocolTable::EntityStateFields,
        ProtocolTable::EntityStateFieldsAlternate,
        ProtocolTable::PlayerStateFields,
        ProtocolTable::ClientStateFields,
        ProtocolTable::ArchivedEntityFields,
        ProtocolTable::HudElemFields,
        ProtocolTable::ObjectiveFields,
    ];

    /// Where the table is in `game`'s executable, or `None` when that game
    /// has no such table.
    pub fn location(self, game: Game) -> Option<TableLocation> {
        let tables = match game {
            Game::CallOfDuty2 => COD2_TABLES,
            Game::UnitedOffensive => UO_TABLES,
        };
        tables.iter().copied().find(|t| t.table == self)
    }

    /// A name for messages.
    pub fn description(self) -> &'static str {
        match self {
            ProtocolTable::HuffmanFrequencies => "Huffman frequency table",
            ProtocolTable::EntityStateFields => "entity state field table",
            ProtocolTable::EntityStateFieldsAlternate => "second entity state field table",
            ProtocolTable::PlayerStateFields => "player state field table",
            ProtocolTable::ClientStateFields => "client state field table",
            ProtocolTable::ArchivedEntityFields => "archived entity field table",
            ProtocolTable::HudElemFields => "HUD element field table",
            ProtocolTable::ObjectiveFields => "objective field table",
        }
    }
}

/// Where a table is in one game's executable, and the SHA-256 of its bytes
/// in the supported build.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TableLocation {
    pub table: ProtocolTable,
    /// Its virtual address (the executables load at 0x00400000).
    pub address: u32,
    /// The number of entries.
    pub entries: usize,
    /// Bytes per entry: 4 for the frequencies, 12 for a `netField_t`.
    pub entry_size: usize,
    /// SHA-256 of the table's bytes, in hexadecimal.
    pub sha256: &'static str,
}

impl TableLocation {
    /// The table's size in bytes.
    pub fn len(&self) -> usize {
        self.entries * self.entry_size
    }

    /// Always false: every table has entries.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// Bytes per `netField_t`: name pointer, offset, bits.
const NET_FIELD_SIZE: usize = 12;

const fn location(
    table: ProtocolTable,
    address: u32,
    entries: usize,
    entry_size: usize,
    sha256: &'static str,
) -> TableLocation {
    TableLocation {
        table,
        address,
        entries,
        entry_size,
        sha256,
    }
}

/// `CoD2MP_s.exe` 1.3. The counts are the ones the code passes with each
/// table (`MSG_WriteDeltaEntity` 59 entity fields, `MSG_WriteDeltaPlayerstate`
/// 105, ...).
const COD2_TABLES: &[TableLocation] = &[
    location(
        ProtocolTable::HuffmanFrequencies,
        0x0059_CB58,
        256,
        4,
        "1cbdca8d6c9bb22247525a4807c5e18cec9b87336c56016424eecd879b505abf",
    ),
    location(
        ProtocolTable::EntityStateFields,
        0x0059_BDA0,
        59,
        NET_FIELD_SIZE,
        "cca9e98aab3df40614c429ae7ceecb01277351eedec0d4171b04ea2648c3f9dc",
    ),
    location(
        ProtocolTable::PlayerStateFields,
        0x0059_C4A0,
        105,
        NET_FIELD_SIZE,
        "f5db42df82893234c22dc84b9d14533883e56e2514da5a3f4d3e31e82cd9e31a",
    ),
    location(
        ProtocolTable::ClientStateFields,
        0x0059_C398,
        22,
        NET_FIELD_SIZE,
        "656a11e60e7c1e554febfdf80013709000df629e13c96de188f9ab663e65213c",
    ),
    location(
        ProtocolTable::ArchivedEntityFields,
        0x0059_C068,
        68,
        NET_FIELD_SIZE,
        "7670ca4f1a66a75b3594916fb7dcbecdb3c0a7b92d6860de40da9d7349c6a3a5",
    ),
    location(
        ProtocolTable::HudElemFields,
        0x0059_C9D8,
        32,
        NET_FIELD_SIZE,
        "1a80d6c27a5a556e4ee03916c77420ab2ea2c3c3ca88b66eef46b91298c5adfe",
    ),
    location(
        ProtocolTable::ObjectiveFields,
        0x0059_C990,
        6,
        NET_FIELD_SIZE,
        "7ecf958791b87136a4e87c8b513c6e4294ef2e4866cb4b103d17203e3995adf1",
    ),
];

/// `CoDUOMP.exe` 1.51.
const UO_TABLES: &[TableLocation] = &[
    location(
        ProtocolTable::HuffmanFrequencies,
        0x005C_58D0,
        256,
        4,
        "1cbdca8d6c9bb22247525a4807c5e18cec9b87336c56016424eecd879b505abf",
    ),
    location(
        ProtocolTable::EntityStateFields,
        0x0058_EC68,
        60,
        NET_FIELD_SIZE,
        "9148f968f20a7efeab5ed17dd820d796806a546a476ac242e65a877784d9e400",
    ),
    location(
        ProtocolTable::EntityStateFieldsAlternate,
        0x0058_EF38,
        60,
        NET_FIELD_SIZE,
        "6796d0116c9cb5dd5150bc71446c7f2ab3f8611dfceb4e226a43e4ae00fb00e8",
    ),
    location(
        ProtocolTable::PlayerStateFields,
        0x0058_F640,
        114,
        NET_FIELD_SIZE,
        "cfb07b9ad449c2d7667ee9ae1959dea72bcadb1ef64ada90bac99b660a02f3ed",
    ),
    location(
        ProtocolTable::ClientStateFields,
        0x0058_F538,
        22,
        NET_FIELD_SIZE,
        "d1326ca8927d0abd54892e0d8afae33ffeabe37218a5f21221a670adfc8423cb",
    ),
    location(
        ProtocolTable::ArchivedEntityFields,
        0x0058_F208,
        68,
        NET_FIELD_SIZE,
        "83d2f8669a3ea946040905a60a3322251a2cab07a5e654e5c5637e19d7c41cd5",
    ),
    location(
        ProtocolTable::HudElemFields,
        0x005C_5768,
        30,
        NET_FIELD_SIZE,
        "740ca7005d7d0259ed09577fcbed1fdffaaf82127623b57d497404a7c1216963",
    ),
    location(
        ProtocolTable::ObjectiveFields,
        0x005C_5720,
        6,
        NET_FIELD_SIZE,
        "24746885ee50d80abd245432b687d55ee4fa0c995eb8d37f962859b7727c8de6",
    ),
];

/// The executable file a game's tables are read from.
pub fn executable_name(game: Game) -> &'static str {
    match game {
        Game::CallOfDuty2 => "CoD2MP_s.exe",
        Game::UnitedOffensive => "CoDUOMP.exe",
    }
}

/// The build the tables are known for, for messages.
pub fn supported_build(game: Game) -> &'static str {
    match game {
        Game::CallOfDuty2 => "Call of Duty 2 1.3 (CoD2MP_s.exe)",
        Game::UnitedOffensive => "Call of Duty: United Offensive 1.51 (CoDUOMP.exe)",
    }
}

/// Why protocol data couldn't be read.
#[derive(Debug)]
pub enum ProtocolDataError {
    /// The file couldn't be read.
    Io { path: PathBuf, error: io::Error },
    /// The file isn't a 32-bit Windows executable (what is wrong with it).
    NotAnExecutable(&'static str),
    /// No section of the file holds these bytes.
    AddressNotInFile { address: u32, len: usize },
    /// The table's bytes differ from the supported build's: another build
    /// (or a modified file).
    WrongBuild { game: Game, table: ProtocolTable },
    /// The game has no such table (the second entity table is United
    /// Offensive's only).
    NoSuchTable { game: Game, table: ProtocolTable },
}

impl fmt::Display for ProtocolDataError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ProtocolDataError::Io { path, error } => {
                write!(f, "can't read {}: {error}", path.display())
            }
            ProtocolDataError::NotAnExecutable(what) => {
                write!(f, "not a 32-bit Windows executable: {what}")
            }
            ProtocolDataError::AddressNotInFile { address, len } => {
                write!(f, "the executable holds no {len} bytes at 0x{address:08X}")
            }
            ProtocolDataError::WrongBuild { game, table } => write!(
                f,
                "this {} is not the expected build: its {} differs from the one in {}. \
                 Use {} from an unmodified {} install.",
                executable_name(*game),
                table.description(),
                supported_build(*game),
                executable_name(*game),
                supported_build(*game),
            ),
            ProtocolDataError::NoSuchTable { game, table } => write!(
                f,
                "{} has no {}",
                supported_build(*game),
                table.description()
            ),
        }
    }
}

impl std::error::Error for ProtocolDataError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            ProtocolDataError::Io { error, .. } => Some(error),
            _ => None,
        }
    }
}

/// One section of the executable: where its bytes are in memory and in the file.
#[derive(Debug, Clone, Copy)]
struct Section {
    /// Relative to the image base.
    virtual_address: u32,
    /// Bytes stored in the file (the rest of the section, if any, is zeros
    /// made at load time and holds no table).
    raw_size: u32,
    raw_offset: u32,
}

/// A game executable opened for reading its protocol tables.
#[derive(Debug)]
pub struct GameExecutable {
    game: Game,
    bytes: Vec<u8>,
    image_base: u32,
    sections: Vec<Section>,
}

fn u16_at(bytes: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(bytes.get(at..at + 2)?.try_into().ok()?))
}

fn u32_at(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?))
}

impl GameExecutable {
    /// Reads `game`'s executable from `path` (the whole file, once).
    pub fn open(game: Game, path: &Path) -> Result<GameExecutable, ProtocolDataError> {
        let bytes = std::fs::read(path).map_err(|error| ProtocolDataError::Io {
            path: path.to_path_buf(),
            error,
        })?;
        GameExecutable::from_bytes(game, bytes)
    }

    /// The executable from its bytes: reads the PE headers (the image base
    /// and the section table).
    pub fn from_bytes(game: Game, bytes: Vec<u8>) -> Result<GameExecutable, ProtocolDataError> {
        use ProtocolDataError::NotAnExecutable;
        if bytes.get(..2) != Some(b"MZ") {
            return Err(NotAnExecutable("no MZ header"));
        }
        let pe = u32_at(&bytes, 0x3C).ok_or(NotAnExecutable("cut short"))? as usize;
        if bytes.get(pe..pe + 4) != Some(b"PE\0\0") {
            return Err(NotAnExecutable("no PE header"));
        }
        let coff = pe + 4;
        let sections = u16_at(&bytes, coff + 2).ok_or(NotAnExecutable("cut short"))? as usize;
        let optional_size = u16_at(&bytes, coff + 16).ok_or(NotAnExecutable("cut short"))?;
        let optional = coff + 20;
        if u16_at(&bytes, optional) != Some(0x10B) {
            return Err(NotAnExecutable("not a 32-bit (PE32) image"));
        }
        let image_base = u32_at(&bytes, optional + 28).ok_or(NotAnExecutable("cut short"))?;
        let table = optional + usize::from(optional_size);
        let sections = (0..sections)
            .map(|i| {
                let at = table + i * 40;
                Some(Section {
                    virtual_address: u32_at(&bytes, at + 12)?,
                    raw_size: u32_at(&bytes, at + 16)?,
                    raw_offset: u32_at(&bytes, at + 20)?,
                })
            })
            .collect::<Option<Vec<_>>>()
            .ok_or(NotAnExecutable("section table cut short"))?;
        Ok(GameExecutable {
            game,
            bytes,
            image_base,
            sections,
        })
    }

    /// The game this executable was opened for.
    pub fn game(&self) -> Game {
        self.game
    }

    /// `len` bytes at the virtual address `address`, as stored in the file.
    /// Plain data only: no check of what the bytes are.
    pub fn read(&self, address: u32, len: usize) -> Result<&[u8], ProtocolDataError> {
        let missing = ProtocolDataError::AddressNotInFile { address, len };
        let rva = u64::from(address.wrapping_sub(self.image_base));
        let end = rva + len as u64;
        let section = self
            .sections
            .iter()
            .find(|s| {
                let start = u64::from(s.virtual_address);
                start <= rva && end <= start + u64::from(s.raw_size)
            })
            .ok_or(missing)?;
        let offset = u64::from(section.raw_offset) + rva - u64::from(section.virtual_address);
        let offset = usize::try_from(offset)
            .map_err(|_| ProtocolDataError::AddressNotInFile { address, len })?;
        self.bytes
            .get(offset..offset + len)
            .ok_or(ProtocolDataError::AddressNotInFile { address, len })
    }

    /// A table's bytes, checked against the supported build's SHA-256.
    pub fn table(&self, table: ProtocolTable) -> Result<&[u8], ProtocolDataError> {
        let game = self.game;
        let location = table
            .location(game)
            .ok_or(ProtocolDataError::NoSuchTable { game, table })?;
        let bytes = self
            .read(location.address, location.len())
            .map_err(|_| ProtocolDataError::WrongBuild { game, table })?;
        if sha256::to_hex(&sha256::digest(bytes)) != location.sha256 {
            return Err(ProtocolDataError::WrongBuild { game, table });
        }
        Ok(bytes)
    }

    /// The Huffman frequency table (`msg_hData`), checked.
    pub fn huffman_frequencies(&self) -> Result<[i32; 256], ProtocolDataError> {
        let bytes = self.table(ProtocolTable::HuffmanFrequencies)?;
        let mut frequencies = [0i32; 256];
        for (value, chunk) in frequencies.iter_mut().zip(bytes.chunks_exact(4)) {
            *value = i32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
        }
        Ok(frequencies)
    }
}

/// Finds a game's executable in the folders named by an environment
/// variable like `IW_PC_INPUT` (several separated like `PATH`), by name in
/// any case, up to two folders down. For tools and tests.
pub fn find_executable(game: Game, folders: &std::ffi::OsStr) -> Option<PathBuf> {
    fn search(dir: &Path, name: &str, depth: u32) -> Option<PathBuf> {
        let mut subfolders = Vec::new();
        for entry in std::fs::read_dir(dir).ok()?.flatten() {
            let path = entry.path();
            if path.is_dir() {
                subfolders.push(path);
            } else if entry
                .file_name()
                .to_string_lossy()
                .eq_ignore_ascii_case(name)
            {
                return Some(path);
            }
        }
        if depth == 0 {
            return None;
        }
        subfolders.sort();
        subfolders
            .iter()
            .find_map(|sub| search(sub, name, depth - 1))
    }
    let name = executable_name(game);
    std::env::split_paths(folders).find_map(|dir| search(&dir, name, 2))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal PE32 image: one section of `data` at RVA 0x1000, stored at
    /// file offset 0x200, image base 0x00400000.
    fn tiny_executable(data: &[u8]) -> Vec<u8> {
        let mut file = vec![0u8; 0x200];
        file[..2].copy_from_slice(b"MZ");
        file[0x3C..0x40].copy_from_slice(&0x40u32.to_le_bytes());
        file[0x40..0x44].copy_from_slice(b"PE\0\0");
        let coff = 0x44;
        file[coff..coff + 2].copy_from_slice(&0x14Cu16.to_le_bytes());
        file[coff + 2..coff + 4].copy_from_slice(&1u16.to_le_bytes());
        file[coff + 16..coff + 18].copy_from_slice(&0xE0u16.to_le_bytes());
        let optional = coff + 20;
        file[optional..optional + 2].copy_from_slice(&0x10Bu16.to_le_bytes());
        file[optional + 28..optional + 32].copy_from_slice(&0x0040_0000u32.to_le_bytes());
        let section = optional + 0xE0;
        file[section..section + 5].copy_from_slice(b".data");
        file[section + 8..section + 12].copy_from_slice(&0x2000u32.to_le_bytes());
        file[section + 12..section + 16].copy_from_slice(&0x1000u32.to_le_bytes());
        file[section + 16..section + 20].copy_from_slice(&(data.len() as u32).to_le_bytes());
        file[section + 20..section + 24].copy_from_slice(&0x200u32.to_le_bytes());
        file.extend_from_slice(data);
        file
    }

    #[test]
    fn reads_by_virtual_address() {
        let data: Vec<u8> = (0..=255).collect();
        let exe = GameExecutable::from_bytes(Game::CallOfDuty2, tiny_executable(&data)).unwrap();
        assert_eq!(exe.read(0x0040_1010, 4).unwrap(), &[16, 17, 18, 19]);
        assert_eq!(exe.read(0x0040_10FC, 4).unwrap(), &[252, 253, 254, 255]);
        // Past the stored bytes (the zero-filled rest of the section), before the image.
        assert!(exe.read(0x0040_10FD, 4).is_err());
        assert!(exe.read(0x0040_0000, 4).is_err());
        assert!(GameExecutable::from_bytes(Game::CallOfDuty2, b"MZ".to_vec()).is_err());
    }

    #[test]
    fn another_build_is_refused_by_name() {
        let exe = GameExecutable::from_bytes(Game::CallOfDuty2, tiny_executable(&[0; 64])).unwrap();
        let error = exe.huffman_frequencies().unwrap_err();
        assert!(matches!(error, ProtocolDataError::WrongBuild { .. }));
        assert!(error.to_string().contains("Call of Duty 2 1.3"));
        let error = exe
            .table(ProtocolTable::EntityStateFieldsAlternate)
            .unwrap_err();
        assert!(matches!(error, ProtocolDataError::NoSuchTable { .. }));
    }

    #[test]
    fn locations_do_not_overlap() {
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let mut spans: Vec<(u32, u32)> = ProtocolTable::ALL
                .iter()
                .filter_map(|t| t.location(game))
                .map(|l| (l.address, l.address + l.len() as u32))
                .collect();
            spans.sort();
            assert!(spans.windows(2).all(|w| w[0].1 <= w[1].0));
            assert!(spans
                .iter()
                .all(|&(start, end)| start >= 0x0040_0000 && end > start));
        }
    }

    /// The real executables, when `IW_PC_INPUT` names the folder(s) holding
    /// them: every table is found and matches its hash.
    #[test]
    fn tables_of_the_supported_builds() {
        let Some(folders) = std::env::var_os("IW_PC_INPUT") else {
            eprintln!("IW_PC_INPUT is not set: skipping the check of the game executables' tables");
            return;
        };
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let Some(path) = find_executable(game, &folders) else {
                eprintln!(
                    "{} not found in IW_PC_INPUT: skipped",
                    executable_name(game)
                );
                continue;
            };
            let exe = GameExecutable::open(game, &path).unwrap();
            for table in ProtocolTable::ALL {
                match table.location(game) {
                    Some(location) => {
                        assert_eq!(exe.table(table).unwrap().len(), location.len())
                    }
                    None => assert!(exe.table(table).is_err()),
                }
            }
            let frequencies = exe.huffman_frequencies().unwrap();
            assert!(frequencies.iter().all(|&f| f > 0));
        }
    }
}
