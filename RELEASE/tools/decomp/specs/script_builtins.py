"""Checks for cod3_game::script (the level scripts' built-ins) and math::crt."""
from __future__ import annotations

import random

import gen
from harness import (F32, F64, Floats, GlobalU32, Int, OutFloats, RetF32, RetF64, RetI32, RetVoid,
                     spec)

forever = iter(int, 1)
SCRIPT_RANDOM_SEED = 0x825F2E94  # the generator's seed, a global (script/random.rs)


def _trig_input(rng: random.Random) -> float:
    r = rng.random()
    if r < 0.1:
        return rng.choice(gen.SPECIAL_FLOATS + [2.2e8, -2.2e8, 2.19999999e8, 1e300, -1e-300, 3.141592653589793,
                                                 1.5707963267948966])
    if r < 0.6:
        return rng.uniform(-10.0, 10.0)
    if r < 0.85:
        return rng.uniform(-1e4, 1e4)
    return rng.uniform(-3e8, 3e8)


spec(name="sin", rust="math::crt::sin", args=[F64()], ret=RetF64(),
     cases=lambda rng: ([_trig_input(rng)] for _ in forever))
spec(name="cos", rust="math::crt::cos", args=[F64()], ret=RetF64(),
     cases=lambda rng: ([_trig_input(rng)] for _ in forever))
spec(name="Broc_Sin", rust="script::builtins::sin", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([gen.f32(_trig_input(rng))] for _ in forever))
spec(name="Broc_Cos", rust="script::builtins::cos", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([gen.f32(_trig_input(rng))] for _ in forever))
spec(name="Broc_Abs", rust="script::builtins::abs", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([gen.coordinate(rng)] for _ in forever))
spec(name="Broc_Sqrt", rust="script::builtins::sqrt", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([gen.coordinate(rng)] for _ in forever))
spec(name="Broc_Length", rust="script::builtins::length", args=[Floats(3)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng)] for _ in forever))
spec(name="Broc_LengthSquared", rust="script::builtins::length_squared", args=[Floats(3)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng)] for _ in forever))
spec(name="Broc_VectorDot", rust="script::builtins::dot", args=[Floats(3), Floats(3)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng), gen.vector(rng)] for _ in forever))
spec(name="Broc_Distance", rust="script::builtins::distance", args=[Floats(3), Floats(3)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng), gen.vector(rng)] for _ in forever))
spec(name="Broc_DistanceSquared", rust="script::builtins::distance_squared", args=[Floats(3), Floats(3)],
     ret=RetF32(), cases=lambda rng: ([gen.vector(rng), gen.vector(rng)] for _ in forever))
spec(name="Broc_Closer", rust="script::builtins::closer", args=[Floats(3), Floats(3), Floats(3)],
     ret=RetI32(), cases=lambda rng: ([gen.vector(rng), gen.vector(rng), gen.vector(rng)] for _ in forever))
spec(name="Broc_VectorNormalize", rust="script::builtins::normalize", args=[OutFloats(3), Floats(3)],
     ret=RetVoid(), cases=lambda rng: ([b"", gen.vector(rng)] for _ in forever))


def _seed(rng):
    return rng.choice([0x89ABCDEF, 0, 1, 0xFFFFFFFF, rng.getrandbits(32), rng.getrandbits(32)])


def _int_bound(rng):
    return rng.choice([0, 1, 2, 3, 10, 100, 32768, -5, -100, 2**31 - 1, -2**31, rng.randint(-100000, 100000)])


spec(name="Broc_RandomInt", rust="script::random::ScriptRandom::int",
     args=[Int(), GlobalU32(address=SCRIPT_RANDOM_SEED)], ret=RetI32(),
     cases=lambda rng: ([_int_bound(rng), _seed(rng)] for _ in forever))
spec(name="Broc_RandomIntRange", rust="script::random::ScriptRandom::int_range",
     args=[Int(), Int(), GlobalU32(address=SCRIPT_RANDOM_SEED)], ret=RetI32(),
     cases=lambda rng: ([_int_bound(rng), _int_bound(rng), _seed(rng)] for _ in forever))
spec(name="Broc_RandomFloat", rust="script::random::ScriptRandom::float",
     args=[F32(), GlobalU32(address=SCRIPT_RANDOM_SEED)], ret=RetF32(),
     cases=lambda rng: ([gen.coordinate(rng), _seed(rng)] for _ in forever))
spec(name="Broc_RandomFloatRange", rust="script::random::ScriptRandom::float_range",
     args=[F32(), F32(), GlobalU32(address=SCRIPT_RANDOM_SEED)], ret=RetF32(),
     cases=lambda rng: ([gen.coordinate(rng), gen.coordinate(rng), _seed(rng)] for _ in forever))
