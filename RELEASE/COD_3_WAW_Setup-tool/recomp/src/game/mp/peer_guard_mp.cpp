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
// [memory-360] 2026-11-16 (owner 20:03: "Start working on multiplayer safety"; dev/agents/memory-360/
// MP-SAFETY-INFRA.md 3.1) Three more drops, for every type. The first two are claude-9b-think's F1 and F7 (11-03,
// COD3-rea-work/c9b-think/owner-1103/p2p-firewall-plan.md; never built: its firewall_mp.cpp sat on top of
// lag compensation drafts that didn't land), redone here on the current tree:
//  - a type outside 0x1D-0x72 (F1). The listener (MPPlayerManager::vfunc_1 0x823D8C08) looks the handler up at
//    this + 0x8210 + type * 8 and never checks the type, but the constructor (0x82405D50) clears and fills only the
//    86 entries for 0x1D-0x72 (688 bytes at + 0x82F8). Any other type reads one of the manager's other fields as a
//    handler and calls it when it isn't 0: one datagram from any console in the session could crash this one.
//    Demonware's own types (0x0A-0x0C) never get here: bdSession::accept 0x821C3510 handles them first and calls
//    the listeners only for the rest. Dropped as "consumed" (the hook returns 1), as claude-9b-think proposed:
//    nothing sends such a type, so no other listener misses one;
//  - a type-checked payload (F7), any type: its first bit set. Every bdMessage__ctor call in cod3mp passes r5 = 0
//    (COD3-rea-work/memory-360/mps/senders.txt), so no console sends one, and the handlers would read every field
//    a few bits off (acct2-main dropped it for 0x23 only);
//  - a type no console sends (memory-360's own, mps/senders.py and scan2.py, read only): all calls of
//    bdMessage__ctor 0x821B1E38 pass a constant type, none in a register, and none passes 0x26, 0x32, 0x53, 0x5A,
//    0x5E, 0x6D or 0x6E, though each has a handler. Only a changed program sends them.
// A type in the table with no handler (0x1D, and 0x70, which session.cpp's hook takes before this guard) goes on
// to the game, which already ignores it (the listener returns 0).
//
// [memory-360] 2026-11-16 Messages only the session's host may send: claude-9b-think's F9, F11 and F12 (firewall r3,
// c9bfw/firewall_mp.cpp 7cfc9e9a, with its review's items: COD3-rea-work/c9b-review/p2p-firewall-game-r3.review.md).
// Read again in the current generated code: MPPeer + 22936 is the bdSession (the 0x1E handler, 0x823E4F8C), whose
// + 8 is the host's connection (0x821B1BD0 reads + 8); on the host itself that is its own local connection (claude-
// 9b's run q-25). A connection is local when its + 8 object's byte + 56 is set (0x821AE440, the 0x1E handler's own
// test at 0x823E4E20).
//  - F11, the game-state messages 0x25, 0x35 and 0x5B (all read into the blocks 0x82680CA8 / 0x82680D58 by
//    0x823D6490, with no sender check; built only behind the host's gate): pass from a local connection, or, on a
//    console that doesn't host, from the host's connection. In claude-9b's run q-34 one 0x25 from a joiner ended
//    the host's match.
//  - F9, 0x1F (the host's answer to a join; its record's name reaches Cvar_Set2): the same rule, the local
//    connection first (review item 2), and its first field, a local player index (bits 1-8), below 4: the handler
//    indexes a 4-entry table with it (0x823F4400).
//  - F12, 0x1E (a join request; its handler 0x823E4D80 runs on any console and checks nothing): only on the
//    console that hosts, while its game state says at most 24 players (+ 91 bounds the handler's slot search), its
//    local index (bits 1-8) below 4, from a console with fewer than 4 players here, at most 8 per console in 10 s.
//  - [memory-360] 0x72 kind 5, the lobby's "start the match" (memory-360's own reading, 21:10): 0x72 is a lobby
//    message whose first byte (bits 1-8) is a kind, 1-11 (its handler 0x823FF020). Kind 5's builder 0x823D1920
//    sends it to everyone (MPPeer__SendAll) and starts the map itself when the lobby's host flag
//    (*(0x82BDCB00) + 0x3C644) is set, and takes the joiner's path otherwise; the handler ignores kind 5 when that
//    flag is set and otherwise calls 0x823D1C40: XSessionStart, then 0x823D1920 -> 0x823D1818 / 0x823D1620 ->
//    0x823FD4F8 -> 0x823FD238, the "map" / "map_restart" (or "exitgame", 0x823FBA08) of RC2-MPSAFE N1. So kind 5
//    comes from the host only, and is dropped unless F11's rule passes it. When this console can't tell the
//    host yet (no host connection), it goes on and that is logged once. The other kinds are guests' too (kind 1
//    from the lobby menus, kind 4 from XenonLiveLobby, kind 8 and its reply 9) and keep going; the test log counts
//    0x72 by kind.
// [memory-360] 2026-11-16 21:40 0x28, the hit (handler 0x823FFEB0), claude-9b-think's F2, F3 and F4 for it (firewall
// r3: GateSender, GateHit and the impact-effect hook), checked here, before the handler runs. claude-9b checked at a
// gate inside the handler (0x823DAF20) and hooked the impact effect, because "the weapon's bit position isn't known"
// to a listener: the point's ranges were data. They are mapped now (MSGBB_ReadOrigin 0x823D7FF8: three ranged floats
// in -8191..8191 at precision 1, 14 bits each; engine/crates/cod3_game/src/net/fields.rs COORD_MAX), and the
// handler's reads (sub_823D7378, 0x823D7FF8, 0x823D7C90, 0x820D9B88 x2, 0x820D9BF8, 0x820D9B18 x3, 0x823D7378) are the
// builder's writes (0x823E9998) in the same order. With no type tags (the header bit is checked above), from bit 1:
// target slot (ranged 0-24: 5 bits, read clamped to 24), point (42), direction (8), a byte (8), weapon (s8), damage
// (s16), dflags (8), means of death (8), hit location (8), attacker slot (5): 117 bits with the header bit. Dropped:
//  - shorter than that: every read must work or the handler stops, but a failed last read (the attacker) leaves it
//    no attacker entity and it reads *(0 + 512) (0x82400274);
//  - the sender's player (0x823DCF70: the first slot on the message's connection) without an entity with a gclient
//    (0x824001E8 reads *(0 + 484)); none at all is the handler's own exit and goes on;
//  - an attacker slot that isn't a player in use with such an entity, or is another console's (no console relays a
//    0x28: only Bullet_Fire's 0x823E9998 builds one);
//  - a weapon the game can't use (0..*(0x82BCBF94) with a def at *(0x82BCBF9C) + 4 x weapon): 0x8246A228 calls
//    CG_Error above the highest one and the impact effect 0x8246B7A8 reads the def's + 192 unchecked (weapon -2 or 127
//    crashed the receiver); no check while the table isn't there, as claude-9b had it;
//  - with a target slot below 24: a target that isn't one of this console's players (the shooter sends a 0x28 only to
//    the target's console) or has no entity (G_Damage(0, ...) reads *(0 + 608)), G_Damage's weapon (the message's
//    above 0, else the attacker entity's + 3) unusable, damage over 1000, a hit location of 19 or more (G_Damage
//    indexes a 19-float table unchecked).
// claude-9b's logs (damage over the weapon's cap, below 1, means of death of 24 or more) stay out. The test log
// prints the first 8 hits as read here and counts hits of exactly 117 bits and longer ones.
//
// [memory-360] 2026-11-16 21:50 A test of the drops themselves (cod3mp_test_peer_guard_forge, empty = off): every
// 40th position update from another console is rewritten, before the guard sees it, into a message no honest
// console sends (a type outside the table, a never-sent type, a type-checked payload, another console's player, a
// hit with weapon 127 / another console's attacker / too short, and by role a lobby start or game state from a
// guest on the host, a join request on a console that doesn't host). Each forgery is logged with what the guard
// did; one let through is logged as an error. Only while cod3mp_peer_guard is on, so nothing forged reaches the
// game unless the guard fails; the update it replaces is one of about 20 a second.
//
// [memory-360] 2026-11-16 22:15 Two more of claude-9b-think's checks:
//  - 0x2D, G_Damage's message for damage to another console's player (builder 0x823E8A50, handler 0x823FC5B8; F8),
//    at the listener like 0x28. Read in the generated code, the handler's reads are the builder's writes in order:
//    target slot (ranged 0-24, 5 bits), a bool (1 bit), the attacker slot (5 bits) only when the bool is set, point
//    (42), direction (8), damage (s16), means of death (8), dflags (s16), hit location (8), weapon (s8): 118 bits with
//    the header bit, or 113 without an attacker. Every read must work or the handler skips G_Damage. Dropped: a
//    target slot below 24 that isn't one of this console's players, a hit location of 19 or more, and G_Damage's
//    weapon unusable (the message's above 0, else the inflictor's + 3: the attacker slot's player's entity when there
//    is one, else the sender's). claude-9b's log-only items (an attacker that isn't the sender's player: explosives
//    and vehicles were not traced; damage below 1 or over 1000) are only counted, in the test log.
//  - 0x23's weapon (F4; firewall r1's PositionWeapon): ReadPositionUpdate 0x823F78B0 reads it with the s8 reader
//    0x820D9B88 into its frame + 80 (return addresses 0x823F7AA8 and 0x823F7F8C; r30 its player) and, on a change,
//    sets the entity's, gclient's and player's weapon and calls 0x8245E720; 255 kicked every receiver through
//    0x8246A228's CG_Error. A hook on 0x820D9B88 (one compare for every other caller) replaces an unusable weapon, as
//    read, by the player's current one (+ 208), so the update applies without a weapon change. The forgery test
//    (pos_weapon) writes 127 there every 40th read.
// Not ported yet: F13 (the game-state reader's ranges; its review's + 89 item goes with it), F8/F10 (0x2D, the setup
// records, 0x61). The first 0x1E or game-state message logs this console's host test once (review item 3), for the
// test match.
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
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3mp_peer_guard, true, "cod3/Multiplayer",
                    "Drop a message from another console that moves a player who isn't that console's (a position "
                    "update for someone else's player), of a type outside the game's handler table (it would crash "
                    "this one) or no console sends, type-checked, a game state or join answer not from the host, "
                    "a join request the host shouldn't take, or a hit no honest console sends (another console's "
                    "attacker, a weapon that doesn't exist, damage over 1000): the 2006 game doesn't check them. "
                    "Applies at once")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3mp_test_peer_guard_forge, "", "cod3",
                      "Tests only: every 40th position update from another console is rewritten into a message no "
                      "honest console sends, to see the peer guard drop it. A comma list of outside, never, typed, "
                      "other, hit_weapon, hit_attacker, hit_short, dmg_weapon, pos_weapon, start, state, join (start "
                      "and state on the host only, join on a console that doesn't host), or all. Only while "
                      "cod3mp_peer_guard is on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
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

