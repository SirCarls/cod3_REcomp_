// cod3mp: the match state the cheat policy asks for (game/multiplayer.h),
// answered by the multiplayer program. The campaign's game/multiplayer.cpp
// (its launch of cod3mp) isn't part of this program.
//
//  - a match runs while this machine runs a server (sv_running) or its
//    client plays in one (the client state below);
//  - every console runs its own server and only exchanges player state with
//    the others (docs/MP_PROTOCOL.md section 1), so sv_running is set on every
//    machine of a system link match. The match's host is the Demonware
//    session's host (IsHost below);
//  - nothing from another machine is ever run here.
//  - [acct2-ocr] 2026-11-07: a match no other console plays (split screen, a local match) is told apart
//    (IsLocalOnlyMatch): there the match rules (game/protected_dvars.h) are this console's own.
//  - [acct2-ocr] 2026-11-07 (owner 18:40 "sv_cheats is only for the host no cheat protected DVAR should be
//    accessed by a client other than the host. If the host toggles sv_cheats it announces it to the server.",
//    18:45 "for the announcement you'll need to have a system link match test"): the host is the console whose
//    bdSession has no host connection (a match no other console plays has none either). Only it can turn cheats
//    on (game/cheat_guard.cpp); on every other console the game's own sv_cheats 1 at each map load is turned
//    off again (game/cheat_watch.cpp, the Cvar_Set2 hook after SV_SpawnServer), and so is any other sv_cheats 1
//    seen during a match (Tick). The host's announcements ("Cheats enabled by the host") go to the other
//    consoles as a peer message of the port's own (type 0x70, "the host's notices" below), shown there as
//    plain text, never run.
//  - [acct2-ocr] 2026-11-07 (docs/GAMEPLAY_PATCHES.md item 1): the same messages carry the host's match rules
//    (game/protected_dvars.h: ServerInfoPairs on the host, ApplyServerInfo on the others, LeaveMatch when their
//    match ends), so every console of a system link match plays by the host's rules.
//  - [acct2-ocr] 2026-11-14 (acct2-main 15:40): those notices are taken from the host's connection only, or from
//    the session's only connection (FindSender, OnNotice), so another guest can't set a match's rules.
//  - [waw-decomp] 2026-11-16 (MP safety review rc2-mpsafe; acct2-main's pg1 run of 11-14: every joiner dropped the
//    host's notices): the host's connection is the bdSession's +8, the connection object bdSession::sendHost
//    (0x821BF728) sends to and setHost (0x821C0EF8) sets; a notice is taken when it came on that object. The +60
//    compared before is a count or 1-based mark (1 on a joiner whose host is list entry 0), not the list's index.

#include "game/mp/session.h"
#include "game/mp/peer_guard_mp.h"  // [acct2-main] 2026-11-14

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/cheat_guard.h"
#include "game/cheat_watch.h"
#include "game/engine.h"
#include "game/game_api.h"
#include "game/guest.h"
#include "game/multiplayer.h"
#include "game/protected_dvars.h"
#include "ui/announcement_feed.h"
#include "ui/game_console.h"

REX_EXTERN(__imp__MPPlayerManager__vfunc_1);  // 0x823D8C08: the game's listener for peer messages

