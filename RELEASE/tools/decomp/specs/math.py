"""Checks for cod3_game::math (angles and vectors)."""
from __future__ import annotations

import gen
from harness import F32, FloatBuffer, Floats, OutFloats, RetF32, RetVoid, spec

forever = iter(int, 1)

spec(name="AngleNormalize180", rust="math::angles::angle_normalize_180", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([gen.angle(rng)] for _ in forever))
spec(name="AngleSubtract", rust="math::angles::angle_subtract", args=[F32(), F32()], ret=RetF32(),
     cases=lambda rng: ([gen.angle(rng), gen.angle(rng)] for _ in forever))
spec(name="AngleDelta", rust="math::angles::angle_delta", args=[F32(), F32()], ret=RetF32(),
     cases=lambda rng: ([gen.angle(rng), gen.angle(rng)] for _ in forever))
spec(name="AnglesSubtract", rust="math::angles::angles_subtract", args=[Floats(3), Floats(3), OutFloats(3)],
     ret=RetVoid(), cases=lambda rng: ([[gen.angle(rng) for _ in range(3)], [gen.angle(rng) for _ in range(3)],
                                       b""] for _ in forever))
spec(name="VectorNormalize", rust="math::vector::normalize", args=[FloatBuffer(count=3)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng)] for _ in forever))
spec(name="VectorNormalize2", rust="math::vector::normalized", args=[Floats(3), OutFloats(3)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng), b""] for _ in forever))
spec(name="VectorNormalizeFast", rust="math::vector::normalize_fast", args=[FloatBuffer(count=3)],
     ret=RetVoid(), cases=lambda rng: ([gen.vector(rng)] for _ in forever))
spec(name="Vec2Normalize", rust="math::vector::normalize_2d", args=[FloatBuffer(count=2)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng, 2)] for _ in forever))
spec(name="Vec2DistanceSq", rust="math::vector::distance_squared_2d", args=[Floats(2), Floats(2)],
     ret=RetF32(), cases=lambda rng: ([gen.vector(rng, 2), gen.vector(rng, 2)] for _ in forever))
spec(name="Vec2Distance", rust="math::vector::distance_2d", args=[Floats(2), Floats(2)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng, 2), gen.vector(rng, 2)] for _ in forever))
spec(name="Distance", rust="math::vector::distance", args=[Floats(3), Floats(3)], ret=RetF32(),
     cases=lambda rng: ([gen.vector(rng), gen.vector(rng)] for _ in forever))
