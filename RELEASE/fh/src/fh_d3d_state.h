/* [waw-decomp] Finest Hour port, phase 2: the Direct3D device's state as the game sets it, and its draws. See
 * fh_d3d_state.c.
 *
 * Each function below is the native answer of one row of fh/config/stand_in_functions.tsv (its stand_in_return
 * column), called by the generated stand-in with the arguments as the game passed them. Each keeps what the game
 * gives it and returns 0 (D3D_OK) as the phase 1 stand-in did. Names and argument meanings are what each function's
 * own code does with them (the table's problems column), checked again by the draw census (FH_DRAW_LOG). */
#pragma once
#include <stdint.h>

#define FH_D3D_STREAMS          16
#define FH_D3D_STAGES           4
#define FH_D3D_RENDER_STATES    256
#define FH_D3D_STAGE_STATES     32
#define FH_D3D_TRANSFORMS       16
#define FH_D3D_LIGHTS           8       /* [fh7] the lights the device can have on at once */
#define FH_D3D_LIGHT_WORDS      26      /* [fh7] a D3DLIGHT8: 104 bytes */
#define FH_D3D_MATERIAL_WORDS   17      /* [fh7] a D3DMATERIAL8: 68 bytes */

/* What the game has set, as the device would hold it. Guest addresses are 32-bit Xbox addresses. */
struct fh_d3d_state {
    struct {
        uint32_t buffer;                /* a D3DVertexBuffer, or 0 */
        uint32_t stride;                /* bytes from one vertex to the next */
    } streams[FH_D3D_STREAMS];
    uint32_t vertex_shader;             /* SetVertexShader's handle: bit 0 set for a program, else an FVF code */
    uint32_t program_address;           /* [acct2-fh] SelectVertexShader's Address: where the program starts */
    uint8_t program_selected;           /* 1 after SelectVertexShader, 0 after SetVertexShader: the draws then run
                                         * the program memory's program from program_address (fh_vsh_selected) */
    uint32_t textures[FH_D3D_STAGES];   /* a D3DBaseTexture per stage, or 0 */
    uint32_t palettes[FH_D3D_STAGES];   /* a D3DPalette per stage, or 0 */
    uint32_t render_states[FH_D3D_RENDER_STATES];
    uint8_t render_state_set[FH_D3D_RENDER_STATES];
    uint32_t stage_states[FH_D3D_STAGES][FH_D3D_STAGE_STATES];
    uint8_t stage_state_set[FH_D3D_STAGES][FH_D3D_STAGE_STATES];
    float transforms[FH_D3D_TRANSFORMS][16];
    uint8_t transform_set[FH_D3D_TRANSFORMS];
    uint32_t viewport[4];               /* D3DVIEWPORT8: X, Y, Width, Height */
    float viewport_depth[2];            /* MinZ, MaxZ */
    /* [fh7] the fixed-function lights and material (SetLight, LightEnable, SetMaterial), kept as the game gives them:
     * a light's words are Type (1 point, 2 spot, 3 directional), Diffuse, Specular, Ambient (four floats each),
     * Position, Direction (three each), Range, Falloff, Attenuation0-2, Theta, Phi; the material's are Diffuse,
     * Ambient, Specular, Emissive (four floats each) and Power. Not drawn yet: the draws only count them. */
    uint32_t lights[FH_D3D_LIGHTS][FH_D3D_LIGHT_WORDS];
    uint8_t light_set[FH_D3D_LIGHTS];
    uint8_t light_on[FH_D3D_LIGHTS];
    uint32_t material[FH_D3D_MATERIAL_WORDS];
    uint8_t material_set;
};

/* The state as it is now. */
const struct fh_d3d_state *fh_d3d_state(void);

/* 0x00221700 D3DDevice_SetStreamSource(StreamNumber, pStreamData, Stride) */
uint32_t fh_d3d_set_stream_source(uint32_t stream, uint32_t buffer, uint32_t stride);

/* 0x00221B70 D3DDevice_SetVertexShader(Handle) */
uint32_t fh_d3d_set_vertex_shader(uint32_t handle);

