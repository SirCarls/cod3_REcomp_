// NGL on Vulkan: 3D meshes, with their base texture (or, without one, a color the draw's tint picks), lit by their
// lightmap, their corners' colors, or the scene's light (its ambient plus one to four directional lights, held to 1
// when the draw asks, as Call of Duty 3's vertex shaders saturate theirs; without the scene's, one fixed light from
// above: the games' worlds have z up). The normal is the corners' (the mesh's own space), or
// without them the face's, from the screen-space slopes of the mesh's own positions (it faces the eye; turned back
// under a mirroring world matrix); the light comes in already turned into the mesh's space. Alpha-tested
// draws keep only pixels whose alpha is above one half (the NV2A's alpha test at 0x80); the alpha goes out for the
// blended pipeline. Everything but the sky is fogged by the scene's fog (nglSetFog, nglSetFogColor).
//
// Two textures mixed (Big Red One's cdWorldBlend, its pixel shaders 0x0089F090 and 0x0089F270): the base texture and
// the second (set 1, at the lightmap's coordinates) mixed by the corners' alpha (cdOcean's, 0x0089E498: by the second's
// alpha too), then times a third (set 2, at the coordinates in the normal's place) when there is one, else unlit; the
// base texture's alpha is kept. A draw not mixed can be multiplied by a third texture the same way after its light
// (cdOcean's foam, times the map over the sea).
//
// The sun's shadow (Call of Duty 3's sun cascades, ngl_vk_set_sun_shadow): for a draw marked shadowed, the first light
// (or the fixed one) times how much of the sun reaches the pixel past the casters in the shadow map (sun_reaching).
// A lightmapped or mixed draw marked shadowed (Big Red One's shadow projector under the soldiers) is darkened by half
// where nothing of it reaches.
//
// Call of Duty 3's lightmapped world (MODE_LIGHTMAP_SUN; its pixel shaders 141294CDC7C97127, E72CD0D5E1523920 and
// kin): the lightmap x (1 + up x the normal against the world's up) plus the sun (the first light) x the lightmap's
// alpha (how much of the sun reaches there) x the normal against the sun (times sun_reaching when shadowed), all
// times the lightmap's fetch scale. The ambient's place holds (scale, up); the second light's way is the world's up.
//
// Bumped surfaces (Call of Duty 3's per-pixel lit models, the gun and arms: pixel shader CDBAC6026B525050): a normal
// map (set 1, DXT5, x and y in alpha and green) bends the normal in a frame made from the screen slopes of the position
// and the base texture's coordinates, as that shader does, and a specular map (set 2) times the first light's highlight
// (power 30, toward the eye) is added.
//
// Shiny surfaces (Call of Duty 3's guns: an environment_Texture and a specular map): lit as usual, plus the
// environment map (set 1) as a sphere map at the normal turned toward the eye (0.5 + 0.5 x its x and y), times the
// mask (set 2, at the base texture's coordinates) when there is one.
//
// [rexglue-native] 2026-11-09: Call of Duty 3's uniforms (its fabric pixel shader 3BA30872B3955C23; a bumped surface
// with a specular map whose draw-light slot carries the fabric's numbers, ngl_vk.h fabric_mask): a woven swatch normal
// map (set 5), tiled across the coordinates, is folded into the normal map's normal by the fabric mask (set 4) channel
// by channel; the base texture is scaled (0.51) and lifted toward pow(it, (0.507, 0.583, 0.71)) where the eye sees the
// surface edge-on; and highlights come from the first light and the nearby point lights, toward the eye's own place.
// Built a second time with NGL_VK_FOUR_SETS for a device with fewer than seven descriptor sets (rn34; was six):
// without the fabric (or the skin and the gun metal below).
// rn30: the soldiers' own uniforms (the game's 21168F53BCF9D156; 3BA30872B3955C23 draws only the view model's sleeves)
// keep the base texture unscaled, and their first light is max(mix(its color x the shadow, 1, saturate(fade x the view
// depth)), floor): in the sun's shadow they still take 0.4 of it (ngl_vk.h fabric_shadow_floor).
// rn32: the soldiers' skin (the game's 3949D29E09A78AA1, the heads; ngl_vk.h noise_swatch) rides the same slot and
// sets: no swatch folded into the normal; the base texture x mix(1.215, 1, s), s from how squarely the surface faces the
// eye; highlights of power 68.528 taken by mix(0.4 x the specular map, the noise swatch at (uv - 0.5) x 11.018 + 0.5,
// 0.38); the first light as the uniforms'. rn40t: the helmets of 1D4725CEF0551EBB the same way with their own numbers
// (no noise: its share 0); rn40x: those of ADC02DCE4FDC9253 also fold a woven swatch (set 4) into the normal whole,
// its repeat in the slot's vec4 15 z (0: none).
// rn34: the gun metal (the game's CFC45995D1294CCE: the Springfield, the BREN; ngl_vk.h metal_cube) rides the same slot
// (and four more vec4s) and sets, plus a cube in set 6: the base texture x Metal_Hue; the light x texture_brightness;
// highlights (the first light and the point lights, power SpecularPower, no N.L) x texture_brightness, plus
// Reflection_Amt x the cube at the eye's reflection in world space (read at its x, z, y, as the game reads it), all
// times the specular map + the scratch map (set 4) + mix(the base texture, a blue-grey rim color, by how edge-on the
// eye sees the surface). Not in the four-sets build either.
// rn36: the vehicles' detail-mapped metal (the game's C90DF51FF5AAF4F8 and 8E7F713B9E3C1FAD: The Mace's tank body;
// ngl_vk.h detail_map) rides the fabric's slot and sets, the detail map in set 5: read at the coordinates x its repeat
// and folded into the normal everywhere (no mask), each z scaled as the material says; the base texture as it is;
// highlights (the first light and the point lights, the material's power) toward the eye's own place; the first light
// as the uniforms'. Not in the four-sets build either.
// rn39: the bumped vehicle parts (the game's B2E0DA0062DFF88D and 9A3D3175A6CAFE56, The Mace's tank treads, and
// 7D4E7CAD9503762D, its wheels, and their unshadowed passes beyond the sun's shadow range; ngl_vk.h vehicle_bumped)
// ride the fabric's slot alone (nothing in sets 4 and 5): the normal map's x and y times the material's scale (z from
// the unscaled ones); the base texture as it is; highlights (the first light and the point lights, the material's
// power) toward the eye's own place; the first light as the uniforms' (B2E0DA00's floor and fade; the others have
// none). Not in the four-sets build either.
// rn40: glass with an environment cube (the soldiers' goggle lenses: the game's FC767C09DBD34C2A with a cube in its tf1;
// ngl_vk.h lens_cube): no normal map, its numbers in the draw's slot (the third vec4 of the fabric's numbers' z 1) and
// its cube in set 6: the base texture x its alpha x (the irradiance rows + the slot's sun x N.L + the nearby point
// lights), plus (highlights of the slot's power from that sun, at its place, and the point lights, toward the eye's
// own place, + the cube at the eye's reflection in world space, read at its x, z, y) x (1 - the alpha) x the
// reflection's scale; no shadow (that shader reads none); drawn blended by the alpha. Not in the four-sets build either.
// rn40e: the soldiers' eyes (the game's 5931940F174EA055, a reflection cube; ngl_vk.h lens_opaque; rn40e-rn40m took
// them for the helmets, which are 1D4725CEF0551EBB's, drawn as the skin above) the same way but
// opaque: the base texture x the light, plus (the highlights, power 500, + the cube x 0.072696) x (0.882, 1, 0.990363);
// the sun shadowed and held as the treads' (max(mix(color x shadow, 1, fade x depth), floor)).
// [acct2-bro] 2026-11-13 rn86a: Call of Duty 3's water (the game's 99861CE203379D5B; ngl_vk.h cod3_water) rides
// Call of Duty 2's water's slot and cube: its ripple map read twice bends the normal; its color x the world's light
// where the sky doesn't show, the sky in the cube (and the sun's glint) where it does, by a Fresnel term; blended.
// Not in the four-sets build either.
// rn41q: the lanterns' unlit effects (ngl_vk.h NGL_VK_TINT_*, in an unlit textured draw's tint): the base texture x
// the corners' colors with the texture's alpha alone (the glass, 5DD7AA099EE15C4F), or the same from one frame of a
// strip of frames side by side (the flames, 1DF1E9465C1429B4); no fog, as for every unlit draw.
#version 450

layout(set = 0, binding = 0) uniform sampler2D base_texture;
layout(set = 1, binding = 0) uniform sampler2D lightmap_texture;    // or the second texture mixed in
layout(set = 2, binding = 0) uniform sampler2D third_texture;
#ifndef NGL_VK_FOUR_SETS
layout(set = 4, binding = 0) uniform sampler2D fabric_mask;         // [rexglue-native] the uniforms' Fabric_Mask
layout(set = 5, binding = 0) uniform sampler2D normal_swatch;       // and Normal_Swatch (ngl_vk.h; rn32: the skin's
                                                                    // Noise_Swatch)
layout(set = 6, binding = 0) uniform samplerCube metal_cube;        // rn34: the gun metal's environment (set 4: its
                                                                    // scratch map)
#endif
// The sun's shadow (ngl_vk_set_sun_shadow): the depth map, compared as it is read (1: no caster nearer the sun), and
// the frame's lookup (mesh.vert uses the matrix; here the bias and a texel)
layout(set = 3, binding = 0) uniform sampler2DShadow sun_shadow;
layout(set = 3, binding = 2) uniform sampler2D scene_depth;   // the opaque meshes' depth, for soft draws (SOFT)
// The draws' own lights (ngl_vk.c place_draw_lights): a slot of 20 vec4s (rn34; was 16), two nearby lights of (place
// in the mesh's own space, inverse range) then (color, 1 when the light is there), then the game's nine irradiance
// rows (row 0's w 1 when they are there), then the fabric's numbers (FABRIC_AT), then (rn34) the gun metal's last ones
// (METAL_AT); the draw's slot in its flags' high 16 bits (1 up), 0: none
layout(std430, set = 3, binding = 3) readonly buffer DrawLights {
    vec4 draw_lights[];
};
layout(set = 3, binding = 1) uniform Shadow {
    mat4 lookup;
    vec4 bias_texel;
} shadow;

