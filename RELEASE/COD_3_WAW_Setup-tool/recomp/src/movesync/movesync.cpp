// movesync - see movesync.h and docs/MOVEMENT_PROFILES.md. [acct2-decomp] 2026-11-07.
// Build with -ffp-contract=off (GCC, Clang); MSVC's default /fp:precise does not contract.

#include "movesync.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

// Every machine of a match must get the same bits, so no multiply and add may be fused into one rounding,
// whatever the build's flags (the test shows -ffp-contract=fast with an FMA target changes results).
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "movesync must not be built with -ffast-math or /fp:fast: its results must match every other build's"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace movesync {
namespace {

// --- little-endian helpers -------------------------------------------------------------------------------------
void Put8(Bytes& b, uint8_t v) { b.push_back(v); }
void Put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Put64(Bytes& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void PutF(Bytes& b, float f) {
  if (f == 0.0f) f = 0.0f;  // -0.0 is written as 0.0: equal profiles have equal bytes
  uint32_t u;
  std::memcpy(&u, &f, 4);
  Put32(b, u);
}
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t Get64(const uint8_t* p) { return static_cast<uint64_t>(Get32(p)) | (static_cast<uint64_t>(Get32(p + 4)) << 32); }
float GetF(const uint8_t* p) {
  const uint32_t u = Get32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// The profile's twelve numbers, in their byte order.
float* Numbers(Profile& p, int i) {
  float* all[12] = {&p.max_speed,   &p.accelerate, &p.air_accelerate, &p.air_wish_cap,
                    &p.friction,    &p.stop_speed, &p.edge_friction,  &p.gravity,
                    &p.jump_speed,  &p.step_size,  &p.crouch_scale,   &p.max_velocity};
  return all[i];
}
float Number(const Profile& p, int i) { return *Numbers(const_cast<Profile&>(p), i); }

bool KernelStyle(Style s) { return s == Style::kQuakeWorld || s == Style::kQuake3 || s == Style::kSource; }

// Finite by the bits, so the check holds whatever the compiler assumes about NaN.
bool Finite(float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  return (u & 0x7f800000u) != 0x7f800000u;
}
bool In(float v, float lo, float hi) { return Finite(v) && v >= lo && v <= hi; }
bool ZeroOr(float v, float lo, float hi) { return v == 0.0f || In(v, lo, hi); }

}  // namespace

// --- bytes and text armor ------------------------------------------------------------------------------------
uint64_t Fnv1a64(const uint8_t* data, size_t size, uint64_t hash) {
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

namespace {
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
int B64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}
}  // namespace

std::string ToBase64Url(const Bytes& b) {
  std::string out;
  size_t i = 0;
  for (; i + 3 <= b.size(); i += 3) {
    const uint32_t v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    for (int k = 3; k >= 0; --k) out.push_back(kB64[(v >> (6 * k)) & 63]);
  }
  if (b.size() - i == 1) {
    const uint32_t v = b[i] << 16;
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
  } else if (b.size() - i == 2) {
    const uint32_t v = (b[i] << 16) | (b[i + 1] << 8);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
  }
  return out;
}

bool FromBase64Url(std::string_view t, size_t max_bytes, Bytes* out) {
  if (t.size() % 4 == 1 || t.size() / 4 * 3 + 2 > max_bytes + 2) return false;
  Bytes b;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : t) {
    const int v = B64Value(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      b.push_back(static_cast<uint8_t>(acc >> bits));
      acc &= (1u << bits) - 1;
    }
  }
  if (acc != 0 || b.size() > max_bytes) return false;  // leftover bits must be zero
  *out = std::move(b);
  return true;
}

std::string ToHex(const Bytes& b) {
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  for (uint8_t v : b) {
    s.push_back(kHex[v >> 4]);
    s.push_back(kHex[v & 15]);
  }
  return s;
}

// --- profiles ------------------------------------------------------------------------------------------------
const char* Validate(const Profile& p) {
  if (static_cast<unsigned>(p.style) >= kStyleCount) return "unknown style";
  if (p.flags & ~flag::kKnown) return "unknown flag";
  for (int i = 0; i < 12; ++i)
    if (!Finite(Number(p, i))) return "a number is not finite";
  if (!KernelStyle(p.style)) {
    for (int i = 0; i < 12; ++i)
      if (Number(p, i) != 0.0f) return "stock and cod4 take no numbers";
    if (p.move_step_ms != 0 || p.jump_cooldown_ms != kGameJumpCooldown) return "stock and cod4 take no numbers";
    const uint16_t allowed = p.style == Style::kStock
                                 ? flag::kSprint
                                 : static_cast<uint16_t>(flag::kSprint | flag::kCod4Sprint | flag::kCod4Speeds | flag::kCod4Jumps);
    if (p.flags & ~allowed) return "a flag this style doesn't have";
    return nullptr;
  }
  const uint16_t allowed = flag::kAutoBhop | flag::kSnapVelocity | flag::kNoFallDamage | flag::kJumpAdds;
  if (p.flags & ~allowed) return "a flag this style doesn't have";
  if (!In(p.max_speed, 50, 600)) return "maxspeed out of range (50-600)";
  if (!In(p.accelerate, 0, 100)) return "accel out of range (0-100)";
  if (!In(p.air_accelerate, 0, 1000)) return "airaccel out of range (0-1000)";
  if (!ZeroOr(p.air_wish_cap, 1, 1000)) return "aircap out of range (0 or 1-1000)";
  if (!In(p.friction, 0, 20)) return "friction out of range (0-20)";
  if (!In(p.stop_speed, 0, 500)) return "stopspeed out of range (0-500)";
  if (!In(p.edge_friction, 1, 4)) return "edgefriction out of range (1-4)";
  if (!In(p.gravity, 100, 4000)) return "gravity out of range (100-4000)";
  if (!In(p.jump_speed, 0, 600)) return "jumpspeed out of range (0-600)";
  if (!ZeroOr(p.step_size, 1, 32)) return "stepsize out of range (0 or 1-32)";
  if (!(p.crouch_scale == 0.0f || (Finite(p.crouch_scale) && p.crouch_scale > 0.0f && p.crouch_scale <= 1.0f)))
    return "crouchscale out of range (0 or above 0 to 1)";
  if (!In(p.max_velocity, 100, 4000)) return "maxvelocity out of range (100-4000)";
  if (p.move_step_ms > 50) return "stepms out of range (0-50)";
  if (p.jump_cooldown_ms != kGameJumpCooldown && p.jump_cooldown_ms > 2000) return "jumpcooldown out of range (0-2000)";
  return nullptr;
}

Bytes Encode(const Profile& p) {
  Bytes b;
  b.reserve(kProfileBytes);
  Put8(b, 1);  // format
  Put8(b, static_cast<uint8_t>(p.style));
  Put16(b, p.flags);
  for (int i = 0; i < 12; ++i) PutF(b, Number(p, i));
  Put16(b, p.move_step_ms);
  Put16(b, p.jump_cooldown_ms);
  return b;
}

bool Decode(const uint8_t* d, size_t size, Profile* out) {
  if (size != kProfileBytes || d[0] != 1) return false;
  Profile p;
  if (d[1] >= kStyleCount) return false;
  p.style = static_cast<Style>(d[1]);
  p.flags = Get16(d + 2);
  for (int i = 0; i < 12; ++i) *Numbers(p, i) = GetF(d + 4 + 4 * i);
  p.move_step_ms = Get16(d + 52);
  p.jump_cooldown_ms = Get16(d + 54);
  if (Validate(p) != nullptr) return false;
  const Bytes again = Encode(p);
  if (std::memcmp(again.data(), d, kProfileBytes) != 0) return false;  // not canonical (-0.0)
  *out = p;
  return true;
}

uint64_t IdOf(const Profile& p) {
  const Bytes b = Encode(p);
  const uint64_t h = Fnv1a64(b.data(), b.size());
  return h == kNoProfile ? 1 : h;
}

Profile Stock(bool sprint) {
  Profile p;
  p.style = Style::kStock;
  p.flags = sprint ? flag::kSprint : 0;
  return p;
}

Profile Cod4(bool sprint, bool speeds, bool jumps) {
  Profile p;
  p.style = Style::kCod4;
  p.flags = static_cast<uint16_t>(flag::kSprint | (sprint ? flag::kCod4Sprint : 0) | (speeds ? flag::kCod4Speeds : 0) |
                                  (jumps ? flag::kCod4Jumps : 0));
  return p;
}

Profile QuakeWorld() {
  Profile p;
  p.style = Style::kQuakeWorld;
  p.flags = flag::kJumpAdds | flag::kNoFallDamage;
  p.max_speed = 320;
  p.accelerate = 10;
  p.air_accelerate = 10;
  p.air_wish_cap = 30;
  p.friction = 4;
  p.stop_speed = 100;
  p.edge_friction = 2;
  p.gravity = 800;
  p.jump_speed = 270;
  p.step_size = 18;
  p.max_velocity = 2000;
  p.jump_cooldown_ms = 0;
  return p;
}

Profile Quake3() {
  Profile p;
  p.style = Style::kQuake3;
  p.flags = flag::kSnapVelocity;
  p.max_speed = 320;
  p.accelerate = 10;
  p.air_accelerate = 1;
  p.friction = 6;
  p.stop_speed = 100;
  p.edge_friction = 1;
  p.gravity = 800;
  p.jump_speed = 270;
  p.step_size = 18;
  p.crouch_scale = 0.25f;
  p.max_velocity = 4000;  // Quake III has no ceiling; every profile here must have one (MOVEMENT_PROFILES.md 10)
  p.move_step_ms = 8;  // Quake III's feel is its 125 fps steps (the velocity is rounded after each)
  p.jump_cooldown_ms = 0;
  return p;
}

Profile Source() {
  Profile p;
  p.style = Style::kSource;
  p.max_speed = 320;
  p.accelerate = 5.5f;
  p.air_accelerate = 12;
  p.air_wish_cap = 30;
  p.friction = 5.2f;
  p.stop_speed = 80;
  p.edge_friction = 1;
  p.gravity = 800;
  p.jump_speed = 301.993377f;
  p.step_size = 18;
  p.crouch_scale = 0.34f;
  p.max_velocity = 3500;
  p.jump_cooldown_ms = 0;
  return p;
}

uint64_t StockId() {
  static const uint64_t id = IdOf(Stock(false));
  return id;
}

// --- text form -----------------------------------------------------------------------------------------------
namespace {
struct Key {
  const char* name;
  int number;      // index into Numbers, or -1
  int kind;        // 0 number, 1 whole number, 2 flag
  uint16_t bit;    // kind 2
};
constexpr Key kKeys[] = {
    {"maxspeed", 0, 0, 0},       {"accel", 1, 0, 0},       {"airaccel", 2, 0, 0},      {"aircap", 3, 0, 0},
    {"friction", 4, 0, 0},       {"stopspeed", 5, 0, 0},   {"edgefriction", 6, 0, 0},  {"gravity", 7, 0, 0},
    {"jumpspeed", 8, 0, 0},      {"stepsize", 9, 0, 0},    {"crouchscale", 10, 0, 0},  {"maxvelocity", 11, 0, 0},
    {"stepms", -1, 1, 0},        {"jumpcooldown", -2, 1, 0},
    {"autobhop", -1, 2, flag::kAutoBhop}, {"snap", -1, 2, flag::kSnapVelocity},
    {"nofalldamage", -1, 2, flag::kNoFallDamage}, {"jumpadds", -1, 2, flag::kJumpAdds},
};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

bool ParseNumber(std::string_view v, float* out) {
  if (v.empty() || v.size() > 12) return false;
  size_t i = v[0] == '-' ? 1 : 0;
  bool digits = false, dot = false;
  for (size_t k = i; k < v.size(); ++k) {
    if (v[k] >= '0' && v[k] <= '9') {
      digits = true;
    } else if (v[k] == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  if (!digits) return false;
  float f = 0;
  const auto r = std::from_chars(v.data(), v.data() + v.size(), f);
  return r.ec == std::errc() && r.ptr == v.data() + v.size() && Finite(f) && (*out = f, true);
}

bool ParseWhole(std::string_view v, uint32_t max, uint32_t* out) {
  if (v.empty() || v.size() > 5) return false;
  uint32_t n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<uint32_t>(c - '0');
  }
  if (n > max) return false;
  *out = n;
  return true;
}

bool ParseName(std::string_view n, Profile* p) {
  auto cod4_digits = [&](std::string_view d) {
    if (d.size() != 4) return false;
    for (char c : d)
      if (c != '0' && c != '1') return false;
    *p = Cod4(d[0] == '1', d[1] == '1', d[3] == '1');  // d[2]: a part taken out in 2026-10 (rules.h)
    return true;
  };
  if (n == "stock" || n == "3") return *p = Stock(false), true;
  if (n == "stock+sprint") return *p = Stock(true), true;
  if (n == "cod4") return *p = Cod4(true, true, true), true;
  if (n.size() == 9 && n.substr(0, 5) == "cod4:") return cod4_digits(n.substr(5));
  if (n.size() == 5 && n[0] == '4') return cod4_digits(n.substr(1));
  if (n == "quakeworld" || n == "qw") return *p = QuakeWorld(), true;
  if (n == "quake3" || n == "q3") return *p = Quake3(), true;
  if (n == "source" || n == "cs") return *p = Source(), true;
  return false;
}

Profile PresetOf(const Profile& p) {
  switch (p.style) {
    case Style::kQuakeWorld: return QuakeWorld();
    case Style::kQuake3: return Quake3();
    case Style::kSource: return Source();
    default: return p;
  }
}

std::string FormatFloat(float f) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), f);  // shortest text that reads back the same float
  return std::string(buf, r.ptr);
}
}  // namespace

