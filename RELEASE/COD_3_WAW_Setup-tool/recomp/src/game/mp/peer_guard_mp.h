// cod3mp - [acct2-main] 2026-11-14 checks on peer messages before the game's own handlers see them
// (peer_guard_mp.cpp; dev/agents/acct2-main/MP-SAFETY.md gap 3).

#pragma once

#include <cstdint>

namespace cod3::mp {

// On the game's thread, from the peer message listener's hook (session.cpp, MPPlayerManager::vfunc_1): `listener`
// its r3, `pair` its r4 (+0 the message, +4 the connection it came from), `message` the bdMessage. False when the
// message must not reach the game's handler (the hook then returns as if it was handled).
bool PeerMessageAllowed(uint8_t* base, uint32_t listener, uint32_t pair, uint32_t message);

}  // namespace cod3::mp