layout(location = 0) in vec3 object_position;
layout(location = 1) flat in uint tint;
layout(location = 2) in vec2 texture_uv;
layout(location = 3) in vec2 lightmap_texture_uv;
layout(location = 4) in vec4 vertex_color;
layout(location = 5) in float fog_amount;
layout(location = 6) flat in vec3 fog_rgb;
layout(location = 7) flat in uvec4 light_toward;  // mesh.vert: each light's way toward it, 10-bit signed x, y, z
layout(location = 8) flat in uvec4 light_colors;  // and its color, 10-bit red, green, blue, 0 to 4
layout(location = 9) flat in vec3 ambient;
layout(location = 10) in vec3 vertex_normal;
layout(location = 11) flat in uint flags;       // ngl_vk.c's PUSH_*; bits 8-15 the draw's MODE_* bits
layout(location = 12) flat in vec4 eye_x;       // mesh.vert: the normal turned toward the eye is
layout(location = 13) flat in vec4 eye_y;       // (eye_x . n, eye_y . n, eye_w . n), xyz of each; their w's are
layout(location = 14) flat in vec4 eye_w;       // the eye's place in the mesh's own space
layout(location = 15) in vec4 shadow_position;  // mesh.vert: in the shadow map's clip space

layout(location = 0) out vec4 color;

#ifdef NGL_VK_MSAA_COVERAGE
// [acct2-bro] 2026-11-22 rn113b: built a second time with NGL_VK_MSAA_COVERAGE for the mesh pipelines' MSAA twins
// (ngl_vk.c msaa_twin): the samples a pixel (a specialization constant the twins set to MSAA's count), by which a
// cut-out's edge takes its share of the pixel's samples (see the alpha test)
layout(constant_id = 0) const int MSAA_SAMPLES = 4;
#endif
const uint ALPHA_TEST = 0x80000000u;        // ngl_vk.c's MESH_ALPHA_TEST
const uint TEXTURED = 0x40000000u;          // ngl_vk.c's MESH_TEXTURED
const uint SHADING_SHIFT = 28u;             // MESH_SHADING_SHIFT; the values are NGL_VK_SHADE_*
const uint SHADE_LIGHT = 0u;                // [acct2-bro] 2026-11-13 rn67a: NGL_VK_SHADE_LIGHT, named for ONE_SIDED
const uint SHADE_LIGHTMAP = 1u;
const uint SHADE_VERTEX_COLOR = 2u;
const uint SHADE_UNLIT = 3u;
const uint LIGHT_ONCE = 0x08000000u;       // MESH_LIGHT_ONCE: the lightmap or colors not doubled
const uint TINT = 0x07FFFFFFu;
// [rexglue-native] 2026-11-10 rn41q: a textured unlit draw's tint carrying one of Call of Duty 3's unlit effects
// (ngl_vk.h NGL_VK_TINT_*: the lanterns' glass, standard_7_1's 5DD7AA099EE15C4F, and flames, standard_10's
// 1DF1E9465C1429B4): the alpha the texture's alone, and a strip of frames side by side (frames in the low 4 bits,
// the one shown in bits 4-7)
const uint TINT_EFFECT = 0x04000000u;
const uint TINT_MAP_ALPHA = 0x02000000u;
// [rexglue-native] 2026-11-10 rn41x: a textured lit draw's tint (ngl_vk.h NGL_VK_TINT_LIGHT_DIMMED; the SpeedTree
// leaves, 51732ACF1B6A87C5 / 6189945E43D7FBC8): the directional lights times the corners' red, no sun shadow
const uint TINT_LIGHT_DIMMED = 0x01000000u;
// [rexglue-native] 2026-11-10 rn46c: a textured lit draw's tint (ngl_vk.h NGL_VK_TINT_LIGHT_MAPPED; SpeedTree's
// branches and fronds, EA6B670C8ACDCED1 and F38DD169E44123DB): the directional lights times the texture in set 1
// (the tree's self-shadow map) at the lightmap's coordinates, no sun shadow; the ambient is not dimmed by it
// [rexglue-native] 2026-11-10 21:40: was 0x00800000u, the top bit of the tint color's red (a bright tint on a textured
// lit draw was taken for a tree: waw-decomp's 21:10 report); now TINT_MAP_ALPHA's number, which is read in the unlit
// branch only, as this one is in the scene-light branch only
const uint TINT_LIGHT_MAPPED = 0x02000000u;
// [acct2-bro] 2026-11-13 rn67a: a textured lit draw's tint (ngl_vk.h NGL_VK_TINT_ONE_SIDED; Call of Duty 3's old
// simple class, the oaks' leaf cards): seen from one side only. The mesh pipeline culls nothing and its front
// faces wind clockwise on the screen; the game's cull mode for these (D3DCULL_CW) drops exactly those. With
// TINT_OTHER_SIDE the faces that wind the other way are dropped instead. TINT_EFFECT's number: that one is read on
// unlit draws only (see effect in main), this one on lit draws only.
const uint TINT_ONE_SIDED = 0x04000000u;
const uint TINT_OTHER_SIDE = 0x00000001u;
const uint MODE_MIX = 1u;                   // the base texture and the second (set 1) mixed
const uint MODE_THIRD = 2u;                 // times the third (set 2) at the coordinates in the normal's place
const uint MODE_SHINY = 4u;                 // plus the environment map (set 1)
const uint MODE_MASKED = 8u;                // the environment map times the mask (set 2)
const uint MODE_BY_ALPHA = 16u;             // the mix also by the second's alpha
const uint MODE_BUMPED = 32u;               // a normal map (set 1) bends the normal
const uint MODE_SPECULAR = 64u;             // and a highlight times a specular map (set 2)
const uint MODE_LIGHTMAP_SUN = 128u;        // CoD3's world: lightmap x (1 + up part) + sun x the lightmap's alpha
const uint SCENE_LIGHT = 1u;                // ngl_vk.c's PUSH_SCENE_LIGHT: the scene's lights, not the fixed one
const uint MIRRORED = 2u;                   // PUSH_MIRRORED: the world matrix's determinant is negative
const uint MORE_LIGHTS_SHIFT = 2u;          // PUSH_MORE_LIGHTS_SHIFT: how many lights after the first, 2 bits
const uint HELD = 16u;                      // PUSH_HELD: the light held to 1 before the texture
const uint SHADOWED = 32u;                  // PUSH_SHADOWED: the first light dimmed by the sun's shadow map
const uint SOFT = 64u;                      // PUSH_SOFT: faded near the opaque meshes behind it (scale in the lightmap u;
                                            // negative: its own depth in the lightmap v, rn42j)
const uint CORNER_ALPHA = 128u;             // PUSH_CORNER_ALPHA: the alpha is the corners' alone (the world's layer B)
const uint MODE_SHIFT = 8u;                 // PUSH_MODE_SHIFT

// How much of the sun reaches this pixel, 0 to 1: four comparisons half a texel around it, averaged (each filtered
// where the GPU filters depth). Outside the map, or for a draw not shadowed, all of it. The map's v runs down where
// Direct3D's clip y runs up (mesh.vert turns y over for Vulkan).
float sun_reaching()
{
    if ((flags & SHADOWED) == 0u || shadow_position.w <= 0.0)
        return 1.0;
    vec3 p = shadow_position.xyz / shadow_position.w;
    vec2 uv = vec2(0.5 + 0.5 * p.x, 0.5 - 0.5 * p.y);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || p.z <= 0.0 || p.z >= 1.0)
        return 1.0;
    float depth = p.z - shadow.bias_texel.x;
    float t = 0.5 * shadow.bias_texel.y;
    return 0.25 * (textureLod(sun_shadow, vec3(uv + vec2(-t, -t), depth), 0.0) +
                   textureLod(sun_shadow, vec3(uv + vec2(t, -t), depth), 0.0) +
                   textureLod(sun_shadow, vec3(uv + vec2(-t, t), depth), 0.0) +
                   textureLod(sun_shadow, vec3(uv + vec2(t, t), depth), 0.0));
}

vec3 unpacked_toward(uint p)
{
    ivec3 v = ivec3(bitfieldExtract(int(p), 0, 10), bitfieldExtract(int(p), 10, 10), bitfieldExtract(int(p), 20, 10));

    return vec3(v) / 511.0;
}

vec3 unpacked_color(uint p)
{
    return vec3(float(p & 1023u), float((p >> 10) & 1023u), float((p >> 20) & 1023u)) * (4.0 / 1023.0);
}

vec3 tint_color(uint t)
{
    uint h = t * 2654435761u;               // spread the tint's bits

    return vec3(0.35) + 0.45 * vec3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0;
}

const uint DRAW_LIGHT_VEC4S = 20u;          // ngl_vk.c's DRAW_LIGHT_FLOATS / 4
const uint IRRADIANCE_AT = 4u;              // the irradiance rows' first vec4 in a slot
// [acct2-bro] 2026-11-21 rn101: row 8's w 1 (with row 0's w 1): the game's own normal map frame (ngl_vk.h
// game_bump_frame)
// [rexglue-native] 2026-11-09: the fabric's numbers in a slot (ngl_vk.c place_draw_lights): (the swatch's repeat, the
// diffuse scale, the highlights' power, 1 when the draw is fabric), then the sheen's three powers and (rn30) the
// shadow's floor, then the shadow's fade with the view depth and (rn32) 1 for the skin, whose second vec4 holds (the
// edge lift, the specular map's scale, the noise swatch's share) in the sheen's place. rn34: the gun metal's in the
// same three: (texture_brightness, Reflection_Amt, SpecularPower, 0), (Metal_Hue, the rim's edge), (the rim's scale, 0,
// 1 for metal, 1 when it has a scratch map); then from METAL_AT the rim's color and the cube's bias, and the world
// matrix's three columns (ngl_vk.c's METAL_FLOATS_AT)
const uint FABRIC_AT = 13u;
const uint METAL_AT = 16u;

