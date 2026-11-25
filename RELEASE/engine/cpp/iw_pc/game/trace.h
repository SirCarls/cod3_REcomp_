// What the movement asks of the world: traces, answered by the game's own trace functions
// (see the Rust version, iw_pc::game::trace). A move's handler byte picks one (0 the server's,
// 1 the client game's); they are the host's (MoveHost).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace iw_pc::game::pmove {
struct PlayerState;
}
namespace iw_pc::game::anim {
struct ClientAnimInfo;
}
namespace iw_pc::game::weapon {
struct WeaponDef;
}

namespace iw_pc::game::trace {

/// Bytes of Call of Duty 2's trace_t (no end point: callers compute it from the fraction).
inline constexpr std::size_t kTraceSize = 0x24;
namespace at {
inline constexpr std::size_t kFraction = 0x00, kNormal = 0x04, kSurfaceFlags = 0x10, kContents = 0x14,
                             kEntityNum = 0x1C, kAllSolid = 0x22, kStartSolid = 0x23;
}
/// The contents bit of bodies' boxes (CONTENTS_BODY).
inline constexpr std::int32_t kContentsBody = 0x02000000;

struct Trace {
    std::array<std::uint8_t, kTraceSize> bytes{};
    std::int32_t integer(std::size_t at) const {
        std::int32_t v;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }
    float real(std::size_t at) const {
        float v;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }
    float fraction() const { return real(at::kFraction); }
    std::array<float, 3> normal() const { return {real(at::kNormal), real(at::kNormal + 4), real(at::kNormal + 8)}; }
    std::int32_t contents() const { return integer(at::kContents); }
    std::uint16_t entity_num() const {
        return static_cast<std::uint16_t>(bytes[at::kEntityNum] | bytes[at::kEntityNum + 1] << 8);
    }
    bool all_solid() const { return bytes[at::kAllSolid] != 0; }
    bool start_solid() const { return bytes[at::kStartSolid] != 0; }
};

/// What the movement needs from the rest of the game.
class MoveHost {
public:
    virtual ~MoveHost() = default;
    /// The box (mins, maxs) swept from start to end through what content_mask collides with,
    /// ignoring entity pass_entity, by the game's trace function `handler`.
    virtual Trace trace(std::uint8_t handler, const std::array<float, 3>& start, const std::array<float, 3>& mins,
                        const std::array<float, 3>& maxs, const std::array<float, 3>& end, std::int32_t pass_entity,
                        std::int32_t content_mask) = 0;
    /// The move-speed scale of weapon `weapon` (its WeaponDef's moveSpeedScale, the float at 0x264).
    virtual float weapon_move_speed_scale(std::int32_t weapon) const = 0;
    /// Plays the player's animation script for `event` as Call of Duty 2's BG_AnimScriptEvent
    /// (0x004F92C0) does: the animation's duration, or -1 for none.
    virtual std::int32_t anim_script_event(pmove::PlayerState& ps, std::int32_t event, bool is_continue, bool force) = 0;
    /// A message for the console (Com_Printf): a warning the movement prints and goes on.
    virtual void print(const char* message) = 0;
    /// How long animation `anim` lasts (milliseconds; animation_t's int at 0x48).
    virtual std::int32_t anim_duration(std::int32_t anim) const = 0;
    /// Weapon `weapon`'s definition (bg_weaponDefs[weapon], the table at 0x01728200; unchecked).
    virtual const weapon::WeaponDef& weapon_def(std::int32_t weapon) const = 0;
    /// The highest weapon number defined (the word at 0x019A1C74).
    virtual std::int32_t weapon_count() const = 0;
    /// Client `client`'s animation state (0x4B8 bytes each from 0xB4BFC in the state at 0x019A1C78).
    virtual anim::ClientAnimInfo& client_anim_info(std::int32_t client) = 0;
};

/// The animation scripts' events a move plays (scriptAnimEventTypes_t, Return to Castle
/// Wolfenstein's names and order).
inline constexpr std::int32_t kAnimEtDeath = 1, kAnimEtJump = 3, kAnimEtJumpBk = 4, kAnimEtLand = 5;
/// The stance changes' (our names, from where duck::check_duck plays them).
inline constexpr std::int32_t kAnimEtProneToCrouch = 12, kAnimEtStandToCrouch = 13, kAnimEtCrouchToStand = 14,
                              kAnimEtProneToStand = 16;

}  // namespace iw_pc::game::trace
