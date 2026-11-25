/* [waw-decomp] Finest Hour port, phase 2: the Direct3D device's state as the game sets it, and its draws. See
 * fh_d3d_state.h.
 *
 * In phase 1 these functions were stand-ins that only logged and returned 0. The native renderer (owner 2026-11-09
 * 14:42: the renderer cuts "At Direct3D") has to draw each DrawIndexedVertices with the state that was current at
 * that moment, so each of them keeps what the game gives it, the way the real function keeps it in the device, and
 * each draw goes on to fh_draw.c with that state.
 *
 * The draw census, FH_DRAW_LOG=<frame>[,<frame>...]: everything the game does through these functions in those frames
 * (frames count Swaps, from 1, as fh_present.c does), in order, goes to the log: each state call with its values, and
 * each draw with its primitive type, its indices, the vertex shader handle (an FVF code decoded), every stream's
 * buffer, stride and first vertices (as hex words and as floats), and each stage's texture (its format word decoded:
 * format number, size, levels, swizzled or linear), and the render and texture stage states in the game's own arrays
 * (below). The first census frame also prints the states set through the calls before it, and every other Direct3D
 * and DirectSound stand-in called in a census frame is printed too (fh_log.c). That census is the evidence the native draws get written from; its numbers and meanings are
 * checked against each function's own code, not taken from any SDK.
 *
 * What the front end does each frame (boot 15, 11-08: per Swap): 7 SetTransform, 2 SetViewport, 3 SetTexture,
 * 3 SetPalette, 3 SetStreamSource, 2 SetVertexShader, 2 vertex buffer locks, 4 DrawIndexedVertices, 1 SetRenderState,
 * 10 SetTextureStageState. */
#include "fh_d3d_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32: guest memory */
#include "xbox_memory_layout.h"     /* XBOX_PTR */
#include "fh_draw.h"                /* the draws on Vulkan */
#include "fh_vsh.h"                 /* vertex shaders made by CreateVertexShader */
#include "fh_settings.h"            /* [acct2-fh] 11-22: video.depth_clear */

#define D3D_OK                  0u
#define CENSUS_LINES_DEFAULT    800     /* lines a census frame may print, so a busy frame can't flood the log;
                                         * FH_DRAW_LOG_LINES=<n> sets another number (a level frame needs ~25,000) */
#define CENSUS_INDICES_SHOWN    12
#define CENSUS_INDICES_SCANNED  65536   /* indices read for their smallest and largest */
#define CENSUS_VERTICES_SHOWN   2
#define CENSUS_WORDS_SHOWN      16      /* a vertex's 32-bit words shown at most */
#define VERTEX_DATA_BIT         0x80000000u     /* a vertex buffer's data as D3D8's Lock2 returns it (fh_d3d.c) */

/* Where the game's Direct3D keeps the render and texture stage states, read from the real functions' code (Europe v3,
 * 2026-11-09): SetRenderState (0x00222390) stores state S's value at 0x00230720 + 4 * S for S below 0x88 (below 0x5C
 * it also pushes it to the GPU at once, from 0x5C the push waits for the next draw); from 0x88 it calls a handler per
 * state (the table at 0x0022E3B0). SetTextureStageState (0x00222DD0) stores stage T's state S at
 * 0x00230520 + 4 * (32 * T + S) (types 0x1C and up through handlers of their own, 0x1C at 0x00222BD0). Both also set
 * bits in the dirty word at 0x00230518. The game's own code writes some of these values straight into the same
 * arrays (the XDK inlines the deferred states), so the arrays, not the calls, are the state a draw has to use: the
 * native functions store there too, and the census reads them back.
 * [acct2-fh] 2026-11-14: the handlers keep their states in the same array: the table's slot for state S is
 * 0x0022E3B0 + 4 * S (so it starts at 0x0022E5D0 with state 0x88), and each handler read so far ends by storing its
 * value at 0x00230720 + 4 * S: FOGCOLOR (138, 0x002226E0, at 0x00230948), NORMALIZENORMALS (142, 0x002227E0),
 * TEXTUREFACTOR (148, 0x00222820), MULTISAMPLEMODE (154, 0x002238C0). The array has 166 states: g_pDevice
 * (0x002309B8) follows it. So every state is stored there now, not only those below 0x88. */
