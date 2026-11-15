// cod3mp - map packs copied into userdata by hand, put right before the game looks for them.
// [acct2-main] 2026-11-13 (owner 19:38: "make sure those weak points have been tested/fixed").
//
// The setup tool's Add-ons button installs the Xbox 360 map packs the way cod3mp needs them (c3w/addons.py):
// each pack's folder ends in its pack number, and each has a content header. A pack copied in by hand (from the
// download, or from another emulator's content folder) has neither, and then cod3mp mounts the wrong packs or
// none (map_packs_mp.cpp says why). Tidy() gives such packs the same two things at start, and leaves every pack
// the setup tool installed to the setup tool.

#pragma once

#include <filesystem>

namespace cod3::mppacks {

// Looks at every folder in USERDATA/0000000000000000/415607E1/00000002 (the content root the SDK uses: the
// program's user_data_root) and, for each map pack the setup tool didn't install: renames its folder so that its
// last character is its pack number (dlmaps.txt's MapPack column), carrying its header along, and writes a content
// header if it has none. Says in the log what it changed and what it left alone. Never throws; does nothing when
// there are no packs.
void Tidy(const std::filesystem::path& user_data_root);

}  // namespace cod3::mppacks
