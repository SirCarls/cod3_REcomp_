// Sliding along what the player runs into (bg_slidemove.c), Call of Duty 2 (see the Rust
// version, iw_pc::game::slide).
#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::slide {

/// The most surfaces one slide move clips against (MAX_CLIP_PLANES).
inline constexpr std::size_t kMaxClipPlanes = 8;

/// The order of planes from the one velocity heads into most to the least; the smallest dot
/// product (see the Rust version). planes is not empty.
/// Original: PM_PermuteRestrictiveClipPlanes at 0x005301F0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; planes in edx, then (velocity, planeCount, permutation) on the stack; the smallest on the x87 stack)
float permute_restrictive_clip_planes(const std::array<float, 3>& velocity, std::span<const std::array<float, 3>> planes,
                                      std::span<std::size_t> permutation);

/// The move over this frame's time against the world (see the Rust version). True: it hit something.
/// Original: PM_SlideMove at 0x005302A0 (CoD2MP_s.exe 1.3; Quake III's name; (pm, pml, gravity) on the stack)
bool slide_move(pmove::Pmove& pm, pmove::MoveLocal& pml, bool gravity, trace::MoveHost& host);

/// The event of a step the view smooths out (EV_STEP_VIEW); the first entity that isn't a player.
inline constexpr std::int32_t kEvStepView = 0x8F;
inline constexpr std::uint16_t kMaxClients = 64;

/// slide_move that also climbs steps (see the Rust version).
/// Original: PM_StepSlideMove at 0x00530AC0 (CoD2MP_s.exe 1.3; Quake III's name; (pm, pml, gravity) on the stack)
void step_slide_move(pmove::Pmove& pm, pmove::MoveLocal& pml, bool gravity, const pmove::MoveSettings& settings,
                     trace::MoveHost& host);

}  // namespace iw_pc::game::slide