// [memory-360] 2026-11-16 The listener's table covers 0x1D-0x72 (see the top).
constexpr uint8_t kTableFirst = 0x1D, kTableLast = 0x72;

// [memory-360] 2026-11-16 A type in the table that no console's code makes (see the top).
bool NeverSent(uint8_t type) {
  switch (type) {
    case 0x26: case 0x32: case 0x53: case 0x5A: case 0x5E: case 0x6D: case 0x6E:
      return true;
    default:
      return false;
  }
}

// [memory-360] 2026-11-16 F9, F11, F12 (see the top; claude-9b-think's firewall r3).
constexpr uint8_t kTypeJoin = 0x1E, kTypeSetup = 0x1F, kTypeGameState = 0x25, kTypeState35 = 0x35,
                  kTypeState5B = 0x5B;
constexpr uint32_t kPeerSession = 22936, kSessionHostConn = 8, kSessionStatus = 64;
constexpr uint32_t kConnHolder = 8, kHolderLocal = 56;
constexpr uint32_t kGameState = 0x82680CA8, kStateMaxPlayers = 91;
constexpr uint32_t kLocalPlayers = 4;
constexpr int64_t kJoinWindowMs = 10000;
constexpr int kJoinMax = 8, kJoinConsoles = 16;
constexpr uint8_t kTypeLobby = 0x72;
constexpr uint32_t kLobbyStart = 5, kLobbyKinds = 12;  // 0x72's kind byte, 1-11
// [memory-360] 0x28 (see the top).
constexpr uint8_t kTypeHit = 0x28;
constexpr uint32_t kHitTargetBit = 1, kHitWeaponBit = 64, kHitDamageBit = 72, kHitLocBit = 104, kHitAttackerBit = 112;
constexpr uint32_t kHitSlotBits = 5, kHitBits = 117;
constexpr uint32_t kSlotClient = 8;           // s32, the player's client (below 0: none)
constexpr uint32_t kPeerLocalSlots = 22929;   // MPPeer + it + local index: this console's slots (0x823D8FF0)
constexpr uint32_t kEntities = 0x82BCC7FC;    // *(it) + 4 x (client + 1): the entity
constexpr int32_t kMaxClient = 64;            // a sanity bound on client numbers read
constexpr uint32_t kEntClient = 512;          // the entity's gclient; the 0x28 handler writes its + 1444
constexpr uint32_t kEntWeapon = 3;            // u8, the entity's weapon (G_Damage's fallback, 0x825864C0)
constexpr uint32_t kWeaponDefs = 0x82BCBF9C;  // *(it) + 4 x weapon: the def
constexpr uint32_t kWeaponMax = 0x82BCBF94;   // s32: the highest weapon index (0x8246A228's CG_Error bound)
constexpr int32_t kWeaponLimit = 255;         // a sanity bound on it
constexpr int32_t kDamageCeiling = 1000;
constexpr uint32_t kHitLocations = 19;        // G_Damage's multiplier table 0x82B2C268 (0x82560858 fills 19)
constexpr int kHitsLogged = 8;
// [memory-360] 0x2D and 0x23's weapon (see the top).
constexpr uint8_t kTypeDamage = 0x2D;
constexpr uint32_t kDmgHasAttackerBit = 6, kDmgAttackerBit = 7;
constexpr uint32_t kDmgDamageOff = 50, kDmgLocOff = 90, kDmgWeaponOff = 98, kDmgEndOff = 106;  // from the point
constexpr uint32_t kPlayerWeapon = 208;     // s32, the player's current weapon (ReadPositionUpdate)
constexpr uint32_t kRetPosWeapon1 = 0x823F7AA8, kRetPosWeapon2 = 0x823F7F8C;

