// The file system's search path, as far as the network needs it: the
// archives a game has loaded, which of them it has read files from, and the
// list of pure checksums a client sends a pure server (cp; see
// client::send_pure_checksums and docs/PC_MULTIPLAYER.md, section 8). The
// Rust version (iw_pc::fs) has the longer comments.
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "iw_pc/base.h"

namespace iw_pc::fs {

/// Which kinds of file the game has read from an archive.
struct Referenced {
    bool any_file = false;      // +0x310, both games
    bool ui_module = false;     // United Offensive: uo_ui_mp_x86.dll (+0x311)
    bool cgame_module = false;  // United Offensive: uo_cgame_mp_x86.dll (+0x312)
};

/// An archive on the search path (.iwd, .pk3).
struct Archive {
    std::int32_t pure_checksum = 0;  // +0x308
    Referenced referenced;
};

/// One place on the search path: an archive, or a folder (no archive).
struct SearchPath {
    std::optional<Archive> archive;
    bool localized = false;  // +0xC: never listed
};

/// The size of the list's buffer, both games.
inline constexpr std::size_t kPureChecksumsSize = 0x2000;

/// The pure checksums of the referenced archives, as text ("@ ", each
/// referenced archive's, the fake one if set, then the feed XORed with them
/// and their number; United Offensive first its client game's and user
/// interface's archives), cut at 8191 characters.
/// Original: FS_ReferencedIwdPureChecksums at 0x0043C330 (CoD2MP_s.exe 1.3)
/// Original: FS_ReferencedPakPureChecksums at 0x00440F50 (CoDUOMP.exe 1.51)
std::vector<std::uint8_t> referenced_pure_checksums(Game game, std::span<const SearchPath> search_paths,
                                                    std::int32_t feed, std::int32_t fake);

}  // namespace iw_pc::fs
