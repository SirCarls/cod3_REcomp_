// cod3mp - map packs copied into userdata by hand, put right before the game looks for them (map_packs_mp.h).
// [acct2-main] 2026-11-13 (owner 19:38: "make sure those weak points have been tested/fixed").
//
// What cod3mp needs of a map pack (acct2-decomp 2026-11-06, acct2-bro 2026-11-08; the setup tool's c3w/addons.py
// has the details):
//   - Its folder's last character is its pack number. MPPopulateContent (0x824150B8) mounts each pack on the drive
//     "dlc" + the last character of the pack's file name, and SV_SpawnServer looks for a pack's maps on dlc<N>, N
//     from the pack's own mp/dlmaps.txt (the MapPack column: Champs 1, Valor 2, Bravo 3). The three packages' own
//     names all end in "41", so copied in under those names all three want dlc1: one mounts, and the other packs'
//     maps are "unknown".
//   - A content header, userdata/0000000000000000/415607E1/Headers/00000002/<folder>.header: the SDK's
//     XCONTENT_AGGREGATE_DATA (328 bytes), as the console kept one for each download. Without it the SDK lists the
//     pack from its folder name, and on the 11-05 build cod3mp got a list entry it couldn't match, so the pack was
//     never mounted (acct2-decomp NOTES 2026-11-06 05:55).
// The setup tool does both when it installs a pack, and its ensure_names() and ensure_headers() keep its own older
// installs right on each run, so the packs its userdata/addons.json lists are left to it. Every other folder that
// is named as a package is (40 to 42 hexadecimal digits) and has a readable mp/dlmaps.txt is put right here, once,
// before the game starts. Nothing in a pack is read but its dlmaps.txt, and nothing in it is changed: only the
// folder's name and the header file beside it.

#include "game/mp/map_packs_mp.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/logging.h>

#include "util.h"

namespace cod3::mppacks {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kTitleId = 0x415607E1;
constexpr uint32_t kMarketplace = 0x00000002;  // content type 2: a download
constexpr uint32_t kDeviceHdd = 1;             // the SDK's DummyDeviceId::HDD
constexpr size_t kHeaderSize = 0x148;          // sizeof(XCONTENT_AGGREGATE_DATA)
constexpr size_t kDisplayName = 0x008;         // UTF-16BE, 128 characters, the last one kept 0
constexpr size_t kFileName = 0x108;            // the folder's name, 42 bytes, no end marker needed
constexpr size_t kFileNameSize = 42;
constexpr size_t kTitle = 0x140;               // after the xuid at 0x138 (0: content every profile sees)
constexpr std::uintmax_t kMaxDlmaps = 64 << 10;

bool IsPackageName(std::string_view name) {
  return name.size() >= 40 && name.size() <= kFileNameSize &&
         std::all_of(name.begin(), name.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string_view Trim(std::string_view s) {
  const auto first = s.find_first_not_of(" \r");
  if (first == std::string_view::npos) return {};
  const auto last = s.find_last_not_of(" \r");
  return s.substr(first, last - first + 1);
}

// A small file's bytes ("" when it is missing or bigger than limit).
std::string ReadSmallFile(const fs::path& path, std::uintmax_t limit) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > limit) return {};
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The pack number that dlmaps.txt gives its maps, read as the setup tool's parse_dlmaps() reads the file: "//"
// comments and blank lines skipped, the columns lined up with runs of tabs, 7 of them, the first one MapPack, and
// a heading line "MapPack ..." skipped. 0 when the file is missing or too big, has no map line or a line without 7
// columns, gives more than one pack number, or a number that isn't one digit from 1 to 9 (one character of a
// folder name).
int PackNumber(const fs::path& dlmaps) {
  const std::string text = ReadSmallFile(dlmaps, kMaxDlmaps);
  int number = 0;
  for (size_t start = 0; start < text.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string_view line = std::string_view(text).substr(start, end - start);
    start = end + 1;
    const auto lead = line.find_first_not_of(" \t\r");
    if (lead == std::string_view::npos || line.substr(lead, 2) == "//") continue;  // blank lines and comments
    std::vector<std::string_view> fields;
    for (size_t f = 0; f <= line.size();) {
      size_t t = line.find('\t', f);
      if (t == std::string_view::npos) t = line.size();
      if (const auto piece = Trim(line.substr(f, t - f)); !piece.empty()) fields.push_back(piece);
      f = t + 1;
    }
    if (fields.size() != 7) return 0;
    std::string first(fields[0]);
    std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (first == "mappack") continue;
    if (first.size() != 1 || first[0] < '1' || first[0] > '9') return 0;
    const int n = first[0] - '0';
    if (number && n != number) return 0;
    number = n;
  }
  return number;
}

std::string DisplayName(int pack) {
  switch (pack) {  // the setup tool's names (KNOWN_PACKS)
    case 1: return "Champs map";
    case 2: return "Valor map pack";
    case 3: return "Bravo map pack";
    default: return "Map pack " + std::to_string(pack);
  }
}

void PutBe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i));
}

