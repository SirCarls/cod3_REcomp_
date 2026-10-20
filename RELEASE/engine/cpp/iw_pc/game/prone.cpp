#include "iw_pc/game/prone.h"

#include "iw_pc/shared/math.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::prone {

using x87::F24;
using Vec = std::array<float, 3>;
namespace m = iw_pc::shared::math;

namespace {
bool above(F24 a, F24 b) { return a.v > b.v; }
bool below(F24 a, F24 b) { return a.v < b.v; }
}  // namespace

bool check_prone(const ProneQuery& q, ProneAngles out, trace::MoveHost& host) {
    auto l = F24::load;
    std::int32_t mask = q.check_type != 0 ? 0x00820011 : 0x00810011;  // (0x810011 + 0x10000: an addition)
    auto trace = [&](const Vec& start, const Vec& mins, const Vec& maxs, const Vec& end) {
        return host.trace(q.handler, start, mins, maxs, end, q.pass_entity, mask);
    };
    const Vec v = q.origin;

    if (!q.already_prone) {
        Vec mins{(-l(q.size)).store(), (-l(q.size)).store(), 0.0f};
        Vec maxs{q.size, q.size, q.height};
        Vec end{v[0], v[1], (l(v[2]) + l(10.0f)).store()};
        if (trace(v, mins, maxs, end).all_solid()) return false;
    }
    if (q.on_ground && q.ground_normal && below(l((*q.ground_normal)[2]), l(0.7f))) return false;

    Vec angles{0.0f, (l(q.yaw) - l(180.0f)).store(), 0.0f};
    Vec forward{}, right{}, up{};
    m::angle_vectors(angles, &forward, &right, &up);
    const Vec small_mins{-6.0f, -6.0f, -6.0f}, small_maxs{6.0f, 6.0f, 6.0f};
    float mid = (l(q.height) - l(6.0f)).store();
    float reach = (l(q.feet_distance) - l(6.0f)).store();

    // The feet: behind, from the middle's height.
    Vec start{v[0], v[1], (l(mid) + l(v[2])).store()};
    Vec end{};
    for (std::size_t i = 0; i < 3; ++i) end[i] = (l(forward[i]) * l(reach) + l(start[i])).store();
    trace::Trace tr = trace(start, small_mins, small_maxs, end);
    bool feet_hit = false;
    float feet_dist;
    if (below(l(tr.fraction()), l(1.0f))) {
        if (!q.on_ground) return false;
        feet_hit = true;
        float d = (l(reach) * l(tr.fraction()) + l(6.0f)).store();
        if (above(l(q.size) + l(2.0f), l(d))) return false;
        float limit = (l(mid) * l(0.7f) + l(48.0f)).store();
        if (below(l(d), l(limit))) {  // too close: try 22 higher
            end[2] = (l(end[2]) + l(22.0f)).store();
            feet_hit = false;
            Vec dir{(l(end[0]) - l(start[0])).store(), (l(end[1]) - l(start[1])).store(), (l(end[2]) - l(start[2])).store()};
            float length = m::vec3_normalize_to(dir, forward);
            tr = trace(start, small_mins, small_maxs, end);
            if (below(l(tr.fraction()), l(1.0f))) {
                feet_hit = true;
                F24 d2 = l(length) * l(tr.fraction()) + l(6.0f);
                if (below(d2, l(limit))) return false;
                feet_dist = d2.store();
            } else {
                feet_dist = q.feet_distance;
            }
        } else {
            feet_dist = d;
        }
    } else {
        feet_dist = q.feet_distance;
    }
    Vec feet = m::vec3_lerp(start, end, tr.fraction());

    // The torso: in front, traced down.
    Vec tstart{};
    for (std::size_t i = 0; i < 3; ++i) tstart[i] = (l(forward[i]) * l(48.0f) + l(v[i])).store();
    tstart[2] = (l(forward[2]) * l(48.0f) + l(v[2]) + l(mid)).store();
    float drop = (l(q.size) * l(2.5f) + l(mid) - l(6.0f)).store();
    Vec tend{tstart[0], tstart[1], (l(tstart[2]) - l(drop)).store()};
    tr = trace(tstart, small_mins, small_maxs, tend);
    auto fail = [&]() {
        if (q.on_ground) return false;
        for (float* o : {out.torso_height, out.torso_pitch, out.waist_pitch})
            if (o) *o = 0.0f;
        return true;
    };
    if (tr.fraction() == 1.0f) return fail();
    if (below(l(tr.normal()[2]), l(0.7f))) return false;
    F24 torso_dist = l(drop) * l(tr.fraction()) + l(6.0f);
    Vec torso{};
    for (std::size_t i = 0; i < 2; ++i) torso[i] = ((l(tend[i]) - l(tstart[i])) * l(tr.fraction()) + l(tstart[i])).store();
    torso[2] = ((l(tend[2]) - l(tstart[2])) * l(tr.fraction()) + l(tstart[2]) - l(6.0f)).store();

    if (feet_hit) {
        if (above(torso_dist * l(-0.75f), l(feet_dist) - torso_dist)) return fail();  // the torso too far below
        F24 a0 = l(feet[0]) - l(torso[0]), a1 = l(feet[1]) - l(torso[1]);
        float dz = (l(feet[2]) - l(torso[2])).store();
        Vec w{(l(forward[0]) * l(6.0f) + a0).store(), (l(forward[1]) * l(6.0f) + a1).store(),
              (l(forward[2]) * l(6.0f) + l(dz) + l(6.0f)).store()};
        m::vec3_normalize(w);
        F24 k = l(reach) - l(48.0f);
        float t1 = (l(w[1]) * k + l(tstart[1])).store();
        F24 t0 = l(w[0]) * k + l(tstart[0]);
        Vec e{};
        e[2] = (k * l(w[2]) + l(tstart[2])).store();
        e[0] = ((l(reach) * l(forward[0]) + l(v[0]) + t0) * l(0.5f)).store();
        e[1] = ((l(reach) * l(forward[1]) + l(v[1]) + l(t1)) * l(0.5f)).store();
        Vec s = tstart;
        trace::Trace t2 = trace(s, small_mins, small_maxs, e);
        if (below(l(t2.fraction()), l(1.0f))) {
            s = m::vec3_lerp(s, e, t2.fraction());
            s[2] = (l(s[2]) + l(18.0f)).store();
            e[2] = (l(e[2]) + l(18.0f)).store();
            t2 = trace(s, small_mins, small_maxs, e);
            if (below(l(t2.fraction()), l(1.0f))) return fail();
        }
        feet = m::vec3_lerp(s, e, t2.fraction());
    }

    // The ground under the feet.
    Vec gstart = feet;
    Vec gend{feet[0], feet[1], (l(feet[2]) - ((l(feet[2]) - l(torso[2])) * l(2.0f) + l(q.size))).store()};
    tr = trace(gstart, small_mins, small_maxs, gend);
    if (tr.fraction() == 1.0f) return fail();
    if (below(l(tr.normal()[2]), l(0.7f))) return false;
    feet = m::vec3_lerp(gstart, gend, tr.fraction());
    feet[2] = (l(feet[2]) - l(6.0f)).store();

    auto sub = [&](const Vec& a, const Vec& b) {
        return Vec{(l(a[0]) - l(b[0])).store(), (l(a[1]) - l(b[1])).store(), (l(a[2]) - l(b[2])).store()};
    };
    bool ok = true;
    float up_pitch = m::vectopitch(sub(torso, v));
    float bend = m::angle_subtract(m::vectopitch(sub(feet, torso)), up_pitch);
    if (below(l(bend), l(-50.0f)) || above(l(bend), l(70.0f))) ok = false;  // (NaN passes)
    const Vec zero{0.0f, 0.0f, 0.0f};
    for (auto [from, to] : {std::pair{v, torso}, std::pair{torso, feet}}) {
        Vec s{from[0], from[1], (l(from[2]) + l(5.0f)).store()};
        Vec e{to[0], to[1], (l(to[2]) + l(5.0f)).store()};
        if (below(l(trace(s, zero, zero, e).fraction()), l(1.0f))) ok = false;
    }

    if (out.torso_height) *out.torso_height = 0.0f;
    if (out.torso_pitch) *out.torso_pitch = m::angle_normalize180(m::vectopitch(sub(v, torso)));
    if (out.waist_pitch) *out.waist_pitch = m::angle_normalize180(m::vectopitch(sub(torso, feet)));
    if (ok) return true;
    if (q.on_ground) return false;
    for (float* o : {out.torso_pitch, out.waist_pitch})
        if (o) *o = 0.0f;
    return true;
}