// [memory-360] 2026-11-17 r6: the remaining crashers the handler audit found (MP-SAFETY-INFRA.md 3.7).
// Four handlers index weaponDefs from an untrusted s8 weapon with no bound (sub_8246B8E0 / sub_8246A228:
// weaponDefs[weapon] -> +192), a remote crash. The weapon's bit position in each payload (header bit 0
// excluded), from the handler's own field reads.
constexpr uint8_t kTypeWeapon27 = 0x27, kTypeWeapon2A = 0x2A, kTypeWeapon4D = 0x4D, kTypeWeapon67 = 0x67;
constexpr uint32_t kW27Bit = 59, kW2ABit = 6, kW4DBit = 43, kW67Bit = 48;
// 0x46 (sub_823E3298): an int32 index at bit 1 used as MPPeer + ((idx + 8571) << 2) <- a small constant,
// an arbitrary relative 4-byte write. The array is based at +34284 and its ctor fields at +34504/+34508
// bound it to (34504 - 34284) / 4 = 55 entries, so a valid index is 0 <= idx < 55.
constexpr uint8_t kTypeWrite46 = 0x46;
constexpr uint32_t kWriteIdxBit = 1;
constexpr int32_t kWriteSlots = 55;

struct Counts {
  uint64_t checked = 0;     // position updates looked at
  uint64_t other = 0;       // dropped: the player is another console's
  uint64_t typed = 0;       // dropped: a type-checked payload
  uint64_t unreadable = 0;  // let through: the objects couldn't be read
  uint64_t reset = 0;       // the read flag was 0 before a message, set back to 1
  uint64_t outside = 0;     // [memory-360] dropped: a type outside the listener's table
  uint64_t never_sent = 0;  // [memory-360] dropped: a type no console sends
  uint64_t typed_all = 0;   // [memory-360] dropped: a type-checked payload of another type than 0x23
  uint64_t not_host = 0;    // [memory-360] dropped: 0x1F, 0x25, 0x35 or 0x5B not from the session's host (F9, F11)
  uint64_t joins = 0;       // [memory-360] dropped: a 0x1E (F12)
  uint64_t index = 0;       // [memory-360] dropped: 0x1F with a local player index of 4 or more (F9)
  uint64_t lobby_start = 0; // [memory-360] dropped: 0x72 kind 5 not from the session's host
  uint64_t hits = 0;        // [memory-360] dropped: a 0x28 (see the top)
  uint64_t hit_whole = 0;   // [memory-360] 0x28s of exactly 117 bits read here (the test log)
  uint64_t hit_longer = 0;  // [memory-360] 0x28s longer than that
  uint64_t damage = 0;      // [memory-360] dropped: a 0x2D (see the top)
  uint64_t dmg_other = 0;   // [memory-360] 0x2Ds naming an attacker that isn't the sender's (counted only)
  uint64_t dmg_odd = 0;     // [memory-360] 0x2Ds with damage below 1 or over 1000 (counted only)
  uint64_t pos_weapon = 0;  // [memory-360] 0x23 weapons replaced by the player's current one
  uint64_t weapon_oob = 0;  // [memory-360] r6 dropped: 0x27/0x2A/0x4D/0x67 with a weapon the game can't look up
  uint64_t write_oob = 0;   // [memory-360] r6 dropped: 0x46 with an out-of-range write index
};
int g_hits_logged = 0;  // [memory-360] the test log's first hits
int g_damage_logged = 0;  // [memory-360] the test log's first 0x2Ds
uint64_t g_lobby_kinds[kLobbyKinds + 1] = {};  // [memory-360] the test log's 0x72 by kind (the last: other values)
bool g_said_lobby_unknown_host = false;
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
                "time(s); dropped of other types: {} outside the handler table, {} of a type no console sends, {} "
                "type-checked, {} not from the session's host, {} join requests, {} 0x1F with a bad index, {} lobby "
                "starts not from the host, {} hits (0x28; read here: {} of 117 bits, {} longer), {} damage messages "
                "(0x2D; counted only: {} naming another console's attacker, {} with damage below 1 or over 1000); {} "
                "position update weapons replaced; {} weapon-index crashers (0x27/0x2A/0x4D/0x67), {} out-of-bounds "
                "writes (0x46)",
                g_counts.checked, g_counts.other, g_counts.typed, g_counts.unreadable, g_counts.reset,
                g_counts.outside, g_counts.never_sent, g_counts.typed_all, g_counts.not_host, g_counts.joins,
                g_counts.index, g_counts.lobby_start, g_counts.hits, g_counts.hit_whole, g_counts.hit_longer,
                g_counts.damage, g_counts.dmg_other, g_counts.dmg_odd, g_counts.pos_weapon, g_counts.weapon_oob,
                g_counts.write_oob);
    REXLOG_INFO("cod3mp: peer guard: peer messages by type (type: how many, the most in one second from one "
                "console): {}; {} not counted by console",
                RateLine(), g_untold);
    std::string kinds;  // [memory-360] 0x72 by kind
    for (uint32_t k = 0; k <= kLobbyKinds; ++k)
      if (g_lobby_kinds[k])
        kinds += (kinds.empty() ? "" : ", ") + std::to_string(k) + ": " + std::to_string(g_lobby_kinds[k]);
    if (!kinds.empty()) REXLOG_INFO("cod3mp: peer guard: lobby messages (0x72) by kind: {}", kinds);
  }
  if (now >= g_next_summary &&
      (g_counts.other != g_said.other || g_counts.typed != g_said.typed || g_counts.outside != g_said.outside ||
       g_counts.never_sent != g_said.never_sent || g_counts.typed_all != g_said.typed_all ||
       g_counts.not_host != g_said.not_host || g_counts.joins != g_said.joins || g_counts.index != g_said.index ||
       g_counts.lobby_start != g_said.lobby_start || g_counts.hits != g_said.hits ||
       g_counts.damage != g_said.damage || g_counts.pos_weapon != g_said.pos_weapon ||
       g_counts.weapon_oob != g_said.weapon_oob || g_counts.write_oob != g_said.write_oob)) {
    g_next_summary = now + std::chrono::seconds(60);
    REXLOG_INFO("cod3mp: peer guard: {} position update(s) dropped in all ({} for another console's player, {} "
                "type-checked); other messages dropped: {} outside the handler table, {} of a type no console "
                "sends, {} type-checked, {} not from the session's host, {} join requests, {} 0x1F with a bad index, "
                "{} lobby starts not from the host, {} hits, {} damage messages; {} position update weapons "
                "replaced; {} weapon-index crashers, {} out-of-bounds writes",
                g_counts.other + g_counts.typed, g_counts.other, g_counts.typed, g_counts.outside,
                g_counts.never_sent, g_counts.typed_all, g_counts.not_host, g_counts.joins, g_counts.index,
                g_counts.lobby_start, g_counts.hits, g_counts.damage, g_counts.pos_weapon, g_counts.weapon_oob,
                g_counts.write_oob);
    g_said = g_counts;
  }
}

