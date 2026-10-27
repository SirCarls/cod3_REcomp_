// Call of Duty 3's shared math: types and declarations, as the game's own
// source would have them. [acct2-decomp]
//
// The game is Quake III's descendant (through Call of Duty and Call of Duty 2),
// so its names, types and file layout follow Quake III's (q_shared.h,
// q_math.c): vec_t, vec3_t, AngleNormalize180 ... The console's own file names
// aren't known. Each function's comment gives its address in default.xex (or
// in codmp_xenonf.xex, the multiplayer program) and its name in the
// recompiled C++ (recomp/generated, `DEFINE_REX_FUNC(...)`), so the two can be
// read side by side. The Rust crate engine/crates/cod3_game is the full
// version; these give the same bits, checked against the original.
#pragma once

#include "cod3/ppc.h"

namespace cod3 {

typedef float vec_t;
typedef vec_t vec2_t[2];
typedef vec_t vec3_t[3];

// ---- q_math: angles ------------------------------------------------------

// Brings an angle into (-180, 180], rounded toward zero to one of the 65536
// steps a 16-bit angle has (190 becomes -170.00244, not -170).
// 0x82526BF8; recompiled: AngleNormalize180
float AngleNormalize180(float angle);

// a1 - a2 brought into [-180, 180] (not rounded to steps).
// 0x82526C58; recompiled: AngleSubtract
float AngleSubtract(float a1, float a2);

// AngleNormalize180(angle1 - angle2).
// 0x8252A008; recompiled: AngleDelta
float AngleDelta(float angle1, float angle2);

// AngleSubtract on each of the three angles: v3 = v1 - v2.
// 0x8252A068; recompiled: AnglesSubtract
void AnglesSubtract(const vec3_t v1, const vec3_t v2, vec3_t v3);

// ---- q_math: vectors -----------------------------------------------------

// Scales v to length 1 and returns its old length (a zero vector stays as it is).
// 0x82526F38; recompiled: VectorNormalize
vec_t VectorNormalize(vec3_t v);

// out = v scaled to length 1 (or zero), and returns v's length.
// 0x82526E08; recompiled: VectorNormalize2
vec_t VectorNormalize2(const vec3_t v, vec3_t out);

// Scales v to about length 1 with the fast inverse square root (off by up to
// about 0.2%). A zero vector becomes NaNs or infinities, as in the original.
// 0x82526E78; recompiled: sub_82526E78
void VectorNormalizeFast(vec3_t v);

// Scales v (x and y) to length 1 and returns its old length.
// 0x82526EF0; recompiled: sub_82526EF0
vec_t Vec2Normalize(vec2_t v);

// The squared distance between two 2D points.
// 0x82526F90; recompiled: sub_82526F90
vec_t Vec2DistanceSq(const vec2_t p1, const vec2_t p2);

// The distance between two 2D points.
// 0x82526FB8; recompiled: sub_82526FB8
vec_t Vec2Distance(const vec2_t p1, const vec2_t p2);

// The distance between two points.
// 0x82526FE0; recompiled: sub_82526FE0
vec_t Distance(const vec3_t p1, const vec3_t p2);

// ---- the multiplayer program's own (codmp_xenonf.xex) ---------------------

// *out = *angle brought into [-180, 180] when it is above 180 (inside, it is
// left as it is). Below -180 it stays below: the fmod keeps the sign (-190
// stays -190, -600 becomes -240), as in the original.
// mp 0x820DA148; recompiled (mp): sub_820DA148
void AngleWrap180(float* out, const float* angle);

// The direction vec points in the horizontal plane, in degrees, [0, 360) (0
// for a vertical or zero vector). Not Quake III's atan2: an arcsine series in
// single precision.
// mp 0x82550680; recompiled (mp): sub_82550680
float vectoyaw(const vec3_t vec);

// The same in [-180, 180].
// mp 0x82550478; recompiled (mp): sub_82550478
float vectoyaw_signed(const vec3_t vec);

// ---- errors (common.cpp) -----------------------------------------------------

// Com_Error's first argument: the two levels CoD3's shared code uses.
enum errorParm_t {
    ERR_FATAL = 0,  // stops the game
    ERR_DROP = 1,   // ends the level, back to the menu
};

// The game's Com_Error never returns: it jumps back to the frame loop. Here
// it throws this, and whoever stands where the frame loop would (the
// checker's answerer) catches it.
struct ComError {
    errorParm_t code;
    char message[1024];
};

// 0x825239A0; recompiled: Com_Error
[[noreturn]] void Com_Error(errorParm_t code, const char* fmt, ...);

// ---- q_shared: strings (q_shared.cpp) ---------------------------------------
//
// The game's characters are signed (lbz + extsb): bytes 0x80 and up compare
// below '\0'. Every function here reads them that way.

// Compares at most n characters ignoring case (A-Z folded to lower case):
// 0 when equal, else the difference of the first differing characters. A
// negative n never runs out.
// 0x820CDE10; recompiled: Q_stricmpn
int Q_stricmpn(const char* s1, const char* s2, int n);

// CoD3's own: folds to UPPER case and returns -1 for any difference, so it
// only tells equal from different (Quake III's sorts). At most 99999 characters.
// 0x82453580; recompiled: Q_stricmp
int Q_stricmp(const char* s1, const char* s2);

// Lower-cases s1 in place (A-Z only) and returns it.
// 0x824515D8; recompiled: Q_strlwr
char* Q_strlwr(char* s1);

// Appends src to the string in dest, a buffer of size bytes: the rest of the
// buffer is filled with zeros and its last byte is always 0. A string already
// size long is fatal ("Q_strcat: already overflowed").
// 0x824535F8; recompiled: Q_strcat
void Q_strcat(char* dest, int size, const char* src);

// The text after the last '.' of the last path component ('/' or '\'), or "".
// 0x82524E50; recompiled: sub_82524E50
const char* COM_GetExtension(const char* name);

// Adds extension to path if path has none (a '.' after the last '/', the
// first character not counting). The copy in the executable was compiled for
// its one caller, Cmd_Exec_f: maxSize 128 and ".cfg" are built into it.
// 0x82451900; recompiled: COM_DefaultExtension
void COM_DefaultExtension(char* path, int maxSize, const char* extension);

// Formats into dest, at most size bytes, always ending with a 0 in dest[size - 1].
// 0x82451568; recompiled: Com_sprintf
void Com_sprintf(char* dest, int size, const char* fmt, ...);

// ---- q_shared: info strings, "\key\value\key\value" (q_shared.cpp) ----------

// CoD3's limit for the plain and the _Big functions alike (Quake III's _Big: 8192).
constexpr int MAX_INFO_STRING = 1024;
constexpr int MAX_INFO_KEY = 1024;
constexpr int MAX_INFO_VALUE = 1024;
// Info_ValueForKey's key buffer is bigger: 8192 bytes on its stack.
constexpr int BIG_INFO_KEY = 8192;

// The value of key (case ignored), or "" when it's missing (or s or key is
// NULL). The value is copied into one of two static buffers used in turn.
// 0x82457570; recompiled: Info_ValueForKey
const char* Info_ValueForKey(const char* s, const char* key);

// Removes key (exact case) from s. A key containing '\' is ignored.
// 0x82453338; recompiled: Info_RemoveKey
void Info_RemoveKey(char* s, const char* key);

// The same code (also limited to 1024 bytes in CoD3); only the message differs.
// 0x824531B8; recompiled: Info_RemoveKey_Big
void Info_RemoveKey_Big(char* s, const char* key);

// Sets key to value: removes the old pair and appends "\key\value" (an empty
// value only removes it). The value loses '\', ';' and '"'; a key with one of
// them is an error. CoD3 checks no length here at all.
// 0x82457408; recompiled: Info_SetValueForKey
void Info_SetValueForKey(char* s, const char* key, const char* value);

// The same with length checks: s under 1024 before, at most 1024 after.
// 0x824571F8; recompiled: Info_SetValueForKey_Big
void Info_SetValueForKey_Big(char* s, const char* key, const char* value);

// ---- name hashes (name_hash.cpp) ---------------------------------------------

// A name as the game keeps it for its scripts (events, level functions,
// animations, tags): the hash and up to 27 characters, lower-cased. 32 bytes.
// (Names ours; the layout the game's.)
struct nameKey_t {
    unsigned int hash;  // h = h * 33 + c from 0, over the lower-cased characters as signed chars
    char text[28];
};

// Fills key from name and returns it (a NULL name leaves it all zero).
// 0x820A0118; recompiled: sub_820A0118
nameKey_t* NameHash_Init(nameKey_t* key, const char* name);

// name's hash (0 for NULL): NameHash_Init into a key on the stack.
// 0x820CEBB8; recompiled: sub_820CEBB8
unsigned int NameHash(const char* name);

// ---- cvar.cpp, files.cpp -------------------------------------------------------

// The dvar table's bucket: each lower-cased character times (its index + 119),
// summed, cut to 8 bits. A NULL name is an error (ERR_DROP).
// 0x82526148; recompiled: generateHashValue
int generateHashValue(const char* fname);

// The file system's bucket: the same sum up to the first '.', with '\'
// counted as '/', its high bits folded down, masked to hashSize (a power of 2).
// 0x82525038; recompiled: FS_HashFileName
int FS_HashFileName(const char* fname, int hashSize);

}  // namespace cod3
