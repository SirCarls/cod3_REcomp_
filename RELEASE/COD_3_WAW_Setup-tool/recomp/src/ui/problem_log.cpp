// cod3 - warnings and errors of this session, grouped. See problem_log.h.

#include "ui/problem_log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <memory>

#include <spdlog/sinks/base_sink.h>

#include <rex/logging.h>

namespace cod3::ui {

namespace {

// spdlog's levels (spdlog::level::level_enum).
constexpr int kInfoLevel = 2;
constexpr int kWarnLevel = 3;
constexpr int kErrorLevel = 4;

constexpr std::string_view kGamePrefix = "[game] ";            // print_hooks.cpp, LogGameLine
constexpr std::string_view kRepeatPrefix = "(previous line repeated ";  // ... N more times)
constexpr std::string_view kComErrorPrefix = "Com_Error(";     // print_hooks.cpp, the Com_Error hook
constexpr std::string_view kPortPrefix = "cod3: ";             // the port's own lines

bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  return true;
}

bool IsHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Leading spaces and ^N color codes off a game line.
std::string_view WithoutLeadingColors(std::string_view s) {
  for (;;) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    if (s.size() >= 2 && s[0] == '^' && std::isdigit(static_cast<unsigned char>(s[1])))
      s.remove_prefix(2);
    else
      return s;
  }
}

// The text without ^N color codes (the game's lines carry them).
std::string WithoutColors(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '^' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
      ++i;
      continue;
    }
    out += text[i];
  }
  return out;
}

std::string Plural(uint64_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// One kind of message: the line with numbers and quoted names replaced, so
// "path='d:\_thai\'" and "path='d:\_leet\'" are the same kind. 32-bit codes
// written 0x12345678 stay, so different codes stay apart.
std::string GroupKey(Problem::Severity severity, std::string_view source, std::string_view text) {
  std::string key;
  key.reserve(source.size() + text.size() + 4);
  key += severity == Problem::Severity::kError ? 'E' : 'W';
  key += source;
  key += '|';
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '\'' || c == '"') {
      const size_t close = text.find(c, i + 1);
      if (close != std::string_view::npos && close - i <= 260) {
        key += c;
        key += '*';
        key += c;
        i = close + 1;
        continue;
      }
    }
    if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
      size_t j = i + 2;
      while (j < text.size() && IsHex(text[j])) ++j;
      if (j - i == 10) {
        for (size_t k = i; k < j; ++k) key += static_cast<char>(std::tolower(static_cast<unsigned char>(text[k])));
      } else {
        key += "0x#";
      }
      i = j;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      key += '#';
      while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
      continue;
    }
    key += c;
    ++i;
  }
  return key;
}

std::string ClockText(std::chrono::system_clock::time_point t) {
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[16];
  if (!std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm)) return "--:--:--";
  return buf;
}

// The game's Com_Error codes (Quake III / Call of Duty numbering).
const char* ComErrorName(long code) {
  switch (code) {
    case 0: return "ERR_FATAL";
    case 1: return "ERR_DROP";
    case 2: return "ERR_SERVERDISCONNECT";
    case 3: return "ERR_DISCONNECT";
    default: return nullptr;
  }
}

struct NamedCode {
  uint32_t code;
  const char* name;
};

