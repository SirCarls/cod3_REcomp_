// cod3 - mantling (see mantle.h).

#include "game/movement/mantle.h"

#include <cmath>

#include "game/movement/program.h"
#include "game/movement/rules.h"

namespace cod3::movement {

namespace {
constexpr int kLookEveryMs = 100;
constexpr int kLongestClimbMs = 2000;  // a safety net; a climb takes 550 ms
}  // namespace

Mantles::Climb* Mantles::For(int client) {
  if (client < 0 || client >= kMaxClients) return nullptr;
  return &climbs_[static_cast<size_t>(client)];
}

bool Mantles::Active(int client) const {
  return client >= 0 && client < kMaxClients && climbs_[static_cast<size_t>(client)].active;
}

void Mantles::Cancel(int client) {
  if (Climb* c = For(client)) c->active = false;
}

void Mantles::CancelAll() {
  for (Climb& c : climbs_) c = Climb{};
}

bool Mantles::TryStart(const MantleInput& in, Tracer& tracer) {
  why_not_ = "";
  last_height_ = 0.0f;
  last_wall_ = TraceHit{};
  Climb* climb = For(in.client);
  if (!climb) return No("not a player's client number");
  if (climb->active) return false;
  // A new level restarts serverTime.
  if (in.server_time < climb->last_look) climb->last_look = -100000;
  if (in.server_time - climb->last_look < kLookEveryMs) return No("looked less than 100 ms ago");
  climb->last_look = in.server_time;
  // Standing, moving normally: not prone or crouched, on a ladder, using a
  // gun or a vehicle, dead or in noclip.
  if (in.pm_type != kPmNormal) return No("not moving normally");
  if (in.pm_flags & (pmf::kProne | pmf::kDucked | pmf::kLadder)) return No("crouched, prone or on a ladder");
  if (in.flags2 & kFlags2NoMove) return No("on a gun or in a vehicle");

  const float* o = in.origin;
  // 1. A wall ahead.
  TraceHit wall;
  const float ahead[3] = {o[0] + in.forward[0] * cod4::kMantleCheckRange,
                          o[1] + in.forward[1] * cod4::kMantleCheckRange, o[2]};
  if (!tracer.Trace(o, ahead, &wall)) return No("the trace failed");
  last_wall_ = wall;
  if (wall.startsolid || wall.allsolid) return No("the player's box starts in solid");
  if (wall.fraction >= 1.0f) return No("no wall ahead");
  if (!mantle::FacingWall(in.forward, wall.normal)) return No("not facing a wall");

  // 2. A walkable top behind it, the standing box fitting on it. The box is
  // moved past the wall's face by its own size, so it stands wholly on top.
  float into[2] = {-wall.normal[0], -wall.normal[1]};
  const float len = std::sqrt(into[0] * into[0] + into[1] * into[1]);
  into[0] /= len;
  into[1] /= len;
  const float reach = in.half_width * 2.0f + 1.0f;
  const float probe_x = wall.endpos[0] + into[0] * reach;
  const float probe_y = wall.endpos[1] + into[1] * reach;
  TraceHit top;
  const float above[3] = {probe_x, probe_y, o[2] + mantle::kMaxHeight + 1.0f};
  const float below[3] = {probe_x, probe_y, o[2] + mantle::kMinHeight};
  if (!tracer.Trace(above, below, &top)) return No("no top");
  if (top.startsolid || top.allsolid) return No("the wall is higher than chest height");
  if (top.fraction >= 1.0f) return No("the top is lower than the jump step's reach");
  if (top.normal[2] < kWalkableNormalZ) return No("the top isn't walkable");
  const float height = top.endpos[2] - o[2];
  last_height_ = height;
  if (!mantle::HeightOk(height)) return No("the top's height is out of range");

  // 3. A clear path: up, then across.
  const float top_z = top.endpos[2] + 1.0f;
  TraceHit up;
  const float raised[3] = {o[0], o[1], top_z};
  if (!tracer.Trace(o, raised, &up) || up.startsolid || up.fraction < 1.0f) return No("no room above");
  TraceHit across;
  const float onto[3] = {probe_x, probe_y, top_z};
  if (!tracer.Trace(raised, onto, &across) || across.startsolid || across.fraction < 1.0f)
    return No("no room on the top");

  climb->active = true;
  climb->start_time = in.server_time;
  for (int i = 0; i < 3; ++i) climb->start[i] = o[i];
  climb->top_z = top_z;
  climb->end[0] = probe_x;
  climb->end[1] = probe_y;
  climb->end[2] = top.endpos[2];
  return true;
}

bool Mantles::Step(const MantleInput& in, float origin[3]) {
  Climb* climb = For(in.client);
  if (!climb || !climb->active) return false;
  const int elapsed = in.server_time - climb->start_time;
  // Noclip, death, a gun or a vehicle, a new level: the climb is over.
  if (in.pm_type != kPmNormal || (in.flags2 & kFlags2NoMove) || elapsed < 0 || elapsed > kLongestClimbMs) {
    climb->active = false;
    return false;
  }
  if (!mantle::Position(climb->start, climb->top_z, climb->end, elapsed, origin)) climb->active = false;
  return true;
}

}  // namespace cod3::movement
