/* inputkit C interface test. [input-port] 2026-11-24
 *
 * Checks that the C API (inputkit.h) gives the same answers as the C++ code for the calls the C ports
 * (Big Red One, Finest Hour) make: key names, a bind table round trip, the motion queue and the turn math.
 * Prints "inputkit_c: N checks, M failed" and exits non-zero on any failure.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "inputkit/inputkit.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static int near_eq(double a, double b) { return fabs(a - b) < 1e-4; }

static void test_keys(void) {
    CHECK(ik_key_parse("V") == 'V');
    CHECK(ik_key_parse("mouse3") == IK_MOUSE3);
    CHECK(ik_key_parse("nonsense") == IK_NO_KEY);
    CHECK(strcmp(ik_key_name(IK_MOUSE1), "MOUSE1") == 0);
    CHECK(strcmp(ik_key_name(IK_NO_KEY), "") == 0);
    CHECK(ik_key_parse(ik_key_name(IK_WHEEL_UP)) == IK_WHEEL_UP);
    /* SDL's middle button is 2 and its right button is 3; the game's MOUSE2 is right. */
    CHECK(ik_key_from_sdl_mouse_button(1) == IK_MOUSE1);
    CHECK(ik_key_from_sdl_mouse_button(2) == IK_MOUSE3);
    CHECK(ik_key_from_sdl_mouse_button(3) == IK_MOUSE2);
    /* SDL_SCANCODE_A is 4. */
    CHECK(ik_key_from_sdl_scancode(4) == 'A');
}

static const ik_action kActions[] = {
    {"fire", "Fire", 0, "MOUSE1"},
    {"ads", "Aim down sights", 0, "MOUSE2"},
    {"melee", "Melee", 0, "V,MOUSE3"},
    {"jump", "Jump", 0, "SPACE"},
    {"veh_gas", "Accelerate", 1, "W"},
};

static void test_binds(void) {
    ik_binds *b = ik_binds_new(kActions, 5, "test_binds");
    int melee, jump, fire, out[IK_MAX_ACTIONS_PER_KEY], n;
    CHECK(b != NULL);
    CHECK(ik_binds_parse(b, "") == 0);
    CHECK(strcmp(ik_binds_serialize(b), "") == 0);

    melee = ik_binds_action_by_id(b, "melee");
    jump = ik_binds_action_by_id(b, "jump");
    fire = ik_binds_action_by_id(b, "fire");
    CHECK(melee == 2 && jump == 3 && fire == 0);
    CHECK(ik_binds_action_by_id(b, "nope") == -1);
    CHECK(ik_binds_key(b, melee, 0) == 'V');
    CHECK(ik_binds_key(b, melee, 1) == IK_MOUSE3);
    CHECK(strcmp(ik_binds_keys_text(b, melee), "V, MOUSE3") == 0);

    /* Rebinding jump to C leaves the defaults, so the setting gets text that parses back the same. */
    CHECK(ik_binds_assign_text(b, jump, "C") == 1);
    CHECK(ik_binds_key(b, jump, 0) == 'C');
    {
        char saved[512];
        ik_binds *c = ik_binds_new(kActions, 5, "test_binds");
        snprintf(saved, sizeof saved, "%s", ik_binds_serialize(b));
        CHECK(saved[0] != '\0');
        CHECK(ik_binds_parse(c, saved) == 0);
        CHECK(ik_binds_key(c, jump, 0) == 'C');
        CHECK(strcmp(ik_binds_serialize(c), saved) == 0);
        ik_binds_free(c);
    }

    /* MOUSE1 does Fire only. */
    n = ik_binds_actions_of_key(b, IK_MOUSE1, out, IK_MAX_ACTIONS_PER_KEY);
    CHECK(n == 1 && out[0] == fire);

    /* A key name that doesn't exist is refused and nothing changes. */
    CHECK(ik_binds_assign_text(b, jump, "NOT_A_KEY") == 0);
    CHECK(ik_binds_key(b, jump, 0) == 'C');
    CHECK(ik_binds_last_note(b)[0] != '\0');

    /* Null objects are safe. */
    CHECK(ik_binds_parse(NULL, "x") == 0);
    CHECK(strcmp(ik_binds_serialize(NULL), "") == 0);
    ik_binds_free(b);
    ik_binds_free(NULL);
}

static void test_motion(void) {
    ik_motion *m = ik_motion_new();
    float dx = -1, dy = -1;
    const int64_t ms = 1000000;
    /* Without resampling, everything added comes out at the next take. */
    ik_motion_add(m, 3, 4, 10 * ms, 0);
    ik_motion_add(m, 1, -2, 11 * ms, 0);
    ik_motion_take(m, 12 * ms, &dx, &dy);
    CHECK(near_eq(dx, 4) && near_eq(dy, 2));
    ik_motion_take(m, 13 * ms, &dx, &dy);
    CHECK(near_eq(dx, 0) && near_eq(dy, 0));
    /* With resampling, the total over time is kept (nothing is lost, nothing is made up). */
    {
        float sum_x = 0;
        int64_t t;
        for (t = 20; t < 60; ++t) ik_motion_add(m, 2, 0, t * ms, 1);
        for (t = 20; t <= 120; t += 4) {
            ik_motion_take(m, t * ms, &dx, &dy);
            sum_x += dx;
        }
        CHECK(near_eq(sum_x, 80));
    }
    ik_motion_clear(m);
    ik_motion_take(m, 200 * ms, &dx, &dy);
    CHECK(near_eq(dx, 0) && near_eq(dy, 0));
    ik_motion_free(m);
}

static void test_turn(void) {
    ik_turn t;
    float yaw, pitch;
    ik_turn_defaults(&t);
    CHECK(near_eq(t.sensitivity, 5) && near_eq(t.m_yaw, 0.022) && t.invert == 0);
    ik_turn_from_counts(&t, 10, 10, &yaw, &pitch);
    CHECK(near_eq(yaw, -1.1) && near_eq(pitch, 1.1));
    t.invert = 1;
    ik_turn_from_counts(&t, 10, 10, &yaw, &pitch);
    CHECK(near_eq(pitch, -1.1));
    CHECK(near_eq(ik_zoom_ratio(32.5, 65), 0.5));
    CHECK(near_eq(ik_zoom_ratio(0, 65), 1));
}

static void test_stick(void) {
    int x = 1000, y = 0;
    inputkit_radial_dead_zone(&x, &y, 0.25f);
    CHECK(x == 0 && y == 0);
    x = 32767;
    y = 0;
    inputkit_radial_dead_zone(&x, &y, 0.25f);
    CHECK(x > 32000 && y == 0);
}

int main(void) {
    test_keys();
    test_binds();
    test_motion();
    test_turn();
    test_stick();
    printf("inputkit_c: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
