#include "iw_pc/shared/math.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <span>

#include "iw_pc/x87.h"

namespace iw_pc::shared::math {

using x87::F24;

namespace {

// The root (stored as a float) and the components scaled by its stored inverse.
float normalize_by(std::span<float> v, float squared) {
    float length = F24::load(squared).sqrt().store();
    if (length != 0.0f) {  // (fucompp with 0: equal skips; unordered divides)
        float inverse = (F24::load(1.0f) / F24::load(length)).store();
        for (float& c : v) c = (F24::load(inverse) * F24::load(c)).store();
    }
    return length;
}

}  // namespace

float vec3_normalize(std::array<float, 3>& v) {
    F24 x = F24::load(v[0]), y = F24::load(v[1]), z = F24::load(v[2]);
    return normalize_by(v, (x * x + y * y + z * z).store());
}

float vec2_normalize(std::array<float, 2>& v) {
    F24 x = F24::load(v[0]), y = F24::load(v[1]);
    return normalize_by(v, (x * x + y * y).store());
}

float vec3_normalize_to(const std::array<float, 3>& v, std::array<float, 3>& out) {
    F24 x = F24::load(v[0]), y = F24::load(v[1]), z = F24::load(v[2]);
    float length = F24::load((x * x + y * y + z * z).store()).sqrt().store();
    if (length != 0.0f) {
        float inverse = (F24::load(1.0f) / F24::load(length)).store();
        for (std::size_t i = 0; i < 3; ++i) out[i] = (F24::load(inverse) * F24::load(v[i])).store();
    } else {
        out = {0.0f, 0.0f, 0.0f};
    }
    return length;
}

namespace {

// The C runtime's atan2 on two floats (see the Rust version): x87 NaN choice, both infinite the
// default NaN, else double precision, stored.
float atan2_stored(float yf, float xf) {
    double y = yf, x = xf, nan;
    if (x87::detail::propagate(y, x, nan)) return static_cast<float>(nan);
    if (std::isinf(x) && std::isinf(y)) return std::bit_cast<float>(std::uint32_t{0xFFC00000});
    return static_cast<float>(std::atan2(y, x));
}

float degrees(float radians, float k) {
    float d = (F24::load(radians) * F24::load(k) / F24::load(3.14159265358979f)).store();
    return d < 0.0f ? (F24::load(d) + F24::load(360.0f)).store() : d;
}

}  // namespace

float vectoyaw(const std::array<float, 3>& v) {
    if (v[1] == 0.0f && v[0] == 0.0f) return 0.0f;
    return degrees(atan2_stored(v[1], v[0]), 180.0f);
}

void vec_to_angles(const std::array<float, 3>& v, std::array<float, 3>& angles) {
    if (v[1] == 0.0f && v[0] == 0.0f) {
        angles = {v[2] > 0.0f ? 270.0f : 90.0f, 0.0f, 0.0f};
        return;
    }
    float yaw = degrees(atan2_stored(v[1], v[0]), 180.0f);
    F24 x = F24::load(v[0]), y = F24::load(v[1]);
    float forward = F24::load((y * y + x * x).store()).sqrt().store();
    float pitch = degrees(atan2_stored(v[2], forward), -180.0f);
    angles = {pitch, yaw, 0.0f};
}

float angle_normalize180(float angle) {
    double r = (F24::load(angle) * F24::load(182.04445f)).v;
    // __ftol2: truncated to 64 bits (NaN and out of range: the integer indefinite).
    std::int64_t t = (std::isnan(r) || r >= 9223372036854775808.0 || r < -9223372036854775808.0)
                         ? INT64_MIN : static_cast<std::int64_t>(r);
    std::int32_t turn = static_cast<std::int32_t>(static_cast<std::uint32_t>(t)) & 0xFFFF;
    float a = (F24::load(F24::integer(turn).store()) * F24::load(0.0054931640625f)).store();
    return a > 180.0f ? (F24::load(a) - F24::load(360.0f)).store() : a;
}

namespace {
/// AngleVectors' sine and cosine of degrees: a * 0.017453292 stored, then fsincos (double precision in
/// the checker's emulator and here), each stored.
void sin_cos_degrees(float a, float& s, float& c) {
    double radians = static_cast<double>((F24::load(a) * F24::load(0.017453292f)).store());
    s = static_cast<float>(std::sin(radians));
    c = static_cast<float>(std::cos(radians));
}
}  // namespace

void angle_vectors(const std::array<float, 3>& angles, std::array<float, 3>* forward, std::array<float, 3>* right,
                   std::array<float, 3>* up) {
    float sy, cy, sp, cp;
    sin_cos_degrees(angles[1], sy, cy);
    sin_cos_degrees(angles[0], sp, cp);
    auto l = F24::load;
    if (forward) *forward = {(l(cp) * l(cy)).store(), (l(cp) * l(sy)).store(), (-l(sp)).store()};
    if (!right && !up) return;
    float sr, cr;
    sin_cos_degrees(angles[2], sr, cr);
    if (right) {
        float t = (l(sr) * l(sp)).store();
        *right = {(l(cr) * l(sy) - l(t) * l(cy)).store(), (l(cr) * l(cy) * l(-1.0f) - l(t) * l(sy)).store(),
                  (l(sr) * l(cp) * l(-1.0f)).store()};
    }
    if (up) {
        float t = (l(cr) * l(sp)).store();
        *up = {(l(t) * l(cy) + l(sr) * l(sy)).store(), (l(t) * l(sy) - l(sr) * l(cy)).store(), (l(cr) * l(cp)).store()};
    }
}

void snap_vector(std::array<float, 3>& v) {
    for (float& c : v) c = x87::F24::integer(x87::F24::load(c).round_to_int()).store();
}

std::array<float, 3> vec3_lerp(const std::array<float, 3>& from, const std::array<float, 3>& to, float fraction) {
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i)
        out[i] = ((F24::load(to[i]) - F24::load(from[i])) * F24::load(fraction) + F24::load(from[i])).store();
    return out;
}

