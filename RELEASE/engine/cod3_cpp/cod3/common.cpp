// [acct2-decomp] Com_Error (q_shared.h): the game's way out of a function it
// can't finish. The console's version prints the message and longjmps back to
// the frame loop (ERR_DROP) or stops the game (ERR_FATAL); here it throws.
#include <cstdarg>
#include <cstdio>

#include "cod3/q_shared.h"

namespace cod3 {

void Com_Error(errorParm_t code, const char* fmt, ...) {
    ComError error{};
    error.code = code;
    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(error.message, sizeof(error.message), fmt, args);
    va_end(args);
    throw error;
}

}  // namespace cod3