// [memory-360] 2026-11-16 Helpers for F9, F11 and F12, as claude-9b-think wrote them (firewall r3: LocalConnection,
// HostConn, HostsSession, FromSessionHost), on the guard's own reads.

// N bits of a message's payload from bit `pos` (least significant first), if the payload holds them.
bool PayloadBits(uint8_t* base, uint32_t message, uint32_t pos, uint32_t n, uint32_t* out) {
  const uint32_t buffer = guest::ReadU32(base, message + kMessagePayload);
  if (!ValidPtr(buffer)) return false;
  const uint32_t data = guest::ReadU32(base, buffer + kBufferData);
  const uint32_t size = guest::ReadU32(base, buffer + kBufferSize);
  const uint32_t written = guest::ReadU32(base, buffer + kBufferWritten);
  if (data < 0x10000 || size == 0 || size > kMaxPayloadBytes || data > 0xC0000000u - size) return false;
  if (pos + n > std::min<uint32_t>(written, size * 8)) return false;
  uint32_t v = 0;
  for (uint32_t k = 0; k < n; ++k) v |= static_cast<uint32_t>(Bit(base, data, pos + k)) << k;
  *out = v;
  return true;
}

// [memory-360] How many bits a message's payload holds, if it can be read.
bool PayloadLength(uint8_t* base, uint32_t message, uint32_t* out) {
  const uint32_t buffer = guest::ReadU32(base, message + kMessagePayload);
  if (!ValidPtr(buffer)) return false;
  const uint32_t data = guest::ReadU32(base, buffer + kBufferData);
  const uint32_t size = guest::ReadU32(base, buffer + kBufferSize);
  const uint32_t written = guest::ReadU32(base, buffer + kBufferWritten);
  if (data < 0x10000 || size == 0 || size > kMaxPayloadBytes || data > 0xC0000000u - size) return false;
  *out = std::min<uint32_t>(written, size * 8);
  return true;
}

// A connection of this console's own (its + 8 object's byte + 56, 0x821AE440).
bool LocalConnection(uint8_t* base, uint32_t conn) {
  if (!ValidPtr(conn)) return false;
  const uint32_t holder = guest::ReadU32(base, conn + kConnHolder);
  return ValidPtr(holder) && guest::ReadU8(base, holder + kHolderLocal) != 0;
}

uint32_t Session(uint8_t* base, uint32_t peer) {
  const uint32_t session = guest::ReadU32(base, peer + kPeerSession);
  return ValidPtr(session) ? session : 0;
}

uint32_t HostConn(uint8_t* base, uint32_t peer) {
  const uint32_t session = Session(base, peer);
  return session ? guest::ReadU32(base, session + kSessionHostConn) : 0;
}

// This console hosts the session: it has one, and its host connection is 0 or one of its own.
bool HostsSession(uint8_t* base, uint32_t peer) {
  if (!Session(base, peer)) return false;
  const uint32_t host = HostConn(base, peer);
  return host == 0 || LocalConnection(base, host);
}

// F9 / F11: from this console's own connection, or, on a console that doesn't host, from the host's connection.
bool FromSessionHost(uint8_t* base, uint32_t peer, uint32_t conn) {
  if (LocalConnection(base, conn)) return true;
  if (HostsSession(base, peer)) return false;
  const uint32_t host = HostConn(base, peer);
  return host != 0 && conn == host;
}

// [memory-360] 0x28 (see the top). Helpers as claude-9b-think's firewall r3 has them (EntityOfClient, UsableEntity,
// WeaponUsable, LocalSlot), on the guard's own reads.
uint32_t EntityOfClient(uint8_t* base, int32_t client) {
  if (client < 0 || client >= kMaxClient) return 0;
  const uint32_t table = guest::ReadU32(base, kEntities);
  if (!ValidPtr(table)) return 0;
  const uint32_t ent = guest::ReadU32(base, table + 4 * static_cast<uint32_t>(client + 1));
  return ValidPtr(ent) ? ent : 0;
}

// An entity the 0x28 handler can write through (its gclient + 1444), else 0.
uint32_t UsableEntity(uint8_t* base, uint32_t ent) {
  return ValidPtr(ent) && ValidPtr(guest::ReadU32(base, ent + kEntClient)) ? ent : 0;
}

// A weapon the game can use: 0..*(kWeaponMax) with a def. True while the table or its count isn't there (no match:
// no weapon is looked up then).
bool WeaponUsable(uint8_t* base, int32_t weapon) {
  const uint32_t defs = guest::ReadU32(base, kWeaponDefs);
  const int32_t max = guest::ReadS32(base, kWeaponMax);
  if (!ValidPtr(defs) || max <= 0 || max > kWeaponLimit) return true;
  if (weapon < 0 || weapon > max) return false;
  return ValidPtr(guest::ReadU32(base, defs + 4 * static_cast<uint32_t>(weapon)));
}

bool LocalSlot(uint8_t* base, uint32_t peer, uint32_t slot) {
  for (uint32_t k = 0; k < kLocalPlayers; ++k)
    if (guest::ReadU8(base, peer + kPeerLocalSlots + k) == slot) return true;
  return false;
}

uint32_t PlayerEntity(uint8_t* base, uint32_t player) {
  return UsableEntity(base, EntityOfClient(base, guest::ReadS32(base, player + kSlotClient)));
}

