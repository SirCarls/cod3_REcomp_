// cod3mp - [acct2-main] 2026-11-14 (owner 15:31: "We want a safe environment when playing online";
// dev/agents/acct2-main/MP-SAFETY.md gap 3): checks on peer messages, for what the 2006 handlers don't check.
//
// The position update (type 0x23, docs/MP_PROTOCOL.md 2.4) names the player it moves in its first 8 bits after the
// payload's header bit. Read in the generated code (build/cod3/mp/generated/codmp):
//  - its handler (MPPeer::HandlePositionUpdate 0x823F9528) finds the sender's own first player slot (0x823DCF70:
//    the first of the 24 slots whose connection object, at +4, is the one the message came on), reads the player
//    index from the payload with that slot as the default, takes the slot only if it is in use (0x823DD068) and not
//    one of this console's own players (0x823D8FF0), and moves it. It never checks that the player belongs to the
//    console that sent the message (MP_PROTOCOL.md 4), so one console could move any other console's player on
//    everyone's screen;
//  - only MPPeer::SendLocalPlayerUpdates 0x82403840 makes this message, in three places, each for this console's
//    own players and never type-checked (bdMessage's constructor 0x821B1E38 with r5 = 0): no console passes on
//    another console's update.
// So a 0x23 is dropped here when the slot it names is in use by another console's connection, or when its payload
// is type-checked (its header bit set). Everything else goes on as before: a slot out of range or unused, or a
// payload too short to name one, the game itself ignores or reads as the sender's own player. When the objects
// can't be read as expected (the listener isn't the player manager's MPPeer, a pointer out of range), nothing is
// dropped, and that is said once. Nothing of the game's is called and no reference is taken.
// Only type 0x23 is looked at; the other ~80 types are for later, as their payloads are mapped.
//
// The bit buffer's readers keep one global byte, 0x82B793E6, that says whether the last read worked: about 13 of
// MPPeer's readers (0x823D7050, 0x823D7DF0, 0x823EF2B0, ...) store each read's result there, and 0x823D8140 tests it
// before it reads, so a failed read at the end of one message changed how the next message was read
// (MP_PROTOCOL.md 4: a position delta then reads no coordinates). Before every peer message reaches the game, the
// byte is set back to 1 ("the last read worked", what it holds after any message that was read whole) when it is 0.
//
// [acct2-main] 2026-11-14 21:05 The test log (cod3mp_debug_peer_guard) also counts every peer message by type, with
// the most of one type in one second from one connection, for MP-SAFETY.md gap 1(c): what real play sends, before
// any rate limit is chosen. Only counted while that test setting is on; nothing is dropped for it.

#include "game/mp/peer_guard_mp.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3mp_peer_guard, true, "cod3/Multiplayer",
                    "Drop a message from another console that moves a player who isn't that console's (a position "
                    "update for someone else's player): the 2006 game doesn't check it. Applies at once")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3mp_debug_peer_guard, false, "cod3",
                    "Tests: every 10 s, how many position updates the peer guard checked, let through and dropped, "
                    "and how many peer messages of each type came (the most in one second from one console)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::mp {

