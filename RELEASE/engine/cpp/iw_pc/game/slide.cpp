#include "iw_pc/game/slide.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "iw_pc/game/prone.h"
#include "iw_pc/net/msg.h"
#include "iw_pc/shared/math.h"

namespace iw_pc::game::slide {

using x87::F24;
namespace pmv = iw_pc::game::pmove;
using Vec = std::array<float, 3>;

namespace {

/// a > b (only "above"; NaN isn't).
bool above(F24 a, F24 b) { return a.v > b.v; }

/// a >= b as fcomp + test ah,1 + je takes it: not below, not unordered.
bool at_least(F24 a, F24 b) { return a.v >= b.v; }

/// The velocity clipped against a plane with every step stored (b: the dot product in the
/// caller's order).
Vec clip_stored(const Vec& v, const Vec& n, F24 backoff) {
    float b = backoff.store();
    float a = F24::load(b).abs().store();
    float c = (F24::load(b) - F24::load(a) * F24::load(0.001f)).store();
    F24 change = F24::load((-F24::load(c)).store());
    Vec out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = (change * F24::load(n[i]) + F24::load(v[i])).store();
    return out;
}

/// (a[o0]*b[o0] + a[o1]*b[o1]) + a[o2]*b[o2].
F24 dot_in(const Vec& a, const Vec& b, std::array<std::size_t, 3> order) {
    auto term = [&](std::size_t i) { return F24::load(a[i]) * F24::load(b[i]); };
    return term(order[0]) + term(order[1]) + term(order[2]);
}

}  // namespace

float permute_restrictive_clip_planes(const Vec& velocity, std::span<const Vec> planes, std::span<std::size_t> permutation) {
    std::array<float, kMaxClipPlanes> dots{};
    F24 v0 = F24::load(velocity[0]), v1 = F24::load(velocity[1]), v2 = F24::load(velocity[2]);
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const Vec& p = planes[i];
        dots[i] = (F24::load(p[0]) * v0 + F24::load(p[2]) * v2 + F24::load(p[1]) * v1).store();
        std::size_t j = i;
        while (j > 0 && !(dots[permutation[j - 1]] < dots[i])) {  // (only a smaller one stops; NaN shifts)
            permutation[j] = permutation[j - 1];
            --j;
        }
        permutation[j] = i;
    }
    return dots[permutation[0]];
}