// NTSTATUS (the Xbox 360 kernel's X_STATUS values are the same numbers) and
// HRESULT codes the SDK and the graphics drivers report.
constexpr NamedCode kStatusNames[] = {
    {0x00000102, "STATUS_TIMEOUT"},
    {0x00000103, "STATUS_PENDING"},
    {0x80000005, "STATUS_BUFFER_OVERFLOW"},
    {0x80000006, "STATUS_NO_MORE_FILES"},
    {0x8000001A, "STATUS_NO_MORE_ENTRIES"},
    {0xC0000001, "STATUS_UNSUCCESSFUL"},
    {0xC0000002, "STATUS_NOT_IMPLEMENTED"},
    {0xC0000004, "STATUS_INFO_LENGTH_MISMATCH"},
    {0xC0000005, "STATUS_ACCESS_VIOLATION"},
    {0xC0000008, "STATUS_INVALID_HANDLE"},
    {0xC000000D, "STATUS_INVALID_PARAMETER"},
    {0xC000000E, "STATUS_NO_SUCH_DEVICE"},
    {0xC000000F, "STATUS_NO_SUCH_FILE"},
    {0xC0000010, "STATUS_INVALID_DEVICE_REQUEST"},
    {0xC0000011, "STATUS_END_OF_FILE"},
    {0xC0000013, "STATUS_NO_MEDIA_IN_DEVICE"},
    {0xC0000017, "STATUS_NO_MEMORY"},
    {0xC0000018, "STATUS_CONFLICTING_ADDRESSES"},
    {0xC000001D, "STATUS_ILLEGAL_INSTRUCTION"},
    {0xC0000022, "STATUS_ACCESS_DENIED"},
    {0xC0000023, "STATUS_BUFFER_TOO_SMALL"},
    {0xC0000024, "STATUS_OBJECT_TYPE_MISMATCH"},
    {0xC0000033, "STATUS_OBJECT_NAME_INVALID"},
    {0xC0000034, "STATUS_OBJECT_NAME_NOT_FOUND"},
    {0xC0000035, "STATUS_OBJECT_NAME_COLLISION"},
    {0xC0000039, "STATUS_OBJECT_PATH_INVALID"},
    {0xC000003A, "STATUS_OBJECT_PATH_NOT_FOUND"},
    {0xC0000043, "STATUS_SHARING_VIOLATION"},
    {0xC000007F, "STATUS_DISK_FULL"},
    {0xC0000094, "STATUS_INTEGER_DIVIDE_BY_ZERO"},
    {0xC000009A, "STATUS_INSUFFICIENT_RESOURCES"},
    {0xC00000BA, "STATUS_FILE_IS_A_DIRECTORY"},
    {0xC00000BB, "STATUS_NOT_SUPPORTED"},
    {0xC00000FD, "STATUS_STACK_OVERFLOW"},
    {0xC0000101, "STATUS_DIRECTORY_NOT_EMPTY"},
    {0xC0000102, "STATUS_FILE_CORRUPT_ERROR"},
    {0xC0000103, "STATUS_NOT_A_DIRECTORY"},
    {0xC0000120, "STATUS_CANCELLED"},
    {0xC0000135, "STATUS_DLL_NOT_FOUND"},
    {0xC0000225, "STATUS_NOT_FOUND"},
    {0x80004001, "E_NOTIMPL"},
    {0x80004002, "E_NOINTERFACE"},
    {0x80004003, "E_POINTER"},
    {0x80004004, "E_ABORT"},
    {0x80004005, "E_FAIL"},
    {0x8000FFFF, "E_UNEXPECTED"},
    {0x80070005, "E_ACCESSDENIED"},
    {0x80070006, "E_HANDLE"},
    {0x8007000E, "E_OUTOFMEMORY"},
    {0x80070057, "E_INVALIDARG"},
    {0x887A0001, "DXGI_ERROR_INVALID_CALL"},
    {0x887A0002, "DXGI_ERROR_NOT_FOUND"},
    {0x887A0004, "DXGI_ERROR_UNSUPPORTED"},
    {0x887A0005, "DXGI_ERROR_DEVICE_REMOVED"},
    {0x887A0006, "DXGI_ERROR_DEVICE_HUNG"},
    {0x887A0007, "DXGI_ERROR_DEVICE_RESET"},
    {0x887A0020, "DXGI_ERROR_DRIVER_INTERNAL_ERROR"},
    {0x887A0022, "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE"},
    {0x887A0026, "DXGI_ERROR_ACCESS_LOST"},
    {0x887A0027, "DXGI_ERROR_WAIT_TIMEOUT"},
};

// Win32 error numbers (the Xbox 360's X_ERROR values are the same), for
// the ones inside an HRESULT (0x8007xxxx).
constexpr NamedCode kWin32Names[] = {
    {1, "ERROR_INVALID_FUNCTION"},   {2, "ERROR_FILE_NOT_FOUND"},      {3, "ERROR_PATH_NOT_FOUND"},
    {5, "ERROR_ACCESS_DENIED"},      {6, "ERROR_INVALID_HANDLE"},      {8, "ERROR_NOT_ENOUGH_MEMORY"},
    {18, "ERROR_NO_MORE_FILES"},     {32, "ERROR_SHARING_VIOLATION"},  {50, "ERROR_NOT_SUPPORTED"},
    {87, "ERROR_INVALID_PARAMETER"}, {122, "ERROR_INSUFFICIENT_BUFFER"}, {126, "ERROR_MOD_NOT_FOUND"},
    {127, "ERROR_PROC_NOT_FOUND"},   {160, "ERROR_BAD_ARGUMENTS"},     {183, "ERROR_ALREADY_EXISTS"},
    {193, "ERROR_BAD_EXE_FORMAT"},   {258, "WAIT_TIMEOUT"},            {997, "ERROR_IO_PENDING"},
    {1167, "ERROR_DEVICE_NOT_CONNECTED"}, {1168, "ERROR_NOT_FOUND"},   {1223, "ERROR_CANCELLED"},
    {1460, "ERROR_TIMEOUT"},
};