// Why a 0x28 from `conn` is dropped, or nullptr. `peer` is the player manager's MPPeer.
const char* HitProblem(uint8_t* base, uint32_t peer, uint32_t conn, uint32_t message) {
  uint32_t length = 0;
  if (!PayloadLength(base, message, &length) || length == 0) return nullptr;  // the handler's own reads decide
  if (length < kHitBits) return "a hit shorter than the 117 bits a console writes (the handler would have no attacker)";
  ++(length == kHitBits ? g_counts.hit_whole : g_counts.hit_longer);
  auto field = [&](uint32_t pos, uint32_t n) {
    uint32_t v = 0;
    PayloadBits(base, message, pos, n, &v);  // inside the length checked above
    return v;
  };
  const uint32_t target = std::min<uint32_t>(field(kHitTargetBit, kHitSlotBits), kSlotCount);
  const int32_t weapon = static_cast<int8_t>(field(kHitWeaponBit, 8));
  const int32_t damage = static_cast<int16_t>(field(kHitDamageBit, 16));
  const uint32_t loc = field(kHitLocBit, 8);
  const uint32_t attacker = std::min<uint32_t>(field(kHitAttackerBit, kHitSlotBits), kSlotCount);
  if (REXCVAR_GET(cod3mp_debug_peer_guard) && g_hits_logged < kHitsLogged) {
    ++g_hits_logged;
    REXLOG_INFO("cod3mp: peer guard: hit {} as read here: target slot {}, attacker slot {}, weapon {}, damage {}, "
                "hit location {} ({} bits, from connection {:08X})",
                g_hits_logged, target, attacker, weapon, damage, loc, length, conn);
  }
  uint32_t sender = 0;  // 0x823DCF70
  for (uint32_t s = 0; s < kSlotCount && !sender; ++s)
    if (guest::ReadU32(base, peer + kPeerSlots + s * kSlotSize + kSlotConnection) == conn)
      sender = peer + kPeerSlots + s * kSlotSize;
  if (!sender) return nullptr;  // the handler's own "unknown sender" exit
  if (!PlayerEntity(base, sender)) return "the sender's player has no entity (the handler would read a null one)";
  const uint32_t ap = attacker < kSlotCount ? peer + kPeerSlots + attacker * kSlotSize : 0;
  const uint32_t aent = ap && guest::ReadU8(base, ap) < kSlotCount ? PlayerEntity(base, ap) : 0;
  if (!aent) return "the attacker isn't a player with an entity (the handler would write through a null one)";
  if (guest::ReadU32(base, ap + kSlotConnection) != conn) return "the attacker is another console's player";
  if (!WeaponUsable(base, weapon)) return "a weapon the game can't look up (it would end the match or crash)";
  if (target >= kSlotCount) return nullptr;  // no target: the handler's own branch, no G_Damage
  const uint32_t tp = peer + kPeerSlots + target * kSlotSize;
  if (!LocalSlot(base, peer, target)) return "the target isn't one of this console's players";
  if (!EntityOfClient(base, guest::ReadS32(base, tp + kSlotClient)))
    return "the target has no entity (G_Damage would read a null one)";
  const int32_t used = weapon > 0 ? weapon : guest::ReadU8(base, aent + kEntWeapon);
  if (used > 0 && !WeaponUsable(base, used)) return "G_Damage's weapon (the attacker's) can't be looked up";
  if (damage > kDamageCeiling) return "damage over 1000 (no bullet sends that much)";
  if (loc >= kHitLocations) return "a hit location of 19 or more (G_Damage's table has 19)";
  return nullptr;
}

// [memory-360] Why a 0x2D from `conn` is dropped, or nullptr (see the top). `peer` is the player manager's MPPeer.
const char* DamageProblem(uint8_t* base, uint32_t peer, uint32_t conn, uint32_t message) {
  uint32_t length = 0;
  if (!PayloadLength(base, message, &length) || length <= kDmgHasAttackerBit) return nullptr;  // no G_Damage
  auto field = [&](uint32_t pos, uint32_t n) {
    uint32_t v = 0;
    PayloadBits(base, message, pos, n, &v);
    return v;
  };
  const bool has_attacker = field(kDmgHasAttackerBit, 1) != 0;
  const uint32_t point = has_attacker ? kDmgAttackerBit + kHitSlotBits : kDmgAttackerBit;
  if (length < point + kDmgEndOff) return nullptr;  // a read fails: the handler skips G_Damage
  const uint32_t target = std::min<uint32_t>(field(kHitTargetBit, kHitSlotBits), kSlotCount);
  const uint32_t attacker =
      has_attacker ? std::min<uint32_t>(field(kDmgAttackerBit, kHitSlotBits), kSlotCount) : kSlotCount;
  const int32_t damage = static_cast<int16_t>(field(point + kDmgDamageOff, 16));
  const uint32_t loc = field(point + kDmgLocOff, 8);
  const int32_t weapon = static_cast<int8_t>(field(point + kDmgWeaponOff, 8));
  if (REXCVAR_GET(cod3mp_debug_peer_guard) && g_damage_logged < kHitsLogged) {
    ++g_damage_logged;
    REXLOG_INFO("cod3mp: peer guard: damage message {} as read here: target slot {}, attacker slot {}, weapon {}, "
                "damage {}, hit location {} ({} bits, from connection {:08X})",
                g_damage_logged, target, attacker, weapon, damage, loc, length, conn);
  }
  if (target >= kSlotCount) return nullptr;  // no target player: no G_Damage
  if (!LocalSlot(base, peer, target)) return "the target isn't one of this console's players";
  if (loc >= kHitLocations) return "a hit location of 19 or more (G_Damage's table has 19)";
  if (!WeaponUsable(base, weapon)) return "a weapon the game can't look up (it would end the match or crash)";
  uint32_t inflictor = 0;  // the attacker slot's player's entity, else the sender's (0x823FC848-0x823FC8AC)
  if (attacker < kSlotCount) {
    const uint32_t ap = peer + kPeerSlots + attacker * kSlotSize;
    if (guest::ReadU8(base, ap) < kSlotCount) inflictor = EntityOfClient(base, guest::ReadS32(base, ap + kSlotClient));
    if (guest::ReadU32(base, ap + kSlotConnection) != conn) ++g_counts.dmg_other;
  }
  if (!inflictor) {
    for (uint32_t s = 0; s < kSlotCount; ++s) {  // 0x823DCF70
      const uint32_t sp = peer + kPeerSlots + s * kSlotSize;
      if (guest::ReadU32(base, sp + kSlotConnection) != conn) continue;
      inflictor = EntityOfClient(base, guest::ReadS32(base, sp + kSlotClient));
      break;
    }
  }
  const int32_t used = weapon > 0 ? weapon : (inflictor ? guest::ReadU8(base, inflictor + kEntWeapon) : 0);
  if (used > 0 && !WeaponUsable(base, used)) return "G_Damage's weapon (the inflictor's) can't be looked up";
  if (damage < 1 || damage > kDamageCeiling) ++g_counts.dmg_odd;
  return nullptr;
}

