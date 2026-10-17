// cod3 - the decomp trace build's game side (game/decomp_trace.h).
//
// Every recompiled function registers its slot here the first time it runs
// (src/game/trace/trace_module.cpp calls cod3_decomp_trace_registry() ->
// RegisterSlot). This file keeps the seen functions, what the game was doing
// at the time (the context), the player's markers and the evidence the
// collectors (trace/evidence.cpp) note, and writes them to
// userdata/decomp_trace/<session>/ every minute and at exit.
//
// What goes into the files: game addresses, game function names, times since
// the session started, the game's own text (dvar names, print formats, game
// file paths, command names) and the player's marker text. No paths of this
// computer, no user or computer names: the writer drops any evidence that
// holds the computer's user name, its host name or the profile name.
//
// Trace build only (src/game/trace/decomp_trace.cmake).

#include "game/decomp_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <imgui.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "game/trace/trace_format.h"
#include "game/trace/trace_internal.h"
#include "ui/game_console.h"

#if defined(_WIN32)
#define COD3_TRACE_EXPORT extern "C" __declspec(dllexport)
#else
#define COD3_TRACE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

COD3_TRACE_EXPORT const cod3::trace::Registry* cod3_decomp_trace_registry();

// The executable's own code (generated/default/cod3_init.cpp).
extern const rex::PPCImageInfo PPCImageConfig;