// Vulkan results as the SDK prints them (vk::to_string).
constexpr const char* kVulkanErrors[] = {
    "ErrorOutOfHostMemory",     "ErrorOutOfDeviceMemory",  "ErrorInitializationFailed",
    "ErrorDeviceLost",          "ErrorMemoryMapFailed",    "ErrorLayerNotPresent",
    "ErrorExtensionNotPresent", "ErrorFeatureNotPresent",  "ErrorIncompatibleDriver",
    "ErrorTooManyObjects",      "ErrorFormatNotSupported", "ErrorFragmentedPool",
    "ErrorUnknown",             "ErrorOutOfPoolMemory",    "ErrorInvalidExternalHandle",
    "ErrorFragmentation",       "ErrorSurfaceLostKHR",     "ErrorNativeWindowInUseKHR",
    "ErrorOutOfDateKHR",        "ErrorIncompatibleDisplayKHR", "ErrorValidationFailedEXT",
    "ErrorFullScreenExclusiveModeLostEXT",
};

const char* Win32Name(uint32_t code) {
  for (const NamedCode& n : kWin32Names)
    if (n.code == code) return n.name;
  return nullptr;
}

// An error name the SDK or a system API prints: X_STATUS_..., VK_ERROR_ ...
bool IsErrorName(std::string_view tok) {
  static constexpr std::string_view kPrefixes[] = {"X_STATUS_", "X_ERROR_", "X_E_",        "STATUS_",
                                                   "ERROR_",    "VK_ERROR_", "DXGI_ERROR_", "D3D12_ERROR_",
                                                   "E_"};
  bool prefixed = false;
  for (auto p : kPrefixes)
    if (tok.size() > p.size() + 1 && StartsWith(tok, p)) prefixed = true;
  if (!prefixed) return false;
  for (char c : tok)
    if (!(std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_'))
      return false;
  return true;
}

// A 32-bit code written "0x1234ABCD" or as eight hex digits alone.
bool ParseCode(std::string_view tok, uint32_t* out) {
  if (tok.size() == 10 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) tok.remove_prefix(2);
  if (tok.size() != 8) return false;
  uint32_t v = 0;
  for (char c : tok) {
    if (!IsHex(c)) return false;
    v = v * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(c))
                                           ? c - '0'
                                           : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
  }
  *out = v;
  return true;
}

// "0xC000000F STATUS_NO_SUCH_FILE", or empty for a number that isn't a code
// we know the kind of (most eight-digit hex numbers in the log are addresses).
std::string DescribeCode(uint32_t v) {
  char buf[96];
  if (auto name = StatusCodeName(v); !name.empty()) {
    std::snprintf(buf, sizeof(buf), "0x%08X %.*s", v, static_cast<int>(name.size()), name.data());
    return buf;
  }
  if ((v & 0xFFFF0000u) == 0x80070000u) {
    const uint32_t win32 = v & 0xFFFFu;
    const char* name = Win32Name(win32);
    std::snprintf(buf, sizeof(buf), "0x%08X (Win32 error %u%s%s)", v, win32, name ? " " : "", name ? name : "");
    return buf;
  }
  if (v >= 0xC0000000u && v <= 0xC0000FFFu) {
    std::snprintf(buf, sizeof(buf), "0x%08X (an NTSTATUS error)", v);
    return buf;
  }
  if ((v & 0xFFFF0000u) == 0x887A0000u) {
    std::snprintf(buf, sizeof(buf), "0x%08X (a DXGI error)", v);
    return buf;
  }
  return {};
}

// Hands every log line to the problem log. Never logs itself (it runs
// inside spdlog's own lock).
class ProblemSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    ProblemLog::Get().OnLogLine(static_cast<int>(msg.level),
                                std::string_view(msg.logger_name.data(), msg.logger_name.size()),
                                std::string_view(msg.payload.data(), msg.payload.size()), msg.time);
  }
  void flush_() override {}
};

}  // namespace

