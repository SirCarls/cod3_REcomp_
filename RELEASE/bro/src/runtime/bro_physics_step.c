/* Big Red One: the game's physics in fixed steps, whatever the frame rate.
 *
 * [waw-decomp/bro1] 2026-11-23, for the owner's frame-rate work (acct2-fh's BRO-FRAMERATE-AUDIT.md, finding B1; Call of
 * Duty 3's game/physics_step.cpp is the same fix for its sibling engine).
 *
 * The game runs its whole update once a drawn frame with that frame's time (G_RunFrame 0x00274660, called once from
 * SV_RunGameFrame). G_RunFrame hands the frame's milliseconds x 0.001 to G_VehiclePhysicsFrame (0x00233BD0, its only
 * caller), which does, in this order:
 *   - the frame's seconds times the float at 0x008FF028 (1.0 in the image);
 *   - Ragdoll_UpdateAllFromEntities (0x00230390, seconds): each ragdoll owner's animated pose kept for its ragdoll;
 *   - Phys_RunFrame (0x000B4170, seconds): the physics world's step (PhysWorld_Step 0x000BE640);
 *   - PhysObj_UpdateAll (0x00230930): the physics objects (boxes, debris, doors) put where their bodies are;
 *   - Ragdoll_UpdateAll (0x00230350, seconds): each ragdoll owner (the list at 0x008F02E0, count 0x008F0464) through
 *     Ragdoll_Update (0x002301F0): its settle timers, its bodies written into its pose, and the blend from the
 *     animation into that pose (blend timer +seconds, over its blend time) into the entity's bones;
 *   - Vehicle_UpdateFromPhysics (0x0022D230) for each physics vehicle (the list at 0x008FEB70, count 0x008FEB9C).
 * (The names are the CoD2-BRO-Native-Loader's, read in our own disassembly of default.xbe.)
 *
 * So the physics step is as long as the frame: 33 ms on the Xbox (30 frames a second), 7 ms at 144, 2-3 ms uncapped.
 * Call of Duty 3 showed what that does above 60 (its S4: window glass and boxes hit hard enough to kill the player, a
 * tank couldn't push a door open). Here the same function is done natively (bro/config/stand_in_functions.tsv,
 * "native bro_physics_frame"): the frame's time is kept and the four physics calls run in whole steps of
 * 1 / BRO_PHYSICS_HZ seconds (60 by default): none, one or several in a frame, at most 0.1 s worth (Com_ModifyMsec's
 * cap on a frame, 100 ms; time past it is dropped, as the game drops it). As in Call of Duty 3, a step is taken once
 * three quarters of one is due and the difference is carried, so at 60 frames a second every frame gets exactly one
 * step though the game's milliseconds alternate (16, 17, 17).
 *   - A frame with no step (above 60 frames a second) runs Ragdoll_UpdateAll with no time: nothing moves and no timer
 *     runs (Ragdoll_Update only adds the time to them; its one division is by the blend time), but each ragdoll's pose
 *     is put back on the bones the animation set this frame. Without it the picture would alternate between a falling
 *     body and its last animation (Call of Duty 3's "ghost", the owner 2026-10-23).
 *   - The physics objects keep where the last step put them, so above 60 they move in 60 Hz steps on screen.
 *   - The vehicles are put where their bodies are every frame, as the game does once a frame.
 * BRO_PHYSICS_HZ=0: one step a frame with the frame's time, as the game does. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "ngl_log.h"
#include "ngl_scene.h"      /* ngl_call_game */

#define PHYSICS_TIME_SCALE      0x008FF028u /* a float: the frame's seconds are multiplied by it (1.0) */
#define RAGDOLL_FROM_ENTITIES   0x00230390u /* Ragdoll_UpdateAllFromEntities(float seconds) */
#define PHYS_RUN_FRAME          0x000B4170u /* Phys_RunFrame(float seconds) */
#define PHYS_OBJECTS_UPDATE     0x00230930u /* PhysObj_UpdateAll(void) */
#define RAGDOLL_UPDATE_ALL      0x00230350u /* Ragdoll_UpdateAll(float seconds) */
#define VEHICLE_FROM_PHYSICS    0x0022D230u /* Vehicle_UpdateFromPhysics(vehicle) */
#define PHYS_VEHICLES           0x008FEB70u /* the physics vehicles (guest pointers) */
#define PHYS_VEHICLE_COUNT      0x008FEB9Cu /* how many; the list ends where this count is kept */
#define MAX_VEHICLES            ((PHYS_VEHICLE_COUNT - PHYS_VEHICLES) / 4u)

