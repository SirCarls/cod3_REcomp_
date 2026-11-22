// cod3 - [acct2-main] 2026-11-21 The game's multi-line text without its fixed-size buffers (see text_buffers.h).

#include "game/text_buffers.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_text_whole, true, "cod3/Game",
                    "Texts longer than the game's own text buffers (menus, hints, message boxes, prompts) are shown "
                    "whole; off: the game's own code, which cuts them at 509 characters or runs a line of 256 bytes or "
                    "more past its buffer")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-main] 2026-11-21 (owner 13:20: "test for extreme edge cases with any of our changes"): a dev test of the fixes
// with the game's own code, run once at the next multi-line text the game sets after it goes on.
REXCVAR_DEFINE_BOOL(cod3_text_buffers_selftest, false, "cod3/Debug",
                    "Dev test: at the next multi-line text the game sets, put extreme texts (64 KB, Russian, Japanese "
                    "and Chinese with no spaces, 300 lines, a 600-byte line, action names back to back) through the "
                    "game's \"[ACTION]\" pass and its line layout on that element, log what came out, then set the "
                    "game's own text again")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::textbuf {

bool On() { return REXCVAR_GET(cod3_text_whole); }

namespace {

using cod3::guest::ReadU32;

// What the game's functions use (each program its own addresses; the layout of FEMultiLineText is the same).
#if defined(COD3_MP)
constexpr uint32_t kSetLine = 0x824EC790;     // (line*, const char*, font index; f1 scale, f2 button scale)
constexpr uint32_t kAssign = 0x821DF378;      // String::operator= (String**, const char*)
constexpr uint32_t kFinish = 0x824EB388;      // (this): after the lines are set
constexpr uint32_t kEmpty = 0x820646D0;       // ""
constexpr uint32_t kActionWordSlot = 376;     // slot 94 (this, word) -> its sentence, or 0
#else
constexpr uint32_t kExpandActions = 0x824DE290;  // (text) -> the text after its "[ACTION]" pass (hooked: ExpandWhole)
constexpr uint32_t kSetLine = 0x824D8108;
constexpr uint32_t kAssign = 0x824A8E00;
constexpr uint32_t kFinish = 0x824CF190;
constexpr uint32_t kEmpty = 0x82065B28;
#endif
constexpr uint32_t kShown = 28;        // String*: the text shown
constexpr uint32_t kFont = 24;         // s32 font index
constexpr uint32_t kScale = 56;        // float
constexpr uint32_t kButtonScale = 116; // float
constexpr uint32_t kLineCount = 136;   // s32: lines in use
constexpr uint32_t kLineMax = 140;     // s32: lines it has room for
constexpr uint32_t kLines = 144;       // line*: 32 bytes each
constexpr uint32_t kLineSize = 32;
constexpr uint32_t kLineString = 20;   // String*: chars at +12
constexpr uint32_t kStringChars = 12;

// The game's own limits, which this file is here to lift (campaign 0x824DE290: strncpy(copy, text, 512); the result
// at most 509 characters, its length a halfword at +510; slot 36: a 256-byte line buffer).
constexpr size_t kPassIn = 511;
constexpr size_t kPassOut = 509;
constexpr size_t kGameLine = 255;
// A piece of a long text given to the campaign's pass at once: small enough that its result fits the pass's buffer
// in practice (each "[ACTION]" word grows to a sentence of up to 63 bytes); a piece whose result still fills it is
// halved and done again.
constexpr size_t kPieceIn = 240;

float F32(uint8_t* base, uint32_t addr) {
  const uint32_t v = ReadU32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// As button_prompts.cpp's: guest memory the game's own pointers use (not the GPU register window).
bool Valid(uint32_t addr) {
  return addr >= 0x10000u && addr < 0xC0000000u && !(addr >= 0x7F000000u && addr < 0x80000000u);
}

std::string ReadText(uint8_t* base, uint32_t addr) {
  if (!Valid(addr)) return {};
  return cod3::guest::ReadString(base, addr, kMaxText);
}

// Once per distinct text: what the game's own code would have done with it.
void ReportOnce(const char* what, const std::string& text) {
  static std::mutex m;
  static std::unordered_set<std::string> seen;
  {
    std::lock_guard lock(m);
    if (seen.size() > 4096 || !seen.insert(text).second) return;
  }
  REXLOG_ERROR("cod3: text buffers: {} ({} bytes), shown whole: \"{}\"", what, text.size(),
               text.substr(0, 120));
}

// [addr, addr + n) is committed, readable guest memory (asked of the guest heaps, as heap_check.cpp does).
bool Mapped(uint32_t addr, uint32_t n) {
  if (n == 0 || !Valid(addr) || !Valid(addr + n - 1)) return false;
  for (uint64_t page = addr & ~0xFFFull; page <= uint64_t(addr) + n - 1; page += 0x1000) {
    auto* mem = rex::system::kernel_state()->memory();
    auto* heap = mem ? mem->LookupHeap(uint32_t(page)) : nullptr;
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(uint32_t(page), &protect) || !(protect & rex::memory::kMemoryProtectWrite))
      return false;
  }
  return true;
}

struct ScratchSlot {
  uint32_t addr = 0, size = 0;
};
thread_local std::array<ScratchSlot, kSlotCount> t_scratch;

// The words of a text as the game's pass splits it (strtok on ' '): each word's start and length; the spaces between
// two words are what lies between them. Spaces before the first word and after the last are dropped, as the pass does.
struct Word {
  size_t at, len;
};
std::vector<Word> Words(const std::string& s) {
  std::vector<Word> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) break;
    const size_t start = i;
    while (i < s.size() && s[i] != ' ') ++i;
    out.push_back({start, i - start});
  }
  return out;
}

