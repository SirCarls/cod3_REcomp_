// cod3 - the movement rows of the game's Options menu (see menu_movement.h).

#include "game/movement/menu_movement.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/movement/settings.h"

namespace cod3::movementmenu {

namespace mv = cod3::movement;

static_assert(kRows == mv::kRowCount, "the menu shows every movement row");

namespace {

struct Text {
  const char* key;
  const char* text;
};
constexpr Text kTexts[] = {
    {"FEMENU_COP_MV_TITLE", "OPTIONS / MOVEMENT"},
    {"FEMENU_COP_MV_ITEM", "MOVEMENT OPTIONS"},
    {"FEMENU_COP_MV_SPRINT", "SPRINT"},
    {"FEMENU_COP_MV_SPRINT_BUTTON", "SPRINT BUTTON"},
    {"FEMENU_COP_MV_STYLE", "MOVEMENT STYLE"},
    {"FEMENU_COP_MV_COD4_SPRINT", "COD4 SPRINT"},
    {"FEMENU_COP_MV_COD4_SPEEDS", "COD4 SPEEDS"},
    {"FEMENU_COP_MV_COD4_JUMPS", "COD4 JUMP TRICKS"},
};
// The row labels' keys, in settings.h's row order.
constexpr const char* kLabelKeys[kRows] = {
    "FEMENU_COP_MV_SPRINT",      "FEMENU_COP_MV_SPRINT_BUTTON", "FEMENU_COP_MV_STYLE",
    "FEMENU_COP_MV_COD4_SPRINT", "FEMENU_COP_MV_COD4_SPEEDS",   "FEMENU_COP_MV_COD4_JUMPS",
};

// Guest copies of the texts, made once (the game keeps the pointers).
std::mutex g_text_mutex;
std::unordered_map<std::string, uint32_t> g_guest_texts;

uint32_t FixedText(uint8_t* base, const char* key, const char* text) {
  std::lock_guard lock(g_text_mutex);
  uint32_t& addr = g_guest_texts[key];
  if (!addr) {
    const uint32_t len = static_cast<uint32_t>(std::strlen(text)) + 1;
    addr = rex::system::kernel_state()->memory()->SystemHeapAlloc(len);
    if (addr) std::memcpy(base + addr, text, len);
  }
  return addr;
}

}  // namespace

int RowCount() { return kRows; }

const char* LabelKey(int row) { return row >= 0 && row < kRows ? kLabelKeys[row] : "FEMENU_COP_MV_SPRINT"; }
const char* TitleKey() { return "FEMENU_COP_MV_TITLE"; }
const char* PauseItemKey() { return "FEMENU_COP_MV_ITEM"; }

std::string Value(int row) { return mv::RowValue(row); }

std::string Note(int row) {
  // Short, like the video rows' notes: why the row can't change now.
  return mv::RowLock(row);
}

std::string Step(PPCContext& ctx, uint8_t* base, int row, bool right) {
  (void)ctx;
  (void)base;
  if (!mv::StepRow(row, right)) return "";
  REXLOG_INFO("cod3: Options > Movement: {} = {}", mv::RowLabel(row), mv::RowValue(row));
  return mv::RowValue(row);
}

uint32_t Localize(uint8_t* base, const char* key) {
  if (!key || std::strncmp(key, "FEMENU_COP_MV_", 14) != 0) return 0;
  for (const Text& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return FixedText(base, t.key, t.text);
  return 0;
}

}  // namespace cod3::movementmenu
