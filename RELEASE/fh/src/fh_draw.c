/* [waw-decomp] Finest Hour port, phase 2: the game's Direct3D draws on Vulkan, through ngl_vk's lower layer
 * (../../ngl_vk/include/ngl_vk_low.h: pipelines, buffers, textures and draws with shaders of our own).
 *
 * The owner chose (2026-11-09 14:42) to cut the renderer at Direct3D's level: RenderWare stays the game's own, and each
 * D3DDevice_DrawIndexedVertices is drawn here with the device's state at that moment. What is written so far is what
 * the front end needs (its draw census, dev/agents/waw-decomp/FINEST-HOUR.md):
 *  - fixed-function draws whose FVF has a position (x, y, z), a diffuse color and texture coordinates (the front end's
 *    0x142), transformed by world (transform 6) x view (0) x projection (1), in triangle lists, strips and fans;
 *  - texture stage 0 (its color and alpha operations and arguments), the alpha test, blending and the depth test,
 *    read from the game's own state arrays (fh_d3d_state.c says why);
 *  - textures in the Xbox's swizzled P8 (8-bit with a palette), A8R8G8B8 and X8R8G8B8, and linear A8R8G8B8 and
 *    X8R8G8B8, turned into 32-bit pictures on the CPU and kept, by their data, format and palette.
 * Draws that need more are left out and counted (fh_draw_report): another FVF, a vertex shader program, another
 * primitive type or texture format.
 *
 * A frame with draws: the first draw after a Swap begins a Vulkan frame and a pass into ngl_vk's picture (cleared to
 * black); Swap ends it (fh_present.c). A frame without draws (a movie) is still shown by ngl_vk_draw_picture.
 *
 * Draws whose SetVertexShader handle is 0 (the title screen's background) use the last FVF the game used with the
 * same stride (fvf_for_stride below says why); FH_FVF0_AS=<hex FVF> forces one for them.
 *
 * [acct2-fh] 2026-11-13: draws with a shader made by CreateVertexShader (fh_vsh.c). One made without a function
 * (the level's world: RenderWare's 0x001F98E7 makes them from its geometry's formats) is drawn like an FVF:
 * its vertices are rewritten on the CPU into the 0x142 layout (position, diffuse color, texture coordinates) from
 * the registers its declaration names, white and (0, 0) where it names none (declaration_vertices below). One with a
 * program (the clones) has its program run on the CPU for each vertex (fh_vsh_run), and the results go out with an
 * identity transform in a layout of their own: the position as the program made it, back in clip space with its w
 * (x, y, z, w), so that Vulkan clips and interpolates with perspective as the console did; then the diffuse color and
 * the texture coordinates (program_vertices). FH_VSH_RUN=0 leaves those draws out instead; FH_VSH_W=0 drops w after
 * the divide as build10 did (for comparison runs).
 *
 * [acct2-fh] 2026-11-13 16:10: the two draws without indices go the same way (draw_vertices below):
 * D3DDevice_DrawVertices (0x00221F60: vertices StartVertex.. of the streams; the particles' quad lists and RenderWare's)
 * and D3DDevice_DrawVerticesUP (0x00221DF0: vertices from a pointer and a stride; RenderWare's 2D and the game's own
 * screen quads). Quad lists, quad strips and polygons are drawn as triangles. Vertices already on the screen (FVF
 * XYZRHW, the game's 0x144 and 0x44) are rewritten into the program layout, in clip space from the viewport's pixels
 * (rhw_vertices).
 *
 * [acct2-fh] 2026-11-13 16:40: a program draw now runs its program on the GPU (shaders/program.vert, one shader that
 * reads the program's instructions from the pixel constants, as fh_vsh_run reads them on the CPU): the vertices go
 * to Vulkan as the game wrote them, each input register in the format its declaration gives (gpu_program_layout).
 * A program the GPU path cannot take (one that writes a constant register, an input kind with no Vulkan vertex
 * format, more than eight inputs) still runs on the CPU. FH_VSH_GPU=0 runs every program on the CPU (for comparison
 * runs).
 *
 * [acct2-fh] 2026-11-14: fog, as the game's Direct3D sets it up (its code at 0x002293A0, read in
 * dev/agents/acct2-fh/NOTES.md): with FOGENABLE (state 92) on, the color is mixed towards FOGCOLOR (138) by a factor
 * f, 1 meaning no fog. With a fog table (FOGTABLEMODE, 93) f comes from a distance d: LINEAR (3)
 * f = (FOGEND - d) / (FOGEND - FOGSTART) (Direct3D gives the NV2A bias 1 + end / (end - start) and scale
 * -1 / (end - start), and the NV2A subtracts 1), EXP (1) f = e^(-density d), EXP2 (2) f = e^(-(density d)^2); without
 * one (NONE, 0) f is the vertex's own fog value. A vertex program's d (or f) is its oFog.x (the game's programs write
 * r12.w there, the clip-space w); for the fixed-function draws d is the eye's depth, the clip-space w (fixed.vert), and
 * those draws get no fog without a table (theirs would come from the specular color's alpha, not carried yet), nor
 * do the vertices already on the screen. FH_FOG=0 draws without fog (for comparison runs).
 *
 * [acct2-fh] 2026-11-14: the check of the GPU programs against the CPU's. FH_VSH_CPU_FRAMES=<frames and ranges>
 * ("6200" or "6200-6210,7000") runs every program draw of those frames on the CPU (fh_vsh_run_many) and the rest on
 * the GPU, so that one run's dumps (FH_VK_DUMP) of a still scene (the game paused) taken in and out of those frames
 * compare program.vert with fh_vsh.c pixel by pixel; ngl_vk_low has no way to read vertices back, and its layer is
 * shared with CoD3, so the comparison is of pictures.
 *
 * [acct2-fh] 2026-11-14: mipmaps. A swizzled texture carries its smaller levels after the first, each half the size
 * of the one before (down to 1 x 1), packed one after another; their number is the Format word's bits 16-19. The
 * level's world textures have up to 9, and the game asks for them around those draws (D3DTSS_MIPFILTER, stage state
 * 5: 1 point, 2 linear, 0 none), so each level goes to the GPU as the game made it and the sampler follows
 * MIPFILTER. Linear textures have one level. FH_MIPS=0 sends only the first level and samples it alone, as before
 * (for comparison runs).
 *
 * [acct2-fh] 2026-11-14: the textures kept stay true to the game's memory. A texture used to be made once, the first
 * time a draw used it, and kept for the run by its data address, format and palette. Three ways that went wrong:
 * RenderWare locks a texture or a palette (D3DResource_Lock2, 0x00221D30, from its raster lock at 0x001C8260) and
 * writes new pixels or colors at the same address; a level section's textures are freed and others made in the same
 * memory, often with the same format; and once 1024 were kept, every draw of a new one made another GPU texture that
 * was never freed. Now a lock of a texture, a texture's surface or a palette (fh_d3d.c calls fh_draw_resource_locked)
 * lets go of the textures made from it, which are made again from the new pixels at their next draw; when the list
 * is full the one drawn longest ago goes. A texture let go of is freed when the next frame begins, after the frame
 * that may still draw with it.
 *
 * [acct2-fh] 2026-11-14: the button pictures, catalogued run by run (owner 11-13 19:04: "start cataloging hints and
 * button prompts throughout our runs. We are going to be adding keycaps similar to cod3's"; "Same with menus"). Every
 * button picture the game draws, in its menus (the front end's and pause menu's set) and beside its hints in play
 * (the Xbox_*_but set), is a 32 x 32 texture (dev/agents/acct2-fh/PROMPTS.md). FH_PROMPT_LOG=1 prints each 32 x 32
 * picture when it comes on the screen, with the frame and a hash of its pixels (the same picture has the same hash
 * in every run, wherever the game keeps it), and the kind of draw it came in with its vertex count:
 *     [prompt] frame 1234: 32 x 32 picture 9F2C...: on the screen (format 0x0B; a program draw of 4 vertices; 3
 *     times before, last at frame 980)
 * "screen" is a draw of pre-transformed (XYZRHW) vertices, "program" one through a vertex program (RenderWare's 2D
 * among them), "world" a fixed-function 3D draw. boot37 (11-13) saw 71 different 32 x 32 pictures, far more than the
 * buttons: most are the level's own small textures, which the kind and the vertex count tell apart (a button is a
 * quad of 4 or 6 vertices in a screen or program draw). A picture off the screen for fewer than PROMPT_GAP_FRAMES
 * frames (a blinking hint) has not left it. fh_draw_report adds how many different ones were seen, by kind.
 * tools/fh_prompt_runs.py gathers the lines of every run into one list; the frames say which screen each was on
 * (FH_VK_DUMP of the same frames).
 *
 * [acct2-fh] 2026-11-14: culling, ZENABLE and the depth bias, as the game's own handlers set the NV2A up
 * (pipeline_state, cull_from_xbox). Frame 7800 of the first level (boot50's census): 536 of 812 draws ask for
 * CULLMODE 0x900 (FRONTFACE 0x901), none for a depth bias, one for ZENABLE 0.
 *
 * [acct2-fh] 2026-11-13: the picture's shape (dev/agents/acct2-fh/RESOLUTION.md, stage 3). With a shape other than
 * the game's 4:3 the 3D view is widened or narrowed to it (fh_aspect.c) and the 3D draws take the whole picture; the
 * 2D draws (screen vertices and RenderWare's 2D programs, of FH_FLAT_PROGRAM instructions or fewer, 4 by default)
 * and the CPU picture (the films) take the screen's 4:3 part in the middle (screen_part); an untextured screen draw
 * over the whole screen (a fade, a flash) takes the whole picture. */
#include "fh_draw.h"
#include "fh_text.h"                /* [waw-decomp-fh2 r19] the text layer */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(FH_HAVE_VULKAN)
#include <vulkan/vulkan.h>

#include "recomp_types.h"           /* MEM32: guest memory */
#include "xbox_memory_layout.h"     /* XBOX_PTR */
#include "ngl_vk_low.h"
#include "ngl_vk_backend.h"
#include "fh_d3d_state.h"
#include "fh_d3d.h"           /* [fh4] fh_d3d_render_target: the back buffer read as a texture */
#include "fh_host.h"          /* [waw-decomp/fh2] 11-23: a steady clock */
#include "fh_vsh.h"
#include "fh_push.h"         /* [acct2-fh] the game's own push buffer writes, read before each draw */
#include "fh_aspect.h"
#include "fh_frame_clock.h"
#if defined(FH_HAVE_SDL3)
#include <SDL3/SDL.h>            /* [waw-decomp/fh19] HD textures: SDL_GlobDirectory, SDL_LoadPNG */
#endif  /* [fh15] held frames: the in-between drawing above 60 */
#include "fh_settings.h"       /* [acct2-fh] the picture's shape: the 2D kept 4:3 in the middle */

/* The shaders, SPIR-V made from shaders/fixed.vert and fixed.frag when the port is built (CMakeLists.txt) */
static const uint32_t fixed_vert_spirv[] =
#include "fixed.vert.inc"
;
static const uint32_t fixed_frag_spirv[] =
#include "fixed.frag.inc"
;
static const uint32_t program_vert_spirv[] =
#include "program.vert.inc"
;

#define GUEST_RENDER_STATES     0x00230720u     /* fh_d3d_state.c */
#define GUEST_STAGE_STATES      0x00230520u
#define DATA_BIT                0x80000000u     /* a resource's Data as D3D8's Lock2 returns it (fh_d3d.c) */

/* Transforms, render states and texture stage states, in the Xbox D3D8's numbering (the census: the values each
 * slot holds; the names of the render states are read from those values and still to be checked against the real
 * functions' push tables) */
#define TRANSFORM_VIEW          0
#define TRANSFORM_PROJECTION    1
#define TRANSFORM_WORLD         6
#define RS_ZFUNC                57
#define RS_ALPHAFUNC            58
#define RS_ALPHABLENDENABLE     59
#define RS_ALPHATESTENABLE      60
#define RS_ALPHAREF             61
#define RS_SRCBLEND             62
#define RS_DESTBLEND            63
#define RS_ZWRITEENABLE         64
#define RS_COLORWRITEENABLE     67
#define RS_STENCILZFAIL         68              /* [fh5] 68-73, 144, 145: the stencil states, in the XDK's order */
#define RS_STENCILPASS          69              /* (68-73 sit between COLORWRITEENABLE 67 and BLENDOP 74, 144-145 */
#define RS_STENCILFUNC          70              /* between ZENABLE 143 and CULLMODE 147, whose handlers are known) */
#define RS_STENCILREF           71
#define RS_STENCILMASK          72
#define RS_STENCILWRITEMASK     73
#define RS_BLENDOP              74
#define RS_STENCILENABLE        144
#define RS_STENCILFAIL          145
#define RS_POLYGONOFFSETZSLOPESCALE 77          /* [acct2-fh] 11-14: the ZBIAS handler (0x00222970) sets these three */
#define RS_POLYGONOFFSETZOFFSET 78
#define RS_SOLIDOFFSETENABLE    81
#define RS_FOGENABLE            92              /* [acct2-fh] 11-14: read from 0x002293A0 (the fog's setup) */
#define RS_FOGTABLEMODE         93
#define RS_FOGSTART             94
#define RS_FOGEND               95
#define RS_FOGDENSITY           96
#define RS_LIGHTING             102             /* [fh7] fixed-function lighting on; [fh8] drawn by fixed.vert */
#define RS_COLORVERTEX          105             /* [fh8] the XDK's order: LOCALVIEWER 104, COLORVERTEX 105 */
#define RS_DIFFUSEMATERIALSOURCE 111            /* [fh8] 110-113: specular, diffuse, ambient, emissive sources */
#define RS_AMBIENTMATERIALSOURCE 112
#define RS_EMISSIVEMATERIALSOURCE 113
#define RS_AMBIENT              115             /* [fh8] the global ambient light, a D3DCOLOR */
#define RS_NORMALIZENORMALS     142
#define RS_SPECULARENABLE       103             /* [fh5] the XDK's order: after LIGHTING 102 */
#define RS_FOGCOLOR             138             /* its handler 0x002226E0 */
#define RS_ZENABLE              143             /* [acct2-fh] 11-14: its handler 0x002235B0 */
#define RS_CULLMODE             147             /* its handler 0x00222730 */
#define RS_TEXTUREFACTOR        148             /* its handler 0x00222820 */
#define CULL_CW                 0x900u          /* D3DCULL_CW: clockwise triangles go (the NV2A's GL_CW) */
#define CULL_CCW                0x901u          /* D3DCULL_CCW: counterclockwise ones go */
#define TSS_ADDRESSU            0
#define TSS_ADDRESSV            1
#define TSS_MAGFILTER           3
#define TSS_MINFILTER           4
#define TSS_MIPFILTER           5
#define TSS_MIPMAPLODBIAS       6               /* [fh10] the XDK's order: 6 LOD bias, 7 max mip level, 8 max anisotropy */
#define TSS_MAXMIPLEVEL         7
#define TSS_MAXANISOTROPY       8
#define TEXF_ANISOTROPIC        3
#define TSS_ALPHAKILL           11              /* [fh5] 11, 21, 28: the XDK's order, as the known ones fix it */
#define TSS_COLOROP             12
#define TSS_TEXTURETRANSFORMFLAGS 21
#define TSS_TEXCOORDINDEX       28
#define TSS_COLORARG0           13
#define TSS_COLORARG1           14
#define TSS_COLORARG2           15
#define TSS_ALPHAOP             16
#define TSS_ALPHAARG0           17
#define TSS_ALPHAARG1           18
#define TSS_ALPHAARG2           19

/* FVF bits (the same on the Xbox as in Direct3D 8) */
#define FVF_POSITION_MASK       0x00Eu
#define FVF_XYZ                 0x002u
#define FVF_XYZRHW              0x004u          /* x, y in the screen's pixels, z 0-1, and 1/w */
#define FVF_NORMAL              0x010u
#define FVF_PSIZE               0x020u
#define FVF_DIFFUSE             0x040u
#define FVF_SPECULAR            0x080u

#define PRIMITIVE_TRIANGLE_LIST     5
#define PRIMITIVE_TRIANGLE_STRIP    6
#define PRIMITIVE_TRIANGLE_FAN      7
#define PRIMITIVE_QUAD_LIST         8
#define PRIMITIVE_QUAD_STRIP        9
#define PRIMITIVE_POLYGON           10

#define MAX_TEXTURES            1024
#define MAX_TEXTURE_SIDE        4096u
#define MAX_INDICES             65536u

enum skip_reason { SKIP_FVF, SKIP_PROGRAM, SKIP_DECLARATION, SKIP_PRIMITIVE, SKIP_NO_STREAM, SKIP_ROOM, SKIP_REASONS };
static const char *const skip_names[SKIP_REASONS] = {
    "an FVF without position, diffuse color and texture coordinates", "a vertex shader program",
    "a declaration this file cannot draw yet", "a primitive type not written yet", "no vertex buffer",
    "no room left in the frame"};

static struct {
    int ready;
    uint32_t game_width, game_height;
    uint32_t flat_program_instructions;  /* [acct2-fh] a program this short is RenderWare's 2D (FH_FLAT_PROGRAM) */
    uint32_t target_width, target_height;
    uint32_t vertex_shader, pixel_shader;
    struct ngl_vk_low_formats formats;
    int frame_open;
    uint32_t white;                     /* a 1 x 1 white texture for draws without one */
    uint32_t picture;                   /* the CPU picture's texture, game_width x game_height */
    uint32_t text_texture, text_rows;   /* [waw-decomp-fh2 r19] the text layer's texture: target_width x text_rows */
    unsigned long drawn, skipped[SKIP_REASONS];
    unsigned long frame_first_drawn;    /* [acct2-fh] 11-22: drawn when this frame began (fh_draw_frame_has_draws) */
    unsigned long unknown_formats;
    uint32_t fvf0_as;
    int run_programs;                   /* FH_VSH_RUN: run vertex programs on the CPU (default 1) */
    int program_w;                      /* FH_VSH_W: give program draws their w (default 1) */
    int programs_many;                  /* FH_VSH_MANY: run a draw's vertices through a program together */
    uint32_t program_shader;            /* shaders/program.vert: vertex programs on the GPU */
    int gpu_programs;                   /* FH_VSH_GPU: run them there (default 1, when the shader was made) */
    int gpu_expand;                     /* FH_VSH_GPU_EXPAND: also those whose inputs need rewriting as floats
                                         * (default 1) */
    int fog;                            /* FH_FOG: draw the game's fog (default 1) */
    int project;                        /* [fh6] FH_PROJECT: a program's oT0 is divided by its w, as the Xbox's
                                         * 2D texture reads do (default 1) */
    unsigned long projected_vertices;   /* [fh6] program vertices run here with oT0.w other than 1 */
    int unread;                         /* [fh6] FH_UNREAD: a set texture that can't be read reads 0 (default 1) */
    unsigned long unread_draws;         /* [fh6] draws whose set texture could not be read */
    const char *cpu_frames;             /* FH_VSH_CPU_FRAMES: frames whose programs run on the CPU (the top of the
                                         * file), or NULL */
    unsigned long cpu_frame_said;       /* the last such frame the log was told of */
    int mips;                           /* FH_MIPS: textures with all their levels, sampled as MIPFILTER says
                                         * (default 1) */
    int prompt_log;                     /* FH_PROMPT_LOG: print the 32 x 32 pictures as they come on the screen */
    int cull;                           /* [acct2-fh] FH_CULL: 1 cull as the game asks (default), 0 never, -1 the other
                                         * way round (for a check) */
    unsigned long cull_probe;           /* FH_CULL_PROBE=<frame>: that frame as the game asks, the next the other way
                                         * round, the one after without culling (dump all three) */
    int stencil;                        /* [fh5] FH_STENCIL: follow the stencil states (default 1) */
    unsigned long stencil_draws;
    unsigned long lit_draws;            /* [fh7] fixed-function draws with LIGHTING on: drawn with their vertex
                                         * color, the lights and material not applied yet */
    unsigned long lit_normal_draws;     /* [fh7] of those, with a normal in the vertex */
    unsigned lit_lights_most;           /* [fh7] the most lights on for one of them */
    unsigned lit_logged;
    int light;                          /* [fh8] FH_LIGHT (default 1): light the lit draws that have a normal */
    unsigned long lit_lighted;          /* [fh8] lit draws drawn with the lights */
    unsigned long lit_no_normal;        /* [fh8] lit draws without a normal: drawn with their vertex color */
    unsigned long unhandled[10];        /* [fh9] draws with a render state this file does not draw yet (unhandled_states) */
    unsigned unhandled_logged;
    unsigned long lod_draws;            /* [fh10] stage 0 with a LOD bias or a max mip level (not drawn yet) */
    unsigned long specular_draws;       /* [fh5] draws with SPECULARENABLE on: the specular color is not added */
    unsigned long odd_coords;           /* [fh5] draws whose stage 0 takes another coordinate set, generated or
                                         * transformed ones: drawn with set 0 as it is */        /* [fh5] draws with the stencil test on, for the report */
    int z_enable;                       /* FH_ZENABLE: follow D3DRS_ZENABLE (default 1); 0 as before build36 */
    int depth_bias;                     /* FH_DEPTH_BIAS: the game's polygon offset (ZBIAS) (default 1) */
    int screen_reads;                   /* [fh4] FH_SCREEN_READS: draws that read the back buffer as a texture read
                                         * the picture drawn so far (default 1); 0 draws them untextured, as before */
    int rect_uv;                        /* [fh4] FH_RECT_UV: a linear texture's coordinates are in texels (default 1) */
    uint32_t screen_texture;            /* [fh4] the picture so far, copied for those draws */
    uint32_t screen_width, screen_height;
    unsigned long screen_frame, screen_drawn;  /* the frame and draw.drawn it was copied at */
    unsigned long screen_copies, screen_draws;  /* for the report */
    unsigned long later_stages[FH_D3D_STAGES];  /* [fh4] draws whose stage 1-3 was on, with or without a texture */
    unsigned later_setups;                      /* [fh4] how many of those setups were logged */
} draw;

