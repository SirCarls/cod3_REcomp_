// Vector math the games share, at Call of Duty 2's x87 precision (see the Rust version,
// iw_pc::shared::math).
#pragma once

#include <array>
#include <cstdint>

namespace iw_pc::shared::math {

/// Scales v to length 1 and returns its length before (a zero vector stays).
/// Original: Vec3Normalize at 0x00426CF0 (CoD2MP_s.exe 1.3; v in esi, the length on the x87 stack)
float vec3_normalize(std::array<float, 3>& v);

/// The same for a 2D vector.
/// Original: Vec2Normalize at 0x00426D80 (CoD2MP_s.exe 1.3; our name; v in esi, the length on the x87 stack)
float vec2_normalize(std::array<float, 2>& v);

/// Vec3Normalize into out, v kept; length 0 gives zeros.
/// Original: Vec3NormalizeTo at 0x00426E90 (CoD2MP_s.exe 1.3; v in edi, out in esi, the length on the x87 stack)
float vec3_normalize_to(const std::array<float, 3>& v, std::array<float, 3>& out);

/// The yaw of a direction, 0 to 360 (0 for x = y = 0).
/// Original: vectoyaw at 0x004274B0 (CoD2MP_s.exe 1.3; Quake III's name; v in ecx, the yaw on the x87 stack)
float vectoyaw(const std::array<float, 3>& v);

/// A direction's angles (pitch, yaw, 0), 0 to 360.
/// Original: VecToAngles at 0x00427750 (CoD2MP_s.exe 1.3; v in esi, angles in edi)
void vec_to_angles(const std::array<float, 3>& v, std::array<float, 3>& angles);

/// An angle brought to (-180, 180] through 16 bits of a turn.
/// Original: AngleNormalize180 at 0x00429C60 (CoD2MP_s.exe 1.3; the angle on the stack, the result on the x87 stack)
float angle_normalize180(float angle);

/// The forward, right and up directions of angles (pitch, yaw, roll); each output may be null.
/// Original: AngleVectors at 0x004279C0 (CoD2MP_s.exe 1.3; Quake III's name; angles in edx, forward in esi, right in edi, up in ebx)
void angle_vectors(const std::array<float, 3>& angles, std::array<float, 3>* forward, std::array<float, 3>* right,
                   std::array<float, 3>* up);

/// (to - from) * fraction + from per component.
/// Original: Vec3Lerp at 0x004936E0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; from in eax, to in edx, out in ecx, fraction on the stack)
std::array<float, 3> vec3_lerp(const std::array<float, 3>& from, const std::array<float, 3>& to, float fraction);

/// Each component rounded to the nearest whole number (fistp: halves to even; NaN and out of range
/// -2147483648), as a float.
/// Original: SnapVector at 0x00467F80 (CoD2MP_s.exe 1.3; Quake III's name; v in eax)
void snap_vector(std::array<float, 3>& v);

/// The pitch of a direction (vec_to_angles' pitch).
/// Original: vectopitch at 0x004275B0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; v in esi, the pitch on the x87 stack)
float vectopitch(const std::array<float, 3>& v);

/// a1 - a2 brought to [-180, 180] by whole turns (see the Rust version for huge angles).
/// Original: AngleSubtract at 0x00429B70 (CoD2MP_s.exe 1.3; Quake III's name; (a1, a2) on the stack, the result on the x87 stack)
float angle_subtract(float a1, float a2);

/// An angle brought into [0, 360) by whole turns, one at a time (see the Rust version for huge angles).
/// Original: AngleWrap360 at 0x00429CC0 (CoD2MP_s.exe 1.3; our name; the angle on the stack, the result on the x87 stack)
float angle_wrap360(float angle);

/// An angle brought into (-180, 180] by whole turns, one at a time.
/// Original: AngleWrap180 at 0x00429D40 (CoD2MP_s.exe 1.3; our name; the angle on the stack, the result on the x87 stack)
float angle_wrap180(float angle);

/// angle_normalize180(a1 - a2).
/// Original: AngleDelta at 0x00429DC0 (CoD2MP_s.exe 1.3; Quake III's name; (a1, a2) on the stack, the result on the x87 stack)
float angle_delta(float a1, float a2);

/// Moves position sideways for a lean (see the Rust version).
/// Original: AddLeanToPosition at 0x0044B380 (CoD2MP_s.exe 1.3; Call of Duty 4's name; position in edx, then (yaw, leanFrac, roll, leanDist) on the stack)
void add_lean_to_position(std::array<float, 3>& position, float yaw, float lean, float roll, float dist);

/// The pitch of a yaw's level direction along a surface of normal `normal`.
/// Original: PitchForYawOnNormal at 0x0042BDC0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; normal in edx, yaw on the stack, the result on the x87 stack)
float pitch_for_yaw_on_normal(float yaw, const std::array<float, 3>& normal);

/// A float rounded to the nearest integer (ties to even): fistp(x + 2^-30) at the x87's precision.
/// Original: RoundFloatToInt at 0x00403710 (CoD2MP_s.exe 1.3; our name; x on the stack, the result in eax)
std::int32_t round_float_to_int(float x);

/// val kept in min to max, as the game's branch-free clamp does it (see the Rust version).
/// Original: I_fclamp at 0x004036D0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; (val, min, max) on the stack, the result on the x87 stack)
float i_fclamp(float val, float min, float max);

/// cur moved toward tgt by (tgt - cur) * rate * delta_time (see the Rust version).
/// Original: DiffTrack at 0x00426640 (CoD2MP_s.exe 1.3; Call of Duty 4's name; (tgt, cur, rate, deltaTime) on the stack, the result on the x87 stack)
float diff_track(float tgt, float cur, float rate, float delta_time);

}  // namespace iw_pc::shared::math