// The draw's nearby point lights, as the game's shaders add them: LIGHT_COLOR x saturate(1 - |L| / range) x N.L each
// [rexglue-native] 2026-11-10 rn40y: 2340DF59702A9AD4's soft terminator (its instructions 45-60, 98-101; ngl_vk.h
// skin_terminator_scale): s = t^2 (3 - 2t), t = saturate(scale x (the vertex normal . toward the light + bias)), so a
// light fades out where the surface itself turns from it; 1 when numbers.y (the scale) is 0
float terminator(vec3 surface, vec3 toward, vec2 numbers)
{
    if (numbers.y <= 0.0 || dot(toward, toward) <= 0.0)
        return 1.0;
    float t = clamp(numbers.y * (dot(surface, normalize(toward)) + numbers.x), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// the draw's nearby point lights; rn40y: each also taken by terminator(surface, ..., numbers)
vec3 nearby_light(uint slot, vec3 normal, vec3 surface, vec2 numbers)
{
    vec3 sum = vec3(0.0);

    for (uint k = 0u; slot != 0u && k < 2u; k++) {
        vec4 place = draw_lights[DRAW_LIGHT_VEC4S * (slot - 1u) + 2u * k];
        vec4 shine = draw_lights[DRAW_LIGHT_VEC4S * (slot - 1u) + 2u * k + 1u];
        vec3 to_light = place.xyz - object_position;
        float away = length(to_light);

        if (shine.w <= 0.0 || away <= 0.0)
            continue;
        sum += shine.rgb * clamp(1.0 - away * place.w, 0.0, 1.0) * max(dot(normal, to_light / away), 0.0) *
               terminator(surface, to_light, numbers);
    }
    return sum;
}

bool has_irradiance(uint slot)
{
    return slot != 0u && draw_lights[DRAW_LIGHT_VEC4S * (slot - 1u) + IRRADIANCE_AT].w > 0.0;
}

// [rexglue-native] 2026-11-09: the fabric's highlights from the draw's nearby point lights, as its shader adds them:
// LIGHT_COLOR1 x the falloff x saturate(N . H)^power, H halfway between the ways toward the light and the eye
vec3 nearby_highlight(uint slot, vec3 normal, vec3 toward_eye, float power)
{
    vec3 sum = vec3(0.0);

    for (uint k = 0u; slot != 0u && k < 2u; k++) {
        vec4 place = draw_lights[DRAW_LIGHT_VEC4S * (slot - 1u) + 2u * k];
        vec4 shine = draw_lights[DRAW_LIGHT_VEC4S * (slot - 1u) + 2u * k + 1u];
        vec3 to_light = place.xyz - object_position;
        float away = length(to_light);
        vec3 halfway = away > 0.0 ? to_light / away + toward_eye : vec3(0.0);

        if (shine.w <= 0.0 || dot(halfway, halfway) <= 0.0)
            continue;
        sum += shine.rgb * clamp(1.0 - away * place.w, 0.0, 1.0) *
               pow(max(dot(normal, normalize(halfway)), 0.0), power);
    }
    return sum;
}

// Call of Duty 3's per-pixel lit models' light (ngl_vk.h irradiance; their pixel shaders' IrradianceCoeffs_0..8, as
// CDBAC6026B525050's c18-c26 take them): the nine rows against xx, yy, zz, xy, yz, zx, x, y and z of the normal
vec3 irradiance(uint slot, vec3 n)
{
    uint at = DRAW_LIGHT_VEC4S * (slot - 1u) + IRRADIANCE_AT;

    return draw_lights[at].rgb * (n.x * n.x) + draw_lights[at + 1u].rgb * (n.y * n.y) +
           draw_lights[at + 2u].rgb * (n.z * n.z) + draw_lights[at + 3u].rgb * (n.x * n.y) +
           draw_lights[at + 4u].rgb * (n.y * n.z) + draw_lights[at + 5u].rgb * (n.z * n.x) +
           draw_lights[at + 6u].rgb * n.x + draw_lights[at + 7u].rgb * n.y + draw_lights[at + 8u].rgb * n.z;
}

// [waw-decomp] 2026-11-10: the rows' third-degree term (ngl_vk.h irradiance_cubic), there when row 1's w is 1: its
// color x (a . n)(b . n)(c . n); the three axes are at METAL_AT, each with one part of the color as its w
vec3 irradiance_cubic(uint slot, vec3 n)
{
    uint at = DRAW_LIGHT_VEC4S * (slot - 1u);
    vec4 a = draw_lights[at + METAL_AT], b = draw_lights[at + METAL_AT + 1u], c = draw_lights[at + METAL_AT + 2u];

    if (draw_lights[at + IRRADIANCE_AT + 1u].w <= 0.0)
        return vec3(0.0);
    return vec3(a.w, b.w, c.w) * (dot(a.xyz, n) * dot(b.xyz, n) * dot(c.xyz, n));
}

// the game's textures repeat; the samplers clamp (for the quads), so the coordinates are wrapped here, with the
// unwrapped ones' derivatives so the wrap leaves no seam
vec4 wrapped(sampler2D s, vec2 uv)
{
    return textureGrad(s, fract(uv), dFdx(uv), dFdy(uv));
}

// [rexglue-native] rn41q: frame `shown` of a strip of `frames` side by side, as 1DF1E9465C1429B4 reads its flames:
// u = (fract(u) + the frame) / frames, v = fract(v), with the unwrapped coordinates' slopes, u's scaled to one frame
vec4 strip_frame(sampler2D s, vec2 uv, uint frames, uint shown)
{
    vec2 scale = vec2(1.0 / float(frames), 1.0);
    return textureGrad(s, vec2((fract(uv.x) + float(shown)) * scale.x, fract(uv.y)), dFdx(uv) * scale,
                       dFdy(uv) * scale);
}

#ifndef NGL_VK_FOUR_SETS
// [waw-decomp] 2026-11-10: Call of Duty 2's water (ngl_vk.h water_cube; the game's water.hlsl, on the Xbox 360 its
// pixel shader DD12484223A211FC). The base texture is the water's height map (its red).
// The height at a place of the map, the map repeating, blended between its four nearest texels (the samplers here
// clamp, and blending across the map's edge by hand leaves no seam in the ripples)
float water_height(vec2 uv)
{
    ivec2 size = textureSize(base_texture, 0);
    vec2 at = uv * vec2(size) - 0.5;
    vec2 low = floor(at);
    vec2 part = at - low;
    ivec2 a = ivec2(low - vec2(size) * floor(low / vec2(size)));               // the lower texel, wrapped
    ivec2 b = ivec2(a.x + 1 < size.x ? a.x + 1 : 0, a.y + 1 < size.y ? a.y + 1 : 0);   // and the next one

    return mix(mix(texelFetch(base_texture, a, 0).r, texelFetch(base_texture, ivec2(b.x, a.y), 0).r, part.x),
               mix(texelFetch(base_texture, ivec2(a.x, b.y), 0).r, texelFetch(base_texture, b, 0).r, part.x), part.y);
}

// The ripples' height: three octaves of the map, each 3.7 times finer and 0.6 times as strong as the last (the
// game's "turbulance")
float water_ripples(vec2 uv)
{
    float sum = 0.0, strength = 1.0;

    for (int octave = 0; octave < 3; octave++) {
        sum += water_height(uv) * strength;
        uv *= 3.7;
        strength *= 0.6;
    }
    return sum;
}

// The water's color at this pixel. The slot holds (the water's color, 0), (the sun's color, 0), (0, 1, 0, 0) from
// FABRIC_AT and the way toward the sun at METAL_AT; the mesh's own space is taken for the world's, z up.
vec3 water_color(uint slot)
{
    uint at = DRAW_LIGHT_VEC4S * (slot - 1u);
    vec3 eye = normalize(object_position - vec3(eye_x.w, eye_y.w, eye_w.w));   // the way from the eye to the pixel
    // the coordinates moved along the eye's way by how far the height at half the coordinates is from 0.5
    vec2 uv = texture_uv + (0.5 - water_height(texture_uv * 0.5)) * eye.xy * (1.5 / 64.0);
    float here = water_ripples(uv);
    float across = water_ripples(uv + vec2(0.5 / 128.0, 0.0));
    float along = water_ripples(uv + vec2(0.0, 0.5 / 128.0));
    vec3 ripple_normal = normalize(vec3(across - here, along - here, 1.0));
    vec3 mirrored = reflect(eye, ripple_normal);
    float glint = max(dot(mirrored, draw_lights[at + METAL_AT].xyz), 0.0);
    vec4 sky = texture(metal_cube, vec3(mirrored.xy, abs(mirrored.z)));

    return mix(draw_lights[at + FABRIC_AT].rgb * ripple_normal.z, sky.rgb, sky.a) +
           pow(glint, 64.0) * draw_lights[at + FABRIC_AT + 1u].rgb;
}

// [acct2-bro] 2026-11-13 rn86a: Call of Duty 3's water (ngl_vk.h cod3_water; the game's 99861CE203379D5B, its
// instructions 16-27: two reads of the ripple normal map, each scrolled on its own). The base texture is that map (x in
// its alpha, y in its green). The slot holds (the water's color, 0), (the reflection's color, 0), (0, 1, 0, -1) from
// FABRIC_AT and from METAL_AT the two layers (scale, offset u, offset v, then the bump's scale and the cube's bias),
// (the Fresnel numbers, the opacity), (the highlights' power, 0, 0, 0). surface is the corner's normal and sun_seen
// how much of the sun reaches the pixel. Gives ((the sky x f + the glint) x the reflection's color, f) and the normal
// the ripples bend, which the draw's light then takes.
vec4 cod3_water_part(uint slot, vec3 surface, float sun_seen, out vec3 bent)
{
    uint at = DRAW_LIGHT_VEC4S * (slot - 1u);
    vec4 first = draw_lights[at + METAL_AT];
    vec4 later = draw_lights[at + METAL_AT + 1u];
    vec4 fresnel = draw_lights[at + METAL_AT + 2u];
    vec2 a = 2.0 * wrapped(base_texture, texture_uv * first.x + first.yz).ag - 1.0;
    vec2 b = 2.0 * wrapped(base_texture, texture_uv * later.x + later.yz).ag - 1.0;
    bent = normalize(surface + vec3(a + b, 0.0) * first.w);
    vec3 eye = normalize(object_position - vec3(eye_x.w, eye_y.w, eye_w.w));   // the way from the eye to the pixel
    vec3 mirrored = reflect(eye, bent);
    float f = clamp(fresnel.x + fresnel.y * pow(max(1.0 - abs(dot(eye, bent)), 0.0), fresnel.z), 0.0, 1.0);
    vec3 sky = texture(metal_cube, vec3(mirrored.x, mirrored.z, mirrored.y), later.w).rgb;
    vec3 toward = unpacked_toward(light_toward[0]);
    float glint = dot(toward, toward) > 0.0
                  ? pow(max(dot(mirrored, normalize(toward)), 0.0), max(draw_lights[at + METAL_AT + 3u].x, 1.0))
                  : 0.0;
    vec3 reflection = draw_lights[at + FABRIC_AT + 1u].rgb;

    return vec4((sky * f + glint * sun_seen * unpacked_color(light_colors[0])) * reflection, f);
}

// [waw-decomp] 2026-11-10: Call of Duty 2's light grid (ngl_vk.h grid_cube; the game's lprobe pixel shaders, the PC
// copy's lightprobe.hlsl), there when the slot's irradiance row 2 has w 1: the normal turned to the world by rows 0
// to 2, the cube of weights (set 6) read there and at the opposite direction, each against its four numbers of
// every color (rows 3 to 8: red's two, green's, blue's)
bool has_grid(uint slot)
{
    return slot != 0u && draw_lights[DRAW_LIGHT_VEC4S * (slot - 1u) + IRRADIANCE_AT + 2u].w > 0.0;
}

vec3 grid_light(uint slot, vec3 n)
{
    uint at = DRAW_LIGHT_VEC4S * (slot - 1u) + IRRADIANCE_AT;
    vec3 way = vec3(dot(n, draw_lights[at].xyz), dot(n, draw_lights[at + 1u].xyz), dot(n, draw_lights[at + 2u].xyz));
    vec4 here = textureLod(metal_cube, way, 0.0);
    vec4 opposite = textureLod(metal_cube, -way, 0.0);

    return vec3(dot(here, draw_lights[at + 3u]) + dot(opposite, draw_lights[at + 4u]),
                dot(here, draw_lights[at + 5u]) + dot(opposite, draw_lights[at + 6u]),
                dot(here, draw_lights[at + 7u]) + dot(opposite, draw_lights[at + 8u]));
}
#endif

void main()
{
#ifdef NGL_VK_MSAA_COVERAGE
    gl_SampleMask[0] = -1;  // [acct2-bro] rn113b: every sample unless a cut-out's edge takes fewer (on every path)
#endif
    uint shading = (tint >> SHADING_SHIFT) & 3u;
    uint mode = (flags >> MODE_SHIFT) & 255u;
    // rn41q (the same for the whole draw); [acct2-bro] 2026-11-13 rn67a: on unlit draws only, as ngl_vk.h has it
    // (on a lit draw the same bit is TINT_ONE_SIDED)
    bool effect = (tint & TEXTURED) != 0u && shading == SHADE_UNLIT && (tint & TINT_EFFECT) != 0u;
    uint strip_frames = effect ? tint & 15u : 0u;
    vec4 base = (tint & TEXTURED) == 0u ? vec4(tint_color(tint & TINT), 1.0)
              : strip_frames > 1u      ? strip_frame(base_texture, texture_uv, strip_frames, (tint >> 4) & 15u)
                                       : wrapped(base_texture, texture_uv);
    // everything that takes derivatives comes before the alpha test's discard
    vec4 second = wrapped(lightmap_texture, lightmap_texture_uv);     // the lightmap, or the second texture mixed
    vec3 third = wrapped(third_texture, vertex_normal.xy).rgb;
    vec3 face_normal = ((flags & MIRRORED) != 0u ? -1.0 : 1.0) *
                       normalize(cross(dFdx(object_position), dFdy(object_position)));
    vec3 normal = dot(vertex_normal, vertex_normal) > 0.01 ? normalize(vertex_normal) : face_normal;
    vec3 surface_normal = normal;   // rn40y: the vertex normal, before any normal map bends it (terminator)
    vec3 light, shine = vec3(0.0), gloss = vec3(0.0);
    float alpha = base.a;
#ifdef NGL_VK_MSAA_COVERAGE
    float alpha_width = fwidth(base.a);  // [acct2-bro] rn113b: the alpha's change across the pixel (before any discard)
#endif
    // [acct2-bro] 2026-11-22 rn115a: the alpha test's edge: the draw's own cut where it has one (ngl_vk.h alpha_cut,
    // in the fourth light's color while the draw has fewer than four lights), its texels of k / 255 and up kept;
    // else one half, as before (above 128 / 255)
    bool own_cut = ((flags >> MORE_LIGHTS_SHIFT) & 3u) < 3u && light_colors[3] != 0u;
    float cut_edge = own_cut ? uintBitsToFloat(light_colors[3]) - 0.5 / 255.0 : 128.0 / 255.0;
    // [rexglue-native] 2026-11-09: the uniforms' fabric (ngl_vk.h fabric_mask): its numbers ride in the draw's slot
#ifndef NGL_VK_FOUR_SETS
    uint fabric_slot = (mode & MODE_BUMPED) != 0u && (mode & MODE_SPECULAR) != 0u ? flags >> 16 : 0u;
    bool fabric = fabric_slot != 0u && draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT].w > 0.0;
    vec4 fabric_numbers = fabric ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT] : vec4(0.0);
    vec3 fabric_sheen = fabric ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 1u].rgb : vec3(1.0);
    // rn30: the third-person uniforms' shadow floor and fade (0: the view model's shader)
    float fabric_floor = fabric ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 1u].w : 0.0;
    float fabric_fade = fabric ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 2u].x : 0.0;
    // rn32: the soldiers' skin (3949D29E09A78AA1): fabric_sheen holds (edge lift, specular scale, noise share)
    bool skin = fabric && draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 2u].y > 0.0;
    // rn40y: the skin's terminator (2340DF59702A9AD4's bias and scale; 0 0: none), METAL_AT's x and y
    vec2 skin_terminator = skin ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT].xy : vec2(0.0);
    // [acct2-bro] 2026-11-12 rn60a: the skin's own tint on its base (ngl_vk.h skin_tint; 0 0 0: none), METAL_AT + 1
    vec3 skin_tint = skin ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT + 1u].xyz : vec3(0.0);
    // rn34: the gun metal (CFC45995D1294CCE): not fabric, its third vec4's z 1
    bool metal = fabric_slot != 0u && !fabric &&
                 draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 2u].z > 0.0;
    // rn36: the vehicles' detail-mapped metal (C90DF51FF5AAF4F8): fabric, its third vec4's w 1
    bool detail = fabric && !skin && draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 2u].w > 0.0;
    // rn39: the bumped vehicle parts (B2E0DA00, 9A3D3175, 7D4E7CAD): fabric, not skin or detail, METAL_AT's w 1
    bool vehicle = fabric && !skin && !detail &&
                   draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT].w > 0.0;
    // [acct2-bro] 2026-11-15 rn99a: the K98 shader (BDA4BB849BDF0C39; ngl_vk.h k98): a vehicle part whose METAL_AT + 3
    // has w 1; METAL_AT + 2 holds its tint and light scale, METAL_AT + 3 its specular color
    bool k98 = vehicle && draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT + 3u].w > 0.0;
    vec4 k98_numbers = k98 ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT + 2u] : vec4(1.0);
    vec3 k98_specular = k98 ? draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT + 3u].rgb : vec3(1.0);
    // [waw-decomp] 2026-11-10: Call of Duty 2's models with a highlights' lookup (ngl_vk.h specular_lookup; the game's
    // 98A77B2DFA1049B4): bumped and specular, not fabric or metal, the slot's third fabric vec4's y 1
    bool lookup = fabric_slot != 0u && !fabric && !metal &&
                  draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 2u].y > 0.0;
    // rn40: the glass (FC767C09DBD34C2A with a cube): not bumped, mixed or shiny, its slot's third vec4's z 1 (only the
    // fabric's kinds write there otherwise, and they are bumped)
    uint lens_slot = (mode & (MODE_BUMPED | MODE_MIX | MODE_SHINY)) == 0u ? flags >> 16 : 0u;
    // [acct2-bro] 2026-11-22 rn111: a shiny draw's own strength and tint (ngl_vk.h env_strength): shiny with the
    // mix's by-alpha bit (ngl_vk.c sets the pair on these draws alone; shiny is never mixed), its slot's last vec4
    // the tint in rgb and the strength in w (0: as before)
    uint shiny_slot = (mode & (MODE_SHINY | MODE_BY_ALPHA | MODE_BUMPED | MODE_MIX)) == (MODE_SHINY | MODE_BY_ALPHA)
                          ? flags >> 16 : 0u;
    vec4 shiny_numbers = shiny_slot != 0u ? draw_lights[DRAW_LIGHT_VEC4S * (shiny_slot - 1u) + METAL_AT + 3u]
                                          : vec4(0.0);
    bool lens = lens_slot != 0u && draw_lights[DRAW_LIGHT_VEC4S * (lens_slot - 1u) + FABRIC_AT + 2u].z > 0.0;
    // [waw-decomp] 2026-11-10: Call of Duty 2's water (ngl_vk.h water_cube): in a slot of the glass's kind, its third
    // vec4's y 1 (the glass leaves it 0); its color is made here, before the alpha test, like every lookup
    bool water = lens_slot != 0u && !lens &&
                 draw_lights[DRAW_LIGHT_VEC4S * (lens_slot - 1u) + FABRIC_AT + 2u].y > 0.0;
    // [acct2-bro] 2026-11-13 rn86a: Call of Duty 3's water (ngl_vk.h cod3_water): of the water's kind, its third
    // vec4's w -1 (Call of Duty 2's leaves it 0); its ripples bend the normal before the light takes it
    bool cod3_water = water && draw_lights[DRAW_LIGHT_VEC4S * (lens_slot - 1u) + FABRIC_AT + 2u].w < -0.5;
    vec3 water_rgb = water && !cod3_water ? water_color(lens_slot) : vec3(0.0);
    vec3 cod3_bent = normal;
    float cod3_sun = cod3_water ? ((mode & MODE_LIGHTMAP_SUN) != 0u ? second.a : 1.0) * sun_reaching() : 0.0;
    vec4 cod3_part = cod3_water ? cod3_water_part(lens_slot, normal, cod3_sun, cod3_bent) : vec4(0.0);
    if (cod3_water)
        normal = cod3_bent;
    // [waw-decomp] 2026-11-10: Call of Duty 2's detail map (ngl_vk.h detail_add_map; its "_dtl" pixel shaders): any
    // slot that is not the fabric's, the metal's, the glass's or the water's, its third fabric vec4's w 2
    uint detail_slot = flags >> 16;
    bool detail_add = detail_slot != 0u && !fabric && !metal && !lens && !water &&
                      draw_lights[DRAW_LIGHT_VEC4S * (detail_slot - 1u) + FABRIC_AT + 2u].w > 1.5;
    // [waw-decomp] 2026-11-10: Call of Duty 2's world surfaces with a highlights' lookup (ngl_vk.h specular_lookup,
    // its second half; the game's lmap_s shaders): a lightmapped draw's slot whose third fabric vec4's x is 1
    bool world_lookup = shading == SHADE_LIGHTMAP && detail_slot != 0u && !lens && !water &&
                        draw_lights[DRAW_LIGHT_VEC4S * (detail_slot - 1u) + FABRIC_AT + 2u].x > 0.0;
    // [waw-decomp] 2026-11-11: Call of Duty 2's world by its normal map (ngl_vk.h world_normal_map): that x is 2
    // (with a highlight: its lookup is on the cube's +z face, set 6) or -1 (without one); the map is in set 4
    float world_mark = shading == SHADE_LIGHTMAP && detail_slot != 0u && !lens && !water
                       ? draw_lights[DRAW_LIGHT_VEC4S * (detail_slot - 1u) + FABRIC_AT + 2u].x : 0.0;
    bool world_bumped = world_mark > 1.5 || world_mark < -0.5;
