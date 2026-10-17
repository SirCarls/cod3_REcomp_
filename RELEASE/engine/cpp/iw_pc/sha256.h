// SHA-256 (FIPS 180-4), for checking the protocol tables read from the
// player's executable against the supported build.
#pragma once

#include <array>
#include <string>

#include "iw_pc/base.h"

namespace iw_pc::sha256 {

std::array<std::uint8_t, 32> digest(ByteView data);
std::string to_hex(const std::array<std::uint8_t, 32>& digest);

}  // namespace iw_pc::sha256
