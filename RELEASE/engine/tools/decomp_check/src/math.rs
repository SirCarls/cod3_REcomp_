//! Requests for `cod3_game::math` (angles and vectors).

use cod3_game::math::{angles, crt, vector};

use crate::{Answer, Args, Ret};

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    Some(Ok(match name {
        "sin" => Ret::F64(crt::sin(args.f64(0))),
        "cos" => Ret::F64(crt::cos(args.f64(0))),
        "AngleNormalize180" => Ret::F32(angles::angle_normalize_180(args.f32(0))),
        "AngleSubtract" => Ret::F32(angles::angle_subtract(args.f32(0), args.f32(1))),
        "AngleDelta" => Ret::F32(angles::angle_delta(args.f32(0), args.f32(1))),
        "AngleWrap180" => {
            let wrapped = angles::angle_wrap_180(args.floats::<1>(1)[0]);
            args.set_floats(0, &[wrapped]);
            Ret::Void
        }
        "AnglesSubtract" => {
            let result = angles::angles_subtract(&args.floats(0), &args.floats(1));
            args.set_floats(2, &result);
            Ret::Void
        }
        "VectorNormalize" => {
            let mut v: [f32; 3] = args.floats(0);
            let length = vector::normalize(&mut v);
            args.set_floats(0, &v);
            Ret::F32(length)
        }
        "VectorNormalize2" => {
            let (v, length) = vector::normalized(&args.floats(0));
            args.set_floats(1, &v);
            Ret::F32(length)
        }
        "VectorNormalizeFast" => {
            let mut v: [f32; 3] = args.floats(0);
            vector::normalize_fast(&mut v);
            args.set_floats(0, &v);
            Ret::Void
        }
        "Vec2Normalize" => {
            let mut v: [f32; 2] = args.floats(0);
            let length = vector::normalize_2d(&mut v);
            args.set_floats(0, &v);
            Ret::F32(length)
        }
        "Vec2DistanceSq" => Ret::F32(vector::distance_squared_2d(
            &args.floats(0),
            &args.floats(1),
        )),
        "Vec2Distance" => Ret::F32(vector::distance_2d(&args.floats(0), &args.floats(1))),
        "Distance" => Ret::F32(vector::distance(&args.floats(0), &args.floats(1))),
        _ => return None,
    }))
}
