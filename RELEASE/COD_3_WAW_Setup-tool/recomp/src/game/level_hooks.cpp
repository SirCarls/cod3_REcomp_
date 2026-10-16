// cod3 - code linked into every level DLL module (libcod3_<level>.so / .dll).
//
// The level configs (config/<level>.toml) put a mid-asm hook after each call
// to the engine's suspend (interface table slots +76 / +80) with `return`. On
// the console that call never returns: the engine later reloads the thread's
// registers and branches straight back to the script. With script threads on
// host fibers the call does return when the thread is resumed, and the hook's
// `return` then hands control to the script right away, instead of running on
// into whatever code follows the call. See src/game/script_threads.cpp.
//
// [cod3mp] The multiplayer program has its game modes' script code in the
// executable itself; recomp/mp/config/codmp.toml puts the same hook after its
// two yield calls, and this file is compiled into cod3mp too.

void cod3_script_yield_returned() {}
