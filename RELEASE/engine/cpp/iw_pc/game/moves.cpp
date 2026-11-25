#include "iw_pc/game/moves.h"

#include "iw_pc/game/jump.h"
#include "iw_pc/game/slide.h"
#include "iw_pc/net/msg.h"
#include "iw_pc/shared/math.h"

namespace iw_pc::game::moves {

using x87::F24;
namespace pmv = iw_pc::game::pmove;

void air_move(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, trace::MoveHost& host) {
    namespace ml = pmv::pml;
    auto l = F24::load;
    pmv::friction(pm.ps, pml, settings);
    const std::uint8_t* cmd = pm.cmd();
    float fmove = static_cast<float>(static_cast<std::int8_t>(cmd[0x18]));
    float smove = static_cast<float>(static_cast<std::int8_t>(cmd[0x19]));
    float scale = pmv::cmd_scale(pm.ps, cmd, settings).store();

    auto forward = pml.vec3(ml::kForward), right = pml.vec3(ml::kRight);
    forward[2] = 0.0f;
    right[2] = 0.0f;
    shared::math::vec3_normalize(forward);
    shared::math::vec3_normalize(right);
    pml.set_vec3(ml::kForward, forward);
    pml.set_vec3(ml::kRight, right);

    std::array<float, 3> wish{};
    for (std::size_t i = 0; i < 2; ++i) wish[i] = (l(smove) * l(right[i]) + l(fmove) * l(forward[i])).store();
    float wish_speed = (l(shared::math::vec3_normalize(wish)) * l(scale)).store();
    pmv::accelerate(pm.ps, pml, wish, wish_speed, 1.0f, settings);

    if (pml.integer(ml::kGroundPlane) != 0) {  // along the surface (its own order of the dot product)
        auto n = pml.vec3(ml::kGroundNormal);
        auto v = pm.ps.velocity();
        F24 backoff = l(n[0]) * l(v[0]) + l(v[2]) * l(n[2]) + l(v[1]) * l(n[1]);
        F24 change = -(backoff - backoff.abs() * l(0.001f));
        std::array<float, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) out[i] = (change * l(n[i]) + l(v[i])).store();
        pm.ps.set_velocity(out);
    }

    slide::step_slide_move(pm, pml, true, settings, host);
    pmv::set_movement_dir(pm, pml);
}

namespace {
/// v along a plane of normal n, in walk_move's order ((v0*n0 + v1*n1) + n2*v2).
std::array<float, 3> along_ground(const std::array<float, 3>& v, const std::array<float, 3>& n) {
    auto l = F24::load;
    F24 b = l(v[0]) * l(n[0]) + l(v[1]) * l(n[1]) + l(n[2]) * l(v[2]);
    F24 change = -(b - b.abs() * l(0.001f));
    return {(change * l(n[0]) + l(v[0])).store(), (change * l(n[1]) + l(v[1])).store(), (change * l(n[2]) + l(v[2])).store()};
}
}  // namespace

