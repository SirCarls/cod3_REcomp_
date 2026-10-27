// [acct2-decomp] Requests for the level scripts' math built-ins: the same
// names and arguments as the Rust answerer's engine/tools/decomp_check/src/script.rs.
// The random built-ins get the seed as their last argument (the global, as
// 4 bytes) and give it back changed.
#include "cod3/scr_math.h"
#include "cod3_check/protocol.h"

namespace cod3::check {

namespace {

// Runs a random built-in on the seed passed as argument seed_arg.
template <typename Call>
Ret WithSeed(Args& args, std::size_t seed_arg, Call call) {
    brocRandomSeed = args.global_u32(seed_arg);
    Ret ret = call();
    args.set_global_u32(seed_arg, brocRandomSeed);
    return ret;
}

}  // namespace

std::optional<Answer> dispatch_script(std::string_view name, Args& args) {
    if (name == "Broc_Abs") return Ok(Ret::F32(Broc_Abs(args.f32(0))));
    if (name == "Broc_Sin") return Ok(Ret::F32(Broc_Sin(args.f32(0))));
    if (name == "Broc_Cos") return Ok(Ret::F32(Broc_Cos(args.f32(0))));
    if (name == "Broc_Sqrt") return Ok(Ret::F32(Broc_Sqrt(args.f32(0))));
    if (name == "Broc_Length") return Ok(Ret::F32(Broc_Length(args.floats<3>(0).data())));
    if (name == "Broc_LengthSquared") return Ok(Ret::F32(Broc_LengthSquared(args.floats<3>(0).data())));
    if (name == "Broc_VectorDot") {
        return Ok(Ret::F32(Broc_VectorDot(args.floats<3>(0).data(), args.floats<3>(1).data())));
    }
    if (name == "Broc_Distance") {
        return Ok(Ret::F32(Broc_Distance(args.floats<3>(0).data(), args.floats<3>(1).data())));
    }
    if (name == "Broc_DistanceSquared") {
        return Ok(Ret::F32(Broc_DistanceSquared(args.floats<3>(0).data(), args.floats<3>(1).data())));
    }
    if (name == "Broc_Closer") {
        return Ok(Ret::Int(Broc_Closer(args.floats<3>(0).data(), args.floats<3>(1).data(), args.floats<3>(2).data())));
    }
    if (name == "Broc_VectorNormalize") {
        const auto v = args.floats<3>(1);
        vec3_t out;
        Broc_VectorNormalize(out, v.data());
        args.set_floats(0, out, 3);
        return Ok(Ret::Void());
    }
    if (name == "Broc_RandomInt") {
        const int max = args.i32(0);
        return Ok(WithSeed(args, 1, [&] { return Ret::Int(Broc_RandomInt(max)); }));
    }
    if (name == "Broc_RandomIntRange") {
        const int min = args.i32(0);
        const int max = args.i32(1);
        return Ok(WithSeed(args, 2, [&] { return Ret::Int(Broc_RandomIntRange(min, max)); }));
    }
    if (name == "Broc_RandomFloat") {
        const float max = args.f32(0);
        return Ok(WithSeed(args, 1, [&] { return Ret::F32(Broc_RandomFloat(max)); }));
    }
    if (name == "Broc_RandomFloatRange") {
        const float min = args.f32(0);
        const float max = args.f32(1);
        return Ok(WithSeed(args, 2, [&] { return Ret::F32(Broc_RandomFloatRange(min, max)); }));
    }
    return std::nullopt;
}

}  // namespace cod3::check