#define GUEST_RENDER_STATES     0x00230720u
#define GUEST_RENDER_STATES_KEPT 166u           /* the states in the array (D3DRS_MAX); g_pDevice comes next */
#define GUEST_STAGE_STATES      0x00230520u

static struct fh_d3d_state state;
static unsigned long frames_ended;      /* the frame being recorded is frames_ended + 1 */
static int census;                      /* this frame is a census frame */
static int census_started;              /* a census frame has printed the states set before it */
static unsigned census_lines;
static unsigned census_lines_max = CENSUS_LINES_DEFAULT;
static unsigned long census_calls, census_draws;

const struct fh_d3d_state *fh_d3d_state(void)
{
    return &state;
}

int fh_d3d_census_active(void)
{
    return census;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The census's output */

static int census_line_allowed(void)
{
    if (!census)
        return 0;
    if (census_lines == census_lines_max)
        fprintf(stderr, "[draws] (%u lines: the rest of this frame isn't shown; FH_DRAW_LOG_LINES raises it)\n",
                census_lines_max);
    return census_lines++ < census_lines_max;
}

static float as_float(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

/* Whether frame is one of the comma-separated numbers in list. */
/* Whether frame is in a list of frames like "3000,6000" or, [acct2-fh] 11-14, ranges like "6200-6210". */
static int in_list(const char *list, unsigned long frame)
{
    char *end;

    while (list && *list) {
        unsigned long n = strtoul(list, &end, 10), last = n;

        if (end == list)
            return 0;
        if (*end == '-') {
            const char *from = end + 1;

            last = strtoul(from, &end, 10);
            if (end == from)
                return 0;
        }
        if (frame >= n && frame <= last)
            return 1;
        if (*end != ',')
            return 0;
        list = end + 1;
    }
    return 0;
}

unsigned long fh_d3d_frame(void)
{
    return frames_ended + 1;
}

int fh_d3d_frame_listed(const char *list)
{
    return in_list(list, frames_ended + 1);
}

/* The render and texture stage states set before the first census frame, once. */
static void census_states_so_far(void)
{
    if (census_started)
        return;
    census_started = 1;
    fprintf(stderr, "[draws] render states set so far (state = value):");
    for (unsigned i = 0; i < FH_D3D_RENDER_STATES; i++)
        if (state.render_state_set[i])
            fprintf(stderr, " %u=0x%X", i, state.render_states[i]);
    fputc('\n', stderr);
    for (unsigned stage = 0; stage < FH_D3D_STAGES; stage++) {
        int any = 0;

        for (unsigned i = 0; i < FH_D3D_STAGE_STATES; i++) {
            if (!state.stage_state_set[stage][i])
                continue;
            if (!any++)
                fprintf(stderr, "[draws] stage %u's states set so far (type = value):", stage);
            fprintf(stderr, " %u=0x%X", i, state.stage_states[stage][i]);
        }
        if (any)
            fputc('\n', stderr);
    }
}

/* The Xbox D3D8 format numbers (bits 8-15 of a texture's Format word) this port has met or expects; the rest print
 * as numbers. The S_ ones are swizzled, the LIN_ ones linear. */
static const char *format_name(uint32_t format)
{
    switch (format) {
    case 0x00: return "S_L8";
    case 0x01: return "S_AL8";
    case 0x02: return "S_A1R5G5B5";
    case 0x03: return "S_X1R5G5B5";
    case 0x04: return "S_A4R4G4B4";
    case 0x05: return "S_R5G6B5";
    case 0x06: return "S_A8R8G8B8";
    case 0x07: return "S_X8R8G8B8";
    case 0x0B: return "S_P8 (palette)";
    case 0x0C: return "DXT1";
    case 0x0E: return "DXT2/3";
    case 0x0F: return "DXT4/5";
    case 0x10: return "LIN_A1R5G5B5";
    case 0x11: return "LIN_R5G6B5";
    case 0x12: return "LIN_A8R8G8B8";
    case 0x13: return "LIN_L8";
    case 0x19: return "S_A8";
    case 0x1A: return "S_A8L8";
    case 0x1C: return "LIN_X1R5G5B5";
    case 0x1D: return "LIN_A4R4G4B4";
    case 0x1E: return "LIN_X8R8G8B8";
    case 0x1F: return "LIN_A8";
    case 0x20: return "LIN_A8L8";
    default: return "?";
    }
}

/* One stage's texture: its object's Format and Size words (D3DPixelContainer: Common, Data, Lock, Format, Size). A
 * Size of 0 means a swizzled texture whose size is in the Format word (log2 width and height); otherwise Size holds
 * the linear texture's width - 1, height - 1 and pitch / 64 - 1. */
static void census_texture(unsigned stage, uint32_t texture)
{
    uint32_t common, data, format, size, width, height, pitch;

    if (!census_line_allowed())
        return;
    if (!texture) {
        fprintf(stderr, "[draws]     stage %u: no texture\n", stage);
        return;
    }
    common = MEM32(texture);
    data = MEM32(texture + 4);
    format = MEM32(texture + 12);
    size = MEM32(texture + 16);
    if (size) {
        width = (size & 0xFFF) + 1;
        height = ((size >> 12) & 0xFFF) + 1;
        pitch = (((size >> 24) & 0xFF) + 1) * 64;
    } else {
        width = 1u << ((format >> 20) & 0xF);
        height = 1u << ((format >> 24) & 0xF);
        pitch = 0;
    }
    fprintf(stderr, "[draws]     stage %u: texture 0x%08X: format 0x%02X %s, %u x %u%s, %u level(s)%s, dimension %u; "
            "Format 0x%08X Size 0x%08X; data 0x%08X; common 0x%08X; palette 0x%08X\n", stage, texture,
            (format >> 8) & 0xFF, format_name((format >> 8) & 0xFF), width, height, size ? ", linear" : ", swizzled",
            (format >> 16) & 0xF, (format & 0x4) ? ", cube" : "", (format >> 4) & 0xF, format, size, data, common,
            stage < FH_D3D_STAGES ? state.palettes[stage] : 0);
    if (pitch && census_line_allowed())
        fprintf(stderr, "[draws]       pitch %u bytes\n", pitch);
}

/* An FVF code in words: the position, the normal, the colors and how many texture coordinate sets. */
static void fvf_text(uint32_t fvf, char *out, size_t room)
{
    static const char *const positions[8] = {"no position", "XYZ", "XYZRHW", "XYZB1", "XYZB2", "XYZB3", "XYZB4", "?"};

    snprintf(out, room, "%s%s%s%s%s, %u texture set(s), texture formats 0x%04X", positions[(fvf >> 1) & 7],
             (fvf & 0x010) ? " NORMAL" : "", (fvf & 0x020) ? " PSIZE" : "", (fvf & 0x040) ? " DIFFUSE" : "",
             (fvf & 0x080) ? " SPECULAR" : "", (fvf >> 8) & 0xF, fvf >> 16);
}

static const char *primitive_name(uint32_t type)
{
    static const char *const names[] = {"?", "point list", "line list", "line loop", "line strip", "triangle list",
                                        "triangle strip", "triangle fan", "quad list", "quad strip", "polygon"};

    return type < sizeof names / sizeof names[0] ? names[type] : "?";
}

/* One vertex of a stream: its words in hex and as floats. */
static void census_vertex(unsigned stream, uint32_t at, uint32_t stride, uint32_t index)
{
    uint32_t words = stride / 4 < CENSUS_WORDS_SHOWN ? stride / 4 : CENSUS_WORDS_SHOWN;

    if (!census_line_allowed())
        return;
    fprintf(stderr, "[draws]       stream %u vertex %u:", stream, index);
    for (uint32_t i = 0; i < words; i++)
        fprintf(stderr, " %08X", MEM32(at + i * 4));
    fprintf(stderr, "  =");
    for (uint32_t i = 0; i < words; i++)
        fprintf(stderr, " %g", (double)as_float(MEM32(at + i * 4)));
    fputc('\n', stderr);
}

/* At each draw: the render states in the game's array that differ from what the last draw saw (all of them at the
 * frame's first draw), and the texture stage rows of stage 0 and of every stage with a texture. */
static void census_guest_states(void)
{
    static uint32_t seen[GUEST_RENDER_STATES_KEPT];
    static unsigned long seen_at_frame;
    int all = seen_at_frame != frames_ended + 1, any = 0;

    seen_at_frame = frames_ended + 1;
    if (census_line_allowed()) {
        fprintf(stderr, "[draws]     render states (game's array, %s):", all ? "all not 0" : "changed");
        for (uint32_t i = 0; i < GUEST_RENDER_STATES_KEPT; i++) {
            uint32_t v = MEM32(GUEST_RENDER_STATES + 4 * i);

            if (all ? v != 0 : v != seen[i]) {
                fprintf(stderr, " %u=0x%X", i, v);
                any = 1;
            }
            seen[i] = v;
        }
        fprintf(stderr, "%s\n", any ? "" : " none");
    }
    for (unsigned stage = 0; stage < FH_D3D_STAGES; stage++) {
        if (stage && !state.textures[stage])
            continue;
        if (!census_line_allowed())
            return;
        fprintf(stderr, "[draws]     stage %u states (game's array):", stage);
        for (uint32_t i = 0; i < FH_D3D_STAGE_STATES; i++)
            fprintf(stderr, " %u=0x%X", i, MEM32(GUEST_STAGE_STATES + 4 * (FH_D3D_STAGE_STATES * stage + i)));
        fputc('\n', stderr);
    }
}

/* The three draws: DrawIndexedVertices (count indices at indices), DrawVertices (count vertices of the streams from
 * first) and DrawVerticesUP (count vertices at up_data, up_stride bytes apart). */
enum census_kind { CENSUS_INDEXED, CENSUS_STREAMS, CENSUS_POINTER };

static void census_draw(enum census_kind kind, uint32_t primitive_type, uint32_t count, uint32_t indices,
                        uint32_t first, uint32_t up_data, uint32_t up_stride)
{
    uint32_t lowest = 0xFFFFFFFFu, highest = 0, scanned;
    char fvf[160];

    census_draws++;
    if (!census_line_allowed())
        return;
    if (kind == CENSUS_INDEXED)
        fprintf(stderr, "[draws] DrawIndexedVertices #%lu: %s (%u), %u indices at 0x%08X  [ret to 0x%08X]\n",
                census_draws, primitive_name(primitive_type), primitive_type, count, indices, MEM32(g_esp));
    else if (kind == CENSUS_POINTER)
        fprintf(stderr, "[draws] DrawVerticesUP #%lu: %s (%u), %u vertices at 0x%08X, stride %u  [ret to 0x%08X]\n",
                census_draws, primitive_name(primitive_type), primitive_type, count, up_data, up_stride,
                MEM32(g_esp));
    else
        fprintf(stderr, "[draws] DrawVertices #%lu: %s (%u), vertices %u..%u of the streams  [ret to 0x%08X]\n",
                census_draws, primitive_name(primitive_type), primitive_type, first, first + count - 1,
                MEM32(g_esp));
    if (!count || (kind == CENSUS_INDEXED && !indices))
        return;
    if (kind == CENSUS_INDEXED) {
        scanned = count < CENSUS_INDICES_SCANNED ? count : CENSUS_INDICES_SCANNED;
        for (uint32_t i = 0; i < scanned; i++) {
            uint32_t index = *(const uint16_t *)XBOX_PTR(indices + 2 * i);

            lowest = index < lowest ? index : lowest;
            highest = index > highest ? index : highest;
        }
        if (census_line_allowed()) {
            fprintf(stderr, "[draws]     indices %u..%u; the first:", lowest, highest);
            for (uint32_t i = 0; i < count && i < CENSUS_INDICES_SHOWN; i++)
                fprintf(stderr, " %u", *(const uint16_t *)XBOX_PTR(indices + 2 * i));
            fputc('\n', stderr);
        }
    } else {
        lowest = kind == CENSUS_POINTER ? 0 : first;
        highest = lowest + count - 1;
    }
    if (census_line_allowed()) {
        const struct fh_vsh *vsh = state.program_selected ? fh_vsh_selected(state.vertex_shader, state.program_address)
                                                          : fh_vsh_find(state.vertex_shader);

        if (vsh) {
            char text[512];

            fh_vsh_describe(vsh, text, sizeof text);
            if (state.program_selected)
                fprintf(stderr, "[draws]     vertex shader: selected, the program memory's from %u, %s%s\n",
                        state.program_address, text, vsh->merged ? " (streams merged)" : "");
            else
                fprintf(stderr, "[draws]     vertex shader: a program, %s\n", text);
        } else if (state.vertex_shader & 1) {
            fprintf(stderr, "[draws]     vertex shader: a program, handle 0x%08X (not made by CreateVertexShader)\n",
                    state.vertex_shader);
        } else {
            fvf_text(state.vertex_shader, fvf, sizeof fvf);
            fprintf(stderr, "[draws]     vertex shader: FVF 0x%08X (%s)\n", state.vertex_shader, fvf);
        }
    }
    for (uint32_t v = 0; kind == CENSUS_POINTER && up_data && v < CENSUS_VERTICES_SHOWN && v < count; v++)
        census_vertex(0, up_data + v * up_stride, up_stride, v);
    for (unsigned s = 0; s < FH_D3D_STREAMS && kind != CENSUS_POINTER; s++) {
        uint32_t buffer = state.streams[s].buffer, stride = state.streams[s].stride, data;

        if (!buffer)
            continue;
        data = MEM32(buffer + 4) | VERTEX_DATA_BIT;
        if (census_line_allowed())
            fprintf(stderr, "[draws]     stream %u: buffer 0x%08X, data 0x%08X, stride %u; common 0x%08X\n", s, buffer,
                    data, stride, MEM32(buffer));
        for (uint32_t v = 0; v < CENSUS_VERTICES_SHOWN && stride && lowest + v <= highest; v++)
            census_vertex(s, data + (lowest + v) * stride, stride, lowest + v);
    }
    for (unsigned stage = 0; stage < FH_D3D_STAGES; stage++)
        if (state.textures[stage])
            census_texture(stage, state.textures[stage]);
    census_guest_states();
}

/* ------------------------------------------------------------------------------------------------------------------
 * The functions */

uint32_t fh_d3d_set_stream_source(uint32_t stream, uint32_t buffer, uint32_t stride)
{
    if (stream < FH_D3D_STREAMS) {
        state.streams[stream].buffer = buffer;
        state.streams[stream].stride = stride;
    }
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SetStreamSource(stream %u, buffer 0x%08X, stride %u)\n", stream, buffer, stride);
    }
    return D3D_OK;
}

uint32_t fh_d3d_select_vertex_shader(uint32_t handle, uint32_t address)
{
    if (handle)
        state.vertex_shader = handle;
    state.program_address = address;
    state.program_selected = 1;
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SelectVertexShader(0x%08X, %u)  [ret to 0x%08X]\n", handle, address, MEM32(g_esp));
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_vertex_shader(uint32_t handle)
{
    state.vertex_shader = handle;
    state.program_selected = 0;
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SetVertexShader(0x%08X)  [ret to 0x%08X]\n", handle, MEM32(g_esp));
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_texture(uint32_t stage, uint32_t texture)
{
    if (stage < FH_D3D_STAGES)
        state.textures[stage] = texture;
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SetTexture(stage %u, 0x%08X)\n", stage, texture);
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_palette(uint32_t stage, uint32_t palette)
{
    if (stage < FH_D3D_STAGES)
        state.palettes[stage] = palette;
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SetPalette(stage %u, 0x%08X; data 0x%08X, common 0x%08X)\n", stage, palette,
                    palette ? MEM32(palette + 4) : 0, palette ? MEM32(palette) : 0);
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_render_state(uint32_t which, uint32_t value)
{
    if (which < GUEST_RENDER_STATES_KEPT)
        MEM32(GUEST_RENDER_STATES + 4 * which) = value;
    if (which < FH_D3D_RENDER_STATES) {
        state.render_states[which] = value;
        state.render_state_set[which] = 1;
    }
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SetRenderState(%u, 0x%08X)\n", which, value);
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage < FH_D3D_STAGES && type < FH_D3D_STAGE_STATES) {
        MEM32(GUEST_STAGE_STATES + 4 * (FH_D3D_STAGE_STATES * stage + type)) = value;
        state.stage_states[stage][type] = value;
        state.stage_state_set[stage][type] = 1;
    }
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SetTextureStageState(stage %u, type %u, 0x%08X)\n", stage, type, value);
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_transform(uint32_t which, uint32_t matrix)
{
    if (which < FH_D3D_TRANSFORMS && matrix) {
        for (int i = 0; i < 16; i++)
            state.transforms[which][i] = as_float(MEM32(matrix + 4 * i));
        state.transform_set[which] = 1;
    }
    if (census) {
        census_calls++;
        if (census_line_allowed() && which < FH_D3D_TRANSFORMS && matrix) {
            const float *m = state.transforms[which];

            fprintf(stderr, "[draws] SetTransform(%u, 0x%08X): %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                    which, matrix, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
                    m[13], m[14], m[15]);
        } else if (census_line_allowed()) {
            fprintf(stderr, "[draws] SetTransform(%u, 0x%08X)\n", which, matrix);
        }
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_viewport(uint32_t viewport)
{
    if (viewport) {
        for (int i = 0; i < 4; i++)
            state.viewport[i] = MEM32(viewport + 4 * i);
        state.viewport_depth[0] = as_float(MEM32(viewport + 16));
        state.viewport_depth[1] = as_float(MEM32(viewport + 20));
    }
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] SetViewport(0x%08X): x %u, y %u, %u x %u, depth %g..%g\n", viewport,
                    state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3],
                    (double)state.viewport_depth[0], (double)state.viewport_depth[1]);
    }
    return D3D_OK;
}

/* [fh7] see fh_d3d_state.h */
uint32_t fh_d3d_set_light(uint32_t index, uint32_t light)
{
    if (index < FH_D3D_LIGHTS && light) {
        for (int i = 0; i < FH_D3D_LIGHT_WORDS; i++)
            state.lights[index][i] = MEM32(light + 4 * i);
        state.light_set[index] = 1;
    }
    if (census) {
        census_calls++;
        if (census_line_allowed() && index < FH_D3D_LIGHTS && light)
            fprintf(stderr, "[draws] SetLight(%u, 0x%08X): type %u, diffuse %g %g %g, direction %g %g %g\n", index,
                    light, state.lights[index][0], (double)as_float(state.lights[index][1]),
                    (double)as_float(state.lights[index][2]), (double)as_float(state.lights[index][3]),
                    (double)as_float(state.lights[index][16]), (double)as_float(state.lights[index][17]),
                    (double)as_float(state.lights[index][18]));
    }
    return D3D_OK;
}

uint32_t fh_d3d_light_enable(uint32_t index, uint32_t enable)
{
    if (index < FH_D3D_LIGHTS)
        state.light_on[index] = enable != 0;
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] LightEnable(%u, %u)\n", index, enable);
    }
    return D3D_OK;
}

