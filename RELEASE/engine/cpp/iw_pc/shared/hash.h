// The games' string hashes: each character (as a signed char) times its
// position plus 119, Quake III's.
#pragma once

#include "iw_pc/base.h"

namespace iw_pc::shared::hash {

/// The dvar table's size (generateHashValue masks to it).
inline constexpr std::uint32_t kCvarHashSize = 256;

/// The file system's bucket for a file name: lower-cased characters (a dot
/// ends the name, \ counts as /), then h ^ h >> 10 ^ h >> 20 masked to
/// hash_size (a power of two).
/// Original: FS_HashFileName at 0x00421AA0 (CoD2MP_s.exe 1.3)
/// Original: FS_HashFileName at 0x0042C990 (CoDUOMP.exe 1.51)
std::uint32_t file_name_hash(ByteView name, std::uint32_t hash_size);

/// The dvar table's bucket: lower-cased characters, the sum cut to 8 bits.
/// (A NULL name is the game's error, "null name in generateHashValue".)
/// Original: generateHashValue at 0x00435A70 (CoD2MP_s.exe 1.3)
/// Original: generateHashValue at 0x0043D860 (CoDUOMP.exe 1.51)
std::uint32_t cvar_hash(ByteView name);

/// The hash of up to max_len characters, case kept, folded
/// (h ^ h >> 10 ^ h >> 20) but not masked. max_len 0 or less: 0.
/// Original: Com_HashKey at 0x0042D8F0 (CoD2MP_s.exe 1.3)
/// Original: Com_HashKey at 0x00435480 (CoDUOMP.exe 1.51)
std::int32_t hash_key(ByteView s, std::int32_t max_len);

}  // namespace iw_pc::shared::hash
