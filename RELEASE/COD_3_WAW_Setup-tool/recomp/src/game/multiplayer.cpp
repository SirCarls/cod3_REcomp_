// cod3 - the Multiplayer item: start cod3mp, or say what is missing (see multiplayer.h).

#include "game/multiplayer.h"

#include <cstring>
#include <mutex>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/relaunch.h"
#include "ui/announcement_feed.h"
#include "ui/game_console.h"

namespace cod3::mp {

namespace {
std::filesystem::path g_game_data_root;  // set once before the game starts

bool MultiplayerDataPresent() {
  std::error_code ec;
  return !g_game_data_root.empty() &&
         std::filesystem::is_regular_file(g_game_data_root / "mp" / "global.cod", ec);
}
}  // namespace

void SetGameDataRoot(const std::filesystem::path& root) { g_game_data_root = root; }

bool MultiplayerAvailable() {
  return ProgramInstalled(Program::kMultiplayer) && MultiplayerDataPresent();
}

// The cheat policy's hook points (multiplayer.h): no match yet, so this
// machine counts as the host and an announcement is shown here only.
bool IsMatchRunning() { return false; }
bool IsHost() { return true; }
void Announce(const std::string& text) {
  REXLOG_INFO("cod3: announcement: {}", text);
  hud::AnnouncementFeed::Push(text);
  ui::GameConsole::Get().Print(ui::ConsoleChannel::kSystem, text + "\n");
}

}  // namespace cod3::mp

namespace {

namespace box = cod3::addr::msgbox;
using cod3::guest::Call;
using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

// The messages. They aren't keys of the game's string table, so the game's
// Localize hands them back unchanged and the box shows them as written.
constexpr const char* kNotInstalled =
    "Multiplayer isn't installed in this copy. Build it again with the setup tool to add it.";
constexpr const char* kNoData =
    "The disc's multiplayer files (assets/mp) are missing. Extract the disc again with the setup tool.";

const char* WhyNotAvailable() {
  return cod3::ProgramInstalled(cod3::Program::kMultiplayer) ? kNoData : kNotInstalled;
}

// A guest copy of a message, made once each: the box keeps pointing at it.
uint32_t GuestText(uint8_t* base, const char* message) {
  static std::mutex lock;
  static const char* made_for[2] = {};
  static uint32_t made[2] = {};
  std::lock_guard guard(lock);
  for (int i = 0; i < 2; ++i)
    if (made_for[i] == message) return made[i];
  for (int i = 0; i < 2; ++i) {
    if (made_for[i]) continue;
    const uint32_t size = uint32_t(std::strlen(message)) + 1;
    const uint32_t text = rex::system::kernel_state()->memory()->SystemHeapAlloc(size);
    if (!text) return 0;
    std::memcpy(base + text, message, size);
    made_for[i] = message;
    made[i] = text;
    return text;
  }
  return 0;
}

bool IsObject(uint8_t* base, uint32_t object, uint32_t vtable) {
  return object >= 0x10000 && (object & 3) == 0 && ReadU32(base, object) == vtable;
}

// Shows `text` (a guest string that stays valid) with one OK button in the
// game's own message box, with the same steps as the game's one-button
// messages (addresses.h, msgbox). OK and Back close it, and the menu below
// carries on. False when the box isn't there (nothing is changed then).
bool ShowGameMessage(PPCContext& ctx, uint8_t* base, uint32_t text) {
  const uint32_t player = ReadU32(base, box::kPlayer);
  if (!text || player >= 4) return false;
  const uint32_t system = ReadU32(base, box::kSystems + player * 4);
  if (!IsObject(base, system, box::kSystemVtable)) return false;
  const uint32_t pages = ReadU32(base, system + box::kSysPages);
  if (pages < 0x10000 || !IsObject(base, ReadU32(base, pages), box::kPageVtable)) return false;

  Call c(ctx, base);
  c.Invoke(box::kClose, {system});
  c.Invoke(box::kSetMessage, {system, text});
  // Which of the two pages: chosen after SetMessage, as the game does.
  const uint32_t mode = ReadU32(base, system + box::kSysMode);
  const bool page_two = ReadU32(base, system + box::kSysHasPageTwo + mode * 4) != 0;
  const uint32_t page = ReadU32(base, pages + (page_two ? 4 : 0));
  if (!IsObject(base, page, box::kPageVtable)) {
    c.Invoke(box::kClose, {system});
    return false;
  }
  c.Invoke(box::kAddButton, {page, box::kOkKey, box::kCloseCallback});
  WriteU32(base, page + box::kPageBack, 0);                  // Back closes the box too
  std::memset(base + page + box::kPageSelected, 0, 2);       // u16: OK highlighted
  c.Invoke(box::kUpdateSelection, {ReadU32(base, system + box::kSysButtons)});
  c.Invoke(box::kOpen, {page, 1, 0});
  return true;
}

}  // namespace

// SV_LoadMpExe_f, the `load_mp_exe` command: the main menu's Multiplayer
// item, an accepted game invite or the console (multiplayer.h).
REX_EXTERN(__imp__SV_LoadMpExe_f);
REX_HOOK_RAW(SV_LoadMpExe_f) {
  if (cod3::mp::MultiplayerAvailable()) {
    // The console's hand-over to the multiplayer program (multiplayer.h):
    // the game stops here and cod3mp starts (game/relaunch.h).
    REXLOG_INFO("cod3: multiplayer asked for; handing over to cod3mp");
    __imp__SV_LoadMpExe_f(ctx, base);
    return;
  }
  const char* why = WhyNotAvailable();
  if (ShowGameMessage(ctx, base, GuestText(base, why))) {
    REXLOG_INFO("cod3: multiplayer asked for; not available: {}", why);
    return;
  }
  // No message box (shouldn't happen): say it in the console instead.
  REXLOG_WARN("cod3: multiplayer asked for; not available: {}", why);
  cod3::ui::GameConsole::Get().Print(cod3::ui::ConsoleChannel::kSystem, std::string(why) + "\n");
}