uint32_t fh_d3d_set_material(uint32_t material)
{
    if (material) {
        for (int i = 0; i < FH_D3D_MATERIAL_WORDS; i++)
            state.material[i] = MEM32(material + 4 * i);
        state.material_set = 1;
    }
    if (census) {
        census_calls++;
        if (census_line_allowed() && material)
            fprintf(stderr, "[draws] SetMaterial(0x%08X): diffuse %g %g %g %g, ambient %g %g %g, emissive %g %g %g\n",
                    material, (double)as_float(state.material[0]), (double)as_float(state.material[1]),
                    (double)as_float(state.material[2]), (double)as_float(state.material[3]),
                    (double)as_float(state.material[4]), (double)as_float(state.material[5]),
                    (double)as_float(state.material[6]), (double)as_float(state.material[12]),
                    (double)as_float(state.material[13]), (double)as_float(state.material[14]));
    }
    return D3D_OK;
}

uint32_t fh_d3d_draw_indexed_vertices(uint32_t primitive_type, uint32_t index_count, uint32_t indices)
{
    if (census) {
        census_calls++;
        census_draw(CENSUS_INDEXED, primitive_type, index_count, indices, 0, 0, 0);
    }
    fh_draw_indexed(primitive_type, index_count, indices);     /* on Vulkan (fh_draw.c) */
    return D3D_OK;
}

