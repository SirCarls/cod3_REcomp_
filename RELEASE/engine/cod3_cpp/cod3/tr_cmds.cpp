// Call of Duty 3's 2D renderer calls and the far plane (tr_public.h), as the
// original source would read: from the recompiled C++ (RE_SetColor,
// RE_StretchPic, sub_824178C8, sub_82417830, sub_82417808) on the owner's PC.
// [acct2-decomp]
#include <cstdint>

#include "cod3/ppc.h"
#include "cod3/tr_public.h"

namespace cod3 {

std::uint32_t tr_color2D = 0xFFFFFFFFu;

const void* nglCurScene = nullptr;

namespace {
cvar_t r_zfar_default = {nullptr, nullptr, nullptr, nullptr, 512, 0, 0, 8192.0f, 8192};
}  // namespace

cvar_t* r_zfar = &r_zfar_default;
float tr_zFarMin = 0.0f;

void RE_SetColor(const float* rgba) {
    if (!rgba) {
        tr_color2D = 0xFFFFFFFFu;  // white
        return;
    }
    // Each component times 255 (0x8207C64C), truncated (fctiwz), then packed
    // alpha, red, green, blue by shifting: no clamp and no mask, so a value
    // outside 0..255 overlaps the next component.
    const std::uint32_t a = static_cast<std::uint32_t>(ppc::fctiwz(static_cast<double>(rgba[3] * 255.0f)));
    const std::uint32_t r = static_cast<std::uint32_t>(ppc::fctiwz(static_cast<double>(rgba[0] * 255.0f)));
    const std::uint32_t g = static_cast<std::uint32_t>(ppc::fctiwz(static_cast<double>(rgba[1] * 255.0f)));
    const std::uint32_t b = static_cast<std::uint32_t>(ppc::fctiwz(static_cast<double>(rgba[2] * 255.0f)));
    tr_color2D = ((((a << 8) | r) << 8 | g) << 8) | b;
}

void RE_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader) {
    if (!nglCurScene) {
        return;
    }
    nglQuad quad;
    nglInitQuad(&quad);
    // The source: nglSetQuadRect(&quad, x, y, x + w, y + h); the adds as the
    // PowerPC does them (an infinite x and w the other way make its NaN).
    nglSetQuadRect(&quad, x, y, ppc::fadds(x, w), ppc::fadds(y, h));
    nglSetQuadUV(&quad, s1, t1, s2, t2);
    nglSetQuadColor(&quad, tr_color2D);
    quad.material = hShader;
    nglListAddQuad(&quad);
}

void RE_DrawQuadPic(const float verts[4][2], const float st[4][2], qhandle_t material) {
    if (!nglCurScene) {
        return;
    }
    nglQuad quad;
    nglInitQuad(&quad);
    nglSetQuadColor(&quad, tr_color2D);
    // The caller goes round the quad; NGL's corners are top left, top right,
    // bottom left, bottom right, so the last two swap.
    static const int corner[4] = {0, 1, 3, 2};
    for (int k = 0; k < 4; ++k) {
        nglQuadVertex& v = quad.verts[corner[k]];
        v.x = verts[k][0];
        v.y = verts[k][1];
        v.u = st[k][0];
        v.v = st[k][1];
    }
    quad.material = material;
    nglListAddQuad(&quad);
}

float RE_GetZFar() {
    const float zfar = r_zfar->value;
    return zfar < tr_zFarMin ? tr_zFarMin : zfar;
}

void RE_SetZFarMin(float zfar) {
    tr_zFarMin = zfar > 0.0f ? zfar : 0.0f;
}

}  // namespace cod3
