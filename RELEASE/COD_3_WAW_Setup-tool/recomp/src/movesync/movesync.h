// movesync - every player's own movement, without desync, in every game of the project.
// [acct2-decomp] 2026-11-07, owner: "make a robust system where every player could potentially have different
// movements without desync. This will be applied to all games." The design is docs/MOVEMENT_PROFILES.md.
//
// Plain logic: no game address, no guest memory, no setting, no network call. Each game's adapter (the recomp's
// movement hooks for cod3 and cod3mp, a dedicated server, the other games' C++ ports) moves players and carries
// the bytes made here; this file decides *which* movement each player has, makes every machine agree on it, and
// checks what others send. Linux and Windows builds must decide alike, bit for bit: the test
// (dev/agents/acct2-decomp/tests/test_movesync.cpp) checks both against the same vectors (owner 2026-11-07:
// "no rust engine. it's C++").
//
// Arithmetic: f32 only, every operation written out in a fixed order, no fused multiply-add (build this file
// with -ffp-contract=off; MSVC's /fp:precise does not contract). sqrt is IEEE's correctly rounded one.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace movesync {

// The version of everything below: profiles, policies, records and the digest. Machines that announce
// another number (or nothing) are treated as stock-only (see Capabilities).
inline constexpr uint16_t kProtocol = 1;

using Bytes = std::vector<uint8_t>;

// --- bytes and text armor ------------------------------------------------------------------------------------
// FNV-1a, 64 bits (public domain; docs/SOURCES.md). Profile ids and the agreement digest.
uint64_t Fnv1a64(const uint8_t* data, size_t size, uint64_t hash = 0xcbf29ce484222325ull);
// RFC 4648 section 5 (URL-safe), no padding. Decoding is strict: only the alphabet, no padding, no
// leftover bits, at most `max_bytes` out.
std::string ToBase64Url(const Bytes& bytes);
bool FromBase64Url(std::string_view text, size_t max_bytes, Bytes* out);
std::string ToHex(const Bytes& bytes);  // lower case; logs and test vectors

// --- profiles ------------------------------------------------------------------------------------------------
// A movement profile is the complete description of how one player moves. Stock and Cod4 run the game's own
// code (and the existing CoD4 parts); the other styles run the velocity kernel below inside the game's own
// collision code. A profile is identified by its exact bytes (Encode), so two machines with the same id run
// the same numbers.
enum class Style : uint8_t {
  kStock = 0,       // the game's own movement, unchanged (console CoD3, PC CoD2 ...)
  kCod4 = 1,        // the recomp's CoD4 style (game/movement/rules.h Parts)
  kQuakeWorld = 2,  // QuakeWorld / Quake 1
  kQuake3 = 3,      // Quake III Arena
  kSource = 4,      // Source / Counter-Strike
};
inline constexpr int kStyleCount = 5;
// Bit (1 << style) in Capabilities::styles.
inline constexpr uint16_t StyleBit(Style s) { return static_cast<uint16_t>(1u << static_cast<unsigned>(s)); }

namespace flag {
inline constexpr uint16_t kSprint = 1u << 0;        // the player may sprint (the campaign's cod3_sprint)
inline constexpr uint16_t kCod4Sprint = 1u << 1;    // CoD4 parts (style kCod4 only)
inline constexpr uint16_t kCod4Speeds = 1u << 2;
inline constexpr uint16_t kCod4Jumps = 1u << 3;
inline constexpr uint16_t kAutoBhop = 1u << 4;      // jump held re-jumps on landing (kernel styles)
inline constexpr uint16_t kSnapVelocity = 1u << 5;  // round the velocity to whole units after each step (Q3)
inline constexpr uint16_t kNoFallDamage = 1u << 6;
inline constexpr uint16_t kJumpAdds = 1u << 7;      // jump adds to the vertical speed (QuakeWorld) instead of setting it
inline constexpr uint16_t kKnown = (1u << 8) - 1;   // any other bit set: refused
}  // namespace flag

inline constexpr uint16_t kGameJumpCooldown = 0xFFFF;  // jump_cooldown_ms: the game's own rule