uint32_t fh_d3d_draw_vertices(uint32_t primitive_type, uint32_t first, uint32_t count)
{
    if (census) {
        census_calls++;
        census_draw(CENSUS_STREAMS, primitive_type, count, 0, first, 0, 0);
    }
    fh_draw_vertices(primitive_type, first, count);
    return D3D_OK;
}

uint32_t fh_d3d_draw_vertices_up(uint32_t primitive_type, uint32_t count, uint32_t data, uint32_t stride)
{
    if (census) {
        census_calls++;
        census_draw(CENSUS_POINTER, primitive_type, count, 0, 0, data, stride);
    }
    fh_draw_vertices_up(primitive_type, count, data, stride);
    return D3D_OK;
}

/* FH_SCREEN_ANSWERS=0: GetViewportOffsetAndScale and the multisample scale write nothing, as their stand-ins did (for
 * comparison runs). */
static int screen_answers(void)
{
    static int answers = -1;

    if (answers < 0)
        answers = !(getenv("FH_SCREEN_ANSWERS") && strcmp(getenv("FH_SCREEN_ANSWERS"), "0") == 0);
    return answers;
}

static uint32_t float_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof bits);
    return bits;
}

uint32_t fh_d3d_get_viewport_offset_and_scale(uint32_t offset, uint32_t scale)
{
    static const float scale_now[4] = {1.0f, 1.0f, 1.0f, 0.0f};

    if (screen_answers()) {
        for (int i = 0; offset && i < 4; i++)
            MEM32(offset + 4 * i) = 0;
        for (int i = 0; scale && i < 4; i++)
            MEM32(scale + 4 * i) = float_bits(scale_now[i]);
    }
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] GetViewportOffsetAndScale(0x%08X, 0x%08X)%s  [ret to 0x%08X]\n", offset, scale,
                    screen_answers() ? ": offset (0, 0, 0, 0), scale (1, 1, 1, 0)" : ": not answered", MEM32(g_esp));
    }
    return D3D_OK;
}

