// cod3 - [acct2-cod3-fx] 2026-11-07 the game's effects, measured frame by frame (owner 2026-11-07 22:15: "various
// effects are flickering or wrong. Smoke being the prime one."). Both programs. dev/agents/acct2-cod3-fx/NOTES.md.
//
// Smoke, dust, sparks and muzzle flashes all reach the GPU through one quad batcher (campaign 0x820FAED0, cod3mp
// 0x8210CDF8; acct2-bro's notes, side104 and side110): it draws its quads (the count at 0x8290B5AC / 0x82A8843C, the
// first index at 0x8290B5B4 / 0x82A88444) with four corners of 12 words each from vertex fetch constant 95 (a position
// in the world, a D3DCOLOR at word 3, the texture coordinates at words 7-8). Each call is one flush; a frame has as many
// flushes as it has kinds of effect (texture, blend) on screen.
//
// With cod3_debug_fx_frames on, every flush is counted into the drawn frame it belongs to (ngl's frame counter,
// 0x8290F28C / 0x82A8C1A0, one step a drawn frame) with its quads and their opacity (the corners' alpha, 0..1, summed
// over a quad's four corners / 4). Once a second:
//   "cod3: fx: 1.00 s, F frames (U with a game update): quads a frame mean M (min A, max B), opacity a frame mean O;
//    frame-to-frame change of the opacity: all X %, after an update Y %, between updates Z %; E frames without
//    effects; S split; live effects L"
// A frame's opacity is what the effects add to the picture; smoke that flickers on screen changes it from one frame to
// the next by much more than smoke that only drifts. "between updates" is the frames without a game update
// (cod3_server_hz): if effects flicker there and not after an update, a step of the game (the systems
// server_step.cpp runs in its updates) feeds them. "split" counts frames whose flushes came less than 0.1 ms after the
// last frame's: the counter stepped inside one frame's drawing, so that pair is one frame (a check of the counting).
// cod3_debug_fx_frame_lines N also writes one line a drawn frame for N frames.
//
// The picture itself (cod3_debug_fx_burst N): cod3_debug_fx_burst_delay seconds after the first frame with effects, N
// pictures back to back from the GPU emulation's output (the presenter's CaptureGuestOutput, as screenshots take it;
// nothing is saved), each cut into 8 x 6 cells and each cell's mean brightness (0-255) logged:
//   "cod3: fx burst K +T ms frame F: mean M; cells c0 .. c47"
// and at the end, per cell, how much it changes from one picture to the next and how often the change turns around
// (flicker turns around nearly every picture; drifting smoke and a moving view mostly don't):
//   "cod3: fx burst done: N pictures in T ms (P a second); change between pictures: all cells X; most: (col row) d
//    turns t % ..."
//
// [acct2-cod3-fx] 2026-11-07 movies (owner 22:33, about Big Red One: its movies play faster than they should, "every
// game checked"). CoD3's movie player (XMEDIA's XMV player, campaign only, docs/COD3_LIBRARIES.md; hook points from
// dev/x360-family/libs/codec_NOTES.md) runs on threads of its own: the WMV plug-in decodes (Decode 0x8220D488), the
// video renderer draws each frame (0x821CDC88: InsertCallback and DrawVertices), the player thread presents
// (0x821DC7F0) and the audio renderer feeds its own XAudio voice at 44.1 kHz (submit 0x821CD3F0). With
// cod3_debug_movie_rate on, once a second while a movie plays:
//   "cod3: movie: 1.00 s: F frames drawn, D decoded, P presents, A audio buffers"
// so the frames shown per real second can be held against the movie's own rate, at any frame rate cap.
// Counting only: nothing the game does or draws changes.
//
// [acct2-cod3-fx] 2026-11-07 23:30 the clocks the movies run on (CoD3's movies play ~17 % fast at the 1000 cap and
// at their own rate at 60, runs mov60 / mov1000). The player and the WMV decoder read the XDK's
// QueryPerformanceCounter (0x82345740: mftb, the guest's 50 MHz tick count; called from 0x821D0E20 and 0x82216280 ..
// 0x8221E180); the game's own clock wrapper 0x82127800 reads it hundreds of times a drawn frame
// (profile_timers.cpp). cod3_debug_guest_clock (also on with cod3_debug_movie_rate), once a second:
//   "cod3: clock: S s: guest ticks X x real, guest system time Y x real; QueryPerformanceCounter N calls (M from the
//    movie player and its decoder, which saw Z x real)"
// 1.000 is real time (the host's steady clock). cod3_debug_guest_clock_stress N adds N threads that read the guest's
// clock in a loop, to see whether reading it often from many threads makes it run fast.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>  // the flushing thread's name, for the log
#endif

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/presenter.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_debug_fx_frames, false, "cod3",
                    "Log once a second how the game's effects (smoke, dust, sparks) change from one drawn frame to "
                    "the next: their quads and opacity a frame, frames with and without a game update")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_fx_frame_lines, 0, "cod3",
                     "With cod3_debug_fx_frames, also one log line a drawn frame for this many frames (0: none)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_fx_burst, 0, "cod3",
                     "Dev test: this many pictures back to back from the game's output, cod3_debug_fx_burst_delay "
                     "seconds after the first frame with effects, each one's brightness in 8 x 6 cells logged (how "
                     "much smoke and other effects flicker on screen); 0: none")
    .range(0, 2000);