// [acct2-main] 2026-11-21 (owner 13:20: "test for extreme edge cases"): a word longer than a piece. Chinese and
// Japanese text has no spaces, so a long row of theirs is one "word" to the pass, and its strncpy(512) would cut it.
// Such a word is cut into parts of at most kPieceIn bytes, each its own word with nothing between them, where the
// pass can't tell the difference: never inside a "[...]" (an action name stays whole), never just before a '[' or
// just after a ']' (whatever the pass takes as a name's start or end is where it was), never inside a UTF-8
// character; after a '\n' when there is one. A word with no such place stays whole (and is reported cut).
std::vector<Word> SplitLong(const std::string& s, const std::vector<Word>& words, size_t most) {
  std::vector<Word> out;
  for (const Word& w : words) {
    size_t at = w.at, left = w.len;
    while (left > most) {
      // Where a part may end: positions (from `at`) 1..most, each the start of the next part.
      size_t best = 0, best_newline = 0;
      int depth = 0;
      for (size_t k = 0; k < left && k <= most; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[at + k]);
        if (k > 0 && depth == 0 && c != '[' && (c & 0xC0) != 0x80 && s[at + k - 1] != ']') {
          best = k;
          if (s[at + k - 1] == '\n') best_newline = k;
        }
        if (c == '[') ++depth;
        if (c == ']' && depth > 0) --depth;
      }
      size_t cut = best_newline ? best_newline : best;
      // None: between two names ("][", names back to back), else, past an unclosed '[', at a character's start.
      for (size_t k = most; !cut && k > 0; --k)
        if (k < left && s[at + k - 1] == ']' && s[at + k] == '[') cut = k;
      if (!cut && s.find(']', at) >= at + left)
        for (size_t k = most; !cut && k > 0; --k)
          if (k < left && (static_cast<unsigned char>(s[at + k]) & 0xC0) != 0x80) cut = k;
      if (!cut) break;  // no place to cut (one name longer than a piece): the rest stays one word
      out.push_back({at, cut});
      at += cut;
      left -= cut;
    }
    out.push_back({at, left});
  }
  return out;
}

// [acct2-main] 2026-11-21 1b. Names joined to other text (text_buffers.h). The "[NAME]"s in one space-free word, as
// [at, len): '[', then letters, digits, '_' or '+' (the packs' names), then ']'.
std::vector<Word> NamesIn(std::string_view w) {
  std::vector<Word> out;
  size_t i = 0;
  while ((i = w.find('[', i)) != std::string_view::npos) {
    size_t j = i + 1;
    auto name_char = [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+';
    };
    while (j < w.size() && j - i <= 61 && name_char(w[j])) ++j;
    if (j < w.size() && w[j] == ']' && j > i + 1) {
      out.push_back({i, j - i + 1});
      i = j + 1;
    } else {
      ++i;
    }
  }
  return out;
}

// A word the game's pass would drop part of: it has a name and something besides that one name.
bool Glued(std::string_view w) {
  const std::vector<Word> names = NamesIn(w);
  return !names.empty() && !(names.size() == 1 && names[0].at == 0 && names[0].len == w.size());
}

