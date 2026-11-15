/* Big Red One: the game's fast sine and cosine (0x000110F0), done natively with the rounding the game sets for it.
 *
 * [acct2-bro] 2026-11-13 (the owner, 16:00: "in bro; when doing a full 180 the camera skews when turned all the way
 * around. It's like a fish eye lens"; 11-08 10:12: "in BRO I also noticed the NPC randomly get wide or narrow").
 *
 * The game's function (0x000110F0 to 0x000111F4, packed SSE: four lanes, two of them used):
 *   - its one stack argument is an angle a in radians, and it hands back two results through the pointers in eax and
 *     ecx;
 *   - lane 0 takes a + K (K at 0x0083EFE0, three quarters of a turn) and lane 1 takes a, and each lane works out the
 *     cosine of what it took, so lane 0 is cos(a + 3 pi / 2) = sin(a) (to [eax]) and lane 1 is cos(a) (to [ecx]);
 *   - the cosine of v: x = -|v| x S (S at 0x0082E1E0, 1 / (2 pi)), n = (x - B) + B with B = 1.5 x 2^23 (0x008FEBD0),
 *     which with the rounding toward zero the game sets at start-up (0x00234450: MXCSR |= 0x6000) is x's whole part;
 *     then y = |(n - x) - H| - Q (H at 0x0082E1F0, a half, and Q at 0x0082E200, a quarter), a triangle wave of v,
 *     and the odd polynomial y (c1 + c3 y^2 + c5 y^4 + c7 y^6 + c9 y^8) (c1 to c9 at 0x0082E210 to 0x0082E250), which
 *     is sin(2 pi y).
 * The recompiled code does the same float math under the host's rounding, which is to nearest. Then n is x rounded to
 * the nearest whole number, the fold lands in the wrong half wave for many angles, and the two results stop being the
 * sine and cosine of one angle (results/acct2-bro-tools/sincos_check.py: at 540 degrees a vector 0.60 long; t237: a
 * soldier's yaw axes 0.645 long). AngleVectors (0x002A2AF0) builds the camera's axes and every soldier's from these:
 * facing some ways the view is squeezed and sheared (the fish eye), and soldiers go wide or narrow.
 *
 * Here: the same steps on the same registers, with the game's own constants read where the game reads them, under
 * the host's rounding set toward zero for this function alone (flush to zero and denormals are zero as the calling
 * thread's game MXCSR has them), then put back. Toward zero on every thread, since the fold only works that way. The
 * rest of the port keeps rounding to nearest: ngl_log.c's follower (BRO_HOST_MXCSR=1), which rounded all of the
 * port's float math toward zero and so made a player standing still creep at high frame rates, stays off.
 *
 * Reached through bro/config/stand_in_functions.tsv ("native bro_fast_sincos"): the recompiler leaves 0x000110F0 to
 * this file, so the function reads its own argument and pops its return address, as the game's plain "ret" does (the
 * caller pops the angle). Nothing reads the 16 bytes of scratch the game's version uses on its stack, so they aren't
 * written; xmm0 to xmm6 end as the game's code leaves them (xmm0: the cosine, from its last movss).
 *
 * BRO_SINCOS_CHECK=1 (test runs only): on each thread, every 2^18 calls, a line with the worst distance of the results
 * handed back from the true sine and cosine, and of what rounding to nearest gives (the port before this file). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <xmmintrin.h>

#include "recomp_types.h"

/* The game's constants: guest addresses, read where the game's code reads them. */
#define SINCOS_THREE_QUARTERS 0x0083EFE0u   /* K, 3 pi / 2: added to lane 0 only (one float) */
#define SINCOS_ROUNDER        0x008FEBD0u   /* B: 1.5 x 2^23 in each lane */
#define SINCOS_SIGN_BITS      0x0082E0B0u   /* the sign bit in each lane */
#define SINCOS_SCALE          0x0082E1E0u   /* S */
#define SINCOS_HALF           0x0082E1F0u   /* H */
#define SINCOS_QUARTER        0x0082E200u   /* Q */
#define SINCOS_C1             0x0082E210u
#define SINCOS_C3             0x0082E220u
#define SINCOS_C5             0x0082E230u
#define SINCOS_C7             0x0082E240u
#define SINCOS_C9             0x0082E250u

