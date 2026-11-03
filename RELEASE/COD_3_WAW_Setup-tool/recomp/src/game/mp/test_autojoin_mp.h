// cod3mp: [claude-9b] automated tests' helpers (test_autojoin_mp.cpp).
#pragma once

#include <cstdint>

namespace cod3::mp_test {

// Once a drawn frame (after BeginNextFrame): the front end's page trace (cod3_debug_menu_pages).
void EachFrame(uint8_t* base);

// [claude-9b-think] 2026-11-02 UNTESTED (test_syslink_mp.cpp), called by EachFrame: while cod3_test_syslink is
// set, queues one step of the system link driver on the game's main thread (Engine::QueueGuestTask).
void SyslinkFrame(uint8_t* base);

}  // namespace cod3::mp_test
