// [acct2-decomp] The file system's hash, as the original source would read
// (q_shared.h): from the recompiled C++ (FS_HashFileName) and checked against
// the original machine code.
#include "cod3/q_shared.h"
#include "cod3/xbox_crt.h"

namespace cod3 {

int FS_HashFileName(const char* fname, int hashSize) {
    int hash = 0;
    for (int i = 0; fname[i] != '\0'; ++i) {
        int letter = xbox_crt::tolower(static_cast<signed char>(fname[i]));
        if (letter == '.') {
            break;  // the extension doesn't count
        }
        if (letter == '\\') {
            letter = '/';  // both separators hash the same
        }
        hash += letter * (i + 119);
    }
    // The high bits folded down. (The compiler made it ((h >> 10) ^ h) >> 10 ^ h,
    // the same bits with one shift fewer.)
    hash = hash ^ (hash >> 10) ^ (hash >> 20);
    hash &= hashSize - 1;
    return hash;
}

}  // namespace cod3