bool slide_move(pmv::Pmove& pm, pmv::MoveLocal& pml, bool gravity, trace::MoveHost& host) {
    namespace ps = pmv::ps;
    namespace ml = pmv::pml;
    Vec primal = pm.ps.velocity();
    Vec end_velocity = primal;
    if (gravity) {
        float g = F24::integer(pm.ps.integer(ps::kGravity)).store();
        end_velocity[2] = (F24::load(end_velocity[2]) - F24::load(g) * F24::load(pml.frame_time())).store();
        primal[2] = end_velocity[2];
        Vec v = pm.ps.velocity();
        v[2] = ((F24::load(end_velocity[2]) + F24::load(v[2])) * F24::load(0.5f)).store();
        pm.ps.set_velocity(v);
        if (pml.integer(ml::kGroundPlane) != 0) {
            Vec n = pml.vec3(ml::kGroundNormal);
            Vec w = pm.ps.velocity();
            pm.ps.set_velocity(clip_stored(w, n, dot_in(w, n, {1, 2, 0})));
        }
    }

    float time_left = pml.frame_time();
    std::array<Vec, kMaxClipPlanes> planes{};
    std::size_t count = 0;
    if (pml.integer(ml::kGroundPlane) != 0) {
        planes[0] = pml.vec3(ml::kGroundNormal);
        count = 1;
    }
    shared::math::vec3_normalize_to(pm.ps.velocity(), planes[count]);
    ++count;

    std::array<std::size_t, kMaxClipPlanes> permutation{};
    int bump = 0;
    for (; bump < 4; ++bump) {
        Vec origin = pm.ps.vec3(ps::kOrigin);
        Vec v = pm.ps.velocity();
        Vec end{};
        for (std::size_t i = 0; i < 3; ++i) end[i] = (F24::load(time_left) * F24::load(v[i]) + F24::load(origin[i])).store();
        trace::Trace tr = pmv::player_trace(pm, origin, pm.vec3(pmv::pm::kMins), pm.vec3(pmv::pm::kMaxs), end,
                                            pm.ps.integer(ps::kClientNum), pm.integer(pmv::pm::kTraceMask), host);
        if (tr.all_solid()) {  // trapped in something solid: no vertical speed
            Vec w = pm.ps.velocity();
            w[2] = 0.0f;
            pm.ps.set_velocity(w);
            return true;
        }
        float fraction = tr.fraction();
        if (F24::load(fraction) > F24::load(0.0f)) {  // (only more than 0 moves; NaN doesn't)
            Vec o = pm.ps.vec3(ps::kOrigin);
            for (std::size_t i = 0; i < 3; ++i)
                o[i] = ((F24::load(end[i]) - F24::load(o[i])) * F24::load(fraction) + F24::load(o[i])).store();
            pm.ps.set_vec3(ps::kOrigin, o);
        }
        std::uint32_t fraction_bits;
        std::memcpy(&fraction_bits, &fraction, 4);
        if (fraction_bits == 0x3F800000u) break;
        pmv::add_touch_ent(pm, tr.entity_num());
        time_left = (F24::load(time_left) - F24::load(fraction) * F24::load(time_left)).store();
        if (count >= kMaxClipPlanes) {
            pm.ps.set_velocity({0.0f, 0.0f, 0.0f});
            return true;
        }

        Vec normal = tr.normal();
        bool same = false;
        for (std::size_t i = 0; i < count && !same; ++i) same = dot_in(normal, planes[i], {0, 1, 2}) > F24::load(0.999f);
        if (same) {  // the same plane as before: pushed off it a little
            Vec w = pm.ps.velocity();
            w = clip_stored(w, normal, dot_in(normal, w, {1, 2, 0}));
            for (std::size_t i = 0; i < 3; ++i) w[i] = (F24::load(normal[i]) + F24::load(w[i])).store();
            pm.ps.set_velocity(w);
            continue;
        }

        planes[count++] = normal;
        v = pm.ps.velocity();
        float into = permute_restrictive_clip_planes(v, std::span<const Vec>(planes.data(), count),
                                                     std::span<std::size_t>(permutation.data(), count));
        if (at_least(F24::load(into), F24::load(0.1f))) continue;
        float hit = (-F24::load(into)).store();
        if (F24::load(hit) > F24::load(pml.real(ml::kImpactSpeed))) pml.set_real(ml::kImpactSpeed, hit);

        const Vec first = planes[permutation[0]];
        Vec clip_velocity = clip_stored(v, first, dot_in(v, first, {1, 0, 2}));
        Vec end_clip_velocity = clip_stored(end_velocity, first, dot_in(end_velocity, first, {0, 2, 1}));
        for (std::size_t j = 1; j < count; ++j) {
            const Vec plane = planes[permutation[j]];
            if (at_least(dot_in(clip_velocity, plane, {2, 1, 0}), F24::load(0.1f))) continue;
            clip_velocity = clip_stored(clip_velocity, plane, dot_in(clip_velocity, plane, {0, 2, 1}));
            end_clip_velocity = clip_stored(end_clip_velocity, plane, dot_in(end_clip_velocity, plane, {0, 2, 1}));
            if (at_least(dot_in(clip_velocity, first, {2, 1, 0}), F24::load(0.0f))) continue;  // not back into the first
            // Slide along the crease of the two.
            F24 p0 = F24::load(first[0]), p1 = F24::load(first[1]), p2 = F24::load(first[2]);
            F24 q0 = F24::load(plane[0]), q1 = F24::load(plane[1]), q2 = F24::load(plane[2]);
            Vec dir = {(q2 * p1 - p2 * q1).store(), (p2 * q0 - q2 * p0).store(), (q1 * p0 - q0 * p1).store()};
            shared::math::vec3_normalize(dir);
            float d = dot_in(dir, pm.ps.velocity(), {2, 1, 0}).store();
            for (std::size_t i = 0; i < 3; ++i) clip_velocity[i] = (F24::load(dir[i]) * F24::load(d)).store();
            d = dot_in(dir, end_velocity, {1, 0, 2}).store();
            for (std::size_t i = 0; i < 3; ++i) end_clip_velocity[i] = (F24::load(dir[i]) * F24::load(d)).store();
            for (std::size_t k = 1; k < count; ++k) {  // a third plane the new move goes into: stop dead
                if (k == j) continue;
                if (!at_least(dot_in(clip_velocity, planes[permutation[k]], {2, 1, 0}), F24::load(0.1f))) {
                    pm.ps.set_velocity({0.0f, 0.0f, 0.0f});
                    return true;
                }
            }
        }
        pm.ps.set_velocity(clip_velocity);
        end_velocity = end_clip_velocity;
    }

    if (gravity) pm.ps.set_velocity(end_velocity);
    if (pm.ps.integer(ps::kPmTime) != 0) pm.ps.set_velocity(primal);  // (no change while a timer runs)
    return bump != 0;
}

