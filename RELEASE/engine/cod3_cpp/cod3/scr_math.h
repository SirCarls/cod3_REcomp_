// The level scripts' math built-ins: abs, sin, cos, sqrt, length, distance,
// vectordot, closer, vectornormalize and the random numbers. [acct2-decomp]
//
// The level DLLs reach them through the level interface table (the slot is
// given for each). "Broc_" is the prefix the decomp's symbol map uses for that
// table's functions; the console's own names aren't known.
#pragma once

#include <cstdint>

#include "cod3/q_shared.h"

namespace cod3 {

// The level scripts' random generator's seed (a global, 0x825F2E94; name ours).
// It starts as 0x89ABCDEF and is separate from the game's own rand.
extern std::uint32_t brocRandomSeed;
inline constexpr std::uint32_t kBrocRandomInitialSeed = 0x89ABCDEFu;

// abs. 0x820A0098, slot 0x174; recompiled: Broc_Abs
float Broc_Abs(float x);
// sin, through the runtime's double sin. 0x820A00C8, slot 0x154; recompiled: Broc_Sin
float Broc_Sin(float x);
// cos. 0x820A00A0, slot 0x158; recompiled: Broc_Cos
float Broc_Cos(float x);
// sqrt. 0x824995C0, slot 0x178; recompiled: Broc_Sqrt
float Broc_Sqrt(float x);
// lengthsquared. 0x820B2170, slot 0x190; recompiled: Broc_LengthSquared
float Broc_LengthSquared(const vec3_t v);
// length. 0x820B2150, slot 0x18C; recompiled: Broc_Length
float Broc_Length(const vec3_t v);
// vectordot. 0x820B2190, slot 0x198; recompiled: Broc_VectorDot
float Broc_VectorDot(const vec3_t a, const vec3_t b);
// distancesquared. 0x820B21F0, slot 0x188; recompiled: Broc_DistanceSquared
float Broc_DistanceSquared(const vec3_t p1, const vec3_t p2);
// distance. 0x820B21B8, slot 0x184; recompiled: Broc_Distance
float Broc_Distance(const vec3_t p1, const vec3_t p2);
// closer(reference, a, b): 1 when a is nearer to reference than b is.
// 0x820B2228, slot 0x194; recompiled: Broc_Closer
int Broc_Closer(const vec3_t reference, const vec3_t a, const vec3_t b);
// vectornormalize: out = v scaled to length 1 (a zero vector stays zero).
// 0x820B2290, slot 0x19C; recompiled: Broc_VectorNormalize
void Broc_VectorNormalize(vec3_t out, const vec3_t v);

// randomint(max): 0 to max - 1. 0x820B2508, slot 0x140; recompiled: Broc_RandomInt
int Broc_RandomInt(int max);
// randomfloat(max): 0 to just under max. 0x820B2538, slot 0x144; recompiled: Broc_RandomFloat
float Broc_RandomFloat(float max);
// randomintrange(min, max): min to max - 1; reversed bounds count from max.
// 0x820B2588, slot 0x148; recompiled: Broc_RandomIntRange
int Broc_RandomIntRange(int min, int max);
// randomfloatrange(min, max): from the smaller bound, as randomintrange.
// 0x820B25E0, slot 0x14C; recompiled: Broc_RandomFloatRange
float Broc_RandomFloatRange(float min, float max);

}  // namespace cod3