namespace cod3::mp {

namespace {

// The client's connection state (codmp_xenonf.xex): 2 while it plays in a
// match: CL_SendCmd (0x8252CB70), CL_CreateNewCommands and
// CL_ForwardToServer_f only work then; CL_Disconnect sets 0.
constexpr uint32_t kClientState = 0x82C2DAC8;
constexpr uint32_t kClientInMatch = 2;

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}

bool ServerRunsHere(uint8_t* base) {
  // sv_running is registered by Com_Init; 0 until then.
  const uint32_t var = guest::ReadU32(base, addr::crossref::kSvRunningPtr);
  return var != 0 && game::CvarInteger(base, var) != 0;
}

bool ClientInMatch(uint8_t* base) { return guest::ReadU32(base, kClientState) == kClientInMatch; }

bool InMatch(uint8_t* base) { return ServerRunsHere(base) || ClientInMatch(base); }

// [acct2-ocr] 2026-11-07 (owner 12:28 "start going thru these open or broken things"): a match no other console
// takes part in (split screen, a local match). The session objects (codmp_xenonf.xex, read in the generated code):
//  - [0x82BCCAC0] is the game's network holder; its +0 the MPPlayerManager (0x13A10 bytes, made by 0x82406C70,
//    which sub_823D2B00 calls with the holder);
//  - the manager's MPPeer is its +0xAEB0 (0x82404580 adds it before MPPeer::SendLocalPlayerUpdates 0x82403840):
//    24 player slots from +0x1010, 0x310 bytes each, a slot's connection at +4 (MPPeer::SendToAllRemote's loop
//    0x823DCD88 sends to a slot whose connection is set, that isn't local and whose +70 is set); this console's
//    players are the slots whose indexes are the bytes at +0x5991..+0x5994 (0x823D8FF0, "is this player local");
//  - Demonware's bdSession is the manager's +0xAE18 (0x823E69F8 / 0x823ED0E0 pass it to bdSession::setHost
//    0x821C0EF8); its +60 names the session's host connection (setHost prints "Already host." when it is 0 and
//    drops it, "Promoted to host.", otherwise), +68 this console's peer index. [acct2-ocr] Seen in system link
//    (run acct2-ocr-sl-c): +60 is 0 on the host and 1 on the joiner (a handle or index, not a pointer), +68 is 0
//    and 1; the joiner's +60 is already 1 when its match starts. A joiner's slot table showed no remote player,
//    so the +60 check is the one that keeps a joiner locked.
// No connection to another player and none to a host: this console is the match's only machine, so the match's
// rules are its own (protected_dvars.cpp). Anything that can't be read counts as a match with other consoles.
constexpr uint32_t kNetHolderPtr = 0x82BCCAC0;
constexpr uint32_t kManagerPeer = 0xAEB0;
constexpr uint32_t kPeerSlots = 0x1010, kSlotSize = 0x310, kSlotCount = 24, kSlotConnection = 4;
constexpr uint32_t kPeerLocalSlots = 0x5991, kLocalSlotCount = 4;
constexpr uint32_t kManagerSession = 0xAE18, kSessionHostConnection = 60, kSessionLocalPeer = 68;

bool ValidPtr(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

struct Peers {
  bool readable = false;   // the holder was there and every pointer looked valid
  bool manager = false;    // an MPPlayerManager exists
  int remote = 0;          // slots with a connection that aren't this console's
  uint32_t host_connection = 0;
  uint32_t local_peer = 0;
};

Peers ReadPeers(uint8_t* base) {
  Peers p;
  const uint32_t holder = guest::ReadU32(base, kNetHolderPtr);
  if (!ValidPtr(holder)) return p;
  const uint32_t manager = guest::ReadU32(base, holder);
  if (manager == 0) {  // no player manager: no peer messages at all
    p.readable = true;
    return p;
  }
  if (!ValidPtr(manager)) return p;
  p.manager = true;
  const uint32_t peer = manager + kManagerPeer;
  bool local[kSlotCount] = {};
  for (uint32_t k = 0; k < kLocalSlotCount; ++k) {
    const uint8_t i = guest::ReadU8(base, peer + kPeerLocalSlots + k);
    if (i < kSlotCount) local[i] = true;
  }
  for (uint32_t i = 0; i < kSlotCount; ++i)
    if (!local[i] && guest::ReadU32(base, peer + kPeerSlots + i * kSlotSize + kSlotConnection) != 0) ++p.remote;
  p.host_connection = guest::ReadU32(base, manager + kManagerSession + kSessionHostConnection);
  p.local_peer = guest::ReadU32(base, manager + kManagerSession + kSessionLocalPeer);
  p.readable = true;
  return p;
}

bool LocalOnly(const Peers& p) { return p.readable && p.remote == 0 && p.host_connection == 0; }

// [acct2-ocr] 2026-11-07: this console hosts the session: no host connection (+60 is 0). Also true in a match no
// other console plays (LocalOnly). Unreadable: not the host.
bool SessionHost(const Peers& p) { return p.readable && p.host_connection == 0; }

// ---------------------------------------------------------------------------------------------------------------
// [acct2-ocr] 2026-11-07 The host's notices: a peer message of the port's own, type 0x70, to every connection of
// the session. The game has no handler for 0x70 (the MPPlayerManager constructor 0x82405D50 fills the table from
// 0x1E to 0x72 without it), and its listener (MPPlayerManager::vfunc_1 0x823D8C08) drops a type whose table entry
// is empty (it returns 0 at 0x823D8E78), so a console without this code ignores it. The payload (a bdBitBuffer
// without type tags, after its header bit): 8 bits 0xC3, 8 bits version 1, 8 bits kind, 8 bits the body's length,
// then its bytes. Kind 1: an announcement, UTF-8, at most 96 bytes. Kind 2: the host's match rules, its
// ServerInfoPairs string ("\\alternate_movement\\0\\sticky_aim\\1..."), printable ASCII, at most 255 bytes.
// How the game makes one (the sender of type 0x55, 0x823DA330): bdMemory::allocate 0x821AD880 (24 bytes),
// bdMessage::ctor 0x821B1E38 (message, type, type-checked: +8 type, +12 payload reference, +16 type-checked),
// one reference (+4 counts them), the counter at 0x82BCCAD4 up by one, bdMessage::getPayload 0x821B1FB0 (out
// reference, message: the bit buffer, made when missing), the writes (bdBitBuffer::writeBits 0x821AFB08: buffer,
// source, bits), a second reference that MPPeer::SendAll 0x823D8F08 (peer, reference, 1, 0) releases, then the
// payload's and the first reference released (0x820DB148, the reference's address). SendAll sends nothing
// without the peer's +0x5998.
constexpr uint8_t kNoticeType = 0x70;
constexpr uint8_t kNoticeMagic = 0xC3, kNoticeVersion = 1, kNoticeAnnouncement = 1, kNoticeMatchRules = 2;
constexpr size_t kNoticeMaxText = 96, kNoticeMaxRules = protected_dvars::kServerInfoMax;  // [acct2-ocr] 2026-11-22 one limit
constexpr uint32_t kAllocate = 0x821AD880, kMessageCtor = 0x821B1E38, kGetPayload = 0x821B1FB0;
constexpr uint32_t kWriteBits = 0x821AFB08, kSendAll = 0x823D8F08, kRelease = 0x820DB148;
constexpr uint32_t kMessageCounter = 0x82BCCAD4;
constexpr uint32_t kPeerSession = 0x5998;
constexpr uint32_t kMessageSize = 24, kMessageRefs = 4, kMessageTypeAt = 8, kMessagePayload = 12;
// A bdBitBuffer (engine/crates/cod3_game/src/net/bit_buffer.rs): +8 its bytes, +0x10 how many, +0x18 one past the
// highest bit written.
constexpr uint32_t kBufferData = 8, kBufferSize = 0x10, kBufferWritten = 0x18;

// Printable text only: ASCII from the space up and whole UTF-8 sequences; anything else is a space. At most
// kNoticeMaxText bytes, cut before a sequence that wouldn't fit.
std::string CleanText(std::string_view in) {
  std::string out;
  for (size_t i = 0; i < in.size();) {
    const auto c = static_cast<unsigned char>(in[i]);
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    bool ok = n != 0 && i + n <= in.size() && (n == 1 ? (c >= 0x20 && c < 0x7F) : c >= 0xC2);
    for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
    if (!ok) n = 1;
    if (out.size() + (ok ? n : 1) > kNoticeMaxText) break;
    if (ok)
      out.append(in.substr(i, n));
    else
      out += ' ';
    i += n;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// The match rules' string: printable ASCII only and at most kNoticeMaxRules bytes, else nothing.
std::string CleanRules(std::string_view in) {
  if (in.size() > kNoticeMaxRules) return {};
  for (const char ch : in)
    if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) >= 0x7F) return {};
  return std::string(in);
}

// On the game's thread: sends the notice to every connection of the session. False when there is no session to
// send it on.
bool SendNotice(PPCContext& parent, uint8_t* base, uint8_t kind, const std::string& text) {
  const uint32_t holder = guest::ReadU32(base, kNetHolderPtr);
  if (!ValidPtr(holder)) return false;
  const uint32_t manager = guest::ReadU32(base, holder);
  if (!ValidPtr(manager)) return false;
  const uint32_t peer = manager + kManagerPeer;
  if (!ValidPtr(guest::ReadU32(base, peer + kPeerSession))) return false;
  const std::string body = kind == kNoticeMatchRules ? CleanRules(text) : CleanText(text);
  if (body.empty()) return false;
  std::vector<uint8_t> payload = {kNoticeMagic, kNoticeVersion, kind, static_cast<uint8_t>(body.size())};
  payload.insert(payload.end(), body.begin(), body.end());

  guest::Call call(parent, base);
  PPCContext& c = call.ctx();
  c.r1.u32 = (c.r1.u32 - 0x140) & ~0xFu;      // our references and the payload's bytes
  const uint32_t own_ref = c.r1.u32 + 0x10, payload_ref = own_ref + 4, send_ref = own_ref + 8;
  const uint32_t bytes = c.r1.u32 + 0x20;     // up to 0x20 + 4 + 255 < 0x140
  const uint32_t message = static_cast<uint32_t>(call.Invoke(kAllocate, {kMessageSize}));
  if (!ValidPtr(message)) return false;
  call.Invoke(kMessageCtor, {message, kNoticeType, 0});
  guest::WriteU32(base, message + kMessageRefs, guest::ReadU32(base, message + kMessageRefs) + 1);
  guest::WriteU32(base, own_ref, message);
  guest::WriteU32(base, kMessageCounter, guest::ReadU32(base, kMessageCounter) + 1);
  guest::WriteU32(base, payload_ref, 0);
  call.Invoke(kGetPayload, {payload_ref, message});
  const uint32_t buffer = guest::ReadU32(base, payload_ref);
  bool sent = false;
  if (ValidPtr(buffer)) {
    for (size_t i = 0; i < payload.size(); ++i) guest::WriteU8(base, bytes + static_cast<uint32_t>(i), payload[i]);
    call.Invoke(kWriteBits, {buffer, bytes, payload.size() * 8});
    guest::WriteU32(base, message + kMessageRefs, guest::ReadU32(base, message + kMessageRefs) + 1);
    guest::WriteU32(base, send_ref, message);
    call.Invoke(kSendAll, {peer, send_ref, 1, 0});   // releases send_ref's reference
    call.Invoke(kRelease, {payload_ref});
    sent = true;
  }
  call.Invoke(kRelease, {own_ref});
  return sent;
}

// `log`: a line for the log when it is sent (the match rules' 10 s repeats aren't logged; a failure always is).
void QueueNotice(const std::string& text, uint8_t kind = kNoticeAnnouncement, bool log = true) {
  Engine::Get().QueueGuestTask([text, kind, log](PPCContext& ctx, uint8_t* base) {
    const bool sent = SendNotice(ctx, base, kind, text);
    // [acct2-ocr] 2026-11-22 a rules string too long to send says so (it used to read "no session")
    const bool too_long = kind == kNoticeMatchRules && text.size() > kNoticeMaxRules;
    if (too_long) REXLOG_ERROR("cod3mp: the host's match rules are {} bytes, over {}: not sent", text.size(), kNoticeMaxRules);
    if (log || !sent)
      REXLOG_INFO("cod3mp: host's {} to the other consoles {}: {}",
                  kind == kNoticeMatchRules ? "match rules" : "notice",
                  sent ? "sent" : too_long ? "not sent (too long)" : "not sent (no session)", text);
  });
}

// [acct2-ocr] 2026-11-14 Who sent a peer message (acct2-main 15:40: "accept kind 2 (and the announcements) only from
// the session's host connection (+60), if the message or the accept path names its sender; if it can't be told,
// accept them only while the session has one remote connection"). Read the way the listener itself finds it
// (MPPlayerManager::vfunc_1 0x823D8C08, in the generated code): its r4 points at two references, +0 the message and
// +4 the connection it came from. The game copies the second (sub_821B3E10) and looks it up with sub_821BFF08 in the
// session that its r3 +0x5998 points to: a list at the session's +20 whose +0 holds the entries, 8 bytes each, and
// whose +8 (the session's +28) how many (sub_821BF978: entries + index * 8, after a bounds check); an entry matches
// when its first word is the connection's object. That index is the one the game hands its own handlers. Here it
// is only read: nothing of the game's is called and no reference is taken.
constexpr uint32_t kListenerSession = 0x5998;
constexpr uint32_t kSessionConnections = 20, kSessionConnectionCount = 28, kConnectionEntrySize = 8;
constexpr uint32_t kMessageFrom = 4;          // in the pair the listener's r4 points at
constexpr uint32_t kSessionHostRef = 8;       // [waw-decomp] 2026-11-16 the session's host connection object
constexpr uint32_t kMaxConnections = 64;      // more than a session holds; past it the list counts as unreadable

struct Sender {
  bool readable = false;  // the session's list could be read
  bool found = false;     // the sending connection is in it
  uint32_t index = 0;     // and its index there
  uint32_t count = 0;     // the list's length
  int connections = 0;    // its entries that hold a connection
  uint32_t from = 0;      // [waw-decomp] 2026-11-16 the connection object the message came on
  uint32_t host = 0;      // [waw-decomp] 2026-11-16 the session's host connection object (+8), 0 when none
};

Sender FindSender(uint8_t* base, uint32_t listener, uint32_t pair) {
  Sender s;
  if (!ValidPtr(listener) || !ValidPtr(pair)) return s;
  const uint32_t session = guest::ReadU32(base, listener + kListenerSession);
  if (!ValidPtr(session)) return s;
  const uint32_t entries = guest::ReadU32(base, session + kSessionConnections);
  s.count = guest::ReadU32(base, session + kSessionConnectionCount);
  if (s.count > kMaxConnections || (s.count != 0 && !ValidPtr(entries))) return s;
  s.readable = true;
  const uint32_t from = guest::ReadU32(base, pair + kMessageFrom);
  s.from = from;                                              // [waw-decomp] 2026-11-16
  s.host = guest::ReadU32(base, session + kSessionHostRef);   // [waw-decomp] 2026-11-16
  for (uint32_t i = 0; i < s.count; ++i) {
    const uint32_t connection = guest::ReadU32(base, entries + i * kConnectionEntrySize);
    if (connection == 0) continue;
    ++s.connections;
    if (!s.found && connection == from) {
      s.found = true;
      s.index = i;
    }
  }
  return s;
}

// The receiving end, on the game's thread (the listener below), on a console that isn't the session's host, at most
// kNoticeBurst of each kind in kNoticeWindow. An announcement is shown, text only, never run; the match rules go to
// protected_dvars::ApplyServerInfo, which takes only its listed keys, each checked against its domain.
constexpr int kNoticeBurst = 6;
constexpr auto kNoticeWindow = std::chrono::seconds(10);

void OnNotice(uint8_t* base, uint32_t listener, uint32_t pair, uint32_t message) {
  static int warned = 0;
  auto drop = [&](const char* why) {
    if (warned < 8) {
      ++warned;
      REXLOG_WARN("cod3mp: a notice from another console dropped: {}", why);
    }
  };
  // Only the host announces, and its own notices come back to it (SendAll reaches every connection of the session,
  // this console's own too: run acct2-ocr-sl-c1): dropped here without a warning.
  const Peers peers = ReadPeers(base);
  if (SessionHost(peers)) return;
  // [acct2-ocr] 2026-11-14 Only the host's notices are taken: from the session's host connection (the bdSession's
  // +60, 1 on a joiner in run acct2-ocr-sl-c), or, while the session holds a single connection, from that one (a
  // guest's only connection is the host's). In a match of three or more, a notice from another guest is dropped.
  // Whether +60 is the list's index is still to be seen in a system link run: the line below logs both whenever they
  // change. A list that can't be read keeps the old rule (any console of the session), with one warning.
  const Sender from = FindSender(base, listener, pair);
  {
    static uint64_t seen = ~0ull;
    // [waw-decomp] 2026-11-16 bit 61: the sender is the session's host connection object.
    const bool from_host = from.host != 0 && from.from == from.host;
    const uint64_t now_seen = (uint64_t{from.readable} << 63) | (uint64_t{from.found} << 62) |
                              (uint64_t{from_host} << 61) |
                              (uint64_t{from.index & 0xFF} << 40) | (uint64_t{peers.host_connection & 0xFF} << 32) |
                              (uint64_t{from.count & 0xFF} << 8) | static_cast<uint64_t>(from.connections & 0xFF);
    if (now_seen != seen) {
      seen = now_seen;
      REXLOG_INFO("cod3mp: a notice from another console: {}; the session's host connection {}, its list {} long "
                  "({} with a connection); the sender {} the host's connection object",  // [waw-decomp] 2026-11-16
                  !from.readable ? std::string("its sender can't be read")
                  : from.found   ? "connection " + std::to_string(from.index)
                                 : std::string("a connection the session doesn't list"),
                  peers.host_connection, from.count, from.connections, from_host ? "is" : "isn't");
    }
  }
  // [mp-exploits] 2026-11-24 A notice whose sender can't be told is dropped (it used to be taken from any console of
  // the session): the match rules then stay at the match defaults on this console, which is safe, while taking them
  // let any guest set them whenever the list misread. The two-seat test (COD3-rea-work/mpx/mt2) checks that honest
  // play never reaches this: the joiner gets the host's rules and logs no "can't be read".
  if (!from.readable) {
    return drop("its sender can't be read");
  } else if (!from.found) {
    return drop("from a connection the session doesn't list");
  } else if (!(from.host != 0 && from.from == from.host) && from.connections != 1) {  // [waw-decomp] 2026-11-16
    return drop("not from the host's connection");
  }
  const uint32_t buffer = guest::ReadU32(base, message + kMessagePayload);
  if (!ValidPtr(buffer)) return drop("no payload");
  const uint32_t data = guest::ReadU32(base, buffer + kBufferData);
  const uint32_t size = guest::ReadU32(base, buffer + kBufferSize);
  const uint32_t written = guest::ReadU32(base, buffer + kBufferWritten);
  if (data < 0x10000 || size == 0 || size > 0x1000 || data > 0xC0000000u - size) return drop("an odd payload");
  const uint32_t bits = std::min<uint32_t>(written, size * 8);
  uint32_t pos = 1;  // after the header bit
  auto read8 = [&](uint8_t& out) {
    if (pos + 8 > bits) return false;
    uint32_t v = 0;
    for (uint32_t i = 0; i < 8; ++i, ++pos) v |= ((guest::ReadU8(base, data + (pos >> 3)) >> (pos & 7)) & 1u) << i;
    out = static_cast<uint8_t>(v);
    return true;
  };
  if (guest::ReadU8(base, data) & 1) return drop("type-checked");
  uint8_t magic = 0, version = 0, kind = 0, length = 0;
  if (!read8(magic) || !read8(version) || !read8(kind) || !read8(length)) return drop("too short");
  if (magic != kNoticeMagic || version != kNoticeVersion || (kind != kNoticeAnnouncement && kind != kNoticeMatchRules) ||
      (kind == kNoticeAnnouncement && length > kNoticeMaxText))
    return drop("not one of this port's notices");
  std::string raw(length, '\0');
  for (auto& ch : raw) {
    uint8_t b = 0;
    if (!read8(b)) return drop("cut short");
    ch = static_cast<char>(b);
  }
  static std::vector<std::chrono::steady_clock::time_point> recent[2];
  auto& times = recent[kind == kNoticeMatchRules ? 1 : 0];
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(times, [&](const auto& t) { return now - t > kNoticeWindow; });
  if (static_cast<int>(times.size()) >= kNoticeBurst) return drop("too many at once");
  times.push_back(now);
  if (kind == kNoticeMatchRules) {
    const std::string rules = CleanRules(raw);
    if (rules.empty()) return drop("match rules that aren't plain text");
    static std::string last;
    if (rules != last) REXLOG_INFO("cod3mp: the host's match rules: {}", rules);
    last = rules;
    protected_dvars::ApplyServerInfo(rules);
    return;
  }
  const std::string text = CleanText(raw);
  if (text.empty()) return drop("no text");
  REXLOG_INFO("cod3mp: announcement from the host: {}", text);
  hud::AnnouncementFeed::Push(text);
  ui::GameConsole::Get().Print(ui::ConsoleChannel::kSystem, text + "\n");
}

bool g_in_match = false;
bool g_cheats = false;
// [acct2-ocr] 2026-11-22 This console has been a guest in the running match (a second at least, so a misread of the
// session can't latch it on the host): until the match ends it plays by the host's rules, also if it becomes the
// session's host (it keeps the old host's rules and sends those), and it isn't the host for cheats (IsHost).
std::atomic<bool> g_guest_this_match{false};
std::atomic<bool> g_turning_off{false};  // a task to turn cheats off is queued

// Cheats on during a match on a console that isn't its host. The console checks `sv_cheats 1` when it is typed
// and the game thread applies it at its next frame, so one typed while a level loads (no match yet) lands in the
// match. Turn them off again, on the game's thread, with every cheat dvar and toggle reset.
void TurnCheatsOff() {
  if (g_turning_off.exchange(true)) return;
  Engine::Get().QueueGuestTask([](PPCContext& ctx, uint8_t* base) {
    g_turning_off = false;
    cheats::Sync(base);
    if (!cheats::Enabled() || !InMatch(base) || IsHost()) return;   // [acct2-ocr] the host's own cheats stay
    Engine::CvarSet(ctx, base, "sv_cheats", "0", /*force=*/true);
    cheats::Sync(base);
    CheatWatch::Get().ResetCheats(ctx, base);
    REXLOG_WARN("cod3mp: sv_cheats was 1 during a match on a console that isn't its host; turned off again");
    hud::AnnouncementFeed::Push("Cheats stay off in multiplayer matches");
  });
}

}  // namespace

// The campaign's launch of this program; here there is nothing to launch.
void SetGameDataRoot(const std::filesystem::path&) {}
bool MultiplayerAvailable() { return true; }

bool IsMatchRunning() {
  uint8_t* base = GuestBase();
  return base && InMatch(base);
}

// [acct2-ocr] 2026-11-07: no match, or this console hosts the session (SessionHost: also a match no other console
// plays).
bool IsHost() {
  uint8_t* base = GuestBase();
  if (!base || !InMatch(base)) return true;
  // [acct2-ocr] 2026-11-22 a guest promoted to host during the match gets no host rights (cheats) in it
  return SessionHost(ReadPeers(base)) && !g_guest_this_match.load(std::memory_order_relaxed);
}

// [acct2-ocr] 2026-11-07: a match this console alone plays (ReadPeers above).
bool IsLocalOnlyMatch() {
  uint8_t* base = GuestBase();
  return base && InMatch(base) && LocalOnly(ReadPeers(base));
}

// Shown on this machine (feed, console, log); [acct2-ocr] the host's also go to the other consoles of the match
// (the host's notices above).
void Announce(const std::string& text) {
  REXLOG_INFO("cod3mp: announcement: {}", text);
  hud::AnnouncementFeed::Push(text);
  ui::GameConsole::Get().Print(ui::ConsoleChannel::kSystem, text + "\n");
  uint8_t* base = GuestBase();
  if (!base || !InMatch(base)) return;
  const Peers peers = ReadPeers(base);
  if (SessionHost(peers) && peers.remote > 0) QueueNotice(text);
}

void Tick() {
  uint8_t* base = GuestBase();
  if (!base) return;
  const bool in_match = InMatch(base);
  cheats::Sync(base);
  const bool on = cheats::Enabled();
  if (in_match != g_in_match) {
    if (in_match) {
      REXLOG_INFO("cod3mp: match started (server here {}, client in a match {}; sv_cheats {}); "
                  "only the session's host can turn cheats on",
                  ServerRunsHere(base), ClientInMatch(base), on ? 1 : 0);
    } else {
      REXLOG_INFO("cod3mp: match ended");
    }
  } else if (in_match && on != g_cheats) {
    REXLOG_WARN("cod3mp: sv_cheats became {} during the match", on ? 1 : 0);
  }
  // [acct2-ocr] 2026-11-07: whether other consoles play the match and who hosts it, when it starts and whenever
  // that changes.
  static int local_state = -2;
  static int last_remote = 0;
  static std::chrono::steady_clock::time_point tell_joiners{};
  // [acct2-ocr] the host's match rule changes are told to every player (protected_dvars' TellHostChange).
  static bool sender_set = false;
  if (!sender_set) {
    sender_set = true;
    protected_dvars::SetServerMessageSender([](const std::string& text) { Announce(text); });
  }
  // [acct2-ocr] The host's match rules to the other consoles: at once when they change, 4 s after a console joins
  // or a match starts, and every 10 s. [acct2-ocr] 2026-11-22 After a join or a start at 0.5, 2 and 4 s (a guest
  // plays by the pending values until the first arrives); a change at most every 2 s (a guest takes 6 in 10 s).
  static std::string rules_sent;
  static std::chrono::steady_clock::time_point rules_at{};
  static std::array<std::chrono::steady_clock::time_point, 3> rules_due{};
  static std::chrono::steady_clock::time_point guest_since{};
  static bool told_promoted = false;
  if (in_match) {
    const Peers peers = ReadPeers(base);
    const bool host = SessionHost(peers);
    const int state = !peers.readable ? -1 : LocalOnly(peers) ? 1 : host ? 2 : 0;
    // [acct2-ocr] 2026-11-22 a guest for a second: one till the match ends (g_guest_this_match)
    {
      const auto t = std::chrono::steady_clock::now();
      if (state != 0) guest_since = {};
      else if (guest_since.time_since_epoch().count() == 0) guest_since = t;
      if (state == 0 && t - guest_since >= std::chrono::seconds(1) && !g_guest_this_match.exchange(true))
        REXLOG_INFO("cod3mp: a guest in this match: the host's match rules apply here until it ends");
      if (g_guest_this_match && state == 2 && !told_promoted) {
        told_promoted = true;
        REXLOG_WARN("cod3mp: this console became the session's host during the match: it keeps the old host's match "
                    "rules and sends those, with no cheats, until the match ends");
      }
    }
    const bool guest = g_guest_this_match.load(std::memory_order_relaxed);
    // [acct2-ocr] 2026-11-22 the state every reader of the match rules sees this frame (game/protected_dvars.h)
    protected_dvars::NoteMatch(true, !guest && (host || state == 1));
    if (on && (!host || guest)) TurnCheatsOff();
    if (!g_in_match || state != local_state) {
      REXLOG_INFO("cod3mp: {} (player manager {}, remote players {}, host connection {:08X}, peer index {})",
                  state == 1   ? "this PC alone plays the match: its own match rules apply (F2 > Game > Server)"
                  : state == 2 ? "this console hosts the match with other consoles: its match rules are the "
                                 "match's (sent to the others), cheats are the host's"
                  : state == 0 ? "other consoles play the match, another one hosts it: the host's match rules "
                                 "apply once they arrive (until then the match defaults, the grenade arc off), no "
                                 "cheats here"
                               : "the session can't be read: the match rules stay at their defaults, no cheats here",
                  peers.manager ? "yes" : "no", peers.remote, peers.host_connection, peers.local_peer);
      local_state = state;
    }
    // A console that joins while the host's cheats are on is told so, a few seconds later (once it is in).
    const auto now = std::chrono::steady_clock::now();
    if (host && on && peers.remote > last_remote) tell_joiners = now + std::chrono::seconds(4);
    if (tell_joiners.time_since_epoch().count() != 0 && now >= tell_joiners) {
      tell_joiners = {};
      if (host && on && peers.remote > 0) QueueNotice("Cheats enabled by the host");
    }
    if (host && peers.remote > 0) {
      if (peers.remote > last_remote || !g_in_match)
        rules_due = {now + std::chrono::milliseconds(500), now + std::chrono::seconds(2), now + std::chrono::seconds(4)};
      const std::string rules = protected_dvars::ServerInfoPairs();
      bool due = false;
      for (auto& t : rules_due) {
        if (t.time_since_epoch().count() != 0 && now >= t) {
          due = true;
          t = {};
        }
      }
      const bool changed = rules != rules_sent;
      if ((changed && now - rules_at >= std::chrono::seconds(2)) || due || now - rules_at >= std::chrono::seconds(10)) {
        QueueNotice(rules, kNoticeMatchRules, /*log=*/changed || due);
        rules_sent = rules;
        rules_at = now;
      }
    } else {
      rules_sent.clear();
      rules_due = {};
    }
    last_remote = peers.remote;
  } else {
    local_state = -2;
    last_remote = 0;
    tell_joiners = {};
    rules_sent.clear();
    rules_due = {};
    guest_since = {};
    told_promoted = false;
    g_guest_this_match.store(false, std::memory_order_relaxed);
    protected_dvars::NoteMatch(false, true);  // [acct2-ocr] 2026-11-22
    // [acct2-ocr] A guest's match is over: the host's rules are dropped, its own values apply again.
    if (g_in_match) protected_dvars::LeaveMatch();
  }
  protected_dvars::FlushHostChanges();  // [acct2-ocr] 2026-11-22 the host's settled changes, told
  g_in_match = in_match;
  g_cheats = on;
}

}  // namespace cod3::mp

