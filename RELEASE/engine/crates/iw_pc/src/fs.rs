//! The file system's search path, as far as the network needs it: the
//! archives a game has loaded, which of them it has read files from, and
//! the list of pure checksums a client sends a pure server (`cp`, see
//! [`crate::client::send_pure_checksums`] and docs/PC_MULTIPLAYER.md,
//! section 8).
//!
//! An archive is an `.iwd` in Call of Duty 2 and a `.pk3` in United
//! Offensive; both are zip files, and their checksums are
//! [`crate::shared::checksum::archive_checksums`] of the files' CRC-32s.

use crate::shared::strings::strcat;
use crate::Game;

/// Which kinds of file the game has read from an archive (the flags the
/// file system sets as it opens files; a server lists only archives a
/// client has used).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Referenced {
    /// Any file but a few the game reads from every archive (United
    /// Offensive: not `.bot`, `.arena`, `.menu` or the sound aliases).
    /// The archive's byte at +0x310, both games.
    pub any_file: bool,
    /// United Offensive only: the user interface module (`uo_ui_mp_x86.dll`;
    /// +0x311).
    pub ui_module: bool,
    /// United Offensive only: the client game module
    /// (`uo_cgame_mp_x86.dll`; +0x312).
    pub cgame_module: bool,
}

/// An archive on the search path.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Archive {
    /// Its checksum keyed with the checksum feed (the archive's +0x308).
    pub pure_checksum: i32,
    pub referenced: Referenced,
}

/// One place on the search path: an archive, or a folder (`None`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SearchPath {
    pub archive: Option<Archive>,
    /// A localized archive (a language's own, `localized_*`; the search
    /// path's +0xC): never listed.
    pub localized: bool,
}

/// An archive of the player's copy as a client keeps it between servers:
/// its files' CRC-32s (each file with any bytes, in the central directory's
/// order), from which each server's checksum feed makes a pure checksum
/// (the games' file system loads the archives again when the feed changes).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LoadedArchive {
    pub crcs: Vec<u32>,
    pub referenced: Referenced,
    pub localized: bool,
}

/// The search path of `archives` for a server's checksum feed (the archives
/// only; folders are never listed).
pub fn search_path_for_feed(archives: &[LoadedArchive], feed: i32) -> Vec<SearchPath> {
    archives
        .iter()
        .map(|a| SearchPath {
            archive: Some(Archive {
                pure_checksum: crate::shared::checksum::archive_checksums(&a.crcs, feed).1,
                referenced: a.referenced,
            }),
            localized: a.localized,
        })
        .collect()
}

/// The size of the list's buffer, both games (a static array of 8192
/// bytes): the list is at most 8191 characters.
pub const PURE_CHECKSUMS_SIZE: usize = 0x2000;

