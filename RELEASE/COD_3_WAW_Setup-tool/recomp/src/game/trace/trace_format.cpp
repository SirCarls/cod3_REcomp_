// cod3 - decomp trace build: the plain rules of the trace files (trace_format.h).

#include "trace_format.h"

#include <algorithm>
#include <cstdio>

namespace cod3::trace {

std::string TsvField(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\\': out += "\\\\"; break;
      default: out += c;
    }
  }
  return out;
}

int MarkerFor(const std::vector<MarkerTime>& markers, uint32_t ms) {
  int before = -1;
  for (size_t i = 0; i < markers.size(); ++i) {
    const MarkerTime& m = markers[i];
    const uint32_t from = m.pressed_ms > kMarkerBeforeMs ? m.pressed_ms - kMarkerBeforeMs : 0;
    if (ms < from || ms > m.saved_ms + kMarkerAfterMs) continue;
    if (ms <= m.saved_ms) return int(i);
    before = int(i);
  }
  return before;
}

const FunctionName* FunctionHolding(const std::vector<FunctionName>& functions, uint32_t address) {
  auto it = std::upper_bound(functions.begin(), functions.end(), address,
                             [](uint32_t a, const FunctionName& f) { return a < f.address; });
  return it == functions.begin() ? nullptr : &*(it - 1);
}

std::string CallerText(std::string_view module, bool main, const FunctionName* function, uint32_t site) {
  char buffer[64];
  if (module.empty()) {
    std::snprintf(buffer, sizeof(buffer), "%s!0x%08X", site >= 0x89000000u ? "level?" : "?", site);
    return buffer;
  }
  std::string text = main ? std::string() : std::string(module) + "!";
  if (!function) {
    std::snprintf(buffer, sizeof(buffer), "0x%08X", site);
  } else {
    text += function->name;
    std::snprintf(buffer, sizeof(buffer), "+0x%X", site - function->address);
  }
  return text + buffer;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

bool HasPrivateWord(std::string_view text, const std::vector<std::string>& words) {
  if (words.empty()) return false;
  const std::string lower = Lower(text);
  return std::any_of(words.begin(), words.end(),
                     [&](const std::string& w) { return !w.empty() && lower.find(w) != std::string::npos; });
}

}  // namespace cod3::trace
