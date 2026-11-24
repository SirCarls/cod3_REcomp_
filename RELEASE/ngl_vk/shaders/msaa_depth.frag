// NGL on Vulkan: [acct2-bro] 2026-11-22 rn113a: with MSAA (ngl_vk_set_msaa), the opaque half's multisampled depth
// into the one-sample depth (src/ngl_vk.c msaa_resolve_depth), which copy_soft_depth then copies for the soft effects
// and the effect layer as before. Each pixel takes its first sample's depth: the depth a one-sample picture has there.
#version 450

layout(set = 0, binding = 0) uniform sampler2DMS depth_samples;

void main()
{
    gl_FragDepth = texelFetch(depth_samples, ivec2(gl_FragCoord.xy), 0).r;
}