bool verify_prone_position(pmove::Pmove& pm, const Vec& start_origin, const Vec& start_velocity, trace::MoveHost& host) {
    namespace ps = pmove::ps;
    if ((pm.ps.pm_flags() & pmove::pm_flags::kProne) == 0) return true;
    ProneQuery q;
    q.pass_entity = pm.ps.integer(ps::kClientNum);
    q.origin = pm.ps.vec3(ps::kOrigin);
    q.size = pm.ps.real(ps::kProneCheckSize);
    q.height = 30.0f;
    q.yaw = pm.ps.real(ps::kProneDirection);
    q.already_prone = true;
    q.on_ground = true;
    q.handler = pm.bytes[pmove::pm::kHandler];
    q.check_type = 0;
    q.feet_distance = 66.0f;
    float h = pm.ps.real(ps::kProneTorsoHeight), t = pm.ps.real(ps::kProneTorsoPitch), w = pm.ps.real(ps::kProneWaistPitch);
    bool fits = check_prone(q, ProneAngles{&h, &t, &w}, host);
    pm.ps.set_real(ps::kProneTorsoHeight, h);
    pm.ps.set_real(ps::kProneTorsoPitch, t);
    pm.ps.set_real(ps::kProneWaistPitch, w);
    if (!fits) {
        pm.ps.set_vec3(ps::kOrigin, start_origin);
        pm.ps.set_velocity(start_velocity);
    }
    return fits;
}

}  // namespace iw_pc::game::prone
