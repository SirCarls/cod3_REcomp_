// [acct2-bro] 2026-11-22 Camo lane: the camo pass's corners (ngl_vk.h camo_texture; src/ngl_vk.c draw_camo). Placed
// as mesh.vert places a draw's corners: the same world-view-projection, which push_mesh_draw has just pushed for the
// draw this pass goes over (Direct3D's row-vector matrix read into a GLSL mat4 as its transpose, so matrix * vector
// here is Direct3D's vector * matrix; y turned over for Vulkan's clip space). The camo's coordinates: the base
// texture's x scale + offset, not wrapped (camo.frag wraps them); the strength and the camo's mean color passed on.
#version 450

layout(location = 0) in vec3 position;      // the mesh's own space
layout(location = 1) in vec2 uv;            // the base texture's coordinates

layout(push_constant) uniform Camo {
    mat4 world_view_projection;     // mesh.vert's, as push_mesh_draw left it
    vec4 scale_offset_strength;     // the pattern's scale, its offset u and v, the strength (draw_camo checked them)
    vec4 mean;                      // the camo's mean red, green and blue (each at least 1/255), unused
} camo;

layout(location = 0) out vec2 camo_uv;
layout(location = 1) flat out vec4 strength_mean;

void main()
{
    gl_Position = camo.world_view_projection * vec4(position, 1.0);
    gl_Position.y = -gl_Position.y;         // as mesh.vert: Direct3D's clip space has y up, Vulkan's down
    camo_uv = uv * camo.scale_offset_strength.x + camo.scale_offset_strength.yz;
    strength_mean = vec4(camo.scale_offset_strength.w, camo.mean.rgb);
}
