// [f2-menus] Big Red One's F2 settings: what the window's rows read and change (see shim/rex/cvar.h).
#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace bro_f2 {

// The window (for display.fullscreen, applied at once). Once, from bro_f2_init.
void SetWindow(SDL_Window* window);

// The window's look, read from bro.toml's [interface] (f2_theme reads them every frame through f2::SetLook).
std::string& ThemeStorage();
int32_t& TextSizeStorage();
double& OpacityStorage();

// bro.toml settings the window has rows for, by key; and whether a change takes effect at once in this build.
bool IsPortSetting(const char* key);
bool AppliesAtOnce(const char* key);

// Writes every change (bro.toml, including [cvars]). False with *error when it can't.
bool Save(std::string* error);

}  // namespace bro_f2
