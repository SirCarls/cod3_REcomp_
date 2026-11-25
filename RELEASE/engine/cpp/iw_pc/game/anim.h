// The animation state the movement keeps for each player: the conditions the animation scripts
// test (bg_animation_mp.c's clientConditions), Call of Duty 2 (see the Rust version,
// iw_pc::game::anim).
#pragma once

#include <cstddef>
#include <cstdint>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::anim {

/// Bytes of a client's animation state; where its conditions start (two words each).
inline constexpr std::size_t kClientAnimInfoSize = 0x4B8, kConditions = 0x45C;
/// The conditions (Call of Duty 4's names and order, which Call of Duty 2's table follows).
inline constexpr std::int32_t kAnimCondPlayerAnimType = 0, kAnimCondWeaponClass = 1, kAnimCondMounted = 2,
                              kAnimCondMoveType = 3, kAnimCondUnderhand = 4, kAnimCondCrouching = 5,
                              kAnimCondFiring = 6, kAnimCondWeaponPosition = 7, kAnimCondStrafing = 8;
inline constexpr std::size_t kNumAnimConditions = 9;
/// Kept as bits, one per value (ANIM_CONDTYPE_BITFLAGS), or as the value (ANIM_CONDTYPE_VALUE).
inline constexpr std::int32_t kAnimCondTypeBitflags = 0, kAnimCondTypeValue = 1;
/// Each condition's kind (animConditionsTable at 0x005D8DD0, as the game starts with it).
inline constexpr std::int32_t kAnimConditionTypes[kNumAnimConditions] = {0, 0, 1, 0, 1, 1, 1, 1, 1};

/// A client's animation state: the game's structure as its bytes.
struct ClientAnimInfo : pmove::Bytes {
    ClientAnimInfo() : Bytes(kClientAnimInfoSize) {}
};

/// A condition set to value (see the Rust version).
/// Original: BG_UpdateConditionValue at 0x004F9360 (CoD2MP_s.exe 1.3; Wolfenstein: Enemy Territory's name; the client in eax, the condition in edx, the value in ecx, checkConversion on the stack)
void update_condition_value(ClientAnimInfo& info, std::int32_t condition, std::int32_t value, bool check_conversion);

/// The conditions the player's state gives this move (see the Rust version).
/// Original: BG_AnimUpdatePlayerStateConditions at 0x004F94B0 (CoD2MP_s.exe 1.3; Wolfenstein: Enemy Territory's name; pm on the stack)
void update_player_state_conditions(const pmove::Pmove& pm, trace::MoveHost& host);

}  // namespace iw_pc::game::anim