void walk_move(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, trace::MoveHost& host) {
    namespace ml = pmv::pml;
    namespace ps = pmv::ps;
    auto l = F24::load;
    if ((pm.ps.pm_flags() & pmv::pm_flags::kJumping) != 0) jump::jump_apply_slowdown(pm.ps, settings);
    if (jump::jump_check(pm, pml, settings, host)) {
        air_move(pm, pml, settings, host);
        return;
    }
    pmv::friction(pm.ps, pml, settings);
    std::array<std::uint8_t, pmv::kUsercmdSize> cmd{};
    std::memcpy(cmd.data(), pm.cmd(), cmd.size());
    float fmove = static_cast<float>(static_cast<std::int8_t>(cmd[pmv::cmd::kForwardMove]));
    float smove = static_cast<float>(static_cast<std::int8_t>(cmd[pmv::cmd::kRightMove]));
    float scale = pmv::cmd_scale_walk(pm, cmd.data(), settings, host).store();
    std::int32_t timer = pm.ps.integer(ps::kDamageTimer);
    scale = (pmv::damage_scale_walk(timer, settings) * l(scale)).store();
    auto msec = static_cast<std::int32_t>(net::ftol2((l(pml.frame_time()) * l(1000.0f)).v));
    timer = static_cast<std::int32_t>(static_cast<std::uint32_t>(timer) - static_cast<std::uint32_t>(msec));
    pm.ps.set_integer(ps::kDamageTimer, timer <= 0 ? 0 : timer);

    auto n = pml.vec3(ml::kGroundNormal);
    auto forward = pml.vec3(ml::kForward);
    // (The forward's z is 0: the original's dot product takes n2 * 0, its z change * n2 alone.)
    F24 b = l(forward[0]) * l(n[0]) + l(n[1]) * l(forward[1]) + l(n[2]) * l(0.0f);
    F24 change = -(b - b.abs() * l(0.001f));
    forward = {(change * l(n[0]) + l(forward[0])).store(), (change * l(n[1]) + l(forward[1])).store(),
               (change * l(n[2])).store()};
    auto right = pml.vec3(ml::kRight);
    right[2] = 0.0f;
    right = along_ground(right, n);
    shared::math::vec3_normalize(forward);
    shared::math::vec3_normalize(right);
    pml.set_vec3(ml::kForward, forward);
    pml.set_vec3(ml::kRight, right);

    std::array<float, 3> wish{};
    for (std::size_t i = 0; i < 3; ++i) wish[i] = (l(smove) * l(right[i]) + l(fmove) * l(forward[i])).store();
    float wish_speed = (l(shared::math::vec3_normalize(wish)) * l(scale)).store();
    bool slick = (pml.integer(ml::kGroundSurfaceFlags) & pmv::kSurfSlick) != 0 ||
                 (pm.ps.pm_flags() & pmv::pm_flags::kTimeKnockback) != 0;
    float accel = 9.0f;
    if (slick) accel = 1.0f;
    else if (pmv::effective_stance(pm.ps) == pmv::stance::kProne) accel = 19.0f;
    else if (pmv::effective_stance(pm.ps) == pmv::stance::kCrouch) accel = 12.0f;
    if ((pm.ps.pm_flags() & pmv::pm_flags::kSlide) != 0) accel = (l(accel) * l(0.25f)).store();
    pmv::accelerate(pm.ps, pml, wish, wish_speed, accel, settings);
    if (slick) {
        F24 fall = F24::integer(pm.ps.integer(ps::kGravity)) * l(pml.frame_time());
        pm.ps.set_real(ps::kVelocity + 8, (l(pm.ps.real(ps::kVelocity + 8)) - fall).store());
    }

    // Along the ground, at the old speed unless that turns it around.
    auto old = pm.ps.velocity();
    F24 o0 = l(old[0]), o1 = l(old[1]), o2 = l(old[2]);
    float speed = (o0 * o0 + o1 * o1 + o2 * o2).sqrt().store();
    b = l(n[0]) * o0 + l(n[1]) * o1 + o2 * l(n[2]);
    change = -(b - b.abs() * l(0.001f));
    std::array<float, 3> v{(change * l(n[0]) + o0).store(), (change * l(n[1]) + o1).store(), (change * l(n[2]) + o2).store()};
    pm.ps.set_velocity(v);
    if (o2 * l(v[2]) + o1 * l(v[1]) + o0 * l(v[0]) > l(0.0f)) {  // (only above keeps the speed)
        shared::math::vec3_normalize(v);
        pm.ps.set_velocity({(l(speed) * l(v[0])).store(), (l(speed) * l(v[1])).store(), (l(speed) * l(v[2])).store()});
    }

    v = pm.ps.velocity();
    if (!(v[0] == 0.0f && v[1] == 0.0f)) slide::step_slide_move(pm, pml, false, settings, host);  // (NaN slides)
    pmv::set_movement_dir(pm, pml);
}