// [acct2-ocr] 2026-11-07 The listener for every peer message (bdSession::accept passes the ones from known peers;
// r3 the listener, r4 two references: +0 the message, +4 the connection it came from, 2026-11-14): the port's
// notices are read here; everything else goes on to the game as before.
REX_HOOK_RAW(MPPlayerManager__vfunc_1) {
  const uint32_t ref = ctx.r4.u32;
  const uint32_t message = ref >= 0x10000 && ref < 0xC0000000u ? cod3::guest::ReadU32(base, ref) : 0;
  if (message >= 0x10000 && message < 0xC0000000u && (message & 3) == 0 &&
      cod3::guest::ReadU8(base, message + 8) == 0x70) {
    cod3::mp::OnNotice(base, ctx.r3.u32, ref, message);
    ctx.r3.u64 = 1;
    return;
  }
  // [acct2-main] 2026-11-14 (dev/agents/acct2-main/MP-SAFETY.md gap 3): a message the game's handler would take
  // without a check it needs (peer_guard_mp.cpp: a position update for another console's player) is dropped, as
  // handled.
  if (message >= 0x10000 && message < 0xC0000000u && (message & 3) == 0 &&
      !cod3::mp::PeerMessageAllowed(base, ctx.r3.u32, ref, message)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__MPPlayerManager__vfunc_1(ctx, base);
}
