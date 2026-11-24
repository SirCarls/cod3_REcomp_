// cod3 - [acct2-ocr] 2026-11-22 (owner 04:31: Titanfall 2's grenade arc, "the grenade/arc star arc-building is one
// I'm after"; 04:40 "just do what you think is right") The grenade arc's sums: where a thrown grenade goes, flown the
// way Call of Duty 3's own server flies it, and where a point of it sits in a view. No game or guest memory here: the
// game's numbers come in as arguments (ui/hud_overlay.cpp reads them from the running game), so this is the same in
// both programs and testable on its own (dev/agents/acct2-ocr/grenade-arc/).
//
// What the game does, read from the campaign's code (the same steps in cod3mp):
//   - The throw (sub_82558E58, from the weapon's fire sub_82569C40): the grenade starts at the weapon's muzzle point
//     plus the view's forward times a constant; its velocity is the forward times the weapon's speed plus straight up
//     times its up speed; then the part of the player's own velocity along that direction is added.
//   - The flight (G_RunMissile): each server frame the grenade's place is its trajectory's at that time (gravity,
//     BG_EvaluateTrajectory), and the straight line from its last place to it is traced against the world. Here the
//     curve is traced in longer stretches (Flight::step; a stretch's chord is within a fraction of a unit of the
//     game's shorter ones), and a bounce's timing follows the game's frames (Flight::frame).
//   - A bounce (sub_82569538, on a hit): the velocity at the moment of the hit is mirrored on the surface
//     (v + n x (v.n x reflect), reflect being the game's constant) and scaled by the bounce; on ground (the normal's z
//     above a constant) with a speed under a constant it comes to rest at the hit; otherwise its new trajectory starts
//     at the hit point moved off the surface (the normal times a constant, its height at most another) at the frame's
//     end (the rest of that frame is not flown).
//   - It goes off when its fuse runs out (a cooked grenade: what was left of it).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cod3::grenade_arc {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};
inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return Add(a, Scale(Sub(b, a), t)); }
inline bool Finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// The throw, as sub_82558E58 makes it.
struct ThrowInputs {
  Vec3 muzzle;             // the weapon's muzzle point (here: the view's origin)
  Vec3 forward;            // the view's forward
  float start_push = 0;    // the game's constant: the start is muzzle + forward x this
  float speed = 0;         // the weapon's speed (its +1920 in the campaign)
  float speed_up = 0;      // the weapon's up speed (+1924)
  Vec3 player_velocity;    // its part along the throw's direction is added
};
struct Throw {
  Vec3 start, velocity;
};
inline Throw MakeThrow(const ThrowInputs& in) {
  Throw t;
  t.start = Add(in.muzzle, Scale(in.forward, in.start_push));
  Vec3 v = Scale(in.forward, in.speed);
  v.z += in.speed_up;
  const float len = Length(v);
  const Vec3 dir = len != 0.0f ? Scale(v, 1.0f / len) : v;
  t.velocity = Add(v, Scale(dir, Dot(in.player_velocity, dir)));
  return t;
}

// The flight's numbers, all the game's own (read from it, never guessed here).
struct Flight {
  float gravity = 800.0f;     // units a second a second, down
  float frame = 0.05f;        // the game's server frame, seconds: a bounce's new trajectory starts at its end
  float step = 0.05f;         // seconds of flight traced at a time
  float reflect = -2.0f;      // the bounce's mirror constant
  float bounce = 0.5f;        // the velocity's scale after a bounce
  float rest_normal_z = 0.7f; // above it the surface counts as ground
  float rest_speed = 20.0f;   // on ground, under it the grenade stops
  float nudge = 0.0f;         // after a bounce the grenade starts this far off the surface, along its normal...
  float nudge_up_most = 1e9f; // ... its height part at most this
  float fuse = 3.0f;          // seconds until it goes off
};

// A trace's answer: the share of the line flown before a hit (1: none) and the surface's normal there; start_solid
// when the line starts inside something (the flight stops there).
struct Hit {
  float fraction = 1.0f;
  Vec3 normal;
  bool start_solid = false;
};

enum PointKind : uint8_t {
  kPath = 0,    // along the flight
  kBounce = 1,  // a bounce
  kRest = 2,    // where it comes to rest (it goes off there)
  kBang = 3,    // where the fuse runs out in flight
};
struct Point {
  Vec3 p;
  uint8_t kind = kPath;
  float time = 0.0f;  // seconds after the throw
};