float vectopitch(const std::array<float, 3>& v) {
    std::array<float, 3> angles{};
    vec_to_angles(v, angles);
    return angles[0];
}

namespace {
/// a by whole turns while `more(a)` (+360 or -360 by `step`), each step stored; no progress ends it.
template <class More>
float turn_while(float a, float step, More more) {
    while (more(a)) {
        float next = (F24::load(a) + F24::load(step)).store();
        if (next == a) return a;  // (the original never returns here)
        a = next;
    }
    return a;
}
}  // namespace

float angle_wrap360(float angle) {
    if (angle < 0.0f) return turn_while(angle, 360.0f, [](float a) { return a < 0.0f; });  // (only below)
    return turn_while(angle, -360.0f, [](float a) { return a >= 360.0f; });  // (at or above; NaN doesn't)
}

float angle_wrap180(float angle) {
    if (angle <= -180.0f) return turn_while(angle, 360.0f, [](float a) { return a <= -180.0f; });
    return turn_while(angle, -360.0f, [](float a) { return a > 180.0f; });
}

float angle_delta(float a1, float a2) { return angle_normalize180((F24::load(a1) - F24::load(a2)).store()); }

void add_lean_to_position(std::array<float, 3>& position, float yaw, float lean, float roll, float dist) {
    auto l = F24::load;
    if (lean == 0.0f) return;  // (only 0 skips; NaN leans)
    float ease = ((l(2.0f) - l(l(lean).abs().store())) * l(lean)).store();
    float rolled = (l(ease) * l(roll)).store();
    std::array<float, 3> right{};
    angle_vectors({0.0f, yaw, rolled}, nullptr, &right, nullptr);
    float d = (l(ease) * l(dist)).store();
    for (std::size_t i = 0; i < 3; ++i) position[i] = (l(right[i]) * l(d) + l(position[i])).store();
}

float pitch_for_yaw_on_normal(float yaw, const std::array<float, 3>& normal) {
    auto l = F24::load;
    float s, c;
    sin_cos_degrees(yaw, s, c);
    F24 n0 = l(normal[0]), n1 = l(normal[1]), n2 = l(normal[2]);
    float d = (n2 * l(0.0f) - l(c) * n0 - l(s) * n1).store();
    std::array<float, 3> along{(l(d) * n0 + l(c)).store(), (l(d) * n1 + l(s)).store(), (l(d) * n2 + l(0.0f)).store()};
    return vectopitch(along);
}

float angle_subtract(float a1, float a2) {
    float a = (F24::load(a1) - F24::load(a2)).store();
    while (a > 180.0f) {  // (only above turns back; NaN doesn't)
        float next = (F24::load(a) - F24::load(360.0f)).store();
        if (next == a) return a;  // (the original never returns here)
        a = next;
    }
    while (a < -180.0f) {
        float next = (F24::load(a) + F24::load(360.0f)).store();
        if (next == a) return a;
        a = next;
    }
    return a;
}

std::int32_t round_float_to_int(float x) { return (F24::load(x) + F24::dbl(9.313225746154785e-10)).round_to_int(); }

float diff_track(float tgt, float cur, float rate, float delta_time) {
    auto l = F24::load;
    float d = (l(tgt) - l(cur)).store();
    float step = (l(d) * l(rate) * l(delta_time)).store();
    float size = l(d).abs().store();
    if (l(size).v > l(0.001f).v) {  // (only above moves; NaN doesn't)
        float step_size = l(step).abs().store();
        if (!(l(size).v < l(step_size).v)) return (l(step) + l(cur)).store();  // (only below stops; NaN moves)
    }
    return tgt;
}

float i_fclamp(float val, float min, float max) {
    // (fsub, fcomp 0, test ah,1: below 0, or NaN.)
    auto below = [](float a, float b) { return !((F24::load(a) - F24::load(b)).v >= 0.0); };
    float upper = below(val, max) ? val : max;
    return below(min, val) ? upper : min;
}

}  // namespace iw_pc::shared::math
