// Requests for the shared functions (strings, info strings, hashes, filters).

#include "iw_pc/cstr.h"
#include "iw_pc/shared/filter.h"
#include "iw_pc/shared/hash.h"
#include "iw_pc/shared/info.h"
#include "iw_pc/shared/strings.h"
#include "protocol.h"

namespace pc_check {

namespace strings = iw_pc::shared::strings;
namespace info = iw_pc::shared::info;
namespace hash = iw_pc::shared::hash;
namespace filter = iw_pc::shared::filter;
using iw_pc::ByteSpan;
using iw_pc::ByteView;
using iw_pc::Game;

namespace {

Answer from_status(const iw_pc::Status& status) { return status ? game_error(*status) : nothing(); }

Answer printed(const std::optional<info::Refusal>& refusal, Game game) {
    if (!refusal) return Answer(Ret(Printed{}));
    const char* format = info::message_format(*refusal, game);
    return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
}

}  // namespace

std::optional<Answer> dispatch_shared(Game game, const std::string& function, Args& args) {
    bool cod2 = game == Game::CallOfDuty2;
    const std::string& f = function;
    // --- strings ---------------------------------------------------------------
    if (f == "Q_stricmpn") return integer(strings::stricmpn(args.text(0), args.text(1), args.i32(2)));
    if (f == "Q_strncmp") return integer(strings::strncmp(args.text(0), args.text(1), args.i32(2)));
    if (f == "Q_stricmp" && cod2) return integer(strings::stricmp(args.text(0), args.text(1)));
    if (f == "Q_stricmp") {
        const Bytes *a = args.str(0), *b = args.str(1);
        ByteView va = a ? ByteView(*a) : ByteView{}, vb = b ? ByteView(*b) : ByteView{};
        return integer(strings::stricmp_nullable(a ? &va : nullptr, b ? &vb : nullptr));
    }
    if (f == "Q_strcmp" && cod2) return integer(strings::strcmp(args.text(0), args.text(1)));
    if (f == "sub_0044FAA0" && !cod2) return integer(strings::stricmp_equal_or_minus_one(args.text(0), args.text(1)));
    if (f == "Q_strncpyz") {
        auto size = static_cast<std::size_t>(args.int64(2));
        Bytes src(args.text(1).begin(), args.text(1).end());
        Bytes* dest = args.buffer(0);
        if (!dest) return std::nullopt;
        strings::strncpyz(ByteSpan(*dest).first(size), src);
        return nothing();
    }
    if (f == "Q_strlwr" || f == "Q_strupr" || f == "Q_CleanStr") {
        Bytes* buffer = args.buffer(0);
        if (!buffer) return std::nullopt;
        if (f == "Q_strlwr") strings::strlwr(*buffer);
        else if (f == "Q_strupr") strings::strupr(*buffer);
        else strings::clean_str(*buffer);
        return string(iw_pc::cstr::text(*buffer));
    }
    if (f == "Q_strcat") {
        auto size = static_cast<std::size_t>(args.int64(1));
        Bytes src(args.text(2).begin(), args.text(2).end());
        Bytes* dest = args.buffer(0);
        if (!dest) return std::nullopt;
        return from_status(strings::strcat(ByteSpan(*dest).first(size), src));
    }
    if (f == "Q_PrintStrlen") return integer(strings::print_strlen(args.text(0)));
    if (f == "COM_SkipPath") return string(strings::skip_path(args.text(0)));
    if (f == "Com_GetExtensionSubString" && cod2) return string(strings::extension_with_dot(args.text(0)));
    if (f == "COM_StripExtension") {
        ByteView name = args.text(0);
        ByteView kept = cod2 ? strings::strip_extension(name) : strings::strip_extension_at_first_dot(name);
        Bytes copy(kept.begin(), kept.end());
        Bytes* out = args.buffer(1);
        if (!out) return Answer::fail("NULL output");
        if (!iw_pc::cstr::write_at(*out, 0, copy)) return Answer::fail("output buffer too small");
        return nothing();
    }
    if (f == "COM_DefaultExtension") {
        auto max_size = static_cast<std::size_t>(args.int64(1));
        Bytes extension(args.text(2).begin(), args.text(2).end());
        Bytes* path = args.buffer(0);
        if (!path) return std::nullopt;
        strings::default_extension(*path, max_size, extension);
        return nothing();
    }
    // --- info strings ------------------------------------------------------------
    if (f == "Info_ValueForKey") {
        const Bytes *text = args.str(0), *key = args.str(1);
        if (!text || !key) return string(ByteView{});  // the original's NULL check
        auto value = info::value_for_key(*text, *key);
        return value ? string(value.value()) : game_error(value.error());
    }
    if (f == "Info_RemoveKey" || f == "Info_RemoveKey_Big") {
        Bytes key(args.text(1).begin(), args.text(1).end());
        Bytes* buffer = args.buffer(0);
        if (!buffer) return std::nullopt;
        return from_status(f == "Info_RemoveKey" ? info::remove_key(*buffer, key) : info::remove_key_big(*buffer, key));
    }
    if (f == "Info_Validate") return integer(info::validate(args.text(0)) ? 1 : 0);
    if (f == "Info_SetValueForKey" || f == "Info_SetValueForKey_Big") {
        Bytes key(args.text(1).begin(), args.text(1).end());
        Bytes value(args.text(2).begin(), args.text(2).end());
        Bytes* buffer = args.buffer(0);
        if (!buffer) return std::nullopt;
        auto result = f == "Info_SetValueForKey" ? info::set_value_for_key(*buffer, key, value)
                                                 : info::set_value_for_key_big(game, *buffer, key, value);
        return printed(result, game);
    }
    // --- hashes --------------------------------------------------------------------
    if (f == "FS_HashFileName") {
        return integer(hash::file_name_hash(args.text(0), static_cast<std::uint32_t>(args.int64(1))));
    }
    if (f == "generateHashValue") {
        const Bytes* name = args.str(0);
        if (!name) return game_error(iw_pc::GameError::drop("null name in generateHashValue"));
        return integer(hash::cvar_hash(*name));
    }
    if (f == "Com_HashKey") return integer(hash::hash_key(args.text(0), args.i32(1)));
    // --- filters -------------------------------------------------------------------
    if (f == "Com_Filter" || f == "Com_FilterPath") {
        ByteView pattern = args.text(0), name = args.text(1);
        bool case_sensitive = args.int64(2) != 0;
        bool matched;
        if (f == "Com_Filter") {
            matched = cod2 ? filter::filter_cod2(pattern, name, case_sensitive)
                           : filter::filter(pattern, name, case_sensitive);
        } else {
            matched = cod2 ? filter::filter_path_cod2(pattern, name, case_sensitive)
                           : filter::filter_path(pattern, name, case_sensitive);
        }
        return integer(matched ? 1 : 0);
    }
    return std::nullopt;
}

}  // namespace pc_check
