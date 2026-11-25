/* [waw-decomp/fh3] 2026-11-23: RenderWare worlds freed under the objects still in them, and two more of RenderWare's
 * lists left pointing at freed memory: crashes Finest Hour has in later levels on the console as well.
 *
 * What the game does (Europe v3 default.xbe; read from our recompiled C, dev/agents/waw-decomp/FH3-2026-11-23.md):
 *   - Each level section is a RenderWare world (RpWorld). RpWorldCreate is 0x001F14E0, RpWorldStreamRead 0x001F5700
 *     (a world read from a level's file), RpWorldDestroy 0x001F1450 (9 direct callers; 0x001F14B0 jumps to it). The
 *     destroy unties the atomics and lights from the world's sectors, runs the world plugins' destructors
 *     (0x001D0300 with the registry at 0x00332FB0) and frees the world (RwFree through [0x00407EF8]), but leaves each
 *     object's own pointer to its world as it was.
 *   - An atomic's world is in its world plugin's block, at the offset kept in [0x003F6DAC] ({world, the frame sync
 *     callback it replaced}). RenderWare calls 0x001F3CC0 when the atomic's frame moves (its frame callback, set by
 *     0x001F3D00): the saved callback, then, when the atomic has a world, 0x001F3B10 / 0x001F3B70 tie it again into
 *     that world's sectors. With a freed world that walks a sector tree in freed memory: a crash where the memory was
 *     used again (Road to Remagen, the passage by the large building, every time), writes into it where it was not
 *     (crashes later, anywhere).
 *   - RpWorldRemoveClump 0x001F4960 (cdecl world, clump; returns world) takes the world from the clump's world plugin
 *     ([0x003F6DB0]), counts it down at world +0x24, moves the world's clump cursor at +0x28, unlinks the clump's
 *     world link (clump +0x20) and removes its atomics and lights through 0x001EFC30 / 0x001EFCC0 / 0x001EFC70.
 *     RpWorldRemoveLight 0x001F4600 (cdecl world, light; returns world) clears the light's world ([0x003F6DB4]),
 *     unties it from sectors (0x001F3D80) and unlinks its world link (light +0x34 next, +0x38 previous). With a freed
 *     world both write into freed memory.
 *   - 0x001DDA70 (RpCollisionWorldForAllIntersections, cdecl world, intersection, callback, data) reads the world
 *     at once; the game asks it of every loaded section, and one caller (the sewers, restarting at a checkpoint) asks
 *     it of a section marked loaded before its world is read: world 0.
 *   - 0x001CE8E0 (RwFrameSyncDirty, no arguments) walks the dirty frames' list (head 0x00407E7C, each frame's link
 *     at +8: next at +0, previous at +4) to its head. A frame freed with a level's sections while still in it leaves
 *     a link of 0 or one into freed memory (just after a mission ends: end of Airfield Ambush).
 *
 * What this does (each through fh/config/wrapped_functions.tsv: the game's own body runs, renamed fh_lifted_*):
 *   - The worlds freed are kept (destroy) until a world is made or read at the same address again (create, read).
 *   - Before 0x001F3CC0 ties an atomic, before 0x001F4960 / 0x001F4600 remove a clump or a light, an object whose world
 *     is freed is taken out of it the way the world's own removal would (its world pointer cleared; a light's world
 *     link left on its own), and the freed world is not touched. The game then places it in the world where it is,
 *     as it does any object with no world.
 *   - 0x001DDA70 with world 0 meets nothing and returns 0 (what it returns for a world with nothing in the way).
 *   - Before 0x001CE8E0 the dirty frames' list is cut at the first link that is no frame's (0, outside the console's
 *     64 MB, not 4-aligned, or whose previous link doesn't point back), and what was past it is left out.
 *   The idea and the case come from FinestHourRemastered's notes (information only: nothing of its code is used; each
 *   fact above re-derived in our own decompile). Every case is told in the log the first few times.
 *
 * Cost: one comparison when no world is freed; otherwise a look through the freed worlds kept (a few, usually) per
 * atomic synced. No allocation. FH_WORLDS_AS_GAME=1: the game's own behaviour, as it was (for tests). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"           /* MEM32, g_esp, g_eax */
#include "xbox_memory_layout.h"     /* XBOX_PTR */

#ifdef _WIN32
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&g_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* the game's own bodies, renamed by tools/fh_decomp/fh_wrap.py */
void fh_lifted_001F14E0(void);
void fh_lifted_001F5700(void);
void fh_lifted_001F1450(void);
void fh_lifted_001F3CC0(void);
void fh_lifted_001F4960(void);
void fh_lifted_001F4600(void);
void fh_lifted_001DDA70(void);
void fh_lifted_001CE8E0(void);

#define ATOMIC_WORLD_OFFSET 0x003F6DACu  /* holds the world plugin's offset in an atomic: {world, saved sync} */
#define CLUMP_WORLD_OFFSET  0x003F6DB0u  /* in a clump: {world, ...} */
#define LIGHT_WORLD_OFFSET  0x003F6DB4u  /* in a light: {world, ...} */
#define LIGHT_LINK          0x34u        /* a light's link in its world's list: next, then previous */
#define DIRTY_FRAMES        0x00407E7Cu  /* RenderWare's dirty frames' list head: next, then previous */
#define CONSOLE_RAM         0x04000000u  /* the console's 64 MB */

/* The worlds freed and not made again. A full list drops its oldest: that world is then treated as the game treats
 * it (as alive), never the other way round, so a full list can't take a live world's objects out of it. */
enum { DEAD_MAX = 256 };
static uint32_t g_dead[DEAD_MAX];
static volatile int g_dead_count;
static int g_dead_next;

