/* [acct2-fh] Finest Hour port: the game's vertex shaders as Direct3D creates them (2026-11-13).
 *
 * Why this exists. In levels the game draws its crowds of soldiers ("clones", 0x000D9F90-0x000DA267) with a vertex
 * shader of its own: 0x0012DC90 reads d:\clones.xvu and hands it, with a declaration at 0x003221CC, to 0x002212F0.
 * That function's own code (read 2026-11-13, dev/agents/acct2-fh/VERTEX-SHADERS.md) is Direct3D's
 * CreateVertexShader(pDeclaration, pFunction, pHandle, Usage):
 *  - pFunction's first 16-bit word is the shader's kind (0x2078, "x ", a vertex program; 0x7378 "xs" a state
 *    shader), its second the number of 128-bit instructions, and the instructions follow from pFunction + 4;
 *  - it makes an object in contiguous memory (0x00134A34), fills it from the declaration (0x00221050, 0x002211F0) and
 *    the program (0x00220FC0) and writes the object's address with bit 0 set to *pHandle; SetVertexShader later tells
 *    a program from an FVF code by that bit.
 * While it was a stand-in it wrote nothing, so the game's handle stayed 0 and the port could not know the clones'
 * vertex layout: their draws were left out.
 *
 * This file is its native answer. It reads the declaration itself (the tokens' layout below), keeps each input
 * register's stream, offset and size and a copy of the program, makes a small zeroed object in guest memory (so the
 * handle is a real address, as the game may expect) and writes object | 1 to *pHandle. fh_draw.c and the census look
 * the shader up by its handle. The second part of this file runs the programs on the CPU, one vertex at a time
 * (fh_vsh_run); each program is decoded once, when it is made, into plain fields (struct fh_vsh_op).
 *
 * The declaration's tokens, as the game's own declarations use them (0x003221CC: 0x20000000, 0x40320000,
 * 0x40220001, 0x40420002, 0x40320003, 0xFFFFFFFF for its 48-byte clone vertices):
 *   bits 29-31: the token type. 1 = select a stream (the stream number in bits 0-3); 2 = stream data: with bit 28
 *   clear, a register (bits 0-4) of a data type (bits 16-23); with bit 28 set, words to skip (bits 16-19);
 *   4 = constants to load (a count of 4-word constants in bits 25-28; the constants follow); 5 = an extension
 *   (a count of words in bits 24-28; they follow); 0 = nothing; 0xFFFFFFFF = the end.
 *   A data type's low 4 bits give the kind of each component and its high 4 bits the count. The sizes below are
 *   checked by the census against each draw's SetStreamSource stride; a kind not listed is counted as a problem,
 *   not guessed. */
#include "fh_vsh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32, g_esp: guest memory and the guest's stack */
#include "xbox_memory_layout.h"     /* XBOX_PTR, xbox_HeapAlloc */
#include "fh_push.h"                /* [acct2-fh] the game's own push buffer writes, read first */

#define D3D_OK                  0u
#define CREATE_OUT_OF_MEMORY    0x8007000Eu     /* what the real function returns when its allocation fails */

#define MAX_SHADERS             256
#define MAX_TOKENS              512             /* a declaration longer than this is cut and counted as a problem */
#define MAX_INSTRUCTIONS        136             /* the NV2A's program memory holds 136 instructions */
#define OBJECT_BYTES            16

#define TOKEN_TYPE(t)           ((t) >> 29)
#define TOKEN_NOP               0u
#define TOKEN_STREAM            1u
#define TOKEN_STREAMDATA        2u
#define TOKEN_CONSTMEM          4u
#define TOKEN_EXT               5u
#define TOKEN_END               0xFFFFFFFFu

static struct fh_vsh shaders[MAX_SHADERS];
static unsigned shader_count;

static const struct fh_vsh_op *decode_program(const uint32_t *words, unsigned instructions);
static void program_facts(struct fh_vsh *vsh);

/* A data type's size in bytes, or 0 when this file does not know the kind. */
static unsigned type_bytes(unsigned type)
{
    unsigned kind = type & 0xF, count = type >> 4;

    switch (kind) {
    case 0x2:                           /* floats: FLOAT1-4 (0x12-0x42); 0x72 is three floats (x, y, w) */
        return count == 7 ? 12 : 4 * count;
    case 0x0:                           /* D3DCOLOR (0x40): four bytes */
        return count == 4 ? 4 : 0;
    case 0x1:                           /* normalized shorts (0x11-0x41) */
    case 0x5:                           /* shorts (0x15-0x45) */
        return 2 * count;
    case 0x4:                           /* bytes (0x14-0x44) */
        return count;
    case 0x6:                           /* three components packed into one word (0x16) */
        return count == 1 ? 4 : 0;
    default:
        return 0;
    }
}

static const char *type_name(unsigned type)
{
    switch (type) {
    case 0x12: return "float1";
    case 0x22: return "float2";
    case 0x32: return "float3";
    case 0x42: return "float4";
    case 0x72: return "float2h";
    case 0x40: return "color";
    case 0x11: return "normshort1";
    case 0x21: return "normshort2";
    case 0x31: return "normshort3";
    case 0x41: return "normshort4";
    case 0x15: return "short1";
    case 0x25: return "short2";
    case 0x35: return "short3";
    case 0x45: return "short4";
    case 0x14: return "byte1";
    case 0x24: return "byte2";
    case 0x34: return "byte3";
    case 0x44: return "byte4";
    case 0x16: return "packed3";
    case 0x02: return "none";
    default:   return "?";
    }
}

/* Reads the declaration's tokens into the shader's inputs. */
static void read_declaration(struct fh_vsh *vsh, uint32_t declaration)
{
    unsigned stream = 0;

    for (unsigned i = 0; i < MAX_TOKENS; i++) {
        uint32_t token = MEM32(declaration + 4 * i);
        unsigned type = TOKEN_TYPE(token);

        if (token == TOKEN_END)
            return;
        if (type == TOKEN_NOP)
            continue;
        if (type == TOKEN_STREAM) {
            stream = token & 0xF;
        } else if (type == TOKEN_STREAMDATA && !(token & 0x10000000u)) {
            unsigned reg = token & 0x1F, data_type = (token >> 16) & 0xFF, bytes = type_bytes(data_type);

            if (reg >= FH_VSH_INPUTS || (!bytes && data_type != 0x02)) {
                vsh->declaration_problems++;
                continue;
            }
            vsh->inputs[reg] = (struct fh_vsh_input){1, (uint8_t)stream, (uint8_t)data_type, (uint8_t)bytes,
                                                     vsh->stream_bytes[stream]};
            vsh->stream_bytes[stream] += bytes;
        } else if (type == TOKEN_STREAMDATA) {
            vsh->stream_bytes[stream] += 4 * ((token >> 16) & 0xF);     /* skipped words */
        } else if (type == TOKEN_CONSTMEM) {
            i += 4 * ((token >> 25) & 0xF);                             /* its constants follow */
        } else if (type == TOKEN_EXT) {
            i += (token >> 24) & 0x1F;                                  /* its words follow */
        } else {
            vsh->declaration_problems++;
        }
    }
    vsh->declaration_problems++;                                        /* no end token in MAX_TOKENS */
}

