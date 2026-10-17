// cod3 - which sound bank files the game registers for the chosen voices
// (game/voice_language.h). Plain C++ with no SDK, so it can be tested alone.
//
// How the game finds its speech (default.xex, the same code on every disc):
// each archive's AUDIO list names its sound banks; the extension says whose
// language a bank is: .wen .wde .wfr .wes .wit are languages 0-4 (English,
// German, French, Spanish, Italian), anything else (.wbk, .wul) belongs to
// every language (5). 0x82478B80 registers each bank in a slot by language,
// and 0x82494E90 loads slot 5 and the slot of the game's language (0x82A2A150,
// which the archives set: English on the USA disc). The discs store voices
// two ways:
//   whole banks (sounds and speech together): sp/blkbrn/blkbrn.wbk (English,
//     slot 5), .wde (Germany), and the Fr/Es/It disc's battle chatter sp/bcen_usa.wfr
//   split banks (Fr/Es/It disc): the sounds in sp/blkbrn/sound/blkbrn.wul
//     (slot 5), the speech in sound/blkbrn.wfr / .wes / .wit (slots 2-4).
// A bank of another language that sits in assets/ (copied from its disc)
// takes the place of the one the archive names: a whole bank of the voice
// language takes a whole bank's slot; a split pair takes it with its sounds
// in slot 5 and its speech in the game's language slot, so the game loads
// both and nothing in the English bank.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cod3::voice {

constexpr int kLanguages = 5;     // the game's order: en de fr es it
constexpr uint32_t kEveryLanguage = 5;
inline constexpr const char* kCodes[kLanguages] = {"en", "de", "fr", "es", "it"};
inline constexpr const char* kNames[kLanguages] = {"English", "German", "French", "Spanish", "Italian"};
// A language's own bank extension (AUDIO list tag), and the extension of its
// whole banks on its disc: English discs keep theirs in .wbk.
inline constexpr const char* kTagExt[kLanguages] = {".wen", ".wde", ".wfr", ".wes", ".wit"};
inline constexpr const char* kWholeExt[kLanguages] = {".wbk", ".wde", ".wfr", ".wes", ".wit"};

// "en" ... "it" -> 0-4; anything else ("disc") -> -1.
inline int LanguageFromCode(std::string_view code) {
  for (int i = 0; i < kLanguages; ++i)
    if (code == kCodes[i]) return i;
  return -1;
}

// The console language (XGetLanguage) whose movie sound track the game opens
// first in 0x824EED20: 1 English -> -en, 3 German -> -de, 4 French -> -fr,
// 5 Spanish -> -es, 6 Italian -> -it.
inline uint32_t ConsoleLanguageFor(int language) {
  constexpr uint32_t kConsole[kLanguages] = {1, 3, 4, 5, 6};
  return language >= 0 && language < kLanguages ? kConsole[language] : 1;
}

struct Registration {
  std::string path;   // as the game spells paths: backslashes, maybe a "d:\" device
  uint32_t slot;
  bool operator==(const Registration& o) const { return path == o.path && slot == o.slot; }
};

namespace detail {
inline std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}
inline int TagLanguage(std::string_view ext) {
  for (int i = 0; i < kLanguages; ++i)
    if (ext == kTagExt[i]) return i;
  return -1;
}
}  // namespace detail

// What to register in place of the game's own registration (path, slot) so
// that `voice` (0-4) is heard. `disc_language` is the game's language
// (0x82A2A150); `exists` says whether a game path has a file. An empty plan
// drops the registration; a plan of the registration itself changes nothing.
inline std::vector<Registration> PlanBank(const std::string& path, uint32_t slot, int disc_language, int voice,
                                          const std::function<bool(const std::string&)>& exists) {
  using detail::Lower;
  const std::vector<Registration> same{{path, slot}};
  if (voice < 0 || voice >= kLanguages || voice == disc_language || disc_language < 0 ||
      disc_language >= kLanguages)
    return same;
  const size_t cut = path.find_last_of('\\');
  const std::string dir = cut == std::string::npos ? std::string() : path.substr(0, cut + 1);
  const std::string file = cut == std::string::npos ? path : path.substr(cut + 1);
  const size_t dot = file.find_last_of('.');
  if (dot == std::string::npos) return same;
  const std::string name = file.substr(0, dot);
  const std::string ext = Lower(file.substr(dot));
  const std::string ldir = Lower(dir);
  const bool in_sound = ldir.size() >= 6 && ldir.compare(ldir.size() - 6, 6, "sound\\") == 0;
  const std::string base_dir = in_sound ? dir.substr(0, dir.size() - 6) : dir;

  const std::string whole = base_dir + name + kWholeExt[voice];
  const std::string split_sounds = base_dir + "sound\\" + name + ".wul";
  const std::string split_speech = base_dir + "sound\\" + name + kTagExt[voice];
  const auto is = [&](const std::string& p) { return Lower(p) != Lower(path) && exists(p); };

  const int tagged = detail::TagLanguage(ext);
  if (!in_sound && (ext == ".wbk" || tagged >= 0)) {
    // A whole bank (of the disc's language, or the game's neutral .wbk).
    if (is(whole)) return {{whole, slot}};
    if (exists(split_sounds) && exists(split_speech))
      return {{split_sounds, kEveryLanguage}, {split_speech, uint32_t(disc_language)}};
    return same;
  }
  if (in_sound && ext == ".wul") {
    // The sounds of a split bank: a whole bank of the voice stands in for the pair.
    if (is(whole)) return {{whole, kEveryLanguage}};
    return same;
  }
  if (in_sound && tagged >= 0) {
    // The speech of a split bank. Only the game's language slot is ever loaded.
    if (slot != uint32_t(disc_language)) return same;
    if (exists(whole)) return {};  // the whole bank took the sounds' slot
    if (is(split_speech)) return {{split_speech, slot}};
    return same;
  }
  return same;
}

}  // namespace cod3::voice
