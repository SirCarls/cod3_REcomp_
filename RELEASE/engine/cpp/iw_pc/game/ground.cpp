#include "iw_pc/game/ground.h"

#include <algorithm>
#include <array>

#include "iw_pc/net/msg.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::ground {

using x87::F24;
namespace pmv = iw_pc::game::pmove;
namespace ps = pmv::ps;
namespace pml_at = pmv::pml;
namespace pm_at = pmv::pm;
namespace pm_flags = pmv::pm_flags;
namespace tr = iw_pc::game::trace;

namespace {

/// The steps correct_all_solid tries, in its order (the game's table at 0x005A1490).
constexpr std::array<std::array<float, 3>, 26> kAllSolidSteps{{
    {0, 0, 1},  {-1, 0, 1},  {0, -1, 1},  {1, 0, 1},  {0, 1, 1},  {-1, 0, 0},   {0, -1, 0},
    {1, 0, 0},  {0, 1, 0},   {0, 0, -1},  {-1, 0, -1}, {0, -1, -1}, {1, 0, -1},  {0, 1, -1},
    {-1, -1, 1}, {1, -1, 1}, {1, 1, 1},   {-1, 1, 1},  {-1, -1, 0}, {1, -1, 0},  {1, 1, 0},
    {-1, 1, 0}, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
}};

/// The ground's surface type for a landing: 0 on a surface without footsteps.
std::int32_t landing_surface(const pmv::MoveLocal& pml) {
    std::int32_t flags = pml.integer(pml_at::kGroundSurfaceFlags);
    return (flags & pmv::kSurfNoSteps) != 0 ? 0 : (flags >> 20) & 0x1F;
}

std::array<float, 3> scaled(const std::array<float, 3>& v, F24 scale) {
    auto l = F24::load;
    return {(scale * l(v[0])).store(), (scale * l(v[1])).store(), (scale * l(v[2])).store()};
}

std::array<float, 3> times_067(const std::array<float, 3>& v) {
    auto l = F24::load;
    return {(l(v[0]) * l(0.67f)).store(), (l(v[1]) * l(0.67f)).store(), (l(v[2]) * l(0.67f)).store()};
}

void leave_ground(pmv::PlayerState& p, pmv::MoveLocal& pml) {
    p.set_integer(ps::kGroundEntityNum, pmv::kEntityNone);
    pml.set_integer(pml_at::kGroundPlane, 0);
    pml.set_integer(pml_at::kAlmostGroundPlane, 0);
    pml.set_integer(pml_at::kWalking, 0);
}

void end_jump(pmv::PlayerState& p) {
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~pm_flags::kJumping);
    p.set_integer(ps::kJumpOriginZ, 0);
}

std::int32_t jump_event(const pmv::Pmove& pm) {
    return static_cast<std::int8_t>(pm.cmd()[pmv::cmd::kForwardMove]) < 0 ? tr::kAnimEtJumpBk : tr::kAnimEtJump;
}

std::int32_t low32(F24 x) { return static_cast<std::int32_t>(static_cast<std::uint64_t>(net::ftol2(x.v))); }

}  // namespace

std::int32_t medium_landing_for_surface(const pmv::MoveLocal& pml) {
    std::int32_t surface = landing_surface(pml);
    return surface == 0 ? 0 : surface + pmv::kEvFootstepRun;
}

std::int32_t hard_landing_for_surface(const pmv::MoveLocal& pml) { return kEvLanding + landing_surface(pml); }

