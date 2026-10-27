// [acct2-decomp] The names the level scripts and the engine trade instead of
// text (events, level functions, animations, tags), as the original source
// would read: NameHash_Init and NameHash (q_shared.h). The console's own file
// and function names aren't known; NameHash / NameHash_Init are the decomp's.
//
// From the recompiled C++ (sub_820A0118, sub_820CEBB8) and checked against the
// original machine code. (The Rust port, cod3_game::shared::hash, sums four
// characters a step for speed; the game does one, as here.)
#include <cstring>

#include "cod3/q_shared.h"
#include "cod3/xbox_crt.h"

namespace cod3 {

nameKey_t* NameHash_Init(nameKey_t* key, const char* name) {
    std::memset(key, 0, sizeof(*key));
    if (!name) {
        return key;
    }
    int length = 0;
    for (const char* c = name; *c; ++c) {
        // Letters lower-cased (the runtime's isalpha/tolower, "C" locale); the
        // rest as is. Added to the hash as a signed char.
        const int ch = static_cast<signed char>(*c);
        const char folded = xbox_crt::isalpha(ch) ? static_cast<char>(xbox_crt::tolower(ch)) : *c;
        key->hash = key->hash * 33 + static_cast<unsigned int>(static_cast<signed char>(folded));
        if (length < 27) {
            key->text[length++] = folded;  // only the first 27 are kept
        }
    }
    key->text[27] = '\0';
    return key;
}

unsigned int NameHash(const char* name) {
    if (!name) {
        return 0;
    }
    nameKey_t key;
    return NameHash_Init(&key, name)->hash;
}

}  // namespace cod3
