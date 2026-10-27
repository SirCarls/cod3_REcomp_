// Call of Duty 3's renderer, as the rest of the game sees it: the 2D color and
// quads, Treyarch's NGL quad, and the table of renderer calls (refexport_t).
// [acct2-decomp]
//
// The renderer keeps Quake III's front end (RE_* functions handed to the game
// in a table by GetRefAPI) but draws through Treyarch's own library, NGL: a
// 2D picture is an NGL quad added to the current scene's render lists, not a
// Quake III backend command. Everything here is read from the recompiled C++
// (recomp/generated) on the owner's PC; addresses are default.xex's.
#pragma once

#include <cstdint>

namespace cod3 {

typedef int qhandle_t;

// ---- NGL quads (ngl_quad.cpp) ------------------------------------------------
//
// NGL's own names for these aren't in the executable; nglListAddQuad is the
// symbol map's, the others are ours in its style.

// One corner of a quad: screen position, texture coordinates, a D3DCOLOR (ARGB).
struct nglQuadVertex {
    float x, y;
    float u, v;
    std::uint32_t color;
};  // 20 bytes

// A 2D quad, 104 bytes as the game keeps it.
struct nglQuad {
    nglQuadVertex verts[4];  // +0: top left, top right, bottom left, bottom right
    float z;                 // +80: the depth it sorts by (when NGL_QUAD_SORTED)
    std::int32_t unknown84;  // +84: 193 from nglInitQuad (not known yet)
    std::uint64_t flags;     // +88: 0x607C00000 from nglInitQuad, so sorted by default
    qhandle_t material;      // +96: what it's drawn with (RE_StretchPic's shader handle)
    std::uint32_t pad100;    // +100
};

// The flag (in the low word of flags) that puts a quad in the scene's
// depth-sorted list instead of the by-material one. (Name ours.)
constexpr std::uint64_t NGL_QUAD_SORTED = 0x00800000u;

// Clears quad and sets the defaults: texture coordinates (0,0) (1,0) (0,1) (1,1),
// white corners, sorted.
// 0x8212A7B0; recompiled: sub_8212A7B0
void nglInitQuad(nglQuad* quad);

// The screen rectangle from (x0, y0) to (x1, y1).
// 0x8212A898; recompiled: sub_8212A898
void nglSetQuadRect(nglQuad* quad, float x0, float y0, float x1, float y1);

// The texture coordinates from (s0, t0) to (s1, t1).
// 0x8212A858; recompiled: sub_8212A858
void nglSetQuadUV(nglQuad* quad, float s0, float t0, float s1, float t1);

// The same color at all four corners.
// 0x8212A880; recompiled: sub_8212A880
void nglSetQuadColor(nglQuad* quad, std::uint32_t color);

// Copies quad into a node of the frame's memory and links it into the current
// scene: its depth-sorted list when NGL_QUAD_SORTED, else its by-material list.
// That is the console's (written out in a comment in ngl_quad.cpp; not checked:
// it needs the scene and frame memory in the oracle). On a PC there are no
// render lists: each quad goes to nglQuadSink (ours), which a PC renderer
// sets, or the checker, to see what was drawn.
// 0x8212AC58; recompiled: nglListAddQuad
void nglListAddQuad(const nglQuad* quad);

// Where nglListAddQuad sends quads on a PC (not the console's; name ours).
extern void (*nglQuadSink)(const nglQuad* quad);

// The scene NGL is building, if any: the 2D calls draw only while there is one.
// 0x8290E9A4 (name ours)
extern const void* nglCurScene;

// ---- the 2D calls (tr_cmds.cpp) ----------------------------------------------

// The color the next 2D pictures are drawn with, as a D3DCOLOR (ARGB).
// 0x82A11C0C (name ours)
extern std::uint32_t tr_color2D;

// Sets the 2D color: each component times 255 and truncated, packed as ARGB;
// NULL is white. Not clamped: a component outside 0..1 spills into its
// neighbours' bits, as on the console.
// 0x82417A58; recompiled: RE_SetColor
void RE_SetColor(const float* rgba);

// Draws a 2D picture: an NGL quad from (x, y) to (x + w, y + h) with texture
// coordinates (s1, t1)-(s2, t2), the 2D color and hShader. Nothing when no
// scene is being built.
// 0x824179A0; recompiled: RE_StretchPic
void RE_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader);

