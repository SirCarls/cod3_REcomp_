// [acct2-decomp] Requests for the renderer's 2D calls, the far plane and NGL's
// quads (tools/decomp/specs/gfx_ngl.py). A quad travels as its 104 bytes, as
// the console keeps it (big-endian); globals as the 4 bytes at their address.
// A call that draws gets a 108-byte "sink": how many quads reached
// nglListAddQuad, then the last one (the checker catches the console's call
// the same way).
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "cod3/tr_public.h"
#include "cod3_check/protocol.h"

namespace cod3::check {

namespace {

std::uint32_t LoadU32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}
void StoreU32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
float LoadF32(const std::uint8_t* p) {
    const std::uint32_t bits = LoadU32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void StoreF32(std::uint8_t* p, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    StoreU32(p, bits);
}

float GlobalF32(Args& args, std::size_t i) {
    const std::uint32_t bits = args.global_u32(i);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

nglQuad LoadQuadAt(const std::uint8_t* p) {
    nglQuad q;
    for (int k = 0; k < 4; ++k) {
        const std::uint8_t* v = p + 20 * k;
        q.verts[k].x = LoadF32(v);
        q.verts[k].y = LoadF32(v + 4);
        q.verts[k].u = LoadF32(v + 8);
        q.verts[k].v = LoadF32(v + 12);
        q.verts[k].color = LoadU32(v + 16);
    }
    q.z = LoadF32(p + 80);
    q.unknown84 = static_cast<std::int32_t>(LoadU32(p + 84));
    q.flags = (std::uint64_t{LoadU32(p + 88)} << 32) | LoadU32(p + 92);
    q.material = static_cast<qhandle_t>(LoadU32(p + 96));
    q.pad100 = LoadU32(p + 100);
    return q;
}

nglQuad LoadQuad(const std::vector<std::uint8_t>& b) {
    if (b.size() < 104) {
        throw std::invalid_argument("a quad is 104 bytes");
    }
    return LoadQuadAt(b.data());
}

void StoreQuadAt(std::uint8_t* p, const nglQuad& q) {
    for (int k = 0; k < 4; ++k) {
        std::uint8_t* v = p + 20 * k;
        StoreF32(v, q.verts[k].x);
        StoreF32(v + 4, q.verts[k].y);
        StoreF32(v + 8, q.verts[k].u);
        StoreF32(v + 12, q.verts[k].v);
        StoreU32(v + 16, q.verts[k].color);
    }
    StoreF32(p + 80, q.z);
    StoreU32(p + 84, static_cast<std::uint32_t>(q.unknown84));
    StoreU32(p + 88, static_cast<std::uint32_t>(q.flags >> 32));
    StoreU32(p + 92, static_cast<std::uint32_t>(q.flags));
    StoreU32(p + 96, static_cast<std::uint32_t>(q.material));
    StoreU32(p + 100, q.pad100);
}

void StoreQuad(std::vector<std::uint8_t>& b, const nglQuad& q) {
    StoreQuadAt(b.data(), q);
}

// The sink of the call being answered: a count, then the last quad.
std::vector<std::uint8_t>* g_sink = nullptr;

void SinkQuad(const nglQuad* quad) {
    StoreU32(g_sink->data(), LoadU32(g_sink->data()) + 1);
    StoreQuadAt(g_sink->data() + 4, *quad);
}

// Runs draw() with the scene (argument scene_arg: 0 = none), the 2D color
// (color_arg) and the sink (sink_arg) in place.
template <typename Draw>
Answer Drawing(Args& args, std::size_t scene_arg, std::size_t color_arg, std::size_t sink_arg, Draw draw) {
    static const int a_scene = 0;
    std::vector<std::uint8_t>* sink = args.buffer(sink_arg);
    if (!sink || sink->size() < 108) {
        throw std::invalid_argument("the sink is 108 bytes");
    }
    nglCurScene = args.global_u32(scene_arg) ? &a_scene : nullptr;
    tr_color2D = args.global_u32(color_arg);
    g_sink = sink;
    nglQuadSink = SinkQuad;
    draw();
    nglQuadSink = nullptr;
    g_sink = nullptr;
    nglCurScene = nullptr;
    return Ok(Ret::Void());
}

// Runs change(quad) on argument 0, a quad buffer.
template <typename Change>
Answer OnQuad(Args& args, Change change) {
    std::vector<std::uint8_t>* b = args.buffer(0);
    if (!b) {
        throw std::invalid_argument("a NULL quad");
    }
    nglQuad q = LoadQuad(*b);
    change(q);
    StoreQuad(*b, q);
    return Ok(Ret::Void());
}

}  // namespace

std::optional<Answer> dispatch_gfx(std::string_view name, Args& args) {
    if (name == "sub_8212A7B0") {  // nglInitQuad
        return OnQuad(args, [](nglQuad& q) { nglInitQuad(&q); });
    }
    if (name == "sub_8212A898") {  // nglSetQuadRect
        const float x0 = args.f32(1), y0 = args.f32(2), x1 = args.f32(3), y1 = args.f32(4);
        return OnQuad(args, [&](nglQuad& q) { nglSetQuadRect(&q, x0, y0, x1, y1); });
    }
    if (name == "sub_8212A858") {  // nglSetQuadUV
        const float s0 = args.f32(1), t0 = args.f32(2), s1 = args.f32(3), t1 = args.f32(4);
        return OnQuad(args, [&](nglQuad& q) { nglSetQuadUV(&q, s0, t0, s1, t1); });
    }
    if (name == "sub_8212A880") {  // nglSetQuadColor
        const std::uint32_t color = static_cast<std::uint32_t>(args.integer(1));
        return OnQuad(args, [&](nglQuad& q) { nglSetQuadColor(&q, color); });
    }
    if (name == "RE_StretchPic") {
        // f32 0-7, hShader 8 (on the stack on the console), scene 9, color 10, sink 11.
        const float x = args.f32(0), y = args.f32(1), w = args.f32(2), h = args.f32(3);
        const float s1 = args.f32(4), t1 = args.f32(5), s2 = args.f32(6), t2 = args.f32(7);
        const qhandle_t shader = args.i32(8);
        return Drawing(args, 9, 10, 11, [&] { RE_StretchPic(x, y, w, h, s1, t1, s2, t2, shader); });
    }
    if (name == "sub_824178C8") {  // RE_DrawQuadPic: verts 0, st 1, material 2, scene 3, color 4, sink 5
        const std::array<float, 8> v = args.floats<8>(0), s = args.floats<8>(1);
        float verts[4][2], st[4][2];
        for (int k = 0; k < 4; ++k) {
            verts[k][0] = v[2 * k];
            verts[k][1] = v[2 * k + 1];
            st[k][0] = s[2 * k];
            st[k][1] = s[2 * k + 1];
        }
        const qhandle_t material = args.i32(2);
        return Drawing(args, 3, 4, 5, [&] { RE_DrawQuadPic(verts, st, material); });
    }
    if (name == "sub_82417830") {  // RE_GetZFar: r_zfar->value 0, tr_zFarMin 1
        cvar_t zfar{};
        zfar.value = args.f32(0);
        cvar_t* const saved = r_zfar;
        r_zfar = &zfar;
        tr_zFarMin = GlobalF32(args, 1);
        const float result = RE_GetZFar();
        r_zfar = saved;
        return Ok(Ret::F32(result));
    }
    if (name == "sub_82417808") {  // RE_SetZFarMin: zfar 0, tr_zFarMin 1
        const float zfar = args.f32(0);
        tr_zFarMin = GlobalF32(args, 1);
        RE_SetZFarMin(zfar);
        std::uint32_t bits;
        std::memcpy(&bits, &tr_zFarMin, 4);
        args.set_global_u32(1, bits);
        return Ok(Ret::Void());
    }
    if (name == "RE_SetColor") {
        // Argument 0: the 4 floats (v:) or NULL (s:-); argument 1: the 2D color global.
        const std::vector<std::uint8_t>* null_or = args.all().at(0).kind == ArgKind::Str ? nullptr : &args.bytes(0);
        float rgba[4];
        if (null_or) {
            if (null_or->size() < 16) {
                throw std::invalid_argument("rgba is 4 floats");
            }
            for (int k = 0; k < 4; ++k) {
                rgba[k] = LoadF32(null_or->data() + 4 * k);
            }
        }
        tr_color2D = args.global_u32(1);
        RE_SetColor(null_or ? rgba : nullptr);
        args.set_global_u32(1, tr_color2D);
        return Ok(Ret::Void());
    }
    return std::nullopt;
}

}  // namespace cod3::check
