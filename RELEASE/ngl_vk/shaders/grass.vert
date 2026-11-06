// NGL on Vulkan: Call of Duty 3's grass, its vertex shader translated instruction for instruction (the game's
// microcode as the SDK's dump_shaders disassembles it, shader_4B4E6FDA02FB39C3; dev/agents/acct2-bro/NOTES.md,
// 2026-11-05 16:02). Each blade is a strip of seven vertices around one root on a 100-unit patch; the patch's color
// texture (the game's tf17) and cell texture (tf18) are read at the root, a blade is kept only where the density is
// above 0, raised to the cell's height, bent by a sine of time (the wind) and colored. The registers keep the
// shader's names (r0..r7, c0..c95); c12..c15 hold the view-projection the meshes use, as Direct3D's columns.
#version 450

layout(location = 0) in vec2 position;      // 16_16 signed: the root on the patch, -1..1
layout(location = 1) in vec4 shape;         // 8_8_8_8 signed: x the side and height along the blade, yzw its look
layout(location = 2) in vec4 variety;       // 8_8_8_8 signed: x the wind's phase, y the height, z w the color

layout(set = 0, binding = 0) uniform sampler2D color_texture;   // tf17
layout(set = 1, binding = 0) uniform sampler2D cell_texture;    // tf18
layout(set = 3, binding = 0) uniform Constants {
    vec4 c[96];
    uvec4 flags;            // x: the game's b0 (blades bent away from c42)
} k;

layout(push_constant) uniform Patch {
    vec4 c84;               // the patch's place in its cell's textures (xy) and in the world (zw)
    vec4 c85;               // the cell's lowest height (x) and its height range (y)
} patch_;

layout(location = 0) out vec4 color;        // o0
layout(location = 1) out vec2 blade_uv;     // o1

// Xenos multiplies the Direct3D 9 way: zero times anything is zero, infinity and NaN included. It matters where
// the shader multiplies by a reciprocal square root that can be infinite: a blade whose shape is zero (r6.xy zero,
// r6.w = rsq(0) infinite) would be NaN under IEEE's 0 * inf. The game's blades never are (a tip's shape x is 1/127),
// so this only keeps the GPU's rule; side71-73: the shards came from reading the wrong vertex buffer.
float xmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }

// the shader's literal constants
const vec4 c252 = vec4(0.0, 0.0, 0.0, 0.4);
const vec4 c253 = vec4(-1.0, 1.0, 2.0, 0.5);
const vec4 c254 = vec4(-0.5, 0.159155, 3.14159, 6.28319);
const vec4 c255 = vec4(0.05, -0.05, 50.0, 1.5);