/* [acct2-fh] 11-13: 0x00221860 D3DDevice_SelectVertexShader(Handle, Address). Read from its code: with a Handle it
 * makes that shader's declaration the device's (+0x794 = Handle - 1, +0x798 = Handle; NV2A 0x1E94 = 6 and the
 * declaration's bit 0 when the inputs change), and always it sets the program's start (NV2A 0x1EA0 = Address,
 * +0x79C = Address). The program itself is whatever LoadVertexShaderProgram put there. Returns 0. */
uint32_t fh_d3d_select_vertex_shader(uint32_t handle, uint32_t address);

/* 0x00220220 D3DDevice_SetTexture(Stage, pTexture) */
uint32_t fh_d3d_set_texture(uint32_t stage, uint32_t texture);

/* 0x002203D0 D3DDevice_SetPalette(Stage, pPalette) */
uint32_t fh_d3d_set_palette(uint32_t stage, uint32_t palette);

/* 0x00222390 D3DDevice_SetRenderState(State, Value) */
uint32_t fh_d3d_set_render_state(uint32_t state, uint32_t value);

/* 0x00222DD0 D3DDevice_SetTextureStageState(Stage, Type, Value) */
uint32_t fh_d3d_set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value);

/* 0x0021F050 D3DDevice_SetTransform(State, pMatrix) */
uint32_t fh_d3d_set_transform(uint32_t state, uint32_t matrix);

/* 0x0021FBA0 D3DDevice_SetViewport(pViewport) */
uint32_t fh_d3d_set_viewport(uint32_t viewport);

/* [fh7] 2026-11-24: the fixed-function lighting calls, stand-ins that kept nothing before (each run called them
 * about 1.4 million times: once per light per mesh). Named from the other Finest Hour recompilation's address table
 * for this same build (reference only) and from the arguments our logs show: SetLight(0..2, a pointer), LightEnable
 * (0..2, 1), SetMaterial(a pointer), with the return sizes the stand-in table already had (8, 8, 4). Each keeps the
 * values (lights 0-7) and returns D3D_OK. */
/* 0x0021FDD0 D3DDevice_SetLight(Index, pLight) */
uint32_t fh_d3d_set_light(uint32_t index, uint32_t light);
/* 0x00220090 D3DDevice_LightEnable(Index, Enable) */
uint32_t fh_d3d_light_enable(uint32_t index, uint32_t enable);
/* 0x0021F1B0 D3DDevice_SetMaterial(pMaterial) */
uint32_t fh_d3d_set_material(uint32_t material);

/* [acct2-fh] 11-22: 0x002271C0 D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil), before build68 a stand-in
 * that did nothing (every clear the game asked for was dropped; the frame's pass clears to black and a depth of 1
 * at its start). The Xbox's flags: D3DCLEAR_ZBUFFER 0x1, D3DCLEAR_STENCIL 0x2, D3DCLEAR_TARGET 0xF0. A clear of the
 * depth (or stencil) after the frame's first draw, of the whole screen (Count 0), is done in the frame
 * (fh_draw_clear_depth) while video.depth_clear (FH_DEPTH_CLEAR, default on) is on; everything is counted. Returns
 * D3D_OK. */
uint32_t fh_d3d_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, uint32_t z_bits,
                      uint32_t stencil);

/* What fh_d3d_clear saw since the last take, for the frame-rate log (fh_frame_pace.c's [clears] line). */
struct fh_d3d_clear_stats {
    unsigned calls;                 /* every call */
    unsigned color;                 /* with any of D3DCLEAR_TARGET's bits */
    unsigned depth;                 /* with D3DCLEAR_ZBUFFER or D3DCLEAR_STENCIL */
    unsigned depth_before_draws;    /* of those, before the frame's first draw (the pass's own clear covers them) */
    unsigned depth_mid_frame;       /* after draws in the frame: what the first-person model needs */
    unsigned with_rects;            /* of the mid-frame ones, with rectangles (Count above 0: not done) */
    unsigned honoured;              /* done in the frame */
};
void fh_d3d_take_clear_stats(struct fh_d3d_clear_stats *out);
int fh_d3d_depth_clear_on(void);

