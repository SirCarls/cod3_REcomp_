// Treyarch's NGL quads (tr_public.h), as the original source would read: from
// the recompiled C++ (sub_8212A7B0, sub_8212A898, sub_8212A858, sub_8212A880,
// nglListAddQuad) on the owner's PC. [acct2-decomp]
#include <cstddef>
#include <cstring>

#include "cod3/tr_public.h"

namespace cod3 {

void nglInitQuad(nglQuad* quad) {
    std::memset(quad, 0, sizeof(*quad));
    // Texture coordinates over the whole image, corner by corner.
    quad->verts[0].u = 0.0f;
    quad->verts[0].v = 0.0f;
    quad->verts[1].u = 1.0f;
    quad->verts[1].v = 0.0f;
    quad->verts[2].u = 0.0f;
    quad->verts[2].v = 1.0f;
    quad->verts[3].u = 1.0f;
    quad->verts[3].v = 1.0f;
    for (nglQuadVertex& corner : quad->verts) {
        corner.color = 0xFFFFFFFFu;  // white, opaque
    }
    quad->unknown84 = 193;
    quad->flags = std::uint64_t{0x181F} << 22;  // includes NGL_QUAD_SORTED
}

void nglSetQuadRect(nglQuad* quad, float x0, float y0, float x1, float y1) {
    quad->verts[0].x = x0;
    quad->verts[0].y = y0;
    quad->verts[1].x = x1;
    quad->verts[1].y = y0;
    quad->verts[2].x = x0;
    quad->verts[2].y = y1;
    quad->verts[3].x = x1;
    quad->verts[3].y = y1;
}

void nglSetQuadUV(nglQuad* quad, float s0, float t0, float s1, float t1) {
    quad->verts[0].u = s0;
    quad->verts[0].v = t0;
    quad->verts[1].u = s1;
    quad->verts[1].v = t0;
    quad->verts[2].u = s0;
    quad->verts[2].v = t1;
    quad->verts[3].u = s1;
    quad->verts[3].v = t1;
}

void nglSetQuadColor(nglQuad* quad, std::uint32_t color) {
    for (nglQuadVertex& corner : quad->verts) {
        corner.color = color;
    }
}

// ---- the render lists (not checked yet) ----------------------------------------
//
// What nglListAddQuad does, from the recompiled code, with the console's
// addresses. On a PC there is no NGL scene to link into, so these are
// declarations for reading, and the function below says what it does in
// the console's own terms.

namespace {

// A quad's node in a render list: 120 bytes, 16-byte aligned. (Names ours.)
struct nglQuadNode {
    std::uint32_t vtable;     // +0: 0x82018020
    std::uint32_t next;       // +4: the list's previous head
    std::uint32_t sortKey;    // +8: the material, or the depth's bits when sorted
    std::uint32_t pad12;
    nglQuad quad;             // +16: a copy of the caller's quad
};

}  // namespace

void (*nglQuadSink)(const nglQuad* quad) = nullptr;

void nglListAddQuad(const nglQuad* quad) {
    // On the console:
    //   if (!quad) return;
    //   node = (nglFrameMemory + 15) & ~15; nglFrameMemory = node + 120;   // 0x8290E9BC
    //   if (!node) return;
    //   node->vtable = 0x82018020;
    //   sub_8212D0E8(nglCurScene);                                          // 0x8290E9A4
    //   memcpy(&node->quad, quad, 104);
    //   if (!(quad->flags & NGL_QUAD_SORTED)) {
    //       node->sortKey = quad->material;
    //       node->next = scene->quads (+820); scene->quads = node; scene->quadCount++ (+828);
    //   } else {
    //       node->sortKey = bits of quad->z;
    //       node->next = scene->sortedQuads (+824); scene->sortedQuads = node; scene->sortedQuadCount++ (+832);
    //   }
    // On a PC the quad goes to whoever takes it (a renderer, or the checker);
    // the Rust renderer's host backend gets the console's draws from the GPU's
    // commands instead.
    static_assert(offsetof(nglQuadNode, quad) == 16, "the node's quad is at +16");
    if (!quad) {
        return;
    }
    if (nglQuadSink) {
        nglQuadSink(quad);
    }
}

}  // namespace cod3