namespace {

/// A noclip or UFO move's up move from the lean buttons (computed: 0 + 127 as the UFO move adds it).
float up_move(std::int32_t buttons, bool computed) {
    auto l = F24::load;
    float up = 0.0f;
    if ((buttons & pmv::button::kLeanRight) != 0) up = computed ? (l(0.0f) + l(127.0f)).store() : 127.0f;
    if ((buttons & pmv::button::kLeanLeft) != 0) up = (l(up) - l(127.0f)).store();
    return up;
}

void fly_through(pmv::PlayerState& ps, float frame_time) {
    auto l = F24::load;
    auto v = ps.velocity();
    auto origin = ps.vec3(pmv::ps::kOrigin);
    std::array<float, 3> moved{};
    for (std::size_t i = 0; i < 3; ++i) moved[i] = (l(frame_time) * l(v[i]) + l(origin[i])).store();
    ps.set_vec3(pmv::ps::kOrigin, moved);
}

}  // namespace

void noclip_move(pmv::Pmove& pm, const pmv::MoveLocal& pml, const pmv::MoveSettings& settings) {
    namespace ml = pmv::pml;
    auto l = F24::load;
    pm.ps.set_integer(pmv::ps::kViewHeightTarget, pmv::kStandViewHeight);
    auto v = pm.ps.velocity();
    F24 speed = (l(v[0]) * l(v[0]) + l(v[1]) * l(v[1]) + l(v[2]) * l(v[2])).sqrt();
    if (speed < l(1.0f)) {  // (only below stops; NaN goes on)
        pm.ps.set_velocity({0.0f, 0.0f, 0.0f});
    } else {
        F24 friction = l(settings.friction) * l(1.5f);
        F24 stop = l(settings.stop_speed);
        F24 control = speed < stop ? stop : speed;
        F24 drop = control * l(pml.frame_time()) * friction;
        F24 new_speed = speed - drop;
        if (new_speed < l(0.0f)) new_speed = l(0.0f);
        F24 ratio = new_speed / speed;
        pm.ps.set_velocity({(ratio * l(v[0])).store(), (ratio * l(v[1])).store(), (ratio * l(v[2])).store()});
    }
    const std::uint8_t* cmd = pm.cmd();
    float fmove = F24::integer(static_cast<std::int8_t>(cmd[pmv::cmd::kForwardMove])).store();
    float smove = F24::integer(static_cast<std::int8_t>(cmd[pmv::cmd::kRightMove])).store();
    float umove = up_move(pm.integer(pmv::pm::kCmd + pmv::cmd::kButtons), false);
    float scale = pmv::move_scale(pm.ps, fmove, smove, umove, settings).store();
    auto forward = pml.vec3(ml::kForward), right = pml.vec3(ml::kRight), up = pml.vec3(ml::kUp);
    std::array<float, 3> wish{};
    for (std::size_t i = 0; i < 3; ++i)
        wish[i] = (l(umove) * l(up[i]) + l(smove) * l(right[i]) + l(fmove) * l(forward[i])).store();
    float length = shared::math::vec3_normalize(wish);
    float wish_speed = (l(length) * l(scale)).store();
    pmv::accelerate(pm.ps, pml, wish, wish_speed, 9.0f, settings);
    fly_through(pm.ps, pml.frame_time());
}

