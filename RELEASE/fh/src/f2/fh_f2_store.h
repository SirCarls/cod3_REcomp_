// [acct2-fh] 2026-11-14 Finest Hour's F2 settings: what the window's rows read and change (see shim/rex/cvar.h and
// fh_f2_store.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace fh_f2 {

// The window's look, from fh.toml's [interface] (f2_theme reads them every frame through f2::SetLook).
std::string& ThemeStorage();
int32_t& TextSizeStorage();
double& OpacityStorage();

// A fh.toml setting the window has a row for.
bool IsSetting(const char* key);

// Writes every change into fh.toml. False with *error when it can't.
bool Save(std::string* error);

}  // namespace fh_f2