/* FH_PROMPT_LOG's pictures: when each was last drawn, how often it came on the screen, and in which kinds of draw. */
#define MAX_PICTURES            256
#define PROMPT_GAP_FRAMES       60      /* off the screen this long (a second at 60) before it counts as gone */
enum picture_kind { PICTURE_SCREEN = 1, PICTURE_PROGRAM = 2, PICTURE_WORLD = 4 };
static struct {
    uint64_t hash;
    uint32_t format;
    unsigned long last, appearances;
    unsigned kinds;                     /* enum picture_kind bits */
} pictures[MAX_PICTURES];
static unsigned picture_count;
static uint64_t last_picture;               /* texture_for's last texture, when a 32 x 32 one (else 0) */
static uint32_t last_picture_format;
static uint32_t last_rect_width, last_rect_height;  /* [fh4] texture_for's last texture, when a linear one (else 0) */

/* Where the draw time goes, since the last report (fh_draw_report): boot20 ran at 17-31 frames a second once the
 * clones' program was run on the CPU, and this tells the interpreter, the copies and Vulkan apart. Nanoseconds. */
static struct {
    uint64_t all, programs, declarations, copies, vulkan, frame_ends;
    uint64_t program_vertices, declaration_vertices, copied_bytes, gpu_program_vertices;
    unsigned long draws, program_draws, gpu_program_draws, frames;
    unsigned long selected_draws, merged_draws;   /* [acct2-fh] SelectVertexShader's draws; those of more streams */
    uint64_t merges;                    /* the time putting their streams side by side */
    uint64_t expands, expanded_vertices;  /* the GPU draws whose inputs were rewritten as floats: time, vertices */
    unsigned long expanded_draws;
    size_t room_most;                   /* the most of ngl_vk_low's frame room one frame used */
} spent;

/* [waw-decomp/fh2] 11-23: a steady clock on both systems (QueryPerformanceCounter on Windows, where this read the
 * wall clock, which a time sync can move; fh_host.c) */
static uint64_t now_ns(void)
{
    return fh_host_now_ns();
}

static struct {
    uint32_t data, format, size, palette;   /* what the texture was made from */
    uint32_t handle;
    unsigned long drawn;                    /* the frame (fh_d3d_frame) it was last drawn in */
    uint64_t picture;                       /* a 32 x 32 one: a hash of its pixels (FH_PROMPT_LOG); 0 otherwise */
    uint32_t picture_format;
    uint32_t rect_width, rect_height;       /* [fh4] a linear one's size (its coordinates are in texels); 0 otherwise */
} textures[MAX_TEXTURES];
static unsigned texture_count;
static uint32_t textures_to_free[MAX_TEXTURES];  /* let go of: freed when the next frame begins */
static unsigned textures_to_free_count;
static unsigned long textures_remade, textures_dropped;  /* for the report: after a lock; the list full */

static uint8_t *scratch;                /* a texture's pixels on their way to the GPU */
static size_t scratch_size;

static float as_float(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint32_t render_state(uint32_t which)
{
    return MEM32(GUEST_RENDER_STATES + 4 * which);
}

static uint32_t stage_state(uint32_t stage, uint32_t type)
{
    return MEM32(GUEST_STAGE_STATES + 4 * (FH_D3D_STAGE_STATES * stage + type));
}

static uint8_t *scratch_room(size_t bytes)
{
    if (bytes > scratch_size) {
        uint8_t *bigger = realloc(scratch, bytes);

        if (!bigger)
            return NULL;
        scratch = bigger;
        scratch_size = bytes;
    }
    return scratch;
}

/* ------------------------------------------------------------------------------------------------------------------
 * Textures */

/* The offset of texel (x, y) in a swizzled texture of width x height (both powers of two): the bits of x and y
 * interleaved, x's first, for as long as each side has bits (the NV2A's swizzle). */
static uint32_t swizzled_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t offset = 0, bit = 1;

    for (uint32_t side = 1; side < width || side < height; side <<= 1) {
        if (side < width) {
            if (x & side)
                offset |= bit;
            bit <<= 1;
        }
        if (side < height) {
            if (y & side)
                offset |= bit;
            bit <<= 1;
        }
    }
    return offset;
}

/* A D3DPalette's colors: Common's top two bits give its size (0: 256 entries, 1: 128, 2: 64, 3: 32). */
static uint32_t palette_entries(uint32_t palette)
{
    static const uint32_t sizes[4] = {256, 128, 64, 32};

    return palette ? sizes[MEM32(palette) >> 30] : 0;
}

/* [fh4] 2026-11-24: the Xbox D3D8 texture formats besides P8 that are plain texels, by the Format word's bits 8-15.
 * Every run logged swizzled format 0x00 (L8) textures as "not written yet" and drew them untextured, so these now
 * decode too. Each entry: the format, 1 for a linear one (else swizzled), and its bytes per texel.
 * The 8-bit A8 formats (0x19, 0x1F) stay out: what the NV2A gives for their color is not checked yet. */
static const struct { uint8_t format, linear, bytes; } texel_formats[] = {
    {0x00, 0, 1}, {0x01, 0, 1}, {0x02, 0, 2}, {0x03, 0, 2}, {0x04, 0, 2}, {0x05, 0, 2}, {0x06, 0, 4}, {0x07, 0, 4},
    {0x1A, 0, 2},                                       /* swizzled: L8 AL8 A1R5G5B5 X1R5G5B5 A4R4G4B4 R5G6B5 A8R8G8B8 X8R8G8B8 A8L8 */
    {0x10, 1, 2}, {0x11, 1, 2}, {0x12, 1, 4}, {0x13, 1, 1}, {0x1B, 1, 1}, {0x1C, 1, 2}, {0x1D, 1, 2}, {0x1E, 1, 4},
    {0x20, 1, 2},                                       /* linear: A1R5G5B5 R5G6B5 A8R8G8B8 L8 AL8 X1R5G5B5 A4R4G4B4 X8R8G8B8 A8L8 */
};

/* 1 when `format` in that layout is one of texel_formats. */
static int texel_kind(uint32_t format, int linear)
{
    for (unsigned i = 0; i < sizeof texel_formats / sizeof texel_formats[0]; i++)
        if (texel_formats[i].format == format && texel_formats[i].linear == (linear != 0))
            return 1;
    return 0;
}

/* Bytes per texel: 1 for P8 and the 8-bit formats, 2 for the 16-bit ones, else 4. */
static uint32_t texel_bytes_of(uint32_t format)
{
    if (format == 0x0B)
        return 1;
    for (unsigned i = 0; i < sizeof texel_formats / sizeof texel_formats[0]; i++)
        if (texel_formats[i].format == format)
            return texel_formats[i].bytes;
    return 4;
}

/* A 5, 6 or 4-bit channel widened to 8 bits, its top bits repeated in the bottom. */
static uint32_t widen(uint32_t value, unsigned bits)
{
    return bits == 4 ? value * 0x11u : bits == 5 ? (value << 3 | value >> 2) : (value << 2 | value >> 4);
}

/* One texel of `format`, read little-endian into raw, as 0xAARRGGBB. */
static uint32_t texel_argb(uint32_t format, uint32_t raw)
{
    uint32_t a = 0xFF, r, g, b;

    switch (format) {
    case 0x00: case 0x13:                               /* L8: gray, opaque */
        r = g = b = raw & 0xFF;
        break;
    case 0x01: case 0x1B:                               /* AL8: gray, and the same value for alpha */
        r = g = b = a = raw & 0xFF;
        break;
    case 0x1A: case 0x20:                               /* A8L8: gray in the low byte, alpha in the high */
        r = g = b = raw & 0xFF;
        a = (raw >> 8) & 0xFF;
        break;
    case 0x02: case 0x10: case 0x03: case 0x1C:         /* A1R5G5B5, X1R5G5B5 */
        r = widen((raw >> 10) & 0x1F, 5);
        g = widen((raw >> 5) & 0x1F, 5);
        b = widen(raw & 0x1F, 5);
        if (format == 0x02 || format == 0x10)
            a = raw & 0x8000 ? 0xFF : 0;
        break;
    case 0x04: case 0x1D:                               /* A4R4G4B4 */
        a = widen((raw >> 12) & 0xF, 4);
        r = widen((raw >> 8) & 0xF, 4);
        g = widen((raw >> 4) & 0xF, 4);
        b = widen(raw & 0xF, 4);
        break;
    case 0x05: case 0x11:                               /* R5G6B5 */
        r = widen((raw >> 11) & 0x1F, 5);
        g = widen((raw >> 5) & 0x3F, 6);
        b = widen(raw & 0x1F, 5);
        break;
    case 0x07: case 0x1E:                               /* X8R8G8B8 */
        return raw | 0xFF000000u;
    default:                                            /* A8R8G8B8 */
        return raw;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

/* Level `level` of the texture at guest object `texture` as a 32-bit picture (blue, green, red, alpha) in scratch;
 * 0 for a format not written yet. */
static int texture_pixels(uint32_t texture, uint32_t palette, uint32_t level, uint32_t *width_out,
                          uint32_t *height_out)
{
    const uint32_t format_word = MEM32(texture + 12), size = MEM32(texture + 16);
    const uint32_t format = (format_word >> 8) & 0xFF;
    uint32_t data = MEM32(texture + 4) | DATA_BIT, width, height, pitch;
    uint8_t *out;

    if (size) {
        if (level)                                      /* a linear texture has one level */
            return 0;
        width = (size & 0xFFF) + 1;
        height = ((size >> 12) & 0xFFF) + 1;
        pitch = (((size >> 24) & 0xFF) + 1) * 64;
    } else {
        const uint32_t texel_bytes = texel_bytes_of(format);

        width = 1u << ((format_word >> 20) & 0xF);
        height = 1u << ((format_word >> 24) & 0xF);
        for (uint32_t l = 0; l < level; l++) {         /* the levels before this one, packed */
            data += width * height * texel_bytes;
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        }
        pitch = 0;
    }
    if (width > MAX_TEXTURE_SIDE || height > MAX_TEXTURE_SIDE || !(out = scratch_room((size_t)width * height * 4)))
        return 0;
    *width_out = width;
    *height_out = height;

    if (format == 0x0B && !size) {                      /* swizzled P8, with its palette */
        uint32_t colors[256], entries = palette_entries(palette);
        const uint32_t palette_data = palette ? MEM32(palette + 4) | DATA_BIT : 0;

        for (uint32_t i = 0; i < 256; i++)
            colors[i] = i < entries ? MEM32(palette_data + 4 * i) : 0;
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++) {
                uint32_t index = *(const uint8_t *)XBOX_PTR(data + swizzled_offset(x, y, width, height));

                memcpy(out + 4 * ((size_t)y * width + x), &colors[index], 4);
            }
        return 1;
    }
    if (texel_kind(format, size != 0)) {                /* the other 8, 16 and 32-bit formats, either layout */
        const uint32_t bytes = texel_bytes_of(format);

        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++) {
                const uint32_t at = size ? y * pitch + bytes * x : bytes * swizzled_offset(x, y, width, height);
                const uint8_t *p = (const uint8_t *)XBOX_PTR(data + at);
                uint32_t raw = p[0];

                if (bytes > 1)
                    raw |= (uint32_t)p[1] << 8;
                if (bytes > 2)
                    raw |= (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
                raw = texel_argb(format, raw);
                memcpy(out + 4 * ((size_t)y * width + x), &raw, 4);
            }
        return 1;
    }
    if (!level && draw.unknown_formats++ < 8)
        fprintf(stderr, "[fh] draw: texture 0x%08X has format 0x%02X (%s), not written yet: drawn without it\n",
                texture, format, size ? "linear" : "swizzled");
    return 0;
}

/* How many levels of the guest texture go to the GPU: the Format word's bits 16-19 for a swizzled one, at most down
 * to 1 x 1; 1 for a linear one, or with FH_MIPS=0. */
static uint32_t texture_levels(uint32_t texture)
{
    const uint32_t format_word = MEM32(texture + 12);
    const uint32_t width_log = (format_word >> 20) & 0xF, height_log = (format_word >> 24) & 0xF;
    const uint32_t most = 1 + (width_log > height_log ? width_log : height_log);
    uint32_t levels = (format_word >> 16) & 0xF;

    if (!draw.mips || MEM32(texture + 16))
        return 1;
    if (levels < 1)
        levels = 1;
    return levels < most ? levels : most;
}

/* Lets go of a texture handle: freed when the next frame begins (frame_begin), since draws of this frame may still
 * use it. With no room left in the list it is freed at once (only when frames end without beginning again). */
static void free_later(uint32_t handle)
{
    if (!handle || handle == draw.white)
        return;
    if (textures_to_free_count < MAX_TEXTURES)
        textures_to_free[textures_to_free_count++] = handle;
    else
        ngl_vk_low_texture_free(handle);
}

/* Takes entry i out of the list (the last one takes its place) and lets go of its texture. */
static void forget_texture(unsigned i)
{
    free_later(textures[i].handle);
    textures[i] = textures[--texture_count];
}

/* At the start of a frame: frees the textures let go of during the frames before. */
static void free_forgotten_textures(void)
{
    for (unsigned i = 0; i < textures_to_free_count; i++)
        ngl_vk_low_texture_free(textures_to_free[i]);
    textures_to_free_count = 0;
}

/* Common's type bits (D3DCOMMON_TYPE_*): the resources whose memory a kept texture was made from. */
#define COMMON_TYPE_MASK        0x00070000u
#define COMMON_TYPE_PALETTE     0x00030000u
#define COMMON_TYPE_TEXTURE     0x00040000u
#define COMMON_TYPE_SURFACE     0x00050000u

void fh_draw_resource_locked(uint32_t resource)
{
    uint32_t type, data;

    if (!resource)
        return;
    type = MEM32(resource) & COMMON_TYPE_MASK;
    if (type != COMMON_TYPE_PALETTE && type != COMMON_TYPE_TEXTURE && type != COMMON_TYPE_SURFACE)
        return;                         /* vertex and index buffers, every frame: nothing kept from them */
    data = MEM32(resource + 4);
    for (unsigned i = 0; i < texture_count;)
        if (type == COMMON_TYPE_PALETTE ? textures[i].palette == data : textures[i].data == data) {
            forget_texture(i);
            textures_remade++;
        } else {
            i++;
        }
}

/* FH_PROMPT_LOG: a 32 x 32 picture is being drawn, in a draw of `kind` with `vertices`; printed when it was not on
 * the screen in the last PROMPT_GAP_FRAMES frames, or comes in a kind of draw not seen with it before. */
static void note_picture(uint64_t hash, uint32_t format, enum picture_kind kind, uint32_t vertices)
{
    static const char *const kind_names[] = {"", "screen", "program", "", "world"};
    const unsigned long frame = fh_d3d_frame();
    unsigned i;

    for (i = 0; i < picture_count && pictures[i].hash != hash; i++)
        ;
    if (i == picture_count) {
        if (picture_count == MAX_PICTURES)
            return;
        picture_count++;
        pictures[i].hash = hash;
        pictures[i].format = format;
        pictures[i].last = 0;
        pictures[i].appearances = 0;
        pictures[i].kinds = 0;
    }
    if (pictures[i].last + PROMPT_GAP_FRAMES < frame || !pictures[i].appearances || !(pictures[i].kinds & kind)) {
        if (pictures[i].appearances)
            fprintf(stderr, "[prompt] frame %lu: 32 x 32 picture %016llX: on the screen (format 0x%02X; a %s draw of "
                    "%u vertices; %lu times before, last at frame %lu)\n", frame, (unsigned long long)hash, format,
                    kind_names[kind], vertices, pictures[i].appearances, pictures[i].last);
        else
            fprintf(stderr, "[prompt] frame %lu: 32 x 32 picture %016llX: on the screen (format 0x%02X; a %s draw of "
                    "%u vertices; the first time)\n", frame, (unsigned long long)hash, format, kind_names[kind],
                    vertices);
        pictures[i].appearances++;
        pictures[i].kinds |= kind;
    }
    pictures[i].last = frame;
}

/* [waw-decomp/fh19] 11-24: HD textures. game.hd_textures (FH_HD_TEXTURES) names a folder of <w>x<h>_<key>.png, where
 * w x h is a texture's level-0 size and key the FNV-1a 64 of the level-0 pixels texture_pixels leaves in scratch
 * (B8G8R8A8; the setup tool's ripper decodes the TXDs the same way). A texture whose name is in the folder is drawn
 * from the PNG instead, at the PNG's own size, its lower levels box-filtered from it. The folder is listed once, so a
 * texture that has no picture there never touches the disk. When every alpha of the game's own level 0 is 0, the
 * picture's alpha is set to 0 too (the ripper writes those opaque for the upscaler). game.hd_textures_log
 * (FH_HD_TEXTURES_LOG) prints each name looked up, with hit or miss. Linear textures (coordinates in texels) keep
 * their own pixels. Without SDL3 there is no PNG reader, and the setting is said to do nothing. */
#define HD_SLOTS 32768u                 /* a power of two: more than 4 times the 4,567 pictures the ripper finds */
static struct {
    int state;                          /* 0: not read yet; 1: on; -1: off */
    int log;
    char dir[1024];
    struct hd_name { uint64_t key; uint32_t width, height; } table[HD_SLOTS];  /* width 0: an empty slot */
    unsigned count;
    unsigned long hits, misses, failed;
} hd;

static unsigned hd_slot(uint64_t key, uint32_t width, uint32_t height)
{
    uint64_t h = (key ^ ((uint64_t)width << 32 | height)) * 0x9E3779B97F4A7C15ull;

    return (unsigned)(h >> 40) & (HD_SLOTS - 1);
}

/* 1 when the folder had <width>x<height>_<key>.png; with add, puts it in. */
static int hd_name(uint64_t key, uint32_t width, uint32_t height, int add)
{
    for (unsigned i = hd_slot(key, width, height), n = 0; n < HD_SLOTS; i = (i + 1) & (HD_SLOTS - 1), n++) {
        struct hd_name *e = &hd.table[i];

        if (!e->width) {
            if (!add)
                return 0;
            e->key = key;
            e->width = width;
            e->height = height;
            hd.count++;
            return 1;
        }
        if (e->key == key && e->width == width && e->height == height)
            return 1;
    }
    return 0;
}

static void hd_start(void)
{
    const char *dir = fh_setting_text("game.hd_textures", "FH_HD_TEXTURES");

    hd.state = -1;
    hd.log = fh_setting_bool("game.hd_textures_log", "FH_HD_TEXTURES_LOG", 0);
    if (!dir || !*dir)
        return;
#if defined(FH_HAVE_SDL3)
    {
        int n = 0, skipped = 0;
        char **names = SDL_GlobDirectory(dir, "*.png", SDL_GLOB_CASEINSENSITIVE, &n);
        size_t len;

        snprintf(hd.dir, sizeof hd.dir, "%s", dir);
        len = strlen(hd.dir);
        while (len > 1 && (hd.dir[len - 1] == '/' || hd.dir[len - 1] == '\\'))
            hd.dir[--len] = 0;
        for (int i = 0; names && i < n; i++) {
            const char *name = names[i], *slash = strrchr(name, '/'), *back = strrchr(name, '\\');
            unsigned width, height;
            unsigned long long key;
            int used = 0;

            if (slash && (!back || slash > back))
                name = slash + 1;
            else if (back)
                name = back + 1;
            if (sscanf(name, "%ux%u_%16llx%n", &width, &height, &key, &used) == 3 && used == (int)strlen(name) - 4 &&
                width && height && hd.count < HD_SLOTS / 2)
                hd_name(key, width, height, 1);
            else
                skipped++;
        }
        SDL_free(names);
        hd.state = 1;
        fprintf(stderr, "[fh] hd textures: %u pictures in %s%s%s\n", hd.count, hd.dir,
                skipped ? " (other files there left alone: " : "", skipped ? "see the names)" : "");
        if (!names)
            fprintf(stderr, "[fh] hd textures: %s could not be listed: %s\n", hd.dir, SDL_GetError());
    }
#else
    fprintf(stderr, "[fh] hd textures: this build has no PNG reader (no SDL3): %s is not read\n", dir);
#endif
}

/* The HD pack's texture for the level-0 pixels in scratch (width x height), its levels written; 0 for none. */
static uint32_t hd_texture(uint32_t width, uint32_t height)
{
    uint64_t key = 0xCBF29CE484222325ull;
    size_t bytes = (size_t)width * height * 4;
    int clear = 1, found;

    if (!hd.state)
        hd_start();
    if (hd.state < 0)
        return 0;
    for (size_t b = 0; b < bytes; b++)
        key = (key ^ scratch[b]) * 0x100000001B3ull;
    for (size_t b = 3; b < bytes && clear; b += 4)
        clear = !scratch[b];
    found = hd_name(key, width, height, 0);
    if (hd.log)
        fprintf(stderr, "[fh] hd: %ux%u_%016llx %s\n", width, height, (unsigned long long)key, found ? "hit" : "miss");
    if (!found) {
        hd.misses++;
        return 0;
    }
#if defined(FH_HAVE_SDL3)
    {
        char path[1200];
        SDL_Surface *png, *bgra;
        uint8_t *level, *next;
        uint32_t w, h, levels = 1, handle;

        snprintf(path, sizeof path, "%s/%ux%u_%016llx.png", hd.dir, width, height, (unsigned long long)key);
        png = SDL_LoadPNG(path);
        bgra = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_BGRA32) : NULL;   /* bytes B, G, R, A: B8G8R8A8 */
        if (png)
            SDL_DestroySurface(png);
        if (!bgra || bgra->w <= 0 || bgra->h <= 0 || bgra->w > 16384 || bgra->h > 16384) {
            if (hd.failed++ < 8)
                fprintf(stderr, "[fh] hd: %s not read: %s\n", path, SDL_GetError());
            if (bgra)
                SDL_DestroySurface(bgra);
            return 0;
        }
        w = (uint32_t)bgra->w;
        h = (uint32_t)bgra->h;
        while ((w >> levels) || (h >> levels))
            levels++;
        level = malloc((size_t)w * h * 4);
        next = malloc((size_t)(w / 2 ? w / 2 : 1) * (h / 2 ? h / 2 : 1) * 4);
        handle = level && next ? ngl_vk_low_texture(NGL_VK_LOW_2D, VK_FORMAT_B8G8R8A8_UNORM, w, h, 1, levels) : 0;
        if (handle) {
            for (uint32_t y = 0; y < h; y++)
                memcpy(level + (size_t)y * w * 4, (const uint8_t *)bgra->pixels + (size_t)y * bgra->pitch, (size_t)w * 4);
            if (clear)
                for (size_t b = 3; b < (size_t)w * h * 4; b += 4)
                    level[b] = 0;
            for (uint32_t l = 0; l < levels; l++) {
                uint32_t nw = w / 2 ? w / 2 : 1, nh = h / 2 ? h / 2 : 1;
                uint8_t *swap;

                if (ngl_vk_low_texture_write(handle, l, 0, level, (size_t)w * h * 4, w * 4)) {
                    ngl_vk_low_texture_free(handle);
                    handle = 0;
                    break;
                }
                if (l + 1 == levels)
                    break;
                for (uint32_t y = 0; y < nh; y++)     /* a box filter: the 2 x 2 above (1 x 2 or 2 x 1 at an edge) */
                    for (uint32_t x = 0; x < nw; x++) {
                        uint32_t x0 = 2 * x < w ? 2 * x : w - 1, y0 = 2 * y < h ? 2 * y : h - 1;
                        uint32_t x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;

                        for (int c = 0; c < 4; c++)
                            next[((size_t)y * nw + x) * 4 + c] =
                                (uint8_t)((level[((size_t)y0 * w + x0) * 4 + c] + level[((size_t)y0 * w + x1) * 4 + c] +
                                           level[((size_t)y1 * w + x0) * 4 + c] + level[((size_t)y1 * w + x1) * 4 + c] +
                                           2) / 4);
                    }
                swap = level;
                level = next;
                next = swap;
                w = nw;
                h = nh;
            }
        }
        free(level);
        free(next);
        SDL_DestroySurface(bgra);
        if (!handle) {
            hd.failed++;
            return 0;
        }
        hd.hits++;
        return handle;
    }
#else
    return 0;
#endif
}

