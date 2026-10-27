"""[acct2-decomp] Checks for the renderer's 2D calls, its far plane and Treyarch's NGL quads.

The C++ version has these (engine/cod3_cpp/cod3/tr_cmds.cpp, ngl_quad.cpp); the
Rust crate doesn't yet. So they are registered only when COD3_DECOMP_ANSWERER
names another answerer (the C++ one), and a plain `decomp.py check` of the
Rust ports stays as it was:

    COD3_DECOMP_ANSWERER=<build>/cod3_check_cpp python3 tools/decomp/decomp.py check --no-build --area gfx

The NGL helpers and three of the renderer's calls have no names in the symbol
map yet, so they go by address (sub_XXXXXXXX); the C++ calls them nglInitQuad,
nglSetQuadRect, nglSetQuadUV, nglSetQuadColor, RE_DrawQuadPic (sub_824178C8),
RE_GetZFar (sub_82417830), RE_SetZFarMin (sub_82417808).

The calls that draw (RE_StretchPic, RE_DrawQuadPic) are checked by what they
hand nglListAddQuad: the oracle runs a stand-in for it that counts the quads
and keeps the last one in a 108-byte "sink" argument, and the C++ answerer's
nglListAddQuad fills the same sink (nglQuadSink). nglCurScene (0x8290E9A4) is
an argument too: zero now and then, so the "no scene, no drawing" path is
checked.
"""
from __future__ import annotations

import os
import random
import struct
from dataclasses import dataclass

import gen
import oracle as guest
from harness import Arg, Buffer, F32, Floats, GlobalU32, Int, RetF32, RetVoid, spec

forever = iter(int, 1)

COLOR_2D = 0x82A11C0C  # the 2D color RE_SetColor writes (a D3DCOLOR, ARGB)
QUAD_SIZE = 104        # an NGL quad (cod3/tr_public.h)
CUR_SCENE = 0x8290E9A4         # nglCurScene: the 2D calls draw only while it is set
NGL_LIST_ADD_QUAD = 0x8212AC58
R_ZFAR = 0x82A11B2C            # the cvar_t* of "r_zfar"
ZFAR_MIN = 0x82AA56E8          # tr_zFarMin (float)
SINK_SIZE = 4 + QUAD_SIZE


@dataclass
class FloatsOrNull(Arg):
    """`count` floats the function only reads, or a NULL pointer (None)."""
    count: int = 4

    def place(self, oracle, value):
        return 0 if value is None else oracle.put(struct.pack(f">{self.count}f", *value), 16)

    def token(self, value):
        return "s:-" if value is None else "v:" + struct.pack(f">{self.count}f", *value).hex()


@dataclass
class StackInt(Int):
    """An integer argument that comes after eight floats, so on the stack:
    the caller's frame + `offset` (the 9th slot, +84). The oracle's calls start
    with r1 at STACK + STACK_SIZE - 0x400."""
    offset: int = 84
    reg = "none"

    def place(self, oracle, value):
        oracle.write(guest.STACK + guest.STACK_SIZE - 0x400 + self.offset, struct.pack(">I", value & 0xFFFFFFFF))
        return value


@dataclass
class CvarValue(Arg):
    """A cvar's value, the function reaching it through the cvar_t* at
    `address` (value at +28, as in Quake III's cvar_t)."""
    address: int = 0
    reg = "none"

    def place(self, oracle, value):
        cvar = oracle.put(bytes(28) + struct.pack(">f", value) + bytes(4), 16)
        oracle.write(self.address, struct.pack(">I", cvar))
        return cvar

    def token(self, value):
        return f"f:{struct.unpack('>I', struct.pack('>f', value))[0]:08x}"


@dataclass
class QuadSink(Arg):
    """Where the call's quads land: a count, then the last quad (see the top)."""
    reg = "none"
    mutable = True

    def place(self, oracle, value):
        return oracle.put(bytes(SINK_SIZE), 16)

    def token(self, value):
        return "b:" + bytes(SINK_SIZE).hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(pointer, SINK_SIZE)


def _catch_quads(oracle, values, pointers):
    sink = pointers[-1]  # the QuadSink is the last argument

    def stand_in(o, quad, *_):
        if quad:
            o.write(sink, struct.pack(">I", o.read_u32(sink) + 1) + o.read(quad, QUAD_SIZE))
        return 0
    oracle.replace(NGL_LIST_ADD_QUAD, stand_in)


def _scene(rng: random.Random) -> int:
    return 0 if rng.random() < 0.05 else rng.getrandbits(32) | 1


def _distance(rng: random.Random) -> float:
    r = rng.random()
    if r < 0.6:
        return rng.uniform(0.0, 20000.0)
    if r < 0.8:
        return rng.choice([0.0, -0.0, 8192.0, 1.0, -1.0, 4096.0])
    if r < 0.9:
        return rng.uniform(-20000.0, 0.0)
    return rng.choice(gen.SPECIAL_FLOATS)