uint32_t fh_d3d_get_multisample_scale(uint32_t x, uint32_t y)
{
    const float width = state.viewport[2] ? (float)state.viewport[2] : 640.0f;
    const float height = state.viewport[3] ? (float)state.viewport[3] : 480.0f;

    if (screen_answers()) {
        if (x)
            MEM32(x) = float_bits(2.0f / width);
        if (y)
            MEM32(y) = float_bits(-2.0f / height);
    }
    if (census) {
        census_calls++;
        if (census_line_allowed())
            fprintf(stderr, "[draws] multisample scale(0x%08X, 0x%08X)%s %g, %g  [ret to 0x%08X]\n", x, y,
                    screen_answers() ? ":" : ": not answered; would be", (double)(2.0f / width),
                    (double)(-2.0f / height), MEM32(g_esp));
    }
    return D3D_OK;
}

void fh_d3d_state_frame_end(unsigned long frame)
{
    static int read_setting;
    static const char *census_frames;

    if (census)
        fprintf(stderr, "[draws] end of frame %lu: %lu calls, %lu draws\n", frame, census_calls, census_draws);
    frames_ended = frame;
    if (!read_setting) {
        const char *lines = getenv("FH_DRAW_LOG_LINES");

        read_setting = 1;
        census_frames = getenv("FH_DRAW_LOG");
        if (lines && atoi(lines) > 0)
            census_lines_max = (unsigned)atoi(lines);
    }
    census = census_frames && in_list(census_frames, frames_ended + 1);
    if (census) {
        census_lines = 0;
        census_calls = census_draws = 0;
        fprintf(stderr, "[draws] frame %lu: the draw census (FH_DRAW_LOG)\n", frames_ended + 1);
        census_states_so_far();
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * [acct2-fh] 11-22: D3DDevice_Clear (see fh_d3d_state.h). Read 2026-11-22: the Xbox Direct3D's clear (0x002271C0, six
 * arguments, 24 bytes) writes the NV2A's clear methods (0x1D8C...) into the push buffer; RenderWare reaches it from
 * its camera clear (0x001CF7C0, in its device's table at 0x001CF898) and from 0x001D6360. Before build68 it was one
 * of the "zero" stand-ins, so a clear of the depth in the middle of a frame (a second camera drawn over the world,
 * as a first-person model is) never happened, and the world's depth stayed under it. */
#define XBOX_CLEAR_ZBUFFER  0x1u
#define XBOX_CLEAR_STENCIL  0x2u
#define XBOX_CLEAR_TARGET   0xF0u
#define CLEARS_SHOWN        8       /* the first few kinds of clear are printed once each */

static struct fh_d3d_clear_stats clear_stats;
static int depth_clear = -1;
static unsigned depth_clear_generation;

int fh_d3d_depth_clear_on(void)
{
    unsigned generation = fh_settings_generation();

    if (depth_clear < 0 || generation != depth_clear_generation) {
        int was = depth_clear;

        depth_clear_generation = generation;
        depth_clear = fh_setting_bool("video.depth_clear", "FH_DEPTH_CLEAR", 1);
        if (depth_clear != was)
            fprintf(stderr, "[fh] the game's depth clears in the middle of a frame: %s\n", depth_clear
                    ? "done (video.depth_clear)" : "left out, as before build68 (video.depth_clear off)");
    }
    return depth_clear;
}

uint32_t fh_d3d_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, uint32_t z_bits,
                      uint32_t stencil)
{
    static struct { uint32_t count, flags; int mid; } shown[CLEARS_SHOWN];
    static int shown_count;
    const int on = fh_d3d_depth_clear_on();
    const int mid = fh_draw_frame_has_draws();
    float z;
    int i;

    memcpy(&z, &z_bits, sizeof z);
    (void)rects;
    clear_stats.calls++;
    for (i = 0; i < shown_count; i++)
        if (shown[i].count == (count ? 1u : 0u) && shown[i].flags == flags && shown[i].mid == mid)
            break;
    if (i == shown_count && shown_count < CLEARS_SHOWN) {
        shown[shown_count].count = count ? 1u : 0u;
        shown[shown_count].flags = flags;
        shown[shown_count].mid = mid;
        shown_count++;
        fprintf(stderr, "[clears] a new kind: Count %u, Flags 0x%X, color 0x%08X, Z %g, stencil %u, %s (frame %lu)\n",
                count, flags, color, (double)z, stencil, mid ? "after draws in the frame" : "before the frame's draws",
                fh_d3d_frame());
    }
    if (flags & XBOX_CLEAR_TARGET)
        clear_stats.color++;
    if (!(flags & (XBOX_CLEAR_ZBUFFER | XBOX_CLEAR_STENCIL)))
        return 0;
    clear_stats.depth++;
    if (!mid) {
        clear_stats.depth_before_draws++;
        return 0;
    }
    clear_stats.depth_mid_frame++;
    if (count) {
        clear_stats.with_rects++;
        return 0;
    }
    if (on && fh_draw_clear_depth(z, stencil & 0xFFu))
        clear_stats.honoured++;
    return 0;
}

void fh_d3d_take_clear_stats(struct fh_d3d_clear_stats *out)
{
    *out = clear_stats;
    clear_stats = (struct fh_d3d_clear_stats){0};
}