#define MXCSR_ROUND_TO_ZERO 0x6000u     /* RC (bits 13-14) = 11 */
#define MXCSR_FTZ_DAZ       0x8040u     /* flush to zero (bit 15), denormals are zero (bit 6) */
#define MXCSR_ALL_MASKED    0x1F80u     /* every exception masked */

/* Not inlined, so the compiler can't move any of its float math to the other side of the _mm_setcsr calls. */
#if defined(_MSC_VER)
#define SINCOS_NOINLINE __declspec(noinline)
#else
#define SINCOS_NOINLINE __attribute__((noinline))
#endif

/* The game's instructions from 0x000110F9 to 0x000111D7, in order, on xmm0 to xmm6; the two results end in lanes 0
 * and 1 of xmm4. Each step's float math rounds as the host's MXCSR says. */
static SINCOS_NOINLINE void fast_sincos_steps(float angle)
{
    RecompXmm offsets;

    g_xmm0 = XMM_ZERO();
    g_xmm1 = XMM_SCALAR(MEMF(SINCOS_THREE_QUARTERS));
    g_xmm3 = XMM_MEM(SINCOS_ROUNDER);
    offsets.f[0] = g_xmm1.f[0];                     /* (K, 0, K, 0), which the game builds on its stack */
    offsets.f[1] = g_xmm0.f[0];
    offsets.f[2] = g_xmm1.f[0];
    offsets.f[3] = g_xmm0.f[0];
    g_xmm0 = offsets;
    g_xmm1 = g_xmm0;
    g_xmm0 = XMM_SCALAR(angle);
    g_xmm5 = XMM_MEM(SINCOS_C9);
    g_xmm0 = XMM_SHUFFLE(g_xmm0, g_xmm0, 0);        /* (a, a, a, a) */
    g_xmm0 = XMM_ADD(g_xmm0, g_xmm1);               /* v = (a + K, a, a + K, a) */
    g_xmm1 = g_xmm0;
    g_xmm0 = XMM_MEM(SINCOS_SIGN_BITS);
    g_xmm2 = g_xmm0;
    g_xmm2 = XMM_ANDN(g_xmm2, g_xmm1);              /* |v| */
    g_xmm1 = g_xmm0;
    g_xmm1 = XMM_XOR(g_xmm1, g_xmm2);               /* -|v| */
    g_xmm2 = XMM_MEM(SINCOS_SCALE);
    g_xmm1 = XMM_MUL(g_xmm1, g_xmm2);               /* x = -|v| x S */
    g_xmm2 = g_xmm1;
    g_xmm2 = XMM_SUB(g_xmm2, g_xmm3);               /* the rounding: x - B ... */
    g_xmm2 = XMM_ADD(g_xmm2, g_xmm3);               /* ... + B = n, x's whole part when rounding toward zero */
    g_xmm3 = XMM_MEM(SINCOS_HALF);
    g_xmm2 = XMM_SUB(g_xmm2, g_xmm1);               /* n - x */
    g_xmm1 = XMM_MEM(SINCOS_QUARTER);
    g_xmm2 = XMM_SUB(g_xmm2, g_xmm3);               /* (n - x) - H */
    g_xmm0 = XMM_ANDN(g_xmm0, g_xmm2);              /* |(n - x) - H| */
    g_xmm0 = XMM_SUB(g_xmm0, g_xmm1);               /* y = |(n - x) - H| - Q */
    g_xmm2 = g_xmm0;
    g_xmm2 = XMM_MUL(g_xmm2, g_xmm0);               /* y^2 */
    g_xmm3 = g_xmm2;
    g_xmm3 = XMM_MUL(g_xmm3, g_xmm2);               /* y^4 */
    g_xmm1 = g_xmm0;
    g_xmm1 = XMM_MUL(g_xmm1, g_xmm2);               /* y^3 */
    g_xmm2 = g_xmm0;
    g_xmm2 = XMM_MUL(g_xmm2, g_xmm3);               /* y^5 */
    g_xmm4 = g_xmm2;
    g_xmm4 = XMM_MUL(g_xmm4, g_xmm3);               /* y^9 */
    g_xmm4 = XMM_MUL(g_xmm4, g_xmm5);               /* c9 y^9 */
    g_xmm5 = XMM_MEM(SINCOS_C7);
    g_xmm6 = g_xmm1;
    g_xmm6 = XMM_MUL(g_xmm6, g_xmm3);               /* y^7 */
    g_xmm3 = XMM_MEM(SINCOS_C5);
    g_xmm6 = XMM_MUL(g_xmm6, g_xmm5);               /* c7 y^7 */
    g_xmm4 = XMM_ADD(g_xmm4, g_xmm6);
    g_xmm2 = XMM_MUL(g_xmm2, g_xmm3);               /* c5 y^5 */
    g_xmm4 = XMM_ADD(g_xmm4, g_xmm2);
    g_xmm2 = XMM_MEM(SINCOS_C3);
    g_xmm1 = XMM_MUL(g_xmm1, g_xmm2);               /* c3 y^3 */
    g_xmm4 = XMM_ADD(g_xmm4, g_xmm1);
    g_xmm1 = XMM_MEM(SINCOS_C1);
    g_xmm0 = XMM_MUL(g_xmm0, g_xmm1);               /* c1 y */
    g_xmm4 = XMM_ADD(g_xmm4, g_xmm0);               /* lane 0 the sine, lane 1 the cosine */
}