void crash_land(pmv::PlayerState& p, const pmv::MoveLocal& pml, const pmv::MoveSettings& settings,
                tr::MoveHost& host) {
    auto l = F24::load;
    F24 gravity = F24::integer(p.integer(ps::kGravity));
    float vel = pml.real(pml_at::kPreviousVelocity + 8);
    float acc = (-gravity).store();
    float a = (-gravity * l(0.5f)).store();
    F24 c = l(pml.real(pml_at::kPreviousOrigin + 8)) - l(p.real(ps::kOrigin + 8));
    F24 den = l(vel) * l(vel) - c * l(a) * l(4.0f);
    if (den < l(0.0f)) return;  // (only below; NaN goes on)
    F24 t = (-l(vel) - den.sqrt()) / (l(a) + l(a));
    F24 landing = (t * l(acc) + l(vel)) * l(-1.0f);
    F24 height = l((landing * landing / (gravity + gravity)).store());

    F24 min = l(settings.fall_damage_min_height), max = l(settings.fall_damage_max_height);
    std::int32_t damage = 0;
    if (max <= min) {  // (at or below warns; NaN goes on)
        host.print(kFallDamageHeightsWarning);
    } else if (!(height <= min) && (pml.integer(pml_at::kGroundSurfaceFlags) & pmv::kSurfNoDamage) == 0 &&
               p.pm_type() < pmv::pm_type::kDead) {
        if (height >= max) {  // (below, or NaN, is a share)
            damage = 100;
        } else {
            std::int32_t n = low32((height - min) / (max - min) * l(100.0f));
            auto un = static_cast<std::uint32_t>(n);
            std::int32_t capped = static_cast<std::int32_t>(un - 100u) >= 0 ? 100 : n;
            damage = static_cast<std::int32_t>(0u - un) >= 0 ? 0 : capped;
        }
    }

    std::int32_t hard = 0;
    if (!(height <= l(12.0f))) {  // (above, or NaN, lands hard)
        hard = std::min(low32(((height - l(12.0f)) * l(0.03846154f) + l(1.0f)) * l(4.0f)), 24);
        host.anim_script_event(p, tr::kAnimEtLand, false, true);  // (BG_AnimScriptEvent, inlined)
    }

    if (damage != 0) {
        if (damage < 100 && (pml.integer(pml_at::kGroundSurfaceFlags) & pmv::kSurfSlick) == 0) {
            std::int32_t time = damage * 35 + 500;
            F24 scale = l(0.5f);
            if (time > 2000) {
                scale = l(0.2f);
                time = 2000;
            } else if (time <= 500) {
                scale = l(0.5f);
            } else if (time >= 1500) {
                scale = l(0.2f);
            } else {
                scale = l(0.5f) - (F24::integer(time) - l(500.0f)) * l(0.001f) * l(0.3f);
            }
            p.set_integer(ps::kPmTime, time);
            p.set_integer(ps::kPmFlags, p.pm_flags() | pm_flags::kSlide);
            p.set_velocity(scaled(p.velocity(), scale));
        } else {
            p.set_velocity(times_067(p.velocity()));
        }
        pmv::add_predictable_event(kEvLandingPain + landing_surface(pml), damage, p);
        return;
    }

    if (!(height > l(4.0f))) return;  // (at or below, or NaN, is nothing)
    if (height < l(8.0f)) {
        std::int32_t surface = landing_surface(pml);
        pmv::add_event(p, surface == 0 ? 0 : surface + pmv::kEvFootstepWalk);
    } else if (height < l(12.0f)) {
        pmv::add_event(p, medium_landing_for_surface(pml));
    } else {
        p.set_velocity(times_067(p.velocity()));
        pmv::add_predictable_event(hard_landing_for_surface(pml), hard, p);
    }
}

bool correct_all_solid(pmv::Pmove& pm, pmv::MoveLocal& pml, tr::Trace& trace, tr::MoveHost& host) {
    auto l = F24::load;
    for (const auto& step : kAllSolidSteps) {
        std::int32_t mask = pm.integer(pm_at::kTraceMask);
        auto origin = pm.ps.vec3(ps::kOrigin);
        std::array<float, 3> point{};
        for (std::size_t i = 0; i < 3; ++i) point[i] = (l(step[i]) + l(origin[i])).store();
        std::int32_t client = pm.ps.integer(ps::kClientNum);
        auto mins = pm.vec3(pm_at::kMins), maxs = pm.vec3(pm_at::kMaxs);
        trace = pmv::player_trace(pm, point, mins, maxs, point, client, mask, host);
        if (trace.start_solid()) continue;
        pm.ps.set_vec3(ps::kOrigin, point);
        std::array<float, 3> down{point[0], point[1], (l(point[2]) - l(1.0f)).store()};
        mask = pm.integer(pm_at::kTraceMask);
        client = pm.ps.integer(ps::kClientNum);
        trace = pmv::player_trace(pm, point, mins, maxs, down, client, mask, host);
        pml.set_ground_trace(trace);
        F24 f = l(trace.fraction());
        auto now = pm.ps.vec3(ps::kOrigin);
        std::array<float, 3> moved{};
        for (std::size_t i = 0; i < 3; ++i) moved[i] = ((l(down[i]) - l(now[i])) * f + l(now[i])).store();
        pm.ps.set_vec3(ps::kOrigin, moved);
        return true;
    }
    leave_ground(pm.ps, pml);
    end_jump(pm.ps);
    return false;
}