struct Profile {
  Style style = Style::kStock;
  uint16_t flags = 0;
  // Kernel styles only; 0 for Stock and Cod4 (their numbers are the game's). Units per second unless noted.
  float max_speed = 0;       // the wish speed's ceiling
  float accelerate = 0;      // ground
  float air_accelerate = 0;
  float air_wish_cap = 0;    // the air wish speed's cap (QuakeWorld / Source 30); 0 = none
  float friction = 0;
  float stop_speed = 0;
  float edge_friction = 0;   // the friction's multiplier next to a drop (QuakeWorld 2); 1 = none
  float gravity = 0;
  float jump_speed = 0;
  float step_size = 0;       // the step-up height in units; 0 = the game's
  float crouch_scale = 0;    // the wish speed's scale while crouched; 0 = the game's
  float max_velocity = 0;    // horizontal speed ceiling, required for kernel styles (100-4000); the checker's bound
  uint16_t move_step_ms = 0;               // fixed movement steps; 0 = one step per user command
  uint16_t jump_cooldown_ms = kGameJumpCooldown;
};

inline constexpr size_t kProfileBytes = 56;
inline constexpr uint64_t kNoProfile = 0;

// nullptr when `p` is acceptable, else why not (a fixed string). Every number finite and in its range; Stock
// and Cod4 have all numbers 0 and only their own flags; unknown flags refused.
const char* Validate(const Profile& p);
// The canonical bytes (format 1, little-endian, -0.0 written as 0.0). Only for a valid profile.
Bytes Encode(const Profile& p);
// Strict: exactly kProfileBytes, format 1, valid, canonical (re-encoding gives the same bytes).
bool Decode(const uint8_t* data, size_t size, Profile* out);
// FNV-1a of the canonical bytes; never kNoProfile.
uint64_t IdOf(const Profile& p);

// Presets. Numbers: QuakeWorld and Quake III from their public defaults, Source from Counter-Strike's
// documented defaults (docs/MOVEMENT_PROFILES.md 3 gives each source).
Profile Stock(bool sprint);
Profile Cod4(bool sprint, bool speeds, bool jumps);
Profile QuakeWorld();
Profile Quake3();
Profile Source();
uint64_t StockId();  // IdOf(Stock(false)): the movement every game falls back to

// The settings' text form. Grammar (strict, at most 200 characters, no spaces):
//   name [ "," key "=" value ]*
//   name: stock | stock+sprint | cod4 | cod4:abcd | quakeworld | qw | quake3 | q3 | source | cs | 3 | 4abcd
//         (abcd: 0/1 digits for sprint, speeds, an unused part, jumps: the CoD4 code rules.h already sends)
//   keys (kernel styles only): maxspeed accel airaccel aircap friction stopspeed edgefriction gravity jumpspeed
//         stepsize crouchscale maxvelocity (decimal numbers, at most 12 characters), stepms jumpcooldown
//         (whole numbers; jumpcooldown "game" = the game's), autobhop snap nofalldamage jumpadds sprint (0/1)
// `why` gets a short reason on failure (never the input text).
bool ParseProfile(std::string_view text, Profile* out, std::string* why);
// The shortest text that parses back to the same profile (the preset's name plus the keys that differ).
std::string Describe(const Profile& p);

// --- what a machine can run ------------------------------------------------------------------------------------
// Announced by every console, client and server. A player is never given a profile their own machine can't
// run exactly: an original PC copy, an Xbox 360, an older build or a build without the kernel is
// {protocol 0, styles = Stock}.
struct Capabilities {
  uint16_t protocol = 0;
  uint16_t styles = StyleBit(Style::kStock);
};
Capabilities StockOnly();
// Encoded as 4 bytes (protocol, styles); Decode refuses unknown style bits.
Bytes Encode(const Capabilities& c);
bool Decode(const uint8_t* data, size_t size, Capabilities* out);

// --- the host's policy -----------------------------------------------------------------------------------------
enum class Mode : uint8_t {
  kEveryoneStock = 0,  // the game as shipped
  kEveryoneSame = 1,   // everyone moves with `fallback`
  kPlayersChoose = 2,  // each player's request, when it is in `allowed`; else `fallback`
  kPerTeam = 3,        // team[t] (0 = fallback)
  kAssigned = 4,       // the host names each player's profile (PlayerFacts::assigned)
};
enum class Timing : uint8_t {
  kAtSpawn = 0,       // a player's new profile starts at their next spawn
  kAtRoundStart = 1,  // ... at the next round start only
};
inline constexpr int kMaxTeams = 4;
inline constexpr int kMaxProfiles = 8;  // profiles one policy can name

