// cod3 - typed calls into the game's own functions (see game_api.h).

#include "game/game_api.h"

#include "game/addresses.h"
#include "game/guest.h"

namespace cod3::game {

namespace {

namespace a = cod3::addr;
using guest::Call;
using guest::ReadF32;
using guest::ReadS32;
using guest::ReadString;
using guest::ReadU32;
using guest::VirtualSlot;

// A guest heap or global object pointer, not 0 and not a small number.
bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

}  // namespace

// ---------------------------------------------------------------------------
// Dvars
// ---------------------------------------------------------------------------
uint32_t CvarFind(PPCContext& ctx, uint8_t* base, std::string_view name) {
  Call call(ctx, base);
  const uint32_t s = call.PushString(name);
  return static_cast<uint32_t>(call.Invoke(a::kCvarFindVar, {s}));
}

uint32_t CvarFindGuest(PPCContext& ctx, uint8_t* base, uint32_t guest_name) {
  if (!guest_name) return 0;
  Call call(ctx, base);
  return static_cast<uint32_t>(call.Invoke(a::kCvarFindVar, {guest_name}));
}

uint32_t CvarGet(PPCContext& ctx, uint8_t* base, std::string_view name, std::string_view value,
                 uint32_t flags) {
  Call call(ctx, base);
  const uint32_t n = call.PushString(name);
  const uint32_t v = call.PushString(value);
  return static_cast<uint32_t>(call.Invoke(a::kCvarGet, {n, v, flags}));
}

void CvarSet2(PPCContext& ctx, uint8_t* base, std::string_view name, std::string_view value,
              bool force) {
  Call call(ctx, base);
  const uint32_t n = call.PushString(name);
  const uint32_t v = call.PushString(value);
  call.Invoke(a::kCvarSet2, {n, v, force ? 1u : 0u});
}

std::string CvarName(uint8_t* base, uint32_t cvar) {
  return cvar ? ReadString(base, ReadU32(base, cvar + a::cvar::kName), 128) : std::string();
}

std::string CvarString(uint8_t* base, uint32_t cvar, size_t max_len) {
  return cvar ? ReadString(base, ReadU32(base, cvar + a::cvar::kString), max_len) : std::string();
}

std::string CvarResetString(uint8_t* base, uint32_t cvar, size_t max_len) {
  return cvar ? ReadString(base, ReadU32(base, cvar + a::cvar::kResetString), max_len) : std::string();
}

float CvarValue(uint8_t* base, uint32_t cvar) { return cvar ? ReadF32(base, cvar + a::cvar::kValue) : 0.0f; }

int32_t CvarInteger(uint8_t* base, uint32_t cvar) { return cvar ? ReadS32(base, cvar + a::cvar::kInteger) : 0; }

uint32_t CvarFlags(uint8_t* base, uint32_t cvar) { return cvar ? ReadU32(base, cvar + a::cvar::kFlags) : 0; }

// ---------------------------------------------------------------------------
// Console text, printing, localized text
// ---------------------------------------------------------------------------
void CbufAddText(PPCContext& ctx, uint8_t* base, std::string_view text) {
  Call call(ctx, base);
  const uint32_t s = call.PushString(text);
  call.Invoke(a::kCbufAddText, {s});
}

void ComPrintf(PPCContext& ctx, uint8_t* base, std::string_view text) {
  Call call(ctx, base);
  const uint32_t fmt = call.PushString("%s");
  const uint32_t s = call.PushString(text);
  call.Invoke(a::kComPrintf, {fmt, s});
}

std::string Localize(PPCContext& ctx, uint8_t* base, std::string_view key, size_t max_len) {
  const uint32_t tables = ReadU32(base, a::prompts::kStringTables);
  if (!Valid(tables)) return {};
  Call call(ctx, base);
  const uint32_t k = call.PushString(key);
  const uint32_t text = static_cast<uint32_t>(call.Invoke(a::prompts::kLocalize, {tables, k}));
  if (!text) return {};
  std::string out = ReadString(base, text, max_len);
  return out == key ? std::string() : out;  // the key itself: not in the tables
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------
uint32_t PanelFindElement(PPCContext& ctx, uint8_t* base, uint32_t panel, std::string_view name) {
  if (!Valid(panel)) return 0;
  Call call(ctx, base);
  const uint32_t s = call.PushString(name);
  return static_cast<uint32_t>(call.Invoke(a::fe::kPanelFindElement, {panel, s}));
}

uint32_t PanelFindQuad(PPCContext& ctx, uint8_t* base, uint32_t panel, std::string_view name) {
  if (!Valid(panel)) return 0;
  Call call(ctx, base);
  const uint32_t s = call.PushString(name);
  const uint32_t quad = static_cast<uint32_t>(call.Invoke(a::fe::kPanelFindQuad, {panel, s}));
  if (!Valid(quad)) return 0;
  const uint32_t str = ReadU32(base, quad + a::fe::kQuadName);
  if (!str || ReadString(base, str + a::pc_menu::kStringChars, 64) != name) return 0;
  return quad;
}

uint32_t OperatorNew(PPCContext& ctx, uint8_t* base, uint32_t size) {
  Call call(ctx, base);
  return static_cast<uint32_t>(call.Invoke(a::fe::kOperatorNew, {size}));
}

void OperatorDelete(PPCContext& ctx, uint8_t* base, uint32_t memory) {
  if (!memory) return;
  Call call(ctx, base);
  call.Invoke(a::fe::kOperatorDelete, {memory});
}

uint32_t CopyElement(PPCContext& ctx, uint8_t* base, uint32_t source, bool quad) {
  if (!Valid(source)) return 0;
  Call call(ctx, base);
  const uint32_t mem =
      static_cast<uint32_t>(call.Invoke(a::fe::kOperatorNew, {quad ? a::fe::kQuadSize : a::fe::kTextSize}));
  if (!Valid(mem)) return 0;
  call.Invoke(quad ? a::fe::kQuadCtor : a::fe::kTextCtor, {mem});
  call.Invoke(VirtualSlot(base, mem, a::fe::kElemAssign), {mem, source});
  return mem;
}

void ElementSetVisible(PPCContext& ctx, uint8_t* base, uint32_t element, bool visible) {
  if (!Valid(element)) return;
  Call call(ctx, base);
  call.Invoke(VirtualSlot(base, element, a::fe::kElemSetVisible), {element, visible ? 1u : 0u});
}

void ElementSetAlpha(PPCContext& ctx, uint8_t* base, uint32_t element, float alpha) {
  if (!Valid(element)) return;
  Call call(ctx, base);
  call.Invoke(VirtualSlot(base, element, a::fe::kElemSetAlpha), {element}, {alpha});
}

void ElementSetText(PPCContext& ctx, uint8_t* base, uint32_t element, std::string_view text) {
  if (!Valid(element)) return;
  Call call(ctx, base);
  const uint32_t s = call.PushString(text);
  call.Invoke(VirtualSlot(base, element, a::fe::kElemSetTextRaw), {element, s});
}

void ElementSetTextKey(PPCContext& ctx, uint8_t* base, uint32_t element, std::string_view key) {
  if (!Valid(element)) return;
  Call call(ctx, base);
  const uint32_t s = call.PushString(key);
  call.Invoke(VirtualSlot(base, element, a::fe::kElemSetTextKey), {element, s});
}

void ItemSetVisible(PPCContext& ctx, uint8_t* base, uint32_t item, bool visible) {
  if (!Valid(item)) return;
  Call call(ctx, base);
  call.Invoke(VirtualSlot(base, item, a::fe::kItemSetVisible), {item, visible ? 1u : 0u});
}

void ItemSetDisabled(PPCContext& ctx, uint8_t* base, uint32_t item, bool disabled) {
  if (!Valid(item)) return;
  Call call(ctx, base);
  call.Invoke(VirtualSlot(base, item, a::fe::kItemSetDisabled), {item, disabled ? 1u : 0u});
}

void ItemSetLabelKey(PPCContext& ctx, uint8_t* base, uint32_t item, std::string_view key) {
  if (!Valid(item)) return;
  Call call(ctx, base);
  const uint32_t s = call.PushString(key);
  call.Invoke(VirtualSlot(base, item, a::fe::kItemSetLabelKey), {item, s});
}

void MessageBoxClose(PPCContext& ctx, uint8_t* base, uint32_t system) {
  if (!Valid(system)) return;
  Call call(ctx, base);
  call.Invoke(a::msgbox::kClose, {system});
}

void MessageBoxSetMessage(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t text) {
  if (!Valid(system) || !text) return;
  Call call(ctx, base);
  call.Invoke(a::msgbox::kSetMessage, {system, text});
}

void MessageBoxAddButton(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t key, uint32_t callback) {
  if (!Valid(page) || !key) return;
  Call call(ctx, base);
  call.Invoke(a::msgbox::kAddButton, {page, key, callback});
}

void MessageBoxUpdateSelection(PPCContext& ctx, uint8_t* base, uint32_t buttons, uint32_t selected) {
  if (!Valid(buttons)) return;
  Call call(ctx, base);
  call.Invoke(a::msgbox::kUpdateSelection, {buttons, selected});
}

void MessageBoxOpen(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!Valid(page)) return;
  Call call(ctx, base);
  call.Invoke(a::msgbox::kOpen, {page, 1, 0});
}

// ---------------------------------------------------------------------------
// Movies
// ---------------------------------------------------------------------------
void MovieStop(PPCContext& ctx, uint8_t* base) {
  Call call(ctx, base);
  call.Invoke(a::movie::kStop);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void SetMultisample(PPCContext& ctx, uint8_t* base, uint32_t type) {
  Call call(ctx, base);
  call.Invoke(a::video::kSetMultisample, {type});
}

void WaitJobGroup(PPCContext& ctx, uint8_t* base, uint32_t group) {
  Call call(ctx, base);
  call.Invoke(a::live_video::kWaitJobGroup, {group});
}

void UnbindAndIdle(PPCContext& ctx, uint8_t* base) {
  Call call(ctx, base);
  call.Invoke(a::live_video::kUnbindAndIdle);
}

void ResourceRelease(PPCContext& ctx, uint8_t* base, uint32_t resource) {
  if (!resource) return;
  Call call(ctx, base);
  call.Invoke(a::live_video::kResourceRelease, {resource});
}

uint32_t CreateTexture(PPCContext& ctx, uint8_t* base, uint32_t flags, uint32_t format, uint32_t width,
                       uint32_t height, uint32_t r7) {
  Call call(ctx, base);
  return static_cast<uint32_t>(
      call.Invoke(a::live_video::kCreateTexture, {flags, format, width, height, r7, 1}));
}

void ReleaseTexture(PPCContext& ctx, uint8_t* base, uint32_t record) {
  if (!record) return;
  Call call(ctx, base);
  call.Invoke(a::live_video::kReleaseTexture, {record});
}

void MakeFullDepth(PPCContext& ctx, uint8_t* base) {
  Call call(ctx, base);
  call.Invoke(a::live_video::kMakeFullDepth);
}

}  // namespace cod3::game