#else
    const bool fabric = false;
    const vec4 fabric_numbers = vec4(0.0);
    const vec3 fabric_sheen = vec3(1.0);
    const float fabric_floor = 0.0;
    const float fabric_fade = 0.0;
    const bool skin = false;
    const vec2 skin_terminator = vec2(0.0);
    const vec3 skin_tint = vec3(0.0);       // [acct2-bro] 2026-11-12 rn60a
    const bool metal = false;
    const bool detail = false;
    const bool vehicle = false;
    const bool k98 = false;                 // [acct2-bro] 2026-11-15 rn99a
    const vec4 k98_numbers = vec4(1.0);
    const vec3 k98_specular = vec3(1.0);
    const bool lookup = false;              // [waw-decomp]
    const uint lens_slot = 0u;
    const vec4 shiny_numbers = vec4(0.0);   // [acct2-bro] 2026-11-22 rn111
    const bool lens = false;
    const bool water = false;               // [waw-decomp]
    const bool detail_add = false;          // [waw-decomp]
    const bool world_lookup = false;        // [waw-decomp]
    const bool world_bumped = false;        // [waw-decomp] 2026-11-11
    const vec3 water_rgb = vec3(0.0);
    const bool cod3_water = false;          // [acct2-bro] 2026-11-13 rn86a
    const vec4 cod3_part = vec4(0.0);
