// cod3 - [acct2-main] 2026-11-15 texts set from the string tables follow a change of the text language (see
// text_rekey.h). Both programs: the hooks below are FEText's and FEMultiLineText's slots 32 and 33 in each, and
// FEMultiLineText's wrapped text from a key (the campaign's slot 90, cod3mp's slot 96);
// recomp/mp/config/campaign_map.tsv has their cod3mp addresses.

#include "game/text_rekey.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/text_language.h"

REXCVAR_DECLARE(bool, cod3_menu_trace);  // game/pc_menu.cpp: logs each text set again

namespace cod3::textkeys {
namespace {

using cod3::guest::Call;
using cod3::guest::ReadString;
using cod3::guest::ReadU32;
using cod3::guest::VirtualSlot;

// The set-text slots (byte offsets into the vtable), the same in both programs for FEText and FEMultiLineText.
constexpr uint32_t kSetTextHash = 128;    // slot 32 (this, hash)
constexpr uint32_t kSetTextKey = 132;     // slot 33 (this, key)
// FEMultiLineText's wrapped text from a key (this, key, width; f1 scale): the campaign's slot 90, cod3mp's slot 96
// (0x82501168: the same code, its slots after 90 are six further on, as slot 91 / 97 in campaign_map.tsv).
#if defined(COD3_MP)
constexpr uint32_t kSetWrappedKey = 384;  // [acct2-main] 2026-11-15 21:20
#else
constexpr uint32_t kSetWrappedKey = 360;
#endif
constexpr uint32_t kTextString = 28;      // FEText: String* of the words shown (chars at +12)
constexpr uint32_t kTextName = 100;       // FEText: String* of its name in the panel (chars at +12)

enum class How : uint8_t { kHash, kKey, kWrapped };

struct Keyed {
  How how = How::kKey;
  uint32_t hash = 0;     // kHash
  std::string key;       // kKey, kWrapped
  uint64_t width = 0;    // kWrapped: r5
  double scale = 0.0;    // kWrapped: f1
  uint32_t gen = 0;      // text::Generation() when it was set
  std::string shown;     // the words it showed right after
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Keyed> g_keyed;  // by text; g_mutex
std::atomic<int> g_logged{0};

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

std::string Words(uint8_t* base, uint32_t text) {
  const uint32_t s = ReadU32(base, text + kTextString);
  return Valid(s) ? ReadString(base, s + 12, 2048) : std::string();
}

std::string Name(uint8_t* base, uint32_t text) {
  const uint32_t s = ReadU32(base, text + kTextName);
  return Valid(s) ? ReadString(base, s + 12, 64) : std::string();
}

void Remember(uint8_t* base, uint32_t text, Keyed k) {
  k.shown = Words(base, text);
  std::lock_guard lock(g_mutex);
  if (g_keyed.size() > 65536) g_keyed.clear();  // never in practice: a text is forgotten when it is deleted
  g_keyed[text] = std::move(k);
}

}  // namespace

void Forget(uint32_t text) {
  std::lock_guard lock(g_mutex);
  g_keyed.erase(text);
}

void Refresh(PPCContext& ctx, uint8_t* base, uint32_t text) {
  if (!Valid(text)) return;
  const uint32_t gen = cod3::text::Generation();
  Keyed k;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_keyed.find(text);
    if (it == g_keyed.end() || it->second.gen == gen) return;
    if (it->second.shown != Words(base, text)) {  // its words were set some other way since: not the key's now
      g_keyed.erase(it);
      return;
    }
    it->second.gen = gen;  // once per change, whatever the call below does
    k = it->second;
  }
  const uint32_t slot = k.how == How::kHash ? kSetTextHash : k.how == How::kKey ? kSetTextKey : kSetWrappedKey;
  const uint32_t fn = VirtualSlot(base, text, slot);
  if (fn < 0x82000000u || fn >= 0x83000000u) return;  // not code: leave it
  {
    Call c(ctx, base);
    if (k.how == How::kHash) {
      c.Invoke(fn, {text, k.hash});
    } else {
      const uint32_t s = c.PushString(k.key);
      if (k.how == How::kKey)
        c.Invoke(fn, {text, s});
      else
        c.Invoke(fn, {text, s, k.width}, {k.scale});
    }
  }
  if (REXCVAR_GET(cod3_menu_trace) && g_logged.fetch_add(1, std::memory_order_relaxed) < 400) {
    const std::string chosen = cod3::text::Chosen();
    REXLOG_INFO("cod3: text set again in {}: {} {} \"{}\" -> \"{}\"", chosen.empty() ? "the game's own" : chosen,
                Name(base, text), k.how == How::kHash ? fmt::format("hash {:08X}", k.hash) : k.key,
                k.shown.substr(0, 80), Words(base, text).substr(0, 80));
  }
}

KeepKey::KeepKey(uint8_t* base, uint32_t text) : base_(base), text_(text) {
  if (!Valid(text)) return;
  std::lock_guard lock(g_mutex);
  auto it = g_keyed.find(text);
  keyed_ = it != g_keyed.end() && it->second.shown == Words(base, text);
}

KeepKey::~KeepKey() {
  if (!keyed_) return;
  const std::string now = Words(base_, text_);
  std::lock_guard lock(g_mutex);
  if (auto it = g_keyed.find(text_); it != g_keyed.end()) it->second.shown = now;
}

}  // namespace cod3::textkeys