// [memory-360] r6: the weapon field of 0x27/0x2A/0x4D/0x67 must be a weapon the game can look up, or the
// handler's weaponDefs[weapon] -> +192 load runs out of bounds (a remote crash). An unreadable field is
// let through: the handler's own read would fail the same safe way, and a real console never sends one.
const char* WeaponIndexProblem(uint8_t* base, uint32_t message, uint32_t bit) {
  uint32_t v = 0;
  if (!PayloadBits(base, message, bit, 8, &v)) return nullptr;
  const int32_t weapon = static_cast<int8_t>(v);
  return WeaponUsable(base, weapon) ? nullptr : "a weapon the game can't look up (it would crash the client)";
}

// [memory-360] r6: 0x46's write index must land inside the array (0 <= idx < 55), or the handler writes a
// small constant to MPPeer + ((idx + 8571) << 2) -- an arbitrary relative 4-byte write.
const char* WriteIndexProblem(uint8_t* base, uint32_t message) {
  uint32_t v = 0;
  if (!PayloadBits(base, message, kWriteIdxBit, 32, &v)) return nullptr;
  const int32_t idx = static_cast<int32_t>(v);
  if (idx < 0 || idx >= kWriteSlots) return "a write index outside the array (an out-of-bounds write)";
  return nullptr;
}

// [memory-360] The drops' own test (cod3mp_test_peer_guard_forge, see the top).
constexpr int kForgeEvery = 40;
struct Forged {
  const char* kind = nullptr;
  uint32_t bits = 0;
};
std::vector<std::string> g_forge_kinds;
bool g_forge_parsed = false;
uint64_t g_forge_seen = 0, g_forge_next = 0, g_forge_done = 0, g_forge_dropped = 0, g_forge_through = 0;

void SetPayloadBits(uint8_t* base, uint32_t message, uint32_t pos, uint32_t n, uint32_t value) {
  const uint32_t buffer = guest::ReadU32(base, message + kMessagePayload);
  const uint32_t data = guest::ReadU32(base, buffer + kBufferData);
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t at = data + ((pos + k) >> 3);
    const uint8_t mask = static_cast<uint8_t>(1u << ((pos + k) & 7));
    const uint8_t old = guest::ReadU8(base, at);
    guest::WriteU8(base, at, (value >> k) & 1 ? (old | mask) : (old & ~mask));
  }
}

// Rewrites a position update from another console when its turn comes; what it became, or no kind.
Forged MaybeForge(uint8_t* base, uint32_t listener, uint32_t pair, uint32_t message) {
  Forged f;
  const std::string& setting = REXCVAR_GET(cod3mp_test_peer_guard_forge);
  if (setting.empty() || !REXCVAR_GET(cod3mp_peer_guard)) return f;
  if (guest::ReadU8(base, message + kMessageType) != kPositionUpdate || !ValidPtr(pair)) return f;
  const uint32_t holder = guest::ReadU32(base, kNetHolderPtr);
  const uint32_t manager = ValidPtr(holder) ? guest::ReadU32(base, holder) : 0;
  if (!ValidPtr(manager) || manager + kManagerPeer != listener) return f;
  const uint32_t conn = guest::ReadU32(base, pair + kPairFrom);
  if (!ValidPtr(conn) || LocalConnection(base, conn)) return f;
  if (!g_forge_parsed) {
    g_forge_parsed = true;
    const std::string all = setting == "all" ? "outside,never,typed,other,hit_weapon,hit_attacker,hit_short,"
                                               "dmg_weapon,start,state,join,pos_weapon"
                                             : setting;
    size_t at = 0;
    while (at <= all.size()) {
      const size_t comma = std::min(all.find(',', at), all.size());
      if (comma > at) g_forge_kinds.push_back(all.substr(at, comma - at));
      at = comma + 1;
    }
  }
  if (g_forge_kinds.empty() || ++g_forge_seen % kForgeEvery != 0) return f;
  uint32_t length = 0;
  if (!PayloadLength(base, message, &length) || length < 9) return f;
  const bool hosts = HostsSession(base, listener);
  const uint32_t local = guest::ReadU8(base, listener + kPeerLocalSlots);
  uint32_t own = kSlotCount;  // the sender's first slot (0x823DCF70)
  for (uint32_t s = 0; s < kSlotCount && own == kSlotCount; ++s)
    if (guest::ReadU32(base, listener + kPeerSlots + s * kSlotSize + kSlotConnection) == conn) own = s;
  for (size_t tries = 0; tries < g_forge_kinds.size(); ++tries) {
    const std::string& k = g_forge_kinds[g_forge_next++ % g_forge_kinds.size()];
    const bool hit = k.rfind("hit_", 0) == 0;
    if ((k == "start" || k == "state") && !hosts) continue;
    if (k == "join" && hosts) continue;
    if ((k == "other" || hit) && local >= kSlotCount) continue;
    if (k == "hit_weapon" && own >= kSlotCount) continue;
    if (k == "dmg_weapon" && (own >= kSlotCount || local >= kSlotCount ||
                              length < kDmgAttackerBit + kHitSlotBits + kDmgEndOff))
      continue;
    if (k == "pos_weapon") continue;  // done in the s8 reader's hook (see the top)
    const uint32_t buffer = guest::ReadU32(base, message + kMessagePayload);
    if (k == "outside") {
      guest::WriteU8(base, message + kMessageType, 0x80);
    } else if (k == "never") {
      guest::WriteU8(base, message + kMessageType, 0x26);
    } else if (k == "typed") {
      SetPayloadBits(base, message, 0, 1, 1);
    } else if (k == "other") {
      SetPayloadBits(base, message, 1, 8, local);
    } else if (k == "hit_weapon" || k == "hit_attacker") {
      guest::WriteU8(base, message + kMessageType, kTypeHit);
      if (length >= kHitBits) {
        SetPayloadBits(base, message, kHitTargetBit, kHitSlotBits, local);
        SetPayloadBits(base, message, kHitWeaponBit, 8, 127);
        SetPayloadBits(base, message, kHitLocBit, 8, 200);  // a second reason, should weapon 127 exist
        SetPayloadBits(base, message, kHitAttackerBit, kHitSlotBits, k == "hit_weapon" ? own : local);
      }
    } else if (k == "hit_short") {
      guest::WriteU8(base, message + kMessageType, kTypeHit);
      if (length >= kHitBits) guest::WriteU32(base, buffer + kBufferWritten, kHitBits - 1);
    } else if (k == "dmg_weapon") {
      const uint32_t point = kDmgAttackerBit + kHitSlotBits;
      guest::WriteU8(base, message + kMessageType, kTypeDamage);
      SetPayloadBits(base, message, kHitTargetBit, kHitSlotBits, local);
      SetPayloadBits(base, message, kDmgHasAttackerBit, 1, 1);
      SetPayloadBits(base, message, kDmgAttackerBit, kHitSlotBits, own);
      SetPayloadBits(base, message, point + kDmgWeaponOff, 8, 127);
      SetPayloadBits(base, message, point + kDmgLocOff, 8, 200);  // a second reason, should weapon 127 exist
    } else if (k == "start") {
      guest::WriteU8(base, message + kMessageType, kTypeLobby);
      SetPayloadBits(base, message, 1, 8, kLobbyStart);
    } else if (k == "state") {
      guest::WriteU8(base, message + kMessageType, kTypeGameState);
    } else if (k == "join") {
      guest::WriteU8(base, message + kMessageType, kTypeJoin);
    } else {
      continue;  // a kind this test doesn't know
    }
    f.kind = k.c_str();
    f.bits = length;
    ++g_forge_done;
    return f;
  }
  return f;
}

