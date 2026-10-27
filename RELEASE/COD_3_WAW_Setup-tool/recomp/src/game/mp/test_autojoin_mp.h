// cod3mp: [claude-9b] automated tests' helpers (test_autojoin_mp.cpp).
#pragma once

#include <cstdint>

namespace cod3::mp_test {

// Once a drawn frame (after BeginNextFrame): the front end's page trace (cod3_debug_menu_pages).
void EachFrame(uint8_t* base);

}  // namespace cod3::mp_test