// [acct2-cod3-fx] 2026-11-07 22:58 the particle step after the last frame's effects were drawn (see the hook below).
REXCVAR_DEFINE_BOOL(cod3_fx_step_after_draw, false, "cod3/Video",
                    "Smoke, dust and sparks are stepped to the next frame only once the job workers have drawn the "
                    "last frame's (as at 60 fps); off: above 60 fps the next frame's step can run while they still "
                    "draw them, as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_movie_rate, false, "cod3",
                    "Log once a second while a movie plays how many of its frames were decoded, drawn and presented "
                    "and how many sound buffers it sent (is it shown at its own speed?)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_guest_clock, false, "cod3",
                    "Log once a second how fast the guest's clock runs against real time (its 50 MHz tick count, "
                    "which QueryPerformanceCounter and the movie player read, and its system time; 1.000 is real "
                    "time); also on with cod3_debug_movie_rate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_guest_clock_stress, 0, "cod3",
                     "Dev test: with cod3_debug_guest_clock, this many extra threads read the guest's clock in a loop "
                     "(does it run fast when many threads read it often?); 0: none")
    .range(0, 8);
REXCVAR_DEFINE_DOUBLE(cod3_debug_fx_burst_delay, 20.0, "cod3",
                      "Seconds from the first frame with effects to cod3_debug_fx_burst's pictures")
    .range(0.0, 600.0);
REXCVAR_DEFINE_INT32(cod3_debug_fx_burst_every, 0, "cod3",
                     "With cod3_debug_fx_burst: the burst again every this many seconds, at most 8 times (0: once)")
    .range(0, 600);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

#if defined(COD3_MP)
constexpr uint32_t kQuadCount = 0x82A8843C;
constexpr uint32_t kFirstIndex = 0x82A88444;
constexpr uint32_t kDevicePointer = 0x82A8C0FC;
constexpr uint32_t kFrameCounter = 0x82A8C1A0;
constexpr uint32_t kEffectList = 0;  // not mapped in cod3mp
constexpr uint32_t kLevelTime = 0;   // cod3mp runs its game every drawn frame (cod3mp_server_hz 0)
#else
constexpr uint32_t kQuadCount = 0x8290B5AC;
constexpr uint32_t kFirstIndex = 0x8290B5B4;
constexpr uint32_t kDevicePointer = 0x8290F24C;
constexpr uint32_t kFrameCounter = 0x8290F28C;
constexpr uint32_t kEffectList = 0x82AA4D54;     // the live effects (acct2-main, effects_debug.cpp): count at +8
constexpr uint32_t kLevelTime = 0x82A4E790 + 156;  // level.time (server_step.cpp)
#endif
constexpr uint32_t kDeviceFetchConstants = 0x0400;  // 24 bytes a texture fetch constant, 8 a vertex one
constexpr uint32_t kCornerFetch = 95;               // the batcher's corners (vertex fetch constant 95)
constexpr uint32_t kCornerWords = 12;
constexpr uint32_t kMaxQuads = 4096;
constexpr uint32_t kMaxEmptyFrames = 8;             // frames without a flush counted between two with one

void MaybeStartBurst(uint8_t* base);  // cod3_debug_fx_burst, below

bool HeapPointer(uint32_t p) { return p >= 0x40000000u && p < 0x80000000u && (p & 3) == 0; }
uint32_t GuestAddress(uint32_t address) { return address < 0x20000000u ? (0xA0000000u | address) : address; }
bool Physical(uint32_t address) { return address >= 0xA0000000u && address < 0xFFF00000u; }

