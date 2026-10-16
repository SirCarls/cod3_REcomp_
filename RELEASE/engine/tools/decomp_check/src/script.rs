//! Requests for `cod3_game::script` (the level scripts' built-ins).

use cod3_game::script::builtins;
use cod3_game::script::random::ScriptRandom;

use crate::{Answer, Args, Ret};

/// Runs a random-number built-in on the seed passed as the last (global) argument.
fn with_seed(args: &mut Args, seed_arg: usize, f: impl FnOnce(&mut ScriptRandom) -> Ret) -> Ret {
    let mut random = ScriptRandom {
        seed: args.global_u32(seed_arg),
    };
    let ret = f(&mut random);
    args.set_global_u32(seed_arg, random.seed);
    ret
}

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    Some(Ok(match name {
        "Broc_Abs" => Ret::F32(builtins::abs(args.f32(0))),
        "Broc_Sin" => Ret::F32(builtins::sin(args.f32(0))),
        "Broc_Cos" => Ret::F32(builtins::cos(args.f32(0))),
        "Broc_Sqrt" => Ret::F32(builtins::sqrt(args.f32(0))),
        "Broc_Length" => Ret::F32(builtins::length(&args.floats(0))),
        "Broc_LengthSquared" => Ret::F32(builtins::length_squared(&args.floats(0))),
        "Broc_VectorDot" => Ret::F32(builtins::dot(&args.floats(0), &args.floats(1))),
        "Broc_Distance" => Ret::F32(builtins::distance(&args.floats(0), &args.floats(1))),
        "Broc_DistanceSquared" => {
            Ret::F32(builtins::distance_squared(&args.floats(0), &args.floats(1)))
        }
        "Broc_Closer" => {
            Ret::Int(builtins::closer(&args.floats(0), &args.floats(1), &args.floats(2)).into())
        }
        "Broc_VectorNormalize" => {
            let v = builtins::normalize(&args.floats(1));
            args.set_floats(0, &v);
            Ret::Void
        }
        "Broc_RandomInt" => {
            let n = args.i32(0);
            with_seed(args, 1, |r| Ret::Int(r.int(n).into()))
        }
        "Broc_RandomIntRange" => {
            let (min, max) = (args.i32(0), args.i32(1));
            with_seed(args, 2, |r| Ret::Int(r.int_range(min, max).into()))
        }
        "Broc_RandomFloat" => {
            let max = args.f32(0);
            with_seed(args, 1, |r| Ret::F32(r.float(max)))
        }
        "Broc_RandomFloatRange" => {
            let (min, max) = (args.f32(0), args.f32(1));
            with_seed(args, 2, |r| Ret::F32(r.float_range(min, max)))
        }
        _ => return None,
    }))
}
