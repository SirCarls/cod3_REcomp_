// cod3 - mantling: climbing onto a ledge up to about chest height (part 3
// of the CoD4 style).
//
// Call of Duty 3 has no mantle (its "vault" is Call of Duty 2's jump step,
// which lifts a jumping player onto anything up to the jump's height), and
// its maps carry no mantle surfaces, so ledges are found with the player's
// own traces (the game's PM_PlayerTrace with the player's box and clip mask,
// playerclip included), conservatively:
//   1. a wall within mantle_check_range (20) ahead, nearly vertical, facing
//      the player within mantle_check_angle (60 degrees);
//   2. a walkable top behind it, between 36 and 60 units above the feet (the
//      jump step already climbs anything lower), with room for the standing
//      box on it;
//   3. a clear path: straight up to that height, then forward onto the top.
// The climb then takes 0.35 s up and 0.2 s forward, timed by the usercmds'
// serverTime (the game's clock: the same at any frame rate), with the
// velocity at zero. It starts when the player presses jump facing such a
// ledge, standing on the ground or rising in a jump with jump held.
//
// Plain logic: the traces go through the Tracer the glue passes in.

#pragma once

#include <array>
#include <cstdint>

namespace cod3::movement {

struct TraceHit {
  float fraction = 1.0f;
  float endpos[3] = {};
  float normal[3] = {};
  bool startsolid = false;
  bool allsolid = false;
};

// The game's player trace (the player's box, clip mask and entity).
class Tracer {
 public:
  virtual bool Trace(const float start[3], const float end[3], TraceHit* out) = 0;

 protected:
  ~Tracer() = default;
};

// What the climb needs to know about the player this step.
struct MantleInput {
  int client = 0;
  int server_time = 0;
  float origin[3] = {};
  float forward[2] = {};   // the view's horizontal direction (unit)
  float half_width = 15.0f;  // the box's half size across
  int pm_type = 0;
  uint32_t pm_flags = 0;
  uint32_t flags2 = 0;
  bool on_ground = false;
};

class Mantles {
 public:
  static constexpr int kMaxClients = 64;

  // Looks for a ledge and starts climbing it. True when it started (the
  // jump is then not made). Throttled to one look per 100 ms per player.
  bool TryStart(const MantleInput& in, Tracer& tracer);
  // While climbing: where the player is this step (`origin` is written).
  // Returns false when not climbing (anymore); the step that ends the climb
  // returns true with the end position.
  bool Step(const MantleInput& in, float origin[3]);
  bool Active(int client) const;
  void Cancel(int client);
  void CancelAll();
  // Why the last TryStart didn't climb ("" when it did, or didn't look), and
  // the ledge's height when one was found: for the debug log only.
  const char* why_not() const { return why_not_; }
  float last_height() const { return last_height_; }
  const TraceHit& last_wall() const { return last_wall_; }  // the forward check's trace

 private:
  struct Climb {
    bool active = false;
    int start_time = 0;
    float start[3] = {};
    float top_z = 0.0f;
    float end[3] = {};
    int last_look = -100000;
  };
  Climb* For(int client);
  bool No(const char* why) {
    why_not_ = why;
    return false;
  }
  std::array<Climb, kMaxClients> climbs_{};
  const char* why_not_ = "";
  float last_height_ = 0.0f;
  TraceHit last_wall_;
};

}  // namespace cod3::movement
