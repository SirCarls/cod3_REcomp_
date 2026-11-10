// cod2 - test runs that get into the game by themselves. [acct2-cod2]
// See test_start.cpp.

#pragma once

#include <string_view>

namespace cod2::test_start {

// Called (from the game's own thread) whenever the game opens a menu.
void OnMenuOpened(std::string_view name);

}  // namespace cod2::test_start