void ufo_move(pmv::Pmove& pm, const pmv::MoveLocal& pml, const pmv::MoveSettings& settings) {
    namespace ml = pmv::pml;
    auto l = F24::load;
    pm.ps.set_integer(pmv::ps::kViewHeightTarget, pmv::kStandViewHeight);
    const std::uint8_t* cmd = pm.cmd();
    float fmove = F24::integer(static_cast<std::int8_t>(cmd[pmv::cmd::kForwardMove])).store();
    float smove = F24::integer(static_cast<std::int8_t>(cmd[pmv::cmd::kRightMove])).store();
    float umove = up_move(pm.integer(pmv::pm::kCmd + pmv::cmd::kButtons), true);
    bool still = fmove == 0.0f && smove == 0.0f && umove == 0.0f;
    auto v = pm.ps.velocity();
    float squared = (l(v[0]) * l(v[0]) + l(v[1]) * l(v[1]) + l(v[2]) * l(v[2])).store();
    float speed = l(squared).sqrt().store();
    if (still || l(speed) < l(1.0f)) {  // (only below stops; NaN goes on)
        pm.ps.set_velocity({0.0f, 0.0f, 0.0f});
    } else {
        float friction = (l(settings.friction) * l(1.5f)).store();
        float control = l(speed) < l(settings.stop_speed) ? settings.stop_speed : speed;
        float drop = (l(control) * l(pml.frame_time()) * l(friction) + l(0.0f)).store();
        float new_speed = (l(speed) - l(drop)).store();
        if (l(new_speed) < l(0.0f)) new_speed = 0.0f;
        float ratio = (l(new_speed) / l(speed)).store();
        pm.ps.set_velocity({(l(ratio) * l(v[0])).store(), (l(ratio) * l(v[1])).store(), (l(ratio) * l(v[2])).store()});
    }
    float scale = pmv::move_scale(pm.ps, fmove, smove, umove, settings).store();
    auto right = pml.vec3(ml::kRight);
    float t = (l(right[2]) * l(0.0f)).store();
    std::array<float, 3> level{(l(t) - l(right[1])).store(), (l(right[0]) - l(t)).store(),
                               (l(right[1]) * l(0.0f) - l(right[0]) * l(0.0f)).store()};
    float lift = (l(umove) * l(0.0f)).store();
    F24 f = l(fmove), r = l(smove);
    std::array<float, 3> wish{(l(level[0]) * f + r * l(right[0]) + l(lift)).store(),
                              (r * l(right[1]) + l(level[1]) * f + l(lift)).store(),
                              (r * l(right[2]) + l(level[2]) * f + l(umove)).store()};
    float length = shared::math::vec3_normalize(wish);
    float wish_speed = (l(length) * l(scale)).store();
    pmv::accelerate(pm.ps, pml, wish, wish_speed, 9.0f, settings);
    fly_through(pm.ps, pml.frame_time());
}

void dead_move(pmv::PlayerState& ps, const pmv::MoveLocal& pml) {
    auto l = F24::load;
    if (!pml.walking()) return;
    auto v = ps.velocity();
    F24 v0 = l(v[0]), v1 = l(v[1]), v2 = l(v[2]);
    F24 forward = (v0 * v0 + v1 * v1 + v2 * v2).sqrt() - l(20.0f);
    if (forward <= l(0.0f)) {  // (0 or below stops; NaN goes on)
        ps.set_velocity({0.0f, 0.0f, 0.0f});
        return;
    }
    float speed = forward.store();
    shared::math::vec3_normalize(v);
    ps.set_velocity({(l(speed) * l(v[0])).store(), (l(speed) * l(v[1])).store(), (l(speed) * l(v[2])).store()});
}

void fly_move(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, trace::MoveHost& host) {
    namespace ml = pmv::pml;
    auto l = F24::load;
    pmv::friction(pm.ps, pml, settings);
    std::array<std::uint8_t, pmv::kUsercmdSize> cmd{};
    std::memcpy(cmd.data(), pm.cmd(), cmd.size());
    F24 scale = pmv::cmd_scale(pm.ps, cmd.data(), settings);
    auto forward = pml.vec3(ml::kForward), right = pml.vec3(ml::kRight);
    std::array<float, 3> wish{};
    F24 z = l(0.0f);
    if (scale.v != 0.0) {  // (fucompp, jp: only 0 moves nowhere; NaN goes on)
        F24 f = F24::integer(static_cast<std::int8_t>(cmd[pmv::cmd::kForwardMove]));
        F24 r = F24::integer(static_cast<std::int8_t>(cmd[pmv::cmd::kRightMove]));
        wish[0] = ((r * l(right[0]) + f * l(forward[0])) * scale).store();
        wish[1] = ((f * l(forward[1]) + r * l(right[1])) * scale).store();
        z = (f * l(forward[2]) + r * l(right[2])) * scale;
    }
    if (pm.ps.integer(pmv::ps::kSpeed) != 0) {
        float up = pmv::move_scale(pm.ps, 0.0f, 0.0f, 127.0f, settings).store();
        std::int32_t buttons = pm.integer(pmv::pm::kCmd + pmv::cmd::kButtons);
        float stored = z.store();
        z = (buttons & pmv::button::kLeanLeft) != 0 ? l(stored) - l(up) * l(127.0f) : l(stored);
        if ((buttons & pmv::button::kLeanRight) != 0) z = z + l(up) * l(127.0f);
    }
    wish[2] = z.store();
    float wish_speed = shared::math::vec3_normalize(wish);
    pmv::accelerate(pm.ps, pml, wish, wish_speed, 8.0f, settings);
    slide::step_slide_move(pm, pml, false, settings, host);
}

