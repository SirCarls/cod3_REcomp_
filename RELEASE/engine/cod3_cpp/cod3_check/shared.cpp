// [acct2-decomp] Requests for the shared area (strings, info strings, name and
// table hashes): the same names, arguments and replies as the Rust answerer's
// engine/tools/decomp_check/src/shared.rs.
//
// A string argument (s:) is its characters; here it gets its terminator. A
// buffer (b:) is the guest's array as the function saw it; the functions get a
// copy with zeros after it, so a function that writes past its array (as
// Info_SetValueForKey can on the console) writes there, and only the array's
// own bytes go back.
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "cod3/q_shared.h"
#include "cod3_check/protocol.h"

namespace cod3::check {

namespace {

// The bytes after a buffer's copy, for writes and reads past its end.
constexpr std::size_t kSlack = 4096;

// An s: argument as a C string; "" for a NULL one (as the Rust answerer does).
std::string Text(const Args& args, std::size_t i) {
    const std::vector<std::uint8_t>* s = args.str(i);
    return s ? std::string(s->begin(), s->end()) : std::string();
}

// An s: argument as a C string, or nullptr for s:-.
struct OptionalText {
    std::string text;
    bool null = false;
    const char* c_str() const { return null ? nullptr : text.c_str(); }
};
OptionalText TextOrNull(const Args& args, std::size_t i) {
    const std::vector<std::uint8_t>* s = args.str(i);
    if (!s) {
        return {std::string(), true};
    }
    return {std::string(s->begin(), s->end()), false};
}

// A b: argument's copy the function works on, written back by Store().
class GuestBuffer {
public:
    GuestBuffer(Args& args, std::size_t i) : target_(args.buffer(i)) {
        if (!target_) {
            throw std::invalid_argument("a NULL buffer");
        }
        bytes_.assign(target_->size() + kSlack, '\0');
        std::memcpy(bytes_.data(), target_->data(), target_->size());
    }
    char* data() { return bytes_.data(); }
    // The string the function left in the array (up to its 0, or the whole array).
    std::string Text() const {
        const std::size_t n = target_->size();
        const char* end = static_cast<const char*>(std::memchr(bytes_.data(), 0, n));
        return std::string(bytes_.data(), end ? static_cast<std::size_t>(end - bytes_.data()) : n);
    }
    void Store() { std::memcpy(target_->data(), bytes_.data(), target_->size()); }

private:
    std::vector<std::uint8_t>* target_;
    std::vector<char> bytes_;
};

// The hashes come back as the unsigned 32-bit numbers they are.
Ret Hash(std::int64_t value) { return Ret::Int(static_cast<std::uint32_t>(value)); }

}  // namespace

std::optional<Answer> dispatch_shared(std::string_view name, Args& args) {
    if (name == "Q_stricmpn") {
        return Ok(Ret::Int(Q_stricmpn(Text(args, 0).c_str(), Text(args, 1).c_str(), args.i32(2))));
    }
    if (name == "Q_stricmp") {
        return Ok(Ret::Int(Q_stricmp(Text(args, 0).c_str(), Text(args, 1).c_str())));
    }
    if (name == "Q_strlwr") {
        GuestBuffer buffer(args, 0);
        Q_strlwr(buffer.data());
        buffer.Store();
        return Ok(Ret::Str(buffer.Text()));
    }
    if (name == "COM_GetExtension") {
        const std::string text = Text(args, 0);
        return Ok(Ret::Str(COM_GetExtension(text.c_str())));
    }
    if (name == "COM_DefaultExtension") {
        // The executable's copy was compiled for Cmd_Exec_f: 128 bytes, ".cfg".
        GuestBuffer buffer(args, 0);
        COM_DefaultExtension(buffer.data(), 128, ".cfg");
        buffer.Store();
        return Ok(Ret::Void());
    }
    if (name == "Q_strcat") {
        const int size = args.i32(1);
        const std::string src = Text(args, 2);
        GuestBuffer buffer(args, 0);
        Q_strcat(buffer.data(), size, src.c_str());
        buffer.Store();
        return Ok(Ret::Void());
    }
    if (name == "Info_ValueForKey") {
        const OptionalText info = TextOrNull(args, 0);
        const OptionalText key = TextOrNull(args, 1);
        return Ok(Ret::Str(Info_ValueForKey(info.c_str(), key.c_str())));
    }
    if (name == "Info_RemoveKey" || name == "Info_RemoveKey_Big") {
        const std::string key = Text(args, 1);
        GuestBuffer buffer(args, 0);
        if (name == "Info_RemoveKey") {
            Info_RemoveKey(buffer.data(), key.c_str());
        } else {
            Info_RemoveKey_Big(buffer.data(), key.c_str());
        }
        buffer.Store();
        return Ok(Ret::Void());
    }
    if (name == "Info_SetValueForKey" || name == "Info_SetValueForKey_Big") {
        const std::string key = Text(args, 1);
        const std::string value = Text(args, 2);
        GuestBuffer buffer(args, 0);
        if (name == "Info_SetValueForKey") {
            Info_SetValueForKey(buffer.data(), key.c_str(), value.c_str());
        } else {
            Info_SetValueForKey_Big(buffer.data(), key.c_str(), value.c_str());
        }
        buffer.Store();
        return Ok(Ret::Void());
    }
    if (name == "NameHash") {
        return Ok(Hash(NameHash(Text(args, 0).c_str())));
    }
    if (name == "NameHash_Init") {
        const std::string text = Text(args, 1);
        nameKey_t key;
        NameHash_Init(&key, text.c_str());
        std::vector<std::uint8_t>* out = args.buffer(0);
        if (!out || out->size() < 32) {
            throw std::invalid_argument("NameHash_Init needs the 32-byte key");
        }
        // As the game stores it: the hash big-endian, then the 28 characters.
        for (int k = 0; k < 4; ++k) {
            (*out)[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(key.hash >> (24 - 8 * k));
        }
        std::memcpy(out->data() + 4, key.text, sizeof(key.text));
        return Ok(Ret::Void());
    }
    if (name == "generateHashValue") {
        return Ok(Hash(generateHashValue(Text(args, 0).c_str())));
    }
    if (name == "FS_HashFileName") {
        return Ok(Hash(FS_HashFileName(Text(args, 0).c_str(), static_cast<int>(args.integer(1)))));
    }
    return std::nullopt;
}

}  // namespace cod3::check
