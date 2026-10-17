// cod3 - decomp trace build: tests of the trace files' rules (trace_format.h).
//
// Built in the trace build folder only, on request:
//   cmake --build out/build/trace --target cod3_trace_format_test && out/build/trace/cod3_trace_format_test
// Prints each failed check and returns 1 if any failed.

#include <cstdio>
#include <string>
#include <vector>

#include "trace_format.h"

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failed;
  }
}

void CheckEq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::printf("FAIL: %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
    ++g_failed;
  }
}

void TestTsvField() {
  using cod3::trace::TsvField;
  CheckEq(TsvField("plain"), "plain", "plain text stays");
  CheckEq(TsvField("a\tb"), "a\\tb", "tab escaped");
  CheckEq(TsvField("line\n"), "line\\n", "line break escaped");
  CheckEq(TsvField("back\\slash"), "back\\\\slash", "backslash escaped");
  CheckEq(TsvField("cr\r"), "cr\\r", "carriage return escaped");
}

void TestMarkerFor() {
  using cod3::trace::MarkerFor;
  using cod3::trace::MarkerTime;
  // Marker 0: F8 at 100 s, saved at 105 s. Marker 1: F8 at 200 s, saved at 210 s.
  const std::vector<MarkerTime> markers = {{100000, 105000}, {200000, 210000}};
  Check(MarkerFor({}, 5000) == -1, "no markers: none");
  Check(MarkerFor(markers, 60000) == -1, "long before the first marker: none");
  Check(MarkerFor(markers, 70000) == 0, "30 s before F8: the marker");
  Check(MarkerFor(markers, 103000) == 0, "between F8 and saving: the marker");
  Check(MarkerFor(markers, 114000) == 0, "within 10 s after saving: the marker");
  Check(MarkerFor(markers, 116000) == -1, "more than 10 s after: none");
  Check(MarkerFor(markers, 175000) == 1, "30 s before the second F8: the second marker");
  // Inside both windows: the marker saved after it (it says what just happened).
  const std::vector<MarkerTime> close = {{100000, 101000}, {110000, 112000}};
  Check(MarkerFor(close, 105000) == 1, "between two close markers: the later one");
  Check(MarkerFor(close, 113000) == 1, "after both: the last one");
  Check(MarkerFor({{10000, 12000}}, 0) == 0, "a marker in the first 30 s covers the start");
}

void TestFunctionHolding() {
  using cod3::trace::FunctionHolding;
  using cod3::trace::FunctionName;
  const std::vector<FunctionName> functions = {{0x82000100, "first"}, {0x82000200, "second"}, {0x82000300, "third"}};
  Check(FunctionHolding(functions, 0x820000FC) == nullptr, "before the first function: none");
  Check(FunctionHolding(functions, 0x82000100) == &functions[0], "a function's own start");
  Check(FunctionHolding(functions, 0x820002FC) == &functions[1], "inside the second function");
  Check(FunctionHolding(functions, 0x82000400) == &functions[2], "after the last start: the last function");
  Check(FunctionHolding({}, 0x82000100) == nullptr, "no table: none");
}

void TestCallerText() {
  using cod3::trace::CallerText;
  using cod3::trace::FunctionName;
  const FunctionName frame{0x82451200, "Com_Frame"};
  const FunctionName level{0x89031000, "sub_89031000"};
  CheckEq(CallerText("default", true, &frame, 0x8245123C), "Com_Frame+0x3C", "executable caller");
  CheckEq(CallerText("saint_lo", false, &level, 0x89031044), "saint_lo!sub_89031000+0x44", "level caller");
  CheckEq(CallerText("", false, nullptr, 0x89031044), "level?!0x89031044", "unknown level");
  CheckEq(CallerText("", false, nullptr, 0x70001000), "?!0x70001000", "unknown module");
  CheckEq(CallerText("saint_lo", false, nullptr, 0x89031044), "saint_lo!0x89031044", "module without function");
}

void TestPrivateWords() {
  using cod3::trace::HasPrivateWord;
  const std::vector<std::string> words = {"jdoe", "desktop-1234"};
  Check(HasPrivateWord("save/JDoe/profile", words), "user name found, any case");
  Check(HasPrivateWord("DESKTOP-1234", words), "host name found");
  Check(!HasPrivateWord("maps/saint_lo.d3dbsp", words), "game path kept");
  Check(!HasPrivateWord("anything", {}), "no words: nothing dropped");
}

}  // namespace

int main() {
  TestTsvField();
  TestMarkerFor();
  TestFunctionHolding();
  TestCallerText();
  TestPrivateWords();
  if (g_failed) {
    std::printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  std::printf("trace format: all checks passed\n");
  return 0;
}