#endif
    vec3 noise = vec3(0.0);     // rn32: the skin's noise swatch, read before the alpha test like every lookup
    vec3 scratch = vec3(0.0);   // rn34: the gun metal's scratch map and its cube's color, the same
    vec3 reflection = vec3(0.0);
    vec3 lens_tint_rgb = vec3(1.0);     // [acct2-bro] 2026-11-22 defect E: the scopes' glass's tint map (white: none)
    float lookup_sharpness = 0.0;   // [waw-decomp] the specular map's alpha, a lookup draw's highlight's sharpness

    if ((mode & MODE_BUMPED) != 0u) {   // the game's per-pixel lit models (pixel shader CDBAC6026B525050)
        vec4 bump = wrapped(lightmap_texture, texture_uv);
        vec3 bent = vec3(2.0 * bump.a - 1.0, 2.0 * bump.g - 1.0, 0.0);
        vec3 along_x = dFdx(object_position), along_y = dFdy(object_position);
        vec2 uv_x = dFdx(texture_uv), uv_y = dFdy(texture_uv);
        vec3 across_y = cross(along_y, normal), across_x = cross(normal, along_x);
        vec3 tangent = across_y * uv_x.x + across_x * uv_y.x;     // the frame from the screen slopes
        vec3 bitangent = across_y * uv_x.y + across_x * uv_y.y;
        float frame = max(dot(tangent, tangent), dot(bitangent, bitangent));

        bent.z = sqrt(max(1.0 - dot(bent.xy, bent.xy), 0.0));
#ifndef NGL_VK_FOUR_SETS
        if (skin) {     // rn32: the skin's noise swatch (set 5), for its highlights below; its normal is the map's alone
            float woven_repeat = draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + FABRIC_AT + 2u].z;

            noise = wrapped(normal_swatch, (texture_uv - 0.5) * fabric_numbers.x + 0.5).rgb;
            // [rexglue-native] 2026-11-10 rn40x: or ADC02DCE4FDC9253's (helmets and bodies with a woven swatch;
            // ngl_vk.h skin_swatch): the woven swatch (set 4) at (uv - 0.5) x its repeat + 0.5 folded in whole (the
            // uniforms' fold with the mask at 1; its instructions 11-29, both z's as sqrt(|1 - x^2 - y^2|))
            if (woven_repeat > 0.0) {
                vec4 woven = wrapped(fabric_mask, (texture_uv - 0.5) * woven_repeat + 0.5);
                vec3 swatch = vec3(2.0 * woven.a - 1.0, 2.0 * woven.g - 1.0, 0.0);

                bent.z = sqrt(abs(1.0 - dot(bent.xy, bent.xy)));
                swatch.z = sqrt(abs(1.0 - dot(swatch.xy, swatch.xy)));
                bent = vec3(bent.x * swatch.z + swatch.x * bent.z, bent.y * swatch.z + swatch.y * bent.z,
                            bent.z * swatch.z);
            }
        } else if (detail) {  // rn36: C90DF51FF5AAF4F8's instructions 39-57 (8E7F713B9E3C1FAD's 22-32 the same)
            vec4 fine = wrapped(normal_swatch, texture_uv * fabric_numbers.x);
            vec3 d = vec3(2.0 * fine.a - 1.0, 2.0 * fine.g - 1.0, 0.0);
            // the normal map's and the detail's z scales (that shader's c33 and c32)
            vec2 scales = draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT].xy;

            bent.z = sqrt(abs(1.0 - dot(bent.xy, bent.xy)));
            d.z = sqrt(abs(1.0 - dot(d.xy, d.xy)));
            bent = vec3(bent.x * scales.y * d.z + d.x * scales.x * bent.z,
                        bent.y * scales.y * d.z + d.y * scales.x * bent.z, bent.z * d.z);
        } else if (vehicle) {   // rn39: B2E0DA00's instructions 36-38 and 52 (9A3D3175's 61-63, 7D4E7CAD's 57-59):
                                // x and y times the material's scale, z from the unscaled ones, sqrt(|1 - x^2 - y^2|)
            bent.z = sqrt(abs(1.0 - dot(bent.xy, bent.xy)));
            bent.xy *= draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT].x;
            // [rexglue-native] 2026-11-10 rn46m: the props' overlay (ngl_vk.h overlay_map; the game's
            // 9960D40B0F3CECA1, its instructions 13-28, 36, 37, 43-66 and 96): METAL_AT's y its scale (0: none), z
            // its angle, the next vec4 its key color; the map in set 4. The color map x a smoothstep of the overlay
            // from the key (0) down to 0.059 (1), each of red, green and blue by itself.
            vec4 overlay_numbers = draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT];
            if (overlay_numbers.y > 0.0) {
                vec3 overlay_key = draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT + 1u].xyz;
                vec2 from_middle = (texture_uv - 0.5) * overlay_numbers.y + vec2(-0.44, 0.0);
                float overlay_sin = sin(overlay_numbers.z), overlay_cos = cos(overlay_numbers.z);
                vec2 overlay_uv = vec2(from_middle.x * overlay_cos + from_middle.y * overlay_sin + 0.94,
                                       from_middle.y * overlay_cos - from_middle.x * overlay_sin + 0.5);
                vec3 overlay_span = vec3(0.059) - overlay_key;
                overlay_span = mix(overlay_span, vec3(-1e-4), lessThan(abs(overlay_span), vec3(1e-4)));
                vec3 overlay_t = clamp((wrapped(fabric_mask, overlay_uv).rgb - overlay_key) / overlay_span, 0.0, 1.0);
                base.rgb *= overlay_t * overlay_t * (3.0 - 2.0 * overlay_t);
            }
        } else if (fabric) {  // the uniforms (3BA30872B3955C23): the woven swatch folded in, channel by channel by the mask
            vec4 woven = wrapped(normal_swatch, (texture_uv - 0.5) * fabric_numbers.x + 0.5);
            vec3 swatch = vec3(2.0 * woven.a - 1.0, 2.0 * woven.g - 1.0, 0.0);
            vec3 mask = wrapped(fabric_mask, texture_uv).rgb;

            // that shader takes both z's as sqrt(|1 - x^2 - y^2|)
            bent.z = sqrt(abs(1.0 - dot(bent.xy, bent.xy)));
            swatch.z = sqrt(abs(1.0 - dot(swatch.xy, swatch.xy)));
            bent += mask * (vec3(bent.x * swatch.z + swatch.x * bent.z, bent.y * swatch.z + swatch.y * bent.z,
                                 bent.z * swatch.z) - bent);
        }
#endif
#ifndef NGL_VK_FOUR_SETS
        // [acct2-bro] 2026-11-12: the gun metal's Normal_Map_Depth (ngl_vk.h metal_normal_depth, the w beside the world
        // matrix's second column; 0: none): the map's x and y times it, its z from the unscaled ones (687855C9's
        // instructions 21-26, CC31EB0C's 26-48)
        if (metal) {
            float metal_depth = draw_lights[DRAW_LIGHT_VEC4S * (fabric_slot - 1u) + METAL_AT + 2u].w;

            if (metal_depth > 0.0)
                bent.xy *= metal_depth;
        }
#endif
        // [acct2-bro] 2026-11-21 rn101: the game's own frame (ngl_vk.h game_bump_frame: the slot's ninth irradiance
        // row's w 1, with row 0's w 1), as CDBAC6026B525050's instructions 28-52 build it (the arms', heads',
        // helmets' and props' shaders the same; the guns' metal and the K98 from the corners' tangent the same
        // way): T along dP/du (the slopes' determinant's sign keeps it along rising u), square to the normal, and
        // B = T x N. The cotangent pair above has B along rising v: the other way round where the coordinates
        // aren't mirrored (the Garand's stamped letters and the hands' streaks came out raised)
        uint frame_slot = flags >> 16;
        uint frame_at = DRAW_LIGHT_VEC4S * (max(frame_slot, 1u) - 1u) + IRRADIANCE_AT;
        if (!lookup && frame_slot != 0u && draw_lights[frame_at].w > 0.0 && draw_lights[frame_at + 8u].w > 0.0) {
            float slopes = uv_x.x * uv_y.y - uv_y.x * uv_x.y;
            vec3 along_u = (slopes < 0.0 ? -1.0 : 1.0) * (along_x * uv_y.y - along_y * uv_x.y);

            along_u -= normal * dot(normal, along_u);
            if (dot(along_u, along_u) > 0.0) {
                tangent = normalize(along_u);
                bitangent = cross(tangent, normal);
                frame = 1.0;
            }
        }
        if (lookup) {   // [waw-decomp] 98A77B2DFA1049B4's instructions 8-12: the normal plus the map's x and y along
                        // the frame's two ways, each of unit length; no z from the map
            // [waw-decomp] 2026-11-11: the frame's two ways run against rising u and v where the slopes' cross
            // product looks away from the normal (Call of Duty 2's view: runs cod2side195 / 196), so the bends turn
            float side = dot(normal, cross(along_x, along_y)) < 0.0 ? -1.0 : 1.0;

            if (dot(tangent, tangent) > 0.0 && dot(bitangent, bitangent) > 0.0)
                normal = normalize(normal + side * (bent.x * normalize(tangent) + bent.y * normalize(bitangent)));
            lookup_sharpness = wrapped(third_texture, texture_uv).a;
        } else if (frame > 0.0)
            normal = normalize(mat3(tangent * inversesqrt(frame), bitangent * inversesqrt(frame), normal) * bent);
        if ((mode & MODE_SPECULAR) != 0u)
            gloss = wrapped(third_texture, texture_uv).rgb;