/* BRO_SINCOS_CHECK: per thread, the worst distance from the true sine and cosine of the results handed back and of
 * rounding to nearest's */
static void note_check(float angle, const float given[2], const float nearest[2])
{
    static RECOMP_TLS unsigned long calls;
    static RECOMP_TLS double worst, worst_nearest, worst_angle;
    const double s = sin((double)angle), c = cos((double)angle);
    const double error = fmax(fabs(given[0] - s), fabs(given[1] - c));
    const double error_nearest = fmax(fabs(nearest[0] - s), fabs(nearest[1] - c));

    if (error > worst)
        worst = error;
    if (error_nearest > worst_nearest) {
        worst_nearest = error_nearest;
        worst_angle = angle;
    }
    if (++calls % (1ul << 18) == 0) {
        fprintf(stderr, "[sincos] %lu calls on this thread: the worst distance from the true sine and cosine %.2e "
                "(rounding toward zero, as the game); rounding to nearest's %.2e (at angle %.5f)\n", calls, worst,
                worst_nearest, worst_angle);
        worst = worst_nearest = 0.0;
    }
}

void bro_fast_sincos(void)
{
    const float angle = MEMF(g_esp + 4);            /* the one stack argument (the caller pops it) */
    const uint32_t sine_at = g_eax, cosine_at = g_ecx;
    const unsigned int host = _mm_getcsr();
    static int check = -1;
    float given[2], nearest[2] = { 0.0f, 0.0f };

    if (check < 0) {
        const char *v = getenv("BRO_SINCOS_CHECK");

        check = v && *v == '1';
    }
    if (check) {                                    /* what the port did before: the host's rounding, to nearest */
        fast_sincos_steps(angle);
        nearest[0] = g_xmm4.f[0];
        nearest[1] = g_xmm4.f[1];
    }
    _mm_setcsr((host & ~(MXCSR_ROUND_TO_ZERO | MXCSR_FTZ_DAZ)) | MXCSR_ALL_MASKED | MXCSR_ROUND_TO_ZERO |
               (g_mxcsr & MXCSR_FTZ_DAZ));
    fast_sincos_steps(angle);
    _mm_setcsr(host);
    given[0] = g_xmm4.f[0];
    given[1] = g_xmm4.f[1];
    MEMF(sine_at) = given[0];                       /* as the game: lane 0 to [eax], then lane 1 to [ecx] */
    MEMF(cosine_at) = given[1];
    g_xmm0 = XMM_SCALAR(given[1]);                  /* the game's last movss leaves the cosine in xmm0 */
    if (check)
        note_check(angle, given, nearest);
    g_esp += 4;                                     /* ret: the return address */
}