namespace cod3::textkeys {
namespace {
// For the hooks below: `how` 0 a hash, 1 a key, 2 a wrapped key; `gen` the generation before the game's own call
// (a change on another thread meanwhile, from F2 or the console, leaves the text out of date: its next draw sets
// it again).
void HookedSet(uint8_t* base, uint32_t text, int how, uint32_t key_or_hash, uint64_t width, double scale,
               uint32_t gen) {
  if (!Valid(text)) return;
  Keyed k;
  k.how = How(how);
  k.gen = gen;
  if (k.how == How::kHash) {
    k.hash = key_or_hash;
  } else {
    if (key_or_hash < 0x10000) return;  // no key: the game left the text as it was
    k.key = ReadString(base, key_or_hash, 256);
    k.width = width;
    k.scale = scale;
  }
  Remember(base, text, std::move(k));
}
}  // namespace
}  // namespace cod3::textkeys

// The hooks are outside the namespace (REX_HOOK_RAW defines the generated function's name).
// FEText's and FEMultiLineText's set from a hash (slot 32: this, hash) and from a key (slot 33: this, key).
REX_EXTERN(__imp__FEText__vfunc_32);
REX_HOOK_RAW(FEText__vfunc_32) {
  const uint32_t text = ctx.r3.u32, arg = ctx.r4.u32;
  const uint32_t gen = cod3::text::Generation();
  __imp__FEText__vfunc_32(ctx, base);
  cod3::textkeys::HookedSet(base, text, 0, arg, 0, 0.0, gen);
}

REX_EXTERN(__imp__FEText__vfunc_33);
REX_HOOK_RAW(FEText__vfunc_33) {
  const uint32_t text = ctx.r3.u32, arg = ctx.r4.u32;
  const uint32_t gen = cod3::text::Generation();
  __imp__FEText__vfunc_33(ctx, base);
  cod3::textkeys::HookedSet(base, text, 1, arg, 0, 0.0, gen);
}

REX_EXTERN(__imp__FEMultiLineText__vfunc_32);
REX_HOOK_RAW(FEMultiLineText__vfunc_32) {
  const uint32_t text = ctx.r3.u32, arg = ctx.r4.u32;
  const uint32_t gen = cod3::text::Generation();
  __imp__FEMultiLineText__vfunc_32(ctx, base);
  cod3::textkeys::HookedSet(base, text, 0, arg, 0, 0.0, gen);
}

REX_EXTERN(__imp__FEMultiLineText__vfunc_33);
REX_HOOK_RAW(FEMultiLineText__vfunc_33) {
  const uint32_t text = ctx.r3.u32, arg = ctx.r4.u32;
  const uint32_t gen = cod3::text::Generation();
  __imp__FEMultiLineText__vfunc_33(ctx, base);
  cod3::textkeys::HookedSet(base, text, 1, arg, 0, 0.0, gen);
}

// FEMultiLineText's wrapped text from a key (this, key, width; f1 scale): the campaign's slot 90 (0x824DD928);
// in cod3mp this name is its slot 96 (0x82501168, campaign_map.tsv), as the message boxes' words.
REX_EXTERN(__imp__FEMultiLineText__vfunc_90);
REX_HOOK_RAW(FEMultiLineText__vfunc_90) {
  const uint32_t text = ctx.r3.u32, key = ctx.r4.u32;
  const uint64_t width = ctx.r5.u64;
  const double scale = ctx.f1.f64;
  const uint32_t gen = cod3::text::Generation();
  __imp__FEMultiLineText__vfunc_90(ctx, base);
  cod3::textkeys::HookedSet(base, text, 2, key, width, scale, gen);
}