namespace cod3::trace {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

constexpr std::chrono::seconds kWriteInterval{60};
constexpr uint32_t kTraceFormat = 1;
constexpr size_t kMaxEvidence = 200000;
constexpr size_t kMaxModules = 64;
constexpr std::string_view kMainModule = "default";

// ---------------------------------------------------------------------------
// Modules and their seen functions
// ---------------------------------------------------------------------------

struct ModuleRecord {
  std::string name;
  ModuleInfo* info = nullptr;                // null until one of its functions ran
  std::vector<FunctionName> by_address;      // the module's function table, sorted
  std::unordered_map<std::string_view, uint32_t> by_name;
  bool main = false;
};

std::mutex g_registry_mutex;                 // modules, and slot registration
std::array<std::unique_ptr<ModuleRecord>, kMaxModules> g_modules;
size_t g_module_count = 0;
std::atomic<Slot*> g_slots{nullptr};         // every registered slot (newest first)
std::atomic<uint32_t> g_slot_count{0};
std::atomic<uint32_t> g_current_level{0};    // Registry::current_level
std::atomic<uint32_t> g_context{0};          // what the game is doing now (an index into g_contexts)

// The record of `name`; made on first use. g_registry_mutex held. 0 = full.
uint32_t ModuleIdLocked(std::string_view name) {
  for (size_t i = 0; i < g_module_count; ++i)
    if (g_modules[i]->name == name) return uint32_t(i + 1);
  if (g_module_count == kMaxModules) return 0;
  auto record = std::make_unique<ModuleRecord>();
  record->name = std::string(name);
  record->main = name == kMainModule;
  g_modules[g_module_count++] = std::move(record);
  return uint32_t(g_module_count);
}

void AttachInfoLocked(ModuleRecord& record, ModuleInfo* info) {
  record.info = info;
  record.by_address.assign(info->functions, info->functions + info->function_count);
  std::sort(record.by_address.begin(), record.by_address.end(),
            [](const FunctionName& x, const FunctionName& y) { return x.address < y.address; });
  record.by_name.reserve(record.by_address.size());
  for (const FunctionName& f : record.by_address) record.by_name.emplace(f.name, f.address);
}

void RegisterSlot(Slot* slot) {
  ModuleInfo* info = slot->module;
  if (!info || info->abi_version != kAbiVersion) return;
  std::lock_guard lock(g_registry_mutex);
  uint32_t id = info->id.load(std::memory_order_relaxed);
  if (!id) {
    id = ModuleIdLocked(info->name);
    if (!id) return;
    ModuleRecord& record = *g_modules[id - 1];
    if (!record.info) AttachInfoLocked(record, info);
    info->id.store(id, std::memory_order_release);
    REXLOG_INFO("cod3: decomp trace: module {} ({} functions)", record.name, record.by_address.size());
  }
  ModuleRecord& record = *g_modules[id - 1];
  std::string_view name = slot->name ? slot->name : "";
  if (name.rfind("__imp__", 0) == 0) name.remove_prefix(7);
  const auto it = record.by_name.find(name);
  slot->address = it != record.by_name.end() ? it->second : 0;
  slot->first_ms = NowMs();
  slot->first_context = g_context.load(std::memory_order_relaxed);
  slot->evidence = record.main && slot->address ? EvidenceFor(slot->address) : nullptr;
  slot->next = g_slots.load(std::memory_order_relaxed);
  g_slots.store(slot, std::memory_order_release);
  g_slot_count.fetch_add(1, std::memory_order_relaxed);
}

const Registry& TheRegistry() {
  static const Registry registry{kAbiVersion, &RegisterSlot, &g_current_level, PPCImageConfig.code_base,
                                 PPCImageConfig.code_size};
  return registry;
}

// ---------------------------------------------------------------------------
// Context: what the game is doing
// ---------------------------------------------------------------------------

struct Context {
  std::string level;
  std::string state;
};
std::mutex g_context_mutex;
std::vector<Context> g_contexts{{"", "starting"}};  // by id
std::map<std::pair<std::string, std::string>, uint32_t> g_context_ids{{{"", "starting"}, 0}};
std::string g_level;  // the current level (g_context_mutex)

// ---------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------

struct Marker {
  uint32_t number = 0;
  uint32_t pressed_ms = 0;  // F8, or the command itself
  uint32_t ms = 0;          // saved
  uint32_t context = 0;
  std::string text;
};
std::mutex g_markers_mutex;
std::vector<Marker> g_markers;

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

struct EvidenceKey {
  uint64_t site;
  uint64_t value_hash;
  uint32_t target;
  Evidence kind;
  bool operator==(const EvidenceKey&) const = default;
};
struct EvidenceKeyHash {
  size_t operator()(const EvidenceKey& k) const {
    uint64_t h = k.site * 0x9E3779B97F4A7C15ull ^ k.value_hash;
    h ^= (uint64_t(k.target) << 8 | uint64_t(k.kind)) * 0xC2B2AE3D27D4EB4Full;
    return size_t(h ^ (h >> 29));
  }
};
struct EvidenceEntry {
  std::string value;
  uint64_t count = 0;
  uint32_t first_ms = 0;
  uint32_t context = 0;
};
std::mutex g_evidence_mutex;
std::unordered_map<EvidenceKey, EvidenceEntry, EvidenceKeyHash> g_evidence;
bool g_evidence_full_logged = false;

uint64_t Fnv1a(std::string_view text) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : text) h = (h ^ c) * 0x100000001B3ull;
  return h;
}

// ---------------------------------------------------------------------------
// Session folder and writer
// ---------------------------------------------------------------------------

std::filesystem::path g_session_dir;
std::string g_session_name;
std::string g_started_utc;
std::mutex g_write_mutex;
std::mutex g_wake_mutex;
std::condition_variable g_wake;
std::atomic<bool> g_writer_started{false};
uint32_t g_written_slots = 0;  // g_write_mutex

}  // namespace

uint32_t NowMs() {
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count());
}

uint64_t CallSite(uint32_t return_address) {
  const Registry& r = TheRegistry();
  uint64_t level = 0;
  if (return_address - r.main_code_base >= r.main_code_size) level = g_current_level.load(std::memory_order_relaxed);
  return (level << 32) | return_address;
}

void NoteEvidence(Evidence kind, uint64_t site, uint32_t target, std::string_view value) {
  if (value.empty() || (uint32_t(site) == 0 && target == 0)) return;
  const EvidenceKey key{site, Fnv1a(value), target, kind};
  const uint32_t now = NowMs();
  std::lock_guard lock(g_evidence_mutex);
  auto it = g_evidence.find(key);
  if (it != g_evidence.end()) {
    ++it->second.count;
    return;
  }
  if (g_evidence.size() >= kMaxEvidence) {
    if (!g_evidence_full_logged) REXLOG_WARN("cod3: decomp trace: evidence table full ({} entries)", kMaxEvidence);
    g_evidence_full_logged = true;
    return;
  }
  g_evidence.emplace(key, EvidenceEntry{std::string(value), 1, now, g_context.load(std::memory_order_relaxed)});
}