/* ngl_vk_low's texture for the guest texture (and palette) a draw uses, made the first time; draw.white for none. */
static uint32_t texture_for(uint32_t texture, uint32_t palette)
{
    uint32_t data, format, size, width, height, handle, levels;
    unsigned i;

    if (!texture)
        return draw.white;
    data = MEM32(texture + 4);
    format = MEM32(texture + 12);
    size = MEM32(texture + 16);
    if (((format >> 8) & 0xFF) != 0x0B)
        palette = 0;                    /* only P8 textures read their palette */
    for (i = 0; i < texture_count; i++)
        if (textures[i].data == data && textures[i].format == format && textures[i].size == size &&
            textures[i].palette == (palette ? MEM32(palette + 4) : 0)) {
            textures[i].drawn = fh_d3d_frame();
            last_picture = textures[i].picture;
            last_picture_format = textures[i].picture_format;
            last_rect_width = textures[i].rect_width;
            last_rect_height = textures[i].rect_height;
            return textures[i].handle;
        }
    last_picture = 0;
    last_rect_width = last_rect_height = 0;
    if (!texture_pixels(texture, palette, 0, &width, &height))
        return draw.white;
    if (size) {
        last_rect_width = width;
        last_rect_height = height;
    }
    if (width == 32 && height == 32) {        /* a button picture's size: FNV-1a of its 32-bit pixels */
        uint64_t hash = 0xCBF29CE484222325ull;

        for (size_t b = 0; b < 32 * 32 * 4; b++)
            hash = (hash ^ scratch[b]) * 0x100000001B3ull;
        last_picture = hash ? hash : 1;
        last_picture_format = (format >> 8) & 0xFF;
    }
    handle = size ? 0 : hd_texture(width, height);   /* [waw-decomp/fh19] the HD pack's picture, if it has one */
    levels = handle ? 0 : texture_levels(texture);    /* 0: hd_texture wrote its own levels */
    if (!handle)
        handle = ngl_vk_low_texture(NGL_VK_LOW_2D, VK_FORMAT_B8G8R8A8_UNORM, width, height, 1, levels);
    if (!handle)
        return draw.white;
    if (levels && ngl_vk_low_texture_write(handle, 0, 0, scratch, (size_t)width * height * 4, width * 4)) {
        ngl_vk_low_texture_free(handle);    /* not drawn with yet */
        return draw.white;
    }
    for (uint32_t level = 1; level < levels; level++) {     /* scratch is copied at each write */
        uint32_t level_width, level_height;

        if (!texture_pixels(texture, palette, level, &level_width, &level_height) ||
            ngl_vk_low_texture_write(handle, level, 0, scratch, (size_t)level_width * level_height * 4,
                                     level_width * 4))
            break;
    }
    if (texture_count == MAX_TEXTURES) {     /* full: the one drawn longest ago goes, if not drawn this frame */
        unsigned oldest = 0;

        for (i = 1; i < texture_count; i++)
            if (textures[i].drawn < textures[oldest].drawn)
                oldest = i;
        if (textures[oldest].drawn < fh_d3d_frame()) {
            forget_texture(oldest);
            textures_dropped++;
        }
    }
    if (texture_count < MAX_TEXTURES) {
        textures[texture_count].data = data;
        textures[texture_count].format = format;
        textures[texture_count].size = size;
        textures[texture_count].palette = palette ? MEM32(palette + 4) : 0;
        textures[texture_count].handle = handle;
        textures[texture_count].drawn = fh_d3d_frame();
        textures[texture_count].picture = last_picture;
        textures[texture_count].picture_format = last_picture_format;
        textures[texture_count].rect_width = last_rect_width;
        textures[texture_count].rect_height = last_rect_height;
        texture_count++;
    } else {
        free_later(handle);                 /* 1024 drawn this frame: drawn once, then let go of */
    }
    return handle;
}

/* ------------------------------------------------------------------------------------------------------------------
 * Pipelines and constants */

/* The Xbox's compare and blend values are OpenGL's (0x200 + n for compares; 0x300-0x308 for blend factors); Vulkan
 * numbers them differently. */
static enum ngl_vk_low_compare compare_from_xbox(uint32_t value)
{
    return value >= 0x200 && value <= 0x207 ? (enum ngl_vk_low_compare)(value - 0x200) : NGL_VK_LOW_ALWAYS;
}

static enum ngl_vk_low_blend_factor blend_from_xbox(uint32_t value)
{
    switch (value) {
    case 0x000: return NGL_VK_LOW_ZERO;
    case 0x001: return NGL_VK_LOW_ONE;
    case 0x300: return NGL_VK_LOW_SRC_COLOR;
    case 0x301: return NGL_VK_LOW_ONE_MINUS_SRC_COLOR;
    case 0x302: return NGL_VK_LOW_SRC_ALPHA;
    case 0x303: return NGL_VK_LOW_ONE_MINUS_SRC_ALPHA;
    case 0x304: return NGL_VK_LOW_DST_ALPHA;
    case 0x305: return NGL_VK_LOW_ONE_MINUS_DST_ALPHA;
    case 0x306: return NGL_VK_LOW_DST_COLOR;
    case 0x307: return NGL_VK_LOW_ONE_MINUS_DST_COLOR;
    default:
        /* [fh5] 0x308 SRC_ALPHA_SATURATE and 0x8001-0x8004 the constant color (BLENDCOLOR, 75) have no factor in
         * ngl_vk_low yet: said once each run, so we know whether the game uses them */
        if (value && value != 1) {
            static unsigned said;
            if (said++ < 4)
                fprintf(stderr, "[fh] draw: blend factor 0x%X not written yet: drawn as ONE\n", value);
        }
        return NGL_VK_LOW_ONE;
    }
}

static enum ngl_vk_low_blend_op blend_op_from_xbox(uint32_t value)
{
    switch (value) {
    case 0x800A: return NGL_VK_LOW_SUBTRACT;
    case 0x800B: return NGL_VK_LOW_REVERSE_SUBTRACT;
    case 0x8007: return NGL_VK_LOW_MIN;
    case 0x8008: return NGL_VK_LOW_MAX;
    default: return NGL_VK_LOW_ADD;     /* 0x8006 */
    }
}

/* [fh5] 2026-11-24 The Xbox's stencil operations are OpenGL's numbers (0 zero, 0x1E00 keep, 0x1E01 replace,
 * 0x1E02/0x1E03 increase/decrease and stop at the ends, 0x150A invert, 0x8507/0x8508 increase/decrease and wrap);
 * these are VkStencilOp's. */
static uint32_t stencil_op_from_xbox(uint32_t value)
{
    switch (value) {
    case 0x0000: return 1;      /* ZERO */
    case 0x1E01: return 2;      /* REPLACE */
    case 0x1E02: return 3;      /* INCREMENT_AND_CLAMP */
    case 0x1E03: return 4;      /* DECREMENT_AND_CLAMP */
    case 0x150A: return 5;      /* INVERT */
    case 0x8507: return 6;      /* INCREMENT_AND_WRAP */
    case 0x8508: return 7;      /* DECREMENT_AND_WRAP */
    default: return 0;          /* KEEP (0x1E00) */
    }
}

/* [acct2-fh] 2026-11-14 Culling as the game's own CULLMODE handler (0x00222730) sets it: none for 0; otherwise the
 * NV2A culls the face (front or back, against FRONTFACE, state 146) whose winding is the CULLMODE value, so whatever
 * FRONTFACE is, D3DCULL_CW drops the triangles that run clockwise on the screen and D3DCULL_CCW the counterclockwise
 * ones. ngl_vk_low's front face is clockwise, as Direct3D's. */
static enum ngl_vk_low_cull cull_from_xbox(uint32_t value)
{
    int way = draw.cull;
    unsigned long frame;

    if (draw.cull_probe) {
        frame = fh_d3d_frame();
        if (frame == draw.cull_probe + 1)
            way = -1;
        else if (frame == draw.cull_probe + 2)
            way = 0;
    }
    if (!way || (value != CULL_CW && value != CULL_CCW))
        return NGL_VK_LOW_CULL_NONE;
    if (way < 0)
        value = value == CULL_CW ? CULL_CCW : CULL_CW;
    return value == CULL_CW ? NGL_VK_LOW_CULL_FRONT : NGL_VK_LOW_CULL_BACK;
}

/* The pipeline state from the game's render states. [acct2-fh] 2026-11-14: the depth test as D3DRS_ZENABLE (143) says
 * (its handler 0x002235B0 turns the NV2A's test on for any value but 0 when there is a depth buffer; 2, the W-buffer,
 * is drawn as the Z-buffer), with ZFUNC's compare (ALWAYS too: a test that always passes still writes the depth
 * when ZWRITEENABLE is on); before build36 the test was on unless the compare was ALWAYS. Culling from CULLMODE
 * (cull_from_xbox), and the game's ZBIAS as its handler (0x00222970) turns it into the polygon offset:
 * SOLIDOFFSETENABLE (81) on, ZOFFSET (78) the constant part in the depth buffer's steps and ZSLOPESCALE (77) the
 * slope part, both negative for a bias towards the eye; Vulkan's depth bias takes the same two numbers. */
static void pipeline_state(struct ngl_vk_low_state *s, enum ngl_vk_low_topology topology)
{
    const uint32_t write = render_state(RS_COLORWRITEENABLE);

    memset(s, 0, sizeof *s);
    s->blend = render_state(RS_ALPHABLENDENABLE) != 0;
    s->color_src = s->alpha_src = blend_from_xbox(render_state(RS_SRCBLEND));
    s->color_dst = s->alpha_dst = blend_from_xbox(render_state(RS_DESTBLEND));
    s->color_op = s->alpha_op = blend_op_from_xbox(render_state(RS_BLENDOP));
    /* D3DCOLORWRITEENABLE: red 1 << 16, green 1 << 8, blue 1, alpha 1 << 24 */
    s->color_mask = ((write >> 16) & 1) | (((write >> 8) & 1) << 1) | ((write & 1) << 2) | (((write >> 24) & 1) << 3);
    if (!s->color_mask)
        s->no_color_write = 1;
    s->depth_compare = compare_from_xbox(render_state(RS_ZFUNC));
    if (draw.z_enable)
        s->depth_test = render_state(RS_ZENABLE) != 0;
    else
        s->depth_test = s->depth_compare != NGL_VK_LOW_ALWAYS;
    s->depth_write = s->depth_test && render_state(RS_ZWRITEENABLE) != 0;
    s->cull = topology == NGL_VK_LOW_TRIANGLE_LIST || topology == NGL_VK_LOW_TRIANGLE_STRIP
                  ? cull_from_xbox(render_state(RS_CULLMODE))
                  : NGL_VK_LOW_CULL_NONE;
    if (draw.depth_bias && render_state(RS_SOLIDOFFSETENABLE)) {
        s->depth_bias = as_float(render_state(RS_POLYGONOFFSETZOFFSET));
        s->depth_bias_slope = as_float(render_state(RS_POLYGONOFFSETZSLOPESCALE));
        if (!(s->depth_bias == s->depth_bias) || !(s->depth_bias_slope == s->depth_bias_slope))
            s->depth_bias = s->depth_bias_slope = 0.0f;   /* not numbers: none */
    }
    /* [fh5] the stencil test: the game clears the stencil with the depth (Flags 0x3) in the middle of frames, so
     * something in it draws with the stencil; before this the test was always off. */
    if (draw.stencil && render_state(RS_STENCILENABLE)) {
        s->stencil = 1;
        s->stencil_compare = compare_from_xbox(render_state(RS_STENCILFUNC));
        s->stencil_fail = stencil_op_from_xbox(render_state(RS_STENCILFAIL));
        s->stencil_depth_fail = stencil_op_from_xbox(render_state(RS_STENCILZFAIL));
        s->stencil_pass = stencil_op_from_xbox(render_state(RS_STENCILPASS));
        s->stencil_reference = render_state(RS_STENCILREF) & 0xFF;
        s->stencil_read_mask = render_state(RS_STENCILMASK) & 0xFF;
        s->stencil_write_mask = render_state(RS_STENCILWRITEMASK) & 0xFF;
        draw.stencil_draws++;
    }
    s->topology = topology;
}

/* The vertex layout of an FVF with position, diffuse color and texture coordinates: the byte offsets of the three
 * inputs the shader reads. Returns 0 for an FVF the shader can't draw yet. */
