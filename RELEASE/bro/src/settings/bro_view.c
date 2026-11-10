/* [acct2-bro-game] Big Red One port: the view's settings from bro.toml.
 *
 * The field of view. The client game keeps its cg_fov in Quake III's vmCvar_t layout: the cvar's handle (+0x0), its
 * modification count (+0x4), the value as a float (+0x8), and more after it. Only one place in the game reads that
 * float: CG_GetViewFov (0x0027A7B0), which keeps it between the game's two limits (the floats at 0x0083EB20 and
 * 0x0083EF28) and hands it to CG_CalcFov (0x002843E0) to make the view's angles from. It is the angle across the
 * picture, in degrees.
 *
 * display.fov in bro.toml (BRO_FOV), when given, is written into that float once a frame, so the view keeps the
 * player's angle whatever the game sets it to (its default when a level starts, a saved profile's). Each time the
 * game sets a value of its own, a log line says so:
 *   [view] the game set its field of view to 65; it stays 100
 * Left out, nothing is written and the game's own angle stays. 1 to 160, as in Quake III's cg_fov.
 *
 * The owner's 500 fps tests use 100. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "bro_settings.h"
#include "bro_view.h"

/* The game's data (the campaign program). */
#define CG_FOV          0x0094C3E0u     /* cg_fov as the client game keeps it */
#define VMCVAR_VALUE    0x8u            /* the value as a float */

#define FOV_LOWEST      1.0
#define FOV_HIGHEST     160.0

static int setting = -1;        /* -1: not read yet; 0: the game's own angle; 1: the player's */
static float fov;               /* the player's angle */
static uint32_t fov_bits;       /* the same as the float's bits, as the game's memory holds it */
static uint32_t game_bits;      /* the last angle the game set itself, so each one is logged once */

static void read_setting(void)
{
    double wanted;

    setting = 0;
    if (!bro_setting_given("display.fov", "BRO_FOV"))
        return;
    wanted = bro_setting_number("display.fov", "BRO_FOV", 0.0);
    if (wanted < FOV_LOWEST || wanted > FOV_HIGHEST) {
        fprintf(stderr, "[view] display.fov %g is not within %g to %g: the game's own field of view stays\n", wanted,
                FOV_LOWEST, FOV_HIGHEST);
        return;
    }
    fov = (float)wanted;
    memcpy(&fov_bits, &fov, sizeof fov_bits);
    setting = 1;
    fprintf(stderr, "[view] field of view: %g degrees (display.fov)\n", wanted);
}

void bro_view_apply(void)
{
    uint32_t bits;

    if (setting < 0)
        read_setting();
    if (!setting)
        return;
    bits = MEM32(CG_FOV + VMCVAR_VALUE);
    if (bits != fov_bits && bits != game_bits) {
        float game;

        memcpy(&game, &bits, sizeof game);
        fprintf(stderr, "[view] the game set its field of view to %g; it stays %g\n", game, fov);
        game_bits = bits;
    }
    MEM32(CG_FOV + VMCVAR_VALUE) = fov_bits;
}