void main()
{
    vec4 r0, r1, r2 = vec4(0.0), r3 = vec4(0.0), r4, r5 = vec4(0.0), r6 = vec4(0.0), r7 = vec4(0.0);
    vec4 c84 = patch_.c84, c85 = patch_.c85;
    float t;

    r2.xy = position.yx;                                    // vfetch r2.yx__
    r1 = variety;
    r0 = shape;
    r3.xy = r2.xy * c255.xy + c84.xy;                       // 10
    r2.xy = r2.xy * c255.zz + c84.zw;                       // 11
    r5.x = abs(r1.x) * k.c[40].x + k.c[80].x;               // 12
    r5.y = k.c[80].z;
    r5.w = c254.y * r5.x - c253.w;                          // 13
    r6.z = 0.0;
    r1.y = r1.y * k.c[36].x + r5.y;                         // 14
    r5.x = fract(r5.w);
    r4 = textureLod(color_texture, r3.xy, 0.0);             // 15
    t = 0.0;
    {
        vec4 cell = textureLod(cell_texture, r3.xy, 0.0);   // 16: r3.xy_w
        r3.x = cell.x;
        r3.y = cell.y;
        r3.w = cell.w;
    }
    r7.z = r3.x * k.c[28].x - k.c[29].x;                    // 17
    r3.z = r7.z * r4.w;                                     // 18
    r5.y = clamp(k.c[82].x * r7.z, 0.0, 1.0);
    r6.xy = r0.xx * r0.zw;                                  // 19
    if (!(r3.z > 0.0)) {                                    // setp_gt: no blade here
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);             // (left to clipping, behind the far plane)
        color = vec4(0.0);
        blade_uv = vec2(0.0);
        return;
    }
    r2.z = r3.w * c85.y + c85.x;                            // 20
    r5.x = r5.x * c254.w - c254.z;                          // 21
    r5.z = -c254.x;
    t = dot(r6.xyz, r6.xyz);                                // 22
    r5.x = sin(r5.x);
    r5.w = t;
    t = r5.x * c253.w + r5.z;                               // 23
    r6.w = inversesqrt(r5.w);
    r5.x = t;
    t = clamp(c253.w + r3.x, 0.0, 1.0);                     // 24
    r5.x = r5.x * r5.y;
    r3.w = t;
    r5.x = r5.x * k.c[80].y + k.c[38].x;                    // 25
    t = xmul(r6.x, r6.w);                                   // 26
    r5.xyz = r5.xxx * k.c[37].xzy;
    r7.x = t;
    r6.z = r3.y * k.c[81].y + k.c[81].x;                    // 27
    t = xmul(r6.y, r6.w);                                   // 28
    r3.y = r0.y * k.c[34].x + r6.z;
    r7.y = t;
    if (k.flags.x != 0u) {                                  // cexec b0: bent away from c42
        r6.xyz = -r2.xyz + k.c[42].xzy;                     // 29
        r3.z = dot(r6.xyz, r6.xyz);                         // 30
        t = inversesqrt(r3.z);                              // 31
        r6.xyz = r6.xyz * k.c[43].x;
        r6.w = t;
        t = 1.0 / r3.z;                                     // 32
        r6.xyz = r6.xyz * r6.w;
        r3.z = t;
        r5.xyz = r6.xyz * -r3.z + r5.xyz;                   // 33
    }
    t = -r7.z;                                              // 34
    r6.x = r7.x > 0.0 ? r7.y : -r7.y;
    r6.z = t;
    r6.y = r7.y > 0.0 ? r7.x : -r7.x;                       // 35
    t = k.c[82].y * r3.w;                                   // 36
    r6.xy = r6.xy * r7.z;
    r3.w = t;
    r5.xyz = r6.xyz * r1.y + r5.xyz;                        // 37
    r5.xyz = r5.xyz * r4.w + r6.xyz;                        // 38
    t = r3.w * r3.y;                                        // 39
    r5.z = r5.z + r7.z;
    r7.w = t;
    t = abs(c253.y) - abs(r0.x);                            // 40
    r5.w = dot(r5.xyz, r5.xyz);
    r0.w = t;
    t = inversesqrt(r5.w);                                  // 41
    r5.xyz = r5.xyz * r7.z;
    r5.w = t;
    t = log2(r0.w);                                         // 42
    r5.xyz = r5.xyz * r5.w;
    r4.w = t;
    r1.xyz = r1.z > 0.0 ? k.c[24].xyz : k.c[25].xyz;       // 43
    r1.xyz = r1.www * k.c[26].x + r1.xyz;                   // 44
    t = xmul(r7.x, r7.w);                                   // 45
    r4.xyz = r4.xyz * r1.xyz;
    r7.x = t;
    t = xmul(r7.y, r7.w);                                   // 46
    r4.xyz = k.c[27].xyz * r4.xyz;
    r7.y = t;
    r3.xyz = r4.xyz * k.c[80].w + k.c[44].xyz;              // 47
    r7.w = 0.0;
    r2.xy = r2.xy + r7.xy;                                  // 48
    r1.xyz = vec3(r7.w, r7.w, r7.z) * -c253.w + r5.xyz;     // 49
    r5.w = 0.0;
    r1.xyz = r1.xyz * -k.c[47].x - vec3(r5.w, r5.w, r5.z);  // 50
    r1.xyz = r5.xyz * c255.w + r1.xyz;                      // 51
    r6.xyz = r1.xyz + vec3(r7.x, r7.y, r7.w);               // 52
    t = xmul(k.c[46].x, r4.w);                              // 53
    r6.xyz = r6.xyz * -r0.w;
    r4.w = t;
    t = exp2(r4.w);                                         // 54
    r6.xyz = r6.xyz + vec3(r5.x, r5.y, r5.w);
    r4.w = t;
    t = c253.y - r4.w;                                      // 55
    r6.xyz = r6.xyz + r1.xyz;
    r4.w = t;
    t = r5.w >= 0.0 ? 1.0 : 0.0;                            // 56
    r6.xyz = vec3(r6.z, r6.x, r6.y) * r0.w + vec3(r5.z, r5.w, r5.w);
    r1.w = t;
    r1.xyz = vec3(r6.y, r6.z, r6.x) * r0.w + r2.xyz;        // 57

    gl_Position = vec4(dot(r1, k.c[12]), dot(r1, k.c[13]), dot(r1, k.c[14]), dot(r1, k.c[15]));   // 58-61
    gl_Position.y = -gl_Position.y;         // Direct3D's clip space has y up, Vulkan's down; z is 0..1 in both
    color = vec4(r3.xyz * r4.w + r4.xyz, 1.0);              // 62
    blade_uv = vec2(r0.x * c253.w + c253.w, r0.w * c253.x + c253.y);   // 63
}
