// The block checksums both games use for their archives: MD4 (RFC 1320, see
// docs/SOURCES.md) of a block, its four words folded into one by XOR. A pure
// server and its clients compare archives by these (docs/PC_MULTIPLAYER.md,
// section 8): an archive's checksum is that of the list of its files'
// CRC-32s, its pure checksum the same with the game's checksum feed first.
#pragma once

#include <array>

#include "iw_pc/base.h"

namespace iw_pc::shared::checksum {

/// MD4 of data: the four state words after the last block (the digest's
/// bytes, read as little-endian words).
std::array<std::uint32_t, 4> md4(ByteView data);

/// MD4 of data, its four words XORed together.
/// Original: Com_BlockChecksum at 0x00443A70 (CoD2MP_s.exe 1.3)
/// Original: Com_BlockChecksum at 0x00449470 (CoDUOMP.exe 1.51)
std::int32_t block_checksum(ByteView data);

/// block_checksum of key (4 bytes, little-endian) followed by data.
/// Original: Com_BlockChecksumKey at 0x00443AE0 (CoD2MP_s.exe 1.3)
/// Original: Com_BlockChecksumKey at 0x004494F0 (CoDUOMP.exe 1.51)
std::int32_t block_checksum_key(ByteView data, std::int32_t key);

/// An archive's checksum and pure checksum, as the games' FS_LoadZipFile
/// makes them: from the CRC-32 of each file in the archive with any bytes (in
/// the central directory's order), as little-endian words; the pure checksum
/// keyed with the game's checksum feed (fs_checksumFeed, the server's number
/// for the map).
/// (The end of FS_LoadZipFile: 0x00423922 in CoD2MP_s.exe 1.3, 0x0042ED8E in
/// CoDUOMP.exe 1.51; checked through the two functions it calls.)
struct ArchiveChecksums {
    std::int32_t checksum;
    std::int32_t pure_checksum;
};
ArchiveChecksums archive_checksums(std::span<const std::uint32_t> crcs, std::int32_t feed);

}  // namespace iw_pc::shared::checksum