namespace {
/// ladder_move's sideways friction (see the Rust version's side_friction).
void side_friction(pmv::Pmove& pm, const pmv::MoveLocal& pml, const std::array<float, 3>& right) {
    namespace ps = pmv::ps;
    auto l = F24::load;
    std::array<float, 2> side{right[0], right[1]};
    shared::math::vec2_normalize(side);
    auto v = pm.ps.velocity();
    float dot = (l(side[1]) * l(v[1]) + l(side[0]) * l(v[0])).store();
    if (dot == 0.0f) return;  // (fucompp, jnp: only 0; NaN goes on)
    float back = (-l(dot)).store();
    float v0 = (l(back) * l(side[0]) + l(v[0])).store();
    float v1 = (l(side[1]) * l(back) + l(v[1])).store();
    pm.ps.set_real(ps::kVelocity, v0);
    pm.ps.set_real(ps::kVelocity + 4, v1);
    float x = (l(dot) * l(pml.frame_time()) * l(16.0f)).store();
    float ax = std::fabs(x);
    if (!(std::fabs(dot) > ax)) return;  // (only above goes on; NaN doesn't)
    if (ax < 1.0f) {
        std::uint32_t xb;
        std::memcpy(&xb, &x, 4);
        x = static_cast<std::int32_t>(xb) < 0 ? -1.0f : 1.0f;
    }
    float rest = (l(dot) - l(x)).store();
    pm.ps.set_real(ps::kVelocity, (l(side[0]) * l(rest) + l(v0)).store());
    pm.ps.set_real(ps::kVelocity + 4, (l(side[1]) * l(rest) + l(v1)).store());
}
}  // namespace

