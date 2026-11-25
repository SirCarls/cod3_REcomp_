// codclassic - test runs straight into a level: a console command after the game's first frames.
// [acct2-uo]
//
// Tests only, off by default:
//   codclassic_debug_start_command  a console command (for example "devmap <map>" or "map <map>"), run once
//                                   through the game's own command buffer
//   codclassic_debug_start_delay    seconds after the game's first event loop (default 10), so the front end
//                                   has opened before the command runs
// The command goes in with Cbuf_AddText, from inside a hook on Com_EventLoop, which Quake III's frame calls once
// a frame on the game's main thread. Addresses (x360-family's names, the "selfname" pass: each function's own
// strings name it): campaign Cbuf_AddText 0x82071190, Com_EventLoop 0x8207DAD0; multiplayer 0x8207A960,
// 0x82087168.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(codclassic_debug_start_command, "", "codclassic/Debug",
                      "Tests: a console command the game runs once, codclassic_debug_start_delay seconds "
                      "after its first frame (for example \"devmap <map>\")");
REXCVAR_DEFINE_INT32(codclassic_debug_start_delay, 10, "codclassic/Debug",
                     "Tests: seconds before codclassic_debug_start_command runs")
    .range(0, 600);

#if defined(CODCLASSIC_MP)
#define CODCLASSIC_CBUF_ADD_TEXT sub_8207A960
#define CODCLASSIC_COM_EVENT_LOOP sub_82087168
#else
#define CODCLASSIC_CBUF_ADD_TEXT sub_82071190
#define CODCLASSIC_COM_EVENT_LOOP sub_8207DAD0
#endif
#define CODCLASSIC_IMP_(name) __imp__##name
#define CODCLASSIC_IMP(name) CODCLASSIC_IMP_(name)

REX_EXTERN(CODCLASSIC_IMP(CODCLASSIC_CBUF_ADD_TEXT));   // Cbuf_AddText(text r3)
REX_EXTERN(CODCLASSIC_IMP(CODCLASSIC_COM_EVENT_LOOP));  // Com_EventLoop() -> r3

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point g_first_frame{};  // only touched on the game's main thread
bool g_command_sent = false;

// Appends `command` and a line end to the game's command buffer. Runs inside a hook on the game's main thread:
// the text is written to the guest stack well below the current frame, the call gets a frame of its own below
// that, and the whole register state is put back afterwards, so the hooked function sees nothing.
void QueueCommand(PPCContext& ctx, uint8_t* base, const std::string& command) {
  const std::string text = command + "\n";
  const PPCContext saved = ctx;
  const uint32_t text_address = (ctx.r1.u32 - 0x400u - static_cast<uint32_t>(text.size())) & ~0xFu;
  std::memcpy(base + text_address, text.c_str(), text.size() + 1);
  ctx.r1.u32 = text_address - 0x100u;
  ctx.r3.u64 = text_address;
  CODCLASSIC_IMP(CODCLASSIC_CBUF_ADD_TEXT)(ctx, base);
  ctx = saved;
  ctx.fpscr.restoreGuestBits(saved.fpscr.csr);
}

}  // namespace

REX_HOOK_RAW(CODCLASSIC_COM_EVENT_LOOP) {
  if (!g_command_sent) {
    const std::string command = REXCVAR_GET(codclassic_debug_start_command);
    if (command.empty()) {
      g_command_sent = true;  // nothing to do; stop checking
    } else {
      const auto now = Clock::now();
      if (g_first_frame == Clock::time_point{}) g_first_frame = now;
      if (now - g_first_frame >= std::chrono::seconds(REXCVAR_GET(codclassic_debug_start_delay))) {
        g_command_sent = true;
        REXLOG_INFO("codclassic: test start command \"{}\"", command);
        QueueCommand(ctx, base, command);
      }
    }
  }
  CODCLASSIC_IMP(CODCLASSIC_COM_EVENT_LOOP)(ctx, base);
}