#ifndef NGL_VK_FOUR_SETS
        if (metal) {    // rn34: the gun metal's scratch map and cube (its instructions 24, 53-59, 90)
            uint at = DRAW_LIGHT_VEC4S * (fabric_slot - 1u);
            // a direction in the mesh's space into world space, one part a column of the world matrix
            vec3 column_x = draw_lights[at + METAL_AT + 1u].xyz, column_y = draw_lights[at + METAL_AT + 2u].xyz;
            vec3 column_z = draw_lights[at + METAL_AT + 3u].xyz;
            vec3 to_eye = vec3(eye_x.w, eye_y.w, eye_w.w) - object_position;
            vec3 n = vec3(dot(column_x, normal), dot(column_y, normal), dot(column_z, normal));
            vec3 v = vec3(dot(column_x, to_eye), dot(column_y, to_eye), dot(column_z, to_eye));
            // the eye's reflection about the normal, plus the game's bias (its c17.x) on each part
            vec3 r = reflect(dot(v, v) > 0.0 ? -normalize(v) : vec3(0.0, 0.0, -1.0),
                             dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0)) + draw_lights[at + METAL_AT].w;

            if (draw_lights[at + FABRIC_AT + 2u].w > 0.0)
                scratch = wrapped(fabric_mask, texture_uv).rgb;
            // the game's cube instruction takes the direction as (x, z, y) (its operands r14.yyxz and r14.zxyy)
            reflection = texture(metal_cube, vec3(r.x, r.z, r.y)).rgb;
            // [rexglue-native-guns] 2026-11-11: the game's own blur of it (ngl_vk.h metal_cube_blur; CFC45995D1294CCE's
            // instructions 59-90): the cube read with the two gradient vectors the game's shader sets, across R
            float cube_blur = draw_lights[at + FABRIC_AT + 2u].y;
            if (cube_blur > 0.0) {
                float across = max(abs(r.z), 1e-4);
                reflection = textureGrad(metal_cube, vec3(r.x, r.z, r.y),
                                         cube_blur * vec3(0.0, -r.y / across, r.z < 0.0 ? -1.0 : 1.0),
                                         cube_blur / across * vec3(r.y * r.y + r.z * r.z, -r.x * r.z, -r.x * r.y)).rgb;
            }
            // [acct2-bro] 2026-11-21 rn102: below 0, the gradients the game's shaders set (ngl_vk.h
            // metal_cube_game_gradients; CFC45995D1294CCE's 12-15 and 58-90): the base coordinates' slopes as
            // (du, dv, 0) plus the blur along A and B, two unit ways square to the read direction, each turned to
            // the slopes' side (sgt - sgt). Above, A and B came scaled by L / |d.y|: blurred without bound where
            // the reflection runs level along y, and without the slopes.
            // [acct2-bro] 2026-11-21 rn104: read at d over its longest part, as the emulator reads the game's cube
            // (its face coordinates come back as a direction whose longest part is 1, and the register gradients
            // are taken against that); the signs +1 where the dot is 0 (the game's sgt - sgt of the dot plus
            // c253.w's 1e-16: GLSL's sign(0) left such pixels unblurred).
            else if (cube_blur < 0.0) {
                float blur = -cube_blur;
                vec3 read_at = vec3(r.x, r.z, r.y);
                float level = length(read_at.yz);
                float longest = max(max(abs(read_at.x), abs(read_at.y)), abs(read_at.z));
                if (level > 0.0 && longest > 0.0) {
                    vec3 a = vec3(0.0, -read_at.z, read_at.y) / level;
                    vec3 b = vec3(level, -read_at.x * read_at.y / level, -read_at.x * read_at.z / level);
                    vec3 slope_x = vec3(uv_x, 0.0), slope_y = vec3(uv_y, 0.0);
                    float s = dot(a, slope_x) + 1e-16 > 0.0 ? 1.0 : -1.0;
                    float t = dot(b, slope_y) + 1e-16 > 0.0 ? 1.0 : -1.0;
                    reflection = textureGrad(metal_cube, read_at / longest,
                                             slope_x + blur * s * a, slope_y + blur * t * b).rgb;
                }
            }
        }
#endif
    }
#ifndef NGL_VK_FOUR_SETS
    if (lens) {     // rn40: FC767C09DBD34C2A's instructions 6-20: the cube along 2 (N.V) N - V in world space, plus c16.x
        uint at = DRAW_LIGHT_VEC4S * (lens_slot - 1u);
        vec3 column_x = draw_lights[at + METAL_AT + 1u].xyz, column_y = draw_lights[at + METAL_AT + 2u].xyz;
        vec3 column_z = draw_lights[at + METAL_AT + 3u].xyz;
        vec3 to_eye = vec3(eye_x.w, eye_y.w, eye_w.w) - object_position;
        vec3 n = vec3(dot(column_x, normal), dot(column_y, normal), dot(column_z, normal));
        vec3 v = vec3(dot(column_x, to_eye), dot(column_y, to_eye), dot(column_z, to_eye));
        vec3 r = reflect(dot(v, v) > 0.0 ? -normalize(v) : vec3(0.0, 0.0, -1.0),
                         dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0)) + draw_lights[at + METAL_AT].w;

        reflection = texture(metal_cube, vec3(r.x, r.z, r.y)).rgb;  // read at x, z, y, as the gun metal's
        // [acct2-bro] 2026-11-22 SP lane, defect E: the scopes' glass (D464FD2ECA69C5E4's 46, ngl_vk.h
        // lens_tint_map): its tint map at the color map's coordinates (the slot's first number 2; 3: none, white)
        float lens_kind = draw_lights[at + FABRIC_AT].x;
        if (lens_kind > 1.5 && lens_kind < 2.5)
            lens_tint_rgb = wrapped(fabric_mask, texture_uv).rgb;
    }
    if (world_bumped) {     // [waw-decomp] 2026-11-11: 6C7F0E21C61DF68F's instructions 4 and 9 to 13 (ngl_vk.h
                            // world_normal_map): the normal plus the map's two bends along the frame's two ways,
                            // each of unit length, as a lookup model's above
        vec4 bump = wrapped(fabric_mask, texture_uv);
        vec3 along_x = dFdx(object_position), along_y = dFdy(object_position);
        vec2 uv_x = dFdx(texture_uv), uv_y = dFdy(texture_uv);
        vec3 across_y = cross(along_y, normal), across_x = cross(normal, along_x);
        vec3 tangent = across_y * uv_x.x + across_x * uv_y.x;
        vec3 bitangent = across_y * uv_x.y + across_x * uv_y.y;
        // the frame's two ways run along rising u and v only where the slopes' cross product looks the normal's way;
        // where it looks the other way (Call of Duty 2's view of its world: runs cod2side176 to 189) they run
        // against them, so the bends are turned about
        float side = dot(normal, cross(along_x, along_y)) < 0.0 ? -1.0 : 1.0;

        if (dot(tangent, tangent) > 0.0 && dot(bitangent, bitangent) > 0.0) {
            if (world_mark < -1.5) {    // [acct2-bro] 2026-11-12: Call of Duty 3's way (ngl_vk.h world_bump_scale;
                                        // 141294CDC7C97127's instructions 17-41): z from x and y, the map's scale,
                                        // the detail normal map (set 5) mixed in, the corner's normal's share z
                vec4 numbers = draw_lights[DRAW_LIGHT_VEC4S * (detail_slot - 1u) + FABRIC_AT];
                vec2 xy = 2.0 * bump.ag - 1.0;
                float z = sqrt(max(1.0 - dot(xy, xy), 0.0));

                xy *= numbers.x;
                if (numbers.z > 0.0) {
                    vec2 detail_xy = 2.0 * wrapped(normal_swatch, texture_uv * numbers.z).ag - 1.0;
                    float detail_z = sqrt(max(1.0 - dot(detail_xy, detail_xy), 0.0));

                    xy = xy * detail_z + numbers.y * detail_xy * z;
                    z *= detail_z;
                }
                uint turned = uint(numbers.w + 0.5);    // the frame's test bits (ngl_vk.h world_bump_frame)
                if ((turned & 4u) != 0u)
                    xy = xy.yx;
                if ((turned & 1u) != 0u)
                    xy.x = -xy.x;
                if ((turned & 2u) != 0u)
                    xy.y = -xy.y;
                normal = normalize(side * (xy.x * normalize(tangent) + xy.y * normalize(bitangent)) + z * normal);
            } else {
                normal = normalize(normal + side * ((2.0 * bump.a - 1.0) * normalize(tangent) +
                                                    (2.0 * bump.g - 1.0) * normalize(bitangent)));
            }
        }
    }
#endif

    if ((mode & MODE_SHINY) != 0u) {    // the same for every pixel of a draw, so the lookups' derivatives are sound
        vec3 toward_eye = vec3(dot(eye_x.xyz, normal), dot(eye_y.xyz, normal), dot(eye_w.xyz, normal));
        vec2 at = dot(toward_eye, toward_eye) > 0.0 ? normalize(toward_eye).xy : vec2(0.0);

        shine = texture(lightmap_texture, vec2(0.5) + vec2(0.5, -0.5) * at).rgb;
        if ((mode & MODE_MASKED) != 0u)
            shine *= wrapped(third_texture, texture_uv).rgb;
    }

#ifdef NGL_VK_MSAA_COVERAGE
    // [acct2-bro] 2026-11-22 rn113b: under MSAA a cut-out's edge (leaves, grass cards, fences, wires) covers a share
    // of the pixel's samples by how far its alpha is past the test, over the alpha's change across the pixel (alpha
    // to coverage, sharpened to the test's edge), turned by the pixel's place so a half-covered run doesn't take the
    // same samples throughout; the resolve then blends the edge with what is behind it, steady as the view moves.
    // A pixel with any share passes the test below; the alpha written is the cut-out's own (at least the test's).
    if ((tint & ALPHA_TEST) != 0u && MSAA_SAMPLES > 1) {
        float cover = clamp((alpha - cut_edge) / max(alpha_width, 1.0 / 255.0) + 0.5, 0.0, 1.0);  // rn115a
        int count = int(cover * float(MSAA_SAMPLES) + 0.5);
        int turn = (int(gl_FragCoord.x) + 3 * int(gl_FragCoord.y)) & (MSAA_SAMPLES - 1);
        int taken = ((1 << count) - 1) << turn;

        if (count <= 0)
            discard;
        gl_SampleMask[0] = (taken | (taken >> MSAA_SAMPLES)) & ((1 << MSAA_SAMPLES) - 1);
        alpha = max(alpha, own_cut ? cut_edge + 1.0 / 255.0 : 129.0 / 255.0);  // rn115a: past the edge
    }