void SetContext(std::string_view level, std::string_view state) {
  std::lock_guard lock(g_context_mutex);
  if (!level.empty()) g_level = std::string(level);
  auto key = std::make_pair(g_level, std::string(state));
  auto it = g_context_ids.find(key);
  if (it == g_context_ids.end()) {
    it = g_context_ids.emplace(key, uint32_t(g_contexts.size())).first;
    g_contexts.push_back({key.first, key.second});
  }
  g_context.store(it->second, std::memory_order_relaxed);
}

std::string CurrentLevel() {
  std::lock_guard lock(g_context_mutex);
  return g_level;
}

void SetCurrentLevelModule(std::string_view name) {
  uint32_t id = 0;
  {
    std::lock_guard lock(g_registry_mutex);
    for (size_t i = 0; i < g_module_count; ++i)
      if (!g_modules[i]->main && g_modules[i]->name == name) id = uint32_t(i + 1);
  }
  // Unknown (a level not loaded yet this session): the library's own code
  // sets it once it runs.
  g_current_level.store(id, std::memory_order_relaxed);
}

uint32_t CurrentLevelModule() { return g_current_level.load(std::memory_order_relaxed); }

bool IsMainFunction(uint32_t address) {
  std::lock_guard lock(g_registry_mutex);
  for (size_t i = 0; i < g_module_count; ++i) {
    const ModuleRecord& m = *g_modules[i];
    if (!m.main || !m.info) continue;
    auto it = std::lower_bound(m.by_address.begin(), m.by_address.end(), address,
                               [](const FunctionName& f, uint32_t a) { return f.address < a; });
    return it != m.by_address.end() && it->address == address;
  }
  return false;
}

namespace {

// ---------------------------------------------------------------------------
// Writing the files
// ---------------------------------------------------------------------------

std::string Field(std::string_view text) { return TsvField(text); }
std::string Hex(uint32_t value) { return fmt::format("0x{:08X}", value); }
std::string Seconds(uint32_t ms) { return fmt::format("{:.1f}", ms / 1000.0); }

// Text that would say who or where the player is: the computer's user name,
// its host name and the game profile's name (at least 3 characters each).
std::vector<std::string> PrivateWords() {
  std::vector<std::string> words;
  auto add = [&](const std::string& w) {
    if (w.size() >= 3) words.push_back(Lower(w));
  };
  for (const char* var : {"USER", "USERNAME", "LOGNAME", "COMPUTERNAME", "HOSTNAME"})
    if (const char* v = std::getenv(var)) add(v);
#if !defined(_WIN32)
  char host[256] = {};
  if (gethostname(host, sizeof(host) - 1) == 0) add(host);
#endif
  if (auto* kernel_state = REX_KERNEL_STATE()) {
    if (auto* profile = kernel_state->user_profile()) add(profile->name());
  }
  return words;
}

struct ModuleView {
  uint32_t id = 0;
  const ModuleRecord* record = nullptr;
};

// Where a return address (CallSite form) is: the module and its function.
struct Place {
  const ModuleRecord* module = nullptr;
  const FunctionName* function = nullptr;
  uint32_t site = 0;
};

class Resolver {
 public:
  explicit Resolver(std::vector<ModuleView> modules) : modules_(std::move(modules)) {
    for (const ModuleView& m : modules_)
      if (m.record->main) main_ = m.record;
  }

  const ModuleRecord* ById(uint32_t id) const {
    for (const ModuleView& m : modules_)
      if (m.id == id) return m.record;
    return nullptr;
  }

  Place Find(uint64_t call_site) const {
    Place place;
    place.site = uint32_t(call_site);
    const uint32_t level = uint32_t(call_site >> 32);
    const ModuleRecord* module = nullptr;
    if (main_ && Inside(*main_, place.site)) module = main_;
    else if (level) module = ById(level);
    if (!module || !Inside(*module, place.site)) return place;
    place.module = module;
    place.function = FunctionHolding(module->by_address, place.site);
    return place;
  }