void ladder_move(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, trace::MoveHost& host) {
    namespace ml = pmv::pml;
    namespace ps = pmv::ps;
    auto l = F24::load;
    if (jump::jump_check(pm, pml, settings, host)) {
        air_move(pm, pml, settings, host);
        return;
    }
    float up_scale = ((l(pml.real(ml::kForward + 8)) + l(0.25f)) * l(2.5f)).store();
    if (up_scale > 1.0f) up_scale = 1.0f;  // (NaN stays)
    else if (up_scale < -1.0f) up_scale = -1.0f;
    auto forward = pml.vec3(ml::kForward);
    forward[2] = 0.0f;
    shared::math::vec3_normalize(forward);
    pml.set_vec3(ml::kForward, forward);
    auto right = pml.vec3(ml::kRight);
    right[2] = 0.0f;
    std::array<float, 3> flat{};
    shared::math::vec3_normalize_to(right, flat);
    auto ladder = pm.ps.vec3(ps::kLadderVec);
    float d = (-(l(flat[0]) * l(ladder[0]) + l(flat[1]) * l(ladder[1]) + l(flat[2]) * l(ladder[2]))).store();
    for (std::size_t i = 0; i < 3; ++i) right[i] = (l(d) * l(ladder[i]) + l(flat[i])).store();
    pml.set_vec3(ml::kRight, right);

    std::array<std::uint8_t, pmv::kUsercmdSize> cmd{};
    std::memcpy(cmd.data(), pm.cmd(), cmd.size());
    float scale = pmv::cmd_scale(pm.ps, cmd.data(), settings).store();
    auto fmove = static_cast<std::int8_t>(cmd[pmv::cmd::kForwardMove]);
    auto smove = static_cast<std::int8_t>(cmd[pmv::cmd::kRightMove]);
    std::array<float, 3> wish{};
    if (fmove != 0) {
        float f = F24::integer(fmove).store();
        wish[2] = (l(f) * l(scale) * l(up_scale) * l(0.5f)).store();
    }
    if (smove != 0) {
        float r = F24::integer(smove).store();
        scale = (l(r) * l(scale) * l(0.2f)).store();
        wish[0] = (l(scale) * l(right[0]) + l(0.0f)).store();
        wish[1] = (l(scale) * l(right[1]) + l(0.0f)).store();
        wish[2] = (l(scale) * l(right[2]) + l(wish[2])).store();
    }
    std::array<float, 3> dir{};
    float wish_speed = shared::math::vec3_normalize_to(wish, dir);
    pmv::accelerate(pm.ps, pml, dir, wish_speed, 9.0f, settings);

    if (fmove == 0) {  // toward 0 by gravity, never past it
        float v = pm.ps.real(ps::kVelocity + 8);
        F24 fall = l(F24::integer(pm.ps.integer(ps::kGravity)).store()) * l(pml.frame_time());
        float z;
        if (v > 0.0f) {
            z = (l(v) - fall).store();
            if (z < 0.0f) z = 0.0f;
        } else {
            z = (fall + l(v)).store();
            if (z > 0.0f) z = 0.0f;
        }
        pm.ps.set_real(ps::kVelocity + 8, z);
    }
    if (smove == 0) side_friction(pm, pml, right);
    if (!pml.walking()) {  // nothing into the ladder; climbing faster than moving away pushes off
        auto v = pm.ps.velocity();
        float dd = -(l(v[1]) * l(ladder[1]) + l(v[0]) * l(ladder[0])).store();
        float v0 = (l(dd) * l(ladder[0]) + l(v[0])).store();
        float v1 = (l(dd) * l(ladder[1]) + l(v[1])).store();
        pm.ps.set_real(ps::kVelocity, v0);
        pm.ps.set_real(ps::kVelocity + 4, v1);
        F24 horizontal = l(v0) * l(v0) + l(v1) * l(v1);
        F24 vertical = l(v[2]) * l(v[2]);
        if (vertical >= horizontal) {  // (below, or NaN, keeps it)
            pm.ps.set_real(ps::kVelocity, (l(v0) - l(ladder[0]) * l(50.0f)).store());
            pm.ps.set_real(ps::kVelocity + 4, (l(v1) - l(ladder[1]) * l(50.0f)).store());
        }
    }
    slide::step_slide_move(pm, pml, false, settings, host);

    float yaw = (l(shared::math::vectoyaw(ladder)) + l(180.0f)).store();
    float turn = (l(yaw) - l(pm.ps.real(ps::kViewAngles + 4))).store();
    auto dir_deg = static_cast<std::int32_t>(net::ftol2(static_cast<double>(shared::math::angle_normalize180(turn))));
    std::int32_t magnitude = dir_deg < 0 ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(dir_deg)) : dir_deg;
    if (magnitude > 75) dir_deg = dir_deg <= 0 ? -75 : 75;
    pm.ps.set_integer(ps::kMovementDir, static_cast<std::int8_t>(dir_deg));
}

void leave_ladder(pmv::PlayerState& p) {
    std::int32_t flags = p.pm_flags();
    if (flags & pmv::pm_flags::kLadder) flags |= pmv::pm_flags::kLadderFall;
    p.set_integer(pmv::ps::kPmFlags, flags & ~pmv::pm_flags::kLadder);
}