#endif
    if ((tint & ALPHA_TEST) != 0u && alpha <= cut_edge)    // rn115a: the edge above
        discard;
    // [acct2-bro] 2026-11-13 rn67a: seen from one side only (TINT_ONE_SIDED, see there)
    if ((tint & TEXTURED) != 0u && shading == SHADE_LIGHT && (tint & TINT_ONE_SIDED) != 0u &&
        gl_FrontFacing == ((tint & TINT_OTHER_SIDE) == 0u))
        discard;

    float doubled = (tint & LIGHT_ONCE) != 0u ? 1.0 : 2.0;

    if ((mode & MODE_MIX) != 0u) {
        base.rgb = mix(base.rgb, second.rgb, vertex_color.a * ((mode & MODE_BY_ALPHA) != 0u ? second.a : 1.0));
        light = vec3(1.0);
    } else if (shading == SHADE_LIGHTMAP) {
        if ((mode & MODE_LIGHTMAP_SUN) != 0u) {     // Call of Duty 3's world (ngl_vk.h lightmap_scale)
            vec3 up = unpacked_toward(light_toward[1]);
            vec3 toward = unpacked_toward(light_toward[0]);
            float facing_up = dot(up, up) > 0.0 ? dot(normal, normalize(up)) : 1.0;
            float facing_sun = dot(toward, toward) > 0.0 ? max(dot(normal, normalize(toward)), 0.0) : 0.0;

            vec3 nearby = nearby_light(flags >> 16, normal, normal, vec2(0.0));

            light = ambient.r * (second.rgb * (1.0 + ambient.g * facing_up) +
                                 unpacked_color(light_colors[0]) * second.a * facing_sun * sun_reaching() + nearby);
#ifndef NGL_VK_FOUR_SETS
            if (world_lookup) {     // [waw-decomp] 6C7F0E21C61DF68F's instructions 14, 17, 29 to 31 and 39: the
                                    // specular map's color x the highlights' color x the lookup at (the map's alpha,
                                    // N.H) x mix(the shadowed share, 1, the sun's share x the sun's reach)
                uint at = DRAW_LIGHT_VEC4S * ((flags >> 16) - 1u);
                vec3 to_eye = vec3(eye_x.w, eye_y.w, eye_w.w) - object_position;

                if (dot(toward, toward) > 0.0 && dot(to_eye, to_eye) > 0.0) {
                    vec4 specular = wrapped(third_texture, texture_uv);
                    float facing = clamp(dot(normal, normalize(normalize(toward) + normalize(to_eye))), 0.0, 1.0);

                    // [waw-decomp] 2026-11-11: with a normal map in set 4 the lookup is the cube's +z face
                    // (ngl_vk.h world_tables), read inside the face: half a texel of 256 from its edges
                    vec2 in_face = clamp(vec2(specular.a, facing), 0.5 / 256.0, 1.0 - 0.5 / 256.0);
                    vec3 table = world_bumped
                        ? textureLod(metal_cube, vec3(2.0 * in_face.x - 1.0, 1.0 - 2.0 * in_face.y, 1.0), 0.0).rgb
                        : textureLod(fabric_mask, vec2(specular.a, facing), 0.0).rgb;

                    shine += specular.rgb * draw_lights[at + FABRIC_AT].rgb * table *
                             mix(draw_lights[at + FABRIC_AT + 1u].x, 1.0, second.a * sun_reaching());
                }
            }
#endif
        } else {
            light = doubled * second.rgb;
        }
    } else if (shading == SHADE_VERTEX_COLOR) {
        light = doubled * vertex_color.rgb;
        alpha *= vertex_color.a;
    } else if (shading == SHADE_UNLIT) {
        light = vertex_color.rgb;
        alpha *= effect && (tint & TINT_MAP_ALPHA) != 0u ? 1.0 : vertex_color.a;   // rn41q: the map's alone
    } else if ((flags & SCENE_LIGHT) != 0u) {
        uint count = 1u + ((flags >> MORE_LIGHTS_SHIFT) & 3u);
        // rn41x: the SpeedTree leaves (51732ACF1B6A87C5's light, 6189945E43D7FBC8 reads no shadow): the lights x the
        // corners' red (the leaf's dimming), the sun not shadowed
        bool dimmed = (tint & TEXTURED) != 0u && (tint & TINT_LIGHT_DIMMED) != 0u;
        // rn46c: SpeedTree's branches and fronds: the lights x the self-shadow map, the sun not shadowed
        bool self_shadowed = (tint & TEXTURED) != 0u && (tint & TINT_LIGHT_MAPPED) != 0u;
        vec3 self_shadow = self_shadowed ? texture(lightmap_texture, lightmap_texture_uv).rgb : vec3(1.0);
        float sun = dimmed || self_shadowed ? 1.0 : sun_reaching();  // the first light is the sun
        uint slot = flags >> 16;
        vec3 first_light = unpacked_color(light_colors[0]) * sun;

        // rn30: the third-person uniforms (21168F53BCF9D156, its instructions 152-157): the shadowed sun eased toward 1
        // with the view depth (COD_SHADOW_FARZ), and never under COD_SHADOW_GREYLEVEL
        if (fabric && fabric_floor > 0.0)
            first_light = max(mix(first_light, vec3(1.0), clamp(fabric_fade / gl_FragCoord.w, 0.0, 1.0)),
                              vec3(fabric_floor));
        light = has_irradiance(slot) ? irradiance(slot, normal) : ambient;
        if (has_irradiance(slot) && !fabric && !lens && !water)     // [waw-decomp] the rows' third-degree term
            light = max(light + irradiance_cubic(slot, normal), vec3(0.0));
#ifndef NGL_VK_FOUR_SETS
        if (!fabric && !lens && !water && has_grid(slot))   // [waw-decomp] Call of Duty 2's light grid instead
            light = grid_light(slot, normal);
#endif
        for (uint i = 0u; i < count; i++) {
            vec3 toward = unpacked_toward(light_toward[i]);

            if (dot(toward, toward) > 0.0)    // rn40y: the sun also by the skin's terminator (1 for others)
                light += max(dot(normal, normalize(toward)), 0.0) * (dimmed ? vertex_color.r : 1.0) * self_shadow *
                         (i == 0u ? first_light * terminator(surface_normal, toward, skin_terminator)
                                  : unpacked_color(light_colors[i]));
        }
        light += nearby_light(slot, normal, surface_normal, skin_terminator);
        if ((flags & HELD) != 0u && !k98)   // [acct2-bro] 2026-11-15 rn99a: the K98 shader doesn't hold it
            light = min(light, vec3(1.0));
        if (lens) {     // rn40: FC767C09DBD34C2A's instructions 22-58 (its sun is the pass's own, unshadowed); rn40e:
                        // or 5931940F174EA055's 52-100 (the eyes: opaque, the sun shadowed as the treads')
            uint at = DRAW_LIGHT_VEC4S * (slot - 1u);
            // (1 for the eyes, the reflection's scale (c31.x; the eyes' 0.072696), the power (300; 500), 0)
            vec4 numbers = draw_lights[at + FABRIC_AT];
            vec4 sun_floor = draw_lights[at + FABRIC_AT + 1u];      // the sun's color (c30; c35), the shadow's floor
            float fade = draw_lights[at + FABRIC_AT + 2u].x;        // the shadow's fade with the view depth
            vec3 tint = vec3(draw_lights[at + METAL_AT + 1u].w, draw_lights[at + METAL_AT + 2u].w,
                             draw_lights[at + METAL_AT + 3u].w);    // the eyes' (0.882, 1, 0.990363); 1 1 1
            vec3 sun_color = sun_floor.rgb;
            vec3 to_sun = draw_lights[at + METAL_AT].xyz - object_position;     // c29 (c34), in the mesh's space
            vec3 to_eye = vec3(eye_x.w, eye_y.w, eye_w.w) - object_position;
            vec3 eye = normalize(eye_w.xyz) * (dot(eye_w.xyz, face_normal) < 0.0 ? -1.0 : 1.0);
            vec3 sun_way = dot(to_sun, to_sun) > 0.0 ? normalize(to_sun) : vec3(0.0);

            if (sun_floor.w > 0.0)  // 5931940F's 52, 89-92: max(mix(color x shadow, 1, sat(fade x depth)), floor)
                sun_color = max(mix(sun_color * sun_reaching(), vec3(1.0), clamp(fade / gl_FragCoord.w, 0.0, 1.0)),
                                vec3(sun_floor.w));
            eye = dot(to_eye, to_eye) > 0.0 ? normalize(to_eye) : eye;
            light = (has_irradiance(slot) ? irradiance(slot, normal) : ambient) +
                    clamp(dot(normal, sun_way), 0.0, 1.0) * sun_color + nearby_light(slot, normal, normal, vec2(0.0));
            vec3 highlights = nearby_highlight(slot, normal, eye, numbers.z);

            if (dot(sun_way, sun_way) > 0.0)
                highlights += sun_color * pow(clamp(dot(normal, normalize(sun_way + eye)), 0.0, 1.0), numbers.z);
            if (numbers.x > 1.5) {  // [acct2-bro] 2026-11-22 SP lane, defect E: the scopes' glass (D464FD2E's
                                    // 49-54): (the color map x c252.x x the light + the tint map x c255.xyz x (the
                                    // cube x c255.w + the highlights)) x c254.zyx, its alpha x c252.x
                vec4 out_numbers = draw_lights[at + FABRIC_AT + 2u];    // (out r, out g, 1: a cube, out b)
                vec3 out_scale = vec3(out_numbers.x, out_numbers.y, out_numbers.w);

                shine += lens_tint_rgb * tint * (reflection * numbers.y + highlights) * out_scale;
                base.rgb *= numbers.w * out_scale;
                alpha = clamp(alpha * numbers.w, 0.0, 1.0);
            } else if (numbers.x > 0.0) {  // the eyes: (highlights + the cube x 0.072696) x the tint, opaque
                shine += (highlights + reflection * numbers.y) * tint;
                alpha = 1.0;
                if (numbers.w > 0.5) {  // rn40m: a check (ngl_vk.h lens_show): 1 the light alone, 2 the color map alone,
                                        // rn40n: 3 magenta (where the eyes are drawn)
                    shine = vec3(0.0);
                    if (numbers.w < 1.5) {
                        base.rgb = vec3(1.0);
                    } else if (numbers.w < 2.5) {
                        light = vec3(1.0);
                    } else {
                        base.rgb = vec3(1.0, 0.0, 1.0);
                        light = vec3(1.0);
                    }
                }
            } else {                // the glass: x (1 - alpha) x c31.x, the color map x alpha
                shine += (highlights + reflection) * (1.0 - alpha) * numbers.y;
                base.rgb *= alpha;
            }
        } else if ((mode & MODE_SPECULAR) != 0u && metal) {    // rn34: the gun metal (CFC45995D1294CCE, instructions 48-115)
            uint at = DRAW_LIGHT_VEC4S * (slot - 1u);
            vec4 numbers = draw_lights[at + FABRIC_AT];         // texture_brightness, Reflection_Amt, SpecularPower
            vec4 hue_edge = draw_lights[at + FABRIC_AT + 1u];   // Metal_Hue, the rim's edge
            float rim_scale = draw_lights[at + FABRIC_AT + 2u].x;
            vec3 rim = draw_lights[at + METAL_AT].rgb;
            vec3 toward = unpacked_toward(light_toward[0]);
            vec3 to_eye = vec3(eye_x.w, eye_y.w, eye_w.w) - object_position;
            vec3 eye = normalize(eye_w.xyz) * (dot(eye_w.xyz, face_normal) < 0.0 ? -1.0 : 1.0);

            eye = dot(to_eye, to_eye) > 0.0 ? normalize(to_eye) : eye;     // toward the eye's own place
            // the rim: t = saturate(3.546 x (0.337 - N.V)), s = t^2 (3 - 2t), the base toward a blue-grey
            float t = clamp(rim_scale * (hue_edge.w - dot(normal, eye)), 0.0, 1.0);
            vec3 mask = gloss + scratch + mix(base.rgb, rim, t * t * (3.0 - 2.0 * t));
            // [acct2-bro] 2026-11-11: 687855C9BA600FC3's mask (the Thompson; ngl_vk.h metal_mask_floor, the w beside
            // the world matrix's first column): a grey floor and the rim added to it, not the base turned to the rim
            float mask_floor = draw_lights[at + METAL_AT + 1u].w;
            if (mask_floor > 0.0)
                mask = gloss + scratch + vec3(mask_floor) + rim * (t * t * (3.0 - 2.0 * t));
            // the highlights: the sun's (its color x the shadow) and the point lights' (x their falloff), no N.L
            vec3 highlights = nearby_highlight(slot, normal, eye, numbers.z);

            if (dot(toward, toward) > 0.0)
                highlights += first_light * pow(max(dot(normal, normalize(normalize(toward) + eye)), 0.0), numbers.z);
            shine += (numbers.x * highlights + numbers.y * reflection) * mask;
            base.rgb *= hue_edge.rgb;
            light *= numbers.x;
#ifndef NGL_VK_FOUR_SETS
        } else if ((mode & MODE_SPECULAR) != 0u && lookup) {
            // [waw-decomp] 98A77B2DFA1049B4's instructions 13-17, 29-33 and 39: the specular map's color x the
            // highlights' color x the lookup at (the map's alpha, N.H) x mix(the shadowed share, 1, the sun's reach)
            uint at = DRAW_LIGHT_VEC4S * (slot - 1u);
            vec3 toward = unpacked_toward(light_toward[0]);
            vec3 to_eye = vec3(eye_x.w, eye_y.w, eye_w.w) - object_position;

            if (dot(toward, toward) > 0.0 && dot(to_eye, to_eye) > 0.0) {
                float facing = clamp(dot(normal, normalize(normalize(toward) + normalize(to_eye))), 0.0, 1.0);

                shine += gloss * draw_lights[at + FABRIC_AT].rgb *
                         textureLod(fabric_mask, vec2(lookup_sharpness, facing), 0.0).rgb *
                         mix(draw_lights[at + FABRIC_AT + 1u].x, 1.0, sun);
            }
#endif
        } else if ((mode & MODE_SPECULAR) != 0u) {     // the first light's highlight (power 30), toward the eye
            vec3 toward = unpacked_toward(light_toward[0]);
            vec3 eye = normalize(eye_w.xyz) * (dot(eye_w.xyz, face_normal) < 0.0 ? -1.0 : 1.0);
            float power = 30.0;

            if (fabric) {   // [rexglue-native] the uniforms (3BA30872B3955C23), toward the eye's own place
                vec3 to_eye = vec3(eye_x.w, eye_y.w, eye_w.w) - object_position;
                float edge;

                eye = dot(to_eye, to_eye) > 0.0 ? normalize(to_eye) : eye;
                power = fabric_numbers.z;
                if (skin) {     // rn32: 3949D29E09A78AA1 (its instructions 68, 104, 112, 124-125, 135, 138)
                    float facing = clamp(dot(normal, eye), 0.0, 1.0);

                    // 1.215 x the base where the surface is seen edge-on, the base itself where it faces the eye
                    base.rgb *= mix(fabric_sheen.x, 1.0, facing * facing * (3.0 - 2.0 * facing));
                    // [acct2-bro] 2026-11-12 rn60a: and times the draw's own tint, when it has one (the soldiers'
                    // simpler bodies: D2B5785887F9C326's c255.xyz on its color map, its instructions 86-101)
                    if (dot(skin_tint, skin_tint) > 0.0)
                        base.rgb *= skin_tint;
                    // the highlights' mask: 0.4 x the specular map, 0.38 of the way to the noise swatch
                    gloss = mix(fabric_sheen.y * gloss, noise, fabric_sheen.z);
                } else if (!detail && !vehicle) {   // (rn36, rn39: the vehicles' metal keeps its base as it is)
                    // the sheen: 0.51 x the base lifted toward pow(base, sheen) as the view turns edge-on (rn30: the
                    // third-person uniforms' scale is 1)
                    edge = clamp(1.0 - dot(normal, eye), 0.0, 1.0);
                    base.rgb = fabric_numbers.y * mix(base.rgb, pow(max(base.rgb, vec3(0.0)), fabric_sheen),
                                                      edge * edge * (3.0 - 2.0 * edge));
                }
                if (k98) {  // [acct2-bro] 2026-11-15 rn99a: BDA4BB849BDF0C39's instructions 25-27, 53, 58-67: the
                            // specular map x c30; the base x mix(c29, 1, smoothstep(clamp((N.E - 0.29) x -4.1841)))
                    float k98_t = clamp((dot(normal, eye) - 0.29) * -4.1841, 0.0, 1.0);

                    gloss *= k98_specular;
                    base.rgb *= mix(k98_numbers.rgb, vec3(1.0), k98_t * k98_t * (3.0 - 2.0 * k98_t));
                }
                shine += gloss * nearby_highlight(slot, normal, eye, power);
            }
            if (dot(toward, toward) > 0.0)
                shine += gloss * first_light * pow(max(dot(normal, normalize(normalize(toward) + eye)), 0.0), power);
        }
    } else {
        vec3 toward = unpacked_toward(light_toward[0]);

        toward = dot(toward, toward) > 0.0 ? normalize(toward) : vec3(0.0);
        light = vec3(0.35 + 0.65 * abs(dot(normal, toward)) * sun_reaching());
    }
    // Big Red One's shadow projector (cdWorld_Draw, cdWorldBlend_Draw; Call of Duty 3's lightmapped world takes the
    // shadow in its sun's part instead, above): a lightmapped or mixed draw marked shadowed is darkened by half where
    // the casters' shadow falls
    if (((mode & MODE_MIX) != 0u || shading == SHADE_LIGHTMAP) && (mode & MODE_LIGHTMAP_SUN) == 0u &&
        (flags & SHADOWED) != 0u)
        light *= 0.5 + 0.5 * sun_reaching();
    if ((mode & MODE_THIRD) != 0u)
        light *= third;
    if ((flags & CORNER_ALPHA) != 0u)       // the world's second layer: mixed in by its corners' weight
        alpha = vertex_color.a;
    if ((flags & SOFT) != 0u) {     // the game's soft particles: by how far the opaque meshes behind are, in depth
        float behind = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
        // [rexglue-native] 2026-11-10 rn42j: a negative scale says the lightmap v holds the draw's own depth, blended
        // from its corners as the game's effect shaders do (their o0.z), in place of the pixel's depth
        float scale = lightmap_texture_uv.x, own = gl_FragCoord.z;
        if (scale < 0.0) {
            scale = -scale;
            own = lightmap_texture_uv.y;
        }
        alpha *= clamp((behind - own) * scale, 0.0, 1.0);
    }
