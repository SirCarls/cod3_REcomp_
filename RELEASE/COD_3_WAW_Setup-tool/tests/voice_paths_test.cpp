// Tests for recomp/src/game/voice_paths.h (which banks the game registers for
// the chosen voices). Plain C++: tests/test_voice_paths.py builds and runs it
// with the system's compiler.
//
//   c++ -std=c++17 -I recomp/src tests/voice_paths_test.cpp && ./a.out

#include "game/voice_paths.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using cod3::voice::PlanBank;
using cod3::voice::Registration;
using Plan = std::vector<Registration>;

namespace {

int failures = 0;

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// A made-up game folder: the files a test has, matched without case as the
// game's file lookups are.
struct Files {
  std::set<std::string> have;
  explicit Files(std::initializer_list<const char*> files) {
    for (const char* f : files) have.insert(Lower(f));
  }
  bool operator()(const std::string& p) const { return have.count(Lower(p)) != 0; }
};

std::string Show(const Plan& plan) {
  std::string s = "{";
  for (const auto& r : plan) s += " (" + r.path + ", " + std::to_string(r.slot) + ")";
  return s + " }";
}

void Expect(const char* what, const Plan& got, const Plan& want) {
  if (got == want) return;
  ++failures;
  std::printf("FAIL %s\n  got  %s\n  want %s\n", what, Show(got).c_str(), Show(want).c_str());
}

void ExpectEq(const char* what, long got, long want) {
  if (got == want) return;
  ++failures;
  std::printf("FAIL %s: got %ld, want %ld\n", what, got, want);
}

constexpr int EN = 0, DE = 1, FR = 2, ES = 3, IT = 4;

// The USA disc's banks with the German disc's and the Fr/Es/It disc's copied
// in (c3w/voices.py), as the real discs lay them out.
const Files kAll{
    "sp\\blkbrn\\blkbrn.wbk",          "sp\\blkbrn\\blkbrn.wde",          "sp\\blkbrn\\sound\\blkbrn.wul",
    "sp\\blkbrn\\sound\\blkbrn.wfr",   "sp\\blkbrn\\sound\\blkbrn.wes",   "sp\\blkbrn\\sound\\blkbrn.wit",
    "sp\\bcen_usa.wbk",                "sp\\bcen_usa.wde",                "sp\\bcen_usa.wfr",
    "sp\\bcen_usa.wes",                "sp\\bcen_usa.wit",                "sp\\global.wbk",
    "sp\\global.wde",                  "sp\\sound\\global.wul",
};
const Files kUsaOnly{"sp\\blkbrn\\blkbrn.wbk", "sp\\bcen_usa.wbk", "sp\\global.wbk"};

void UsaGame() {
  const std::string level = "sp\\blkbrn\\blkbrn.wbk";
  Expect("disc voices change nothing", PlanBank(level, 5, EN, -1, kAll), Plan{{level, 5}});
  Expect("English on the English game changes nothing", PlanBank(level, 5, EN, EN, kAll), Plan{{level, 5}});
  Expect("German: the whole German bank in the English bank's slot", PlanBank(level, 5, EN, DE, kAll),
         Plan{{"sp\\blkbrn\\blkbrn.wde", 5}});
  Expect("French: the split pair, its speech in the game's language slot", PlanBank(level, 5, EN, FR, kAll),
         Plan{{"sp\\blkbrn\\sound\\blkbrn.wul", 5}, {"sp\\blkbrn\\sound\\blkbrn.wfr", 0}});
  Expect("Italian: the same with .wit", PlanBank(level, 5, EN, IT, kAll),
         Plan{{"sp\\blkbrn\\sound\\blkbrn.wul", 5}, {"sp\\blkbrn\\sound\\blkbrn.wit", 0}});
  Expect("no other disc's files: the game's own bank", PlanBank(level, 5, EN, DE, kUsaOnly), Plan{{level, 5}});
  Expect("speech without its sounds: the game's own bank",
         PlanBank(level, 5, EN, ES, Files{"sp\\blkbrn\\blkbrn.wbk", "sp\\blkbrn\\sound\\blkbrn.wes"}),
         Plan{{level, 5}});
  Expect("battle chatter banks are whole on the Fr/Es/It disc", PlanBank("sp\\bcen_usa.wbk", 5, EN, ES, kAll),
         Plan{{"sp\\bcen_usa.wes", 5}});
  Expect("global has no speech of its own on the Fr/Es/It disc: kept",
         PlanBank("sp\\global.wbk", 5, EN, FR, kAll), Plan{{"sp\\global.wbk", 5}});
  Expect("global's German bank", PlanBank("sp\\global.wbk", 5, EN, DE, kAll), Plan{{"sp\\global.wde", 5}});
  Expect("a device and capitals are kept as the game spells them",
         PlanBank("d:\\SP\\BlkBrn\\BlkBrn.WBK", 5, EN, DE, Files{"d:\\sp\\blkbrn\\blkbrn.wde"}),
         Plan{{"d:\\SP\\BlkBrn\\BlkBrn.wde", 5}});
  Expect("not a bank", PlanBank("sp\\blkbrn\\blkbrn", 5, EN, DE, kAll), Plan{{"sp\\blkbrn\\blkbrn", 5}});
  Expect("not a sound bank", PlanBank("sp\\blkbrn\\blkbrn.cod", 5, EN, DE, kAll),
         Plan{{"sp\\blkbrn\\blkbrn.cod", 5}});
}

void OtherGames() {
  // A game built from the Fr/Es/It disc (its language French): the pair and
  // the other two languages' speech are registered.
  const std::string wul = "sp\\blkbrn\\sound\\blkbrn.wul";
  Expect("English sounds and speech in place of the split sounds", PlanBank(wul, 5, FR, EN, kAll),
         Plan{{"sp\\blkbrn\\blkbrn.wbk", 5}});
  Expect("and the French speech goes", PlanBank("sp\\blkbrn\\sound\\blkbrn.wfr", 2, FR, EN, kAll), Plan{});
  Expect("speech of a language the game doesn't load stays",
         PlanBank("sp\\blkbrn\\sound\\blkbrn.wes", 3, FR, EN, kAll),
         Plan{{"sp\\blkbrn\\sound\\blkbrn.wes", 3}});
  Expect("Spanish on the French game: the sounds stay", PlanBank(wul, 5, FR, ES, kAll), Plan{{wul, 5}});
  Expect("and the Spanish speech takes the French slot",
         PlanBank("sp\\blkbrn\\sound\\blkbrn.wfr", 2, FR, ES, kAll),
         Plan{{"sp\\blkbrn\\sound\\blkbrn.wes", 2}});
  // A game built from the German disc.
  Expect("English on the German game", PlanBank("sp\\blkbrn\\blkbrn.wde", 1, DE, EN, kAll),
         Plan{{"sp\\blkbrn\\blkbrn.wbk", 1}});
  Expect("a game language out of range changes nothing", PlanBank("sp\\blkbrn\\blkbrn.wbk", 5, 7, DE, kAll),
         Plan{{"sp\\blkbrn\\blkbrn.wbk", 5}});
}

void Codes() {
  ExpectEq("en", cod3::voice::LanguageFromCode("en"), 0);
  ExpectEq("it", cod3::voice::LanguageFromCode("it"), 4);
  ExpectEq("disc", cod3::voice::LanguageFromCode("disc"), -1);
  // XGetLanguage's numbers, as 0x824EED20 picks a movie's sound track by them
  ExpectEq("console en", cod3::voice::ConsoleLanguageFor(EN), 1);
  ExpectEq("console de", cod3::voice::ConsoleLanguageFor(DE), 3);
  ExpectEq("console fr", cod3::voice::ConsoleLanguageFor(FR), 4);
  ExpectEq("console es", cod3::voice::ConsoleLanguageFor(ES), 5);
  ExpectEq("console it", cod3::voice::ConsoleLanguageFor(IT), 6);
}

}  // namespace

int main() {
  UsaGame();
  OtherGames();
  Codes();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("voice_paths: all passed\n");
  return 0;
}