  // "Com_Frame+0x3C", "saint_lo!sub_89031000+0x44", "level?!0x89031044".
  static std::string Text(const Place& p) {
    return CallerText(p.module ? std::string_view(p.module->name) : std::string_view(), p.module && p.module->main,
                      p.function, p.site);
  }

  const ModuleRecord* main() const { return main_; }

 private:
  static bool Inside(const ModuleRecord& m, uint32_t address) {
    return m.info && address - m.info->code_base < m.info->code_size;
  }
  std::vector<ModuleView> modules_;
  const ModuleRecord* main_ = nullptr;
};

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), std::streamsize(text.size()));
    if (!out) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    // Some C runtimes don't replace an existing file on rename.
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
  }
  return !ec;
}

void WriteAll(const char* why) {
  std::lock_guard write_lock(g_write_mutex);
  if (g_session_dir.empty()) return;

  std::vector<ModuleView> modules;
  {
    std::lock_guard lock(g_registry_mutex);
    for (size_t i = 0; i < g_module_count; ++i)
      if (g_modules[i]->info) modules.push_back({uint32_t(i + 1), g_modules[i].get()});
  }
  const Resolver resolver(modules);

  std::vector<const Slot*> slots;
  for (const Slot* s = g_slots.load(std::memory_order_acquire); s; s = s->next) slots.push_back(s);
  auto module_of = [&](const Slot* s) -> const ModuleRecord* {
    return s->module ? resolver.ById(s->module->id.load(std::memory_order_acquire)) : nullptr;
  };
  std::sort(slots.begin(), slots.end(), [&](const Slot* x, const Slot* y) {
    const ModuleRecord* mx = module_of(x);
    const ModuleRecord* my = module_of(y);
    const bool main_x = mx && mx->main, main_y = my && my->main;
    if (main_x != main_y) return main_x;
    const std::string_view nx = mx ? std::string_view(mx->name) : "", ny = my ? std::string_view(my->name) : "";
    if (nx != ny) return nx < ny;
    return x->address < y->address;
  });

  std::vector<Context> contexts;
  {
    std::lock_guard lock(g_context_mutex);
    contexts = g_contexts;
  }
  auto context_of = [&](uint32_t id) -> const Context& { return contexts[id < contexts.size() ? id : 0]; };
  std::vector<Marker> markers;
  {
    std::lock_guard lock(g_markers_mutex);
    markers = g_markers;
  }
  std::vector<uint32_t> marker_counts(markers.size(), 0);
  std::vector<MarkerTime> marker_times;
  for (const Marker& m : markers) marker_times.push_back({m.pressed_ms, m.ms});
  const std::vector<std::string> private_words = PrivateWords();

  // functions.tsv and calls.tsv
  std::string functions =
      "module\taddress\tname\tfirst_seen_s\tlevel\tstate\tmarker\tcalls\tcallers\n";
  std::string calls = "callee_module\tcallee_address\tcallee_name\tcaller_module\tcaller_address\tcaller_name\tcall_site\n";
  struct Coverage {
    uint32_t seen = 0, named = 0;
  };
  std::map<std::string, Coverage> coverage;
  uint64_t total_calls = 0;
  for (const Slot* s : slots) {
    total_calls += s->calls.load(std::memory_order_relaxed);
    const ModuleRecord* m = module_of(s);
    const std::string module = m ? m->name : "?";
    std::string_view name = s->name ? s->name : "";
    if (name.rfind("__imp__", 0) == 0) name.remove_prefix(7);
    const Context& c = context_of(s->first_context);
    const int marker = MarkerFor(marker_times, s->first_ms);
    if (marker >= 0) ++marker_counts[size_t(marker)];
    auto& cov = coverage[module];
    ++cov.seen;
    if (name.rfind("sub_", 0) != 0) ++cov.named;

    std::string callers;
    for (const auto& caller : s->callers) {
      const uint64_t site = caller.load(std::memory_order_relaxed);
      if (!site) continue;
      const Place p = resolver.Find(site);
      if (!callers.empty()) callers += ", ";
      callers += Resolver::Text(p);
      calls += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", module, s->address ? Hex(s->address) : "?", Field(name),
                           p.module ? p.module->name : (p.site >= 0x89000000u ? "level?" : "?"),
                           p.function ? Hex(p.function->address) : "?",
                           p.function ? Field(p.function->name) : "?", Hex(p.site));
    }
    if (s->more_callers.load(std::memory_order_relaxed)) callers += ", ...";
    functions += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", module, s->address ? Hex(s->address) : "?",
                             Field(name), Seconds(s->first_ms), Field(c.level), Field(c.state),
                             marker >= 0 ? Field(fmt::format("{}: {}", markers[size_t(marker)].number,
                                                             markers[size_t(marker)].text))
                                         : "",
                             s->calls.load(std::memory_order_relaxed), Field(callers));
  }

  // modules.tsv
  std::string modules_tsv = "module\tfunctions\tseen\tseen_named\tseen_unnamed\n";
  for (const ModuleView& v : modules) {
    const Coverage cov = coverage[v.record->name];
    modules_tsv += fmt::format("{}\t{}\t{}\t{}\t{}\n", v.record->name, v.record->by_address.size(), cov.seen,
                               cov.named, cov.seen - cov.named);
  }

  // markers.tsv
  std::string markers_tsv = "number\tpressed_s\ttime_s\tlevel\tstate\ttext\tfunctions\n";
  for (size_t i = 0; i < markers.size(); ++i) {
    const Marker& m = markers[i];
    const Context& c = context_of(m.context);
    markers_tsv += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", m.number, Seconds(m.pressed_ms), Seconds(m.ms),
                               Field(c.level), Field(c.state), Field(m.text), marker_counts[i]);
  }

  // evidence.tsv
  struct Row {
    EvidenceKey key;
    EvidenceEntry entry;
  };
  std::vector<Row> rows;
  {
    std::lock_guard lock(g_evidence_mutex);
    rows.reserve(g_evidence.size());
    for (const auto& [key, entry] : g_evidence) rows.push_back({key, entry});
  }
  std::unordered_set<std::string> commands;  // known command names: a typed unknown word isn't kept
  for (const Row& r : rows)
    if (r.key.kind == Evidence::kCommandHandler) commands.insert(Lower(r.entry.value));
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
    if (x.key.kind != y.key.kind) return x.key.kind < y.key.kind;
    return x.entry.first_ms < y.entry.first_ms;
  });
  std::string evidence =
      "kind\tmodule\tfunction_address\tfunction_name\tvalue\tcount\tfirst_seen_s\tlevel\tstate\tcall_site\tvia\n";
  size_t dropped = 0;
  for (const Row& r : rows) {
    if (r.key.kind == Evidence::kCommandRun && !commands.count(r.entry.value)) continue;
    if (HasPrivateWord(r.entry.value, private_words)) {
      ++dropped;
      continue;
    }
    const Place caller = resolver.Find(r.key.site);
    std::string module, address, name, via;
    if (r.key.target) {
      // About another function (a command handler, a level slot's function).
      const Place target = resolver.Find(r.key.target);
      module = target.module ? target.module->name : "?";
      address = Hex(r.key.target);
      name = target.function && target.function->address == r.key.target ? target.function->name : "?";
      if (uint32_t(r.key.site)) via = Resolver::Text(caller);
    } else {
      module = caller.module ? caller.module->name : (caller.site >= 0x89000000u ? "level?" : "?");
      address = caller.function ? Hex(caller.function->address) : "?";
      name = caller.function ? caller.function->name : "?";
    }
    const Context& c = context_of(r.entry.context);
    evidence += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", EvidenceKindName(r.key.kind), module,
                            address, Field(name), Field(r.entry.value), r.entry.count, Seconds(r.entry.first_ms),
                            Field(c.level), Field(c.state), uint32_t(r.key.site) ? Hex(uint32_t(r.key.site)) : "",
                            Field(via));
  }

  // trace.tsv: what this trace is
  std::string info = "key\tvalue\n";
  info += fmt::format("format\t{}\n", kTraceFormat);
  info += fmt::format("session\t{}\n", g_session_name);
  info += fmt::format("started_utc\t{}\n", g_started_utc);
  info += fmt::format("written_s\t{}\n", Seconds(NowMs()));
  info += fmt::format("functions_seen\t{}\n", slots.size());
  info += fmt::format("calls_total\t{}\n", total_calls);
  info += fmt::format("markers\t{}\n", markers.size());
  info += fmt::format("evidence\t{}\n", rows.size() - dropped);
  info += fmt::format("sampling\tthe first {} calls of each function, then 1 in {}\n", kFirstCalls, kSampleMask + 1);
  info += fmt::format("marker_window\tfirst seen from {} s before the marker key to {} s after the marker\n",
                      kMarkerBeforeMs / 1000, kMarkerAfterMs / 1000);

  std::error_code ec;
  std::filesystem::create_directories(g_session_dir, ec);
  bool ok = WriteFile(g_session_dir / "functions.tsv", functions);
  ok &= WriteFile(g_session_dir / "calls.tsv", calls);
  ok &= WriteFile(g_session_dir / "evidence.tsv", evidence);
  ok &= WriteFile(g_session_dir / "markers.tsv", markers_tsv);
  ok &= WriteFile(g_session_dir / "modules.tsv", modules_tsv);
  ok &= WriteFile(g_session_dir / "trace.tsv", info);
  const uint32_t new_slots = uint32_t(slots.size()) - std::min(g_written_slots, uint32_t(slots.size()));
  g_written_slots = uint32_t(slots.size());
  if (ok)
    REXLOG_INFO("cod3: decomp trace: wrote {} functions (+{}), {} evidence, {} markers, {} calls so far ({})",
                slots.size(), new_slots, rows.size() - dropped, markers.size(), total_calls, why);
  else
    REXLOG_WARN("cod3: decomp trace: couldn't write the trace files in userdata/decomp_trace/{}", g_session_name);
}

