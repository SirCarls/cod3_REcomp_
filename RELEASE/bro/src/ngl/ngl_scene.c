/* [acct2-bro] Big Red One port: the parts of nglInit (0x00064BC0) and nglBeginFrame (0x00064690) the port still
 * needs, natively, around NGL's own code.
 *
 * nglInit makes the Direct3D device and NGL's GPU state, which this port replaces, and on the way sets up two things
 * the rest of NGL's own code needs (ngl_scene_init calls the game's functions for them):
 *   - nglInitMeshManagers (0x00067880): NGL's mesh-file, mesh and section-3 managers and its two shader caches
 *     (skip lists at 0x00902AC0 and 0x00902AD4, each with a head node of 0xFFFFFFFF keys as the end marker). Without
 *     them nglParseMesh's HashStrMap_Find (0x00093F10) walked from a null head forever: runs 55-57 hung loading the
 *     Prologue's first mesh file (found with BRO_WHERE).
 *   - nglShaderList_InitAll (0x00093BE0): calls each material class's register function, which nglInitShaders
 *     (0x00045790, run by Sys_Main before nglInit) listed; each puts its class in the shader caches, where
 *     nglParseMesh looks a mesh's materials up by name.
 *
 * NGL's scenes are the game's own code again. nglPushScene (0x00062FC0) carves a scene from the frame arena, links
 * it under the current one and sets its defaults (nglInitScene, nglSetDefaultSceneParams: a 640x480 viewport, a
 * 50-degree perspective from 0.1 to 10000, an identity camera); nglPopScene (0x0005E730) goes back to the parent;
 * nglSetCameraMatrix, nglSetPerspective, nglUpdateScene, nglGetMatrix, nglSphereInFrustum and the rest keep each
 * scene's matrices and frustum, which the game reads back (83 nglGetMatrix calls a frame in the Prologue) and which
 * the 3D drawing needs. Until run 56 they were stand-ins, so every matrix the game asked for was garbage and
 * nglSphereInFrustum said everything was off screen.
 *
 * nglBeginFrame stays native for the frame arena. The game allocates from it itself: R_AddDObjSurfaces (0x00244020)
 * and 14 more of its functions (models and the renderer) take per-frame records by bumping ngl_frameArenaPtr
 * (0x009025A8) directly, with no end check, and so do nglPushScene and nglCreateLightSet. nglSetFrameMemorySize
 * (0x000778B0, a stand-in: it also makes Direct3D vertex and index buffers) would allocate the arena and keep its
 * base in 0x009025A4 and its size in 0x009025AC; nglBeginFrame puts the pointer back at the base every frame.
 * Without that the pointer started at 0 and climbed through guest memory, frame after frame, into the game's own
 * code: at about 90 s in the Prologue it reached IN_PadConsumeReleased's jump table (0x0005652C) and the game
 * crashed (runs 47-50).
 *
 * Then, as the original does, the frame's light set and the root scene. The light set is the game's own
 * nglCreateLightSet (0x0006F7C0): 0x70 bytes from the frame arena, eight light lists that each start out pointing at
 * themselves, kept at 0x00902E40 (every scene's defaults take it, nglSetDefaultSceneParams) and made current at
 * 0x00902E44 (where nglLightSetAddLight adds lights). Until run 60 it was a zeroed block: R_AddDObjSurfaces walks a
 * scene's light list until it comes back to the head, so from a zeroed head it followed a null pointer into guest
 * address 0x18 and crashed in every run (run 59). Then ngl_curScene (0x0090259C) is cleared and nglPushScene(0)
 * called. The menus draw only while a scene is current (0x003153D0 and 0x00315440, from SCR_DrawScreenField
 * 0x002F1D90, check it first). */
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc: guest memory */
#include "ngl_scene.h"

#define NGL_CUR_SCENE       0x0090259Cu
#define NGL_PUSH_SCENE      0x00062FC0u     /* nglPushScene, the game's own */
#define NGL_INIT_MESH_MANAGERS  0x00067880u /* nglInitMeshManagers */
#define NGL_SHADER_LIST_INIT    0x00093BE0u /* nglShaderList_InitAll */

