// [acct2-decomp] The dvar system's table bucket, as the original source would
// read (q_shared.h): from the recompiled C++ (generateHashValue) and checked
// against the original machine code.
#include "cod3/q_shared.h"
#include "cod3/xbox_crt.h"

namespace cod3 {

namespace {
constexpr int kCvarHashSize = 256;  // the dvar table's buckets
}  // namespace

int generateHashValue(const char* fname) {
    if (!fname) {
        Com_Error(ERR_DROP, "null name in generateHashValue");
    }
    int hash = 0;
    for (int i = 0; fname[i] != '\0'; ++i) {
        const int letter = xbox_crt::tolower(static_cast<signed char>(fname[i]));
        hash += letter * (i + 119);
    }
    hash &= kCvarHashSize - 1;
    return hash;
}

}  // namespace cod3