// F12's per-console join count (at most kJoinMax in kJoinWindowMs).
struct JoinCount {
  uint32_t conn = 0;
  int64_t since_ms = 0;
  int count = 0;
};
JoinCount g_joins[kJoinConsoles];
bool g_said_host_test = false;

bool JoinAllowed(uint32_t conn) {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  JoinCount* c = nullptr;
  for (auto& k : g_joins)
    if (k.conn == conn) c = &k;
  if (!c) {  // a free entry, else the one counted longest ago
    c = &g_joins[0];
    for (auto& k : g_joins)
      if (k.conn == 0 || k.since_ms < c->since_ms) c = &k;
    *c = JoinCount{conn, now, 0};
  }
  if (now - c->since_ms >= kJoinWindowMs) {
    c->since_ms = now;
    c->count = 0;
  }
  if (c->count >= kJoinMax) return false;
  ++c->count;
  return true;
}

// Review item 3: this console's host test, once, for the test match's log.
void SayHostTest(uint8_t* base, uint32_t peer, uint8_t type, uint32_t conn) {
  if (g_said_host_test) return;
  g_said_host_test = true;
  const uint32_t session = Session(base, peer);
  const uint32_t host = HostConn(base, peer);
  REXLOG_INFO("cod3mp: peer guard: host test at the first {:02X} (from connection {:08X}, {}): this console {}; "
              "session {:08X}, status {}, host connection {:08X} ({})",
              type, conn, LocalConnection(base, conn) ? "local" : "remote",
              HostsSession(base, peer) ? "hosts" : "doesn't host", session,
              session ? guest::ReadU32(base, session + kSessionStatus) : 0u, host,
              host == 0 ? "none" : (LocalConnection(base, host) ? "local" : "remote"));
}

bool Unreadable(const char* what) {
  ++g_counts.unreadable;
  if (!g_said_unreadable) {
    g_said_unreadable = true;
    REXLOG_WARN("cod3mp: peer guard: {} can't be read as expected; position updates go through unchecked", what);
  }
  return true;
}

bool CheckMessage(uint8_t* base, uint32_t listener, uint32_t pair, uint32_t message);

}  // namespace

// [memory-360] The guard, with the drops' own test around it (cod3mp_test_peer_guard_forge).
bool PeerMessageAllowed(uint8_t* base, uint32_t listener, uint32_t pair, uint32_t message) {
  const Forged f = MaybeForge(base, listener, pair, message);
  const bool allowed = CheckMessage(base, listener, pair, message);
  if (f.kind) {
    ++(allowed ? g_forge_through : g_forge_dropped);
    if (allowed)
      REXLOG_ERROR("cod3mp: peer guard test: the forged '{}' (from a {}-bit position update) was let through", f.kind,
                   f.bits);
    else
      REXLOG_INFO("cod3mp: peer guard test: the forged '{}' (from a {}-bit position update) was dropped ({} forged, "
                  "{} dropped, {} let through)",
                  f.kind, f.bits, g_forge_done, g_forge_dropped, g_forge_through);
  }
  return allowed;
}