void ground_trace_missed(pmv::Pmove& pm, pmv::MoveLocal& pml, tr::MoveHost& host) {
    auto l = F24::load;
    auto origin = pm.ps.vec3(ps::kOrigin);
    std::uint8_t handler = pm.bytes[pm_at::kHandler];
    std::int32_t mask = pm.integer(pm_at::kTraceMask);
    std::int32_t client = pm.ps.integer(ps::kClientNum);
    auto mins = pm.vec3(pm_at::kMins), maxs = pm.vec3(pm_at::kMaxs);
    if (pm.ps.integer(ps::kGroundEntityNum) != pmv::kEntityNone) {
        std::array<float, 3> point{origin[0], origin[1], (l(origin[2]) - l(64.0f)).store()};
        tr::Trace t = host.trace(handler, origin, mins, maxs, point, client, mask);
        F24 fraction = l(t.fraction());
        if (fraction.v == 1.0) {  // (only equal falls; NaN doesn't)
            host.anim_script_event(pm.ps, jump_event(pm), false, true);  // (BG_AnimScriptEvent, inlined)
            pml.set_integer(pml_at::kAlmostGroundPlane, 0);
        } else {
            pml.set_integer(pml_at::kAlmostGroundPlane, fraction < l(0.015625f) ? 1 : 0);  // (NaN isn't near)
        }
    } else {
        std::array<float, 3> point{origin[0], origin[1], (l(origin[2]) - l(1.0f)).store()};
        tr::Trace t = host.trace(handler, origin, mins, maxs, point, client, mask);
        pml.set_integer(pml_at::kAlmostGroundPlane, l(t.fraction()).v != 1.0 ? 1 : 0);  // (NaN hit something)
    }
    pm.ps.set_integer(ps::kGroundEntityNum, pmv::kEntityNone);
    pml.set_integer(pml_at::kGroundPlane, 0);
    pml.set_integer(pml_at::kWalking, 0);
}

void ground_trace(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, tr::MoveHost& host) {
    auto l = F24::load;
    auto origin = pm.ps.vec3(ps::kOrigin);
    std::array<float, 3> start = origin, end{};
    if ((pm.ps.integer(ps::kEFlags) & pmv::kEFlagsMounted) != 0) {
        end = {origin[0], origin[1], (l(origin[2]) - l(1.0f)).store()};
    } else {
        start = {origin[0], origin[1], (l(origin[2]) + l(0.25f)).store()};
        end = {origin[0], origin[1], (l(origin[2]) - l(0.25f)).store()};
    }
    auto mins = pm.vec3(pm_at::kMins), maxs = pm.vec3(pm_at::kMaxs);
    std::int32_t mask = pm.integer(pm_at::kTraceMask);
    std::int32_t client = pm.ps.integer(ps::kClientNum);
    tr::Trace trace = pmv::player_trace(pm, start, mins, maxs, end, client, mask, host);
    pml.set_ground_trace(trace);
    if (trace.all_solid() && !correct_all_solid(pm, pml, trace, host)) return;
    if (trace.start_solid()) {
        start[2] = (l(pm.ps.real(ps::kOrigin + 8)) - l(0.001f)).store();  // (only the height is the origin's now)
        mask = pm.integer(pm_at::kTraceMask);
        client = pm.ps.integer(ps::kClientNum);
        trace = pmv::player_trace(pm, start, mins, maxs, end, client, mask, host);
        if (trace.start_solid()) {
            leave_ground(pm.ps, pml);
            return;
        }
        pml.set_ground_trace(trace);
    }
    if (l(trace.fraction()).v == 1.0) {  // (only equal misses; NaN doesn't)
        ground_trace_missed(pm, pml, host);
        return;
    }

    auto v = pm.ps.velocity();
    auto n = trace.normal();
    if ((pm.ps.pm_flags() & pm_flags::kLadder) == 0 && l(v[2]) > l(0.0f) &&
        l(n[1]) * l(v[1]) + l(n[2]) * l(v[2]) + l(n[0]) * l(v[0]) > l(10.0f)) {
        host.anim_script_event(pm.ps, jump_event(pm), false, false);  // (BG_AnimScriptEvent, inlined)
        leave_ground(pm.ps, pml);
        return;
    }
    if (l(n[2]) < l(0.7f)) {  // (only below is too steep; NaN isn't)
        pm.ps.set_integer(ps::kGroundEntityNum, pmv::kEntityNone);
        pml.set_integer(pml_at::kGroundPlane, 1);
        pml.set_integer(pml_at::kAlmostGroundPlane, 1);
        pml.set_integer(pml_at::kWalking, 0);
        end_jump(pm.ps);
        return;
    }
    pml.set_integer(pml_at::kGroundPlane, 1);
    pml.set_integer(pml_at::kAlmostGroundPlane, 1);
    pml.set_integer(pml_at::kWalking, 1);
    if (pm.ps.integer(ps::kGroundEntityNum) == pmv::kEntityNone) crash_land(pm.ps, pml, settings, host);
    std::int32_t entity = trace.entity_num();
    pm.ps.set_integer(ps::kGroundEntityNum, entity);
    pmv::add_touch_ent(pm, entity);
}

}  // namespace iw_pc::game::ground