def _component(rng: random.Random) -> float:
    """A color component: mostly 0..1, sometimes out of range or special."""
    r = rng.random()
    if r < 0.6:
        return rng.random()
    if r < 0.7:
        return rng.choice([0.0, 1.0, 0.5, -0.0, 1.0 / 255.0, 254.5 / 255.0])
    if r < 0.85:
        return rng.uniform(-2.0, 3.0)
    return rng.choice(gen.SPECIAL_FLOATS + [1e9, -1e9, 255.0, 8.5, -1.0])


def _screen(rng: random.Random) -> float:
    r = rng.random()
    if r < 0.7:
        return rng.uniform(-100.0, 1380.0)
    if r < 0.85:
        return rng.choice([0.0, 1.0, 640.0, 480.0, 1280.0, 720.0, -0.0])
    return rng.choice(gen.SPECIAL_FLOATS + [1e30, -1e30])


def _quad(rng: random.Random) -> bytes:
    """A quad's starting bytes: random, so any byte a function writes or leaves shows up."""
    return bytes(rng.getrandbits(8) for _ in range(QUAD_SIZE))


if os.environ.get("COD3_DECOMP_ANSWERER"):
    _QUAD = Buffer(size=QUAD_SIZE, terminated=False)

    spec(name="RE_SetColor", rust="gfx::tr_cmds::RE_SetColor",
         args=[FloatsOrNull(4), GlobalU32(address=COLOR_2D)], ret=RetVoid(), golden=False,
         cases=lambda rng: ([None if rng.random() < 0.05 else [gen.f32(_component(rng)) for _ in range(4)],
                             rng.getrandbits(32)] for _ in forever))
    spec(name="sub_8212A7B0", rust="gfx::ngl::nglInitQuad", args=[_QUAD], ret=RetVoid(), golden=False,
         cases=lambda rng: ([_quad(rng)] for _ in forever))
    spec(name="sub_8212A898", rust="gfx::ngl::nglSetQuadRect", args=[_QUAD, F32(), F32(), F32(), F32()],
         ret=RetVoid(), golden=False,
         cases=lambda rng: ([_quad(rng)] + [gen.f32(_screen(rng)) for _ in range(4)] for _ in forever))
    spec(name="sub_8212A858", rust="gfx::ngl::nglSetQuadUV", args=[_QUAD, F32(), F32(), F32(), F32()],
         ret=RetVoid(), golden=False,
         cases=lambda rng: ([_quad(rng)] + [gen.f32(_component(rng)) for _ in range(4)] for _ in forever))
    spec(name="sub_8212A880", rust="gfx::ngl::nglSetQuadColor", args=[_QUAD, Int()], ret=RetVoid(), golden=False,
         cases=lambda rng: ([_quad(rng), rng.getrandbits(32)] for _ in forever))

    _DRAWS = [GlobalU32(address=CUR_SCENE), GlobalU32(address=COLOR_2D), QuadSink()]
    spec(name="RE_StretchPic", rust="gfx::tr_cmds::RE_StretchPic",
         args=[F32() for _ in range(8)] + [StackInt(offset=84)] + _DRAWS, ret=RetVoid(), golden=False,
         setup=_catch_quads,
         cases=lambda rng: ([gen.f32(_screen(rng)) for _ in range(4)] + [gen.f32(_component(rng)) for _ in range(4)]
                            + [rng.getrandbits(32), _scene(rng), rng.getrandbits(32), None] for _ in forever))
    spec(name="sub_824178C8", rust="gfx::tr_cmds::RE_DrawQuadPic",
         args=[Floats(count=8), Floats(count=8), Int()] + _DRAWS, ret=RetVoid(), golden=False,
         setup=_catch_quads,
         cases=lambda rng: ([[gen.f32(_screen(rng)) for _ in range(8)], [gen.f32(_component(rng)) for _ in range(8)],
                             rng.getrandbits(32), _scene(rng), rng.getrandbits(32), None] for _ in forever))
    spec(name="sub_82417830", rust="gfx::tr_cmds::RE_GetZFar",
         args=[CvarValue(address=R_ZFAR), GlobalU32(address=ZFAR_MIN)], ret=RetF32(), golden=False,
         cases=lambda rng: ([gen.f32(_distance(rng)), struct.unpack(">I", struct.pack(">f", gen.f32(_distance(rng))))[0]]
                            for _ in forever))
    spec(name="sub_82417808", rust="gfx::tr_cmds::RE_SetZFarMin",
         args=[F32(), GlobalU32(address=ZFAR_MIN)], ret=RetVoid(), golden=False,
         cases=lambda rng: ([gen.f32(_distance(rng)), rng.getrandbits(32)] for _ in forever))