struct Policy {
  Mode mode = Mode::kEveryoneStock;
  Timing timing = Timing::kAtSpawn;
  uint64_t fallback = kNoProfile;           // kNoProfile = stock
  uint64_t team[kMaxTeams] = {};
  std::vector<uint64_t> allowed;            // kPlayersChoose; sorted, unique
  std::vector<Profile> table;               // every profile the policy names, by value (sorted by id, unique)
};
// The policy as shipped: everyone stock.
Policy StockPolicy();
// nullptr when acceptable: every named id is in the table (or kNoProfile), sizes within limits, table valid,
// sorted and unique, allowed sorted and unique.
const char* Validate(const Policy& p);
// Puts the table and allowed lists in canonical order (call before Validate / Encode after editing).
void Canonicalize(Policy* p);
Bytes Encode(const Policy& p);
bool Decode(const uint8_t* data, size_t size, Policy* out);
// The definition of `id` in the policy's table, or nullptr. StockId() is always known.
const Profile* Find(const Policy& p, uint64_t id);

struct PlayerFacts {
  uint64_t requested = kNoProfile;  // the player's own choice (kPlayersChoose)
  uint8_t team = 0;
  uint64_t assigned = kNoProfile;   // the host's choice for this player (kAssigned)
  Capabilities caps = StockOnly();  // the player's own machine
};
// The profile this player must move with. Pure and total: every machine computes the same id from the same
// policy and facts. The result's style is always one the player's machine runs; StockId() when nothing fits.
uint64_t Resolve(const Policy& policy, const PlayerFacts& player);

// --- each player's timeline: profile changes are numbered epochs ------------------------------------------------
// A change never happens under a moving player: the owner (peer-to-peer) or the authority (client-server)
// promotes the wanted profile at a spawn or round start (Policy::timing) and numbers it. Everyone keeps the
// same short history, so a late packet or a prediction replay finds the profile that was in force.
struct EpochEntry {
  uint8_t epoch = 0;
  uint64_t id = kNoProfile;
  int32_t from_ms = 0;  // the game time the epoch began
};
class Timeline {
 public:
  static constexpr int kKeep = 8;
  // Start over from a full record (join, or a gap in the epochs).
  void Reset(uint8_t epoch, uint64_t id, int32_t from_ms);
  // The next epoch from the owner or authority. True when taken; false when stale or a duplicate (ignored) and
  // also false with *gap set when epochs were skipped (ask for a full record, then Reset).
  bool Apply(uint8_t epoch, uint64_t id, int32_t from_ms, bool* gap);
  // The owner's or authority's side: at a spawn or round start, `wanted` becomes current. True (and a new
  // epoch) only when it differs.
  bool Promote(uint64_t wanted, int32_t now_ms);
  uint64_t Current() const;
  uint8_t Epoch() const;
  bool Known() const { return count_ > 0; }
  // The profile in force at `time_ms` / with `epoch`; kNoProfile when the history no longer has it.
  uint64_t At(int32_t time_ms) const;
  uint64_t ByEpoch(uint8_t epoch) const;

 private:
  EpochEntry entries_[kKeep];
  int count_ = 0;  // newest at entries_[count_ - 1]
  void Push(const EpochEntry& e);
};

// --- what machines tell each other ------------------------------------------------------------------------------
// One player's movement as its owner (peer-to-peer) or the authority (client-server) announces it.
struct PlayerRecord {
  uint8_t slot = 0;
  uint8_t team = 0;
  uint8_t epoch = 0;
  uint64_t requested = kNoProfile;
  uint64_t effective = kNoProfile;
  int32_t from_ms = 0;
};
inline constexpr size_t kRecordBytes = 23;
Bytes Encode(const PlayerRecord& r);
bool Decode(const uint8_t* data, size_t size, PlayerRecord* out);

// The digest every machine computes over the policy and every player's slot, team, epoch and effective profile
// (records in slot order; `requested` and `from_ms` are left out: they may legitimately differ in flight).
// Machines compare digests; equal digests mean everyone agrees on who moves how.
// SHA-256 (FIPS 180-4): a two-faced host can't make two different policies with the same digest, as it could
// with a 64-bit hash (2^32 work). Ids stay 64-bit names inside one verified table (duplicates are refused).
using Digest32 = std::array<uint8_t, 32>;
Digest32 Sha256(const uint8_t* data, size_t size);
Digest32 Digest(const Policy& policy, std::vector<PlayerRecord> records);

// Peer-to-peer agreement. Digests differ for a moment around every epoch change; a difference that lasts more
// than `grace_ms` means the machines disagree, and until the next round start every machine moves its own
// players with StockId() (the safe choice: every machine runs it).
class Agreement {
 public:
  static constexpr int kMaxPeers = 32;
  explicit Agreement(int32_t grace_ms = 3000) : grace_ms_(grace_ms) {}
  void Report(int peer, const Digest32& digest, int32_t now_ms);  // peer 0..kMaxPeers-1
  void Forget(int peer);
  // Call every frame with this machine's own digest. False once a disagreement outlasted the grace time; it
  // stays false until RoundStart.
  bool Check(const Digest32& own_digest, int32_t now_ms);
  void RoundStart();
  bool Failed() const { return failed_; }