bool ParseProfile(std::string_view text, Profile* out, std::string* why) {
  auto fail = [&](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (text.empty() || text.size() > 200) return fail("empty or longer than 200 characters");
  const size_t comma = text.find(',');
  Profile p;
  if (!ParseName(text.substr(0, comma), &p)) return fail("unknown movement name");
  bool seen[kKeyCount] = {};
  size_t at = comma;
  while (at != std::string_view::npos) {
    if (!KernelStyle(p.style)) return fail("stock and cod4 take no keys");
    const size_t next = text.find(',', at + 1);
    const std::string_view item = text.substr(at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1);
    const size_t eq = item.find('=');
    if (eq == std::string_view::npos) return fail("a key without a value");
    const std::string_view k = item.substr(0, eq), v = item.substr(eq + 1);
    int found = -1;
    for (int i = 0; i < kKeyCount; ++i)
      if (k == kKeys[i].name) found = i;
    if (found < 0) return fail("unknown key");
    if (seen[found]) return fail("a key given twice");
    seen[found] = true;
    const Key& key = kKeys[found];
    if (key.kind == 0) {
      float f;
      if (!ParseNumber(v, &f)) return fail("not a plain decimal number");
      *Numbers(p, key.number) = f;
    } else if (key.kind == 1) {
      uint32_t n;
      if (key.number == -2 && v == "game") {
        p.jump_cooldown_ms = kGameJumpCooldown;
      } else if (!ParseWhole(v, 60000, &n)) {
        return fail("not a whole number");
      } else if (key.number == -1) {
        p.move_step_ms = static_cast<uint16_t>(n);
      } else {
        p.jump_cooldown_ms = static_cast<uint16_t>(n);
      }
    } else {
      if (v != "0" && v != "1") return fail("a switch takes 0 or 1");
      p.flags = static_cast<uint16_t>(v == "1" ? (p.flags | key.bit) : (p.flags & ~key.bit));
    }
    at = next;
  }
  if (const char* bad = Validate(p)) return fail(bad);
  *out = p;
  return true;
}

std::string Describe(const Profile& p) {
  switch (p.style) {
    case Style::kStock: return (p.flags & flag::kSprint) ? "stock+sprint" : "stock";
    case Style::kCod4: {
      std::string s = "cod4:";
      s += (p.flags & flag::kCod4Sprint) ? '1' : '0';
      s += (p.flags & flag::kCod4Speeds) ? '1' : '0';
      s += '0';
      s += (p.flags & flag::kCod4Jumps) ? '1' : '0';
      return s;
    }
    default: break;
  }
  const Profile base = PresetOf(p);
  std::string s = p.style == Style::kQuakeWorld ? "quakeworld" : p.style == Style::kQuake3 ? "quake3" : "source";
  for (const Key& key : kKeys) {
    if (key.kind == 0) {
      const float a = Number(p, key.number), b = Number(base, key.number);
      if (a != b) s += std::string(",") + key.name + "=" + FormatFloat(a);
    } else if (key.kind == 1) {
      const uint16_t a = key.number == -1 ? p.move_step_ms : p.jump_cooldown_ms;
      const uint16_t b = key.number == -1 ? base.move_step_ms : base.jump_cooldown_ms;
      if (a != b) s += std::string(",") + key.name + "=" + (a == kGameJumpCooldown && key.number == -2 ? "game" : std::to_string(a));
    } else if ((p.flags & key.bit) != (base.flags & key.bit)) {
      s += std::string(",") + key.name + "=" + ((p.flags & key.bit) ? "1" : "0");
    }
  }
  return s;
}

// --- capabilities ------------------------------------------------------------------------------------------------
Capabilities StockOnly() { return Capabilities{}; }

Bytes Encode(const Capabilities& c) {
  Bytes b;
  Put16(b, c.protocol);
  Put16(b, c.styles);
  return b;
}

bool Decode(const uint8_t* d, size_t size, Capabilities* out) {
  if (size != 4) return false;
  Capabilities c;
  c.protocol = Get16(d);
  c.styles = Get16(d + 2);
  if (c.styles & ~((1u << kStyleCount) - 1)) return false;
  if (!(c.styles & StyleBit(Style::kStock))) return false;  // everyone runs the game's own movement
  *out = c;
  return true;
}

// --- policy ------------------------------------------------------------------------------------------------------
Policy StockPolicy() { return Policy{}; }

void Canonicalize(Policy* p) {
  std::sort(p->table.begin(), p->table.end(), [](const Profile& a, const Profile& b) { return IdOf(a) < IdOf(b); });
  p->table.erase(std::unique(p->table.begin(), p->table.end(),
                             [](const Profile& a, const Profile& b) { return IdOf(a) == IdOf(b); }),
                 p->table.end());
  std::sort(p->allowed.begin(), p->allowed.end());
  p->allowed.erase(std::unique(p->allowed.begin(), p->allowed.end()), p->allowed.end());
}

const Profile* Find(const Policy& p, uint64_t id) {
  for (const Profile& prof : p.table)
    if (IdOf(prof) == id) return &prof;
  if (id == StockId()) {
    static const Profile stock = Stock(false);
    return &stock;
  }
  return nullptr;
}

const char* Validate(const Policy& p) {
  if (static_cast<unsigned>(p.mode) > 4) return "unknown mode";
  if (static_cast<unsigned>(p.timing) > 1) return "unknown timing";
  if (p.table.size() > kMaxProfiles || p.allowed.size() > kMaxProfiles) return "too many profiles";
  uint64_t prev = 0;
  for (size_t i = 0; i < p.table.size(); ++i) {
    if (Validate(p.table[i]) != nullptr) return "a profile in the table is invalid";
    const uint64_t id = IdOf(p.table[i]);
    if (i > 0 && id <= prev) return "the table is not sorted and unique";
    prev = id;
  }
  auto known = [&](uint64_t id) { return id == kNoProfile || Find(p, id) != nullptr; };
  if (!known(p.fallback)) return "the fallback is not in the table";
  for (uint64_t t : p.team)
    if (!known(t)) return "a team's profile is not in the table";
  for (size_t i = 0; i < p.allowed.size(); ++i) {
    if (p.allowed[i] == kNoProfile || !known(p.allowed[i])) return "an allowed profile is not in the table";
    if (i > 0 && p.allowed[i] <= p.allowed[i - 1]) return "the allowed list is not sorted and unique";
  }
  return nullptr;
}

Bytes Encode(const Policy& p) {
  Bytes b;
  Put8(b, 1);  // format
  Put8(b, static_cast<uint8_t>(p.mode));
  Put8(b, static_cast<uint8_t>(p.timing));
  Put8(b, static_cast<uint8_t>(p.table.size()));
  Put8(b, static_cast<uint8_t>(p.allowed.size()));
  Put8(b, 0);
  Put8(b, 0);
  Put8(b, 0);
  Put64(b, p.fallback);
  for (uint64_t t : p.team) Put64(b, t);
  for (uint64_t a : p.allowed) Put64(b, a);
  for (const Profile& prof : p.table) {
    const Bytes pb = Encode(prof);
    b.insert(b.end(), pb.begin(), pb.end());
  }
  return b;
}

bool Decode(const uint8_t* d, size_t size, Policy* out) {
  if (size < 48 || d[0] != 1 || d[5] || d[6] || d[7]) return false;
  const size_t tables = d[3], allowed = d[4];
  if (tables > kMaxProfiles || allowed > kMaxProfiles) return false;
  if (size != 48 + 8 * allowed + kProfileBytes * tables) return false;
  if (d[1] > 4 || d[2] > 1) return false;
  Policy p;
  p.mode = static_cast<Mode>(d[1]);
  p.timing = static_cast<Timing>(d[2]);
  p.fallback = Get64(d + 8);
  for (int t = 0; t < kMaxTeams; ++t) p.team[t] = Get64(d + 16 + 8 * t);
  for (size_t i = 0; i < allowed; ++i) p.allowed.push_back(Get64(d + 48 + 8 * i));
  const uint8_t* tp = d + 48 + 8 * allowed;
  for (size_t i = 0; i < tables; ++i) {
    Profile prof;
    if (!Decode(tp + kProfileBytes * i, kProfileBytes, &prof)) return false;
    p.table.push_back(prof);
  }
  if (Validate(p) != nullptr) return false;
  *out = std::move(p);
  return true;
}

uint64_t Resolve(const Policy& policy, const PlayerFacts& player) {
  if (Validate(policy) != nullptr) return StockId();
  auto runs = [&](uint64_t id) {
    if (id == StockId()) return true;  // every machine runs the game's own movement
    const Profile* prof = Find(policy, id);
    return prof != nullptr && player.caps.protocol == kProtocol && (player.caps.styles & StyleBit(prof->style)) != 0;
  };
  auto in_table = [&](uint64_t id) { return id != kNoProfile && Find(policy, id) != nullptr; };
  const uint64_t fallback = policy.fallback == kNoProfile ? StockId() : policy.fallback;
  uint64_t want = StockId();
  switch (policy.mode) {
    case Mode::kEveryoneStock: want = StockId(); break;
    case Mode::kEveryoneSame: want = fallback; break;
    case Mode::kPlayersChoose:
      want = std::binary_search(policy.allowed.begin(), policy.allowed.end(), player.requested) ? player.requested
                                                                                                 : fallback;
      break;
    case Mode::kPerTeam:
      want = (player.team < kMaxTeams && policy.team[player.team] != kNoProfile) ? policy.team[player.team] : fallback;
      break;
    case Mode::kAssigned: want = in_table(player.assigned) ? player.assigned : fallback; break;
  }
  if (runs(want)) return want;
  if (runs(fallback)) return fallback;
  return StockId();
}

// --- timeline ------------------------------------------------------------------------------------------------------
void Timeline::Push(const EpochEntry& e) {
  if (count_ == kKeep) {
    for (int i = 1; i < kKeep; ++i) entries_[i - 1] = entries_[i];
    --count_;
  }
  entries_[count_++] = e;
}

void Timeline::Reset(uint8_t epoch, uint64_t id, int32_t from_ms) {
  count_ = 0;
  Push({epoch, id, from_ms});
}

bool Timeline::Apply(uint8_t epoch, uint64_t id, int32_t from_ms, bool* gap) {
  if (gap) *gap = false;
  if (count_ == 0) {
    Reset(epoch, id, from_ms);
    return true;
  }
  const EpochEntry& last = entries_[count_ - 1];
  const uint8_t ahead = static_cast<uint8_t>(epoch - last.epoch);
  if (ahead == 1) {
    Push({epoch, id, from_ms});
    return true;
  }
  // Same epoch with another profile, or epochs skipped: this copy can't be trusted any more.
  if ((ahead == 0 && id != last.id) || (ahead > 1 && ahead < 128)) {
    if (gap) *gap = true;
  }
  return false;  // a duplicate or a stale (older) record
}

bool Timeline::Promote(uint64_t wanted, int32_t now_ms) {
  if (count_ > 0 && entries_[count_ - 1].id == wanted) return false;
  const uint8_t epoch = count_ > 0 ? static_cast<uint8_t>(entries_[count_ - 1].epoch + 1) : 0;
  Push({epoch, wanted, now_ms});
  return true;
}

uint64_t Timeline::Current() const { return count_ > 0 ? entries_[count_ - 1].id : kNoProfile; }
uint8_t Timeline::Epoch() const { return count_ > 0 ? entries_[count_ - 1].epoch : 0; }

uint64_t Timeline::At(int32_t time_ms) const {
  for (int i = count_ - 1; i >= 0; --i)
    if (entries_[i].from_ms <= time_ms) return entries_[i].id;
  return kNoProfile;
}

uint64_t Timeline::ByEpoch(uint8_t epoch) const {
  for (int i = count_ - 1; i >= 0; --i)
    if (entries_[i].epoch == epoch) return entries_[i].id;
  return kNoProfile;
}

// --- records and agreement -----------------------------------------------------------------------------------------
Bytes Encode(const PlayerRecord& r) {
  Bytes b;
  Put8(b, r.slot);
  Put8(b, r.team);
  Put8(b, r.epoch);
  Put64(b, r.requested);
  Put64(b, r.effective);
  Put32(b, static_cast<uint32_t>(r.from_ms));
  return b;
}

bool Decode(const uint8_t* d, size_t size, PlayerRecord* out) {
  if (size != kRecordBytes || d[0] >= 64 || d[1] >= kMaxTeams) return false;
  PlayerRecord r;
  r.slot = d[0];
  r.team = d[1];
  r.epoch = d[2];
  r.requested = Get64(d + 3);
  r.effective = Get64(d + 11);
  r.from_ms = static_cast<int32_t>(Get32(d + 19));
  if (r.effective == kNoProfile) return false;
  *out = r;
  return true;
}

namespace {
uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Digest32 Sha256(const uint8_t* data, size_t size) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  Bytes m(data, data + size);
  const uint64_t bits = static_cast<uint64_t>(size) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  for (size_t at = 0; at < m.size(); at += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<uint32_t>(m[at + 4 * i]) << 24) | (static_cast<uint32_t>(m[at + 4 * i + 1]) << 16) |
             (static_cast<uint32_t>(m[at + 4 * i + 2]) << 8) | m[at + 4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  Digest32 out;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
  return out;
}

Digest32 Digest(const Policy& policy, std::vector<PlayerRecord> records) {
  std::sort(records.begin(), records.end(), [](const PlayerRecord& a, const PlayerRecord& b) {
    if (a.slot != b.slot) return a.slot < b.slot;
    if (a.epoch != b.epoch) return a.epoch < b.epoch;
    return a.effective < b.effective;
  });
  Bytes b = {'M', 'S', 'D', '2'};
  const Bytes pb = Encode(policy);
  b.insert(b.end(), pb.begin(), pb.end());
  for (const PlayerRecord& r : records) {
    Put8(b, r.slot);
    Put8(b, r.team);
    Put8(b, r.epoch);
    Put64(b, r.effective);
  }
  return Sha256(b.data(), b.size());
}

void Agreement::Report(int peer, const Digest32& digest, int32_t) {
  if (peer < 0 || peer >= kMaxPeers) return;
  peers_[peer].known = true;
  peers_[peer].digest = digest;
}

void Agreement::Forget(int peer) {
  if (peer >= 0 && peer < kMaxPeers) peers_[peer] = Peer{};
}

bool Agreement::Check(const Digest32& own, int32_t now_ms) {
  bool differ = false;
  for (const Peer& p : peers_)
    if (p.known && p.digest != own) differ = true;
  if (!differ) {
    differing_ = false;
  } else if (!differing_) {
    differing_ = true;
    differ_since_ = now_ms;
  } else if (now_ms - differ_since_ > grace_ms_) {
    failed_ = true;
  }
  return !failed_;
}

void Agreement::RoundStart() {
  failed_ = false;
  differing_ = false;
}

// --- motion checks -------------------------------------------------------------------------------------------------
MotionLimits LimitsFor(const Profile& p, const GameLimits& game) {
  MotionLimits m;
  const float sprint = game.sprint_scale > 1.0f ? game.sprint_scale : 1.0f;
  switch (p.style) {
    case Style::kStock:
      m.max_horizontal = game.base_speed * sprint;
      m.max_rise = game.jump_speed;
      m.gravity = game.gravity;
      break;
    case Style::kCod4:
      m.max_horizontal = game.base_speed * (sprint > 1.5f ? sprint : 1.5f);
      m.max_rise = game.jump_speed;
      m.gravity = game.gravity;
      break;
    default:
      m.max_horizontal = p.max_velocity;
      m.max_rise = (p.flags & flag::kJumpAdds) ? p.jump_speed * 2.0f : p.jump_speed;
      m.gravity = p.gravity;
      break;
  }
  return m;
}

bool MotionChecker::Excuse(int32_t now_ms) {
  if (ever_excused_ && now_ms - last_excuse_ < kExcuseGapMs) {
    flood_ = true;
    return false;
  }
  ever_excused_ = true;
  last_excuse_ = now_ms;
  excused_ = true;
  return true;
}

uint32_t MotionChecker::Feed(const MotionSample& s) {
  uint32_t v = 0;
  if (flood_) {
    v |= violation::kExcuseFlood;
    flood_ = false;
  }
  const MotionSample* last = count_ > 0 ? &ring_[newest_] : nullptr;
  const int32_t gap = last ? s.time_ms - last->time_ms : 0;
  if (last && gap <= 0) return v;  // out of order: ignored
  if (!last || excused_ || gap > 2000) {  // the first one, an excused jump, or a long silence: start again here
    excused_ = false;
    ring_[0] = s;
    newest_ = 0;
    count_ = 1;
    if (v) score_ = score_ + 1.0f;
    return v;
  }
  const float dt = static_cast<float>(gap) / 1000.0f;
  score_ = score_ - 0.5f * dt;
  if (score_ < 0.0f) score_ = 0.0f;
  const float hs = std::sqrt(s.velocity[0] * s.velocity[0] + s.velocity[1] * s.velocity[1]);
  if (hs > limits_.max_horizontal * 1.15f + 8.0f) v |= violation::kSpeed;
  if (s.velocity[2] > limits_.max_rise * 1.15f + 8.0f) v |= violation::kRise;
  const float dx = s.origin[0] - last->origin[0], dy = s.origin[1] - last->origin[1];
  const float dist = std::sqrt(dx * dx + dy * dy);
  if (dist > limits_.max_horizontal * 1.25f * dt + 16.0f) v |= violation::kDistance;
  const float climb = s.origin[2] - last->origin[2];
  if (climb > limits_.max_rise * dt + 34.0f) v |= violation::kClimb;  // 18 of step plus the rounding
  newest_ = (newest_ + 1) % kRing;
  ring_[newest_] = s;
  if (count_ < kRing) ++count_;
  // The sustained check: the oldest update at least kWindowMs back. Over 2 s the arrival jitter is a few percent,
  // so 10 % over the profile's ceiling is enough.
  for (int k = count_ - 1; k >= 1; --k) {
    const MotionSample& old = ring_[(newest_ - k + kRing) % kRing];
    const int32_t span = s.time_ms - old.time_ms;
    if (span < kWindowMs) break;
    const float wx = s.origin[0] - old.origin[0], wy = s.origin[1] - old.origin[1];
    const float t = static_cast<float>(span) / 1000.0f;
    if (std::sqrt(wx * wx + wy * wy) > limits_.max_horizontal * 1.10f * t + 24.0f) v |= violation::kSustained;
    break;
  }
  for (uint32_t bit = v; bit; bit &= bit - 1) score_ = score_ + 1.0f;
  return v;
}

int CommandClock::Allow(int asked_ms, int32_t now_ms) {
  if (asked_ms < 0) asked_ms = 0;
  if (!started_) {
    started_ = true;
    last_ = now_ms;
    credit_ = kBurstMs;
  }
  int32_t passed = now_ms - last_;
  if (passed < 0) passed = 0;
  last_ = now_ms;
  int64_t credit = static_cast<int64_t>(credit_) + passed;
  if (credit > kBurstMs) credit = kBurstMs;  // no saving up: a client can't idle and then move in one burst
  const int granted = asked_ms < credit ? asked_ms : static_cast<int>(credit);
  credit_ = static_cast<int32_t>(credit - granted);
  refused_ += asked_ms - granted;
  return granted;
}

namespace {
MotionLimits Stricter(const MotionLimits& a, const MotionLimits& b) {
  MotionLimits m = b;
  if (a.max_horizontal < m.max_horizontal) m.max_horizontal = a.max_horizontal;
  if (a.max_rise < m.max_rise) m.max_rise = a.max_rise;
  return m;
}
}  // namespace

PeerView::PeerView(const GameLimits& game) : game_(game), policy_(StockPolicy()) {
  for (Slot& s : slots_) {
    s.pending = LimitsFor(Stock(false), game_);
    s.checker.SetLimits(s.pending);
  }
}

void PeerView::SetOwner(int slot, int peer) {
  if (slot < 0 || slot >= kSlots) return;
  if (slots_[slot].owner != peer) {
    const MotionLimits stock = LimitsFor(Stock(false), game_);
    slots_[slot] = Slot{};
    slots_[slot].owner = peer;
    slots_[slot].pending = stock;
    slots_[slot].checker.SetLimits(stock);
  }
}

void PeerView::Recompute(int slot, bool at_spawn) {
  Slot& s = slots_[slot];
  const Profile* p = Find(policy_, Expected(slot));
  s.pending = LimitsFor(p ? *p : Stock(false), game_);
  // Looser limits only from a spawn this machine saw (or the first facts of a player); stricter ones at once.
  const MotionLimits use = (at_spawn || !s.limits_set) ? s.pending : Stricter(s.checker.Limits(), s.pending);
  s.limits_set = true;
  const MotionLimits& cur = s.checker.Limits();
  if (use.max_horizontal != cur.max_horizontal || use.max_rise != cur.max_rise || use.gravity != cur.gravity)
    s.checker.SetLimits(use);
}

uint32_t PeerView::SetPolicy(const Policy& p, int from_peer, int32_t) {
  if (from_peer != host_) return problem::kNotHost;
  if (Validate(p) != nullptr) return problem::kInvalidPolicy;
  policy_ = p;
  for (int i = 0; i < kSlots; ++i)
    if (slots_[i].owner >= 0) Recompute(i, false);
  return 0;
}

uint32_t PeerView::SetFacts(int slot, const PlayerFacts& f, int from_peer, int32_t now_ms) {
  if (slot < 0 || slot >= kSlots) return problem::kBadSlot;
  Slot& s = slots_[slot];
  if (s.owner < 0 || from_peer != s.owner) return problem::kNotOwner;
  if (s.have_facts && now_ms - s.facts_at < kFactsGapMs) return problem::kTooSoon;
  s.facts = f;
  s.have_facts = true;
  s.facts_at = now_ms;
  Recompute(slot, false);
  return 0;
}

uint64_t PeerView::Expected(int slot) const {
  if (slot < 0 || slot >= kSlots || !slots_[slot].have_facts) return StockId();
  return Resolve(policy_, slots_[slot].facts);
}

uint32_t PeerView::OnRecord(int from_peer, const PlayerRecord& r) {
  if (r.slot >= kSlots) return problem::kBadSlot;
  Slot& s = slots_[r.slot];
  if (s.owner < 0 || from_peer != s.owner) return problem::kNotOwner;
  if (r.effective != Expected(r.slot)) return problem::kWrongProfile;  // never applied; the limits stay the policy's
  const bool first = !s.timeline.Known();
  bool gap = false;
  const bool taken = s.timeline.Apply(r.epoch, r.effective, r.from_ms, &gap);
  uint32_t out = gap ? problem::kEpochGap : 0;
  if (taken && !first && !s.spawn_seen) out |= problem::kChangeWithoutSpawn;
  if (taken) s.spawn_seen = false;
  return out;
}

void PeerView::OnSpawn(int slot, int32_t now_ms) {
  if (slot < 0 || slot >= kSlots) return;
  slots_[slot].spawn_seen = true;
  Recompute(slot, true);
  slots_[slot].checker.Excuse(now_ms);
}

void PeerView::OnRoundStart(int32_t now_ms) {
  for (int i = 0; i < kSlots; ++i)
    if (slots_[i].owner >= 0) OnSpawn(i, now_ms);
}

bool PeerView::Excuse(int slot, int32_t now_ms) {
  return slot >= 0 && slot < kSlots && slots_[slot].checker.Excuse(now_ms);
}

uint32_t PeerView::OnMotion(int slot, const MotionSample& sample) {
  if (slot < 0 || slot >= kSlots || slots_[slot].owner < 0) return 0;
  return slots_[slot].checker.Feed(sample);
}

const MotionChecker* PeerView::Checker(int slot) const {
  return slot >= 0 && slot < kSlots ? &slots_[slot].checker : nullptr;
}

// --- velocity kernel -------------------------------------------------------------------------------------------------
namespace {
float Dot(const float v[3], const float d[3]) { return (v[0] * d[0] + v[1] * d[1]) + v[2] * d[2]; }
void AddScaled(float v[3], const float d[3], float s) {
  v[0] = v[0] + s * d[0];
  v[1] = v[1] + s * d[1];
  v[2] = v[2] + s * d[2];
}
}  // namespace

KernelResult StepVelocity(const Profile& p, const KernelInput& input, KernelState* st) {
  KernelResult r;
  if (!KernelStyle(p.style)) return r;
  float* v = st->velocity;
  for (int i = 0; i < 3; ++i) {
    if (!Finite(v[i])) v[i] = 0.0f;
    if (v[i] > kHardSpeed) v[i] = kHardSpeed;
    if (v[i] < -kHardSpeed) v[i] = -kHardSpeed;
  }
  KernelInput in = input;
  bool dir_ok = true;
  for (int i = 0; i < 3; ++i) dir_ok = dir_ok && Finite(in.wish_dir[i]);
  if (!dir_ok || Dot(in.wish_dir, in.wish_dir) > 1.01f) in.wish_dir[0] = in.wish_dir[1] = in.wish_dir[2] = 0.0f;
  if (in.msec > kMaxStepMs) in.msec = kMaxStepMs;
  const float dt = static_cast<float>(in.msec) / 1000.0f;
  float wish = Finite(in.wish_speed) ? in.wish_speed : 0.0f;
  if (wish > p.max_speed) wish = p.max_speed;
  if (wish < 0.0f) wish = 0.0f;
  if (in.crouched && p.crouch_scale > 0.0f) wish = wish * p.crouch_scale;

  // Jump first: a jump on the landing step leaves the ground before friction (Quake's bunny hop).
  if (st->on_ground && in.jump && (!st->jump_held || (p.flags & flag::kAutoBhop))) {
    v[2] = (p.flags & flag::kJumpAdds) ? v[2] + p.jump_speed : p.jump_speed;
    st->on_ground = false;
    r.jumped = true;
  }
  st->jump_held = in.jump;

  if (st->on_ground) {
    const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    if (speed < 1.0f) {
      v[0] = 0.0f;
      v[1] = 0.0f;
    } else {
      const float control = speed < p.stop_speed ? p.stop_speed : speed;
      float drop = (control * p.friction) * dt;
      if (in.near_edge) drop = drop * p.edge_friction;
      float kept = speed - drop;
      if (kept < 0.0f) kept = 0.0f;
      const float scale = kept / speed;
      v[0] = v[0] * scale;
      v[1] = v[1] * scale;
      v[2] = v[2] * scale;
    }
    const float add = wish - Dot(v, in.wish_dir);
    if (add > 0.0f) {
      float accel = (p.accelerate * dt) * wish;
      if (accel > add) accel = add;
      AddScaled(v, in.wish_dir, accel);
    }
  } else {
    float capped = wish;
    if (p.air_wish_cap > 0.0f && capped > p.air_wish_cap) capped = p.air_wish_cap;
    const float add = capped - Dot(v, in.wish_dir);
    if (add > 0.0f) {
      float accel = (p.air_accelerate * dt) * wish;  // the full wish speed: air strafing's gain
      if (accel > add) accel = add;
      AddScaled(v, in.wish_dir, accel);
    }
  }

  if (p.max_velocity > 0.0f) {
    const float hs = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    if (hs > p.max_velocity) {
      const float scale = p.max_velocity / hs;
      v[0] = v[0] * scale;
      v[1] = v[1] * scale;
    }
  }
  if (v[2] > kHardSpeed) v[2] = kHardSpeed;
  if (v[2] < -kHardSpeed) v[2] = -kHardSpeed;
  return r;
}

float GravityEnd(float velocity_z, float gravity, uint16_t msec) {
  return velocity_z - gravity * (static_cast<float>(msec) / 1000.0f);
}

float SnapToUnit(float v) {
  if (!(std::fabs(v) < 8388608.0f)) return v;  // 2^23 and up are whole already (and NaN stays NaN)
  const float f = std::floor(v);
  const float diff = v - f;  // exact below 2^23
  if (diff > 0.5f) return f + 1.0f;
  if (diff < 0.5f) return f;
  return std::fmod(f, 2.0f) == 0.0f ? f : f + 1.0f;  // halves to even
}

namespace {
// Six QuakeWorld steps and two gravity steps from the test vectors whose results a fused build gets wrong
// (found by building the test with clang -ffp-contract=fast; floats as their bits).
struct CheckCase {
  uint32_t s[3];
  bool ground, held;
  uint32_t dir[2], wish;
  bool jump, crouch;
  uint16_t msec;
  uint32_t out[3];
};
constexpr CheckCase kCheckCases[] = {
    {{0x44574754, 0x43c14f30, 0}, true, true, {0xbe5c5399, 0xbf7a0108}, 0x43826582, true, false, 37,
     {0x44323a19, 0x436b2a6c, 0}},
    {{0xc28b3020, 0xc397d1b8, 0}, false, false, {0x3f5fbe28, 0x3ef8cc64}, 0x432dbe5d, false, false, 37,
     {0xc1568ba0, 0xc3883336, 0}},
    {{0xc43b86ba, 0x4411f5ec, 0}, true, false, {0x3f5ea2e1, 0x3efcbce4}, 0x4303a362, false, false, 23,
     {0xc423b0ee, 0x440844c5, 0}},
    {{0x443036b4, 0xc12e3dc0, 0xc3d3bde2}, true, true, {0xbf1abe33, 0x3f4bf011}, 0x428db7c6, false, true, 46,
     {0x440add5e, 0x4188a3d8, 0xc3acc800}},
    {{0xc389c1ac, 0x4453647e, 0x41a93020}, false, false, {0x3ed790d2, 0xbf683450}, 0x43a474cf, false, false, 41,
     {0xc35c463c, 0x4435a430, 0x41a93020}},
    {{0xc37050a8, 0xc3dfdc49, 0}, true, false, {0x3f4de896, 0xbf181c58}, 0x43895da2, false, false, 28,
     {0xc31786de, 0xc3dda44e, 0}},
};
float AsFloat(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
uint32_t AsBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
}  // namespace

bool ArithmeticCheck() {
  const Profile qw = QuakeWorld();
  for (const CheckCase& c : kCheckCases) {
    KernelState st;
    for (int i = 0; i < 3; ++i) st.velocity[i] = AsFloat(c.s[i]);
    st.on_ground = c.ground;
    st.jump_held = c.held;
    KernelInput in;
    in.wish_dir[0] = AsFloat(c.dir[0]);
    in.wish_dir[1] = AsFloat(c.dir[1]);
    in.wish_speed = AsFloat(c.wish);
    in.jump = c.jump;
    in.crouched = c.crouch;
    in.msec = c.msec;
    StepVelocity(qw, in, &st);
    for (int i = 0; i < 3; ++i)
      if (AsBits(st.velocity[i]) != c.out[i]) return false;
  }
  return AsBits(GravityEnd(AsFloat(0xc03fffff), 800.0f, 15)) == 0xc1700000 &&
         AsBits(GravityEnd(AsFloat(0xbf800000), 800.0f, 19)) == 0xc181999a;
}

}  // namespace movesync
