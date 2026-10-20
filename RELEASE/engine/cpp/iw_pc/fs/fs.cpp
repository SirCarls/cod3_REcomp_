#include "iw_pc/fs/fs.h"

#include <string>

#include "iw_pc/cstr.h"
#include "iw_pc/shared/strings.h"

namespace iw_pc::fs {

namespace {

// Q_strcat(info, sizeof(info), va("%i ", number)). Never fails: the
// buffer's last byte is always 0.
void append(ByteSpan info, std::int32_t number) {
    std::string text = std::to_string(number) + " ";
    shared::strings::strcat(info, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

}  // namespace

std::vector<std::uint8_t> referenced_pure_checksums(Game game, std::span<const SearchPath> search_paths,
                                                    std::int32_t feed, std::int32_t fake) {
    std::vector<std::uint8_t> info(kPureChecksumsSize, 0);
    std::int32_t checksum = feed;
    std::uint32_t count = 0;
    // United Offensive's passes: the client game, the user interface, then
    // every referenced archive. Call of Duty 2's: only the last.
    int first_pass = game == Game::CallOfDuty2 ? 0 : 2;
    for (int pass = first_pass; pass >= 0; --pass) {
        if (pass == 0) {
            std::size_t at = cstr::len(info);
            info[at] = '@';
            info[at + 1] = ' ';
            info[at + 2] = 0;
        }
        for (const SearchPath& path : search_paths) {
            if (!path.archive || path.localized) continue;
            const Archive& archive = *path.archive;
            bool flag = pass == 2   ? archive.referenced.cgame_module
                        : pass == 1 ? archive.referenced.ui_module
                                    : archive.referenced.any_file;
            if (!flag) continue;
            append(info, archive.pure_checksum);
            if (pass != 0) break;
            checksum ^= archive.pure_checksum;
            ++count;
        }
        if (fake != 0) append(info, fake);
    }
    append(info, static_cast<std::int32_t>(count) ^ checksum);
    info.resize(cstr::len(info));
    return info;
}

}  // namespace iw_pc::fs
