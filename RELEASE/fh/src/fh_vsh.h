/* [acct2-fh] Finest Hour port: the game's vertex shaders as Direct3D creates them. See fh_vsh.c. */
#pragma once
#include <stdint.h>

#define FH_VSH_INPUTS           16      /* the vertex registers v0-v15 */
#define FH_VSH_STREAMS          16

/* One input register as the declaration feeds it. */
struct fh_vsh_input {
    uint8_t used;                       /* the declaration names this register */
    uint8_t stream;                     /* the vertex stream it is read from */
    uint8_t type;                       /* the declaration's data type (D3DVSDT_*: count << 4 | kind) */
    uint8_t bytes;                      /* its size in the vertex */
    uint16_t offset;                    /* its offset in its stream's vertex */
};

struct fh_vsh_op;                       /* one decoded instruction (fh_vsh.c) */

/* A vertex shader: what CreateVertexShader was given, kept for the draws. */
struct fh_vsh {
    uint32_t handle;                    /* what the game got back: a guest address with bit 0 set */
    struct fh_vsh_input inputs[FH_VSH_INPUTS];
    uint16_t stream_bytes[FH_VSH_STREAMS];  /* a vertex's size in each stream, from the declaration */
    uint16_t program_kind;              /* the function's first 16-bit word (0x2078 "x ": a vertex program) */
    uint16_t instructions;              /* its second word: how many 128-bit instructions follow */
    const uint32_t *program;            /* the instructions, 4 words each (a copy), or NULL without a function */
    const struct fh_vsh_op *ops;        /* the same instructions decoded once into plain fields (fh_vsh.c), or
                                         * NULL; fh_vsh_run reads these */
    uint16_t temps_touched;             /* the temps the program reads or writes (bit n: rn; r0 always) */
    uint16_t inputs_read;               /* the v registers it reads (bit n: vn) */
    uint8_t many_at_once;               /* no instruction writes a constant register, so fh_vsh_run_many may run
                                         * vertices together */
    uint32_t declaration_problems;      /* tokens this file does not read yet (counted, not guessed) */
    uint8_t fixed_function;             /* made without a function: Direct3D's fixed pipeline then reads the
                                         * declaration's registers by meaning (v0 the position, v3 the diffuse
                                         * color, v9 the first texture coordinates) */
    /* [acct2-fh] 11-13, a selected program (fh_vsh_selected): a declaration with the program memory's program */
    uint8_t selected;                   /* made by fh_vsh_selected, not by CreateVertexShader */
    uint8_t merged;                     /* its inputs come from more than one stream: the draw puts each vertex's
                                         * streams side by side, and inputs[] are offsets in that merged vertex
                                         * (stream 0, stream_bytes[0] bytes) */
    uint16_t merged_streams;            /* the streams it reads (bit n: stream n) */
    uint16_t merged_at[FH_VSH_STREAMS];   /* where each stream's bytes start in the merged vertex */
    uint16_t source_bytes[FH_VSH_STREAMS];/* each stream's vertex size, as the declaration gives it */
};

/* 0x002212F0 D3DDevice_CreateVertexShader(pDeclaration, pFunction, pHandle, Usage): keeps the declaration's inputs
 * and the program, writes a handle to *pHandle and returns 0 (D3D_OK). */
uint32_t fh_d3d_create_vertex_shader(uint32_t declaration, uint32_t function, uint32_t handle_out, uint32_t usage);

/* The shader a handle names, or NULL (an FVF code, 0, or a handle this file did not make). */
const struct fh_vsh *fh_vsh_find(uint32_t handle);

/* [acct2-fh] 11-13. The NV2A's program memory, 136 instructions, as the game loads it:
 * 0x002217F0 D3DDevice_LoadVertexShaderProgram(pFunction, Address) copies the function's instructions (its second
 * 16-bit word says how many; they follow from pFunction + 4) to Address on (NV2A 0x1E9C = Address, then 0x0B00 with
 * the words; the real code also keeps a copy in the device at +0x10A8, which this file doesn't: nothing read it).
 * RenderWare's skinned pipeline loads its program this way (0x001EC33D, at address 12) before each skinned draw.
 * Returns 0 (D3D_OK). */
uint32_t fh_d3d_load_vertex_shader_program(uint32_t function, uint32_t address);

/* A shader for a draw after D3DDevice_SelectVertexShader(Handle, Address) (fh_d3d_state.c): Handle's declaration with
 * the program in the program memory from Address to its last instruction (the one with word 3's bit 0 set). Kept
 * (32 at most) and found again by the declaration, Address and the program's words. A declaration that reads more
 * than one stream comes back merged (see the struct). NULL when Handle names no declaration of CreateVertexShader's or Address holds no program. */
const struct fh_vsh *fh_vsh_selected(uint32_t handle, uint32_t address);

/* A one-line description of a shader's inputs and program, for the census and the log. */
void fh_vsh_describe(const struct fh_vsh *vsh, char *text, unsigned size);

/* ------------------------------------------------------------------------------------------------------------------
 * Running a program on the CPU (fh_vsh.c, second part) */

#define FH_VSH_CONSTANT_REGISTERS 192   /* the NV2A's constants 0-191: Direct3D's c-96..c95 (c0 is 96) */

/* 0x00221450 (one register) and 0x00221500 (four), Direct3D's inline SetVertexShaderConstant: the first register's
 * NV2A number in ecx (Direct3D's c0 is 96), four floats a register at edx. Keeps them for the programs, and in the
 * game's own copy at 0x0022F900 + 16 x register as the real functions do (GetVertexShaderConstant, 0x002216D0,
 * reads that copy; the game calls it from 0x00183690). Returns 0. */
