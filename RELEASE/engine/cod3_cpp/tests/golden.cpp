// Replays the original game's recorded answers (engine/tools/decomp_check/golden/*.txt,
// written by `python3 tools/decomp/decomp.py golden` from the game's own code in
// the checker's emulator) through the C++ version. [acct2-decomp]
//
//   cod3_golden <golden folder>
//
// The same files and the same comparison as the Rust crate's
// engine/tools/decomp_check/tests/golden.rs: exactly, except that any NaN
// matches any NaN (the PowerPC and x86 make different NaN bits) and a case may
// allow a few units in the last place. Functions the C++ version doesn't have
// yet (it answers "err unknown-function") are counted and listed, not failed.
// It needs no game files and no emulator.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cod3_check/protocol.h"

namespace {

struct Case {
    std::string request;
    std::string reply;
    std::uint32_t ulps = 0;
    std::set<std::size_t> float_tokens;  // reply tokens (0 = the result) that are buffers of big-endian floats
};

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(text);
    while (std::getline(in, part, separator)) {
        parts.push_back(part);
    }
    if (!text.empty() && text.back() == separator) {
        parts.emplace_back();
    }
    return parts;
}

std::vector<Case> ReadCases(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() == ".txt") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<Case> cases;
    for (const auto& path : files) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::vector<std::string> fields = Split(line, '\t');
            if (fields.size() < 2) continue;
            Case c;
            c.request = fields[0];
            c.reply = fields[1];
            if (fields.size() > 2) {
                for (const std::string& rule : Split(fields[2], ' ')) {
                    if (rule.rfind("ulps=", 0) == 0) {
                        c.ulps = static_cast<std::uint32_t>(std::stoul(rule.substr(5)));
                    } else if (rule.rfind("floats=", 0) == 0) {
                        for (const std::string& n : Split(rule.substr(7), ',')) {
                            c.float_tokens.insert(static_cast<std::size_t>(std::stoul(n)));
                        }
                    }
                }
            }
            cases.push_back(std::move(c));
        }
    }
    return cases;
}

// Distance between two float bit patterns in units in the last place.
std::uint64_t UlpDistance(std::uint32_t a, std::uint32_t b) {
    auto ordered = [](std::uint32_t x) -> std::int64_t {
        return x < 0x80000000u ? static_cast<std::int64_t>(x) : 0x80000000LL - static_cast<std::int64_t>(x);
    };
    const std::int64_t d = ordered(a) - ordered(b);
    return static_cast<std::uint64_t>(d < 0 ? -d : d);
}

bool FloatsMatch(std::uint32_t a, std::uint32_t b, std::uint32_t ulps) {
    const float fa = std::bit_cast<float>(a);
    const float fb = std::bit_cast<float>(b);
    return (std::isnan(fa) && std::isnan(fb)) || a == b || UlpDistance(a, b) <= ulps;
}

std::vector<std::uint8_t> Hex(const std::string& s) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

bool TokensMatch(const std::string& expected, const std::string& actual, std::uint32_t ulps, bool float_buffer) {
    if (expected == actual) return true;
    if (expected.size() < 2 || actual.size() < 2 || expected.compare(0, 2, actual, 0, 2) != 0) return false;
    const std::string kind = expected.substr(0, 2);
    if (kind == "f:") {
        return FloatsMatch(static_cast<std::uint32_t>(std::stoul(expected.substr(2), nullptr, 16)),
                           static_cast<std::uint32_t>(std::stoul(actual.substr(2), nullptr, 16)), ulps);
    }
    if (kind == "d:") {
        const double e = std::bit_cast<double>(static_cast<std::uint64_t>(std::stoull(expected.substr(2), nullptr, 16)));
        const double a = std::bit_cast<double>(static_cast<std::uint64_t>(std::stoull(actual.substr(2), nullptr, 16)));
        return std::isnan(e) && std::isnan(a);
    }
    if (kind == "b:" && float_buffer && expected.size() == actual.size()) {
        const std::vector<std::uint8_t> e = Hex(expected.substr(2));
        const std::vector<std::uint8_t> a = Hex(actual.substr(2));
        if (e.size() % 4 != 0) return false;
        for (std::size_t k = 0; k < e.size(); k += 4) {
            auto bits = [](const std::vector<std::uint8_t>& b, std::size_t at) {
                return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
                       (std::uint32_t{b[at + 2]} << 8) | std::uint32_t{b[at + 3]};
            };
            if (!FloatsMatch(bits(e, k), bits(a, k), ulps)) return false;
        }
        return true;
    }
    return false;
}

bool RepliesMatch(const Case& c, const std::string& actual_reply) {
    const std::vector<std::string> expected = Split(c.reply, ' ');
    const std::vector<std::string> actual = Split(actual_reply, ' ');
    if (expected[0] == "err") {
        // The port must refuse the same input with the same exit (the level may follow).
        return actual.size() >= 2 && expected.size() >= 2 && actual[0] == expected[0] && actual[1] == expected[1];
    }
    if (expected.size() != actual.size() || expected[0] != actual[0]) return false;
    for (std::size_t i = 1; i < expected.size(); ++i) {
        if (!TokensMatch(expected[i], actual[i], c.ulps, c.float_tokens.count(i - 1) != 0)) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: cod3_golden <golden folder>\n");
        return 2;
    }
    const std::vector<Case> cases = ReadCases(argv[1]);
    if (cases.empty()) {
        std::printf("no recorded answers found in %s\n", argv[1]);
        return 1;
    }
    std::map<std::string, std::pair<int, int>> per_function;  // name -> (matched, cases)
    std::map<std::string, int> not_ported;
    int differing = 0;
    int shown = 0;
    for (const Case& c : cases) {
        const std::string name = c.request.substr(0, c.request.find(' '));
        const std::string reply = cod3::check::answer(c.request);
        if (reply.rfind("err unknown-function", 0) == 0) {
            ++not_ported[name];
            continue;
        }
        auto& tally = per_function[name];
        ++tally.second;
        if (RepliesMatch(c, reply)) {
            ++tally.first;
        } else {
            ++differing;
            if (shown++ < 10) {
                std::printf("DIFFERS %s\n    original: %s\n    C++:      %s\n", c.request.c_str(), c.reply.c_str(),
                            reply.c_str());
            }
        }
    }
    int checked = 0;
    for (const auto& [name, tally] : per_function) {
        std::printf("%-28s %d/%d\n", name.c_str(), tally.first, tally.second);
        checked += tally.second;
    }
    int skipped = 0;
    for (const auto& [name, count] : not_ported) skipped += count;
    std::printf("%zu functions, %d recorded answers: %d differ. Not in C++ yet: %zu functions (%d answers)\n",
                per_function.size(), checked, differing, not_ported.size(), skipped);
    return differing == 0 && checked > 0 ? 0 : 1;
}
