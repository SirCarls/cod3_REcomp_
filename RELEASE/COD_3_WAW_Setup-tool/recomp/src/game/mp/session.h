// cod3mp: the match state behind game/multiplayer.h (session.cpp).

#pragma once

namespace cod3::mp {

// UI thread, every frame: notes when a match starts or ends (log line), and
// turns cheats off again if sv_cheats is 1 during a match.
void Tick();

}  // namespace cod3::mp