/* 0x00222000 D3DDevice_DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData): VertexCount is the number of
 * 16-bit indices at pIndexData. */
uint32_t fh_d3d_draw_indexed_vertices(uint32_t primitive_type, uint32_t index_count, uint32_t indices);

/* [acct2-fh] 2026-11-13, read from their code (Europe v3):
 * 0x00221F60 D3DDevice_DrawVertices(PrimitiveType, StartVertex, VertexCount): pushes NV2A 0x17FC (begin) with the
 * primitive type, then 0x1810 words of (count - 1) << 24 | start in pieces of 256 vertices, then 0x17FC 0 (end); no
 * indices. Called by the particles (0x0012EACA: quad lists, stride 24) and RenderWare (0x001C9BC4). */
uint32_t fh_d3d_draw_vertices(uint32_t primitive_type, uint32_t first, uint32_t count);

/* 0x00221DF0 D3DDevice_DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride):
 * pushes 0x17FC with the primitive type, then the vertices themselves after 0x1818 (inline data), from the pointer,
 * stride bytes apart, then 0x17FC 0. Called by RenderWare's 2D (0x001D5202: strips of 4) and the game's screen quads
 * (0x001842E5: FVF 0x144, quad lists; 0x00184765: FVF 0x44, fans of 4). */
uint32_t fh_d3d_draw_vertices_up(uint32_t primitive_type, uint32_t count, uint32_t data, uint32_t stride);

/* 0x002223F0 D3DDevice_GetViewportOffsetAndScale(pOffset, pScale): on the console, from the viewport (X, Y, Width,
 * Height, MinZ, MaxZ), the screen's super-sample scale (ss) and its screen-space offset (o):
 * offset = (X ss + o + Width ss / 2, Y ss + o + Height ss / 2, MinZ z, 0), scale = (Width ss / 2, -Height ss / 2,
 * (MaxZ - MinZ) z, 0), z being the depth buffer's largest value. The programs map clip space to the screen with these.
 * The port draws in Vulkan's clip space and lets Vulkan map it to the viewport, so it answers offset (0, 0, 0, 0) and
 * scale (1, 1, 1, 0): what c-37 and c-38 are while a program runs here (fh_vsh_draw_begin). Callers: RenderWare's 2D
 * (0x001D4E61, 0x001D5092) and its skinned pipeline (0x001EF322). */
uint32_t fh_d3d_get_viewport_offset_and_scale(uint32_t offset, uint32_t scale);

/* 0x0022CBC0: two floats from the device, the multisample scale (samples a pixel across and down: 1 and 1 without
 * multisampling; 0x002258CB makes them from the multisample type's two digits). RenderWare's 2D (0x001D50D4) puts them
 * in c0.xy, and its programs (0x00318088, 0x003180CC, 0x00318100) do oPos.xy = v0.xy * c0.xy + c1.xy with c1 the
 * viewport's top left corner from GetViewportOffsetAndScale. In the port's clip space a pixel across the viewport is
 * 2 / Width and one down is -2 / Height, so that is the answer; c1 is then (-1, 1). */
uint32_t fh_d3d_get_multisample_scale(uint32_t x, uint32_t y);

/* Called by Swap (fh_present.c) with the number of the frame that just ended (counting from 1). */
void fh_d3d_state_frame_end(unsigned long frame);

/* [acct2-fh] 11-14: the frame being recorded (Swaps so far + 1, as FH_VK_DUMP counts), and whether it is in a list
 * of frames and ranges ("3000,6200-6210"), as the census reads FH_DRAW_LOG. */
unsigned long fh_d3d_frame(void);
int fh_d3d_frame_listed(const char *list);

/* 1 while a census frame (FH_DRAW_LOG) is being recorded: fh_log.c then prints every stand-in call too. */
int fh_d3d_census_active(void);