void WriterLoop() {
  std::unique_lock lock(g_wake_mutex);
  for (;;) {
    g_wake.wait_for(lock, kWriteInterval);
    lock.unlock();
    WriteAll("every minute");
    lock.lock();
  }
}

void AddMarker(std::string text, uint32_t pressed_ms) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
  text.erase(0, start);
  if (text.empty()) text = "(no text)";
  if (text.size() > 200) text.resize(200);
  Marker m;
  m.pressed_ms = pressed_ms;
  m.ms = NowMs();
  m.context = g_context.load(std::memory_order_relaxed);
  m.text = std::move(text);
  {
    std::lock_guard lock(g_markers_mutex);
    m.number = uint32_t(g_markers.size() + 1);
    g_markers.push_back(m);
  }
  REXLOG_INFO("cod3: decomp trace: marker {} at {} s: {}", m.number, Seconds(m.ms), m.text);
  cod3::ui::GameConsole::Get().Print(cod3::ui::ConsoleChannel::kSystem,
                                     fmt::format("Trace marker {} saved: {}\n", m.number, m.text));
}

// ---------------------------------------------------------------------------
// The marker key (F8) and its box
// ---------------------------------------------------------------------------
//
// The key listener takes the typed text itself (not an ImGui text box), so
// nothing typed is lost when frames are slow, e.g. while a level loads; the
// box only shows it. While the box is open the game and the other listeners
// get no keys. All of this runs on the UI thread.