// The header the setup tool's content_header() writes: device 1 (the hard drive), type 2, the display name, the
// folder's name, xuid 0 and the title. No licence mask follows (as ensure_headers(): there's no package file here
// to take one from, and neither program asks for a licence).
std::vector<uint8_t> ContentHeader(const std::string& name, const std::string& display) {
  std::vector<uint8_t> h(kHeaderSize, 0);
  PutBe32(h, 0x000, kDeviceHdd);
  PutBe32(h, 0x004, kMarketplace);
  for (size_t i = 0; i < display.size() && i < 127; ++i)  // plain ASCII names: each is a 0 byte and the character
    h[kDisplayName + 2 * i + 1] = uint8_t(display[i]);
  std::copy(name.begin(), name.end(), h.begin() + kFileName);  // IsPackageName: at most 42 characters
  PutBe32(h, kTitle, kTitleId);
  return h;
}

// A new file first, then the rename, so a header is never half written.
bool WriteFileWhole(const fs::path& path, const std::vector<uint8_t>& data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path temp = path;
  temp += ".cod3mp-new";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    out.close();
    if (!out) {
      fs::remove(temp, ec);
      return false;
    }
  }
  fs::rename(temp, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temp, ignored);
    return false;
  }
  return true;
}

// The setup tool lists each pack it installed in userdata/addons.json by its folder's name, in quotes ("id") and
// at the end of its "folder" path. Package names are only hexadecimal digits, so a text search finds them.
bool ListedBySetupTool(const std::string& registry, const std::string& name) {
  return !registry.empty() &&
         (registry.find('"' + name + '"') != std::string::npos || registry.find('/' + name + '"') != std::string::npos);
}

}  // namespace

void Tidy(const fs::path& user_data_root) {
  if (user_data_root.empty()) return;
  const fs::path title = user_data_root / "0000000000000000" / "415607E1";
  const fs::path content = title / "00000002";
  const fs::path headers = title / "Headers" / "00000002";
  std::error_code ec;
  if (!fs::is_directory(content, ec)) return;
  std::vector<std::string> names;
  for (fs::directory_iterator it(content, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code dir_ec;
    if (!it->is_directory(dir_ec)) continue;
    std::string name = cod3::PathToUtf8(it->path().filename());
    if (IsPackageName(name)) names.push_back(std::move(name));
  }
  if (names.empty()) return;
  std::sort(names.begin(), names.end());
  const std::string registry = ReadSmallFile(user_data_root / "addons.json", 1 << 20);
  int renamed = 0, written = 0, setup_tool = 0, left = 0;
  for (std::string name : names) {
    if (ListedBySetupTool(registry, name)) {
      ++setup_tool;
      continue;
    }
    const fs::path folder = content / cod3::Utf8ToPath(name);
    const int pack = PackNumber(folder / "mp" / "dlmaps.txt");
    if (!pack) {
      REXLOG_INFO("cod3mp: map packs: {} has no mp/dlmaps.txt with one pack number; left as it is", name);
      ++left;
      continue;
    }
    std::string want = name;
    want.back() = char('0' + pack);
    if (want != name) {
      std::error_code rename_ec;
      if (fs::exists(content / cod3::Utf8ToPath(want), rename_ec)) {
        REXLOG_WARN("cod3mp: map packs: {} is pack {}, but {} is there already (a second copy of it?); {} is left as "
                    "it is, and it shares the drive dlc{}: with any other pack whose name ends the same way",
                    name, pack, want, name, name.back());
        ++left;
        continue;
      }
      fs::rename(folder, content / cod3::Utf8ToPath(want), rename_ec);
      if (rename_ec) {
        REXLOG_WARN("cod3mp: map packs: couldn't rename {} to {} ({}); its maps stay unknown", name, want,
                    rename_ec.message());
        ++left;
        continue;
      }
      // A header that came with it (another emulator writes one) follows, with the new name inside.
      const fs::path old_head = headers / cod3::Utf8ToPath(name + ".header");
      const std::string old_data = ReadSmallFile(old_head, 4096);
      if (old_data.size() >= kHeaderSize) {
        std::vector<uint8_t> data(old_data.begin(), old_data.end());
        std::fill_n(data.begin() + kFileName, kFileNameSize, uint8_t(0));
        std::copy(want.begin(), want.end(), data.begin() + kFileName);
        std::error_code remove_ec;
        if (WriteFileWhole(headers / cod3::Utf8ToPath(want + ".header"), data)) fs::remove(old_head, remove_ec);
      }
      REXLOG_INFO("cod3mp: map packs: renamed {} to {} (pack {} in its dlmaps.txt), so the game mounts it on dlc{}:, "
                  "where it looks for that pack's maps", name, want, pack, pack);
      ++renamed;
      name = want;
    }
    std::error_code head_ec;
    const fs::path head = headers / cod3::Utf8ToPath(name + ".header");
    if (!fs::exists(head, head_ec)) {
      if (WriteFileWhole(head, ContentHeader(name, DisplayName(pack)))) {
        REXLOG_INFO("cod3mp: map packs: wrote {}'s content header ({}, pack {})", name, DisplayName(pack), pack);
        ++written;
      } else {
        REXLOG_WARN("cod3mp: map packs: couldn't write {}; the game may not mount {}", cod3::PathToUtf8(head), name);
      }
    }
  }
  REXLOG_INFO("cod3mp: map packs: {} found; {} installed by the setup tool (left to it), {} renamed, {} given a "
              "header, {} left as they were", names.size(), setup_tool, renamed, written, left);
}

}  // namespace cod3::mppacks