std::string_view StatusCodeName(uint32_t code) {
  for (const NamedCode& n : kStatusNames)
    if (n.code == code) return n.name;
  return {};
}

std::string FindErrorCode(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    if (!IsWordChar(text[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < text.size() && IsWordChar(text[j])) ++j;
    const std::string_view tok = text.substr(i, j - i);
    i = j;
    if (IsErrorName(tok)) return std::string(tok);
    if (StartsWith(tok, "Error")) {
      for (const char* v : kVulkanErrors)
        if (tok == v) return "VkResult " + std::string(tok);
    }
    uint32_t code = 0;
    if (ParseCode(tok, &code)) {
      if (std::string described = DescribeCode(code); !described.empty()) return described;
    }
  }
  return {};
}

ProblemLog& ProblemLog::Get() {
  // Never destroyed: loggers may still write while the process ends.
  static ProblemLog* log = new ProblemLog();
  return *log;
}

void ProblemLog::Install() {
  {
    std::lock_guard lock(mutex_);
    if (installed_) return;
    installed_ = true;
  }
  rex::AddSink(std::make_shared<ProblemSink>());
}

void ProblemLog::OnLogLine(int level, std::string_view category, std::string_view text,
                           std::chrono::system_clock::time_point when) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
  if (level < kWarnLevel) {
    // Below warning only the game's own prints count, and only those that
    // say they are a warning or an error.
    if (level != kInfoLevel || !StartsWith(text, kGamePrefix)) return;
    const std::string_view line = text.substr(kGamePrefix.size());
    if (StartsWith(line, kRepeatPrefix)) {
      // print_hooks.cpp counts a line repeated within a second instead of
      // logging it again: add those to the group of the line before.
      uint64_t n = 0;
      for (size_t i = kRepeatPrefix.size(); i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])); ++i)
        n = n * 10 + static_cast<uint64_t>(line[i] - '0');
      std::lock_guard lock(mutex_);
      if (n && last_game_group_ < groups_.size()) {
        Problem& g = groups_[last_game_group_];
        g.count += n;
        (g.severity == Problem::Severity::kError ? errors_ : warnings_) += n;
        ++generation_;
      }
      return;
    }
    const std::string_view plain = WithoutLeadingColors(line);
    Problem::Severity severity;
    if (StartsWithNoCase(plain, "warning")) {
      severity = Problem::Severity::kWarning;
    } else if (StartsWithNoCase(plain, "error")) {
      severity = Problem::Severity::kError;
      // The Com_Error hook prints its message as "^1ERROR: ..." too; it is
      // already counted from its error line.
      std::lock_guard lock(mutex_);
      if (!last_com_error_.empty() && plain.size() > 7 && plain.substr(7) == last_com_error_) {
        last_com_error_.clear();
        last_game_group_ = SIZE_MAX;
        return;
      }
    } else {
      std::lock_guard lock(mutex_);
      last_game_group_ = SIZE_MAX;
      return;
    }
    const std::string clean = WithoutColors(line);
    Add(severity, "game", clean, when, {});
    return;
  }

  const Problem::Severity severity = level >= kErrorLevel ? Problem::Severity::kError : Problem::Severity::kWarning;
  if (StartsWith(text, kComErrorPrefix)) {
    // "Com_Error(1): <message>" from the Com_Error hook (print_hooks.cpp).
    long code = std::strtol(std::string(text.substr(kComErrorPrefix.size(), 12)).c_str(), nullptr, 10);
    std::string code_text = "Com_Error " + std::to_string(code);
    if (const char* name = ComErrorName(code)) code_text += std::string(" ") + name;
    const size_t colon = text.find("): ");
    const std::string message = WithoutColors(colon == std::string_view::npos ? text : text.substr(colon + 3));
    {
      std::lock_guard lock(mutex_);
      last_com_error_ = message;
    }
    Add(severity, "game", message, when, std::move(code_text));
    return;
  }
  std::string_view source = category;
  if (StartsWith(text, kPortPrefix)) source = "port";
  Add(severity, source.empty() ? std::string_view("sdk") : source, text, when, {});
}

