// Whether a player fits lying down where he is (BG_CheckProne), Call of Duty 2 (see the Rust
// version, iw_pc::game::prone).
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::prone {

struct ProneQuery {
    std::int32_t pass_entity = 0;
    std::array<float, 3> origin{};
    float size = 0.0f;
    float height = 0.0f;
    float yaw = 0.0f;
    bool already_prone = false;
    bool on_ground = false;
    std::optional<std::array<float, 3>> ground_normal;
    std::uint8_t handler = 0;
    std::int32_t check_type = 0;
    float feet_distance = 0.0f;
};

/// The outputs, where asked for (null: not asked); a failed check leaves them as they are.
struct ProneAngles {
    float* torso_height = nullptr;
    float* torso_pitch = nullptr;
    float* waist_pitch = nullptr;
};

/// Original: BG_CheckProne at 0x00514700 (CoD2MP_s.exe 1.3; Call of Duty 4's name; 14 arguments on the stack: passEntityNum, vPos, fSize, fHeight, fYaw, pfTorsoHeight, pfTorsoPitch, pfWaistPitch, bAlreadyProne, bOnGround, vGroundNormal, handler, proneCheckType, prone_feet_dist)
bool check_prone(const ProneQuery& q, ProneAngles out, trace::MoveHost& host);

/// A prone player's move checked: he must still fit lying down; if not, back to where the move
/// started (see the Rust version). Not prone: true.
/// Original: PM_VerifyPronePosition at 0x00530130 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in eax, the start velocity in ebx, the start origin on the stack)
bool verify_prone_position(pmove::Pmove& pm, const std::array<float, 3>& start_origin,
                           const std::array<float, 3>& start_velocity, trace::MoveHost& host);

}  // namespace iw_pc::game::prone