// Flies the throw and writes its points (at most `max_points`): the start, two points a step (the step's middle on
// the curve, drawn only; and its end, traced), each bounce, and the end (a rest or the fuse). `trace(a, b)` traces the
// straight line from a to b against the world. Stops after `bounces` bounces (the next bounce point is the last), at
// rest, at the fuse, or after 20 seconds. Returns the number of points.
template <class Trace>
int Fly(const Throw& t, const Flight& f, int bounces, Trace&& trace, Point* out, int max_points) {
  if (max_points < 2 || !Finite(t.start) || !Finite(t.velocity) || !(f.step > 0.0f)) return 0;
  int n = 0;
  auto put = [&](Vec3 p, uint8_t kind, float time) {
    if (n < max_points) out[n++] = Point{p, kind, time};
  };
  put(t.start, kPath, 0.0f);
  Vec3 base = t.start, vel = t.velocity;  // the trajectory: base + vel x s - g s^2 / 2, s from `since`
  float since = 0.0f, now = 0.0f;
  Vec3 at = t.start;
  int bounced = 0;
  const float fuse = std::clamp(f.fuse, 0.0f, 20.0f);
  auto eval = [&](float s) {
    Vec3 p = Add(base, Scale(vel, s));
    p.z -= 0.5f * f.gravity * s * s;
    return p;
  };
  while (n < max_points) {
    const float end_time = now + f.step;
    const bool bang = end_time >= fuse;
    const float to = bang ? fuse : end_time;
    const Vec3 mid = eval((now + to) * 0.5f - since);
    const Vec3 next = eval(to - since);
    const Hit h = trace(at, next);
    if (h.start_solid) return n;  // it starts inside something: no further
    if (!(h.fraction < 1.0f)) {
      put(mid, kPath, (now + to) * 0.5f);
      put(next, bang ? kBang : kPath, to);
      if (bang) return n;
      at = next;
      now = end_time;
      continue;
    }
    // A hit: where and when on the line, and the velocity then.
    const float fr = std::clamp(h.fraction, 0.0f, 1.0f);
    const Vec3 hit = Lerp(at, next, fr);
    const float hit_time = now + (to - now) * fr;
    if (hit_time >= fuse) {  // it goes off before the hit
      put(next, kBang, fuse);
      return n;
    }
    if (fr > 0.5f) put(mid, kPath, (now + to) * 0.5f);
    Vec3 v = vel;
    v.z -= f.gravity * (hit_time - since);
    const float d = Dot(v, h.normal);
    v = Add(v, Scale(h.normal, d * f.reflect));
    v = Scale(v, f.bounce);
    if (h.normal.z > f.rest_normal_z && Length(v) < f.rest_speed) {
      put(hit, kRest, hit_time);
      return n;
    }
    put(hit, kBounce, hit_time);
    if (++bounced > bounces) return n;
    Vec3 off = Scale(h.normal, f.nudge);
    off.z = std::min(off.z, f.nudge_up_most);
    base = Add(hit, off);
    vel = v;
    at = base;
    // the game starts the new trajectory at the end of the frame the hit fell in (its frames count from the throw)
    now = f.frame > 0.0f ? std::max(hit_time, std::ceil(hit_time / f.frame - 1e-4f) * f.frame) : end_time;
    since = now;
    if (now >= 20.0f) return n;
  }
  return n;
}

// The points of a flight up to `fuse` seconds after the throw (a cooked grenade's fuse left): those before it, then
// its place at that time (a kBang) when the flight went on past it. Returns the count written to `out`.
inline int CutAtFuse(const Point* in, int n, float fuse, Point* out, int max_points) {
  int k = 0;
  for (int i = 0; i < n && k < max_points; ++i) {
    if (in[i].time <= fuse) {
      out[k++] = in[i];
      continue;
    }
    if (i > 0) {
      const float span = in[i].time - in[i - 1].time;
      const float u = span > 0.0f ? (fuse - in[i - 1].time) / span : 1.0f;
      out[k++] = Point{Lerp(in[i - 1].p, in[i].p, std::clamp(u, 0.0f, 1.0f)), kBang, fuse};
    }
    break;
  }
  return k;
}

// A view: its origin, its forward / left / up axes, and tan(half its field of view) across and down.
struct Camera {
  Vec3 origin, forward, left, up;
  float tan_x = 1.0f, tan_y = 0.75f;
};
constexpr float kNear = 4.0f;  // points nearer than this in front of the view aren't drawn

// A point's place in the view: -1 at the left / top edge, 1 at the right / bottom. False behind (or too near).
inline bool Project(const Camera& c, Vec3 p, float* nx, float* ny) {
  const Vec3 d = Sub(p, c.origin);
  const float z = Dot(d, c.forward);
  if (!(z >= kNear) || !(c.tan_x > 0.0f) || !(c.tan_y > 0.0f)) return false;
  *nx = -Dot(d, c.left) / (z * c.tan_x);
  *ny = -Dot(d, c.up) / (z * c.tan_y);
  return std::isfinite(*nx) && std::isfinite(*ny);
}

// The piece of the line a..b in front of the view (both ends at kNear or more), or false.
inline bool ClipNear(const Camera& c, Vec3* a, Vec3* b) {
  const float za = Dot(Sub(*a, c.origin), c.forward), zb = Dot(Sub(*b, c.origin), c.forward);
  if (za < kNear && zb < kNear) return false;
  if (za < kNear) *a = Lerp(*a, *b, (kNear - za) / (zb - za));
  else if (zb < kNear) *b = Lerp(*a, *b, (kNear - za) / (zb - za));
  return true;
}

// tan(half the field of view) from a refdef value that may be the tangent or the angle in degrees.
inline float TanHalf(float v) {
  if (!std::isfinite(v) || v <= 0.0f) return 0.0f;
  if (v > 3.0f) return v < 179.0f ? std::tan(v * 0.5f * 3.14159265f / 180.0f) : 0.0f;  // degrees
  return v;
}

}  // namespace cod3::grenade_arc
