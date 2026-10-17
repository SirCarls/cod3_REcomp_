// cod3 - decomp trace build: the plain rules of the trace files, kept apart
// from the game so they can be tested on their own (trace_format_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "trace_abi.h"

namespace cod3::trace {

// A function first seen from kMarkerBeforeMs before the marker key was
// pressed to kMarkerAfterMs after the marker was saved gets the marker.
inline constexpr uint32_t kMarkerBeforeMs = 30000;
inline constexpr uint32_t kMarkerAfterMs = 10000;

// One TSV field: tabs, line breaks and backslashes escaped as \t \n \r \\
// (tools/trace/merge_traces.py undoes it).
std::string TsvField(std::string_view text);

struct MarkerTime {
  uint32_t pressed_ms;  // the marker key (or the command)
  uint32_t saved_ms;
};

// The marker a function first seen at `ms` gets (an index into `markers`,
// which are in the order they were saved), or -1: the first marker saved at
// or after it whose window holds it (a marker says what just happened),
// else the last one saved before it whose window holds it.
int MarkerFor(const std::vector<MarkerTime>& markers, uint32_t ms);

// The function of `functions` (sorted by address) that holds `address`:
// the last one starting at or before it, or nullptr.
const FunctionName* FunctionHolding(const std::vector<FunctionName>& functions, uint32_t address);

// A caller as functions.tsv shows it: "Com_Frame+0x3C" in the executable,
// "saint_lo!sub_89031000+0x44" in a level library, "level?!0x89031044" when
// the module isn't known, "saint_lo!0x89031044" when the function isn't.
// `module` empty = not known; `main` = the executable's code.
std::string CallerText(std::string_view module, bool main, const FunctionName* function, uint32_t site);

// Whether `text` holds one of `words` (lower case), ignoring case.
bool HasPrivateWord(std::string_view text, const std::vector<std::string>& words);

// Lower case, ASCII only.
std::string Lower(std::string_view text);

}  // namespace cod3::trace