struct MarkerBox {
  bool open = false;
  uint32_t pressed_ms = 0;
  std::string text;               // UTF-8
  bool text_input_before = false;  // the window's text input when the box opened
  // Where the last letter or digit taken from a key press sits in `text`,
  // until the typed character for it (OnKeyChar) replaces it; npos = none.
  size_t key_char_at = std::string::npos;
};
MarkerBox g_marker_box;  // UI thread
rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::Window* g_window = nullptr;
constexpr size_t kMaxMarkerBytes = 200;

class MarkerDialog;
MarkerDialog* g_marker_dialog = nullptr;  // UI thread; the box being drawn

class MarkerDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MarkerDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) { g_marker_dialog = this; }
  ~MarkerDialog() override {
    if (g_marker_dialog == this) g_marker_dialog = nullptr;
  }
  void Dismiss() {
    if (g_marker_dialog == this) g_marker_dialog = nullptr;
    Close();
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_marker_box.open) return;
    const float width = std::min(io.DisplaySize.x - 40.0f, 760.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.2f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.05f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.47f, 0.25f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##cod3_trace_marker", nullptr, flags)) {
      ImGui::TextUnformatted("Trace marker: what just happened?");
      ImGui::TextDisabled("Enter saves it, Esc cancels (e.g. \"planted the bomb\", \"entered the tank\")");
      ImGui::Separator();
      // The typed text with a blinking caret.
      const bool caret = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
      ImGui::TextUnformatted((g_marker_box.text + (caret ? "_" : " ")).c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
  }
};