void check_ladder_move(pmv::Pmove& pm, pmv::MoveLocal& pml, trace::MoveHost& host) {
    namespace ps = pmv::ps;
    namespace fl = pmv::pm_flags;
    namespace ml = pmv::pml;
    auto l = F24::load;
    bool walking = pml.integer(ml::kWalking) != 0;
    if (walking) pm.ps.set_integer(ps::kPmFlags, pm.ps.pm_flags() & ~fl::kLadderFall);
    std::int32_t flags = pm.ps.pm_flags();
    if (pm.ps.integer(ps::kPmTime) != 0 && (flags & fl::kLadder) == 0 && (flags & (fl::kSlide | fl::kTimeKnockback)) != 0)
        return;
    float trace_dist = walking ? 8.0f : 30.0f;
    bool hanging = (flags & fl::kLadder) != 0 && pm.ps.integer(ps::kGroundEntityNum) == pmv::kEntityNone;
    std::array<float, 3> dir;
    if (hanging) {
        auto v = pm.ps.vec3(ps::kLadderVec);
        for (std::size_t i = 0; i < 3; ++i) dir[i] = (-l(v[i])).store();
    } else {
        auto forward = pml.vec3(ml::kForward);
        dir = {forward[0], forward[1], 0.0f};
        shared::math::vec3_normalize(dir);
    }
    if (pm.ps.pm_type() >= pmv::pm_type::kDead) {
        pm.ps.set_integer(ps::kGroundEntityNum, pmv::kEntityNone);
        for (std::size_t at : {ml::kGroundPlane, ml::kAlmostGroundPlane, ml::kWalking}) pml.set_integer(at, 0);
        leave_ladder(pm.ps);
        return;
    }
    std::int32_t target = pm.ps.integer(ps::kViewHeightTarget);
    std::int32_t since_jump = static_cast<std::int32_t>(static_cast<std::uint32_t>(pm.server_time()) -
                                                        static_cast<std::uint32_t>(pm.ps.integer(ps::kJumpTime)));
    if ((flags & fl::kLadderFall) != 0 || target == pmv::kProneViewHeight || since_jump < 300) {
        leave_ladder(pm.ps);
        return;
    }
    auto box_mins = pm.vec3(pmv::pm::kMins), box_maxs = pm.vec3(pmv::pm::kMaxs);
    std::array<float, 3> mins{(l(box_mins[0]) + l(6.0f)).store(), (l(box_mins[1]) + l(6.0f)).store(), 8.0f};
    std::array<float, 3> maxs{(l(box_maxs[0]) - l(6.0f)).store(), (l(box_maxs[1]) - l(6.0f)).store(), box_maxs[2]};
    if (l(maxs[2]).v < l(8.0f).v) maxs[2] = 8.0f;  // (only below is raised; NaN isn't)
    auto origin = pm.ps.vec3(ps::kOrigin);
    std::int32_t client = pm.ps.integer(ps::kClientNum);
    auto ladder_hit = [&](const trace::Trace& t) {  // (only below 1 hit; NaN didn't)
        return l(t.fraction()).v < 1.0 && (t.integer(trace::at::kSurfaceFlags) & kSurfLadder) != 0;
    };
    std::array<float, 3> spot;
    for (std::size_t i = 0; i < 3; ++i) spot[i] = (l(dir[i]) * l(trace_dist) + l(origin[i])).store();
    trace::Trace t = pmv::player_trace(pm, origin, mins, maxs, spot, client, pm.integer(pmv::pm::kTraceMask), host);
    bool backing = walking && static_cast<std::int8_t>(pm.cmd()[pmv::cmd::kForwardMove]) <= 0;
    if (ladder_hit(t) && !backing) {
        if (pm.ps.pm_flags() & fl::kLadder) return;
        pm.ps.set_vec3(ps::kLadderVec, t.normal());
        auto ladder = pm.ps.vec3(ps::kLadderVec);
        std::array<float, 3> into;
        for (std::size_t i = 0; i < 3; ++i) into[i] = ((-l(ladder[i])) * l(trace_dist) + l(origin[i])).store();
        trace::Trace again =
            pmv::player_trace(pm, origin, mins, maxs, into, client, pm.integer(pmv::pm::kTraceMask), host);
        if (ladder_hit(again)) {
            pm.ps.set_integer(ps::kPmFlags, pm.ps.pm_flags() | fl::kLadder);
            return;
        }
    }
    leave_ladder(pm.ps);
    if (hanging) host.anim_script_event(pm.ps, trace::kAnimEtJump, false, true);  // (BG_AnimScriptEvent, inlined)
}

}  // namespace iw_pc::game::moves
