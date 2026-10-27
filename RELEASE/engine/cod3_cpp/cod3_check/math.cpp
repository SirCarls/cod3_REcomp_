// [acct2-decomp] Requests for q_math, the multiplayer program's math and the C
// runtime's sin/cos: the same names and arguments as the Rust answerer's
// engine/tools/decomp_check/src/{math,fields}.rs.
#include "cod3/q_shared.h"
#include "cod3/xbox_crt.h"
#include "cod3_check/protocol.h"

namespace cod3::check {

std::optional<Answer> dispatch_math(std::string_view name, Args& args) {
    if (name == "sin") return Ok(Ret::F64(xbox_crt::sin(args.f64(0))));
    if (name == "cos") return Ok(Ret::F64(xbox_crt::cos(args.f64(0))));
    if (name == "AngleNormalize180") return Ok(Ret::F32(AngleNormalize180(args.f32(0))));
    if (name == "AngleSubtract") return Ok(Ret::F32(AngleSubtract(args.f32(0), args.f32(1))));
    if (name == "AngleDelta") return Ok(Ret::F32(AngleDelta(args.f32(0), args.f32(1))));
    if (name == "AngleWrap180") {
        const auto angle = args.floats<1>(1);
        float out = 0.0f;
        AngleWrap180(&out, angle.data());
        args.set_floats(0, &out, 1);
        return Ok(Ret::Void());
    }
    if (name == "AnglesSubtract") {
        const auto v1 = args.floats<3>(0);
        const auto v2 = args.floats<3>(1);
        vec3_t v3;
        AnglesSubtract(v1.data(), v2.data(), v3);
        args.set_floats(2, v3, 3);
        return Ok(Ret::Void());
    }
    if (name == "VectorNormalize") {
        auto v = args.floats<3>(0);
        const float length = VectorNormalize(v.data());
        args.set_floats(0, v.data(), 3);
        return Ok(Ret::F32(length));
    }
    if (name == "VectorNormalize2") {
        const auto v = args.floats<3>(0);
        vec3_t out;
        const float length = VectorNormalize2(v.data(), out);
        args.set_floats(1, out, 3);
        return Ok(Ret::F32(length));
    }
    if (name == "VectorNormalizeFast") {
        auto v = args.floats<3>(0);
        VectorNormalizeFast(v.data());
        args.set_floats(0, v.data(), 3);
        return Ok(Ret::Void());
    }
    if (name == "Vec2Normalize") {
        auto v = args.floats<2>(0);
        const float length = Vec2Normalize(v.data());
        args.set_floats(0, v.data(), 2);
        return Ok(Ret::F32(length));
    }
    if (name == "Vec2DistanceSq") return Ok(Ret::F32(Vec2DistanceSq(args.floats<2>(0).data(), args.floats<2>(1).data())));
    if (name == "Vec2Distance") return Ok(Ret::F32(Vec2Distance(args.floats<2>(0).data(), args.floats<2>(1).data())));
    if (name == "Distance") return Ok(Ret::F32(Distance(args.floats<3>(0).data(), args.floats<3>(1).data())));
    if (name == "vectoyaw") return Ok(Ret::F32(vectoyaw(args.floats<3>(0).data())));
    if (name == "vectoyaw_signed") return Ok(Ret::F32(vectoyaw_signed(args.floats<3>(0).data())));
    return std::nullopt;
}

}  // namespace cod3::check