#define DEFAULT_HZ              60
#define MOST_HZ                 240
#define MOST_FRAME_SECONDS      0.1         /* Com_ModifyMsec (0x002937A0): a frame is at most 100 ms */
#define REPORT_FRAMES           18000ul     /* a line with the steps taken this often */

static void call_with_seconds(uint32_t address, float seconds)
{
    uint32_t bits;

    memcpy(&bits, &seconds, sizeof bits);
    ngl_call_game(address, 1, bits);
}

/* One step of the physics, the four calls as G_VehiclePhysicsFrame makes them. */
static void physics_step(float seconds)
{
    call_with_seconds(RAGDOLL_FROM_ENTITIES, seconds);
    call_with_seconds(PHYS_RUN_FRAME, seconds);
    ngl_call_game(PHYS_OBJECTS_UPDATE, 0, 0);
    call_with_seconds(RAGDOLL_UPDATE_ALL, seconds);
}

static void vehicles_from_physics(void)
{
    uint32_t count = MEM32(PHYS_VEHICLE_COUNT);

    if (count > MAX_VEHICLES)
        count = MAX_VEHICLES;
    for (uint32_t i = 0; i < count; i++)
        ngl_call_game(VEHICLE_FROM_PHYSICS, 1, MEM32(PHYS_VEHICLES + 4u * i));
}

static int physics_hz(void)
{
    static int hz = -1;

    if (hz < 0) {
        const char *setting = getenv("BRO_PHYSICS_HZ");

        hz = setting && *setting ? atoi(setting) : DEFAULT_HZ;
        if (hz < 0 || hz > MOST_HZ)
            hz = DEFAULT_HZ;
        if (hz)
            fprintf(stderr, "[physics] fixed steps of 1/%d s, whatever the frame rate (BRO_PHYSICS_HZ=0: one step "
                    "a frame with the frame's time, as the game does)\n", hz);
        else
            fprintf(stderr, "[physics] one step a frame with the frame's time, as the game does (BRO_PHYSICS_HZ=0)\n");
    }
    return hz;
}

/* G_VehiclePhysicsFrame (0x00233BD0), cdecl, one float on the stack: the frame's seconds. */
void bro_physics_frame(void)
{
    static double due;                      /* seconds of physics owed (may be a little below 0) */
    static unsigned long frames, steps_taken, frames_without;
    const float seconds = MEMF(g_esp + 4) * MEMF(PHYSICS_TIME_SCALE);
    const int hz = physics_hz();

    if (hz == 0 || !(seconds > 0.0f)) {     /* off, or a frame with no time (paused): as the game does */
        physics_step(seconds);
    } else {
        const double step = 1.0 / hz;
        const int most_steps = (int)(MOST_FRAME_SECONDS * hz + 0.5) > 1 ? (int)(MOST_FRAME_SECONDS * hz + 0.5) : 1;
        int steps = 0;

        due += seconds;
        while (due >= 0.75 * step && steps < most_steps) {
            physics_step((float)step);
            due -= step;
            steps++;
        }
        if (steps == most_steps && due > step)
            due = 0.0;                      /* past the cap: dropped, as the game drops it */
        if (!steps) {
            call_with_seconds(RAGDOLL_UPDATE_ALL, 0.0f);    /* the ragdolls' pose back on their bones, no time */
            frames_without++;
        }
        steps_taken += (unsigned long)steps;
        if (++frames % REPORT_FRAMES == 0) {
            fprintf(stderr, "[physics] %lu frames: %lu steps of 1/%d s, %lu frames without one\n", frames,
                    steps_taken, hz, frames_without);
        }
    }
    vehicles_from_physics();
    g_esp += 4;                             /* ret: the return address (the caller pops the seconds) */
}
