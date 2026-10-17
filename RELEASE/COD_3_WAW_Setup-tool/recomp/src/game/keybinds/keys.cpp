// cod3 - key codes and names (see keys.h).

#include "game/keybinds/keys.h"

#include <cctype>

namespace cod3::binds {

namespace {

struct Name {
  const char* name;
  KeyCode code;
};

// World at War's names first (KeyName writes the first name of a code),
// then the SDK's spellings and other aliases (only read).
constexpr Name kNames[] = {
    {"SPACE", 0x20},      {"TAB", 0x09},         {"ENTER", 0x0D},       {"ESCAPE", 0x1B},
    {"BACKSPACE", 0x08},  {"SHIFT", 0x10},       {"CTRL", 0x11},        {"ALT", 0x12},
    {"CAPSLOCK", 0x14},   {"PAUSE", 0x13},       {"UPARROW", 0x26},     {"DOWNARROW", 0x28},
    {"LEFTARROW", 0x25},  {"RIGHTARROW", 0x27},  {"INS", 0x2D},         {"DEL", 0x2E},
    {"HOME", 0x24},       {"END", 0x23},         {"PGUP", 0x21},        {"PGDN", 0x22},
    {"F1", 0x70},         {"F2", 0x71},          {"F3", 0x72},          {"F4", 0x73},
    {"F5", 0x74},         {"F6", 0x75},          {"F7", 0x76},          {"F8", 0x77},
    {"F9", 0x78},         {"F10", 0x79},         {"F11", 0x7A},         {"F12", 0x7B},
    {"KP_0", 0x60},       {"KP_1", 0x61},        {"KP_2", 0x62},        {"KP_3", 0x63},
    {"KP_4", 0x64},       {"KP_5", 0x65},        {"KP_6", 0x66},        {"KP_7", 0x67},
    {"KP_8", 0x68},       {"KP_9", 0x69},        {"KP_STAR", 0x6A},     {"KP_PLUS", 0x6B},
    {"KP_MINUS", 0x6D},   {"KP_DEL", 0x6E},      {"KP_SLASH", 0x6F},    {"SEMICOLON", 0xBA},
    // COMMA, not ",": the comma separates keys in cod3.toml and F2 (read: ",").
    {"=", 0xBB},          {"COMMA", 0xBC},       {"-", 0xBD},           {".", 0xBE},
    {"/", 0xBF},          {"`", 0xC0},           {"[", 0xDB},           {"\\", 0xDC},
    {"]", 0xDD},          {"'", 0xDE},           {"MOUSE1", kMouse1},   {"MOUSE2", kMouse2},
    {"MOUSE3", kMouse3},  {"MOUSE4", kMouse4},   {"MOUSE5", kMouse5},   {"MWHEELUP", kWheelUp},
    {"MWHEELDOWN", kWheelDown},
    // Aliases: the SDK's keybind spellings (rex/ui/keybinds.h) and a few more.
    {"Return", 0x0D},     {"KP_ENTER", 0x0D},    {"NumpadEnter", 0x0D}, {"Control", 0x11},
    {"Up", 0x26},         {"Down", 0x28},        {"Left", 0x25},        {"Right", 0x27},
    {"Insert", 0x2D},     {"Delete", 0x2E},      {"PageUp", 0x21},      {"PageDown", 0x22},
    {"Numpad0", 0x60},    {"Numpad1", 0x61},     {"Numpad2", 0x62},     {"Numpad3", 0x63},
    {"Numpad4", 0x64},    {"Numpad5", 0x65},     {"Numpad6", 0x66},     {"Numpad7", 0x67},
    {"Numpad8", 0x68},    {"Numpad9", 0x69},     {"NumpadStar", 0x6A},  {"NumpadPlus", 0x6B},
    {"NumpadMinus", 0x6D}, {"NumpadSlash", 0x6F}, {";", 0xBA},          {"Semicolon", 0xBA},
    {"Plus", 0xBB},       {",", 0xBC},           {"Minus", 0xBD},       {"Period", 0xBE},
    {"Slash", 0xBF},      {"Backtick", 0xC0},    {"~", 0xC0},           {"LBracket", 0xDB},
    {"Backslash", 0xDC},  {"RBracket", 0xDD},    {"Quote", 0xDE},       {"LMB", kMouse1},
    {"RMB", kMouse2},     {"MMB", kMouse3},      {"MB4", kMouse4},      {"MM4", kMouse4},
    {"MB5", kMouse5},     {"MM5", kMouse5},      {"Esc", 0x1B},         {"Caps", 0x14},
};

bool SameText(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

}  // namespace

bool IsCombination(std::string_view name) {
  name = Trim(name);
  const size_t plus = name.find('+');
  // "+" alone is no key name we know; "KP_PLUS" has no '+'.
  return plus != std::string_view::npos && plus > 0 && plus + 1 < name.size();
}

KeyCode ParseKey(std::string_view name) {
  name = Trim(name);
  if (name.empty() || IsCombination(name)) return kNoKey;
  if (name.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(name[0]);
    if (std::isalpha(c)) return static_cast<KeyCode>(std::toupper(c));
    if (std::isdigit(c)) return static_cast<KeyCode>(c);
  }
  for (const Name& n : kNames)
    if (SameText(name, n.name)) return n.code;
  return kNoKey;
}

std::string KeyName(KeyCode code) {
  if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, char(code));
  for (const Name& n : kNames)
    if (n.code == code) return n.name;
  return "";
}

std::string PromptName(KeyCode code) {
  switch (code) {
    case 0x26: return "UP";
    case 0x28: return "DOWN";
    case 0x25: return "LEFT";
    case 0x27: return "RIGHT";
    case 0x1B: return "ESC";
    default: break;
  }
  std::string name = KeyName(code);
  if (name.rfind("KP_", 0) == 0) return "NUM" + name.substr(3);
  return name;
}

bool IsAlwaysReserved(KeyCode code) { return code == kKeyBacktick || code == kKeyEscape; }

}  // namespace cod3::binds