 private:
  struct Peer {
    bool known = false;
    Digest32 digest{};
  };
  Peer peers_[kMaxPeers];
  int32_t grace_ms_;
  int32_t differ_since_ = 0;
  bool differing_ = false;
  bool failed_ = false;
};

// --- checking what others send (peer-to-peer) -------------------------------------------------------------------
// Each console moves its own players, so a console could claim one profile and move another way (a cheat or a
// bug). Every console checks every remote player's updates against the bounds of their announced profile.
// The game's own stock numbers, from the adapter (cod3mp: g_speed 190, sprint x1.9167, jump 249.8, gravity 800).
struct GameLimits {
  float base_speed = 190.0f;
  float sprint_scale = 1.0f;
  float jump_speed = 0.0f;
  float gravity = 800.0f;
};
struct MotionLimits {
  float max_horizontal = 0;  // units per second
  float max_rise = 0;        // upward speed, units per second
  float gravity = 0;
};
MotionLimits LimitsFor(const Profile& p, const GameLimits& game);

struct MotionSample {
  int32_t time_ms = 0;  // when this machine received the update
  float origin[3] = {};
  float velocity[3] = {};
};
namespace violation {
inline constexpr uint32_t kSpeed = 1u << 0;        // the reported horizontal speed
inline constexpr uint32_t kRise = 1u << 1;         // the reported upward speed
inline constexpr uint32_t kDistance = 1u << 2;     // horizontal distance between two updates (a teleport)
inline constexpr uint32_t kClimb = 1u << 3;        // height gained between two updates
inline constexpr uint32_t kSustained = 1u << 4;    // distance over 2 s or more: a speed hack under the per-update slack
inline constexpr uint32_t kExcuseFlood = 1u << 5;  // jumps excused more often than real events allow
}  // namespace violation
// Tolerances cover the position updates' rounding (1 unit, a direction byte) and their arrival jitter. Nothing
// the checked player sends can turn a check off: only events this machine saw itself excuse a jump (Excuse).
class MotionChecker {
 public:
  static constexpr float kSuspicious = 5.0f;    // score at which the adapter reports the player
  static constexpr int32_t kExcuseGapMs = 1000; // at most one excused jump per second
  static constexpr int32_t kWindowMs = 2000;    // the sustained-speed check's span
  static constexpr int kRing = 32;
  explicit MotionChecker(const MotionLimits& limits) : limits_(limits) {}
  void SetLimits(const MotionLimits& limits) {  // a new epoch: the window starts again
    limits_ = limits;
    count_ = 0;
  }
  const MotionLimits& Limits() const { return limits_; }
  // This machine saw the player spawn, leave a vehicle, or get knocked back by damage it applied: the next update
  // may jump. False (and a kExcuseFlood violation on the next update) when asked more often than kExcuseGapMs.
  bool Excuse(int32_t now_ms);
  uint32_t Feed(const MotionSample& s);  // the violations in this update; adds to the score
  float Score() const { return score_; }
  bool Suspicious() const { return score_ >= kSuspicious; }

 private:
  MotionLimits limits_;
  MotionSample ring_[kRing];
  int count_ = 0;
  int newest_ = 0;
  bool excused_ = false;
  bool flood_ = false;
  bool ever_excused_ = false;
  int32_t last_excuse_ = 0;
  float score_ = 0;
};

// --- the authority's clock for each client (client-server games) ------------------------------------------------
// A client that runs its clock fast (a timescale speed hack) asks for more movement time than really passed. The
// authority grants each command at most the real time since the last one, plus a burst for late or bunched
// packets; the rest is refused (and counted for the log).
class CommandClock {
 public:
  static constexpr int32_t kBurstMs = 500;
  int Allow(int asked_ms, int32_t now_ms);  // the msec this command may move
  int64_t Refused() const { return refused_; }

 private:
  bool started_ = false;
  int32_t last_ = 0;
  int32_t credit_ = 0;
  int64_t refused_ = 0;
};

