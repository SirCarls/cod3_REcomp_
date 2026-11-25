// codclassic - one log line per film: how fast each RoQ film played against its own frame rate. [acct2-uo]
//
// For the frame-rate audit's film check (owner 2026-11-07 22:33, after Big Red One's films played too fast:
// every game's films measured against their own frame rate at 60 and uncapped, muted and unmuted).
//
//   codclassic_log_films  default true. At each film's start: its name, its frame rate and which clock it
//                         follows. At each end (or each pass of a looping film): the frames it showed, the
//                         time that took, and the speed against its own rate (1.00 = right).
//
// CoD Classic's RoQ player (Quake III's, with CoD's changes; NOTES 11-07 22:40) keeps 16 film slots of 0x1D0
// bytes. The fields read here, big-endian 32-bit:
//   +0x2C  0: the film follows real time x com_timescale; set: it follows its sound stream's play position
//   +0x34  status: 1 playing; at the end 0 (RoQShutdown) or, for a looping film, 5 (RoQReset)
//   +0x58  frames decoded so far (-1 before the first, as in Quake III)
//   +0xB4  the film's own frames a second (from its header)
// CIN_PlayCinematic(name r3, ...) returns the slot's handle; CIN_RunCinematic(handle r3), once a frame for
// each playing film, returns its status. Addresses (x360-family's names and the decompile):
//   campaign: CIN_PlayCinematic 0x82056130, CIN_RunCinematic 0x82055E60, slots at 0x82571B00
//   MP:       CIN_PlayCinematic 0x820571C8, CIN_RunCinematic 0x82056EF8, slots at 0x82581DA0
// The time is the host's steady clock: a run paused by bgjob while it plays a film counts the pause too.
//
//   codclassic_skip_films  default false. For test runs (owner 2026-11-08 00:21: skip cutscenes and films in every
//                         test run, except runs that time films). Each film that the `cinematic` command starts
//                         (CL_PlayCinematic_f: the intro chain atvi -> iw_logo -> aspyr -> cod_intro, which the
//                         game queues in its init at 0x8207EB78 through `nextmap`, and a level's films) is stopped
//                         after its first frame with the game's own CIN_StopCinematic(handle), the call its
//                         `cinematic` command and its disconnect use. That ends the film the normal way
//                         (RoQShutdown runs `nextmap`), so the chain goes on to the next film and then the menu.
//                         Films the UI starts itself (menu backgrounds) keep playing.
//   Slot fields used for it: +0x34 status (above), +0xBC the frame buffer (0 until the first frame).
//   campaign: CL_PlayCinematic_f 0x82056530, CIN_StopCinematic 0x820550F8
//   MP:       CL_PlayCinematic_f 0x820575C8, CIN_StopCinematic 0x82056178
// The game's own guard for the intro: it is queued only while x360shared ordinal 28's int is 0 (8207EB78).

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(codclassic_log_films, true, "codclassic/Debug",
                    "A log line at each film's start and end: its frame rate, its clock and its speed "
                    "against that rate");

REXCVAR_DEFINE_BOOL(codclassic_skip_films, false, "codclassic/Debug",
                    "Stop each film the cinematic command starts (the intro films, a level's films) after its "
                    "first frame, as the game's own skip does; menu films keep playing. For test runs, not for "
                    "runs that time films");

#if defined(CODCLASSIC_MP)
#define CODCLASSIC_CIN_PLAY sub_820571C8
#define CODCLASSIC_CIN_RUN sub_82056EF8
#define CODCLASSIC_CIN_COMMAND sub_820575C8
#define CODCLASSIC_CIN_STOP sub_82056178
#else
#define CODCLASSIC_CIN_PLAY sub_82056130
#define CODCLASSIC_CIN_RUN sub_82055E60
#define CODCLASSIC_CIN_COMMAND sub_82056530
#define CODCLASSIC_CIN_STOP sub_820550F8
#endif
#define CODCLASSIC_FILM_IMP_(name) __imp__##name
#define CODCLASSIC_FILM_IMP(name) CODCLASSIC_FILM_IMP_(name)