namespace {

bool CheckMessage(uint8_t* base, uint32_t listener, uint32_t pair, uint32_t message) {
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
  // [memory-360] 2026-11-16 F1, F7 and the types no console sends (see the top), for every type.
  auto drop_any = [&](uint64_t& count, const char* why) {
    ++count;
    if (g_warned < kWarnings) {
      ++g_warned;
      REXLOG_WARN("cod3mp: peer guard: a message of type {:02X} dropped (from connection {:08X}): {}{}", type,
                  ValidPtr(pair) ? guest::ReadU32(base, pair + kPairFrom) : 0u, why,
                  g_warned == kWarnings ? "; further ones are only counted" : "");
    }
    Report();
    return false;
  };
  if (type < kTableFirst || type > kTableLast) return drop_any(g_counts.outside, "outside the handler table");
  if (NeverSent(type)) return drop_any(g_counts.never_sent, "no console sends that type");
  if (type != kPositionUpdate) {
    // The payload's header bit (MP_PROTOCOL.md 2.3), read as the 0x23 check below reads it; unreadable = go on.
    const uint32_t buffer = guest::ReadU32(base, message + kMessagePayload);
    if (ValidPtr(buffer)) {
      const uint32_t data = guest::ReadU32(base, buffer + kBufferData);
      const uint32_t size = guest::ReadU32(base, buffer + kBufferSize);
      const uint32_t written = guest::ReadU32(base, buffer + kBufferWritten);
      if (data >= 0x10000 && size != 0 && size <= kMaxPayloadBytes && data <= 0xC0000000u - size && written >= 1 &&
          Bit(base, data, 0))
        return drop_any(g_counts.typed_all, "a type-checked payload, which no console sends");
    }
    // [memory-360] r6 2026-11-17 the confirmed crashers (MP-SAFETY-INFRA.md 3.7). A pure payload/weaponDefs
    // check, so it needs no listener context and never drops a real console's traffic (every legit weapon is
    // usable and every legit 0x46 index is in range).
    if (type == kTypeWrite46) {
      const char* why = WriteIndexProblem(base, message);
      return why ? drop_any(g_counts.write_oob, why) : true;
    }
    if (type == kTypeWeapon27 || type == kTypeWeapon2A || type == kTypeWeapon4D || type == kTypeWeapon67) {
      const uint32_t bit = type == kTypeWeapon27 ? kW27Bit
                         : type == kTypeWeapon2A ? kW2ABit
                         : type == kTypeWeapon4D ? kW4DBit
                                                 : kW67Bit;
      const char* why = WeaponIndexProblem(base, message, bit);
      return why ? drop_any(g_counts.weapon_oob, why) : true;
    }
    // [memory-360] 2026-11-16 21:40 0x28 (see the top). Only when the listener is the player manager's MPPeer.
    if (type == kTypeHit) {
      const uint32_t holder = guest::ReadU32(base, kNetHolderPtr);
      const uint32_t manager = ValidPtr(holder) ? guest::ReadU32(base, holder) : 0;
      if (!ValidPtr(manager) || manager + kManagerPeer != listener || !ValidPtr(pair)) return true;
      const char* why = HitProblem(base, listener, guest::ReadU32(base, pair + kPairFrom), message);
      return why ? drop_any(g_counts.hits, why) : true;
    }
    if (type == kTypeDamage) {  // [memory-360] 22:15 0x2D (see the top)
      const uint32_t holder = guest::ReadU32(base, kNetHolderPtr);
      const uint32_t manager = ValidPtr(holder) ? guest::ReadU32(base, holder) : 0;
      if (!ValidPtr(manager) || manager + kManagerPeer != listener || !ValidPtr(pair)) return true;
      const char* why = DamageProblem(base, listener, guest::ReadU32(base, pair + kPairFrom), message);
      return why ? drop_any(g_counts.damage, why) : true;
    }
    // [memory-360] 2026-11-16 F9, F11, F12 and 0x72 kind 5 (see the top). Only when the listener is the player
    // manager's MPPeer.
    uint32_t kind = 0;
    if (type == kTypeLobby) {
      if (!PayloadBits(base, message, 1, 8, &kind)) return true;
      if (REXCVAR_GET(cod3mp_debug_peer_guard)) ++g_lobby_kinds[kind < kLobbyKinds ? kind : kLobbyKinds];
      if (kind != kLobbyStart) return true;
    } else if (type != kTypeJoin && type != kTypeSetup && type != kTypeGameState && type != kTypeState35 &&
               type != kTypeState5B) {
      return true;
    }
    const uint32_t holder = guest::ReadU32(base, kNetHolderPtr);
    const uint32_t manager = ValidPtr(holder) ? guest::ReadU32(base, holder) : 0;
    if (!ValidPtr(manager) || manager + kManagerPeer != listener || !ValidPtr(pair)) return true;
    const uint32_t conn = guest::ReadU32(base, pair + kPairFrom);
    SayHostTest(base, listener, type, conn);
    if (type == kTypeLobby) {  // kind 5
      if (FromSessionHost(base, listener, conn)) return true;
      if (HostConn(base, listener) == 0) {
        if (!g_said_lobby_unknown_host) {
          g_said_lobby_unknown_host = true;
          REXLOG_WARN("cod3mp: peer guard: a lobby start (0x72 kind 5) from connection {:08X} let through: this "
                      "console has no host connection yet",
                      conn);
        }
        return true;
      }
      return drop_any(g_counts.lobby_start, "a lobby start (0x72 kind 5) not from the session's host");
    }
    if (type == kTypeSetup) {
      if (!FromSessionHost(base, listener, conn)) return drop_any(g_counts.not_host, "not from the session's host");
      uint32_t index = 0;
      if (PayloadBits(base, message, 1, 8, &index) && index >= kLocalPlayers)
        return drop_any(g_counts.index, "a local player index of 4 or more");
      return true;
    }
    if (type != kTypeJoin) {  // 0x25, 0x35, 0x5B
      if (!FromSessionHost(base, listener, conn)) return drop_any(g_counts.not_host, "not from the session's host");
      return true;
    }
    if (!HostsSession(base, listener)) return drop_any(g_counts.joins, "a join request to a console that doesn't host");
    if (guest::ReadU8(base, kGameState + kStateMaxPlayers) > kSlotCount)
      return drop_any(g_counts.joins, "a join request while the game state says over 24 players");
    uint32_t index = 0;
    if (PayloadBits(base, message, 1, 8, &index) && index >= kLocalPlayers)
      return drop_any(g_counts.joins, "a join request with a local player index of 4 or more");
    uint32_t owned = 0;
    for (uint32_t s = 0; s < kSlotCount; ++s) {
      const uint32_t slot = listener + kPeerSlots + s * kSlotSize;
      if (guest::ReadU8(base, slot) < kSlotCount && guest::ReadU32(base, slot + kSlotConnection) == conn) ++owned;
    }
    if (owned >= kLocalPlayers) return drop_any(g_counts.joins, "a join request from a console with 4 players here");
    if (!JoinAllowed(conn)) return drop_any(g_counts.joins, "join requests over 8 in 10 s from one console");
    return true;
  }
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

}  // namespace

// [memory-360] 0x23's weapon, after ReadPositionUpdate's s8 read into `out` (its frame + 80; `player` its r30).
uint64_t g_pos_weapon_reads = 0;
int g_pos_weapon_warned = 0;
void CheckPositionWeapon(uint8_t* base, uint32_t out, uint32_t player) {
  if (!REXCVAR_GET(cod3mp_peer_guard) || !ValidPtr(out & ~3u) || !ValidPtr(player)) return;
  const std::string& forge = REXCVAR_GET(cod3mp_test_peer_guard_forge);
  const bool forged = !forge.empty() && (forge == "all" || forge.find("pos_weapon") != std::string::npos) &&
                      ++g_pos_weapon_reads % kForgeEvery == 0;
  if (forged) guest::WriteU8(base, out, 127);
  const int32_t weapon = static_cast<int8_t>(guest::ReadU8(base, out));
  if (WeaponUsable(base, weapon)) {
    if (forged) REXLOG_ERROR("cod3mp: peer guard test: the forged 'pos_weapon' (weapon 127) was let through");
    return;
  }
  const int32_t current = guest::ReadS32(base, player + kPlayerWeapon);
  guest::WriteU8(base, out, static_cast<uint8_t>(current));
  ++g_counts.pos_weapon;
  if (forged) {
    ++g_forge_done;
    ++g_forge_dropped;
    REXLOG_INFO("cod3mp: peer guard test: the forged 'pos_weapon' (weapon 127) was replaced by the player's {} ({} "
                "forged, {} dropped, {} let through)",
                current, g_forge_done, g_forge_dropped, g_forge_through);
  } else if (g_pos_weapon_warned < kWarnings) {
    ++g_pos_weapon_warned;
    REXLOG_WARN("cod3mp: peer guard: a position update's weapon {} (one the game can't look up) replaced by the "
                "player's current one, {}",
                weapon, current);
  }
  Report();
}

}  // namespace cod3::mp

// [memory-360] 2026-11-16 22:15 0x820D9B88, the game's s8 reader (r3 the buffer, r4 where to): ReadPositionUpdate's
// two weapon reads only (one compare for every other caller); see the top.
REX_EXTERN(__imp__sub_820D9B88);
REX_HOOK_RAW(sub_820D9B88) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (lr != cod3::mp::kRetPosWeapon1 && lr != cod3::mp::kRetPosWeapon2) {
    __imp__sub_820D9B88(ctx, base);
    return;
  }
  const uint32_t out = ctx.r4.u32, player = ctx.r30.u32;
  __imp__sub_820D9B88(ctx, base);
  cod3::mp::CheckPositionWeapon(base, out, player);
}
