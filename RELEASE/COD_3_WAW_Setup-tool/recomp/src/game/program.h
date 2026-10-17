// cod3 - which program this file is compiled into: the campaign (cod3,
// default.xex) or multiplayer (cod3mp, codmp_xenonf.xex).
//
// The same game sources go into both (mp/CMakeLists.txt defines COD3_MP for
// cod3mp). A guest address written in a .cpp file goes through COD3_ADDR,
// which keeps the campaign's value for cod3 and gives cod3mp its own; the
// pairs are checked against mp/config/campaign_map.tsv by
// tools/mp_aliases.py. Addresses in addresses.h need nothing: cod3mp gets
// game/mp/addresses_mp.h instead.

#pragma once

#if defined(COD3_MP)
#define COD3_ADDR(sp, mp) (mp)
#define COD3_TEXT(sp, mp) mp  // a string naming an address ("XThread8212A780")
#else
#define COD3_ADDR(sp, mp) (sp)
#define COD3_TEXT(sp, mp) sp
#endif

namespace cod3 {

#if defined(COD3_MP)
inline constexpr bool kMultiplayer = true;
#else
inline constexpr bool kMultiplayer = false;
#endif

}  // namespace cod3