void ProblemLog::Add(Problem::Severity severity, std::string_view source, std::string_view text,
                     std::chrono::system_clock::time_point when, std::string code) {
  if (text.size() > kMaxText) text = text.substr(0, kMaxText);
  std::string key = GroupKey(severity, source, text);
  std::lock_guard lock(mutex_);
  ++generation_;
  (severity == Problem::Severity::kError ? errors_ : warnings_) += 1;
  const bool from_game = source == "game";
  if (auto it = index_.find(key); it != index_.end()) {
    Problem& g = groups_[it->second];
    ++g.count;
    g.last_seen = when;
    g.last_order = ++order_;
    if (g.text != text) {
      const bool known = std::find(g.examples.begin(), g.examples.end(), g.text) != g.examples.end();
      if (!known && g.examples.size() < kMaxExamples) g.examples.push_back(g.text);
      g.text.assign(text);
    }
    if (g.code.empty()) g.code = code.empty() ? FindErrorCode(text) : std::move(code);
    if (from_game) last_game_group_ = it->second;
    return;
  }
  if (groups_.size() >= kMaxGroups) {
    ++dropped_;
    if (from_game) last_game_group_ = SIZE_MAX;
    return;
  }
  Problem p;
  p.severity = severity;
  p.source.assign(source);
  p.code = code.empty() ? FindErrorCode(text) : std::move(code);
  p.text.assign(text);
  p.count = 1;
  p.first_seen = p.last_seen = when;
  p.last_order = ++order_;
  index_.emplace(std::move(key), groups_.size());
  groups_.push_back(std::move(p));
  if (from_game) last_game_group_ = groups_.size() - 1;
}

uint64_t ProblemLog::generation() const {
  std::lock_guard lock(mutex_);
  return generation_;
}

size_t ProblemLog::group_count() const {
  std::lock_guard lock(mutex_);
  return groups_.size();
}

uint64_t ProblemLog::error_lines() const {
  std::lock_guard lock(mutex_);
  return errors_;
}

uint64_t ProblemLog::warning_lines() const {
  std::lock_guard lock(mutex_);
  return warnings_;
}

uint64_t ProblemLog::dropped_lines() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

void ProblemLog::CopyNewestFirst(std::vector<Problem>& out) const {
  {
    std::lock_guard lock(mutex_);
    out.assign(groups_.begin(), groups_.end());
  }
  std::sort(out.begin(), out.end(), [](const Problem& a, const Problem& b) { return a.last_order > b.last_order; });
}

void ProblemLog::Clear() {
  std::lock_guard lock(mutex_);
  groups_.clear();
  index_.clear();
  errors_ = warnings_ = dropped_ = 0;
  last_game_group_ = SIZE_MAX;
  last_com_error_.clear();
  ++generation_;
}

std::string ProblemLog::FormatLine(const Problem& p) {
  std::string line = "[" + ClockText(p.last_seen) + "] ";
  line += p.severity == Problem::Severity::kError ? "error" : "warning";
  line += " x" + std::to_string(p.count) + " " + p.source;
  if (!p.code.empty()) line += " [" + p.code + "]";
  line += ": " + p.text;
  if (p.count > 1) line += " (first at " + ClockText(p.first_seen) + ")";
  return line;
}

std::string ProblemLog::AsText(size_t max_groups) const {
  std::vector<Problem> list;
  CopyNewestFirst(list);
  uint64_t errors = 0, warnings = 0, dropped = 0;
  {
    std::lock_guard lock(mutex_);
    errors = errors_;
    warnings = warnings_;
    dropped = dropped_;
  }
  std::string out = "Problems this session: " + Plural(errors, "error", "errors") + ", " +
                    Plural(warnings, "warning", "warnings") + " (" + Plural(list.size(), "kind", "kinds") +
                    "), newest first.\n";
  size_t shown = 0;
  for (const Problem& p : list) {
    if (max_groups && shown >= max_groups) {
      out += "... and " + std::to_string(list.size() - shown) + " more kinds (F2 > Console > Problems)\n";
      break;
    }
    out += FormatLine(p) + "\n";
    ++shown;
  }
  if (dropped)
    out += std::to_string(dropped) + " more lines of other kinds weren't kept (the list holds " +
           std::to_string(kMaxGroups) + " kinds).\n";
  return out;
}

}  // namespace cod3::ui