struct Frame {
  uint32_t flushes = 0, quads = 0;
  double opacity = 0.0;
  bool update = false;
  bool split = false;
};

struct Second {
  Clock::time_point since = Clock::now();
  uint64_t frames = 0, updates = 0, empty = 0, splits = 0;
  uint64_t flushes = 0, overlapped = 0;  // flushes, and those while the particles stepped (0x82430108) elsewhere
  uint64_t quads = 0;
  uint32_t quads_min = UINT32_MAX, quads_max = 0;
  double opacity = 0.0;
  double change_all = 0.0, change_update = 0.0, change_between = 0.0;
  uint64_t pairs_update = 0, pairs_between = 0;
};

std::atomic<uint64_t> g_pool_allocs{0}, g_pool_failed{0};  // apsPoolSet_Alloc (0x820F86C8): particles and parts made
std::atomic<int> g_stepping{0};  // the particle step (0x82430108, in RE_RenderScene) is running on some thread
std::atomic<int> g_in_flush{0};                 // flushes running now (on any thread)
std::atomic<uint64_t> g_concurrent{0};          // flushes that started while another thread was in one
thread_local bool t_stepping = false;
thread_local bool t_flusher = false;  // this thread has flushed the batcher: a job worker (they draw the effects)

std::mutex g_mutex;
bool g_started = false;
uint32_t g_counter = 0;       // the frame counter of g_frame
uint32_t g_level_time = 0;    // level.time at the last frame's first flush
Frame g_frame;                // the frame being counted
double g_last_opacity = -1.0; // the frame before it (-1: none yet)
Clock::time_point g_last_flush{};
Second g_second;
int64_t g_lines_left = -1;    // per-frame lines still to write (-1: take the setting)
uint64_t g_frame_number = 0;

// The quads' opacity in one flush, read from its corners (0 when they can't be read).
double FlushOpacity(uint8_t* base, uint32_t quads, uint32_t first_index) {
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!HeapPointer(device) || quads == 0 || quads > kMaxQuads) return 0.0;
  const uint32_t at = device + kDeviceFetchConstants + 8 * kCornerFetch;
  const uint32_t word0 = g::ReadU32(base, at), word1 = g::ReadU32(base, at + 4);
  if ((word0 & 3) != 3) return 0.0;  // not a vertex fetch constant
  const uint32_t vertices = GuestAddress(word0 & 0x1FFFFFFC);
  const uint32_t words = (word1 >> 2) & 0xFFFFFF, endian = word1 & 3;
  const uint32_t first_corner = first_index / 6 * 4;
  if (!Physical(vertices) || uint64_t{first_corner + 4 * quads} * kCornerWords > words) return 0.0;
  uint64_t alpha = 0;
  for (uint32_t c = first_corner; c < first_corner + 4 * quads; ++c) {
    uint32_t color = g::ReadU32(base, vertices + 4 * (kCornerWords * c + 3));  // a D3DCOLOR, 0xAARRGGBB
    if (endian == 0) color = __builtin_bswap32(color);
    alpha += color >> 24;
  }
  return double(alpha) / (255.0 * 4.0);
}

// A finished frame into the second's sums (under g_mutex).
void EndFrame(const Frame& f) {
  Second& s = g_second;
  ++s.frames;
  s.updates += f.update;
  s.empty += f.flushes == 0;
  s.splits += f.split;
  s.quads += f.quads;
  s.quads_min = std::min(s.quads_min, f.quads);
  s.quads_max = std::max(s.quads_max, f.quads);
  s.opacity += f.opacity;
  if (g_last_opacity >= 0.0) {
    const double change = std::fabs(f.opacity - g_last_opacity);
    s.change_all += change;
    if (f.update) {
      s.change_update += change;
      ++s.pairs_update;
    } else {
      s.change_between += change;
      ++s.pairs_between;
    }
  }
  g_last_opacity = f.opacity;
  ++g_frame_number;
  if (g_lines_left < 0) g_lines_left = REXCVAR_GET(cod3_debug_fx_frame_lines);
  if (g_lines_left > 0) {
    --g_lines_left;
    REXLOG_INFO("cod3: fx frame {}: {}, {} flushes, {} quads, opacity {:.2f}{}", g_frame_number,
                f.update ? "update" : "between", f.flushes, f.quads, f.opacity, f.split ? ", split" : "");
  }
}