void OpenMarkerBox() {
  g_marker_box = MarkerBox{true, NowMs(), {}, g_window && g_window->IsTextInputActive(), std::string::npos};
  // Like the console: while the box is open the game gets no input.
  cod3::ui::GameConsole::SetOpen(true);
  // Typed characters (OnKeyChar) only come while the window takes text
  // input (SDL); ImGui turns it on only for its own text boxes.
  if (g_window) g_window->SetTextInputActive(true);
  if (g_drawer && !g_marker_dialog) new MarkerDialog(g_drawer);
}

void CloseMarkerBox(bool save) {
  if (save) AddMarker(g_marker_box.text, g_marker_box.pressed_ms);
  g_marker_box.open = false;
  g_marker_box.text.clear();
  cod3::ui::GameConsole::SetOpen(false);
  if (g_window) g_window->SetTextInputActive(g_marker_box.text_input_before);
  if (g_marker_dialog) g_marker_dialog->Dismiss();
}

// Removes the last UTF-8 character.
void EraseLastCharacter(std::string& text) {
  while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
  if (!text.empty()) text.pop_back();
}

void AppendUtf8(std::string& text, uint32_t c) {
  if (c < 0x80) {
    text += char(c);
  } else if (c < 0x800) {
    text += char(0xC0 | (c >> 6));
    text += char(0x80 | (c & 0x3F));
  } else {
    text += char(0xE0 | (c >> 12));
    text += char(0x80 | ((c >> 6) & 0x3F));
    text += char(0x80 | (c & 0x3F));
  }
}

// The character a key press stands for, from the key alone: letters,
// digits and space, or 0. Typed characters (OnKeyChar) can lag behind or be
// missing (they only start once the window takes text input, and key
// presses queued before that have none), so the box takes these from the key
// press and lets the typed character, when it comes, replace it.
char KeyCharacter(const rex::ui::KeyEvent& e) {
  using rex::ui::VirtualKey;
  if (e.is_ctrl_pressed() || e.is_alt_pressed() || e.is_super_pressed()) return 0;
  const auto key = static_cast<uint32_t>(e.virtual_key());
  if (key >= uint32_t(VirtualKey::kA) && key <= uint32_t(VirtualKey::kZ))
    return char((e.is_shift_pressed() ? 'A' : 'a') + (key - uint32_t(VirtualKey::kA)));
  if (key >= uint32_t(VirtualKey::k0) && key <= uint32_t(VirtualKey::k9) && !e.is_shift_pressed())
    return char('0' + (key - uint32_t(VirtualKey::k0)));
  if (key >= uint32_t(VirtualKey::kNumpad0) && key <= uint32_t(VirtualKey::kNumpad0) + 9)
    return char('0' + (key - uint32_t(VirtualKey::kNumpad0)));
  if (key == uint32_t(VirtualKey::kSpace)) return ' ';
  return 0;
}

class MarkerKeyListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    using rex::ui::VirtualKey;
    const VirtualKey key = e.virtual_key();
    if (!g_marker_box.open) {
      if (key == VirtualKey::kF8) {
        if (!cod3::ui::GameConsole::IsOpen()) OpenMarkerBox();
        e.set_handled(true);
      }
      return;
    }
    // A typed character (OnKeyChar) belongs to the key press just before it.
    g_marker_box.key_char_at = std::string::npos;
    if (key == VirtualKey::kReturn) {
      CloseMarkerBox(true);
    } else if (key == VirtualKey::kEscape) {
      CloseMarkerBox(false);
    } else if (key == VirtualKey::kBack) {
      EraseLastCharacter(g_marker_box.text);
    } else if (const char c = KeyCharacter(e); c && g_marker_box.text.size() < kMaxMarkerBytes) {
      g_marker_box.key_char_at = g_marker_box.text.size();
      g_marker_box.text += c;
    }
    e.set_handled(true);  // nothing else sees a key while the box is open
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (g_marker_box.open) e.set_handled(true);
  }
  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (!g_marker_box.open) return;
    const uint32_t c = static_cast<uint32_t>(e.virtual_key());
    std::string& text = g_marker_box.text;
    if (c >= 0x20 && c != 0x7F && c < 0x10000) {
      // The typed character of the key press just taken: it replaces the
      // guess (another layout, Shift, Caps Lock).
      if (g_marker_box.key_char_at != std::string::npos && g_marker_box.key_char_at + 1 == text.size())
        text.pop_back();
      g_marker_box.key_char_at = std::string::npos;
      if (text.size() + 3 <= kMaxMarkerBytes) AppendUtf8(text, c);
    }
    e.set_handled(true);
  }
};
MarkerKeyListener g_marker_keys;

}  // namespace

// ---------------------------------------------------------------------------
// Public (game/decomp_trace.h)
// ---------------------------------------------------------------------------

void Start(const std::filesystem::path& user_data_dir) {
  if (g_writer_started.exchange(true)) return;
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char name[32], iso[32];
  std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S", &utc);
  std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &utc);
  {
    std::lock_guard lock(g_write_mutex);
    g_session_name = name;
    g_started_utc = iso;
    g_session_dir = user_data_dir / "decomp_trace" / g_session_name;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_session_dir, ec);
  REXLOG_INFO("cod3: decomp trace build: writing userdata/decomp_trace/{}/ every minute and at exit; F8 or "
              "trace_mark <words> adds a marker",
              g_session_name);
  std::thread(WriterLoop).detach();
}

void AttachMarkerKey(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  g_drawer = drawer;
  g_window = window;
  // First of all listeners, so F8 never reaches the game or a text box.
  if (window) window->AddInputListener(&g_marker_keys, 210);
}

void Flush() { WriteAll("at exit"); }

bool HandleConsoleCommand(const std::string& command, const std::vector<std::string>& args) {
  auto& console = cod3::ui::GameConsole::Get();
  if (command == "trace_mark") {
    std::string text;
    for (size_t i = 1; i < args.size(); ++i) text += (i > 1 ? " " : "") + args[i];
    if (text.empty()) {
      console.Print(cod3::ui::ConsoleChannel::kSystem,
                    "trace_mark <words>: notes what just happened for the decomp trace (F8 does the same)\n");
      return true;
    }
    AddMarker(text, NowMs());
    return true;
  }
  if (command == "trace_status") {
    size_t markers = 0;
    {
      std::lock_guard lock(g_markers_mutex);
      markers = g_markers.size();
    }
    console.Print(cod3::ui::ConsoleChannel::kSystem,
                  fmt::format("Decomp trace: {} functions seen, {} markers; files in userdata/decomp_trace/{}/ "
                              "(written every minute and at exit; trace_write writes now)\n",
                              g_slot_count.load(), markers, g_session_name));
    return true;
  }
  if (command == "trace_write") {
    WriteAll("trace_write");
    console.Print(cod3::ui::ConsoleChannel::kSystem,
                  fmt::format("Decomp trace written to userdata/decomp_trace/{}/\n", g_session_name));
    return true;
  }
  return false;
}

}  // namespace cod3::trace

COD3_TRACE_EXPORT const cod3::trace::Registry* cod3_decomp_trace_registry() { return &cod3::trace::TheRegistry(); }