// Draws a 2D quad of any shape: four screen points and their texture
// coordinates, given going round the quad (top left, top right, bottom right,
// bottom left), with the 2D color and material. Nothing when no scene is being
// built. (Name ours: slot 21 of refexport_t.)
// 0x824178C8; recompiled: sub_824178C8
void RE_DrawQuadPic(const float verts[4][2], const float st[4][2], qhandle_t material);

// ---- the far plane (tr_cmds.cpp) ---------------------------------------------

// A console variable as the renderer reads it; only `value` is used here
// (at +28 on the console, as in Quake III's cvar_t).
struct cvar_t {
    char* name;
    char* string;
    char* resetString;
    char* latchedString;
    int flags;
    int modified;
    int modificationCount;
    float value;
    int integer;
};

// The far clip distance the player has set ("r_zfar", default "8192", flags 512).
// 0x82A11B2C holds the pointer (name ours)
extern cvar_t* r_zfar;

// The least far clip distance the game asks for (a level's fog, say). 0x82AA56E8 (name ours)
extern float tr_zFarMin;

// The far clip distance to draw with: r_zfar, but never nearer than tr_zFarMin.
// (Name ours: slot 9 of refexport_t.)
// 0x82417830; recompiled: sub_82417830
float RE_GetZFar();

// Sets tr_zFarMin; a negative distance (or NaN) is 0. (Name ours: slot 14.)
// 0x82417808; recompiled: sub_82417808
void RE_SetZFarMin(float zfar);

// ---- refexport_t: the renderer's calls, as GetRefAPI hands them out ----------
//
// GetRefAPI (0x824309F0) fills the table at 0x82AB9B58 and returns it. The slot
// is the byte offset / 4 (the numbering docs/FUNCTIONS.md and the symbol map
// use). Slots it leaves empty: 3, 4, 6, 7, 8, 13, 16, 19, 22, 23, 27, 28, 29.
// Names for the unnamed ones are still to be found (their callers say which
// call each is).
//
//  slot  offset  function                  address
//   0      0     RE_Shutdown               0x82419700
//   1      4     RE_BeginRegistration      0x8242BA20
//   2      8     ?                         0x8242F770 (calls 0x8242F4A8)
//   5     20     ?                         0x82417B08 (*0x82A2ABC8 to 0x82AA05F0; points 0x82AADA20 at 0x82AA04F0)
//   9     36     RE_GetZFar (ours)         0x82417830
//  10     40     (empty)                   0x824214D0 (returns at once)
//  11     44     ?                         0x82417088 (copies 3 words in 0x82A11Bxx: +7016 to +7160, +6984 to +7260, +7296 to +7228)
//  12     48     ?                         0x8241F720 (539 lines)
//  14     56     RE_SetZFarMin (ours)      0x82417808
//  15     60     RE_RenderScene            0x82430710
//  17     68     RE_SetColor               0x82417A58
//  18     72     RE_StretchPic             0x824179A0
//  20     80     ?                         0x82417698 (not a function start in the index)
//  21     84     RE_DrawQuadPic (ours)     0x824178C8
//  24     96     RE_BeginFrame             0x8241A0A0
//  25    100     RE_EndFrame               0x824178A0
//  26    104     (empty)                   0x82417898 (returns at once)
//  30    120     ?                         0x82419AD8
//  31    124     (empty)                   0x82417770 (returns at once)
//  32    128     ?                         0x82417870 (a, b to 0x82AADACC, 0x82AADAC8)
//  33    132     ?                         0x82417858 (a, b to 0x82AADAE0, 0x82AADADC)
//  34    136     ? (text: looks for '{' '}', a scale in f1) 0x8242A908
//  35    140     ?                         0x82419C40
//  36    144     ?                         0x8242A8D0 (slot 39's call with two more arguments: 0, -1)
//  37    148     ?                         0x8242C7C8
//  38    152     ?                         0x8242C640 (relatives: CoD2 360 SP 0x8236B910, WaW 360 SP 0x82448600)
//  39    156     ?                         0x8242A1E0
struct refexport_t;

}  // namespace cod3