void SecondLine(uint8_t* base) {
  Second& s = g_second;
  const double secs = std::chrono::duration<double>(Clock::now() - s.since).count();
  if (secs < 1.0) return;
  if (s.frames > 0) {
    const double mean_opacity = s.opacity / double(s.frames);
    const auto percent = [mean_opacity](double sum, uint64_t n) {
      return n && mean_opacity > 0.0 ? 100.0 * sum / double(n) / mean_opacity : 0.0;
    };
    const uint64_t pairs = s.pairs_update + s.pairs_between;
    const uint32_t live = kEffectList ? g::ReadU32(base, kEffectList + 8) : 0;
    REXLOG_INFO("cod3: fx: {:.2f} s, {} frames ({} with a game update): quads a frame mean {:.1f} (min {}, max {}), "
                "opacity a frame mean {:.2f}; frame-to-frame change of the opacity: all {:.1f} %, after an update "
                "{:.1f} %, between updates {:.1f} %; {} frames without effects; {} split; live effects {}; {} of {} "
                "flushes while the particles stepped on another thread, {} while another thread flushed; pool allocations {} ({} "
                "failed)",
                secs, s.frames, s.updates, double(s.quads) / double(s.frames), s.quads_min, s.quads_max,
                mean_opacity, percent(s.change_all, pairs), percent(s.change_update, s.pairs_update),
                percent(s.change_between, s.pairs_between), s.empty, s.splits, live, s.overlapped, s.flushes,
                g_concurrent.exchange(0), g_pool_allocs.exchange(0), g_pool_failed.exchange(0));
  }
  s = Second{};
}

// One flush of the batcher: its quad count and first index as it starts (it clears them), its corners after it drew
// (it sets vertex fetch constant 95 to them for its draw).
// Which thread flushes (the game's main thread builds the frame and steps the particles in RE_RenderScene; a flush on
// another thread reads particles the next frame may already be stepping). Logged when it changes, at most 8 times.
void NoteFlushThread() {
  static thread_local bool noted = false;
  static std::atomic<int> lines{0};
  if (noted) return;
  noted = true;
  if (lines.fetch_add(1, std::memory_order_relaxed) >= 8) return;
  char name[32] = "?";
#if !defined(_WIN32)
  pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
  REXLOG_INFO("cod3: fx: the effect batcher flushes on thread '{}'", name);
}

void OnFlush(uint8_t* base, uint32_t quads, uint32_t first_index) {
  if (quads == 0 || quads > kMaxQuads) return;  // nothing drawn
  MaybeStartBurst(base);
  if (!REXCVAR_GET(cod3_debug_fx_frames)) return;
  NoteFlushThread();
  const bool overlapped = g_stepping.load(std::memory_order_relaxed) > 0 && !t_stepping;
  const double opacity = FlushOpacity(base, quads, first_index);
  const uint32_t counter = g::ReadU32(base, kFrameCounter);
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_started) {
    g_started = true;
    g_counter = counter;
    g_level_time = kLevelTime ? g::ReadU32(base, kLevelTime) : 0;
    g_frame = Frame{};
    g_frame.update = true;
  } else if (counter != g_counter) {
    const bool split = now - g_last_flush < std::chrono::microseconds(100);
    EndFrame(g_frame);
    const uint32_t skipped = std::min(counter - g_counter - 1, kMaxEmptyFrames);
    for (uint32_t i = 0; i < skipped && counter - g_counter < 1000; ++i) EndFrame(Frame{});
    g_counter = counter;
    g_frame = Frame{};
    g_frame.split = split;
    const uint32_t level_time = kLevelTime ? g::ReadU32(base, kLevelTime) : 0;
    g_frame.update = !kLevelTime || level_time != g_level_time;
    g_level_time = level_time;
    SecondLine(base);
  }
  ++g_frame.flushes;
  ++g_second.flushes;
  g_second.overlapped += overlapped;
  g_frame.quads += quads;
  g_frame.opacity += opacity;
  g_last_flush = now;
}


// ---------------------------------------------------------------------------------------------------------------
// cod3_debug_fx_burst: the picture's brightness in cells, picture after picture (see the top).
constexpr int kCellsX = 8, kCellsY = 6, kCells = kCellsX * kCellsY;
std::atomic<bool> g_burst_started{false};

rex::ui::Presenter* GamePresenter() {
  auto* kernel = rex::system::kernel_state();
  auto* runtime = kernel ? kernel->emulator() : nullptr;
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics ? graphics->presenter() : nullptr;
}