static int fvf_layout(uint32_t fvf, uint32_t stride, struct ngl_vk_low_layout *layout)
{
    uint32_t offset, diffuse_at, uv_at;

    if ((fvf & FVF_POSITION_MASK) != FVF_XYZ || !(fvf & FVF_DIFFUSE) || ((fvf >> 8) & 0xF) == 0)
        return 0;
    offset = 12;                                /* x, y, z */
    if (fvf & FVF_NORMAL)
        offset += 12;
    if (fvf & FVF_PSIZE)
        offset += 4;
    diffuse_at = offset;
    offset += 4;
    if (fvf & FVF_SPECULAR)
        offset += 4;
    uv_at = offset;                             /* the first coordinate set; its size bits (16-17) 0 mean two floats */
    if ((fvf >> 16) & 3)
        return 0;
    memset(layout, 0, sizeof *layout);
    layout->attributes[0] = (struct ngl_vk_low_attribute){0, 0, 0, VK_FORMAT_R32G32B32_SFLOAT};
    layout->attributes[1] = (struct ngl_vk_low_attribute){1, 0, diffuse_at, VK_FORMAT_R8G8B8A8_UNORM};
    layout->attributes[2] = (struct ngl_vk_low_attribute){2, 0, uv_at, VK_FORMAT_R32G32_SFLOAT};
    /* [acct2-fh] 11-14: fixed.vert's fog input; these draws take the depth instead (c4.x 0), so it reads x unused */
    layout->attributes[3] = (struct ngl_vk_low_attribute){3, 0, 0, VK_FORMAT_R32_SFLOAT};
    /* [fh8] fixed.vert's normal (location 4): after x, y, z when the FVF has one; otherwise the position, unused
     * (c5.x 0: not lit) */
    layout->attributes[4] = (struct ngl_vk_low_attribute){4, 0, (fvf & FVF_NORMAL) ? 12 : 0,
                                                          VK_FORMAT_R32G32B32_SFLOAT};
    layout->attribute_count = 5;
    layout->stride[0] = stride;
    layout->stream_count = 1;
    return 1;
}

/* out = a x b for Direct3D's row-major 4 x 4 matrices */
static void multiply(float out[16], const float a[16], const float b[16])
{
    float r[16];

    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            r[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] + a[row * 4 + 1] * b[1 * 4 + col] +
                               a[row * 4 + 2] * b[2 * 4 + col] + a[row * 4 + 3] * b[3 * 4 + col];
    memcpy(out, r, sizeof r);
}

/* How a draw's vertices give fixed.frag their fog distance (or factor), for pixel_constants and fixed.vert's c4. */
enum fog_source { FOG_NONE, FOG_FROM_DEPTH, FOG_FROM_VERTEX };

#define PIXEL_CONSTANTS         8               /* fixed.frag's registers: c0-c7 */
#define VERTEX_CONSTANTS        67              /* fixed.vert's: c0-c4; [fh8] c5-c66 the lighting */
#define UNLIT_CONSTANTS         6               /* [fh8] an unlit draw sends c0-c5 (c5.x 0: not lit) */
#define LIGHT_AT                19              /* [fh8] the first light's six registers */

/* [fh9] Render states this file does not draw yet, with their usual value (the XDK's defaults; 0 too, for states the
 * game's array may hold before the device sets them): the draws with another value are counted and the first few
 * logged, to find the ones that matter. Indices in the XDK's order (SetRenderState's table at 0x0022E5D0 starts at
 * 136: VERTEXBLEND 137, FILLMODE 139, BACKFILLMODE 140, TWOSIDEDLIGHTING 141).
 * [fh11] FRONTFACE (146) left out: the r10 runs found it 0x901 on every draw, and it changes nothing we draw. Its
 * handler (0x002227A0) writes NV2A's front face and calls CULLMODE's (0x00222730), which culls GL_FRONT when CULLMODE
 * equals FRONTFACE and GL_BACK otherwise: the triangles culled are always those of CULLMODE's winding, as
 * cull_from_xbox() draws (see above). It would matter only for two-sided lighting, which is never on. */
#define UNHANDLED_STATES 9
static const struct { uint16_t state; uint32_t usual; const char *name; } unhandled_states[UNHANDLED_STATES] = {
    {66, 0x1D01, "flat shading (SHADEMODE)"}, {139, 0x1B02, "FILLMODE"}, {140, 0x1B02, "BACKFILLMODE"},
    {137, 0, "VERTEXBLEND"}, {141, 0, "TWOSIDEDLIGHTING"}, {118, 0, "POINTSPRITEENABLE"}, {98, 0, "WRAP0"},
    {150, 0, "LOGICOP"}, {151, 0, "EDGEANTIALIAS"}};