// A word with its names through `name(text) -> result` and everything else as it is.
std::string ExpandGluedWord(std::string_view w, const std::function<std::string(const std::string&)>& name) {
  std::string out;
  size_t i = 0;
  for (const Word& n : NamesIn(w)) {
    out.append(w.substr(i, n.at - i));
    out += name(std::string(w.substr(n.at, n.len)));
    i = n.at + n.len;
  }
  out.append(w.substr(i));
  return out;
}

// A glued word's place while the rest of the text goes through the pass: MARK<index>MARK, MARK a control byte from
// 1 to 7 the text doesn't have.
char MarkFor(const std::string& s) {
  for (char c = 1; c < 8; ++c)
    if (s.find(c) == std::string::npos) return c;
  return 0;
}

}  // namespace

uint32_t Scratch(uint8_t* base, Slot slot, size_t size) {
  (void)base;
  ScratchSlot& s = t_scratch[size_t(slot)];
  if (size > kMaxText + 16) return 0;
  if (s.addr && s.size >= size) return s.addr;
  // Grows by doubling; the smaller block is kept (a pointer the game took from it may still be read this frame).
  uint32_t want = std::max<uint32_t>(1024, s.size * 2);
  while (want < size) want *= 2;
  const uint32_t at = rex::system::kernel_state()->memory()->SystemHeapAlloc(want);
  if (!at) return 0;
  s.addr = at, s.size = want;
  return at;
}

uint32_t PutText(uint8_t* base, Slot slot, const std::string& s) {
  const uint32_t at = Scratch(base, slot, s.size() + 1);
  if (!at) return 0;
  std::memcpy(base + at, s.c_str(), s.size() + 1);
  return at;
}

uint32_t ExpandWhole(uint8_t* base, uint32_t text, const std::function<uint32_t(uint32_t)>& pass) {
  if (!On() || !Valid(text)) return pass(text);
  const std::string s = ReadText(base, text);
  // [acct2-main] 2026-11-21 1b. Words with a name joined to other text: each such word takes a mark's place while the
  // rest goes through the game's pass (as below, so spaces, ends and lengths are the game's), then comes back with
  // its names through the pass one by one and its other text as it was.
  const char mark = s.find('[') != std::string::npos ? MarkFor(s) : 0;
  if (mark) {
    std::vector<std::string> glued;
    std::string marked;
    size_t at = 0;
    for (const Word& w : Words(s)) {
      marked.append(s, at, w.at - at);
      const std::string_view word(s.data() + w.at, w.len);
      if (Glued(word)) {
        marked += mark + std::to_string(glued.size()) + mark;
        glued.emplace_back(word);
      } else {
        marked.append(word);
      }
      at = w.at + w.len;
    }
    marked.append(s, at, std::string::npos);
    if (!glued.empty()) {
      ReportOnce("a name joined to other text in one word (the game's pass keeps only the name)", s);
      const uint32_t in = PutText(base, kGlued, marked);
      if (in) {
        const uint32_t out = ExpandWhole(base, in, pass);  // no glued word is left in it
        const std::string r = Valid(out) ? ReadText(base, out) : std::string();
        auto name = [&](const std::string& n) {
          const uint32_t nin = PutText(base, kExpandIn, n);
          const uint32_t nout = nin ? pass(nin) : 0;
          const std::string got = Valid(nout) ? cod3::guest::ReadString(base, nout, kPassOut + 2) : std::string();
          return got.empty() ? n : got;
        };
        std::string done;
        for (size_t i = 0; i < r.size();) {
          const size_t end = r[i] == mark ? r.find(mark, i + 1) : std::string::npos;
          if (end != std::string::npos && end > i + 1) {
            const size_t k = size_t(std::strtoul(r.substr(i + 1, end - i - 1).c_str(), nullptr, 10));
            if (k < glued.size()) {
              done += ExpandGluedWord(glued[k], name);
              i = end + 1;
              continue;
            }
          }
          done += r[i++];
        }
        const uint32_t whole = PutText(base, kGlued, done);
        if (whole) return whole;
      }
    }
  }
  if (s.size() >= kMaxText) ReportOnce("a text longer than the 64 KB followed (the rest is not shown)", s);
  if (s.size() < kPassIn) {
    const uint32_t out = pass(text);
    if (!Valid(out) || strnlen(reinterpret_cast<const char*>(base + out), kPassOut + 1) < kPassOut) return out;
  }
  // The game's pass would cut this text: run it on pieces and join them.
  const std::vector<Word> words = SplitLong(s, Words(s), kPieceIn);
  std::string result;
  bool cut = false;
  // Pass on words [first, last): their text with the spaces between them, as one piece.
  std::function<void(size_t, size_t)> piece = [&](size_t first, size_t last) {
    const size_t from = words[first].at, to = words[last - 1].at + words[last - 1].len;
    const uint32_t in = PutText(base, kExpandIn, s.substr(from, to - from));
    if (!in) {
      cut = true;
      return;
    }
    const uint32_t out = pass(in);
    const std::string got = Valid(out) ? cod3::guest::ReadString(base, out, kPassOut + 2) : std::string();
    if (got.size() >= kPassOut && last - first > 1) {
      const size_t mid = first + (last - first) / 2;
      piece(first, mid);
      result.append(s, words[mid - 1].at + words[mid - 1].len, words[mid].at - (words[mid - 1].at + words[mid - 1].len));
      piece(mid, last);
      return;
    }
    if (got.size() >= kPassOut) cut = true;  // one word the pass can't hold (never in the packs: words are short)
    result += got;
  };
  size_t first = 0;
  while (first < words.size()) {
    size_t last = first + 1;
    while (last < words.size() && words[last].at + words[last].len - words[first].at <= kPieceIn) ++last;
    piece(first, last);
    if (last < words.size()) {
      const size_t gap_from = words[last - 1].at + words[last - 1].len;
      result.append(s, gap_from, words[last].at - gap_from);
    }
    first = last;
  }
  ReportOnce(cut ? "the \"[ACTION]\" pass could not hold one of its words (cut)"
                 : "a text longer than the \"[ACTION]\" pass's 509 characters",
             s);
  const uint32_t at = PutText(base, kExpandOut, result);
  return at ? at : pass(text);
}