// The mean brightness of each cell (0-255) and of the whole picture; false without a picture.
bool CellMeans(const rex::ui::RawImage& image, float cells[kCells], float& mean) {
  if (image.width < uint32_t(kCellsX) || image.height < uint32_t(kCellsY) || image.stride < size_t(image.width) * 4 ||
      image.data.size() < image.stride * (image.height - 1) + size_t(image.width) * 4)
    return false;
  double sums[kCells] = {};
  uint64_t counts[kCells] = {};
  for (uint32_t y = 0; y < image.height; y += 2) {
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    const int cy = int(uint64_t(y) * kCellsY / image.height);
    for (uint32_t x = 0; x < image.width; x += 2) {
      const uint8_t* p = row + size_t(x) * 4;
      const int cx = int(uint64_t(x) * kCellsX / image.width);
      sums[cy * kCellsX + cx] += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
      ++counts[cy * kCellsX + cx];
    }
  }
  double all = 0.0;
  for (int i = 0; i < kCells; ++i) {
    cells[i] = counts[i] ? float(sums[i] / double(counts[i])) : 0.0f;
    all += cells[i];
  }
  mean = float(all / kCells);
  return true;
}

void OneBurst(uint8_t* base, int pictures);

void RunBurst(uint8_t* base, int pictures, double delay_s) {
  std::this_thread::sleep_for(std::chrono::duration<double>(delay_s));
  const int every = REXCVAR_GET(cod3_debug_fx_burst_every);
  for (int round = 0; round < (every > 0 ? 8 : 1); ++round) {
    if (round > 0) std::this_thread::sleep_for(std::chrono::seconds(every));
    OneBurst(base, pictures);
  }
}

void OneBurst(uint8_t* base, int pictures) {
  rex::ui::Presenter* presenter = GamePresenter();
  if (!presenter) {
    REXLOG_INFO("cod3: fx burst: no presenter");
    return;
  }
  std::vector<std::array<float, kCells>> shots;
  shots.reserve(size_t(pictures));
  const auto start = Clock::now();
  for (int k = 0; k < pictures; ++k) {
    rex::ui::RawImage image;
    std::array<float, kCells> cells{};
    float mean = 0.0f;
    if (!presenter->CaptureGuestOutput(image) || !CellMeans(image, cells.data(), mean)) continue;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::string text;
    char item[16];
    for (int i = 0; i < kCells; ++i) {
      std::snprintf(item, sizeof(item), " %.0f", cells[i]);
      text += item;
    }
    REXLOG_INFO("cod3: fx burst {} +{:.1f} ms frame {}: mean {:.1f}; cells{}", shots.size(), ms,
                g::ReadU32(base, kFrameCounter), mean, text);
    shots.push_back(cells);
  }
  const double total_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  if (shots.size() < 3) {
    REXLOG_INFO("cod3: fx burst done: only {} pictures", shots.size());
    return;
  }
  // Per cell: the mean change between pictures and how often it turns around (+ then -, or - then +).
  double change[kCells] = {}, turns[kCells] = {}, all = 0.0;
  for (int i = 0; i < kCells; ++i) {
    double last = 0.0;
    for (size_t k = 1; k < shots.size(); ++k) {
      const double d = double(shots[k][i]) - double(shots[k - 1][i]);
      change[i] += std::fabs(d);
      if (k > 1 && d * last < 0.0 && std::fabs(d) > 0.5 && std::fabs(last) > 0.5) turns[i] += 1.0;
      if (std::fabs(d) > 0.5) last = d;
    }
    change[i] /= double(shots.size() - 1);
    turns[i] = 100.0 * turns[i] / double(shots.size() - 2);
    all += change[i];
  }
  int order[kCells];
  for (int i = 0; i < kCells; ++i) order[i] = i;
  std::sort(order, order + kCells, [&change](int a, int b) { return change[a] > change[b]; });
  std::string most;
  char item[64];
  for (int j = 0; j < 8; ++j) {
    const int i = order[j];
    std::snprintf(item, sizeof(item), " (%d %d) %.2f turns %.0f %%", i % kCellsX, i / kCellsX, change[i], turns[i]);
    most += item;
  }
  REXLOG_INFO("cod3: fx burst done: {} pictures in {:.0f} ms ({:.0f} a second); change between pictures: all cells "
              "{:.2f}; most:{}",
              shots.size(), total_ms, 1000.0 * double(shots.size()) / total_ms, all / kCells, most);
}