#ifndef NGL_VK_FOUR_SETS
    if (detail_add) {   // [waw-decomp] 6C7F0E21C61DF68F's instructions 19, 27 and 33: the color map + the detail map
                        // at the coordinates x its repeat + its offset (-0.5), before the light multiplies it
        vec4 numbers = draw_lights[DRAW_LIGHT_VEC4S * ((flags >> 16) - 1u) + FABRIC_AT + 1u];

        base.rgb = max(base.rgb + wrapped(normal_swatch, texture_uv * numbers.yz).rgb + numbers.w, vec3(0.0));
    }
#endif
    if (lookup)     // [waw-decomp] 2026-11-10: 98A77B2DFA1049B4's instruction 38, the color map x the corners' color
        base.rgb *= vertex_color.rgb;
    if (cod3_water) {   // [acct2-bro] 2026-11-13 rn86a: Call of Duty 3's water (cod3_water_part): its own color in
                        // the world's light where the sky doesn't show, the sky and the glint where it does
        uint at = DRAW_LIGHT_VEC4S * (lens_slot - 1u);
        float given = draw_lights[at + METAL_AT + 2u].w;    // [acct2-bro] 2026-11-13 rn86e: negative: rn86a's
        float opacity = abs(given);

        base.rgb = vec3(1.0);
        light = draw_lights[at + FABRIC_AT].rgb * light * (1.0 - cod3_part.a);
        shine = cod3_part.rgb;
        // [acct2-bro] 2026-11-13 rn86e: the game's alpha, 1 - (1 - a) x b with b its Fresnel-like term (nearly
        // opaque looking down); rn86a's opacity + (1 - opacity) x f when the opacity came negative
        alpha = given < 0.0 ? opacity + (1.0 - opacity) * cod3_part.a : 1.0 - (1.0 - opacity) * cod3_part.a;
    } else if (water) {    // [waw-decomp] the water's own color (water_color), its alpha its corners' alone
        base.rgb = vec3(1.0);
        light = water_rgb;
        shine = vec3(0.0);
        alpha = vertex_color.a;
    }
    if (shiny_numbers.w > 0.0) {    // [acct2-bro] 2026-11-22 rn111: (light x base + strength x shine) x tint
        light *= shiny_numbers.rgb;
        shine *= shiny_numbers.w * shiny_numbers.rgb;
    }
    if (k98) {  // [acct2-bro] 2026-11-15 rn99a: its literal c254.z on the light and the highlights (its 36-37, 45, 73)
        light *= k98_numbers.w;
        shine *= k98_numbers.w;
    }
    color = vec4(min(base.rgb * light + shine, vec3(1.0)), alpha);
    if (shading != SHADE_UNLIT)             // the sky and the background aren't fogged
        color.rgb = mix(color.rgb, fog_rgb, clamp(fog_amount, 0.0, 1.0));
}