// --- one machine's checked view of every player (the host's view rules) ---------------------------------------
// Owner-moves games: every machine keeps one, and the host's verdicts are the ones that count ("players check in
// with the host"). Nothing a player's machine claims is taken on trust:
//  - only the session's owner of a slot may speak for it, only the host for the policy;
//  - the profile a player moves with must be the one Resolve gives them; a claimed other profile is reported and
//    its limits are never used;
//  - a new profile's (looser) limits start at a spawn this machine saw; until then the stricter of old and new;
//  - requests change at most once per 5 s; jumps are excused only by events this machine saw.
namespace problem {
inline constexpr uint32_t kBadSlot = 1u << 0;
inline constexpr uint32_t kNotOwner = 1u << 1;         // a machine spoke for a player it doesn't own
inline constexpr uint32_t kNotHost = 1u << 2;          // a policy from a machine that isn't the host
inline constexpr uint32_t kWrongProfile = 1u << 3;     // the owner claims a profile the policy doesn't give
inline constexpr uint32_t kEpochGap = 1u << 4;         // epochs skipped: ask the owner for a full record
inline constexpr uint32_t kChangeWithoutSpawn = 1u << 5;  // a new epoch with no spawn seen since the last one
inline constexpr uint32_t kTooSoon = 1u << 6;          // facts changed again within 5 s (ignored)
inline constexpr uint32_t kInvalidPolicy = 1u << 7;
}  // namespace problem

class PeerView {
 public:
  static constexpr int kSlots = 64;
  static constexpr int32_t kFactsGapMs = 5000;
  explicit PeerView(const GameLimits& game);
  void SetHost(int peer) { host_ = peer; }
  // From the session layer (who joined with which slot), never from a movesync message.
  void SetOwner(int slot, int peer);
  uint32_t SetPolicy(const Policy& p, int from_peer, int32_t now_ms);
  uint32_t SetFacts(int slot, const PlayerFacts& f, int from_peer, int32_t now_ms);
  uint32_t OnRecord(int from_peer, const PlayerRecord& r);
  void OnSpawn(int slot, int32_t now_ms);  // this machine saw the player spawn
  void OnRoundStart(int32_t now_ms);
  bool Excuse(int slot, int32_t now_ms);   // another event this machine saw (vehicle, knockback)
  uint32_t OnMotion(int slot, const MotionSample& s);  // violation:: flags, against the policy's profile
  uint64_t Expected(int slot) const;       // the profile the policy gives this player
  const MotionChecker* Checker(int slot) const;

 private:
  struct Slot {
    int owner = -1;
    PlayerFacts facts;
    bool have_facts = false;
    int32_t facts_at = 0;
    Timeline timeline;
    MotionChecker checker{MotionLimits{}};
    MotionLimits pending;
    bool limits_set = false;
    bool spawn_seen = false;
  };
  void Recompute(int slot, bool at_spawn);
  GameLimits game_;
  Policy policy_;
  int host_ = -1;
  Slot slots_[kSlots];
};

// --- the velocity kernel of the kernel styles -------------------------------------------------------------------
// One movement step's velocity change, written from the published formulas (docs/MOVEMENT_PROFILES.md 4).
// The game's own collision (step slide move) then moves the player; gravity for the move is GravityEnd.
struct KernelState {
  float velocity[3] = {};
  bool on_ground = false;
  bool jump_held = false;  // the jump button was down in the previous step
};
struct KernelInput {
  float wish_dir[3] = {};   // unit length, horizontal (z = 0), or all 0 for no input
  float wish_speed = 0;     // the command's wish speed before the profile's ceiling
  bool jump = false;
  bool crouched = false;
  bool near_edge = false;   // the adapter's edge test (QuakeWorld's edge friction)
  uint16_t msec = 0;
};
struct KernelResult {
  bool jumped = false;
};
// Only for kQuakeWorld, kQuake3 and kSource profiles (others: nothing changes). Inputs are made safe first: a
// non-finite or non-unit direction moves nowhere, msec is at most kMaxStepMs, every velocity component stays
// within kHardSpeed, so no input can make the velocity NaN, infinite, or a teleport-sized step.
inline constexpr uint16_t kMaxStepMs = 200;
inline constexpr float kHardSpeed = 8000.0f;
KernelResult StepVelocity(const Profile& p, const KernelInput& in, KernelState* state);
float GravityEnd(float velocity_z, float gravity, uint16_t msec);
// Nearest whole unit, halves to even, whatever the rounding mode (Quake III's SnapVector).
float SnapToUnit(float v);

// True when this build computes the kernel exactly as every other build does. A compiler that fuses multiplies
// and adds (clang's -ffp-contract=fast, -ffast-math, MSVC's /fp:fast) changes about 7% of the vectors' results.
// Every adapter calls this once at startup; when it is false the machine announces StockOnly(), so it is never
// given a profile it would move differently from the others (the safe failure: the game's own movement).
bool ArithmeticCheck();

}  // namespace movesync