void MaybeStartBurst(uint8_t* base) {
  const int pictures = REXCVAR_GET(cod3_debug_fx_burst);
  if (pictures <= 0 || g_burst_started.exchange(true)) return;
  const double delay_s = REXCVAR_GET(cod3_debug_fx_burst_delay);
  REXLOG_INFO("cod3: fx burst: {} pictures in {:.0f} s", pictures, delay_s);
  std::thread(RunBurst, base, pictures, delay_s).detach();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// cod3_debug_guest_clock: the guest's clock against the host's steady clock (see the top).
namespace {

std::atomic<uint64_t> g_qpc_calls{0}, g_qpc_movie_calls{0};
std::mutex g_qpc_movie_mutex;
uint64_t g_qpc_movie_first = 0, g_qpc_movie_last = 0;  // the counts the movie code read this second
Clock::time_point g_qpc_movie_first_at{}, g_qpc_movie_last_at{};

bool GuestClockWanted() { return REXCVAR_GET(cod3_debug_guest_clock) || REXCVAR_GET(cod3_debug_movie_rate); }

void GuestClockLine(double real, uint64_t ticks, uint64_t system) {
  const double frequency = double(rex::chrono::Clock::guest_tick_frequency());
  double movie_rate = 0.0;
  {
    std::lock_guard<std::mutex> lock(g_qpc_movie_mutex);
    const double movie_real = std::chrono::duration<double>(g_qpc_movie_last_at - g_qpc_movie_first_at).count();
    if (movie_real > 0.1 && g_qpc_movie_last > g_qpc_movie_first)
      movie_rate = double(g_qpc_movie_last - g_qpc_movie_first) / frequency / movie_real;
    g_qpc_movie_first = g_qpc_movie_last = 0;
  }
  REXLOG_INFO("cod3: clock: {:.2f} s: guest ticks {:.4f} x real, guest system time {:.4f} x real; "
              "QueryPerformanceCounter {} calls ({} from the movie player and its decoder, which saw {:.4f} x real)",
              real, double(ticks) / frequency / real, double(system) / 1e7 / real, g_qpc_calls.exchange(0),
              g_qpc_movie_calls.exchange(0), movie_rate);
}

void GuestClockThread() {
  uint64_t ticks = rex::chrono::Clock::QueryGuestTickCount();
  uint64_t system = rex::chrono::Clock::QueryGuestSystemTime();
  auto at = Clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint64_t ticks_now = rex::chrono::Clock::QueryGuestTickCount();
    const uint64_t system_now = rex::chrono::Clock::QueryGuestSystemTime();
    const auto now = Clock::now();
    const double real = std::chrono::duration<double>(now - at).count();
    if (real > 0.0 && GuestClockWanted()) GuestClockLine(real, ticks_now - ticks, system_now - system);
    ticks = ticks_now;
    system = system_now;
    at = now;
  }
}

void GuestClockStressThread() {
  for (;;) rex::chrono::Clock::QueryGuestTickCount();
}

// From the hooks the game calls in menus, movies and play: starts the once-a-second line (and the stress threads).
void MaybeStartGuestClock() {
  static std::atomic<bool> started{false};
  if (started.load(std::memory_order_relaxed) || !GuestClockWanted() || started.exchange(true)) return;
  std::thread(GuestClockThread).detach();
  const int stress = REXCVAR_GET(cod3_debug_guest_clock_stress);
  if (stress > 0) REXLOG_INFO("cod3: clock: {} threads read the guest's clock in a loop", stress);
  for (int i = 0; i < stress; ++i) std::thread(GuestClockStressThread).detach();
}

}  // namespace

#if !defined(COD3_MP)
// The XDK's QueryPerformanceCounter (0x82345740; r3: where the 64-bit count goes): counted, and for the calls from
// the movie player and the WMV decoder (return address in 0x821C0000 .. 0x821E0000 or 0x82200000 .. 0x82220000) the
// counts they get, against the host's clock.
REX_EXTERN(__imp__QueryPerformanceCounter_guest);
REX_HOOK_RAW(QueryPerformanceCounter_guest) {
  if (!GuestClockWanted()) {
    __imp__QueryPerformanceCounter_guest(ctx, base);
    return;
  }
  MaybeStartGuestClock();
  const uint32_t out = ctx.r3.u32;
  const uint32_t from = uint32_t(ctx.lr);
  __imp__QueryPerformanceCounter_guest(ctx, base);
  g_qpc_calls.fetch_add(1, std::memory_order_relaxed);
  const bool movie = (from >= 0x821C0000u && from < 0x821E0000u) || (from >= 0x82200000u && from < 0x82220000u);
  if (!movie || out == 0) return;
  g_qpc_movie_calls.fetch_add(1, std::memory_order_relaxed);
  const uint64_t count = (uint64_t(g::ReadU32(base, out)) << 32) | g::ReadU32(base, out + 4);
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(g_qpc_movie_mutex);
  if (g_qpc_movie_first == 0) {
    g_qpc_movie_first = count;
    g_qpc_movie_first_at = now;
  }
  if (count > g_qpc_movie_last) {
    g_qpc_movie_last = count;
    g_qpc_movie_last_at = now;
  }
}
#endif