void step_slide_move(pmv::Pmove& pm, pmv::MoveLocal& pml, bool gravity, const pmv::MoveSettings& settings,
                     trace::MoveHost& host) {
    namespace ps = pmv::ps;
    namespace ml = pmv::pml;
    namespace pf = pmv::pm_flags;
    auto l = F24::load;
    auto clear_jump = [](pmv::Pmove& m) {
        m.ps.set_integer(ps::kPmFlags, m.ps.pm_flags() & ~pf::kJumping);
        m.ps.set_real(ps::kJumpOriginZ, 0.0f);
    };
    std::int32_t flags = pm.ps.pm_flags();
    bool jumping = false;
    float step = 0.0f;
    bool had_ground;
    if ((flags & pf::kLadder) != 0) {
        clear_jump(pm);
        had_ground = false;
    } else if (pml.integer(ml::kGroundPlane) != 0) {
        had_ground = true;
    } else {
        if ((flags & pf::kJumping) != 0 && pm.ps.integer(ps::kPmTime) != 0) clear_jump(pm);
        had_ground = false;
    }

    const Vec start_o = pm.ps.vec3(ps::kOrigin);
    const Vec start_v = pm.ps.velocity();
    bool bumped = slide_move(pm, pml, gravity, host);

    flags = pm.ps.pm_flags();
    float step_size = (flags & pf::kProne) != 0 ? 10.0f : 18.0f;
    if (pm.ps.integer(ps::kGroundEntityNum) == pmv::kEntityNone) {
        if ((flags & pf::kJumping) != 0 && pm.ps.integer(ps::kPmTime) != 0) clear_jump(pm);
        bool climb = false;
        if (bumped && (pm.ps.pm_flags() & pf::kJumping) != 0 &&
            pmv::jump_get_step_height(pm.ps, start_o, step_size, settings)) {
            if (l(step_size) < l(1.0f)) return;  // (below 1 stops; NaN goes on)
            jumping = true;
            climb = true;
        }
        if (!climb) {  // on a ladder only when climbing
            if ((pm.ps.pm_flags() & pf::kLadder) == 0) return;
            if (!above(l(pm.ps.velocity()[2]), l(0.0f))) return;
        }
    }

    const Vec down_o = pm.ps.vec3(ps::kOrigin);
    const float moved[2] = {(l(down_o[0]) - l(start_o[0])).store(), (l(down_o[1]) - l(start_o[1])).store()};
    const Vec down_v = pm.ps.velocity();
    const Vec mins = pm.vec3(pmv::pm::kMins), maxs = pm.vec3(pmv::pm::kMaxs);
    const std::int32_t pass = pm.ps.integer(ps::kClientNum);

    if (bumped) {  // up a step
        Vec up{start_o[0], start_o[1], (l(start_o[2]) + l(step_size) + l(1.0f)).store()};
        trace::Trace tr = pmv::player_trace(pm, start_o, mins, maxs, up, pass, pm.integer(pmv::pm::kTraceMask), host);
        F24 rise = (l(step_size) + l(1.0f)) * l(tr.fraction()) - l(1.0f);
        step = rise.store();
        if (rise < l(1.0f)) {  // (1 or more, or NaN, steps)
            step = 0.0f;
        } else {
            pm.ps.set_vec3(ps::kOrigin, {up[0], up[1], (l(start_o[2]) + l(step)).store()});
            pm.ps.set_velocity(start_v);
            slide_move(pm, pml, gravity, host);
        }
    }

    if (had_ground || step != 0.0f || std::isnan(step)) {  // down again (equal to 0 skips; NaN doesn't)
        Vec o = pm.ps.vec3(ps::kOrigin);
        Vec down{o[0], o[1], (l(o[2]) - l(step)).store()};
        if (had_ground) down[2] = (l(down[2]) - l(9.0f)).store();
        trace::Trace tr = pmv::player_trace(pm, o, mins, maxs, down, pass, pm.integer(pmv::pm::kTraceMask), host);
        if (tr.entity_num() < kMaxClients) {  // onto a player: undone
            pm.ps.set_vec3(ps::kOrigin, down_o);
            pm.ps.set_velocity(down_v);
            return;
        }
        if (l(tr.fraction()) < l(1.0f)) {
            if (l(tr.normal()[2]) < l(0.3f)) {  // too steep: undone
                pm.ps.set_vec3(ps::kOrigin, down_o);
                pm.ps.set_velocity(down_v);
                return;
            }
            Vec cur = pm.ps.vec3(ps::kOrigin);
            Vec set{};
            for (std::size_t i = 0; i < 3; ++i) set[i] = ((l(down[i]) - l(cur[i])) * l(tr.fraction()) + l(cur[i])).store();
            pm.ps.set_vec3(ps::kOrigin, set);
            pm.ps.set_velocity(pmv::clip_velocity(pm.ps.velocity(), tr.normal()));
        } else if (step != 0.0f || std::isnan(step)) {
            Vec cur = pm.ps.vec3(ps::kOrigin);
            cur[2] = (l(cur[2]) - l(step)).store();
            pm.ps.set_vec3(ps::kOrigin, cur);
        }
    }

    // Did the step get further along the velocity than the plain slide?
    Vec o = pm.ps.vec3(ps::kOrigin);
    Vec v = pm.ps.velocity();
    F24 stepped = (l(o[1]) - l(start_o[1])) * l(v[1]) + (l(o[0]) - l(start_o[0])) * l(v[0]);
    F24 plain = l(moved[1]) * l(v[1]) + l(moved[0]) * l(v[0]) + l(0.001f);
    bool keep = !(plain.v >= stepped.v);  // (the plain slide at least as far takes it; NaN steps)
    if (keep && jumping) {
        F24 top = l(settings.jump_height) + l(pm.ps.real(ps::kJumpOriginZ));
        keep = !(top.v <= l(pm.ps.real(ps::kOrigin + 8)).v);  // (below the jump's height, or NaN, keeps it)
    }
    if (!keep) {
        pm.ps.set_vec3(ps::kOrigin, down_o);
        pm.ps.set_velocity(down_v);
        if (had_ground) {  // down 9 after the plain slide
            Vec down{down_o[0], down_o[1], (l(down_o[2]) - l(9.0f)).store()};
            trace::Trace tr = pmv::player_trace(pm, down_o, mins, maxs, down, pass, pm.integer(pmv::pm::kTraceMask), host);
            if (l(tr.fraction()) < l(1.0f)) {
                Vec set{};
                for (std::size_t i = 0; i < 3; ++i)
                    set[i] = ((l(down[i]) - l(down_o[i])) * l(tr.fraction()) + l(down_o[i])).store();
                pm.ps.set_vec3(ps::kOrigin, set);
                pm.ps.set_velocity(pmv::clip_velocity(pm.ps.velocity(), tr.normal()));
            }
        }
    }
    if (jumping) pmv::jump_clamp_velocity(pm.ps, down_o, settings);

    if (!had_ground || pm.ps.pm_type() >= pmv::pm_type::kDead) return;
    if (!prone::verify_prone_position(pm, start_o, start_v, host)) return;
    float delta = (l(pm.ps.real(ps::kOrigin + 8)) - l(down_o[2])).store();
    if (!above(l(delta).abs(), l(0.5f))) return;
    std::int32_t rise = shared::math::round_float_to_int(delta);
    if (rise == 0) return;
    rise = rise < -16 ? -16 : (rise > 24 ? 24 : rise);
    std::int32_t sequence = pm.ps.integer(ps::kEventSequence);
    auto slot = static_cast<std::size_t>(sequence & (pmv::kMaxPsEvents - 1));
    pm.ps.set_integer(ps::kEvents + 4 * slot, kEvStepView);
    pm.ps.set_integer(ps::kEventParms + 4 * slot, (rise + 128) & 0xFF);
    pm.ps.set_integer(ps::kEventSequence, static_cast<std::int32_t>(static_cast<std::uint32_t>(sequence) + 1u));

    // Slowed by the step's height.
    F24 climbed = l(pm.ps.real(ps::kOrigin + 8)) - l(start_o[2]);
    F24 scale = (l(1.0f) - climbed.abs() / l(step_size)) * l(0.8f) + l(std::bit_cast<float>(0x3E4CCCCCu));
    Vec w = pm.ps.velocity();
    for (float& c : w) c = (scale * l(c)).store();
    pm.ps.set_velocity(w);

    std::int32_t size = rise < 0 ? -rise : rise;
    if (size <= 3 || pm.ps.integer(ps::kGroundEntityNum) == pmv::kEntityNone) return;
    if (!pmv::should_make_footsteps(pm, settings)) return;
    std::int32_t n = std::min(size / 2, 4);
    std::int32_t old = pm.ps.integer(ps::kBobCycle);
    F24 bob = F24::integer(n) * l(1.25f) + l(7.0f) + F24::integer(old);
    std::int32_t next = static_cast<std::int32_t>(net::ftol2(bob.v)) & 0xFF;
    pm.ps.set_integer(ps::kBobCycle, next);
    pmv::footstep_event(pm, pml, old, next, true, host);
}

}  // namespace iw_pc::game::slide
