// Wildcard filters for names and paths (*, ?, [a-z]), used to match file
// names and ban lists.
//
// United Offensive's Com_Filter is Quake III's: a * followed by text looks
// for that text anywhere in the rest of the name. Call of Duty 2's compares
// the whole rest of the name at each place, so the text only matches at the
// very end: *mp_* matches names ending in mp_.
//
// Quirks both keep: [[ doesn't escape a bracket; ]] inside a class is a ];
// a ? or a class can move past the end of the name, and an unclosed class
// past the end of the filter, where the originals read whatever memory
// follows (the ports read zeros there).
#pragma once

#include "iw_pc/base.h"

namespace iw_pc::shared::filter {

/// The most of a filter or name Com_FilterPath looks at.
inline constexpr std::size_t kFilterPathKeep = 63;

/// United Offensive's Com_Filter: whether `name` matches `filter`.
/// Original: Com_Filter at 0x004351B0 (CoDUOMP.exe 1.51)
bool filter(ByteView filter, ByteView name, bool case_sensitive);

/// Call of Duty 2's Com_Filter: the text after a * must be the whole rest
/// of the name.
/// Original: Com_Filter at 0x0042D660 (CoD2MP_s.exe 1.3)
bool filter_cod2(ByteView filter, ByteView name, bool case_sensitive);

/// filter() on paths: both cut to 63 characters, \ and : read as /.
/// Original: Com_FilterPath at 0x004353D0 (CoDUOMP.exe 1.51)
bool filter_path(ByteView filter, ByteView name, bool case_sensitive);

/// filter_cod2() on paths, cut and converted like filter_path().
/// Original: Com_FilterPath at 0x0042D860 (CoD2MP_s.exe 1.3)
bool filter_path_cod2(ByteView filter, ByteView name, bool case_sensitive);

}  // namespace iw_pc::shared::filter
