#include "iw_pc/game/jump.h"

#include "iw_pc/shared/math.h"

namespace iw_pc::game::jump {

using x87::F24;
namespace pmv = iw_pc::game::pmove;
namespace ps = pmv::ps;
namespace pm_flags = pmv::pm_flags;

namespace {
/// How long a jump may wait for its landing's slowdown (pm_time, milliseconds).
constexpr std::int32_t kJumpLandTime = 1800;
}  // namespace

void jump_activate_slowdown(pmv::PlayerState& p) {
    if (p.integer(ps::kPmTime) == 0) {
        p.set_integer(ps::kPmFlags, p.pm_flags() | pm_flags::kJumping);
        p.set_integer(ps::kPmTime, kJumpLandTime);
    }
}

void jump_apply_slowdown(pmv::PlayerState& p, const pmv::MoveSettings& settings) {
    auto l = F24::load;
    std::int32_t time = p.integer(ps::kPmTime);
    F24 scale = l(1.0f);
    if (time > kJumpLandTime) {
        p.set_integer(ps::kPmFlags, p.pm_flags() & ~pm_flags::kJumping);
        p.set_integer(ps::kJumpOriginZ, 0);
        scale = l(0.65f);
    } else if (time == 0) {
        F24 top = l(p.real(ps::kJumpOriginZ)) + l(18.0f);
        if (top > l(p.real(ps::kOrigin + 8))) {  // (at or below, or NaN: the short one)
            scale = l(0.65f);
            p.set_integer(ps::kPmTime, kJumpLandTime);
        } else {
            scale = l(0.5f);
            p.set_integer(ps::kPmTime, 1200);
        }
    }
    if (!settings.jump_slowdown_enable) scale = l(1.0f);
    auto v = p.velocity();
    p.set_velocity({(scale * l(v[0])).store(), (scale * l(v[1])).store(), (scale * l(v[2])).store()});
}

F24 jump_get_land_factor(const pmv::PlayerState& p, const pmv::MoveSettings& settings) {
    if (!settings.jump_slowdown_enable) return F24::load(1.0f);
    std::int32_t time = p.integer(ps::kPmTime);
    if (time >= 1700) return F24::load(2.5f);
    return F24::integer(time) * F24::load(0.00088235294f) + F24::load(1.0f);
}

void jump_start(pmv::Pmove& pm, pmv::MoveLocal& pml, float height, const pmv::MoveSettings& settings) {
    auto l = F24::load;
    std::int32_t time = pm.server_time();
    pmv::PlayerState& p = pm.ps;
    F24 h = l(height);
    F24 squared = F24::integer(p.integer(ps::kGravity)) * (h + h);
    if ((p.pm_flags() & pm_flags::kJumping) != 0 && p.integer(ps::kPmTime) <= kJumpLandTime)
        squared = l(squared.store()) / jump_get_land_factor(p, settings);
    F24 speed = squared.sqrt();
    pml.set_integer(pmv::pml::kGroundPlane, 0);
    pml.set_integer(pmv::pml::kAlmostGroundPlane, 0);
    pml.set_integer(pmv::pml::kWalking, 0);
    p.set_integer(ps::kGroundEntityNum, pmv::kEntityNone);
    p.set_integer(ps::kJumpTime, time);
    p.set_integer(ps::kJumpOriginZ, p.integer(ps::kOrigin + 8));
    p.set_integer(ps::kPmTime, 0);
    p.set_integer(ps::kPmFlags, p.pm_flags() | pm_flags::kJumping);
    p.set_real(ps::kVelocity + 8, speed.store());
    F24 spread = l(settings.jump_spread_add) + l(p.real(ps::kAimSpreadScale));
    p.set_real(ps::kAimSpreadScale, spread.store());
    if (spread > l(255.0f)) p.set_real(ps::kAimSpreadScale, 255.0f);  // (only above; NaN doesn't)
}

void jump_add_surface_event(pmv::PlayerState& p, const pmv::MoveLocal& pml) {
    if ((p.pm_flags() & pm_flags::kLadder) != 0) {
        pmv::add_event(p, kEvJump + pmv::kLadderSurfaceType);
        return;
    }
    std::int32_t flags = pml.integer(pmv::pml::kGroundSurfaceFlags);
    if ((flags & pmv::kSurfNoSteps) != 0) return;
    std::int32_t surface = (flags >> 20) & 0x1F;
    if (surface == 0) return;
    pmv::add_event(p, kEvJump + surface);
}

void jump_push_off_ladder(pmv::PlayerState& p, const pmv::MoveLocal& pml, const pmv::MoveSettings& settings) {
    auto l = F24::load;
    p.set_real(ps::kVelocity + 8, (l(p.real(ps::kVelocity + 8)) * l(0.75f)).store());
    auto forward = pml.vec3(pmv::pml::kForward);
    std::array<float, 3> flat{forward[0], forward[1], 0.0f};
    shared::math::vec3_normalize(flat);
    auto ladder = p.vec3(ps::kLadderVec);
    F24 facing = l(ladder[2]) * l(forward[2]) + l(ladder[1]) * l(forward[1]) + l(ladder[0]) * l(forward[0]);
    float x = flat[0], y = flat[1];
    if (facing < l(0.0f)) {  // (only below mirrors; NaN doesn't)
        F24 d = (l(flat[2]) * l(ladder[2]) + l(flat[1]) * l(ladder[1]) + l(flat[0]) * l(ladder[0])) * l(-2.0f);
        std::array<float, 3> push{};
        for (std::size_t i = 0; i < 3; ++i) push[i] = (d * l(ladder[i]) + l(flat[i])).store();
        shared::math::vec3_normalize(push);
        x = push[0];
        y = push[1];
    }
    F24 speed = l(settings.jump_ladder_push_vel);
    p.set_real(ps::kVelocity, (l(x) * speed).store());
    p.set_real(ps::kVelocity + 4, (l(y) * speed).store());
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~pm_flags::kLadder);
}

bool jump_check(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, trace::MoveHost& host) {
    std::int32_t since = static_cast<std::int32_t>(static_cast<std::uint32_t>(pm.server_time()) -
                                                   static_cast<std::uint32_t>(pm.ps.integer(ps::kJumpTime)));
    if (since < 500 || (pm.ps.pm_flags() & pm_flags::kNoJump) != 0 || pm.ps.pm_type() >= pmv::pm_type::kDead ||
        pmv::effective_stance(pm.ps) != pmv::stance::kStand)
        return false;
    std::int32_t buttons = pm.integer(pmv::pm::kCmd + pmv::cmd::kButtons);
    if ((buttons & pmv::button::kJump) == 0) return false;
    if ((pm.integer(pmv::pm::kOldCmd + pmv::cmd::kButtons) & pmv::button::kJump) != 0) {
        pm.set_integer(pmv::pm::kCmd + pmv::cmd::kButtons, buttons & ~pmv::button::kJump);
        return false;
    }
    jump_start(pm, pml, settings.jump_height, settings);
    jump_add_surface_event(pm.ps, pml);
    if ((pm.ps.pm_flags() & pm_flags::kLadder) != 0) jump_push_off_ladder(pm.ps, pml, settings);
    std::int32_t event = static_cast<std::int8_t>(pm.cmd()[pmv::cmd::kForwardMove]) < 0 ? trace::kAnimEtJumpBk
                                                                                         : trace::kAnimEtJump;
    host.anim_script_event(pm.ps, event, false, true);
    return true;
}

}  // namespace iw_pc::game::jump