/* A D3DCOLOR's red, green, blue and alpha as four floats 0-1. */
static void color_floats(uint32_t color, float out[4])
{
    out[0] = (float)((color >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((color >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(color & 0xFF) / 255.0f;
    out[3] = (float)(color >> 24) / 255.0f;
}

/* [fh8] Fixed-function lighting, as Direct3D defines it, done in fixed.vert in the eye's space. Fills c5-c66 and
 * returns how many registers the draw sends:
 *   c5 x 1 lit, y 1 normalize the normals (NORMALIZENORMALS), z the lights on; c6 x, y, w 1: the diffuse, ambient,
 *   emissive colors come from the vertex's diffuse color (COLORVERTEX and the material sources), else the material;
 *   c7-c10 world x view's rows; c11-c13 the normals' matrix (world x view's 3 x 3, inverted and transposed);
 *   c14 the global ambient (AMBIENT); c15-c18 the material's diffuse, ambient, specular, emissive;
 *   from c19 six registers a light: diffuse, ambient, position (in the eye's space; w the type), direction (the
 *   eye's space, length 1; w the range), attenuation 0-2 and falloff, cos(theta / 2) and cos(phi / 2). */
static unsigned lighting_constants(const struct fh_d3d_state *st, int vertex_diffuse, float *vs)
{
    const float *view = st->transforms[TRANSFORM_VIEW];
    float wv[16], det, cross[3][3];
    unsigned lights = 0;
    const uint32_t color_vertex = render_state(RS_COLORVERTEX) && vertex_diffuse;

    memset(&vs[20], 0, (size_t)(VERTEX_CONSTANTS - 5) * 4 * sizeof(float));
    multiply(wv, st->transforms[TRANSFORM_WORLD], view);
    memcpy(&vs[28], wv, sizeof wv);
    /* the inverse's transpose is the cofactors over the determinant; a row's cofactors are the other two rows'
     * cross product */
    for (int r = 0; r < 3; r++) {
        const float *a = &wv[((r + 1) % 3) * 4], *b = &wv[((r + 2) % 3) * 4];

        cross[r][0] = a[1] * b[2] - a[2] * b[1];
        cross[r][1] = a[2] * b[0] - a[0] * b[2];
        cross[r][2] = a[0] * b[1] - a[1] * b[0];
    }
    det = wv[0] * cross[0][0] + wv[1] * cross[0][1] + wv[2] * cross[0][2];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            vs[44 + r * 4 + c] = (det > 1e-12f || det < -1e-12f) ? cross[r][c] / det : wv[r * 4 + c];
    color_floats(render_state(RS_AMBIENT), &vs[56]);
    for (int k = 0; k < 16; k++)
        vs[60 + k] = as_float(st->material[k]);
    for (int i = 0; i < FH_D3D_LIGHTS; i++) {
        const uint32_t *l = st->lights[i];
        float *c = &vs[(LIGHT_AT + lights * 6) * 4];
        float p[3], d[3], length;

        if (!st->light_on[i] || !st->light_set[i] || l[0] < 1 || l[0] > 3)
            continue;
        for (int k = 0; k < 4; k++) {
            c[k] = as_float(l[1 + k]);          /* diffuse */
            c[4 + k] = as_float(l[9 + k]);      /* ambient */
        }
        for (int k = 0; k < 3; k++) {
            p[k] = as_float(l[13 + k]);
            d[k] = as_float(l[16 + k]);
        }
        for (int k = 0; k < 3; k++) {
            c[8 + k] = p[0] * view[k] + p[1] * view[4 + k] + p[2] * view[8 + k] + view[12 + k];
            c[12 + k] = d[0] * view[k] + d[1] * view[4 + k] + d[2] * view[8 + k];
        }
        length = sqrtf(c[12] * c[12] + c[13] * c[13] + c[14] * c[14]);
        if (length > 0.0f)
            c[12] /= length, c[13] /= length, c[14] /= length;
        c[11] = (float)l[0];
        c[15] = as_float(l[19]);                /* range */
        c[16] = as_float(l[21]);
        c[17] = as_float(l[22]);
        c[18] = as_float(l[23]);
        c[19] = as_float(l[20]);                /* falloff */
        c[20] = cosf(as_float(l[24]) * 0.5f);
        c[21] = cosf(as_float(l[25]) * 0.5f);
        lights++;
    }
    vs[20] = 1.0f;
    vs[21] = render_state(RS_NORMALIZENORMALS) ? 1.0f : 0.0f;
    vs[22] = (float)lights;
    vs[24] = color_vertex && render_state(RS_DIFFUSEMATERIALSOURCE) == 1 ? 1.0f : 0.0f;
    vs[25] = color_vertex && render_state(RS_AMBIENTMATERIALSOURCE) == 1 ? 1.0f : 0.0f;
    vs[27] = color_vertex && render_state(RS_EMISSIVEMATERIALSOURCE) == 1 ? 1.0f : 0.0f;
    return LIGHT_AT + lights * 6;
}

/* The pixel constants for texture stage 0, the alpha test and the fog (shaders/fixed.frag). */
/* [fh6] textured: 1 a texture is read, 0 none is set (a white texel), 2 one is set that could not be read here (a
 * format not written yet, a failed screen copy): its texel reads 0, as a stage whose texture can't be fetched does in
 * the other recompilation's renderer, so the draw adds nothing rather than drawing a flat white shape. FH_UNREAD=0
 * draws those white, as before. */
static void pixel_constants(float c[4 * PIXEL_CONSTANTS], int textured, enum fog_source fog, const float uv[4])
{
    const uint32_t table = render_state(RS_FOGTABLEMODE);

    memset(c, 0, 4 * PIXEL_CONSTANTS * sizeof(float));
    memcpy(&c[28], uv, 4 * sizeof(float));  /* [fh4] c7: stage 0's coordinates scaled (x, y) and moved (z, w) */
    c[0] = (float)stage_state(0, TSS_COLOROP);
    c[1] = (float)stage_state(0, TSS_COLORARG1);
    c[2] = (float)stage_state(0, TSS_COLORARG2);
    c[3] = (float)stage_state(0, TSS_ALPHAOP);
    c[4] = (float)stage_state(0, TSS_ALPHAARG1);
    c[5] = (float)stage_state(0, TSS_ALPHAARG2);
    c[6] = (float)textured;
    c[7] = textured == 1 && stage_state(0, TSS_ALPHAKILL) ? 1.0f : 0.0f;   /* [fh5] c1.w: texels of alpha 0 are dropped */
    c[21] = (float)stage_state(0, TSS_COLORARG0);   /* [fh5] c5.y, c5.z: the third arguments (MULTIPLYADD, LERP) */
    c[22] = (float)stage_state(0, TSS_ALPHAARG0);
    c[23] = draw.project ? 1.0f : 0.0f;          /* [fh6] c5.w: divide the coordinates by q (fixed.frag) */
    c[8] = render_state(RS_ALPHATESTENABLE) ? 1.0f : 0.0f;
    c[9] = (float)compare_from_xbox(render_state(RS_ALPHAFUNC));
    c[10] = (float)(render_state(RS_ALPHAREF) & 0xFF) / 255.0f;
    color_floats(render_state(RS_TEXTUREFACTOR), &c[12]);  /* [acct2-fh] 11-14: its slot found (148) */
    /* the fog (the top of this file): c4 on, the table mode, start, end; c5 the density; c6 the color */
    if (draw.fog && render_state(RS_FOGENABLE) && fog != FOG_NONE && (table != 0 || fog == FOG_FROM_VERTEX)) {
        c[16] = 1.0f;
        c[17] = (float)(table <= 3 ? table : 0);
        c[18] = as_float(render_state(RS_FOGSTART));
        c[19] = as_float(render_state(RS_FOGEND));
        c[20] = as_float(render_state(RS_FOGDENSITY));
        color_floats(render_state(RS_FOGCOLOR), &c[24]);
        c[27] = 0.0f;
    }
}

/* [fh11] The pain effect (sub_0012A800 -> sub_001298C0) draws the picture it read back over each new frame with
 * SRCALPHA / INVSRCALPHA and a vertex alpha of 0xCC (0.8: the r12 runs' log), so each drawn frame keeps 80% of the
 * last: a smear whose length is counted in drawn frames. The Xbox drew 60 (or 30) a second; here a frame can be a
 * 20th of a second or a 200th, and the smear lasted three times longer at 20 frames a second and a third as long
 * above 60. The project's rule is that the game behaves alike at any frame rate, so the alpha of such a draw is
 * raised to the frame's length in 60ths (fixed.frag, c2.w): 0.8 per 60th of a second at any rate (0.8^3 = 0.51 a
 * frame at 20, 0.8^(1/3) = 0.93 at 180). A frame after a pause (over a quarter second, or not the next frame) counts
 * as one 60th. FH_BLUR_BY_TIME=0 keeps the per-frame 0.8. */
static float screen_blend_exponent(void)
{
    static unsigned long last_frame;
    static uint64_t last_ns;
    static int seen, on = -1;
    static float n = 1.0f;
    const unsigned long frame = fh_d3d_frame();
    const uint64_t t = now_ns();

    if (on < 0) {
        const char *e = getenv("FH_BLUR_BY_TIME");
        on = !(e && *e == '0');
    }
    if (!on)
        return 0.0f;
    if (!seen || frame != last_frame) {
        const double dt = seen ? (double)(t - last_ns) / 1e9 : 0.0;
        n = (!seen || frame != last_frame + 1 || dt <= 0.0 || dt > 0.25) ? 1.0f : (float)(dt * 60.0);
        if (n < 0.05f)
            n = 0.05f;
        if (n > 5.0f)
            n = 5.0f;
        if (seen == 1 && frame == last_frame + 1)
            fprintf(stderr, "[fh] draw: the read-back picture's alpha raised to %.2f (this frame's length in 60ths; "
                    "FH_BLUR_BY_TIME=0 not)\n", (double)n);
        seen = seen < 2 ? seen + 1 : 2;
        last_frame = frame;
        last_ns = t;
    }
    return n;
}

/* The sampler from stage 0's filters and address modes (D3DTEXF: 0 none, 1 point, 2 linear; D3DTADDRESS: 1 wrap,
 * 2 mirror, 3 clamp, 4 border). MIPFILTER none samples the first level alone, point the nearest level, linear (or
 * better) blends two; ngl_vk_low's mip: 2 none, 0 nearest, 1 linear. */
static uint32_t sampler_for_stage0(void)
{
    const uint32_t filter = stage_state(0, TSS_MAGFILTER) == 1 && stage_state(0, TSS_MINFILTER) == 1 ? 0 : 1;
    const uint32_t mipfilter = stage_state(0, TSS_MIPFILTER);
    const uint32_t mip = !draw.mips || mipfilter == 0 ? 2 : mipfilter == 1 ? 0 : 1;
    uint32_t u = stage_state(0, TSS_ADDRESSU), v = stage_state(0, TSS_ADDRESSV);

    float anisotropy = 1.0f;

    /* D3DTADDRESS 1 wrap, 2 mirror, 3 clamp, 4 border, [fh10] 5 clamp to edge (the Xbox's own; was read as wrap) */
    u = u >= 1 && u <= 4 ? u - 1 : u == 5 ? 2 : 0;
    v = v >= 1 && v <= 4 ? v - 1 : v == 5 ? 2 : 0;
    /* [fh10] the anisotropic filter, with the stage's most */
    if (stage_state(0, TSS_MINFILTER) == TEXF_ANISOTROPIC || stage_state(0, TSS_MAGFILTER) == TEXF_ANISOTROPIC) {
        const uint32_t most = stage_state(0, TSS_MAXANISOTROPY);

        anisotropy = most > 1 ? (float)(most > 16 ? 16 : most) : 1.0f;
    }
    if (stage_state(0, TSS_MIPMAPLODBIAS) || stage_state(0, TSS_MAXMIPLEVEL))
        draw.lod_draws++;
    return ngl_vk_low_sampler(filter, mip, u, v, 0, anisotropy, 0);
}

/* ------------------------------------------------------------------------------------------------------------------
 * The frame */

/* [fh15] 2026-11-24 In-between drawing above 60 (game.draw_between; the project's any-FPS rule, after CoD3's
 * FRAMERATE-AUDIT): above 60 frames a second the game updates in whole 60ths (game.fixed_step_above_60) and the
 * frames between are held frames, drawn again by fh_frame_clock.c with the same state, so the picture moved 60 times
 * a second whatever the frame rate. Each draw's vertex constants (fixed function: world x view x projection and the
 * lights; a vertex program: the game's 192 registers, which hold its camera and bones) are kept from the last two
 * updates, keyed by what the draw draws (its vertices, indices, count, texture, shader) and the how-many-th such draw
 * of the frame it is. While held frames are being drawn every frame draws its constants part way from the last
 * update's to this one's, by the time since this update over the time between the two (so 1 update of lag, as any
 * interpolation has). A draw with no match in the last update, or whose constants jumped (a camera cut, two draws of
 * one model swapped), is drawn as it is. Screen draws (XYZRHW) are left alone. */
#define BETWEEN_SLOTS 4096                              /* a power of 2: twice the draws a frame, at least */
#define BETWEEN_FLOATS (FH_VSH_CONSTANT_REGISTERS * 4)
#define BETWEEN_POOL (2048u * BETWEEN_FLOATS)

struct between_entry {
    uint64_t key;
    uint32_t generation;
    uint32_t floats;
    uint32_t at;                                        /* in the pool */
};

static struct {
    int setting;                                        /* -1 not read yet */
    struct between_entry *table[2];
    float *pool[2];
    uint32_t pool_used[2], generation[2];
    int cur;                                            /* this update's table; the last update's is cur ^ 1 */
    struct { uint64_t sig; uint32_t frame, n; } *seen; /* how many draws of a kind this frame */
    uint32_t frame;
    int64_t update_ns, last_update_ns, held_ns, report_ns;
    int update_frame, active;
    float alpha;
    unsigned long lerped, snapped, missing, stored, frames_active;
} between = {-1};

static uint64_t between_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

static void between_frame_begin(void)
{
    const int64_t now = (int64_t)fh_host_now_ns();

    if (between.setting < 0) {
        between.setting = fh_setting_bool("game.draw_between", "FH_DRAW_BETWEEN", 1);
        if (between.setting) {
            for (int i = 0; i < 2; i++) {
                between.table[i] = calloc(BETWEEN_SLOTS, sizeof *between.table[i]);
                between.pool[i] = malloc((size_t)BETWEEN_POOL * sizeof(float));
            }
            between.seen = calloc(BETWEEN_SLOTS, sizeof *between.seen);
            if (!between.table[0] || !between.table[1] || !between.pool[0] || !between.pool[1] || !between.seen)
                between.setting = 0;
        }
        fprintf(stderr, "[fh] in-between drawing above 60: %s (game.draw_between)\n", between.setting ? "on" : "off");
    }
    if (!between.setting)
        return;
    between.frame++;
    if (fh_frame_clock_drawing_held()) {
        between.update_frame = 0;
        between.held_ns = now;
    } else {
        between.update_frame = 1;
        between.cur ^= 1;
        between.generation[between.cur]++;
        between.pool_used[between.cur] = 0;
        between.last_update_ns = between.update_ns;
        between.update_ns = now;
    }
    {
        const int64_t span = between.update_ns - between.last_update_ns;

        between.active = between.last_update_ns && between.held_ns && now - between.held_ns < 250000000 &&
                         span > 0 && span < 100000000;
        between.alpha = 1.0f;
        if (between.active) {
            const double a = (double)(now - between.update_ns) / (double)span;

            between.alpha = a < 0 ? 0.0f : a > 1 ? 1.0f : (float)a;
            between.frames_active++;
        }
    }
    if (!between.report_ns)
        between.report_ns = now;
    if (now - between.report_ns >= 5000000000ll) {
        if (between.frames_active)
            fprintf(stderr, "[fh] in-between drawing: %lu frames, %lu draws drawn between, %lu drawn as they are "
                    "(jumped), %lu with no match; %lu kept\n", between.frames_active, between.lerped, between.snapped,
                    between.missing, between.stored);
        between.report_ns = now;
        between.frames_active = between.lerped = between.snapped = between.missing = between.stored = 0;
    }
}

static struct between_entry *between_find(int which, uint64_t key, int add)
{
    struct between_entry *table = between.table[which];
    const uint32_t generation = between.generation[which];

    for (uint32_t i = 0, at = (uint32_t)(key >> 20) & (BETWEEN_SLOTS - 1); i < BETWEEN_SLOTS / 2;
         i++, at = (at + 1) & (BETWEEN_SLOTS - 1)) {
        struct between_entry *e = &table[at];

        if (e->generation != generation) {
            if (!add)
                return NULL;
            e->generation = generation;
            e->key = key;
            e->floats = 0;
            return e;
        }
        if (e->key == key)
            return e;
    }
    return NULL;
}

static void between_constants(float *c, unsigned registers, uint64_t sig)
{
    uint32_t n = 0, floats = registers * 4;
    uint64_t key;
    struct between_entry *e;

    if (!between.setting || floats > BETWEEN_FLOATS)
        return;
    /* the how-many-th draw of this kind this frame */
    for (uint32_t i = 0, at = (uint32_t)(sig >> 24) & (BETWEEN_SLOTS - 1); i < BETWEEN_SLOTS / 2;
         i++, at = (at + 1) & (BETWEEN_SLOTS - 1)) {
        if (between.seen[at].frame != between.frame) {
            between.seen[at].frame = between.frame;
            between.seen[at].sig = sig;
            between.seen[at].n = 0;
        }
        if (between.seen[at].sig == sig) {
            n = between.seen[at].n++;
            break;
        }
    }
    key = between_mix(sig, n);
    if (between.update_frame && between.pool_used[between.cur] + floats <= BETWEEN_POOL &&
        (e = between_find(between.cur, key, 1)) != NULL && !e->floats) {
        e->at = between.pool_used[between.cur];
        e->floats = floats;
        memcpy(&between.pool[between.cur][e->at], c, floats * sizeof(float));
        between.pool_used[between.cur] += floats;
        between.stored++;
    }
    if (!between.active || between.alpha >= 1.0f)
        return;
    e = between_find(between.cur ^ 1, key, 0);
    if (!e || e->floats != floats) {
        between.missing++;
        return;
    }
    {
        const float *p = &between.pool[between.cur ^ 1][e->at];
        float most = 0.0f, moved = 0.0f;

        for (uint32_t i = 0; i < floats; i++) {
            const float m = fabsf(c[i]), d = fabsf(c[i] - p[i]);

            if (m > most)
                most = m;
            if (d > moved)
                moved = d;
        }
        if (!(moved <= 0.5f * most + 1e-6f)) {     /* a jump (or a NaN): drawn as it is */
            between.snapped++;
            return;
        }
        for (uint32_t i = 0; i < floats; i++)
            c[i] = p[i] + (c[i] - p[i]) * between.alpha;
        between.lerped++;
    }
}

static int frame_begin(void)
{
    struct ngl_vk_low_pass pass;

    if (draw.frame_open)
        return 1;
    ngl_vk_low_frame_begin();
    free_forgotten_textures();
    memset(&pass, 0, sizeof pass);
    pass.color[0] = ngl_vk_low_main_color();
    pass.color_count = 1;
    pass.depth = ngl_vk_low_main_depth();
    pass.clear_color = 1;
    pass.clear_rgba[3] = 1.0f;
    pass.clear_depth = 1;
    pass.clear_depth_value = 1.0f;
    pass.label = "Finest Hour frame";
    if (ngl_vk_low_begin_pass(&pass)) {
        ngl_vk_low_frame_end();
        return 0;
    }
    draw.frame_open = 1;
    draw.frame_first_drawn = draw.drawn;
    between_frame_begin();    /* [fh15] */
    return 1;
}

/* [acct2-fh] 2026-11-13 Where the game's 640 x 480 screen lies on ngl_vk's picture (RESOLUTION.md, stage 3). The 3D
 * draws take the whole picture: their view is the picture's shape (fh_aspect.c widens the game's camera). A 2D draw
 * (flat: the HUD, the menus, the films) takes the screen's own 4:3 part in the middle when the picture is another
 * shape: the whole height of a wider picture, the whole width of a narrower one. That part is the same share of
 * the picture's width or height whether the picture is shown with bars or stretched over the window. */
static void screen_part(int flat, float *x, float *y, float *width, float *height)
{
    const float shape = fh_aspect_shape(), game = (float)draw.game_width / (float)draw.game_height;

    *x = 0.0f;
    *y = 0.0f;
    *width = (float)draw.target_width;
    *height = (float)draw.target_height;
    if (!flat || fabsf(shape / game - 1.0f) < 0.001f)
        return;
    if (shape > game) {
        *width *= game / shape;
        *x = ((float)draw.target_width - *width) * 0.5f;
    } else {
        *height *= shape / game;
        *y = ((float)draw.target_height - *height) * 0.5f;
    }
}

/* The game's viewport (D3DVIEWPORT8, in the 640 x 480 back buffer) on ngl_vk's picture: across the whole picture,
 * or its middle 4:3 part for a 2D draw (screen_part). */
static void set_viewport(const struct fh_d3d_state *st, int flat)
{
    float part_x, part_y, part_width, part_height, sx, sy;
    uint32_t w = st->viewport[2] ? st->viewport[2] : draw.game_width;
    uint32_t h = st->viewport[3] ? st->viewport[3] : draw.game_height;

    screen_part(flat, &part_x, &part_y, &part_width, &part_height);
    sx = part_width / (float)draw.game_width;
    sy = part_height / (float)draw.game_height;
    ngl_vk_low_viewport(part_x + st->viewport[0] * sx, part_y + st->viewport[1] * sy, w * sx, h * sy,
                        st->viewport_depth[0],
                        st->viewport_depth[1] > st->viewport_depth[0] ? st->viewport_depth[1] : 1.0f);
}

/* [fh4] 2026-11-24 Draws that read the back buffer as a texture. When the player is hit, the game draws with the
 * device's own render target set on stage 0 (boot93 log-c: right after "[console] hits", texture 0x01085980, the
 * surface GetRenderTarget handed RenderWare at 0x001CEE6D, "has format 0x00": our surface's header has no format
 * word, so the draws went untextured). On the Xbox a surface can be read as a linear texture: the draw reads what was
 * drawn into the back buffer so far, with coordinates in its pixels. Here the frame so far is in ngl_vk's picture, so
 * the open pass is ended, the picture copied into a texture of its own (ngl_vk_low_copy) and the pass begun again
 * with the colors and depth kept, as fh_draw_clear_depth does. One copy serves every such draw until another draw
 * changes the picture. 0 when it could not (the draw then goes untextured, as before). */
static uint32_t screen_copy(void)
{
    struct ngl_vk_low_pass pass;
    const uint32_t width = draw.target_width, height = draw.target_height;

    if (!draw.frame_open || !width || !height)
        return 0;
    if (draw.screen_texture && draw.screen_frame == fh_d3d_frame() && draw.screen_drawn == draw.drawn &&
        draw.screen_width == width && draw.screen_height == height)
        return draw.screen_texture;
    if (!draw.screen_texture || draw.screen_width != width || draw.screen_height != height) {
        free_later(draw.screen_texture);
        draw.screen_texture = ngl_vk_low_target(draw.formats.color[0], width, height, 1, 0);
        draw.screen_width = width;
        draw.screen_height = height;
        if (!draw.screen_texture)
            return 0;
    }
    ngl_vk_low_end_pass();
    memset(&pass, 0, sizeof pass);
    pass.color[0] = ngl_vk_low_main_color();
    pass.color_count = 1;
    pass.depth = ngl_vk_low_main_depth();
    pass.label = "Finest Hour frame, after a copy of the picture";
    if (ngl_vk_low_copy(ngl_vk_low_main_color(), 0, 0, width, height, draw.screen_texture, 0, 0)) {
        if (ngl_vk_low_begin_pass(&pass)) {
            ngl_vk_low_frame_end();
            draw.frame_open = 0;
        }
        return 0;
    }
    if (ngl_vk_low_begin_pass(&pass)) {
        ngl_vk_low_frame_end();
        draw.frame_open = 0;
        return 0;
    }
    draw.screen_frame = fh_d3d_frame();
    draw.screen_drawn = draw.drawn;
    draw.screen_copies++;
    return draw.screen_texture;
}

/* [fh4] Stage 0's coordinates as fixed.frag reads them (c7: scale x, y, then offset x, y). A swizzled texture's are
 * 0..1 already. A linear one's are in its texels on the Xbox (the NV2A's rectangle textures), so they are divided by
 * its size (FH_RECT_UV=0: as before, untouched). The back buffer's are in the game's 640 x 480 screen pixels, and its
 * copy is the whole picture, so they go where that screen pixel lies on the picture: across the whole picture for a
 * 3D draw, inside the middle 4:3 part for a 2D one (screen_part, as set_viewport places the draw itself). */
static void stage0_uv(int screen, int flat, float uv[4])
{
    uv[0] = 1.0f;
    uv[1] = 1.0f;
    uv[2] = 0.0f;
    uv[3] = 0.0f;
    if (screen) {
        float part_x, part_y, part_width, part_height;

        screen_part(flat, &part_x, &part_y, &part_width, &part_height);
        uv[0] = part_width / (float)draw.game_width / (float)draw.target_width;
        uv[1] = part_height / (float)draw.game_height / (float)draw.target_height;
        uv[2] = part_x / (float)draw.target_width;
        uv[3] = part_y / (float)draw.target_height;
    } else if (draw.rect_uv && last_rect_width && last_rect_height) {
        uv[0] = 1.0f / (float)last_rect_width;
        uv[1] = 1.0f / (float)last_rect_height;
    }
}

/* Draws whose vertex shader handle is 0. The title screen's background (two strips from 0x001FB2E9, their handle set
 * at 0x001CB217) is drawn that way, and on the console it shows; the real SetVertexShader (0x00221B70) would build a
 * format with no inputs from it (0x00220DD0), so something else keeps the format on the Xbox. Their vertices have the
 * layout of the last FVF the game used with the same stride (0x142 for 24 bytes: the title screen, 2026-11-09 boot
 * fhw3b), so that FVF is used for them; FH_FVF0_AS forces one. */
#define STRIDES_REMEMBERED 8
static struct { uint32_t stride, fvf; } last_fvf[STRIDES_REMEMBERED];

static void remember_fvf(uint32_t fvf, uint32_t stride)
{
    unsigned i;

    for (i = 0; i < STRIDES_REMEMBERED && last_fvf[i].stride && last_fvf[i].stride != stride; i++)
        ;
    if (i < STRIDES_REMEMBERED) {
        last_fvf[i].stride = stride;
        last_fvf[i].fvf = fvf;
    }
}

static uint32_t fvf_for_stride(uint32_t stride)
{
    for (unsigned i = 0; i < STRIDES_REMEMBERED && last_fvf[i].stride; i++)
        if (last_fvf[i].stride == stride)
            return last_fvf[i].fvf;
    return 0;
}

/* A shader made without a function draws as an FVF would, from the registers its declaration names: v0 the position
 * (three floats), v3 the diffuse color, v9 the first texture coordinates (two floats). Every register it names must be
 * in stream 0 and inside the stream's stride; a position is needed; other registers (the normal, v2) are not used by
 * the fixed-function shaders yet. Returns 0 when this file cannot draw it. */
#define DECLARATION_POSITION    0
#define DECLARATION_DIFFUSE     3
#define DECLARATION_TEXCOORD0   9
#define DECLARATION_NORMAL      2               /* [fh8] the normal, for the lights */
#define DECLARED_NORMPACKED3    0x16            /* [fh8] the Xbox's packed normal: x 11 bits, y 11, z 10, signed */
#define DECLARED_FLOAT2         0x22
#define DECLARED_FLOAT3         0x32
#define DECLARED_COLOR          0x40
#define REWRITTEN_FVF           0x152u          /* position, [fh8] normal, diffuse color, one pair of texture
                                                 * coordinates */
#define REWRITTEN_STRIDE        36u
#define PROGRAM_STRIDE          32u             /* x, y, z, w; diffuse color; one pair of texture coordinates; the
                                                 * fog distance (oFog.x) */

/* The layout program draws are written in: shaders/fixed.vert reads a four-part position (a three-part one, as in
 * every FVF draw, gets w = 1 from Vulkan). */
static void program_layout(struct ngl_vk_low_layout *layout)
{
    memset(layout, 0, sizeof *layout);
    layout->attributes[0] = (struct ngl_vk_low_attribute){0, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT};
    layout->attributes[1] = (struct ngl_vk_low_attribute){1, 0, 16, VK_FORMAT_R8G8B8A8_UNORM};
    layout->attributes[2] = (struct ngl_vk_low_attribute){2, 0, 20, VK_FORMAT_R32G32_SFLOAT};
    layout->attributes[3] = (struct ngl_vk_low_attribute){3, 0, 28, VK_FORMAT_R32_SFLOAT};
    layout->attributes[4] = (struct ngl_vk_low_attribute){4, 0, 0, VK_FORMAT_R32G32B32_SFLOAT};   /* [fh8] unused */
    layout->attribute_count = 5;
    layout->stride[0] = PROGRAM_STRIDE;
    layout->stream_count = 1;
}

/* Every register a shader's declaration names is in stream 0 and inside the stream's stride. */
static int inputs_in_stream0(const struct fh_vsh *vsh, uint32_t stride)
{
    for (unsigned r = 0; r < FH_VSH_INPUTS; r++) {
        const struct fh_vsh_input *in = &vsh->inputs[r];

        if (in->used && (in->stream != 0 || in->offset + in->bytes > stride))
            return 0;
    }
    return 1;
}

/* [fh8] 1 when a declaration's normal (v2) is one declaration_vertices can read: three floats or NORMPACKED3 */
static int declared_normal(const struct fh_vsh *vsh)
{
    const struct fh_vsh_input *n = &vsh->inputs[DECLARATION_NORMAL];

    return n->used && (n->type == DECLARED_FLOAT3 || n->type == DECLARED_NORMPACKED3);
}

/* [fh8] A signed field of a packed normal as -1..1 */
static float packed_part(uint32_t word, unsigned shift, unsigned bits)
{
    const int32_t value = (int32_t)(word << (32 - shift - bits)) >> (32 - bits);

    return (float)value / (float)((1 << (bits - 1)) - 1);
}

static int declaration_drawable(const struct fh_vsh *vsh, uint32_t stride)
{
    const struct fh_vsh_input *position = &vsh->inputs[DECLARATION_POSITION];
    const struct fh_vsh_input *diffuse = &vsh->inputs[DECLARATION_DIFFUSE];
    const struct fh_vsh_input *uv = &vsh->inputs[DECLARATION_TEXCOORD0];

    if (!position->used || position->type != DECLARED_FLOAT3)
        return 0;
    if ((diffuse->used && diffuse->type != DECLARED_COLOR) || (uv->used && uv->type != DECLARED_FLOAT2))
        return 0;
    return inputs_in_stream0(vsh, stride);
}

/* Rewrites count vertices of a declared layout (stride bytes each, from the guest's vertex data at data) into the
 * REWRITTEN_FVF layout at out. */
static void declaration_vertices(const struct fh_vsh *vsh, uint32_t data, uint32_t stride, uint32_t count,
                                 uint8_t *out)
{
    const struct fh_vsh_input *diffuse = &vsh->inputs[DECLARATION_DIFFUSE];
    const struct fh_vsh_input *uv = &vsh->inputs[DECLARATION_TEXCOORD0];
    const struct fh_vsh_input *normal = &vsh->inputs[DECLARATION_NORMAL];
    const uint16_t position_at = vsh->inputs[DECLARATION_POSITION].offset;
    const int has_normal = declared_normal(vsh);
    static const uint32_t white = 0xFFFFFFFFu;
    static const float no_uv[2] = {0.0f, 0.0f};

    for (uint32_t v = 0; v < count; v++, out += REWRITTEN_STRIDE) {
        const uint8_t *in = (const uint8_t *)XBOX_PTR(data + v * stride);
        float n[3] = {0.0f, 0.0f, 0.0f};

        memcpy(out, in + position_at, 12);
        if (has_normal && normal->type == DECLARED_FLOAT3) {
            memcpy(n, in + normal->offset, 12);
        } else if (has_normal) {
            uint32_t word;

            memcpy(&word, in + normal->offset, 4);
            n[0] = packed_part(word, 0, 11);
            n[1] = packed_part(word, 11, 11);
            n[2] = packed_part(word, 22, 10);
        }
        memcpy(out + 12, n, 12);
        memcpy(out + 24, diffuse->used ? in + diffuse->offset : (const uint8_t *)&white, 4);
        memcpy(out + 28, uv->used ? in + uv->offset : (const uint8_t *)no_uv, 8);
    }
}

/* A float 0-1 as a byte of a D3DCOLOR. */
static uint32_t color_byte(float x)
{
    return x <= 0.0f ? 0u : (x >= 1.0f ? 255u : (uint32_t)(x * 255.0f + 0.5f));
}

/* One vertex in program_layout from what its program wrote (see program_vertices). */
static void program_vertex(const struct fh_vsh_result *r, uint8_t *out)
{
    const float w = draw.program_w ? r->position[3] : 1.0f;
    const float position[4] = {r->position[0] * w, r->position[1] * w, r->position[2] * w, w};
    const uint32_t color = color_byte(r->diffuse[3]) << 24 | color_byte(r->diffuse[0]) << 16 |
                           color_byte(r->diffuse[1]) << 8 | color_byte(r->diffuse[2]);
    float uv[2] = {r->texcoord0[0], r->texcoord0[1]};

    memcpy(out, position, 16);
    memcpy(out + 16, &color, 4);
    /* [fh6] the Xbox divides oT0 by its w at each pixel; these vertices carry two coordinates, so the divide is made
     * here, per vertex (exact while w is the same across a triangle; program.vert's draws divide at each pixel) */
    if (r->texcoord0[3] != 1.0f) {
        draw.projected_vertices++;
        if (draw.project && r->texcoord0[3] != 0.0f)
            uv[0] /= r->texcoord0[3], uv[1] /= r->texcoord0[3];
    }
    memcpy(out + 20, uv, 8);
    memcpy(out + 28, &r->fog, 4);
}

/* Runs a shader's program for count vertices from data and writes program_layout's vertices at out (drawn with an
 * identity transform): the position, oD0 as a D3DCOLOR, oT0's first two. The program ends with the divide by w
 * (x/w, y/w, z/w, w), so the position is multiplied back by w: Vulkan then clips the parts behind the eye and
 * interpolates with perspective. Returns 0 when the program cannot run here (fh_vsh_run). The viewport registers
 * are set once for the draw around it. */
static int program_vertices(const struct fh_vsh *vsh, uint32_t data, uint32_t stride, uint32_t count, uint8_t *out)
{
    struct fh_vsh_result results[FH_VSH_BATCH];
    int ok = 1;

    fh_vsh_draw_begin();
    for (uint32_t first = 0; first < count && ok; first += FH_VSH_BATCH) {
        const uint32_t n = count - first < FH_VSH_BATCH ? count - first : FH_VSH_BATCH;

        if (draw.programs_many)
            ok = fh_vsh_run_many(vsh, data + first * stride, stride, n, results);
        else
            for (uint32_t v = 0; v < n && ok; v++)
                ok = fh_vsh_run(vsh, (const uint8_t *)XBOX_PTR(data + (first + v) * stride), &results[v]);
        for (uint32_t v = 0; v < n && ok; v++, out += PROGRAM_STRIDE)
            program_vertex(&results[v], out);
    }
    fh_vsh_draw_end();
    return ok;
}

/* Where an XYZRHW FVF keeps the diffuse color and the first texture coordinates (no_input when it has none): x, y, z
 * and rhw come first, then the point size, the color, the specular color and the coordinate sets. Returns 0 for an
 * FVF that is not XYZRHW or whose first coordinates are not two floats. */
#define NO_INPUT 0xFFFFFFFFu

static int rhw_layout(uint32_t fvf, uint32_t stride, uint32_t *diffuse_at, uint32_t *uv_at)
{
    uint32_t offset = 16;

    if ((fvf & FVF_POSITION_MASK) != FVF_XYZRHW || ((fvf >> 16) & 3))
        return 0;
    if (fvf & FVF_PSIZE)
        offset += 4;
    *diffuse_at = (fvf & FVF_DIFFUSE) ? offset : NO_INPUT;
    offset += (fvf & FVF_DIFFUSE) ? 4 : 0;
    offset += (fvf & FVF_SPECULAR) ? 4 : 0;
    *uv_at = ((fvf >> 8) & 0xF) ? offset : NO_INPUT;
    offset += ((fvf >> 8) & 0xF) ? 8 : 0;
    return offset <= stride;
}

/* Rewrites count XYZRHW vertices (stride bytes apart from data) into program_layout's vertices at out, drawn with an
 * identity transform like a program's. x and y are the render target's pixels; the draw's Vulkan viewport is the
 * game's viewport, so they are mapped across it into clip space (x right, y up), with w = 1 / rhw so that Vulkan
 * interpolates with perspective as the console does. */
/* [acct2-fh] The last screen draw's extent in the game's pixels (rhw_vertices): a fade or a flash covers the whole
 * screen, and covers the whole picture too (spans_screen). */
static float screen_low_x, screen_high_x, screen_low_y, screen_high_y;

static int spans_screen(void)
{
    return screen_low_x <= 1.0f && screen_high_x >= (float)draw.game_width - 1.0f && screen_low_y <= 1.0f &&
           screen_high_y >= (float)draw.game_height - 1.0f;
}

static void rhw_vertices(uint32_t fvf, const struct fh_d3d_state *st, uint32_t data, uint32_t stride,
                         uint32_t count, uint8_t *out)
{
    const float left = (float)st->viewport[0], top = (float)st->viewport[1];
    const float width = (float)(st->viewport[2] ? st->viewport[2] : draw.game_width);
    const float height = (float)(st->viewport[3] ? st->viewport[3] : draw.game_height);
    static const uint32_t white = 0xFFFFFFFFu;
    static const float no_uv[2] = {0.0f, 0.0f}, no_fog = 0.0f;  /* these draws get no fog (FOG_NONE) */
    uint32_t diffuse_at, uv_at;

    rhw_layout(fvf, stride, &diffuse_at, &uv_at);
    screen_low_x = screen_low_y = 1e30f;
    screen_high_x = screen_high_y = -1e30f;
    for (uint32_t v = 0; v < count; v++, out += PROGRAM_STRIDE) {
        const uint8_t *in = (const uint8_t *)XBOX_PTR(data + v * stride);
        float p[4], position[4];

        memcpy(p, in, 16);
        screen_low_x = p[0] < screen_low_x ? p[0] : screen_low_x;
        screen_high_x = p[0] > screen_high_x ? p[0] : screen_high_x;
        screen_low_y = p[1] < screen_low_y ? p[1] : screen_low_y;
        screen_high_y = p[1] > screen_high_y ? p[1] : screen_high_y;
        position[3] = p[3] > 0.0f ? 1.0f / p[3] : 1.0f;
        position[0] = ((p[0] - left) * 2.0f / width - 1.0f) * position[3];
        position[1] = (1.0f - (p[1] - top) * 2.0f / height) * position[3];
        position[2] = p[2] * position[3];
        memcpy(out, position, 16);
        memcpy(out + 16, diffuse_at != NO_INPUT ? in + diffuse_at : (const uint8_t *)&white, 4);
        memcpy(out + 20, uv_at != NO_INPUT ? in + uv_at : (const uint8_t *)no_uv, 8);
        memcpy(out + 28, &no_fog, 4);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Vertex programs on the GPU (shaders/program.vert) */

#define GPU_SLOTS               8               /* program.vert's input locations */
#define GPU_FIRST_INSTRUCTION   16              /* the pixel constant holding instruction 0 */
#define GPU_NO_REGISTER         16u

/* The Vulkan vertex format of a declaration's data type, or 0 for one the GPU path does not take: floats, D3DCOLOR
 * (its bytes blue, green, red, alpha, which B8G8R8A8_UNORM reads as red, green, blue, alpha, as fh_vsh.c's
 * read_input does), and one, two or four normalized shorts, shorts or bytes (three 8- or 16-bit parts are not a
 * format every GPU reads, and packed3 has none). A format with fewer parts gives the rest as (0, 0, 0, 1), as
 * read_input does. */
static uint32_t gpu_input_format(unsigned type)
{
    switch (type) {
    case 0x12: return VK_FORMAT_R32_SFLOAT;
    case 0x22: return VK_FORMAT_R32G32_SFLOAT;
    case 0x32: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x42: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x40: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x11: return VK_FORMAT_R16_SNORM;
    case 0x21: return VK_FORMAT_R16G16_SNORM;
    case 0x41: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x15: return VK_FORMAT_R16_SSCALED;
    case 0x25: return VK_FORMAT_R16G16_SSCALED;
    case 0x45: return VK_FORMAT_R16G16B16A16_SSCALED;
    case 0x14: return VK_FORMAT_R8_UNORM;
    case 0x24: return VK_FORMAT_R8G8_UNORM;
    case 0x44: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return 0;
    }
}

/* Whether a program draw can run on the GPU, and if so its vertex layout (the program's input registers in register
 * order at locations 0 and up, the rest of the eight locations reading a byte they ignore) and the bool/loop block
 * words program.vert reads (the register of each location, the instruction count, whether w is kept). */
static int gpu_program_layout(const struct fh_vsh *vsh, uint32_t stride, struct ngl_vk_low_layout *layout,
                              uint32_t words[40], uint32_t *expanded_stride)
{
    unsigned slots = 0;
    int raw = 1;

    *expanded_stride = 0;
    if (!draw.gpu_programs || !fh_vsh_gpu_ready(vsh))
        return 0;
    memset(layout, 0, sizeof *layout);
    memset(words, 0, 40 * sizeof words[0]);
    for (unsigned r = 0; r < FH_VSH_INPUTS; r++) {
        const struct fh_vsh_input *in = &vsh->inputs[r];
        uint32_t format;

        if (!(vsh->inputs_read & (1u << r)) || !in->used)
            continue;           /* a register the program reads but the declaration doesn't name reads 0 */
        format = gpu_input_format(in->type);
        if (slots == GPU_SLOTS || in->offset + in->bytes > stride)
            return 0;
        if (!format || in->stream != 0)
            raw = 0;            /* [acct2-fh] 11-13: then all of them go as floats, below */
        layout->attributes[slots] = (struct ngl_vk_low_attribute){slots, 0, in->offset, format};
        words[slots++] = r;
    }
    if (!raw && draw.gpu_expand) {
        /* the vertices rewritten as four floats a register (fh_vsh_expand), for the formats Vulkan has no vertex
         * format for: the skinned meshes' packed3 normal and three-part bone indices and weights */
        uint8_t registers[FH_VSH_INPUTS];
        const unsigned n = fh_vsh_expanded_registers(vsh, registers);

        if (!n || n != slots)
            return 0;
        for (unsigned k = 0; k < n; k++)
            layout->attributes[k] = (struct ngl_vk_low_attribute){k, 0, 16 * k, VK_FORMAT_R32G32B32A32_SFLOAT};
        stride = 16 * n;
        *expanded_stride = stride;
    } else if (!raw) {
        return 0;
    }
    for (unsigned k = slots; k < GPU_SLOTS; k++) {
        layout->attributes[k] = (struct ngl_vk_low_attribute){k, 0, 0, VK_FORMAT_R8_UNORM};
        words[k] = GPU_NO_REGISTER;
    }
    layout->attribute_count = GPU_SLOTS;
    layout->stride[0] = stride;
    layout->stream_count = 1;
    words[8] = vsh->instructions;
    words[9] = draw.program_w ? 1u : 0u;
    return 1;
}

/* Whether this frame's programs run on the CPU for the check (FH_VSH_CPU_FRAMES); says so once a frame. */
static int cpu_frame(void)
{
    if (!draw.cpu_frames || !fh_d3d_frame_listed(draw.cpu_frames))
        return 0;
    if (draw.cpu_frame_said != fh_d3d_frame()) {
        draw.cpu_frame_said = fh_d3d_frame();
        fprintf(stderr, "[fh] frame %lu: its vertex programs run on the CPU (FH_VSH_CPU_FRAMES)\n", draw.cpu_frame_said);
    }
    return 1;
}

static void leave_out(enum skip_reason reason)
{
    draw.skipped[reason]++;
}

/* Where a draw's vertex numbers come from: DrawIndexedVertices' 16-bit indices in guest memory, or (indices 0) the
 * numbers first, first + 1, ... of DrawVertices and DrawVerticesUP. */
struct vertex_numbers {
    uint32_t indices;
    uint32_t first;
};

static uint32_t vertex_number(const struct vertex_numbers *n, uint32_t i)
{
    return n->indices ? *(const uint16_t *)XBOX_PTR(n->indices + 2 * i) : n->first + i;
}

/* The triangles a primitive type makes from count vertex numbers: how many, the Vulkan topology they are drawn in,
 * and how many indices that takes. Lists, fans, polygons and quad lists become triangle lists; strips stay strips,
 * and so do quad strips (a quad strip's quads are the pairs of a triangle strip over the same vertices). Returns 0
 * for a primitive type not written yet (points and lines). */
static int primitive_shape(uint32_t primitive_type, uint32_t count, enum ngl_vk_low_topology *topology,
                           uint32_t *triangles, uint32_t *index_count)
{
    switch (primitive_type) {
    case PRIMITIVE_TRIANGLE_LIST:
        *topology = NGL_VK_LOW_TRIANGLE_LIST;
        *triangles = count / 3;
        *index_count = *triangles * 3;
        return 1;
    case PRIMITIVE_TRIANGLE_STRIP:
    case PRIMITIVE_QUAD_STRIP:
        if (primitive_type == PRIMITIVE_QUAD_STRIP)
            count &= ~1u;
        *topology = NGL_VK_LOW_TRIANGLE_STRIP;
        *triangles = count >= 3 ? count - 2 : 0;
        *index_count = count;
        return 1;
    case PRIMITIVE_TRIANGLE_FAN:
    case PRIMITIVE_POLYGON:
        *topology = NGL_VK_LOW_TRIANGLE_LIST;
        *triangles = count >= 3 ? count - 2 : 0;
        *index_count = *triangles * 3;
        return 1;
    case PRIMITIVE_QUAD_LIST:
        *topology = NGL_VK_LOW_TRIANGLE_LIST;
        *triangles = count / 4 * 2;
        *index_count = *triangles * 3;
        return 1;
    default:
        return 0;
    }
}

/* The index_count indices primitive_shape gave, relative to the lowest vertex number: a fan's and a polygon's as
 * (first, i + 1, i + 2), a quad list's quad (a, b, c, d) as (a, b, c) and (a, c, d), the rest as they are. */
static void write_indices(uint32_t primitive_type, uint32_t index_count, const struct vertex_numbers *n,
                          uint32_t lowest, uint16_t *out)
{
    if (primitive_type == PRIMITIVE_TRIANGLE_FAN || primitive_type == PRIMITIVE_POLYGON) {
        const uint16_t first = (uint16_t)(vertex_number(n, 0) - lowest);

        for (uint32_t t = 0; 3 * t < index_count; t++) {
            out[3 * t] = first;
            out[3 * t + 1] = (uint16_t)(vertex_number(n, t + 1) - lowest);
            out[3 * t + 2] = (uint16_t)(vertex_number(n, t + 2) - lowest);
        }
    } else if (primitive_type == PRIMITIVE_QUAD_LIST) {
        for (uint32_t q = 0; 6 * q < index_count; q++) {
            const uint16_t a = (uint16_t)(vertex_number(n, 4 * q) - lowest);
            const uint16_t b = (uint16_t)(vertex_number(n, 4 * q + 1) - lowest);
            const uint16_t c = (uint16_t)(vertex_number(n, 4 * q + 2) - lowest);
            const uint16_t d = (uint16_t)(vertex_number(n, 4 * q + 3) - lowest);

            out[6 * q] = a;
            out[6 * q + 1] = b;
            out[6 * q + 2] = c;
            out[6 * q + 3] = a;
            out[6 * q + 4] = c;
            out[6 * q + 5] = d;
        }
    } else {
        for (uint32_t i = 0; i < index_count; i++)
            out[i] = (uint16_t)(vertex_number(n, i) - lowest);
    }
}

/* [acct2-fh] 11-13. A selected program whose declaration reads more than one stream (RenderWare's skinned meshes:
 * position, normal and texture coordinates in stream 0; the bone indices and weights, 3, 6 or 9 bytes, in stream 1):
 * the draw puts each vertex's streams side by side in a guest buffer of this file's, in the order fh_vsh_selected
 * merged the declaration, and then draws that buffer as one stream. Only the vertices lowest..highest are merged. */
#define MERGED_ROOM             (2u << 20)

static uint32_t merged_room;            /* the guest buffer, MERGED_ROOM bytes, made on the first merged draw */

/* Every stream the merged declaration reads has a vertex buffer whose stride holds the declaration's vertex. */
static int merged_streams_ready(const struct fh_vsh *vsh, const struct fh_d3d_state *st)
{
    for (unsigned s = 0; s < FH_VSH_STREAMS; s++) {
        if (!(vsh->merged_streams & (1u << s)))
            continue;                   /* a stream the declaration doesn't read */
        if (s >= FH_D3D_STREAMS || !st->streams[s].buffer || !vsh->source_bytes[s] ||
            st->streams[s].stride < vsh->source_bytes[s])
            return 0;
    }
    return 1;
}

/* Vertices first..first+count-1 of the streams, merged; the guest address of the first, or 0 without room. */
static uint32_t merge_streams(const struct fh_vsh *vsh, const struct fh_d3d_state *st, uint32_t first, uint32_t count)
{
    const uint32_t merged_stride = vsh->stream_bytes[0];
    uint8_t *out;

    if (!merged_room) {
        merged_room = xbox_HeapAlloc(MERGED_ROOM, 16);
        if (!merged_room)
            return 0;
    }
    if (!merged_stride || (uint64_t)count * merged_stride > MERGED_ROOM)
        return 0;
    out = (uint8_t *)XBOX_PTR(merged_room);
    for (unsigned s = 0; s < FH_VSH_STREAMS; s++) {
        const uint32_t bytes = vsh->source_bytes[s], at = vsh->merged_at[s];

        if (!(vsh->merged_streams & (1u << s)))
            continue;
        {
            const uint32_t stride = st->streams[s].stride;
            const uint8_t *in = (const uint8_t *)XBOX_PTR((MEM32(st->streams[s].buffer + 4) | DATA_BIT) + first * stride);

            for (uint32_t v = 0; v < count; v++)
                memcpy(out + v * merged_stride + at, in + v * stride, bytes);
        }
    }
    return merged_room;
}

/* One draw of count vertex numbers with the device's state now. vertex_data is the guest address of vertex 0 (0 when
 * there is none) and stride the bytes from one vertex to the next. */
static void draw_vertices(uint32_t primitive_type, uint32_t count, const struct vertex_numbers *numbers,
                          uint32_t vertex_data, uint32_t stride)
{
    fh_push_read();     /* [acct2-fh] 11-13: RenderWare pushes the skinned draws' light and material itself */
    const struct fh_d3d_state *st = fh_d3d_state();
    struct ngl_vk_low_layout layout;
    struct ngl_vk_low_state state;
    struct ngl_vk_low_draw d;
    enum ngl_vk_low_topology topology;
    uint32_t fvf = st->vertex_shader;
    const struct fh_vsh *declared = NULL;   /* a shader made without a function: its vertices are rewritten */
    int on_screen = 0;                      /* an XYZRHW FVF: its vertices are rewritten into clip space */
    int on_gpu = 0;                         /* a program run by shaders/program.vert */
    int flat;                               /* [acct2-fh] a 2D draw: kept in the screen's 4:3 part (set_viewport) */
    uint32_t expanded_stride = 0;           /* its inputs rewritten as floats (fh_vsh_expand): a vertex's bytes */
    uint32_t gpu_words[40];                 /* its bool/loop block */
    static float gpu_vs[FH_VSH_CONSTANT_REGISTERS][4], gpu_ps[256][4];
    uint32_t lowest = 0xFFFFFFFFu, highest = 0, used, data, triangles, index_count, texture, pipeline,
             vertex_room_buffer, index_room_buffer, out_stride;
    size_t vertex_room_offset, index_room_offset, vertex_bytes;
    uint16_t *out_indices;
    uint8_t *out_vertices;
    float wvp[16], vs[4 * VERTEX_CONSTANTS], ps[4 * PIXEL_CONSTANTS];
    unsigned vertex_constants = UNLIT_CONSTANTS;     /* [fh8] c0-c5, or more for a lit draw */
    float uv[4];                            /* [fh4] stage 0's coordinates (stage0_uv) */
    int reads_screen;                       /* [fh4] stage 0 is the back buffer (screen_copy) */
    int unreadable;                         /* [fh6] stage 0's texture is set but could not be read */
    enum fog_source fog;
    uint64_t started = now_ns(), mark;

    if (!draw.ready || !count || count > MAX_INDICES)
        return;
    spent.draws++;
    if (fvf & 1) {
        declared = st->program_selected ? fh_vsh_selected(fvf, st->program_address) : fh_vsh_find(fvf);
        if (!declared || (!declared->fixed_function && (!draw.run_programs || !declared->program))) {
            leave_out(SKIP_PROGRAM);
            return;
        }
        if (declared->merged) {         /* its streams are merged below; the merged vertex is the stride now */
            if (!merged_streams_ready(declared, st)) {
                leave_out(SKIP_DECLARATION);
                return;
            }
            stride = declared->stream_bytes[0];
        }
        if (declared->fixed_function ? !declaration_drawable(declared, stride) : !inputs_in_stream0(declared, stride)) {
            leave_out(SKIP_DECLARATION);
            return;
        }
        if (declared->fixed_function)
            fvf_layout(REWRITTEN_FVF, REWRITTEN_STRIDE, &layout);
        else if (!cpu_frame() && gpu_program_layout(declared, stride, &layout, gpu_words, &expanded_stride))
            on_gpu = 1;
        else
            program_layout(&layout);
    } else {
        uint32_t diffuse_at, uv_at;

        if (fvf == 0)
            fvf = draw.fvf0_as ? draw.fvf0_as : fvf_for_stride(stride);
        else if ((fvf & FVF_POSITION_MASK) != FVF_XYZRHW)
            remember_fvf(fvf, stride);      /* screen vertices are never what a handle-0 draw holds */
        if (rhw_layout(fvf, stride, &diffuse_at, &uv_at)) {
            on_screen = 1;
            program_layout(&layout);
        } else if (!fvf_layout(fvf, stride, &layout)) {
            leave_out(SKIP_FVF);
            return;
        }
    }
    if (!vertex_data || !stride) {
        leave_out(SKIP_NO_STREAM);
        return;
    }
    if (!primitive_shape(primitive_type, count, &topology, &triangles, &index_count)) {
        leave_out(SKIP_PRIMITIVE);
        return;
    }
    if (!triangles || !frame_begin())
        return;

    /* Only the vertices the draw uses, lowest to highest, are copied (or run); the indices are written relative to
     * the lowest. A mesh that draws part of a large vertex buffer then costs only that part. */
    if (numbers->indices) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t number = vertex_number(numbers, i);

            lowest = number < lowest ? number : lowest;
            highest = number > highest ? number : highest;
        }
    } else {
        lowest = numbers->first;
        highest = numbers->first + count - 1;
    }
    used = highest - lowest + 1;
    data = vertex_data + lowest * stride;
    if (declared && declared->selected)
        spent.selected_draws++;
    if (declared && declared->merged) {
        mark = now_ns();
        data = merge_streams(declared, st, lowest, used);
        spent.merges += now_ns() - mark;
        if (!data) {
            leave_out(SKIP_ROOM);
            return;
        }
        spent.merged_draws++;
    }

    /* the indices */
    out_indices = ngl_vk_low_frame_room(NGL_VK_LOW_INDICES, (size_t)index_count * 2, &index_room_buffer,
                                        &index_room_offset);
    if (!out_indices) {
        leave_out(SKIP_ROOM);
        return;
    }
    write_indices(primitive_type, index_count, numbers, lowest, out_indices);

    /* the vertices */
    out_stride = on_screen ? PROGRAM_STRIDE
                           : (on_gpu && expanded_stride ? expanded_stride
                              : (!declared || on_gpu ? stride
                                                     : (declared->fixed_function ? REWRITTEN_STRIDE : PROGRAM_STRIDE)));
    vertex_bytes = (size_t)used * out_stride;
    out_vertices = ngl_vk_low_frame_room(NGL_VK_LOW_VERTICES, vertex_bytes, &vertex_room_buffer, &vertex_room_offset);
    if (!out_vertices) {
        leave_out(SKIP_ROOM);
        return;
    }
    mark = now_ns();
    if (on_screen) {
        rhw_vertices(fvf, st, data, stride, used, out_vertices);
        spent.declarations += now_ns() - mark;
        spent.declaration_vertices += used;
    } else if (on_gpu && expanded_stride) {
        if (!fh_vsh_expand(declared, data, stride, used, (float *)out_vertices)) {
            leave_out(SKIP_DECLARATION);
            return;
        }
        spent.expands += now_ns() - mark;
        spent.expanded_vertices += used;
        spent.expanded_draws++;
        spent.gpu_program_vertices += used;
        spent.gpu_program_draws++;
    } else if (on_gpu) {
        memcpy(out_vertices, (const void *)XBOX_PTR(data), vertex_bytes);
        spent.copies += now_ns() - mark;
        spent.gpu_program_vertices += used;
        spent.gpu_program_draws++;
    } else if (declared && declared->fixed_function) {
        declaration_vertices(declared, data, stride, used, out_vertices);
        spent.declarations += now_ns() - mark;
        spent.declaration_vertices += used;
    } else if (declared) {
        int ran = program_vertices(declared, data, stride, used, out_vertices);

        spent.programs += now_ns() - mark;
        spent.program_vertices += used;
        spent.program_draws++;
        if (!ran) {
            leave_out(SKIP_PROGRAM);
            return;
        }
    } else {
        memcpy(out_vertices, (const void *)XBOX_PTR(data), vertex_bytes);
        spent.copies += now_ns() - mark;
    }
    spent.copied_bytes += vertex_bytes;

    /* world x view x projection, the rows as vertex constants c0-c3; an identity for vertices already in clip space
     * (a program's, or XYZRHW's rewritten) */
    multiply(wvp, st->transforms[TRANSFORM_WORLD], st->transforms[TRANSFORM_VIEW]);
    multiply(wvp, wvp, st->transforms[TRANSFORM_PROJECTION]);
    if (on_screen || (declared && !declared->fixed_function)) {
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        memcpy(wvp, identity, sizeof wvp);
    }
    memcpy(vs, wvp, sizeof wvp);
    /* where the fog distance comes from: a program's oFog (on the CPU written into the vertices, on the GPU by
     * program.vert), the depth for the fixed-function draws, none for screen vertices; c4.x tells fixed.vert */
    fog = on_screen ? FOG_NONE : (declared && !declared->fixed_function ? FOG_FROM_VERTEX : FOG_FROM_DEPTH);
    memset(&vs[16], 0, 8 * sizeof(float));     /* c4, and [fh8] c5: not lit unless lighting_constants below */
    vs[16] = fog == FOG_FROM_DEPTH ? 0.0f : 1.0f;
    vertex_constants = UNLIT_CONSTANTS;

    mark = now_ns();
    /* [fh4] 2026-11-24: count the draws that turn on texture stages 1-3, which are not drawn yet (only stage 0 is),
     * and log the first few setups, to see whether the game's multi-texture draws (detail or light maps) matter. */
    for (uint32_t s = 1; s < FH_D3D_STAGES; s++) {
        const uint32_t op = stage_state(s, TSS_COLOROP);

        if (op <= 1)
            break;
        draw.later_stages[s]++;
        if (draw.later_setups < 12) {
            draw.later_setups++;
            fprintf(stderr, "[fh] draw: stage %u on: color op %u (%u, %u), alpha op %u (%u, %u), texture 0x%08X "
                    "format word 0x%08X\n", s, op, stage_state(s, TSS_COLORARG1), stage_state(s, TSS_COLORARG2),
                    stage_state(s, TSS_ALPHAOP), stage_state(s, TSS_ALPHAARG1), stage_state(s, TSS_ALPHAARG2),
                    st->textures[s], st->textures[s] ? MEM32(st->textures[s] + 12) : 0);
        }
    }
    if (render_state(RS_SPECULARENABLE))
        draw.specular_draws++;
    for (unsigned u = 0; u < UNHANDLED_STATES; u++) {
        const uint32_t value = render_state(unhandled_states[u].state);

        if (value && value != unhandled_states[u].usual) {
            draw.unhandled[u]++;
            if (draw.unhandled_logged < 12) {
                draw.unhandled_logged++;
                fprintf(stderr, "[fh] draw: render state %u (%s) is 0x%X: not drawn yet\n", unhandled_states[u].state,
                        unhandled_states[u].name, value);
            }
        }
    }
    /* [fh7] fixed-function draws lit by the device's lights (SetLight, SetMaterial: fh_d3d_state.c) */
    if (render_state(RS_LIGHTING) && !on_screen && (!declared || declared->fixed_function)) {
        unsigned on = 0;

        for (int i = 0; i < FH_D3D_LIGHTS; i++)
            on += st->light_on[i] && st->light_set[i];
        const int has_normal = declared ? declared_normal(declared) : (fvf & FVF_NORMAL) != 0;

        draw.lit_draws++;
        if (has_normal)
            draw.lit_normal_draws++;
        if (has_normal && draw.light) {
            /* fvf_layout takes only FVFs with a diffuse color; a declaration's may have none (written white) */
            vertex_constants = lighting_constants(st, declared ? declared->inputs[DECLARATION_DIFFUSE].used : 1, vs);
            draw.lit_lighted++;
        } else if (!has_normal) {
            draw.lit_no_normal++;
        }
        draw.lit_lights_most = on > draw.lit_lights_most ? on : draw.lit_lights_most;
        if (draw.lit_logged < 4) {
            draw.lit_logged++;
            fprintf(stderr, "[fh] draw: lit draw, FVF 0x%X%s, %u light(s) on, light 0 type %u, material diffuse %g %g "
                    "%g ambient %g %g %g\n", declared ? 0 : fvf, declared ? " (declaration)" : "", on,
                    st->lights[0][0], (double)as_float(st->material[0]), (double)as_float(st->material[1]),
                    (double)as_float(st->material[2]), (double)as_float(st->material[4]),
                    (double)as_float(st->material[5]), (double)as_float(st->material[6]));
        }
    }
    /* [fh5] stage 0's coordinates come from set 0 as they are here: count the draws that ask otherwise */
    if (st->textures[0] && (stage_state(0, TSS_TEXCOORDINDEX) || stage_state(0, TSS_TEXTURETRANSFORMFLAGS))) {
        if (draw.odd_coords++ < 4)
            fprintf(stderr, "[fh] draw: stage 0 wants coordinates 0x%X, transform flags 0x%X: drawn with set 0\n",
                    stage_state(0, TSS_TEXCOORDINDEX), stage_state(0, TSS_TEXTURETRANSFORMFLAGS));
    }
    /* [fh4] the back buffer read as a texture (screen_copy) */
    reads_screen = draw.screen_reads && st->textures[0] && st->textures[0] == fh_d3d_render_target();
    if (reads_screen) {
        texture = screen_copy();
        last_picture = 0;
        last_rect_width = last_rect_height = 0;
        if (!texture) {
            texture = draw.white;
            reads_screen = 0;
        } else {
            draw.screen_draws++;
            /* [fh11] how the game blends the picture it read back (the pain effect, sub_0012A800 -> sub_001298C0,
             * which blends the last frame over each new one): the blend and its alpha, to scale it by the frame's
             * length so the smear lasts as long at any frame rate (the project's any-FPS rule) */
            if (draw.screen_draws <= 6)
                fprintf(stderr, "[fh] draw: screen read %lu: blend %u src %u dst %u, TFACTOR 0x%08X, stage 0 color op "
                        "%u (%u, %u) alpha op %u (%u, %u), FVF 0x%X stride %u, vertex 0 word 4 0x%08X\n",
                        draw.screen_draws, render_state(RS_ALPHABLENDENABLE), render_state(RS_SRCBLEND),
                        render_state(RS_DESTBLEND), render_state(RS_TEXTUREFACTOR), stage_state(0, TSS_COLOROP),
                        stage_state(0, TSS_COLORARG1), stage_state(0, TSS_COLORARG2), stage_state(0, TSS_ALPHAOP),
                        stage_state(0, TSS_ALPHAARG1), stage_state(0, TSS_ALPHAARG2), fvf, stride,
                        stride >= 20 ? MEM32(vertex_data + 16) : 0u);
        }
    } else {
        texture = texture_for(st->textures[0], st->palettes[0]);
    }
    if (draw.prompt_log && last_picture && texture != draw.white)
        note_picture(last_picture, last_picture_format,
                     on_screen ? PICTURE_SCREEN
                               : declared && !declared->fixed_function ? PICTURE_PROGRAM : PICTURE_WORLD,
                     count);
    /* 2D or 3D, as below (a textured draw is never widened as a fade is) */
    flat = on_screen || (declared && !declared->fixed_function && declared->program &&
                         declared->instructions <= draw.flat_program_instructions);
    stage0_uv(reads_screen, flat, uv);
    /* [fh6] a texture is set but could not be read: see pixel_constants */
    unreadable = draw.unread && st->textures[0] && texture == draw.white;
    if (unreadable)
        draw.unread_draws++;
    pixel_constants(ps, unreadable ? 2 : texture != draw.white, fog, uv);
    /* [fh11] the pain effect's smear the same length at any frame rate (screen_blend_exponent) */
    if (reads_screen && render_state(RS_ALPHABLENDENABLE) && render_state(RS_SRCBLEND) == 0x302 &&
        render_state(RS_DESTBLEND) == 0x303)
        ps[11] = screen_blend_exponent();
    pipeline_state(&state, topology);
    pipeline = ngl_vk_low_pipeline(on_gpu ? draw.program_shader : draw.vertex_shader, draw.pixel_shader, &layout,
                                   &state, &draw.formats);
    if (!pipeline) {
        spent.vulkan += now_ns() - mark;
        spent.all += now_ns() - started;
        return;
    }

    memset(&d, 0, sizeof d);
    d.pipeline = pipeline;
    d.vertex_buffer[0] = vertex_room_buffer;
    d.vertex_offset[0] = vertex_room_offset;
    d.index_buffer = index_room_buffer;
    d.index_offset = index_room_offset;
    d.index_count = index_count;
    d.vertex_constants = vs;
    d.vertex_constant_count = vertex_constants;
    d.pixel_constants = ps;
    d.pixel_constant_count = PIXEL_CONSTANTS;
    if (on_gpu) {   /* the 192 constant registers; fixed.frag's four, then the program (shaders/program.vert) */
        fh_vsh_gpu_constants(gpu_vs);
        memset(&gpu_ps[0][0], 0, GPU_FIRST_INSTRUCTION * 4 * sizeof(float));
        memcpy(gpu_ps, ps, sizeof ps);
        memcpy(gpu_ps[GPU_FIRST_INSTRUCTION], declared->program, (size_t)declared->instructions * 16);
        d.vertex_constants = &gpu_vs[0][0];
        d.vertex_constant_count = FH_VSH_CONSTANT_REGISTERS;
        d.pixel_constants = &gpu_ps[0][0];
        d.pixel_constant_count = GPU_FIRST_INSTRUCTION + declared->instructions;
        d.bool_loop_constants = gpu_words;
    }
    if (!on_screen && !reads_screen) {   /* [fh15] the in-between drawing above 60 (between_constants) */
        uint64_t sig = between_mix(between_mix(between_mix(vertex_data, stride), count), primitive_type);

        sig = between_mix(between_mix(sig, numbers->indices), numbers->first);
        sig = between_mix(between_mix(sig, st->textures[0]), st->vertex_shader);
        between_constants(on_gpu ? &gpu_vs[0][0] : vs, d.vertex_constant_count, sig);
    }
    d.texture[0] = texture;
    d.sampler[0] = sampler_for_stage0();
    /* [acct2-fh] 2D: screen vertices, and RenderWare's 2D programs (3 and 4 instructions: the 3D ones transform with
     * four dot products at least); an untextured screen draw over the whole screen (a fade, a flash) is the whole
     * picture's, as the 3D */
    flat = on_screen || (declared && !declared->fixed_function && declared->program &&
                         declared->instructions <= draw.flat_program_instructions);
    if (on_screen && texture == draw.white && spans_screen())
        flat = 0;
    set_viewport(st, flat);
    if (ngl_vk_low_draw(&d) == 0)
        draw.drawn++;
    spent.vulkan += now_ns() - mark;
    spent.all += now_ns() - started;
}

/* Stream 0's vertex 0: its vertex buffer's data, or 0 when it has none. */
static uint32_t stream0_data(const struct fh_d3d_state *st)
{
    return st->streams[0].buffer ? (MEM32(st->streams[0].buffer + 4) | DATA_BIT) : 0;
}

void fh_draw_indexed(uint32_t primitive_type, uint32_t index_count, uint32_t indices)
{
    const struct fh_d3d_state *st = fh_d3d_state();
    const struct vertex_numbers numbers = {indices, 0};

    if (indices)
        draw_vertices(primitive_type, index_count, &numbers, stream0_data(st), st->streams[0].stride);
}

void fh_draw_vertices(uint32_t primitive_type, uint32_t first, uint32_t count)
{
    const struct fh_d3d_state *st = fh_d3d_state();
    const struct vertex_numbers numbers = {0, first};

    draw_vertices(primitive_type, count, &numbers, stream0_data(st), st->streams[0].stride);
}

void fh_draw_vertices_up(uint32_t primitive_type, uint32_t count, uint32_t data, uint32_t stride)
{
    const struct vertex_numbers numbers = {0, 0};

    draw_vertices(primitive_type, count, &numbers, data, stride);
}

int fh_draw_frame_open(void)
{
    return draw.frame_open;
}

int fh_draw_frame_has_draws(void)
{
    return draw.frame_open && draw.drawn > draw.frame_first_drawn;
}

int fh_draw_clear_depth(float z, uint32_t stencil)
{
    struct ngl_vk_low_pass pass;

    if (!draw.frame_open)
        return 0;
    ngl_vk_low_end_pass();
    memset(&pass, 0, sizeof pass);
    pass.color[0] = ngl_vk_low_main_color();
    pass.color_count = 1;
    pass.depth = ngl_vk_low_main_depth();
    pass.clear_color = 0;                   /* the colors drawn so far are kept */
    pass.clear_depth = 1;
    pass.clear_depth_value = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    pass.clear_stencil_value = stencil;
    pass.label = "Finest Hour frame, after a depth clear";
    if (ngl_vk_low_begin_pass(&pass) == 0)
        return 1;
    pass.clear_depth = 0;                   /* could not: go on without the clear rather than lose the frame */
    pass.label = "Finest Hour frame, after a depth clear that failed";
    if (ngl_vk_low_begin_pass(&pass) == 0)
        return 0;
    ngl_vk_low_frame_end();
    draw.frame_open = 0;
    return 0;
}

/* The CPU picture over the whole screen, opaque: two triangles in clip space with an identity transform. */
static void draw_picture_quad(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch)
{
    /* an identity transform, and c4.x 0: no fog input (the fog is off below) */
    static const float identity[4 * VERTEX_CONSTANTS] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    /* x, y, z, color (white, opaque), u, v: clip space has y up, so the picture's top row is at y = 1 */
    static const struct { float x, y, z; uint32_t color; float u, v; } corners[4] = {
        {-1.0f, 1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {-1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 1.0f}, {1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 1.0f}};
    struct ngl_vk_low_layout layout;
    struct ngl_vk_low_state state;
    struct ngl_vk_low_draw d;
    uint32_t vertex_buffer, pipeline;
    size_t vertex_offset;
    void *room;
    uint8_t *rows;
    float ps[4 * PIXEL_CONSTANTS];

    if (!draw.picture) {
        draw.picture = ngl_vk_low_texture(NGL_VK_LOW_2D, VK_FORMAT_B8G8R8A8_UNORM, width, height, 1, 1);
        if (!draw.picture)
            return;
    }
    if (!(rows = scratch_room((size_t)width * height * 4)))
        return;
    for (uint32_t y = 0; y < height; y++) {
        memcpy(rows + (size_t)y * width * 4, pixels + (size_t)y * pitch, (size_t)width * 4);
        for (uint32_t x = 0; x < width; x++)
            rows[((size_t)y * width + x) * 4 + 3] = 0xFF;   /* X8R8G8B8: the fourth byte is unused */
    }
    if (ngl_vk_low_texture_write(draw.picture, 0, 0, rows, (size_t)width * height * 4, width * 4))
        return;
    room = ngl_vk_low_frame_room(NGL_VK_LOW_VERTICES, sizeof corners, &vertex_buffer, &vertex_offset);
    if (!room || !fvf_layout(0x142, sizeof corners[0], &layout))
        return;
    memcpy(room, corners, sizeof corners);
    memset(&state, 0, sizeof state);
    state.topology = NGL_VK_LOW_TRIANGLE_STRIP;
    pipeline = ngl_vk_low_pipeline(draw.vertex_shader, draw.pixel_shader, &layout, &state, &draw.formats);
    if (!pipeline)
        return;
    memset(ps, 0, sizeof ps);
    ps[0] = 2.0f;           /* SELECTARG1 */
    ps[1] = 2.0f;           /* TEXTURE */
    ps[3] = 2.0f;
    ps[4] = 2.0f;
    ps[6] = 1.0f;
    ps[28] = 1.0f;          /* [fh4] c7: the coordinates as they are */
    ps[29] = 1.0f;
    memset(&d, 0, sizeof d);
    d.pipeline = pipeline;
    d.vertex_buffer[0] = vertex_buffer;
    d.vertex_offset[0] = vertex_offset;
    d.vertex_count = 4;
    d.vertex_constants = identity;
    d.vertex_constant_count = UNLIT_CONSTANTS;
    d.pixel_constants = ps;
    d.pixel_constant_count = PIXEL_CONSTANTS;
    d.texture[0] = draw.picture;
    d.sampler[0] = ngl_vk_low_sampler(1, 2, 2, 2, 2, 1.0f, 0);
    {   /* [acct2-fh] the films and the game's CPU text: the screen's 4:3 part of the picture (screen_part) */
        float x, y, width, height;

        screen_part(1, &x, &y, &width, &height);
        ngl_vk_low_viewport(x, y, width, height, 0.0f, 1.0f);
    }
    ngl_vk_low_draw(&d);
}


/* [waw-decomp-fh2 r19] 2026-11-24 The game's text (fh_text.c) over the whole frame, one texel to a pixel: the
 * layer's rows that hold text go into a texture as wide as the picture (made again only when it needs more rows,
 * rounded up to 128), drawn as a quad over those rows with the layer's alpha (its colors are already times alpha:
 * ONE, ONE_MINUS_SRC_ALPHA). Nothing is scaled: the text was drawn at this size. */
static void draw_text_quad(void)
{
    static const float identity[4 * VERTEX_CONSTANTS] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    struct { float x, y, z; uint32_t color; float u, v; } corners[4];
    struct ngl_vk_low_layout layout;
    struct ngl_vk_low_state state;
    struct ngl_vk_low_draw d;
    const uint8_t *pixels;
    uint32_t width, height, top, rows, vertex_buffer, pipeline;
    size_t vertex_offset;
    void *room;
    uint8_t *texels;
    float ps[4 * PIXEL_CONSTANTS], y0, y1, v1;

    if (!fh_text_layer(&pixels, &width, &height, &top, &rows))
        return;
    if (width != draw.target_width || height != draw.target_height || !rows)
        return;                 /* drawn for another size (a resolution change between): left out this once */
    if (!draw.text_texture || draw.text_rows < rows) {
        uint32_t want = (rows + 127u) & ~127u;

        if (want > height)
            want = height;
        free_later(draw.text_texture);
        draw.text_texture = ngl_vk_low_texture(NGL_VK_LOW_2D, VK_FORMAT_B8G8R8A8_UNORM, width, want, 1, 1);
        draw.text_rows = draw.text_texture ? want : 0;
        if (!draw.text_texture)
            return;
    }
    if (!(texels = scratch_room((size_t)width * draw.text_rows * 4)))
        return;
    memcpy(texels, pixels + (size_t)top * width * 4, (size_t)rows * width * 4);
    memset(texels + (size_t)rows * width * 4, 0, (size_t)(draw.text_rows - rows) * width * 4);
    if (ngl_vk_low_texture_write(draw.text_texture, 0, 0, texels, (size_t)width * draw.text_rows * 4, width * 4))
        return;
    /* clip space has y up: the picture's top row at y = 1 */
    y0 = 1.0f - 2.0f * (float)top / (float)height;
    y1 = 1.0f - 2.0f * (float)(top + rows) / (float)height;
    v1 = (float)rows / (float)draw.text_rows;
    corners[0].x = -1.0f; corners[0].y = y0; corners[0].u = 0.0f; corners[0].v = 0.0f;
    corners[1].x = 1.0f;  corners[1].y = y0; corners[1].u = 1.0f; corners[1].v = 0.0f;
    corners[2].x = -1.0f; corners[2].y = y1; corners[2].u = 0.0f; corners[2].v = v1;
    corners[3].x = 1.0f;  corners[3].y = y1; corners[3].u = 1.0f; corners[3].v = v1;
    for (int i = 0; i < 4; i++) {
        corners[i].z = 0.0f;
        corners[i].color = 0xFFFFFFFFu;
    }
    room = ngl_vk_low_frame_room(NGL_VK_LOW_VERTICES, sizeof corners, &vertex_buffer, &vertex_offset);
    if (!room || !fvf_layout(0x142, sizeof corners[0], &layout))
        return;
    memcpy(room, corners, sizeof corners);
    memset(&state, 0, sizeof state);
    state.topology = NGL_VK_LOW_TRIANGLE_STRIP;
    state.blend = 1;
    state.color_src = state.alpha_src = NGL_VK_LOW_ONE;
    state.color_dst = state.alpha_dst = NGL_VK_LOW_ONE_MINUS_SRC_ALPHA;
    state.color_op = state.alpha_op = NGL_VK_LOW_ADD;
    pipeline = ngl_vk_low_pipeline(draw.vertex_shader, draw.pixel_shader, &layout, &state, &draw.formats);
    if (!pipeline)
        return;
    memset(ps, 0, sizeof ps);
    ps[0] = 2.0f;           /* SELECTARG1 */
    ps[1] = 2.0f;           /* TEXTURE */
    ps[3] = 2.0f;
    ps[4] = 2.0f;
    ps[6] = 1.0f;
    ps[28] = 1.0f;          /* c7: the coordinates as they are */
    ps[29] = 1.0f;
    memset(&d, 0, sizeof d);
    d.pipeline = pipeline;
    d.vertex_buffer[0] = vertex_buffer;
    d.vertex_offset[0] = vertex_offset;
    d.vertex_count = 4;
    d.vertex_constants = identity;
    d.vertex_constant_count = UNLIT_CONSTANTS;
    d.pixel_constants = ps;
    d.pixel_constant_count = PIXEL_CONSTANTS;
    d.texture[0] = draw.text_texture;
    d.sampler[0] = ngl_vk_low_sampler(0, 2, 2, 2, 2, 1.0f, 0);     /* nearest: one texel to a pixel */
    ngl_vk_low_viewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    ngl_vk_low_draw(&d);
}

int fh_draw_size(uint32_t *width, uint32_t *height)
{
    if (!draw.ready)
        return 0;
    *width = draw.target_width;
    *height = draw.target_height;
    return 1;
}

int fh_draw_screen_area(float *x, float *y, float *width, float *height)
{
    if (!draw.ready)
        return 0;
    screen_part(1, x, y, width, height);
    return 1;
}

int fh_draw_begin_frame(void)
{
    return draw.ready ? frame_begin() : 0;
}

int fh_draw_frame_end(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch)
{
    struct ngl_vk_low_counts counts;
    uint64_t started;
    int result;

    if (!draw.frame_open)
        return -1;
    if (pixels)
        draw_picture_quad(pixels, width, height, pitch);
    draw_text_quad();       /* [waw-decomp-fh2 r19] the game's text on top, at the picture's size */
    ngl_vk_low_frame_counts(&counts);
    if (counts.frame_room_used > spent.room_most)
        spent.room_most = counts.frame_room_used;
    draw.frame_open = 0;
    started = now_ns();
    result = ngl_vk_low_frame_end();
    spent.frame_ends += now_ns() - started;
    spent.frames++;
    return result;
}

int fh_draw_start(uint32_t game_width, uint32_t game_height)
{
    struct ngl_vk_backend backend;
    const char *fvf0 = getenv("FH_FVF0_AS");
    static const uint32_t white = 0xFFFFFFFFu;

    if (draw.ready)
        return 0;
    if (ngl_vk_low_init() || ngl_vk_backend(&backend)) {
        fprintf(stderr, "[fh] draw: ngl_vk's lower layer didn't start; the game's draws are left out\n");
        return -1;
    }
    draw.game_width = game_width ? game_width : 640;
    draw.game_height = game_height ? game_height : 480;
    draw.target_width = backend.width;
    draw.target_height = backend.height;
    draw.formats.color[0] = backend.target_format;
    draw.formats.color_count = 1;
    draw.formats.depth_format = backend.depth_format;
    draw.formats.samples = 1;
    draw.vertex_shader = ngl_vk_low_shader(NGL_VK_LOW_VERTEX_SHADER, fixed_vert_spirv,
                                           sizeof fixed_vert_spirv / sizeof fixed_vert_spirv[0]);
    draw.pixel_shader = ngl_vk_low_shader(NGL_VK_LOW_PIXEL_SHADER, fixed_frag_spirv,
                                          sizeof fixed_frag_spirv / sizeof fixed_frag_spirv[0]);
    draw.white = ngl_vk_low_texture(NGL_VK_LOW_2D, VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1);
    if (!draw.vertex_shader || !draw.pixel_shader || !draw.white ||
        ngl_vk_low_texture_write(draw.white, 0, 0, &white, 4, 4)) {
        fprintf(stderr, "[fh] draw: the shaders or the white texture couldn't be made; the game's draws are left out\n");
        return -1;
    }
    if (fvf0 && *fvf0)
        draw.fvf0_as = (uint32_t)strtoul(fvf0, NULL, 16);
    draw.run_programs = !(getenv("FH_VSH_RUN") && strcmp(getenv("FH_VSH_RUN"), "0") == 0);
    draw.flat_program_instructions = getenv("FH_FLAT_PROGRAM") ? (uint32_t)strtoul(getenv("FH_FLAT_PROGRAM"), NULL, 10)
                                                               : 4u;
    draw.program_w = !(getenv("FH_VSH_W") && strcmp(getenv("FH_VSH_W"), "0") == 0);
    draw.programs_many = !(getenv("FH_VSH_MANY") && strcmp(getenv("FH_VSH_MANY"), "0") == 0);
    draw.program_shader = ngl_vk_low_shader(NGL_VK_LOW_VERTEX_SHADER, program_vert_spirv,
                                            sizeof program_vert_spirv / sizeof program_vert_spirv[0]);
    draw.gpu_programs = draw.program_shader != 0 &&
                        !(getenv("FH_VSH_GPU") && strcmp(getenv("FH_VSH_GPU"), "0") == 0);
    draw.gpu_expand = !(getenv("FH_VSH_GPU_EXPAND") && strcmp(getenv("FH_VSH_GPU_EXPAND"), "0") == 0);
    draw.fog = !(getenv("FH_FOG") && strcmp(getenv("FH_FOG"), "0") == 0);
    draw.project = !(getenv("FH_PROJECT") && strcmp(getenv("FH_PROJECT"), "0") == 0);
    draw.light = !(getenv("FH_LIGHT") && strcmp(getenv("FH_LIGHT"), "0") == 0);
    draw.unread = !(getenv("FH_UNREAD") && strcmp(getenv("FH_UNREAD"), "0") == 0);
    draw.cpu_frames = getenv("FH_VSH_CPU_FRAMES");
    draw.mips = !(getenv("FH_MIPS") && strcmp(getenv("FH_MIPS"), "0") == 0);
    draw.screen_reads = !(getenv("FH_SCREEN_READS") && strcmp(getenv("FH_SCREEN_READS"), "0") == 0);
    draw.rect_uv = !(getenv("FH_RECT_UV") && strcmp(getenv("FH_RECT_UV"), "0") == 0);
    draw.prompt_log = getenv("FH_PROMPT_LOG") && strcmp(getenv("FH_PROMPT_LOG"), "0") != 0;
    /* [acct2-fh] 2026-11-14 culling, ZENABLE and the depth bias (pipeline_state) */
    draw.cull = getenv("FH_CULL") ? atoi(getenv("FH_CULL")) : 1;
    draw.cull = draw.cull > 0 ? 1 : draw.cull < 0 ? -1 : 0;
    draw.cull_probe = getenv("FH_CULL_PROBE") ? strtoul(getenv("FH_CULL_PROBE"), NULL, 10) : 0;
    draw.stencil = !(getenv("FH_STENCIL") && strcmp(getenv("FH_STENCIL"), "0") == 0);
    draw.z_enable = !(getenv("FH_ZENABLE") && strcmp(getenv("FH_ZENABLE"), "0") == 0);
    draw.depth_bias = !(getenv("FH_DEPTH_BIAS") && strcmp(getenv("FH_DEPTH_BIAS"), "0") == 0);
    if (!draw.program_shader)
        fprintf(stderr, "[fh] draw: shaders/program.vert couldn't be made; vertex programs run on the CPU\n");
    draw.ready = 1;
    fprintf(stderr, "[fh] draw: the game's draws go to Vulkan (%u x %u)%s; vertex programs on the %s\n",
            draw.target_width, draw.target_height, draw.fvf0_as ? "; vertex format 0 drawn as FH_FVF0_AS" : "",
            draw.gpu_programs ? "GPU where they can be" : "CPU");
    return 0;
}

/* [acct2-fh] 2026-11-13 A new render resolution (fh_present.c, RESOLUTION.md): ngl_vk is about to be closed and opened
 * again at another size, which frees everything made on it. So the layer below is shut down here and every handle
 * this file keeps from it is forgotten: the textures are made again from the game's memory as they are drawn, the CPU
 * picture's texture with the next CPU picture. What isn't Vulkan's (the prompt pictures' list, the counts) stays.
 * Called between two frames (none open). */
void fh_draw_stop(void)
{
    if (!draw.ready)
        return;
    ngl_vk_low_shutdown();
    texture_count = 0;
    textures_to_free_count = 0;
    draw.ready = 0;
    draw.frame_open = 0;
    draw.white = draw.picture = 0;
    draw.text_texture = draw.text_rows = 0;     /* [waw-decomp-fh2 r19] */
    draw.vertex_shader = draw.pixel_shader = draw.program_shader = 0;
    draw.screen_texture = 0;                /* [fh4] ngl_vk_low_shutdown freed it */
    draw.screen_width = draw.screen_height = 0;
    last_picture = 0;
    last_picture_format = 0;
}

void fh_draw_report(void)
{
    const double frames = spent.frames ? (double)spent.frames : 1.0;

    fprintf(stderr, "[fh] draw: %lu drawn", draw.drawn);
    for (int i = 0; i < SKIP_REASONS; i++)
        if (draw.skipped[i])
            fprintf(stderr, "; %lu left out (%s)", draw.skipped[i], skip_names[i]);
    if (draw.screen_draws)
        fprintf(stderr, "; %lu read the picture so far (%lu copies)", draw.screen_draws, draw.screen_copies);
    if (draw.specular_draws)
        fprintf(stderr, "; %lu with specular on (not added yet)", draw.specular_draws);
    if (draw.lit_draws)
        fprintf(stderr, "; %lu lit by the fixed-function lights (%lu with normals, up to %u lights; %lu drawn with "
                "the lights, %lu without a normal drawn with their colors)", draw.lit_draws, draw.lit_normal_draws,
                draw.lit_lights_most, draw.lit_lighted, draw.lit_no_normal);
    if (draw.lod_draws)
        fprintf(stderr, "; %lu with a mip LOD bias or max mip level on stage 0 (not drawn yet)", draw.lod_draws);
    for (unsigned u = 0; u < UNHANDLED_STATES; u++)
        if (draw.unhandled[u])
            fprintf(stderr, "; %lu with %s other than usual (not drawn yet)", draw.unhandled[u], unhandled_states[u].name);
    if (draw.projected_vertices)
        fprintf(stderr, "; %lu program vertices with oT0.w other than 1", draw.projected_vertices);
    if (draw.unread_draws)
        fprintf(stderr, "; %lu with a texture that could not be read (read as 0)", draw.unread_draws);
    if (draw.odd_coords)
        fprintf(stderr, "; %lu with other coordinates on stage 0", draw.odd_coords);
    if (draw.stencil_draws)
        fprintf(stderr, "; %lu with the stencil test", draw.stencil_draws);
    if (draw.later_stages[1])
        fprintf(stderr, "; %lu with stage 1 on, %lu stage 2, %lu stage 3 (not drawn yet)", draw.later_stages[1],
                draw.later_stages[2], draw.later_stages[3]);
    fprintf(stderr, "; %u textures kept (%lu made again after a lock, %lu let go of with the list full)\n",
            texture_count, textures_remade, textures_dropped);
    if (draw.prompt_log) {
        unsigned by_kind[3] = {0, 0, 0};

        for (unsigned i = 0; i < picture_count; i++)
            for (unsigned k = 0; k < 3; k++)
                by_kind[k] += (pictures[i].kinds >> k) & 1;
        fprintf(stderr, "[prompt] %u different 32 x 32 pictures drawn so far (%u in screen draws, %u in program draws, "
                "%u in world draws)\n", picture_count, by_kind[0], by_kind[1], by_kind[2]);
    }
    /* the time since the last report, a frame on average, in milliseconds */
    fprintf(stderr, "[fh] draw time a frame (%lu frames): all draws %.2f ms (%.0f draws); vertex programs %.2f ms "
            "(%.0f draws, %.0f vertices, %.0f ns a vertex); declarations %.2f ms (%.0f vertices); copies %.2f ms; "
            "%.2f MB of vertices; textures, pipelines and Vulkan draws %.2f ms; frame end %.2f ms; frame room at most "
            "%.1f MB; programs on the GPU %.0f draws (%.0f vertices; %.0f draws, %.0f vertices rewritten as floats, "
            "%.2f ms); selected programs %.0f draws (%.0f with streams merged, %.2f ms)\n",
            spent.frames, spent.all / 1e6 / frames, spent.draws / frames, spent.programs / 1e6 / frames,
            spent.program_draws / frames, spent.program_vertices / frames,
            spent.program_vertices ? (double)spent.programs / (double)spent.program_vertices : 0.0,
            spent.declarations / 1e6 / frames, spent.declaration_vertices / frames, spent.copies / 1e6 / frames,
            spent.copied_bytes / 1e6 / frames, spent.vulkan / 1e6 / frames, spent.frame_ends / 1e6 / frames,
            spent.room_most / 1e6, spent.gpu_program_draws / frames, spent.gpu_program_vertices / frames,
            spent.expanded_draws / frames, spent.expanded_vertices / frames, spent.expands / 1e6 / frames,
            spent.selected_draws / frames, spent.merged_draws / frames, spent.merges / 1e6 / frames);
    memset(&spent, 0, sizeof spent);
}

#else   /* built without Vulkan (ngl_vk): nothing is drawn */

int fh_draw_start(uint32_t game_width, uint32_t game_height)
{
    (void)game_width;
    (void)game_height;
    return -1;
}

void fh_draw_stop(void)
{
}

void fh_draw_indexed(uint32_t primitive_type, uint32_t index_count, uint32_t indices)
{
    (void)primitive_type;
    (void)index_count;
    (void)indices;
}

void fh_draw_vertices(uint32_t primitive_type, uint32_t first, uint32_t count)
{
    (void)primitive_type;
    (void)first;
    (void)count;
}

void fh_draw_vertices_up(uint32_t primitive_type, uint32_t count, uint32_t data, uint32_t stride)
{
    (void)primitive_type;
    (void)count;
    (void)data;
    (void)stride;
}

int fh_draw_frame_open(void)
{
    return 0;
}

int fh_draw_frame_end(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch)
{
    (void)pixels;
    (void)width;
    (void)height;
    (void)pitch;
    return -1;
}

void fh_draw_report(void)
{
}

/* [waw-decomp-fh2 r19] without Vulkan: no picture, so the game's text stays its own (fh_text.c) */
int fh_draw_size(uint32_t *width, uint32_t *height)
{
    (void)width;
    (void)height;
    return 0;
}

int fh_draw_screen_area(float *x, float *y, float *width, float *height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    return 0;
}

int fh_draw_begin_frame(void)
{
    return 0;
}

void fh_draw_resource_locked(uint32_t resource)
{
    (void)resource;
}
#endif