namespace {
void SetLinesOf(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t in);

bool Utf8Whole(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

// Every bit of `in` that isn't a name (cut at spaces) is in `out`, in the same order: nothing of the text was dropped.
bool KeptInOrder(const std::string& in, const std::string& out) {
  size_t from = 0, i = 0;
  std::string rest;
  for (const Word& n : NamesIn(in)) {
    rest.append(in, i, n.at - i);
    rest += ' ';
    i = n.at + n.len;
  }
  rest.append(in, i, std::string::npos);
  for (const Word& w : Words(rest)) {
    const size_t at = out.find(rest.substr(w.at, w.len), from);
    if (at == std::string::npos) return false;
    from = at + w.len;
  }
  return true;
}

std::string Repeat(const std::string& unit, size_t bytes) {
  std::string r;
  while (r.size() + unit.size() <= bytes) r += unit;
  return r;
}

void SelfTest(PPCContext& ctx, uint8_t* base, uint32_t self) {
  struct Case {
    const char* name;
    std::string text;
  };
  const std::string ru = "\xD0\x9D\xD0\xB0\xD1\x86\xD0\xB8\xD1\x81\xD1\x82\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB5 ";
  const std::string ja = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE6\x96\x87\xE7\xAB\xA0\xE3\x80\x82";
  const std::string zh = "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE6\x9C\xAC";
  std::string lines300;
  for (int i = 0; i < 300; ++i) lines300 += "line " + std::to_string(i) + "\n";
  const Case cases[] = {
      {"empty", ""},
      {"spaces only", std::string(600, ' ')},
      {"ascii 509", Repeat("abcd ", 509)},
      {"ascii 511", Repeat("abcd ", 511) + "x"},
      {"ascii 4000", Repeat("word ", 4000)},
      {"ascii 64 KB", Repeat("word ", kMaxText - 8)},
      {"russian 4000", Repeat(ru, 4000)},
      {"japanese 4000 no spaces", Repeat(ja, 4000)},
      {"chinese 20000 no spaces", Repeat(zh, 20000)},
      {"one word 600", std::string(600, 'x')},
      {"300 lines", lines300},
      {"a 600-byte line", std::string("first\n") + Repeat("long ", 600) + "\nlast"},
      {"newlines only", std::string(100, '\n')},
      {"crlf", Repeat("line one\r\n", 2000)},
      {"names back to back", Repeat("[USE]", 1500)},
      {"names spaced", Repeat("[USE] ", 3000)},
      {"japanese with names", Repeat(ja + "[USE]", 3000)},
      // [acct2-main] 2026-11-21 1b: names joined to other text in short texts (the game's own pass dropped the rest).
      {"short, a name in quotes", "\xC2\xAB[USE]\xC2\xBB, " + ru + "[USE]."},
      {"short japanese with a name", ja + "[USE]" + ja},
      {"short names back to back", "[USE][USE] [USE]"},
  };
  const int32_t room = int32_t(ReadU32(base, self + kLineMax));
  for (const Case& c : cases) {
    const uint32_t at = PutText(base, kTest, c.text);
    if (!at) {
      REXLOG_ERROR("cod3: text buffers selftest: {}: no guest memory", c.name);
      continue;
    }
    uint32_t out = 0;
    {
      cod3::guest::Call call(ctx, base);
#if defined(COD3_MP)
      out = uint32_t(call.Invoke(cod3::guest::VirtualSlot(base, self, 372), {self, at}));
#else
      out = uint32_t(call.Invoke(kExpandActions, {at}));
#endif
    }
    const std::string got = ReadText(base, out);
    // Plain text (no "[") comes back as it went in, spaces at its ends aside.
    std::string plain = c.text;
    const size_t a = plain.find_first_not_of(' ');
    plain = a == std::string::npos ? std::string() : plain.substr(a, plain.find_last_not_of(' ') - a + 1);
    const bool has_names = c.text.find('[') != std::string::npos;
    const bool kept = KeptInOrder(c.text, got);
    SetLinesOf(ctx, base, self, at);
    const int32_t count = int32_t(ReadU32(base, self + kLineCount));
    const int32_t want_lines = 1 + int32_t(std::count(got.begin(), got.end(), '\n'));
    // Each line laid out holds its own text (the first 48 bytes of each compared).
    int32_t first_bad = -1;
    size_t pos = 0;
    for (int32_t k = 0; k < count && first_bad < 0 && !has_names; ++k) {  // names: the layout draws them its own way
      const size_t end = std::min(got.find('\n', pos), got.size());
      const std::string line = pos <= got.size() ? got.substr(pos, end - pos) : std::string();
      const uint32_t l = ReadU32(base, self + kLines) + uint32_t(k) * kLineSize;
      const uint32_t str = Valid(l) ? ReadU32(base, l + kLineString) : 0;
      const std::string laid = str ? cod3::guest::ReadString(base, str + kStringChars, 48) : std::string();
      if (laid.compare(0, laid.size(), line, 0, std::min(line.size(), laid.size())) != 0) first_bad = k;
      pos = end + 1;
    }
    REXLOG_INFO("cod3: text buffers selftest: {}: {} bytes in, {} out ({}), UTF-8 {}, lines {} of {} (room {}){}",
                c.name, c.text.size(), got.size(),
                has_names ? (kept ? "names expanded, text kept" : "names expanded, TEXT LOST")
                : got == plain ? "same as in" : "DIFFERENT from in",
                Utf8Whole(got) ? "whole" : "BROKEN", count, want_lines, room,
                first_bad >= 0 ? " LINE " + std::to_string(first_bad) + " NOT ITS TEXT" : std::string());
  }
}
}  // namespace

void SetLines(PPCContext& ctx, uint8_t* base) {
  static std::atomic<bool> tested{false};
  static std::atomic<int> calls{0};
  // On an element with room for several lines (a message box, a page of help), so the lines are laid out too; else
  // on the 300th text set.
  const bool roomy = Valid(ctx.r3.u32) && int32_t(ReadU32(base, ctx.r3.u32 + kLineMax)) >= 4;
  if (REXCVAR_GET(cod3_text_buffers_selftest) && !tested.load() && (roomy || ++calls >= 300) && !tested.exchange(true)) {
    REXLOG_INFO("cod3: text buffers selftest: start");
    SelfTest(ctx, base, ctx.r3.u32);
    REXLOG_INFO("cod3: text buffers selftest: done; the game's own text again");
  }
  SetLinesOf(ctx, base, ctx.r3.u32, ctx.r4.u32);
}

namespace {
void SetLinesOf(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t in) {
  // 1. The "[ACTION]" pass.
  uint32_t text = 0;
  {
    cod3::guest::Call c(ctx, base);
#if defined(COD3_MP)
    text = uint32_t(c.Invoke(cod3::guest::VirtualSlot(base, self, 372), {self, in}));  // slot 93 (hooked below)
#else
    text = uint32_t(c.Invoke(kExpandActions, {in}));
#endif
  }
  const std::string s = ReadText(base, text);
  // 2. How many lines: one more than its '\n's, at most the element's room.
  const int32_t lines = 1 + int32_t(std::count(s.begin(), s.end(), '\n'));
  const int32_t room = int32_t(ReadU32(base, self + kLineMax));
  const int32_t count = lines - 1 < room ? lines : room;
  cod3::guest::WriteU32(base, self + kLineCount, uint32_t(count));
  // 3. Each line laid out from its own copy (the game's: a 256-byte stack buffer).
  size_t pos = 0;
  bool long_line = false;
  for (int32_t k = 0; k < count; ++k) {
    const size_t end = pos <= s.size() ? std::min(s.find('\n', pos), s.size()) : s.size();
    const std::string line = pos <= s.size() ? s.substr(pos, end - pos) : std::string();
    if (line.size() > kGameLine) long_line = true;
    const uint32_t at = PutText(base, kLine, line);
    if (at) {
      cod3::guest::Call c(ctx, base);
      c.Invoke(kSetLine, {ReadU32(base, self + kLines) + uint32_t(k) * kLineSize, at, ReadU32(base, self + kFont)},
               {F32(base, self + kScale), F32(base, self + kButtonScale)});
    }
    pos = end + 1;
  }
  if (long_line) ReportOnce("a line longer than the game's 256-byte line buffer", s);
  // 4. The text shown is the first line's, then the element finishes.
  {
    const uint32_t first = ReadU32(base, self + kLines);
    const uint32_t str = Valid(first) ? ReadU32(base, first + kLineString) : 0;
    cod3::guest::Call c(ctx, base);
    c.Invoke(kAssign, {self + kShown, str ? str + kStringChars : kEmpty});
  }
  {
    cod3::guest::Call c(ctx, base);
    c.Invoke(kFinish, {self});
  }
}

}  // namespace

// [acct2-main] 2026-11-21 4. The level's loading-screen text (text_buffers.h). Called right after the game's
// strncpy(field, src, 163) returned to the level info function; it then does `mr r3,<field register>` and strtok.
void LevelInfoCopied(PPCContext& ctx, uint8_t* base, uint32_t src) {
#if defined(COD3_MP)
  constexpr uint32_t kField = 0x82B792E8;
  PPCRegister& field = ctx.r29;
#else
  constexpr uint32_t kField = 0x829EACB8;
  PPCRegister& field = ctx.r30;
#endif
  constexpr size_t kFieldSize = 163;
  // Only that copy, as the function makes it (the field in its register), and only a text the field can't hold.
  if (!On() || field.u32 != kField || !Valid(src) || !Mapped(src, 1)) return;
  if (strnlen(reinterpret_cast<const char*>(base + src), kFieldSize) < kFieldSize) return;
  const std::string s = ReadText(base, src);
  // One block for every level, grown when a text needs more. The block it replaces is kept: the loading screen's
  // pointers into it are read until the next level's info replaces them, as they were into the game's field.
  static std::mutex m;
  static uint32_t at = 0, size = 0;
  std::lock_guard lock(m);
  if (s.size() + 1 > size) {
    uint32_t want = std::max<uint32_t>(1024, size * 2);
    while (want < s.size() + 1) want *= 2;
    const uint32_t got = rex::system::kernel_state()->memory()->SystemHeapAlloc(want);
    if (!got) {
      ReportOnce("no memory for the whole loading-screen text: cut as the game does", s);
      return;
    }
    at = got, size = want;
  }
  std::memcpy(base + at, s.c_str(), s.size() + 1);
  base[kField + kFieldSize - 1] = 0;  // the game's field ends too (strncpy left it with no zero)
  field.u64 = at;
  if (s.size() >= kMaxText) ReportOnce("a text longer than the 64 KB followed (the rest is not shown)", s);
  ReportOnce("the level's loading-screen text, longer than the game's 163-byte field", s);
}

void WithRoom(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame,
              uint32_t buffer_at, uint32_t buffer_size, const char* what) {
  const uint32_t r1 = ctx.r1.u32;
  // The room, and below it 16 KB for the function's own frame and its callees', must be the thread's stack.
  constexpr uint32_t kBelow = 16 * 1024;
  if (!On()) {
    original(ctx, base);
    return;
  }
  // [acct2-main] 2026-11-21 (owner 13:20: "test for extreme edge cases"): as much room as the thread's stack has, up to
  // the longest text followed (kMaxText) and at least kRoom: a text of any length the game can look up runs into it.
  // Within this thread's own stack: its low end is the PCR's stack_end_ptr (r13 + 0x74, rex/system/xthread.h), so the
  // room never reaches a neighbouring thread's stack, even where the two are mapped back to back.
  const uint32_t pcr = ctx.r13.u32;
  const uint32_t stack_low = Valid(pcr) && Mapped(pcr + 0x74, 4) ? ReadU32(base, pcr + 0x74) : 0;
  uint32_t room = 0;
  for (uint32_t want = uint32_t(kMaxText) + 4096; want >= kRoom; want /= 2) {
    const uint32_t try_low = (r1 - want) & ~0xFu;
    // The last range found mapped on this thread is kept: the word wrap runs for every line laid out.
    thread_local uint32_t ok_lo = 0, ok_hi = 0;
    const uint32_t lo = try_low - kBelow;
    if (try_low >= kBelow && (stack_low == 0 || stack_low >= r1 || lo >= stack_low) &&
        ((lo >= ok_lo && r1 <= ok_hi) || (Mapped(lo, r1 - lo) && (ok_lo = lo, ok_hi = r1, true)))) {
      room = want;
      break;
    }
  }
  if (!room) {
    ReportOnce("no stack for the room above a text buffer: the game's own code runs as it is", what);
    original(ctx, base);
    return;
  }
  const uint32_t low = (r1 - room) & ~0xFu;
  std::array<uint64_t, 18> gpr;
  std::array<double, 18> fpr;
  PPCRegister* const g[18] = {&ctx.r14, &ctx.r15, &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22,
                              &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  PPCRegister* const f[18] = {&ctx.f14, &ctx.f15, &ctx.f16, &ctx.f17, &ctx.f18, &ctx.f19, &ctx.f20, &ctx.f21, &ctx.f22,
                              &ctx.f23, &ctx.f24, &ctx.f25, &ctx.f26, &ctx.f27, &ctx.f28, &ctx.f29, &ctx.f30, &ctx.f31};
  for (int i = 0; i < 18; ++i) gpr[i] = g[i]->u64, fpr[i] = f[i]->f64;
  const auto cr2 = ctx.cr2, cr3 = ctx.cr3, cr4 = ctx.cr4;
  const uint64_t lr = ctx.lr;  // [acct2-main] 2026-11-21 its saved return address is in the part a long text runs over
  // [acct2-bro] 2026-11-22: the buffer starts empty, so a call that builds no text there (the hint's, on most frames)
  // isn't read back below as the stack's untouched fill (0xBE bytes: 2.47.0's runs logged it as 261-byte hints)
  base[low - frame + buffer_at] = 0;
  cod3::guest::WriteU32(base, low, r1);  // the room's link to the caller's frame, as a frame of the game's has
  ctx.r1.u64 = low;
  original(ctx, base);
  ctx.r1.u64 = r1;
  for (int i = 0; i < 18; ++i) g[i]->u64 = gpr[i], f[i]->f64 = fpr[i];
  ctx.cr2 = cr2, ctx.cr3 = cr3, ctx.cr4 = cr4;
  ctx.lr = lr;
  // What the buffer held when the function returned (its frame is below the room, still as it left it).
  const uint32_t buf = low - frame + buffer_at;
  const size_t len = strnlen(reinterpret_cast<const char*>(base + buf), room + frame);
  if (len >= buffer_size) ReportOnce(what, std::string(reinterpret_cast<const char*>(base + buf), len));
  // The text ran past the room too (into the caller's frame): say so loudly (never seen; the room is the stack's).
  if (len >= size_t(room) + frame - buffer_at)
    REXLOG_ERROR("cod3: text buffers: {}: {} bytes, more than the {} bytes of room: the caller's frame was written",
                 what, len, room);
}

}  // namespace cod3::textbuf

// The in-game hint (IGOHintWidget slot 2: "Press ~x to pick up the Thompson", the mounted gun's, a health pack's):
// it builds its text with sprintf, or copies a looked-up text byte by byte, into a 256-byte buffer on its stack, with
// no size check, then sets it on its FEMultiLineText. Run with room above its frame (text_buffers.h WithRoom): the
// whole text, however long, is what it sets. Frames: campaign 448 bytes, the buffer at +128 up to the saved registers
// at +384; cod3mp 480 bytes, the buffer at +128 up to +384 too.
// [acct2-main] 2026-11-21 (owner 13:20: "test for extreme edge cases"): the word wrap (FEMultiLineText's layout of a
// line, campaign 0x824DD290, cod3mp 0x824F6F98) copies each word (up to ' ', '-', '\n' or '\r') with strncpy(word
// length) into a buffer at +96 in its 512-byte frame, with no size check: a word of more than about 260 bytes ran
// over its saved registers. No pack has one today (the longest word is 45 bytes), but a long word with no break (a
// path, a row of Chinese or Japanese without spaces) would. Run with room above its frame, as the hint is.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_824F6F98);
REX_HOOK_RAW(sub_824F6F98) {
  cod3::textbuf::WithRoom(ctx, base, __imp__sub_824F6F98, 512, 96, 256,
                          "a word longer than the word wrap's 256-byte word buffer");
}
#else
REX_EXTERN(__imp__sub_824DD290);
REX_HOOK_RAW(sub_824DD290) {
  cod3::textbuf::WithRoom(ctx, base, __imp__sub_824DD290, 512, 96, 256,
                          "a word longer than the word wrap's 256-byte word buffer");
}
#endif

REX_EXTERN(__imp__IGOHintWidget__vfunc_2);
REX_HOOK_RAW(IGOHintWidget__vfunc_2) {
#if defined(COD3_MP)
  constexpr uint32_t kFrame = 480;
#else
  constexpr uint32_t kFrame = 448;
#endif
  cod3::textbuf::WithRoom(ctx, base, __imp__IGOHintWidget__vfunc_2, kFrame, 128, 256,
                          "an in-game hint longer than the game's 256-byte hint buffer");
}

#if defined(COD3_MP)
// cod3mp's "[ACTION]" pass, FEMultiLineText slot 93 (0x824D6C48; this, text). The game's: strncpy(copy, text, 512),
// then each space-separated word through slot 94 (this, word) -> its sentence, appended with strcat to a static
// 512-byte buffer with no bound, and the spaces between the words. A long text ran past that buffer. The same here,
// word by word through slot 94 (hooked by button_prompts.cpp), into a buffer of ours that fits.
// Its generated name is also a campaign name that mp_aliases.h points elsewhere (campaign slot 93 is cod3mp's 99),
// so the alias is set aside for these lines only.
#pragma push_macro("FEMultiLineText__vfunc_93")
#pragma push_macro("__imp__FEMultiLineText__vfunc_93")
#undef FEMultiLineText__vfunc_93
#undef __imp__FEMultiLineText__vfunc_93
REX_EXTERN(__imp__FEMultiLineText__vfunc_93);
REX_HOOK_RAW(FEMultiLineText__vfunc_93) {
  using namespace cod3::textbuf;
  if (!On()) {
    __imp__FEMultiLineText__vfunc_93(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32, text = ctx.r4.u32;
  if (!text) {
    ctx.r3.u64 = 0;
    return;
  }
  const std::string s = cod3::guest::ReadString(base, text, kMaxText);
  std::string result;
  const uint32_t word_slot = cod3::guest::VirtualSlot(base, self, kActionWordSlot);
  size_t i = 0, prev_end = std::string::npos;
  bool glued = false;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) break;
    const size_t start = i;
    while (i < s.size() && s[i] != ' ') ++i;
    if (prev_end != std::string::npos) result.append(start - prev_end, ' ');
    prev_end = i;
    // [acct2-main] 2026-11-21 1b. A word with a name joined to other text: slot 94 asked about each name alone, the
    // rest of the word kept as it is (slot 94 on the whole word gave the name's sentence only, or nothing).
    const std::string_view whole(s.data() + start, i - start);
    if (Glued(whole)) {
      glued = true;
      result += ExpandGluedWord(whole, [&](const std::string& n) {
        const uint32_t nw = PutText(base, kExpandIn, n);
        if (!nw) return n;
        cod3::guest::Call c(ctx, base);
        const uint32_t said = uint32_t(c.Invoke(word_slot, {self, nw}));
        return said ? cod3::guest::ReadString(base, said, kMaxText) : n;
      });
      continue;
    }
    const uint32_t word = PutText(base, kExpandIn, s.substr(start, i - start));
    if (!word) continue;
    cod3::guest::Call c(ctx, base);
    const uint32_t said = uint32_t(c.Invoke(word_slot, {self, word}));
    if (said) result += cod3::guest::ReadString(base, said, kMaxText);
  }
  if (glued) ReportOnce("a name joined to other text in one word (the game's pass keeps only the name)", s);
  if (s.size() >= 511 || result.size() >= 512) {
    static std::mutex m;
    static std::unordered_set<std::string> seen;
    std::lock_guard lock(m);
    if (seen.size() < 4096 && seen.insert(s).second)
      REXLOG_ERROR("cod3: text buffers: a text past cod3mp's \"[ACTION]\" pass's 512 bytes ({} in, {} out), shown "
                   "whole: \"{}\"", s.size(), result.size(), s.substr(0, 120));
  }
  const uint32_t at = PutText(base, kExpandOut, result);
  ctx.r3.u64 = at ? at : kEmpty;  // the heap full: an empty text, never a pointer to nothing
}
#pragma pop_macro("__imp__FEMultiLineText__vfunc_93")
#pragma pop_macro("FEMultiLineText__vfunc_93")
#endif