/// The pure checksums of the referenced archives, as text: `@`, then the
/// pure checksum of each archive with `any_file` set (search path order,
/// localized archives left out), then the fake checksum (`fs_fakeChkSum`)
/// if it isn't 0, then the checksum feed XORed with every listed checksum
/// and with their number. Each number is followed by a space.
///
/// United Offensive first lists the client game module's archive and then
/// the user interface's (the first archive on the path with each flag;
/// these two are neither counted nor XORed), each pass followed by the fake
/// checksum if it is set, so: `<cgame> <fake> <ui> <fake> @ <archives>
/// <fake> <total>`. Call of Duty 2 has only the last pass (its client game
/// is part of the executable).
///
/// The text is cut at 8191 characters, as the original's `Q_strcat` cuts
/// it (Call of Duty 2 calls it `I_strncat`).
///
/// Original: `FS_ReferencedIwdPureChecksums` at 0x0043C330 (CoD2MP_s.exe 1.3)
/// Original: `FS_ReferencedPakPureChecksums` at 0x00440F50 (CoDUOMP.exe 1.51)
pub fn referenced_pure_checksums(
    game: Game,
    search_paths: &[SearchPath],
    feed: i32,
    fake: i32,
) -> Vec<u8> {
    fn append(info: &mut [u8], number: i32) {
        // `Q_strcat(info, sizeof(info), va("%i ", number))`. Never fails:
        // the buffer's last byte is always 0, so the text is always shorter
        // than the buffer.
        let appended = strcat(info, format!("{number} ").as_bytes());
        debug_assert!(appended.is_ok());
    }
    let mut info = vec![0u8; PURE_CHECKSUMS_SIZE];
    let mut checksum = feed;
    let mut count: i32 = 0;
    // United Offensive's passes: the client game, the user interface, then
    // every referenced archive. Call of Duty 2's: only the last.
    let first_pass = match game {
        Game::CallOfDuty2 => 0,
        Game::UnitedOffensive => 2,
    };
    for pass in (0..=first_pass).rev() {
        if pass == 0 {
            // `strcat(info, "@ ")`, which leaves the same bytes.
            let at = crate::cstr::len(&info);
            info[at] = b'@';
            info[at + 1] = b' ';
            info[at + 2] = 0;
        }
        for path in search_paths {
            let Some(archive) = path.archive.filter(|_| !path.localized) else {
                continue;
            };
            let flag = match pass {
                2 => archive.referenced.cgame_module,
                1 => archive.referenced.ui_module,
                _ => archive.referenced.any_file,
            };
            if !flag {
                continue;
            }
            append(&mut info, archive.pure_checksum);
            if pass != 0 {
                break;
            }
            checksum ^= archive.pure_checksum;
            count = count.wrapping_add(1);
        }
        if fake != 0 {
            append(&mut info, fake);
        }
    }
    append(&mut info, count ^ checksum);
    info.truncate(crate::cstr::len(&info));
    info
}

#[cfg(test)]
mod tests {
    use super::*;

    fn archive(
        pure_checksum: i32,
        any_file: bool,
        ui_module: bool,
        cgame_module: bool,
    ) -> SearchPath {
        SearchPath {
            archive: Some(Archive {
                pure_checksum,
                referenced: Referenced {
                    any_file,
                    ui_module,
                    cgame_module,
                },
            }),
            localized: false,
        }
    }

    #[test]
    fn lists_referenced_archives() {
        let paths = [
            archive(11, true, false, false),
            SearchPath::default(),
            archive(22, false, false, false),
            SearchPath {
                localized: true,
                ..archive(33, true, false, false)
            },
            archive(-44, true, false, false),
        ];
        let total = 2 ^ 7 ^ 11 ^ -44;
        assert_eq!(
            referenced_pure_checksums(Game::CallOfDuty2, &paths, 7, 0),
            format!("@ 11 -44 {total} ").into_bytes()
        );
        assert_eq!(
            referenced_pure_checksums(Game::CallOfDuty2, &paths, 7, 5),
            format!("@ 11 -44 5 {total} ").into_bytes()
        );
        assert_eq!(
            referenced_pure_checksums(Game::CallOfDuty2, &[], 7, 0),
            b"@ 7 ".to_vec()
        );
    }

    #[test]
    fn united_offensive_lists_its_modules_first() {
        let paths = [
            archive(1, true, true, false),
            archive(2, true, false, true),
            archive(3, false, true, true),
        ];
        let total = 2 ^ 9 ^ 1 ^ 2;
        assert_eq!(
            referenced_pure_checksums(Game::UnitedOffensive, &paths, 9, 0),
            format!("2 1 @ 1 2 {total} ").into_bytes()
        );
        assert_eq!(
            referenced_pure_checksums(Game::UnitedOffensive, &paths, 9, 4),
            format!("2 4 1 4 @ 1 2 4 {total} ").into_bytes()
        );
    }

    #[test]
    fn a_long_list_is_cut() {
        let paths = vec![archive(-2_000_000_000, true, false, false); 1000];
        let text = referenced_pure_checksums(Game::CallOfDuty2, &paths, 0, 0);
        assert_eq!(text.len(), PURE_CHECKSUMS_SIZE - 1);
    }
}