// 0x82430108 (cod3mp 0x824481C0, dev/waw-decomp/cod3/cod3_relatives.tsv), from RE_RenderScene once a drawn frame:
// the effects' clock (0x82A2ACD8) advanced by the frame's seconds (Com_Frame's time block 0x825F4E08 + 12, clamped),
// then the particle systems stepped (0x8242FC80) and listed.
//
// cod3_fx_step_after_draw: the effects are drawn by the game's four job workers (their batcher flushes, measured with
// cod3_debug_fx_frames), and at 60 fps they are done before the next frame's RE_RenderScene steps the particles (0 of
// 4,000-14,000 flushes a second overlapped, run acct2-cod3-fx-c60). Above 60 fps the frames come back to back and the
// step started while the workers still drew the last frame's particles from the same arrays (0.4-3.5 % of the flushes
// at ~110 fps, run a250). With the setting the main thread first waits for the queued jobs (0x8212A110 with -1, the
// game's own wait, which the main thread does every frame for its render job anyway), so drawing and stepping never
// overlap. Not on a job worker (it would wait for itself).
static bool OnJobWorker() {
  if (t_flusher) return true;  // it drew effects: a job worker
#if !defined(_WIN32)
  static thread_local int worker = -1;
  if (worker < 0) {
    char name[32] = "";
    pthread_getname_np(pthread_self(), name, sizeof(name));
    worker = std::strncmp(name, "XThread8212A", 12) == 0 ? 1 : 0;  // the campaign's job workers (0x8212A520's)
  }
  return worker == 1;
#else
  return true;  // no thread names to tell: never wait
#endif
}

// Before the particle step: whether to wait for the queued jobs first (cod3_fx_step_after_draw).
static bool WaitBeforeFxStep() { return REXCVAR_GET(cod3_fx_step_after_draw) && !OnJobWorker(); }

// cod3_debug_fx_frames: the step's thread (logged once) and the flushes that overlap it.
static bool FxStepBegin() {
  if (!REXCVAR_GET(cod3_debug_fx_frames)) return false;
  static std::atomic<int> named{0};
  if (named.fetch_add(1, std::memory_order_relaxed) == 0) {
    char name[32] = "?";
#if !defined(_WIN32)
    pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
    REXLOG_INFO("cod3: fx: the particle step runs on thread '{}'", name);
  }
  g_stepping.fetch_add(1, std::memory_order_relaxed);
  t_stepping = true;
  return true;
}
static void FxStepEnd() {
  t_stepping = false;
  g_stepping.fetch_sub(1, std::memory_order_relaxed);
}

#if defined(COD3_MP)
REX_EXTERN(sub_82141E50);  // 0x8212A110's twin (mp_aliases.h)
REX_EXTERN(__imp__sub_824481C0);
REX_HOOK_RAW(sub_824481C0) {
  if (WaitBeforeFxStep()) {
    const uint64_t r3 = ctx.r3.u64;
    ctx.r3.s64 = -1;
    sub_82141E50(ctx, base);
    ctx.r3.u64 = r3;
  }
  MaybeStartGuestClock();
  const bool counted = FxStepBegin();
  __imp__sub_824481C0(ctx, base);
  if (counted) FxStepEnd();
}
#else
REX_EXTERN(sub_8212A110);
REX_EXTERN(__imp__sub_82430108);
REX_HOOK_RAW(sub_82430108) {
  if (WaitBeforeFxStep()) {
    const uint64_t r3 = ctx.r3.u64;
    ctx.r3.s64 = -1;
    sub_8212A110(ctx, base);
    ctx.r3.u64 = r3;
  }
  MaybeStartGuestClock();
  const bool counted = FxStepBegin();
  __imp__sub_82430108(ctx, base);
  if (counted) FxStepEnd();
}
#endif