static int as_game(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("FH_WORLDS_AS_GAME");
        v = e && *e && *e != '0';
        if (v)
            fprintf(stderr, "[fh] worlds: FH_WORLDS_AS_GAME: freed worlds left as the game leaves them\n");
    }
    return v;
}

static void dead_remove_locked(uint32_t world)
{
    for (int i = 0; i < g_dead_count; i++) {
        if (g_dead[i] == world) {
            g_dead[i] = g_dead[--g_dead_count];
            if (g_dead_next > g_dead_count)
                g_dead_next = g_dead_count;
            return;
        }
    }
}

static void born(uint32_t world)
{
    if (!world || !g_dead_count)
        return;
    LOCK();
    dead_remove_locked(world);
    UNLOCK();
}

static void died(uint32_t world)
{
    if (!world || as_game())
        return;
    LOCK();
    dead_remove_locked(world);
    if (g_dead_count < DEAD_MAX) {
        g_dead[g_dead_count++] = world;
    } else {
        g_dead[g_dead_next] = world;            /* the oldest goes */
        g_dead_next = (g_dead_next + 1) % DEAD_MAX;
    }
    UNLOCK();
}

static int is_dead(uint32_t world)
{
    int dead = 0;
    if (!world || !g_dead_count)
        return 0;
    LOCK();
    for (int i = 0; i < g_dead_count && !dead; i++)
        dead = g_dead[i] == world;
    UNLOCK();
    return dead;
}

static void told(const char *what, uint32_t object, uint32_t world)
{
    static int n;
    if (n < 32) {
        n++;
        fprintf(stderr, "[fh] worlds: %s %08X was still in the freed world %08X: taken out\n", what, object, world);
    }
}

/* RpWorldCreate (cdecl, the bounding box): the world, or 0 */
void sub_001F14E0(void)
{
    fh_lifted_001F14E0();
    born(g_eax);
}

/* RpWorldStreamRead (cdecl, the stream): the world, or 0 */
void sub_001F5700(void)
{
    fh_lifted_001F5700();
    born(g_eax);
}

/* RpWorldDestroy (cdecl, the world) */
void sub_001F1450(void)
{
    died(MEM32(g_esp + 4));
    fh_lifted_001F1450();
}

/* An atomic's frame synced (cdecl, the atomic): its saved callback, then tied again into its world's sectors */
void sub_001F3CC0(void)
{
    uint32_t atomic = MEM32(g_esp + 4);
    uint32_t at = atomic + MEM32(ATOMIC_WORLD_OFFSET);
    uint32_t world = MEM32(at);

    if (is_dead(world)) {
        told("an atomic", atomic, world);
        MEM32(at) = 0;          /* its ties went with the world's sectors */
    }
    fh_lifted_001F3CC0();
}

/* RpWorldRemoveClump (cdecl, world, clump): the world */
void sub_001F4960(void)
{
    uint32_t world = MEM32(g_esp + 4), clump = MEM32(g_esp + 8);
    uint32_t at = clump + MEM32(CLUMP_WORLD_OFFSET);
    uint32_t own = MEM32(at);    /* the world the game's body counts down and unlinks from */

    if (is_dead(own) || (!own && is_dead(world))) {
        told("a clump", clump, own ? own : world);
        MEM32(at) = 0;
        g_eax = world;
        g_esp += 4;             /* cdecl: the return address only */
        return;
    }
    fh_lifted_001F4960();
}

/* RpWorldRemoveLight (cdecl, world, light): the world */
void sub_001F4600(void)
{
    uint32_t world = MEM32(g_esp + 4), light = MEM32(g_esp + 8);
    uint32_t at = light + MEM32(LIGHT_WORLD_OFFSET);
    uint32_t own = MEM32(at);

    if (is_dead(world) || is_dead(own)) {
        told("a light", light, is_dead(world) ? world : own);
        MEM32(at) = 0;
        MEM32(light + LIGHT_LINK) = light + LIGHT_LINK;      /* its link on its own, as an unlinked one */
        MEM32(light + LIGHT_LINK + 4) = light + LIGHT_LINK;
        g_eax = world;
        g_esp += 4;
        return;
    }
    fh_lifted_001F4600();
}

/* RpCollisionWorldForAllIntersections (cdecl, world, intersection, callback, data): the world */
void sub_001DDA70(void)
{
    if (!MEM32(g_esp + 4) && !as_game()) {
        static int n;
        if (n < 8) {
            n++;
            fprintf(stderr, "[fh] worlds: intersections asked of a section with no world yet (from %08X)\n",
                    MEM32(g_esp));
        }
        g_eax = 0;
        g_esp += 4;
        return;
    }
    fh_lifted_001DDA70();
}

/* RwFrameSyncDirty (no arguments): 1 */
void sub_001CE8E0(void)
{
    if (!as_game()) {
        uint32_t prev = DIRTY_FRAMES, at = MEM32(DIRTY_FRAMES);
        for (int n = 0; at != DIRTY_FRAMES && n < (1 << 16); n++) {
            int ok = at >= 0x10000u && at < CONSOLE_RAM - 8 && !(at & 3u) && MEM32(at + 4) == prev;
            if (!ok) {
                static int told_n;
                if (told_n < 8) {
                    told_n++;
                    fprintf(stderr, "[fh] worlds: the dirty frames' list broken after %08X (link %08X): cut there\n",
                            prev, at);
                }
                MEM32(prev) = DIRTY_FRAMES;
                MEM32(DIRTY_FRAMES + 4) = prev;
                break;
            }
            prev = at;
            at = MEM32(at);
        }
    }
    fh_lifted_001CE8E0();
}