uint32_t fh_d3d_create_vertex_shader(uint32_t declaration, uint32_t function, uint32_t handle_out, uint32_t usage)
{
    struct fh_vsh *vsh;
    uint32_t object;
    char text[512];

    (void)usage;
    if (shader_count == MAX_SHADERS) {
        fprintf(stderr, "[fh] CreateVertexShader: more than %u shaders; this one gets no handle\n", MAX_SHADERS);
        return CREATE_OUT_OF_MEMORY;
    }
    object = xbox_HeapAlloc(OBJECT_BYTES, 16);
    if (!object) {
        fprintf(stderr, "[fh] CreateVertexShader: no guest memory for its object\n");
        return CREATE_OUT_OF_MEMORY;
    }
    memset((void *)XBOX_PTR(object), 0, OBJECT_BYTES);

    vsh = &shaders[shader_count++];
    memset(vsh, 0, sizeof *vsh);
    vsh->handle = object | 1;
    vsh->fixed_function = function == 0;
    if (declaration)
        read_declaration(vsh, declaration);
    if (function) {
        uint32_t head = MEM32(function);

        vsh->program_kind = (uint16_t)(head & 0xFFFF);
        vsh->instructions = (uint16_t)(head >> 16);
        if (vsh->instructions && vsh->instructions <= MAX_INSTRUCTIONS) {
            uint32_t *copy = malloc((size_t)vsh->instructions * 16);

            if (copy) {
                memcpy(copy, (const void *)XBOX_PTR(function + 4), (size_t)vsh->instructions * 16);
                vsh->program = copy;
                vsh->ops = decode_program(copy, vsh->instructions);
                program_facts(vsh);
            }
        }
    }
    if (handle_out)
        MEM32(handle_out) = vsh->handle;

    fh_vsh_describe(vsh, text, sizeof text);
    fprintf(stderr, "[fh] CreateVertexShader(declaration 0x%08X, function 0x%08X) [ret to 0x%08X]: %s\n", declaration,
            function, MEM32(g_esp), text);
    /* the program's words, once per shader, for reading the NV2A's instruction format against the census */
    for (unsigned i = 0; vsh->program && i < vsh->instructions; i++)
        fprintf(stderr, "[fh]     0x%08X instruction %2u: %08X %08X %08X %08X\n", vsh->handle, i,
                vsh->program[4 * i], vsh->program[4 * i + 1], vsh->program[4 * i + 2], vsh->program[4 * i + 3]);
    return D3D_OK;
}

