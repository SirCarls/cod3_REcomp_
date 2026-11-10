/* [acct2-bro] Big Red One port: the nglInit and nglBeginFrame stand-ins: NGL's managers and shader caches; the frame
 * arena, the frame's light set and the root scene (pushed by the game's own nglPushScene). See ngl_scene.c. */
#pragma once
#include <stdint.h>

/* What nglInit does that NGL's own code needs: NGL's mesh managers and shader caches, and the material classes
 * registered (the game's nglInitMeshManagers and nglShaderList_InitAll). Returns 0. */
uint32_t ngl_scene_init(void);

/* What nglBeginFrame does: starts the frame arena over and pushes this frame's root scene. Returns 0. */
uint32_t ngl_scene_begin_frame(void);

/* Calls the game's recompiled function at address (cdecl, at most one argument) from native code, as its own code
 * would. The function's return value is left in g_eax. */
void ngl_call_game(uint32_t address, int has_argument, uint32_t argument);

/* The same with count arguments (cdecl: arguments[0] ends up at [esp + 4]). */
void ngl_call_game_args(uint32_t address, int count, const uint32_t *arguments);

/* This frame's root scene, as ngl_scene_begin_frame pushed it (the others are its children); 0 before the first
 * frame. */
uint32_t ngl_scene_root(void);