#if !defined(COD3_MP)
// apsPoolSet_Alloc (0x820F86C8; Big Red One's name, dev/bro-decomp/cod3_xcheck): the particle system's allocations
// from its pools (it calls apsMemoryPool_Alloc 0x820F8550); 0 back means a full pool. If emitters spawn by the drawn
// frame instead of by time, these grow with the frame rate.
REX_EXTERN(__imp__sub_820F86C8);
REX_HOOK_RAW(sub_820F86C8) {
  __imp__sub_820F86C8(ctx, base);
  if (!REXCVAR_GET(cod3_debug_fx_frames)) return;
  g_pool_allocs.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32 == 0) g_pool_failed.fetch_add(1, std::memory_order_relaxed);
}
#endif

#if !defined(COD3_MP)  // the movie player is in the campaign only
namespace {
std::atomic<uint64_t> g_movie_drawn{0}, g_movie_decoded{0}, g_movie_presents{0}, g_movie_audio{0};
std::mutex g_movie_mutex;
Clock::time_point g_movie_since{};

// After any of the counted calls: one line a second while the counts move.
void MovieSecond() {
  std::lock_guard<std::mutex> lock(g_movie_mutex);
  const auto now = Clock::now();
  if (g_movie_since == Clock::time_point{}) {
    g_movie_since = now;
    return;
  }
  const double secs = std::chrono::duration<double>(now - g_movie_since).count();
  if (secs < 1.0) return;
  if (secs > 3.0) {  // a movie starting after a pause: start the second again
    g_movie_since = now;
    g_movie_drawn = g_movie_decoded = g_movie_presents = g_movie_audio = 0;
    return;
  }
  REXLOG_INFO("cod3: movie: {:.2f} s: {} frames drawn, {} decoded, {} presents, {} audio buffers", secs,
              g_movie_drawn.exchange(0), g_movie_decoded.exchange(0), g_movie_presents.exchange(0),
              g_movie_audio.exchange(0));
  g_movie_since = now;
}
}  // namespace

#define COD3_MOVIE_COUNT(name, counter)              \
  REX_EXTERN(__imp__##name);                         \
  REX_HOOK_RAW(name) {                               \
    if (REXCVAR_GET(cod3_debug_movie_rate)) {        \
      counter.fetch_add(1, std::memory_order_relaxed); \
      MovieSecond();                                 \
      MaybeStartGuestClock();                        \
    }                                                \
    __imp__##name(ctx, base);                        \
  }
COD3_MOVIE_COUNT(sub_821CDC88, g_movie_drawn)     // the video renderer's frame draw
COD3_MOVIE_COUNT(sub_8220D488, g_movie_decoded)   // the WMV plug-in's Decode
COD3_MOVIE_COUNT(sub_821DC7F0, g_movie_presents)  // the player thread's present
COD3_MOVIE_COUNT(sub_821CD3F0, g_movie_audio)     // the audio renderer's buffer submit
#undef COD3_MOVIE_COUNT
#endif

#if defined(COD3_MP)
REX_EXTERN(__imp__sub_8210CDF8);
REX_HOOK_RAW(sub_8210CDF8) {
  t_flusher = true;
  if (!REXCVAR_GET(cod3_debug_fx_frames) && REXCVAR_GET(cod3_debug_fx_burst) <= 0) {
    __imp__sub_8210CDF8(ctx, base);
    return;
  }
  const uint32_t quads = g::ReadU32(base, kQuadCount) & 0x7FFFFFFF;
  const uint32_t first_index = g::ReadU32(base, kFirstIndex) & 0x7FFFFFFF;
  if (g_in_flush.fetch_add(1, std::memory_order_relaxed) > 0) g_concurrent.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8210CDF8(ctx, base);
  g_in_flush.fetch_sub(1, std::memory_order_relaxed);
  OnFlush(base, quads, first_index);
}
#else
REX_EXTERN(__imp__sub_820FAED0);
REX_HOOK_RAW(sub_820FAED0) {
  t_flusher = true;
  if (!REXCVAR_GET(cod3_debug_fx_frames) && REXCVAR_GET(cod3_debug_fx_burst) <= 0) {
    __imp__sub_820FAED0(ctx, base);
    return;
  }
  const uint32_t quads = g::ReadU32(base, kQuadCount) & 0x7FFFFFFF;
  const uint32_t first_index = g::ReadU32(base, kFirstIndex) & 0x7FFFFFFF;
  if (g_in_flush.fetch_add(1, std::memory_order_relaxed) > 0) g_concurrent.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_820FAED0(ctx, base);
  g_in_flush.fetch_sub(1, std::memory_order_relaxed);
  OnFlush(base, quads, first_index);
}
#endif