const struct fh_vsh *fh_vsh_find(uint32_t handle)
{
    if (!(handle & 1))
        return NULL;
    for (unsigned i = 0; i < shader_count; i++)
        if (shaders[i].handle == handle)
            return &shaders[i];
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------
 * [acct2-fh] 11-13: the program memory and the programs drawn from it (fh_vsh.h says what the game's functions do) */

#define SELECTED_KEPT           32              /* (declaration, address, program) triples kept */

static uint32_t program_memory[MAX_INSTRUCTIONS * 4];

/* boot29 (11-13 17:24): RenderWare loads its 1-, 2- and 3-bone programs (25, 32 and 36 instructions) in turn at
 * address 12, one before each skinned draw, so a kept shader is found by the program's words themselves (a hash and
 * the count), not by when the memory last changed: that remade one for every draw. */
static struct {
    uint32_t handle, address, hash, count;
    struct fh_vsh vsh;
} selected_kept[SELECTED_KEPT];
static unsigned selected_next;

/* The program from address on: its instruction count up to the last instruction (0 when the memory past address
 * never ends one) and a hash of its words (FNV-1a). */
static uint32_t program_at(uint32_t address, unsigned *count_out)
{
    uint32_t hash = 2166136261u;
    unsigned count = 0;

    while (address + count < MAX_INSTRUCTIONS) {
        const uint32_t *words = &program_memory[(address + count) * 4];

        for (unsigned k = 0; k < 4; k++)
            hash = (hash ^ words[k]) * 16777619u;
        count++;
        if (words[3] & 1)                   /* the last instruction */
            break;
    }
    if (count && !(program_memory[(address + count - 1) * 4 + 3] & 1))
        count = 0;
    *count_out = count;
    return hash;
}

uint32_t fh_d3d_load_vertex_shader_program(uint32_t function, uint32_t address)
{
    uint32_t count;

    if (!function || address >= MAX_INSTRUCTIONS)
        return D3D_OK;
    count = MEM32(function) >> 16;
    if (count > MAX_INSTRUCTIONS - address)
        count = MAX_INSTRUCTIONS - address;
    memcpy(&program_memory[address * 4], (const void *)XBOX_PTR(function + 4), (size_t)count * 16);
    return D3D_OK;
}

/* The selected shader for one (declaration, address) pair, made from the declaration and the program memory. */
static int make_selected(struct fh_vsh *vsh, const struct fh_vsh *declaration, uint32_t address, unsigned count)
{
    unsigned streams = 0, at = 0;
    uint32_t *copy;

    if (!count)
        return 0;                       /* no program there: the memory past Address never ends one */
    copy = malloc((size_t)count * 16);
    if (!copy)
        return 0;
    memcpy(copy, &program_memory[address * 4], (size_t)count * 16);

    *vsh = *declaration;
    vsh->selected = 1;
    vsh->fixed_function = 0;
    vsh->program_kind = 0x2078;
    vsh->instructions = (uint16_t)count;
    vsh->program = copy;
    vsh->ops = decode_program(copy, count);
    program_facts(vsh);

    /* more than one stream: the streams' bytes side by side, in stream order */
    for (unsigned r = 0; r < FH_VSH_INPUTS; r++)
        if (vsh->inputs[r].used)
            streams |= 1u << vsh->inputs[r].stream;
    if (streams & ~1u) {
        vsh->merged = 1;
        vsh->merged_streams = (uint16_t)streams;
        for (unsigned s = 0; s < FH_VSH_STREAMS; s++) {
            vsh->source_bytes[s] = declaration->stream_bytes[s];
            vsh->merged_at[s] = (uint16_t)at;
            if (streams & (1u << s))
                at += declaration->stream_bytes[s];
        }
        for (unsigned r = 0; r < FH_VSH_INPUTS; r++)
            if (vsh->inputs[r].used) {
                vsh->inputs[r].offset = (uint16_t)(vsh->merged_at[vsh->inputs[r].stream] + vsh->inputs[r].offset);
                vsh->inputs[r].stream = 0;
            }
        memset(vsh->stream_bytes, 0, sizeof vsh->stream_bytes);
        vsh->stream_bytes[0] = (uint16_t)at;
    }
    return vsh->ops != NULL;
}

static void log_constants_once(void);   /* below, by the constant registers */

const struct fh_vsh *fh_vsh_selected(uint32_t handle, uint32_t address)
{
    const struct fh_vsh *declaration = fh_vsh_find(handle);
    unsigned slot, count;
    uint32_t hash;

    if (!declaration || address >= MAX_INSTRUCTIONS)
        return NULL;
    hash = program_at(address, &count);
    if (!count)
        return NULL;
    for (unsigned i = 0; i < SELECTED_KEPT; i++)
        if (selected_kept[i].count == count && selected_kept[i].hash == hash && selected_kept[i].handle == handle &&
            selected_kept[i].address == address &&
            memcmp(selected_kept[i].vsh.program, &program_memory[address * 4], (size_t)count * 16) == 0)
            return &selected_kept[i].vsh;

    slot = selected_next++ % SELECTED_KEPT;
    free((void *)selected_kept[slot].vsh.program);
    free((void *)selected_kept[slot].vsh.ops);
    memset(&selected_kept[slot], 0, sizeof selected_kept[slot]);
    if (!make_selected(&selected_kept[slot].vsh, declaration, address, count)) {
        free((void *)selected_kept[slot].vsh.program);
        free((void *)selected_kept[slot].vsh.ops);
        memset(&selected_kept[slot], 0, sizeof selected_kept[slot]);
        return NULL;
    }
    selected_kept[slot].handle = handle;
    selected_kept[slot].address = address;
    selected_kept[slot].hash = hash;
    selected_kept[slot].count = count;
    if (getenv("FH_VSH_SELECTED_LOG")) {    /* each one made: what the skinned pipeline draws with */
        char text[512];

        fh_vsh_describe(&selected_kept[slot].vsh, text, sizeof text);
        fprintf(stderr, "[fh] selected program at %u for 0x%08X: %s%s\n", address, handle, text,
                selected_kept[slot].vsh.merged ? " (streams merged)" : "");
        for (unsigned i = 0; i < count; i++)   /* its words, as CreateVertexShader prints a program's */
            fprintf(stderr, "[fh]     selected %u/0x%08X instruction %2u: %08X %08X %08X %08X\n", address, handle, i,
                    program_memory[(address + i) * 4], program_memory[(address + i) * 4 + 1],
                    program_memory[(address + i) * 4 + 2], program_memory[(address + i) * 4 + 3]);
        log_constants_once();
    }
    return &selected_kept[slot].vsh;
}

void fh_vsh_describe(const struct fh_vsh *vsh, char *text, unsigned size)
{
    int n = snprintf(text, size, "handle 0x%08X", vsh->handle);
    const char *separator = ";";

    for (unsigned r = 0; r < FH_VSH_INPUTS && n > 0 && (unsigned)n < size; r++) {
        const struct fh_vsh_input *in = &vsh->inputs[r];

        if (!in->used)
            continue;
        n += snprintf(text + n, size - n, "%s v%u %s at %u (stream %u)", separator, r, type_name(in->type),
                      in->offset, in->stream);
        separator = ",";
    }
    for (unsigned s = 0; s < FH_VSH_STREAMS && n > 0 && (unsigned)n < size; s++)
        if (vsh->stream_bytes[s])
            n += snprintf(text + n, size - n, "; stream %u vertex %u bytes", s, vsh->stream_bytes[s]);
    if (n > 0 && (unsigned)n < size)
        n += snprintf(text + n, size - n, "; program kind 0x%04X, %u instructions%s", vsh->program_kind,
                      vsh->instructions, vsh->program ? "" : " (not kept)");
    if (n > 0 && (unsigned)n < size && vsh->declaration_problems)
        snprintf(text + n, size - n, "; %u declaration token(s) not read", vsh->declaration_problems);
}

/* ------------------------------------------------------------------------------------------------------------------
 * Running a program on the CPU (2026-11-13)
 *
 * The instruction layout is our own reading of the NV2A's 128-bit vertex program words, checked on the game's own
 * programs (dev/agents/acct2-fh/tools/fh_vsh_decode.py prints them; VERTEX-SHADERS.md): with it, clones.xvu reads as
 * a sensible program, four dp4 of v0 with c0-c3 into oPos (c0-c3 being the matrix the game loads per clone through
 * 0x00221500), a light term from the normal v3 and c6 into oD0 with c4 and c5, v1 into oT0, and the Xbox's usual
 * screen mapping at the end. Each instruction drives two units at once, the MAC (vector) and the ILU (scalar); both
 * read their operands before either writes. A field is (word, first bit, bits):
 *   word 1: ILU op 25/3, MAC op 21/4, constant index 13/8, input index 9/4, operand A: negate 8/1, swizzle 0/8
 *   word 2: A temp 28/4, A source 26/2; B: negate 25, swizzle 17/8, temp 13/4, source 11/2;
 *           C: negate 10, swizzle 2/8, temp (high 2 bits) 0/2
 *   word 3: C temp (low 2 bits) 30/2, C source 28/2, MAC write mask 24/4, temp written 20/4, ILU write mask 16/4,
 *           output write mask 12/4, output is an o register (1) or a constant (0) 11/1, output index 3/8,
 *           output from the ILU (1) or the MAC (0) 2/1, constant index relative to a0.x 1/1, last instruction 0/1
 * A source: 1 a temp r0-r11 (r12 reads oPos), 2 the input v[input index], 3 the constant c[constant index]. A
 * swizzle is four 2-bit picks, x first (bits 7-6). Masks are x y z w from bit 3 down. When both units write a temp,
 * the ILU's goes to r1. MAC ops: 0 nop, 1 mov (A), 2 mul (A B), 3 add (A C), 4 mad (A B C), 5 dp3, 6 dph, 7 dp4,
 * 8 dst, 9 min, 10 max, 11 slt, 12 sge, 13 arl (A). ILU ops (on C): 0 nop, 1 mov, 2 rcp, 3 rcc, 4 rsq, 5 exp, 6 log,
 * 7 lit. Outputs: 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7 oB0, 8 oB1, 9-12 oT0-oT3.
 *
 * The screen mapping. Every program here ends the Xbox way: oPos.xyz = r12.xyz * c-38, then
 * oPos.xyz = r12.xyz * (1 / r12.w) + c-37, with the viewport's scale in c-38 and its offset in c-37 (Direct3D keeps
 * them there). The port draws in Vulkan's clip space and lets Vulkan map to the screen, so while a program runs here
 * c-38 is (1, 1, 1, 1) and c-37 is (0, 0, 0, 0): oPos then ends as (x/w, y/w, z/w, w). */

#define CONSTANT_VIEWPORT_SCALE     (96 - 38)
#define CONSTANT_VIEWPORT_OFFSET    (96 - 37)
#define OUTPUT_REGISTERS            13
#define OUTPUT_POSITION             0
#define OUTPUT_DIFFUSE              3
#define OUTPUT_FOG                  5
#define OUTPUT_TEXCOORD0            9
#define TEMPS                       13          /* r0-r11, and r12 (oPos as read) */

static float constants[FH_VSH_CONSTANT_REGISTERS][4];

#define GUEST_CONSTANT_COPY         0x0022F900u     /* the game's copy of the 192 registers, 16 bytes each */
#define GUEST_DEVICE_FLAGS          0x002309C8u     /* a byte; 0x10: SetShaderConstantMode's "no reserved constants" */

/* [acct2-fh] 2026-11-14 SetShaderConstantMode (0x00221900), read from its own code: the mode is its stack argument;
 * bit 0x10 (no reserved constants) sets the device's flag 0x200 (device + 8) and goes, the rest is kept at
 * device + 0x1928 (GetShaderConstantMode, 0x00221030, hands that back through its one argument). When the rest is 0
 * (96 constants) it pushes register 60 (0x1EA4 = 0x3C) and the three registers c-36..c-34 from 0x0022E340 (12 words:
 * (0, 0.5, 1, 2), (-1, 0, 1, 2), (0, 0, -1, 0)), then the fixed-function matrices (not the port's: fh_draw.c draws
 * those its own way). RenderWare's skinned meshes run with 0x11 (192 constants, none reserved): their bone matrices
 * fill registers 14-190 (three a bone, from c-82), so registers 58 and 59 (c-38, c-37: the viewport's scale and
 * offset in the other modes) are bone 14's third row and bone 15's first. Until 11-14 the port set c-38 = 1 and
 * c-37 = 0 for every draw, those two bones included: the soldiers' foreheads and fingers came out stretched (owner,
 * 11-14 13:19). Now the screen-mapping registers are set only while they are reserved. FH_CONSTANT_MODE=0 ignores
 * the mode as before (for comparison runs). */
#define GUEST_DEVICE_POINTER        0x002309B8u     /* Direct3D's device */
#define MODE_NO_RESERVED            0x10u
#define RESERVED_RELOADED_FIRST     60u             /* c-36: three registers from here, loaded again in mode 0 */
#define GUEST_RESERVED_VALUES       0x0022E340u     /* their 12 words in the game's data */

static uint32_t constant_mode;                      /* as last set, 0x10 included */
static int follow_mode = -1;                        /* FH_CONSTANT_MODE: -1 not looked at yet */

static int screen_registers_reserved(void)
{
    if (follow_mode < 0) {
        const char *v = getenv("FH_CONSTANT_MODE");

        follow_mode = !(v && v[0] == '0' && v[1] == 0);
    }
    return !follow_mode || !(constant_mode & MODE_NO_RESERVED);
}

/* words 32-bit words from data into the registers from first_register on, a register's x y z w in turn; a word
 * past register 191 is dropped */
static void load_words(uint32_t first_register, uint32_t data, uint32_t words)
{
    for (uint32_t i = 0; i < words && first_register + i / 4 < FH_VSH_CONSTANT_REGISTERS; i++)
        memcpy(&constants[first_register + i / 4][i % 4], (const void *)XBOX_PTR(data + 4 * i), 4);
}

/* [acct2-fh] 11-13: with FH_VSH_SELECTED_LOG, the constant registers as the first selected program is drawn (those
 * not all zero), to read the skinning programs against: register n is c(n - 96) in Direct3D's numbering. */
static void log_constants_once(void)
{
    static int done;

    if (done++)
        return;
    for (unsigned r = 0; r < FH_VSH_CONSTANT_REGISTERS; r++)
        if (constants[r][0] != 0.0f || constants[r][1] != 0.0f || constants[r][2] != 0.0f || constants[r][3] != 0.0f)
            fprintf(stderr, "[fh]     constant %3u (c%d): %g %g %g %g\n", r, (int)r - 96, constants[r][0],
                    constants[r][1], constants[r][2], constants[r][3]);
}

static void keep_guest_copy(uint32_t first_register, uint32_t data, uint32_t words)
{
    for (uint32_t i = 0; i < words && first_register + i / 4 < FH_VSH_CONSTANT_REGISTERS; i++)
        MEM32(GUEST_CONSTANT_COPY + 16 * first_register + 4 * i) = MEM32(data + 4 * i);
}

uint32_t fh_d3d_set_shader_constant_mode(uint32_t mode)
{
    static unsigned long sets;
    const uint32_t device = MEM32(GUEST_DEVICE_POINTER);

    fh_push_read();     /* what the game pushed before this comes first */
    if (device) {
        MEM32(device + 8) = mode & MODE_NO_RESERVED ? MEM32(device + 8) | 0x200u : MEM32(device + 8) & ~0x200u;
        MEM32(device + 0x1928) = mode & ~MODE_NO_RESERVED;
    }
    if (!(mode & ~MODE_NO_RESERVED))                /* 96 constants: c-36..c-34 loaded again, as its push does */
        load_words(RESERVED_RELOADED_FIRST, GUEST_RESERVED_VALUES, 12);
    if (++sets <= 2 && mode != constant_mode)
        fprintf(stderr, "[fh] SetShaderConstantMode(0x%X): %s\n", mode,
                mode & MODE_NO_RESERVED ? "no reserved constants: c-38 and c-37 stay as the game loads them (the "
                                          "skinned meshes' bones)"
                                        : "c-38 and c-37 are the screen mapping again");
    constant_mode = mode;
    return 0;
}

uint32_t fh_d3d_get_shader_constant_mode(uint32_t mode_out)
{
    const uint32_t device = MEM32(GUEST_DEVICE_POINTER);

    if (mode_out)
        MEM32(mode_out) = device ? MEM32(device + 0x1928) : constant_mode & ~MODE_NO_RESERVED;
    return 0;
}

void fh_vsh_constant_word(uint32_t reg, unsigned word, uint32_t bits)
{
    if (reg < FH_VSH_CONSTANT_REGISTERS && word < 4)
        memcpy(&constants[reg][word], &bits, 4);
}

uint32_t fh_d3d_set_vertex_shader_constants(uint32_t first_register, uint32_t data, uint32_t count)
{
    fh_push_read();     /* what the game pushed before this write comes first */
    keep_guest_copy(first_register, data, 4 * count);
    load_words(first_register, data, 4 * count);
    return D3D_OK;
}

uint32_t fh_d3d_load_vertex_shader_constant_words(uint32_t first_register, uint32_t data, uint32_t words)
{
    fh_push_read();     /* what the game pushed before this write comes first */
    load_words(first_register, data, words);
    return D3D_OK;
}

uint32_t fh_d3d_set_vertex_shader_constant_words(uint32_t first_register, uint32_t data, uint32_t words)
{
    fh_push_read();     /* what the game pushed before this write comes first */
    if (!(*(const uint8_t *)XBOX_PTR(GUEST_DEVICE_FLAGS) & 0x10))
        keep_guest_copy(first_register, data, words);
    load_words(first_register, data, words);
    return D3D_OK;
}

static uint32_t bits(const uint32_t *words, unsigned word, unsigned first, unsigned count)
{
    return (words[word] >> first) & ((1u << count) - 1);
}

/* An input register's four components from the vertex, as the declaration types it; 0 for a kind not read yet. */
static int read_input(const struct fh_vsh_input *in, const uint8_t *vertex, float v[4])
{
    const uint8_t *p = vertex + in->offset;
    unsigned kind = in->type & 0xF, count = in->type >> 4;

    v[0] = v[1] = v[2] = 0.0f;
    v[3] = 1.0f;
    if (kind == 0x2 && count >= 1 && count <= 4) {               /* floats */
        memcpy(v, p, 4 * count);
    } else if (in->type == 0x40) {                               /* D3DCOLOR, 0xAARRGGBB: red, green, blue, alpha */
        uint32_t c;

        memcpy(&c, p, 4);
        v[0] = ((c >> 16) & 0xFF) / 255.0f;
        v[1] = ((c >> 8) & 0xFF) / 255.0f;
        v[2] = (c & 0xFF) / 255.0f;
        v[3] = (c >> 24) / 255.0f;
    } else if (in->type == 0x16) {                               /* three signed 11, 11, 10-bit parts, normalized */
        uint32_t w;

        memcpy(&w, p, 4);
        v[0] = (float)((int32_t)(w << 21) >> 21) / 1023.0f;
        v[1] = (float)((int32_t)(w << 10) >> 21) / 1023.0f;
        v[2] = (float)((int32_t)w >> 22) / 511.0f;
    } else if (kind == 0x1 || kind == 0x5) {                     /* normalized or plain shorts */
        for (unsigned i = 0; i < count && i < 4; i++) {
            int16_t s;

            memcpy(&s, p + 2 * i, 2);
            v[i] = kind == 0x1 ? s / 32767.0f : (float)s;
        }
    } else if (kind == 0x4) {                                    /* bytes, 0-255 as 0-1 */
        for (unsigned i = 0; i < count && i < 4; i++)
            v[i] = p[i] / 255.0f;
    } else {
        return 0;
    }
    return 1;
}

static void write_masked(float dst[4], const float value[4], unsigned mask)
{
    for (unsigned i = 0; i < 4; i++)
        if (mask & (8u >> i))
            dst[i] = value[i];
}

static float clamped_reciprocal(float x)
{
    float r = 1.0f / x;

    if (r > 0.0f)
        return r < 5.42101e-20f ? 5.42101e-20f : (r > 1.884467e+19f ? 1.884467e+19f : r);
    return r > -5.42101e-20f ? -5.42101e-20f : (r < -1.884467e+19f ? -1.884467e+19f : r);
}

/* One instruction with its fields taken out of the words once, at CreateVertexShader, so the vertex loop does no bit
 * work. The meanings are the ones the layout above gives. */
enum operand_source { FROM_TEMP0, FROM_TEMP, FROM_POSITION, FROM_INPUT, FROM_CONSTANT };

struct fh_vsh_op {
    uint8_t mac, ilu;                   /* the two units' operations */
    uint8_t source[3];                  /* A, B, C: an operand_source */
    uint8_t temp[3];                    /* the temp each reads (FROM_TEMP) */
    uint8_t pick[3][4];                 /* each operand's swizzle as four component numbers, x first */
    uint8_t negate[3];
    uint8_t input;                      /* the v register (FROM_INPUT) */
    uint8_t constant;                   /* the constant index, plus a0.x when relative is set */
    uint8_t relative;
    uint8_t mac_temp, mac_mask;         /* where the MAC's result goes among the temps, and its mask */
    uint8_t mac_to_position;            /* the MAC writes r12, which is oPos */
    uint8_t ilu_temp, ilu_mask;         /* where the ILU's goes (r1 when the MAC writes a temp too) */
    uint8_t out_mask, out_is_o, out_from_ilu, out_index;
    uint8_t last;
    /* for fh_vsh_run_many: which components (mask bits as above, x = 8) of each operand and of each unit's result
     * are used, so that the others are not computed */
    uint8_t rows_a, rows_b, rows_c, rows_mac, rows_ilu;
};

/* The components of the operands each operation reads, for the components of its result that are used. */
static void operand_rows(struct fh_vsh_op *op)
{
    enum { X = 8, Y = 4, Z = 2, W = 1, XYZ = 14, XYZW = 15 };
    const unsigned mac = op->rows_mac, ilu = op->rows_ilu;

    op->rows_a = op->rows_b = op->rows_c = 0;
    switch (op->mac) {
    case 1: op->rows_a = (uint8_t)mac; break;
    case 2: op->rows_a = op->rows_b = (uint8_t)mac; break;
    case 3: op->rows_a = op->rows_c = (uint8_t)mac; break;
    case 4: op->rows_a = op->rows_b = op->rows_c = (uint8_t)mac; break;
    case 5: if (mac) op->rows_a = op->rows_b = XYZ; break;
    case 6: if (mac) { op->rows_a = XYZ; op->rows_b = XYZW; } break;
    case 7: if (mac) op->rows_a = op->rows_b = XYZW; break;
    case 8:
        op->rows_a = (uint8_t)(((mac & Y) ? Y : 0) | ((mac & Z) ? Z : 0));
        op->rows_b = (uint8_t)(((mac & Y) ? Y : 0) | ((mac & W) ? W : 0));
        break;
    case 9: case 10: case 11: case 12: op->rows_a = op->rows_b = (uint8_t)mac; break;
    case 13: op->rows_a = X; break;  /* arl sets a0 whether or not anything is written */
    default: break;
    }
    switch (op->ilu) {
    case 1: op->rows_c |= (uint8_t)ilu; break;
    case 2: case 3: case 4: case 5: case 6: if (ilu) op->rows_c |= X; break;
    case 7: if (ilu) op->rows_c |= X | Y | W; break;
    default: break;
    }
}

static const struct fh_vsh_op *decode_program(const uint32_t *words, unsigned instructions)
{
    struct fh_vsh_op *ops = calloc(instructions, sizeof *ops);

    if (!ops)
        return NULL;
    for (unsigned i = 0; i < instructions; i++) {
        const uint32_t *w = words + 4 * i;
        struct fh_vsh_op *op = &ops[i];
        const unsigned sources[3] = {bits(w, 2, 26, 2), bits(w, 2, 11, 2), bits(w, 3, 28, 2)};
        const unsigned temps[3] = {bits(w, 2, 28, 4), bits(w, 2, 13, 4),
                                   (bits(w, 2, 0, 2) << 2) | bits(w, 3, 30, 2)};
        const unsigned picks[3] = {bits(w, 1, 0, 8), bits(w, 2, 17, 8), bits(w, 2, 2, 8)};
        const unsigned negates[3] = {bits(w, 1, 8, 1), bits(w, 2, 25, 1), bits(w, 2, 10, 1)};
        const unsigned temp_out = bits(w, 3, 20, 4);

        op->mac = (uint8_t)bits(w, 1, 21, 4);
        op->ilu = (uint8_t)bits(w, 1, 25, 3);
        op->input = (uint8_t)bits(w, 1, 9, 4);
        op->constant = (uint8_t)bits(w, 1, 13, 8);
        op->relative = (uint8_t)bits(w, 3, 1, 1);
        for (unsigned o = 0; o < 3; o++) {
            if (sources[o] == 1)        /* r0-r11; r12 reads oPos; 13-15 (no such temp) read r0 as before */
                op->source[o] = temps[o] == 12 ? FROM_POSITION : (temps[o] < 12 ? FROM_TEMP : FROM_TEMP0);
            else if (sources[o] == 2)
                op->source[o] = FROM_INPUT;
            else if (sources[o] == 3)
                op->source[o] = FROM_CONSTANT;
            else
                op->source[o] = FROM_TEMP0; /* an unused operand */
            op->temp[o] = (uint8_t)temps[o];
            for (unsigned k = 0; k < 4; k++)
                op->pick[o][k] = (uint8_t)((picks[o] >> (6 - 2 * k)) & 3);
            op->negate[o] = (uint8_t)negates[o];
        }
        op->mac_temp = (uint8_t)(temp_out % TEMPS);
        op->mac_mask = (uint8_t)bits(w, 3, 24, 4);
        op->mac_to_position = op->mac && op->mac != 13 && temp_out == 12;
        op->ilu_temp = (uint8_t)(op->mac ? 1 : temp_out % TEMPS);
        op->ilu_mask = (uint8_t)bits(w, 3, 16, 4);
        op->out_mask = (uint8_t)bits(w, 3, 12, 4);
        op->out_is_o = (uint8_t)bits(w, 3, 11, 1);
        op->out_from_ilu = (uint8_t)bits(w, 3, 2, 1);
        op->out_index = (uint8_t)bits(w, 3, 3, 8);
        op->last = (uint8_t)bits(w, 3, 0, 1);
        op->rows_mac = (uint8_t)(((op->mac && op->mac != 13) || op->mac_to_position ? op->mac_mask : 0) |
                                 (op->out_mask && !op->out_from_ilu ? op->out_mask : 0));
        op->rows_ilu = (uint8_t)((op->ilu ? op->ilu_mask : 0) | (op->out_mask && op->out_from_ilu ? op->out_mask : 0));
        operand_rows(op);
    }
    return ops;
}

static float saved_scale[4], saved_offset[4];
static int screen_mapping_set;          /* [acct2-fh] 11-14: draw_begin set c-38 and c-37 (they are reserved) */

void fh_vsh_draw_begin(void)
{
    screen_mapping_set = screen_registers_reserved();
    if (!screen_mapping_set)            /* [acct2-fh] 11-14: no reserved constants: they are the program's own */
        return;
    memcpy(saved_scale, constants[CONSTANT_VIEWPORT_SCALE], 16);
    memcpy(saved_offset, constants[CONSTANT_VIEWPORT_OFFSET], 16);
    constants[CONSTANT_VIEWPORT_SCALE][0] = constants[CONSTANT_VIEWPORT_SCALE][1] = 1.0f;
    constants[CONSTANT_VIEWPORT_SCALE][2] = constants[CONSTANT_VIEWPORT_SCALE][3] = 1.0f;
    memset(constants[CONSTANT_VIEWPORT_OFFSET], 0, 16);
}

void fh_vsh_draw_end(void)
{
    if (!screen_mapping_set)
        return;
    screen_mapping_set = 0;
    memcpy(constants[CONSTANT_VIEWPORT_SCALE], saved_scale, 16);
    memcpy(constants[CONSTANT_VIEWPORT_OFFSET], saved_offset, 16);
}

int fh_vsh_run(const struct fh_vsh *vsh, const uint8_t *vertex, struct fh_vsh_result *out)
{
    float input[FH_VSH_INPUTS][4] = {{0}}, temp[TEMPS][4] = {{0}}, output[OUTPUT_REGISTERS][4] = {{0}};
    int a0 = 0;

    if (!vsh->ops)
        return 0;
    for (unsigned r = 0; r < FH_VSH_INPUTS; r++)
        if (vsh->inputs[r].used && !read_input(&vsh->inputs[r], vertex, input[r]))
            return 0;
    output[OUTPUT_DIFFUSE][0] = output[OUTPUT_DIFFUSE][1] = output[OUTPUT_DIFFUSE][2] = 1.0f;
    output[OUTPUT_DIFFUSE][3] = output[OUTPUT_POSITION][3] = output[OUTPUT_TEXCOORD0][3] = 1.0f;

    for (unsigned i = 0; i < vsh->instructions; i++) {
        const struct fh_vsh_op *op = &vsh->ops[i];
        const int index = (int)op->constant + (op->relative ? a0 : 0);
        float operand[3][4], mac_value[4] = {0}, ilu_value[4] = {0};

        /* the three operands, read before anything is written */
        for (unsigned o = 0; o < 3; o++) {
            const float *src;

            switch (op->source[o]) {
            case FROM_TEMP: src = temp[op->temp[o]]; break;
            case FROM_POSITION: src = output[OUTPUT_POSITION]; break;
            case FROM_INPUT: src = input[op->input]; break;
            case FROM_CONSTANT:
                src = index >= 0 && index < FH_VSH_CONSTANT_REGISTERS ? constants[index] : temp[0];
                break;
            default: src = temp[0]; break;  /* an unused operand, or a constant index out of range */
            }
            for (unsigned k = 0; k < 4; k++)
                operand[o][k] = op->negate[o] ? -src[op->pick[o][k]] : src[op->pick[o][k]];
        }
        const float *a = operand[0], *b = operand[1], *c = operand[2];

        switch (op->mac) {
        case 0: break;
        case 1: memcpy(mac_value, a, 16); break;
        case 2: for (int k = 0; k < 4; k++) mac_value[k] = a[k] * b[k]; break;
        case 3: for (int k = 0; k < 4; k++) mac_value[k] = a[k] + c[k]; break;
        case 4: for (int k = 0; k < 4; k++) mac_value[k] = a[k] * b[k] + c[k]; break;
        case 5: mac_value[0] = mac_value[1] = mac_value[2] = mac_value[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            break;
        case 6: mac_value[0] = mac_value[1] = mac_value[2] = mac_value[3] =
                    a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
            break;
        case 7: mac_value[0] = mac_value[1] = mac_value[2] = mac_value[3] =
                    a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            break;
        case 8: mac_value[0] = 1.0f; mac_value[1] = a[1] * b[1]; mac_value[2] = a[2]; mac_value[3] = b[3]; break;
        case 9: for (int k = 0; k < 4; k++) mac_value[k] = a[k] < b[k] ? a[k] : b[k]; break;
        case 10: for (int k = 0; k < 4; k++) mac_value[k] = a[k] > b[k] ? a[k] : b[k]; break;
        case 11: for (int k = 0; k < 4; k++) mac_value[k] = a[k] < b[k] ? 1.0f : 0.0f; break;
        case 12: for (int k = 0; k < 4; k++) mac_value[k] = a[k] >= b[k] ? 1.0f : 0.0f; break;
        case 13: a0 = (int)floorf(a[0] + 1e-3f); break;   /* [fh10] a little over, as program.vert: 3 x 0.99999 means 3 */
        default: return 0;                  /* 14, 15: no such operation */
        }
        switch (op->ilu) {
        case 0: break;
        case 1: memcpy(ilu_value, c, 16); break;
        case 2: ilu_value[0] = ilu_value[1] = ilu_value[2] = ilu_value[3] = 1.0f / c[0]; break;
        case 3: ilu_value[0] = ilu_value[1] = ilu_value[2] = ilu_value[3] = clamped_reciprocal(c[0]); break;
        case 4: ilu_value[0] = ilu_value[1] = ilu_value[2] = ilu_value[3] = 1.0f / sqrtf(fabsf(c[0])); break;
        case 5: {
            float f = floorf(c[0]);

            ilu_value[0] = exp2f(f);
            ilu_value[1] = c[0] - f;
            ilu_value[2] = exp2f(c[0]);
            ilu_value[3] = 1.0f;
            break;
        }
        case 6: {
            float m = fabsf(c[0]), e = m > 0.0f ? floorf(log2f(m)) : -INFINITY;

            ilu_value[0] = e;
            ilu_value[1] = m > 0.0f ? m / exp2f(e) : 1.0f;
            ilu_value[2] = m > 0.0f ? log2f(m) : -INFINITY;
            ilu_value[3] = 1.0f;
            break;
        }
        default: {                          /* 7: lit */
            float diffuse = c[0] > 0.0f ? c[0] : 0.0f, power = c[3] < -127.9961f ? -127.9961f :
                                                                (c[3] > 127.9961f ? 127.9961f : c[3]);

            ilu_value[0] = ilu_value[3] = 1.0f;
            ilu_value[1] = diffuse;
            ilu_value[2] = c[0] > 0.0f && c[1] > 0.0f ? powf(c[1], power) : 0.0f;
            break;
        }
        }

        /* the writes: temps, then the o register or constant the instruction names */
        if (op->mac && op->mac != 13)
            write_masked(temp[op->mac_temp], mac_value, op->mac_mask);
        if (op->ilu)
            write_masked(temp[op->ilu_temp], ilu_value, op->ilu_mask);
        if (op->out_mask) {
            const float *value = op->out_from_ilu ? ilu_value : mac_value;

            if (op->out_is_o) {
                if (op->out_index < OUTPUT_REGISTERS)
                    write_masked(output[op->out_index], value, op->out_mask);
            } else if (op->out_index < FH_VSH_CONSTANT_REGISTERS) {
                write_masked(constants[op->out_index], value, op->out_mask);
            }
        }
        /* a write to temp 12 is a write to oPos */
        if (op->mac_to_position)
            write_masked(output[OUTPUT_POSITION], mac_value, op->mac_mask);
        if (op->last)
            break;
    }

    memcpy(out->position, output[OUTPUT_POSITION], 16);
    memcpy(out->diffuse, output[OUTPUT_DIFFUSE], 16);
    memcpy(out->texcoord0, output[OUTPUT_TEXCOORD0], 16);
    out->fog = output[OUTPUT_FOG][0];
    return 1;
}

int fh_vsh_gpu_ready(const struct fh_vsh *vsh)
{
    if (!vsh->ops || !vsh->many_at_once || !vsh->instructions || vsh->instructions > FH_VSH_GPU_INSTRUCTIONS)
        return 0;
    for (unsigned i = 0; i < vsh->instructions; i++) {
        if (vsh->ops[i].mac > 13)
            return 0;               /* fh_vsh_run leaves such a draw out */
        if (vsh->ops[i].last)
            break;
    }
    return 1;
}

void fh_vsh_gpu_constants(float out[FH_VSH_CONSTANT_REGISTERS][4])
{
    memcpy(out, constants, sizeof constants);
    if (!screen_registers_reserved())   /* [acct2-fh] 11-14: as fh_vsh_draw_begin */
        return;
    out[CONSTANT_VIEWPORT_SCALE][0] = out[CONSTANT_VIEWPORT_SCALE][1] = 1.0f;
    out[CONSTANT_VIEWPORT_SCALE][2] = out[CONSTANT_VIEWPORT_SCALE][3] = 1.0f;
    memset(out[CONSTANT_VIEWPORT_OFFSET], 0, 16);
}

unsigned fh_vsh_expanded_registers(const struct fh_vsh *vsh, uint8_t registers[FH_VSH_INPUTS])
{
    unsigned n = 0;

    for (unsigned r = 0; r < FH_VSH_INPUTS; r++) {
        const struct fh_vsh_input *in = &vsh->inputs[r];
        float probe[4];
        static const uint8_t zero[16];

        if (!(vsh->inputs_read & (1u << r)) || !in->used)
            continue;                   /* read but not named: reads 0, as on the CPU */
        if (!read_input(&(struct fh_vsh_input){1, 0, in->type, in->bytes, 0}, zero, probe))
            return 0;                   /* a kind read_input doesn't know */
        registers[n++] = (uint8_t)r;
    }
    return n;
}

int fh_vsh_expand(const struct fh_vsh *vsh, uint32_t data, uint32_t stride, uint32_t count, float *out)
{
    uint8_t registers[FH_VSH_INPUTS];
    const unsigned n = fh_vsh_expanded_registers(vsh, registers);
    const uint8_t *vertex = (const uint8_t *)XBOX_PTR(data);

    if (!n)
        return 0;
    for (uint32_t v = 0; v < count; v++, vertex += stride)
        for (unsigned k = 0; k < n; k++)
            if (!read_input(&vsh->inputs[registers[k]], vertex, out + ((size_t)v * n + k) * 4))
                return 0;
    return 1;
}

/* What fh_vsh_run_many needs to know about a program, from its decoded instructions: which temps and inputs it
 * touches (only those are cleared for each group of vertices) and whether it writes a constant register. */
static void program_facts(struct fh_vsh *vsh)
{
    vsh->temps_touched = 1;             /* r0: an unused operand reads it */
    vsh->inputs_read = 0;
    vsh->many_at_once = vsh->ops != NULL;
    for (unsigned i = 0; vsh->ops && i < vsh->instructions; i++) {
        const struct fh_vsh_op *op = &vsh->ops[i];

        for (unsigned o = 0; o < 3; o++) {
            if (op->source[o] == FROM_TEMP)
                vsh->temps_touched |= (uint16_t)(1u << op->temp[o]);
            else if (op->source[o] == FROM_INPUT)
                vsh->inputs_read |= (uint16_t)(1u << op->input);
        }
        if (op->mac && op->mac != 13)
            vsh->temps_touched |= (uint16_t)(1u << op->mac_temp);
        if (op->ilu)
            vsh->temps_touched |= (uint16_t)(1u << op->ilu_temp);
        if (op->out_mask && !op->out_is_o)
            vsh->many_at_once = 0;
        if (op->last)
            break;
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Many vertices at once (2026-11-13, build12)
 *
 * boot21 measured fh_vsh_run at ~190 ns a vertex: ~10 ms a frame for the ~52,000 clone vertices of the level's busiest
 * frames, most of the port's draw time. Here a group of up to FH_VSH_BATCH vertices goes through each instruction
 * together. Each register is held as four rows, one per component, each row holding that component for every vertex
 * of the group. A swizzle then only picks rows, and every operation is a plain loop over the vertices, which the
 * compiler turns into vector instructions. Each vertex is computed with the same operations in the same order as in
 * fh_vsh_run, so the results are the same to the bit (checked on random programs when this was written). A program
 * that writes a constant register is run one vertex at a time instead (a later vertex reads an earlier one's write).
 * The rows are static: draws come from one guest thread at a time. */

typedef float lanes[FH_VSH_BATCH];

static lanes many_input[FH_VSH_INPUTS][4], many_temp[TEMPS][4], many_output[OUTPUT_REGISTERS][4];
static lanes many_operand[3][4], many_mac[4], many_ilu[4];
static int many_a0[FH_VSH_BATCH];

static void many_write(lanes dst[4], lanes value[4], unsigned mask, unsigned n)
{
    for (unsigned k = 0; k < 4; k++)
        if (mask & (8u >> k))
            memcpy(dst[k], value[k], n * sizeof(float));
}

static void many_fill(float *row, float value, unsigned n)
{
    for (unsigned v = 0; v < n; v++)
        row[v] = value;
}

/* One operand's row k for the group: the source's component c, negated or not. */
static void many_operand_row(const struct fh_vsh_op *op, unsigned o, unsigned k, unsigned n)
{
    float *dst = many_operand[o][k];
    const unsigned c = op->pick[o][k];
    const float *src;

    switch (op->source[o]) {
    case FROM_TEMP: src = many_temp[op->temp[o]][c]; break;
    case FROM_POSITION: src = many_output[OUTPUT_POSITION][c]; break;
    case FROM_INPUT: src = many_input[op->input][c]; break;
    case FROM_CONSTANT:
        if (op->relative) {             /* c[a0.x + n]: each vertex its own register */
            for (unsigned v = 0; v < n; v++) {
                const int index = (int)op->constant + many_a0[v];
                const float x = index >= 0 && index < FH_VSH_CONSTANT_REGISTERS ? constants[index][c]
                                                                                : many_temp[0][c][v];

                dst[v] = op->negate[o] ? -x : x;
            }
            return;
        }
        if (op->constant < FH_VSH_CONSTANT_REGISTERS) {
            const float x = constants[op->constant][c];

            many_fill(dst, op->negate[o] ? -x : x, n);
            return;
        }
        src = many_temp[0][c];          /* a constant index out of range reads r0, as in fh_vsh_run */
        break;
    default: src = many_temp[0][c]; break;
    }
    if (op->negate[o])
        for (unsigned v = 0; v < n; v++)
            dst[v] = -src[v];
    else
        memcpy(dst, src, n * sizeof(float));
}

int fh_vsh_run_many(const struct fh_vsh *vsh, uint32_t data, uint32_t stride, uint32_t count,
                    struct fh_vsh_result *out)
{
    const unsigned n = count < FH_VSH_BATCH ? count : FH_VSH_BATCH;

    if (!vsh->ops)
        return 0;
    if (!vsh->many_at_once || count > FH_VSH_BATCH) {
        for (uint32_t v = 0; v < count; v++)
            if (!fh_vsh_run(vsh, (const uint8_t *)XBOX_PTR(data + v * stride), &out[v]))
                return 0;
        return 1;
    }

    /* the inputs the declaration feeds, from each vertex; one it does not feed reads 0 */
    for (unsigned r = 0; r < FH_VSH_INPUTS; r++) {
        if (vsh->inputs[r].used) {
            for (unsigned v = 0; v < n; v++) {
                float x[4];

                if (!read_input(&vsh->inputs[r], (const uint8_t *)XBOX_PTR(data + v * stride), x))
                    return 0;
                for (unsigned k = 0; k < 4; k++)
                    many_input[r][k][v] = x[k];
            }
        } else if (vsh->inputs_read & (1u << r)) {
            for (unsigned k = 0; k < 4; k++)
                many_fill(many_input[r][k], 0.0f, n);
        }
    }
    for (unsigned r = 0; r < TEMPS; r++)
        if (vsh->temps_touched & (1u << r))
            for (unsigned k = 0; k < 4; k++)
                many_fill(many_temp[r][k], 0.0f, n);
    for (unsigned k = 0; k < 4; k++) {   /* oPos (0, 0, 0, 1), oD0 (1, 1, 1, 1), oT0 (0, 0, 0, 1), oFog 0 */
        many_fill(many_output[OUTPUT_POSITION][k], k == 3 ? 1.0f : 0.0f, n);
        many_fill(many_output[OUTPUT_DIFFUSE][k], 1.0f, n);
        many_fill(many_output[OUTPUT_TEXCOORD0][k], k == 3 ? 1.0f : 0.0f, n);
        many_fill(many_output[OUTPUT_FOG][k], 0.0f, n);
    }
    memset(many_a0, 0, sizeof many_a0);

    for (unsigned i = 0; i < vsh->instructions; i++) {
        const struct fh_vsh_op *op = &vsh->ops[i];
        lanes *a = many_operand[0], *b = many_operand[1], *c = many_operand[2];

        if (op->mac > 13)
            return 0;                   /* 14, 15: no such operation */
        const unsigned rows[3] = {op->rows_a, op->rows_b, op->rows_c};

        for (unsigned o = 0; o < 3; o++)
            for (unsigned k = 0; k < 4; k++)
                if (rows[o] & (8u >> k))
                    many_operand_row(op, o, k, n);

        switch (op->mac) {
        case 0:
        case 13:
            for (unsigned k = 0; k < 4; k++)
                many_fill(many_mac[k], 0.0f, n);
            if (op->mac == 13)
                for (unsigned v = 0; v < n; v++)
                    many_a0[v] = (int)floorf(a[0][v] + 1e-3f);
            break;
        case 1:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    memcpy(many_mac[k], a[k], n * sizeof(float));
            break;
        case 2:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    for (unsigned v = 0; v < n; v++)
                        many_mac[k][v] = a[k][v] * b[k][v];
            break;
        case 3:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    for (unsigned v = 0; v < n; v++)
                        many_mac[k][v] = a[k][v] + c[k][v];
            break;
        case 4:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    for (unsigned v = 0; v < n; v++)
                        many_mac[k][v] = a[k][v] * b[k][v] + c[k][v];
            break;
        case 5:
            for (unsigned v = 0; v < (op->rows_mac ? n : 0); v++)
                many_mac[0][v] = a[0][v] * b[0][v] + a[1][v] * b[1][v] + a[2][v] * b[2][v];
            goto spread;
        case 6:
            for (unsigned v = 0; v < (op->rows_mac ? n : 0); v++)
                many_mac[0][v] = a[0][v] * b[0][v] + a[1][v] * b[1][v] + a[2][v] * b[2][v] + b[3][v];
            goto spread;
        case 7:
            for (unsigned v = 0; v < (op->rows_mac ? n : 0); v++)
                many_mac[0][v] = a[0][v] * b[0][v] + a[1][v] * b[1][v] + a[2][v] * b[2][v] + a[3][v] * b[3][v];
        spread:                         /* the dot products' one value in all four components */
            for (unsigned k = 1; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    memcpy(many_mac[k], many_mac[0], n * sizeof(float));
            break;
        case 8:
            for (unsigned v = 0; v < (op->rows_mac ? n : 0); v++) {
                many_mac[0][v] = 1.0f;
                many_mac[1][v] = a[1][v] * b[1][v];
                many_mac[2][v] = a[2][v];
                many_mac[3][v] = b[3][v];
            }
            break;
        case 9:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    for (unsigned v = 0; v < n; v++)
                        many_mac[k][v] = a[k][v] < b[k][v] ? a[k][v] : b[k][v];
            break;
        case 10:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    for (unsigned v = 0; v < n; v++)
                        many_mac[k][v] = a[k][v] > b[k][v] ? a[k][v] : b[k][v];
            break;
        case 11:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    for (unsigned v = 0; v < n; v++)
                        many_mac[k][v] = a[k][v] < b[k][v] ? 1.0f : 0.0f;
            break;
        default:                        /* 12: sge */
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_mac & (8u >> k))
                    for (unsigned v = 0; v < n; v++)
                        many_mac[k][v] = a[k][v] >= b[k][v] ? 1.0f : 0.0f;
            break;
        }

        switch (op->ilu) {
        case 0:
            for (unsigned k = 0; k < 4; k++)
                many_fill(many_ilu[k], 0.0f, n);
            break;
        case 1:
            for (unsigned k = 0; k < 4; k++)
                if (op->rows_ilu & (8u >> k))
                    memcpy(many_ilu[k], c[k], n * sizeof(float));
            break;
        case 2:
            for (unsigned v = 0; v < (op->rows_ilu ? n : 0); v++)
                many_ilu[0][v] = many_ilu[1][v] = many_ilu[2][v] = many_ilu[3][v] = 1.0f / c[0][v];
            break;
        case 3:
            for (unsigned v = 0; v < (op->rows_ilu ? n : 0); v++)
                many_ilu[0][v] = many_ilu[1][v] = many_ilu[2][v] = many_ilu[3][v] = clamped_reciprocal(c[0][v]);
            break;
        case 4:
            for (unsigned v = 0; v < (op->rows_ilu ? n : 0); v++)
                many_ilu[0][v] = many_ilu[1][v] = many_ilu[2][v] = many_ilu[3][v] = 1.0f / sqrtf(fabsf(c[0][v]));
            break;
        case 5:
            for (unsigned v = 0; v < (op->rows_ilu ? n : 0); v++) {
                float f = floorf(c[0][v]);

                many_ilu[0][v] = exp2f(f);
                many_ilu[1][v] = c[0][v] - f;
                many_ilu[2][v] = exp2f(c[0][v]);
                many_ilu[3][v] = 1.0f;
            }
            break;
        case 6:
            for (unsigned v = 0; v < (op->rows_ilu ? n : 0); v++) {
                float m = fabsf(c[0][v]), e = m > 0.0f ? floorf(log2f(m)) : -INFINITY;

                many_ilu[0][v] = e;
                many_ilu[1][v] = m > 0.0f ? m / exp2f(e) : 1.0f;
                many_ilu[2][v] = m > 0.0f ? log2f(m) : -INFINITY;
                many_ilu[3][v] = 1.0f;
            }
            break;
        default:                        /* 7: lit */
            for (unsigned v = 0; v < (op->rows_ilu ? n : 0); v++) {
                float diffuse = c[0][v] > 0.0f ? c[0][v] : 0.0f,
                      power = c[3][v] < -127.9961f ? -127.9961f : (c[3][v] > 127.9961f ? 127.9961f : c[3][v]);

                many_ilu[0][v] = many_ilu[3][v] = 1.0f;
                many_ilu[1][v] = diffuse;
                many_ilu[2][v] = c[0][v] > 0.0f && c[1][v] > 0.0f ? powf(c[1][v], power) : 0.0f;
            }
            break;
        }

        /* the writes, in fh_vsh_run's order */
        if (op->mac && op->mac != 13)
            many_write(many_temp[op->mac_temp], many_mac, op->mac_mask, n);
        if (op->ilu)
            many_write(many_temp[op->ilu_temp], many_ilu, op->ilu_mask, n);
        if (op->out_mask && op->out_is_o && op->out_index < OUTPUT_REGISTERS)
            many_write(many_output[op->out_index], op->out_from_ilu ? many_ilu : many_mac, op->out_mask, n);
        if (op->mac_to_position)
            many_write(many_output[OUTPUT_POSITION], many_mac, op->mac_mask, n);
        if (op->last)
            break;
    }

    for (unsigned v = 0; v < n; v++) {
        for (unsigned k = 0; k < 4; k++) {
            out[v].position[k] = many_output[OUTPUT_POSITION][k][v];
            out[v].diffuse[k] = many_output[OUTPUT_DIFFUSE][k][v];
            out[v].texcoord0[k] = many_output[OUTPUT_TEXCOORD0][k][v];
        }
        out[v].fog = many_output[OUTPUT_FOG][0][v];
    }
    return 1;
}