namespace {

constexpr uint8_t kPositionUpdate = 0x23;
// As session.cpp reads them: the network holder, its MPPlayerManager, the manager's MPPeer and its 24 player slots.
constexpr uint32_t kNetHolderPtr = 0x82BCCAC0;
constexpr uint32_t kManagerPeer = 0xAEB0;
constexpr uint32_t kPeerSlots = 0x1010, kSlotSize = 0x310, kSlotCount = 24, kSlotConnection = 4;
// The message (bdMessage: +8 the type, +12 the payload's bdBitBuffer) and the pair the listener's r4 points at.
constexpr uint32_t kMessageType = 8, kMessagePayload = 12, kPairFrom = 4;
// A bdBitBuffer: +8 its bytes, +0x10 how many, +0x18 one past the highest bit written.
constexpr uint32_t kBufferData = 8, kBufferSize = 0x10, kBufferWritten = 0x18;
constexpr uint32_t kMaxPayloadBytes = 0x10000;  // far more than any game message
constexpr uint32_t kLastReadOk = 0x82B793E6;    // the bit buffer readers' "the last read worked" byte
constexpr int kWarnings = 8;

bool ValidPtr(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

struct Counts {
  uint64_t checked = 0;     // position updates looked at
  uint64_t other = 0;       // dropped: the player is another console's
  uint64_t typed = 0;       // dropped: a type-checked payload
  uint64_t unreadable = 0;  // let through: the objects couldn't be read
  uint64_t reset = 0;       // the read flag was 0 before a message, set back to 1
};
Counts g_counts;
Counts g_said;  // as of the last summary
std::chrono::steady_clock::time_point g_next_debug{};
std::chrono::steady_clock::time_point g_next_summary{};
int g_warned = 0;
bool g_said_unreadable = false;

// The test log's counts by message type (cod3mp_debug_peer_guard). Up to kRateConnections connections are told
// apart; a match has at most 24 players, on fewer consoles.
constexpr int kRateConnections = 16;
struct TypeRate {
  uint64_t total = 0;
  uint32_t most = 0;  // the most in one second from one connection
};
struct ConnectionRate {
  uint32_t from = 0;  // the connection object; 0 = a free entry
  int64_t second = -1;
  uint16_t now[256] = {};  // this second's messages by type
};
TypeRate g_types[256];
ConnectionRate g_connections[kRateConnections];
uint64_t g_untold = 0;  // messages from a connection past the first kRateConnections, or from none that could be read

void CountRate(uint8_t type, uint32_t from, std::chrono::steady_clock::time_point now) {
  ++g_types[type].total;
  const int64_t second = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  ConnectionRate* c = nullptr;
  if (from != 0)
    for (auto& k : g_connections)  // filled in order and never emptied, so the first free one ends the search
      if (k.from == from || k.from == 0) {
        c = &k;
        break;
      }
  if (!c) {
    ++g_untold;
    return;
  }
  c->from = from;
  if (c->second != second) {
    c->second = second;
    std::fill(std::begin(c->now), std::end(c->now), uint16_t(0));
  }
  if (c->now[type] < 0xFFFF) ++c->now[type];
  g_types[type].most = std::max<uint32_t>(g_types[type].most, c->now[type]);
}

// "23: 5321 (31), 4C: 210 (10), ...": the 16 types that came most often.
std::string RateLine() {
  int order[256];
  for (int i = 0; i < 256; ++i) order[i] = i;
  std::stable_sort(std::begin(order), std::end(order),
                   [](int a, int b) { return g_types[a].total > g_types[b].total; });
  std::string out;
  for (int k = 0; k < 16 && g_types[order[k]].total != 0; ++k) {
    char item[64];
    std::snprintf(item, sizeof(item), "%s%02X: %llu (%u)", out.empty() ? "" : ", ", order[k],
                  static_cast<unsigned long long>(g_types[order[k]].total), g_types[order[k]].most);
    out += item;
  }
  return out.empty() ? "none" : out;
}

// Bit n of a payload (docs/MP_PROTOCOL.md 2.3: least significant bit first).
int Bit(uint8_t* base, uint32_t data, uint32_t n) { return (guest::ReadU8(base, data + (n >> 3)) >> (n & 7)) & 1; }

// The debug line every 10 s (cod3mp_debug_peer_guard), and one line a minute when something was dropped since.
void Report() {
  const auto now = std::chrono::steady_clock::now();
  if (REXCVAR_GET(cod3mp_debug_peer_guard) && now >= g_next_debug) {
    g_next_debug = now + std::chrono::seconds(10);
    REXLOG_INFO("cod3mp: peer guard: {} position updates checked, {} dropped (another console's player), {} "
                "dropped (type-checked), {} let through unchecked (couldn't be read); the read flag set back {} "
                "time(s)",
                g_counts.checked, g_counts.other, g_counts.typed, g_counts.unreadable, g_counts.reset);
    REXLOG_INFO("cod3mp: peer guard: peer messages by type (type: how many, the most in one second from one "
                "console): {}; {} not counted by console",
                RateLine(), g_untold);
  }
  if (now >= g_next_summary && (g_counts.other != g_said.other || g_counts.typed != g_said.typed)) {
    g_next_summary = now + std::chrono::seconds(60);
    REXLOG_INFO("cod3mp: peer guard: {} position update(s) dropped in all ({} for another console's player, {} "
                "type-checked)",
                g_counts.other + g_counts.typed, g_counts.other, g_counts.typed);
    g_said = g_counts;
  }
}

bool Unreadable(const char* what) {
  ++g_counts.unreadable;
  if (!g_said_unreadable) {
    g_said_unreadable = true;
    REXLOG_WARN("cod3mp: peer guard: {} can't be read as expected; position updates go through unchecked", what);
  }
  return true;
}

}  // namespace

bool PeerMessageAllowed(uint8_t* base, uint32_t listener, uint32_t pair, uint32_t message) {
  const uint8_t type = guest::ReadU8(base, message + kMessageType);
  if (REXCVAR_GET(cod3mp_debug_peer_guard)) {
    CountRate(type, ValidPtr(pair) ? guest::ReadU32(base, pair + kPairFrom) : 0, std::chrono::steady_clock::now());
    Report();
  }
  if (!REXCVAR_GET(cod3mp_peer_guard)) return true;
  if (guest::ReadU8(base, kLastReadOk) == 0) {
    guest::WriteU8(base, kLastReadOk, 1);
    ++g_counts.reset;
  }
  if (type != kPositionUpdate) return true;
  Report();
  ++g_counts.checked;

  // The listener must be the player manager's MPPeer, whose slots the handler reads.
  const uint32_t holder = guest::ReadU32(base, kNetHolderPtr);
  if (!ValidPtr(holder)) return Unreadable("the network holder");
  const uint32_t manager = guest::ReadU32(base, holder);
  if (!ValidPtr(manager) || manager + kManagerPeer != listener) return Unreadable("the message's listener");
  if (!ValidPtr(pair)) return Unreadable("the message's sender");
  const uint32_t from = guest::ReadU32(base, pair + kPairFrom);
  const uint32_t buffer = guest::ReadU32(base, message + kMessagePayload);
  if (!ValidPtr(from) || !ValidPtr(buffer)) return Unreadable("the message's sender or payload");
  const uint32_t data = guest::ReadU32(base, buffer + kBufferData);
  const uint32_t size = guest::ReadU32(base, buffer + kBufferSize);
  const uint32_t written = guest::ReadU32(base, buffer + kBufferWritten);
  if (data < 0x10000 || size == 0 || size > kMaxPayloadBytes || data > 0xC0000000u - size)
    return Unreadable("the message's payload");
  const uint32_t bits = std::min<uint32_t>(written, size * 8);

  auto drop = [&](uint64_t& count, uint32_t slot, uint32_t owner) {
    ++count;
    if (g_warned < kWarnings) {
      ++g_warned;
      REXLOG_WARN("cod3mp: peer guard: a position update {} dropped (from connection {:08X}; slot {}, connection "
                  "{:08X}){}",
                  &count == &g_counts.typed ? "with a type-checked payload" : "for another console's player", from,
                  slot, owner, g_warned == kWarnings ? "; further ones are only counted" : "");
    }
    Report();
    return false;
  };
  if (bits >= 1 && Bit(base, data, 0)) return drop(g_counts.typed, kSlotCount, 0);
  if (bits < 9) return true;  // too short to name a player: the game reads the sender's own
  uint32_t slot = 0;
  for (uint32_t k = 0; k < 8; ++k) slot |= static_cast<uint32_t>(Bit(base, data, 1 + k)) << k;
  if (slot >= kSlotCount) return true;  // the game finds no player
  const uint32_t owner = guest::ReadU32(base, listener + kPeerSlots + slot * kSlotSize + kSlotConnection);
  if (owner == 0 || owner == from) return true;
  return drop(g_counts.other, slot, owner);
}

}  // namespace cod3::mp