#define NGL_FRAME_ARENA_BASE 0x009025A4u
#define NGL_FRAME_ARENA_PTR  0x009025A8u   /* ngl_frameArenaPtr */
#define NGL_FRAME_ARENA_SIZE 0x009025ACu
#define FRAME_ARENA_BYTES    0x100000u     /* the game asks for 0xC0000 */

#define NGL_FRAME_LIGHT_SET  0x00902E40u   /* the frame's light set */
#define NGL_CURRENT_LIGHT_SET 0x00902E44u  /* the light set nglLightSetAddLight adds to */
#define NGL_CREATE_LIGHT_SET 0x0006F7C0u   /* nglCreateLightSet, the game's own */

static uint32_t root_scene;            /* this frame's root scene (nglPushScene(0) at the frame's start) */

/* What nglBeginFrame does with the frame arena: make it the first time, then start it over. */
static void begin_frame_arena(void)
{
    static uint32_t peak;
    static int warned;
    uint32_t base = MEM32(NGL_FRAME_ARENA_BASE), size = MEM32(NGL_FRAME_ARENA_SIZE);

    if (!base) {
        if (size < FRAME_ARENA_BYTES)
            size = FRAME_ARENA_BYTES;
        base = xbox_HeapAlloc(size, 16);
        if (!base) {
            fprintf(stderr, "[ngl] no guest memory for the frame arena\n");
            return;
        }
        memset((void *)XBOX_PTR(base), 0, size);
        fprintf(stderr, "[ngl] frame arena: %u KB at 0x%08X (the pointer was 0x%08X)\n", size / 1024, base,
                MEM32(NGL_FRAME_ARENA_PTR));
        MEM32(NGL_FRAME_ARENA_BASE) = base;
        MEM32(NGL_FRAME_ARENA_SIZE) = size;
    } else {
        uint32_t taken = MEM32(NGL_FRAME_ARENA_PTR) - base;     /* by the frame that just ended */

        if (taken > peak) {
            peak = taken;
            if (taken > size && !warned) {
                warned = 1;
                fprintf(stderr, "[ngl] a frame took %u bytes of the %u-byte frame arena\n", taken, size);
            }
        }
    }
    MEM32(NGL_FRAME_ARENA_PTR) = base;
}

/* Calls the game's function at address (cdecl), as its own code would: the arguments pushed last to first and a
 * return address on the guest stack; the callee's ret pops the return address and the caller pops the arguments. */
void ngl_call_game_args(uint32_t address, int count, const uint32_t *arguments)
{
    recomp_func_t fn = recomp_lookup(address);

    if (!fn) {
        fprintf(stderr, "[ngl] 0x%08X has no recompiled code\n", address);
        return;
    }
    for (int i = count - 1; i >= 0; i--) {
        g_esp -= 4u;
        MEM32(g_esp) = arguments[i];
    }
    g_esp -= 4u;
    MEM32(g_esp) = address;             /* the return address; only the callee's ret reads it */
    fn();
    g_esp += 4u * (uint32_t)count;
}

void ngl_call_game(uint32_t address, int has_argument, uint32_t argument)
{
    ngl_call_game_args(address, has_argument ? 1 : 0, &argument);
}

uint32_t ngl_scene_init(void)
{
    ngl_call_game(NGL_INIT_MESH_MANAGERS, 0, 0);
    ngl_call_game(NGL_SHADER_LIST_INIT, 0, 0);
    fprintf(stderr, "[ngl] init: NGL's mesh managers and shader caches made, material classes registered\n");
    return 0;
}

uint32_t ngl_scene_begin_frame(void)
{
    begin_frame_arena();
    if (MEM32(NGL_FRAME_ARENA_BASE)) {
        ngl_call_game(NGL_CREATE_LIGHT_SET, 0, 0);
        MEM32(NGL_FRAME_LIGHT_SET) = MEM32(NGL_CURRENT_LIGHT_SET);    /* nglCreateLightSet's, which it made current */
    }
    MEM32(NGL_CUR_SCENE) = 0;
    ngl_call_game(NGL_PUSH_SCENE, 1, 0);    /* nglPushScene(0): the root scene, no settings */
    root_scene = MEM32(NGL_CUR_SCENE);
    return 0;
}

uint32_t ngl_scene_root(void)
{
    return root_scene;
}