uint32_t fh_d3d_set_vertex_shader_constants(uint32_t first_register, uint32_t data, uint32_t count);

/* 0x002214B0 SetVertexShaderConstant1Fast (words 4) and 0x002215B0 SetVertexShaderConstantNotInlineFast (words from
 * its one stack argument): the first register in ecx, the data at edx, a count of 32-bit words. Their own code only
 * writes the push buffer (NV2A 0x1EA4, then 0x0B80 in pieces of at most 16 words), so only the programs' copy is
 * kept. Returns 0. */
uint32_t fh_d3d_load_vertex_shader_constant_words(uint32_t first_register, uint32_t data, uint32_t words);

/* 0x00221690 SetVertexShaderConstantNotInline: the same, and first the game's own copy, unless the device's flags
 * (byte 0x002309C8) have 0x10 set, as its code does. Returns 0. */
uint32_t fh_d3d_set_vertex_shader_constant_words(uint32_t first_register, uint32_t data, uint32_t words);

/* [acct2-fh] 11-14: 0x00221900 SetShaderConstantMode(Mode) and 0x00221030 GetShaderConstantMode(pMode), as their own
 * code does (fh_vsh.c says what): the mode decides whether c-38 and c-37 are the screen mapping fh_vsh_draw_begin and
 * fh_vsh_gpu_constants set for the port (Mode without 0x10) or the program's own registers (with 0x10: RenderWare's
 * skinned meshes, whose bones fill them). Both return 0. */
uint32_t fh_d3d_set_shader_constant_mode(uint32_t mode);
uint32_t fh_d3d_get_shader_constant_mode(uint32_t mode_out);

/* [acct2-fh] 11-13: one 32-bit word of a constant register (word 0-3: x, y, z, w), as the NV2A's 0x0B80 method loads
 * it; for the push buffer's constant loads (fh_push.c). A register past 191 is dropped. */
void fh_vsh_constant_word(uint32_t reg, unsigned word, uint32_t bits);

/* What a program wrote for one vertex. */
struct fh_vsh_result {
    float position[4];                  /* oPos, as the port reads it: x/w, y/w, z/w and w (fh_vsh.c says why) */
    float diffuse[4];                   /* oD0: red, green, blue, alpha */
    float texcoord0[4];                 /* oT0 */
    float fog;                          /* [acct2-fh] 11-14: oFog's x (0 when the program doesn't write it): with a
                                         * fog table the distance it is looked up by, without one the fog factor
                                         * itself (fh_draw.c, fixed.frag) */
};

/* A draw's vertices are run between these two. begin sets the viewport registers c-38 (scale) to 1 and c-37
 * (offset) to 0, so a program's own screen mapping leaves oPos in clip space for Vulkan; end puts the game's values
 * back. Once a draw, not once a vertex. [acct2-fh] 11-14: only while the constant mode reserves them (not with
 * SetShaderConstantMode's 0x10, when they hold the skinned meshes' bones). */
void fh_vsh_draw_begin(void);
void fh_vsh_draw_end(void);

/* Runs a shader's program on one vertex of stream 0 (its declaration says where each input is), between
 * fh_vsh_draw_begin and fh_vsh_draw_end. Returns 0 when it cannot (no program, an input kind or an instruction this
 * file does not read); then the draw is left out. */
int fh_vsh_run(const struct fh_vsh *vsh, const uint8_t *vertex, struct fh_vsh_result *out);

#define FH_VSH_BATCH 64                 /* the most vertices fh_vsh_run_many takes in one call */

/* The same for count (at most FH_VSH_BATCH) vertices of stream 0 at guest address data, stride bytes apart, results
 * in out[0..count-1]; also between fh_vsh_draw_begin and fh_vsh_draw_end. The results are fh_vsh_run's to the bit;
 * it is several times faster because each instruction is decoded once for all the vertices. */
int fh_vsh_run_many(const struct fh_vsh *vsh, uint32_t data, uint32_t stride, uint32_t count,
                    struct fh_vsh_result *out);

/* [acct2-fh] For the programs run on the GPU (fh_draw.c, shaders/program.vert). */
#define FH_VSH_GPU_INSTRUCTIONS 136     /* all of the NV2A's program memory */

/* Whether a shader's program can run on the GPU: it has one, it writes no constant register (a vertex would then
 * read an earlier vertex's write, as on the CPU only fh_vsh_run does), and every operation is one fh_vsh_run knows. */
int fh_vsh_gpu_ready(const struct fh_vsh *vsh);

/* The 192 constant registers as a program reads them during a draw: c-38 = (1, 1, 1, 1) and c-37 = (0, 0, 0, 0), as
 * fh_vsh_draw_begin sets them for the CPU (only while the constant mode reserves them). */
void fh_vsh_gpu_constants(float out[FH_VSH_CONSTANT_REGISTERS][4]);

/* [acct2-fh] 11-13. For the GPU, a draw's inputs as plain floats: the registers the program reads and the declaration
 * names, in register order (their numbers in registers[]; returns how many, 0 when a kind can't be read). Then
 * fh_vsh_expand writes count vertices from guest address data, stride bytes apart, as that many four-float registers
 * each, the values fh_vsh_run reads (a register past four components is its first four). So the formats Vulkan has no
 * vertex format for (packed3; three 8- or 16-bit parts, as the skinned meshes' bone indices and weights) and odd
 * offsets reach shaders/program.vert all the same. Returns 0 when a vertex can't be read. */
unsigned fh_vsh_expanded_registers(const struct fh_vsh *vsh, uint8_t registers[FH_VSH_INPUTS]);
int fh_vsh_expand(const struct fh_vsh *vsh, uint32_t data, uint32_t stride, uint32_t count, float *out);