REX_EXTERN(CODCLASSIC_FILM_IMP(CODCLASSIC_CIN_PLAY));  // CIN_PlayCinematic(name r3, ...) -> handle r3
REX_EXTERN(CODCLASSIC_FILM_IMP(CODCLASSIC_CIN_RUN));   // CIN_RunCinematic(handle r3) -> status r3
REX_EXTERN(CODCLASSIC_FILM_IMP(CODCLASSIC_CIN_COMMAND));  // CL_PlayCinematic_f(), the `cinematic` command
REX_EXTERN(CODCLASSIC_CIN_STOP);                       // CIN_StopCinematic(handle r3) -> status r3 (2 = stopped)

namespace {

using Clock = std::chrono::steady_clock;

#if defined(CODCLASSIC_MP)
constexpr uint32_t kSlots = 0x82581DA0u;
#else
constexpr uint32_t kSlots = 0x82571B00u;
#endif
constexpr uint32_t kSlotSize = 0x1D0u;
constexpr int kSlotCount = 16;
constexpr uint32_t kSoundClock = 0x2C, kStatus = 0x34, kFramesDecoded = 0x58, kFramesPerSecond = 0xB4;
constexpr uint32_t kFrameBuffer = 0xBC;
constexpr int kStatusPlaying = 1, kStatusLooped = 5, kStatusStopped = 2;

bool g_in_command = false;  // inside CL_PlayCinematic_f: the film it starts may be skipped

// What the log knows of each slot (main thread only: the films run from the client frame).
struct Film {
  bool timing = false;          // between a start (or a loop's restart) and its end
  std::string name;
  Clock::time_point started{};
  int32_t first_frame = 0;      // frames decoded when the timing started
  int32_t last_frames = 0;      // the highest count seen, in case the end clears it
  int32_t fps = 0;              // the film's rate and clock, read at the start (the end may clear the slot)
  int32_t sound_clock = 0;
  bool skippable = false;       // started by the `cinematic` command, not yet stopped by codclassic_skip_films
};
Film g_films[kSlotCount];

int32_t ReadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return static_cast<int32_t>(__builtin_bswap32(value));
}

int32_t SlotField(const uint8_t* base, int handle, uint32_t offset) {
  return ReadGuest32(base, kSlots + static_cast<uint32_t>(handle) * kSlotSize + offset);
}

std::string GuestString(const uint8_t* base, uint32_t address) {
  if (address == 0) return {};
  const char* text = reinterpret_cast<const char*>(base + address);
  return std::string(text, strnlen(text, 128));
}

const char* ClockName(int32_t sound_clock) { return sound_clock != 0 ? "its sound" : "real time"; }

void StartTiming(Film& film, const uint8_t* base, int handle) {
  film.timing = true;
  film.started = Clock::now();
  film.first_frame = SlotField(base, handle, kFramesDecoded);
  film.last_frames = film.first_frame;
  film.fps = SlotField(base, handle, kFramesPerSecond);
  film.sound_clock = SlotField(base, handle, kSoundClock);
}

// Logs the pass that just ended: the frames shown, the time they took and the speed against the film's rate.
void LogPass(const Film& film, const char* how) {
  const double seconds = std::chrono::duration<double>(Clock::now() - film.started).count();
  const int32_t shown = film.last_frames - film.first_frame;
  const double own_seconds = film.fps > 0 ? static_cast<double>(shown) / film.fps : 0.0;
  REXLOG_INFO("codclassic: film {} {}: {} frames in {:.2f} s, {:.2f} s at its own {} frames a second: "
              "speed {:.2f} (follows {})",
              film.name, how, shown, seconds, own_seconds, film.fps, seconds > 0.0 ? own_seconds / seconds : 0.0,
              ClockName(film.sound_clock));
}

}  // namespace

REX_HOOK_RAW(CODCLASSIC_CIN_COMMAND) {
  g_in_command = true;
  CODCLASSIC_FILM_IMP(CODCLASSIC_CIN_COMMAND)(ctx, base);
  g_in_command = false;
}

REX_HOOK_RAW(CODCLASSIC_CIN_PLAY) {
  const std::string name = GuestString(base, ctx.r3.u32);
  CODCLASSIC_FILM_IMP(CODCLASSIC_CIN_PLAY)(ctx, base);
  const int handle = static_cast<int32_t>(ctx.r3.u32);
  if (handle >= 0 && handle < kSlotCount) {
    g_films[handle].name = name;
    g_films[handle].skippable = g_in_command && REXCVAR_GET(codclassic_skip_films);
  }
  if (!REXCVAR_GET(codclassic_log_films)) return;
  if (handle < 0 || handle >= kSlotCount) {
    REXLOG_INFO("codclassic: film {} didn't start (handle {})", name, handle);
    return;
  }
  Film& film = g_films[handle];
  StartTiming(film, base, handle);
  REXLOG_INFO("codclassic: film {} starts (slot {}): {} frames a second, follows {}", name, handle, film.fps,
              ClockName(film.sound_clock));
}

REX_HOOK_RAW(CODCLASSIC_CIN_RUN) {
  const int handle = static_cast<int32_t>(ctx.r3.u32);
  const bool known = handle >= 0 && handle < kSlotCount;
  // codclassic_skip_films: once the film has decoded a frame of its own and its frame buffer is up (so
  // CL_PlayCinematic_f's first loop has ended and the rest of its command line, such as `set nextmap ...`, has run),
  // stop it the game's way. The frame count matters: a reused slot keeps the last film's buffer, and r16 (b23)
  // stopped iw_logo before its `set nextmap` ran, which ended the chain early and left nextmap set. One try per film;
  // a refusal (status left as it was) lets it play on.
  if (known && g_films[handle].skippable && SlotField(base, handle, kFramesDecoded) >= 1 &&
      SlotField(base, handle, kFrameBuffer) != 0 && SlotField(base, handle, kStatus) == kStatusPlaying) {
    Film& film = g_films[handle];
    film.skippable = false;
    const int32_t frames = SlotField(base, handle, kFramesDecoded);
    CODCLASSIC_CIN_STOP(ctx, base);  // r3 is still the handle
    const int status = static_cast<int32_t>(ctx.r3.u32);
    const std::string what =
        status == kStatusStopped ? std::string("skipped") : "not skipped (status " + std::to_string(status) + ")";
    REXLOG_INFO("codclassic: film {} {} after {} frames (codclassic_skip_films)", film.name, what, frames);
    if (status == kStatusStopped) {
      film.timing = false;
      return;  // r3 = 2, what CIN_RunCinematic itself returns for a film that has ended
    }
    ctx.r3.u32 = static_cast<uint32_t>(handle);
  }
  if (known && g_films[handle].timing) {
    const int32_t frames = SlotField(base, handle, kFramesDecoded);
    if (frames > g_films[handle].last_frames) g_films[handle].last_frames = frames;
  }
  CODCLASSIC_FILM_IMP(CODCLASSIC_CIN_RUN)(ctx, base);
  if (!known || !REXCVAR_GET(codclassic_log_films)) return;
  Film& film = g_films[handle];
  if (!film.timing) return;
  // The frames this last call decoded (an end may reset the count, so the highest seen counts).
  const int32_t frames = SlotField(base, handle, kFramesDecoded);
  if (frames > film.last_frames) film.last_frames = frames;
  const int status = static_cast<int32_t>(ctx.r3.u32);
  if (status == kStatusLooped) {
    LogPass(film, "loops");
    StartTiming(film, base, handle);
  } else if (status != kStatusPlaying) {
    LogPass(film, "ends");
    film.timing = false;
  }
}
