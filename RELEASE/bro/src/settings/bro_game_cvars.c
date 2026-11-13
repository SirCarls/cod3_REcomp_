/* [f2-menus] Big Red One's own cvars for the F2 window (see bro_game_cvars.h and dev/agents/f2-menus/bro/DVARS.md).
 *
 * The game (the campaign program, default.xbe) keeps its cvars as Quake III does:
 *   cvar_indexes   0x00940A10  610 (0x262) records of 44 bytes, filled in order by Cvar_Get (0x0029C2D0);
 *   cvar_numIndexes 0x009476F8 how many are in use;
 *   a record: name +0x00, string +0x04, resetString +0x08, latchedString +0x0C, flags +0x10, modified +0x14,
 *             modificationCount +0x18, value (float) +0x1C, integer +0x20, next +0x24, hashNext +0x28.
 * Cvar_Set2 (0x0029C030) is cdecl: (name, value, force); without force it leaves ROM (0x40) and INIT (0x10) cvars
 * alone, CHEAT (0x200) ones while sv_cheats is 0, and keeps a LATCH (0x20) cvar's value for the next level.
 * Cbuf_AddText (0x002AABB0) takes its text in eax (Cbuf_AddText's callers: mov eax, text; call).
 * Both are run as ngl_frame_clock.c runs the game's own functions: a call from here, at the frame's end, with the
 * arguments and the texts on the game's stack below its stack pointer, and the caller's eax, ecx and edx put back. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* XBOX_PTR: guest memory */
#include "bro_settings.h"
#include "bro_game_cvars.h"

/* The campaign program's (default.xbe) */
#define CVAR_INDEXES        0x00940A10u
#define CVAR_NUM_INDEXES    0x009476F8u
#define CVAR_RECORD         0x2Cu
#define CVAR_MAX            0x262
#define CVAR_SET2           0x0029C030u
#define CBUF_ADDTEXT        0x002AABB0u
#define CBUF_DATA           0x00935328u     /* the command buffer's data pointer: 0 until Cbuf_Init */

#define CVAR_NAME           0x00u
#define CVAR_STRING         0x04u
#define CVAR_RESET          0x08u
#define CVAR_LATCHED        0x0Cu
#define CVAR_FLAGS          0x10u
#define CVAR_MOD_COUNT      0x18u
#define CVAR_VALUE          0x1Cu

#define NAME_CHARS          64
#define VALUE_CHARS         256
#define MAX_KEPT            256
#define MAX_QUEUED          64

static const char *guest_text(uint32_t address)
{
    return address ? (const char *)XBOX_PTR(address) : NULL;
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

int bro_game_cvars_count(void)
{
    uint32_t count = MEM32(CVAR_NUM_INDEXES);

    return count > CVAR_MAX ? CVAR_MAX : (int)count;
}

int bro_game_cvar_at(int index, struct bro_cvar *out)
{
    uint32_t record = CVAR_INDEXES + (uint32_t)index * CVAR_RECORD;
    uint32_t name;

    if (index < 0 || index >= bro_game_cvars_count())
        return 0;
    name = MEM32(record + CVAR_NAME);
    if (!name || !MEM32(record + CVAR_STRING))
        return 0;
    out->name = guest_text(name);
    out->value = guest_text(MEM32(record + CVAR_STRING));
    out->reset = MEM32(record + CVAR_RESET) ? guest_text(MEM32(record + CVAR_RESET)) : "";
    out->latched = guest_text(MEM32(record + CVAR_LATCHED));
    out->flags = MEM32(record + CVAR_FLAGS);
    out->modification_count = (int)MEM32(record + CVAR_MOD_COUNT);
    {
        uint32_t bits = MEM32(record + CVAR_VALUE);

        memcpy(&out->number, &bits, sizeof out->number);
    }
    return 1;
}

/* Names found before, by a hash of the name, so the F2 window's rows don't search the game's list every frame. A
 * slot is checked against the game's record before it is used (the game never moves a cvar to another index). */
#define CACHE_SLOTS 1024
static int cache_index[CACHE_SLOTS];
static int cache_ready;

static unsigned name_hash(const char *name)
{
    unsigned h = 2166136261u;

    for (; *name; name++)
        h = (h ^ (unsigned)tolower((unsigned char)*name)) * 16777619u;
    return h % CACHE_SLOTS;
}

static int find_index(const char *name)
{
    int i, count = bro_game_cvars_count();
    unsigned h = name_hash(name);
    struct bro_cvar c;

    if (!cache_ready) {
        for (i = 0; i < CACHE_SLOTS; i++)
            cache_index[i] = -1;
        cache_ready = 1;
    }
    i = cache_index[h];
    if (i >= 0 && bro_game_cvar_at(i, &c) && same_name(c.name, name))
        return i;
    for (i = 0; i < count; i++)
        if (bro_game_cvar_at(i, &c) && same_name(c.name, name)) {
            cache_index[h] = i;
            return i;
        }
    return -1;
}

int bro_game_cvar_find(const char *name, struct bro_cvar *out)
{
    int i = find_index(name);

    return i >= 0 && bro_game_cvar_at(i, out);
}

int bro_game_cvar_read_only(uint32_t flags)
{
    return (flags & (BRO_CVAR_ROM | BRO_CVAR_INIT)) != 0;
}

/* ---- the game's functions, called from here ---------------------------------------------------------------------- */

static void push32(uint32_t value)
{
    g_esp -= 4u;
    MEM32(g_esp) = value;
}

/* A copy of text on the game's stack (below what is in use), its address returned. */
static uint32_t push_text(const char *text)
{
    size_t n = strlen(text) + 1;

    g_esp -= (uint32_t)((n + 3u) & ~3u);
    memcpy(XBOX_PTR(g_esp), text, n);
    return g_esp;
}

static int call_game(uint32_t va)
{
    static int reported;
    recomp_func_t fn = recomp_lookup(va);

    if (!fn) {
        if (reported++ < 4)
            fprintf(stderr, "[f2] no recompiled function at 0x%08X\n", va);
        return -1;
    }
    push32(0);      /* the return address; the function's ret takes it off again */
    fn();
    return 0;
}

static void game_set(const char *name, const char *value, int force)
{
    uint32_t esp = g_esp, eax = g_eax, ecx = g_ecx, edx = g_edx;
    uint32_t name_at, value_at;

    g_esp -= 64u;   /* room between the caller's stack and ours */
    name_at = push_text(name);
    value_at = push_text(value);
    g_esp &= ~15u;
    push32((uint32_t)force);
    push32(value_at);
    push32(name_at);
    call_game(CVAR_SET2);
    g_esp = esp;
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
}

static void game_command(const char *text)
{
    uint32_t esp = g_esp, eax = g_eax, ecx = g_ecx, edx = g_edx;

    g_esp -= 64u;
    g_eax = push_text(text);
    g_esp &= ~15u;
    call_game(CBUF_ADDTEXT);
    g_esp = esp;
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
}

/* ---- the queue, and what the player chose ------------------------------------------------------------------------- */

static struct {
    char name[NAME_CHARS];
    char value[VALUE_CHARS];
    int is_command;
} queued[MAX_QUEUED];
static int queued_count;

static struct {
    char name[NAME_CHARS];
    char value[VALUE_CHARS];
    int index;              /* in the game's cvars, -1 until it has made it */
    int seen;               /* its modification count after our last look, -1: not put in yet */
    int puts_back;          /* how many times the game reset it and it went back (logged a few times) */
} kept[MAX_KEPT];
static int kept_count;
static int kept_loaded;
static int adopted;

static int kept_slot(const char *name, int make)
{
    int i;

    for (i = 0; i < kept_count; i++)
        if (same_name(kept[i].name, name))
            return i;
    if (!make || kept_count == MAX_KEPT)
        return -1;
    snprintf(kept[kept_count].name, NAME_CHARS, "%s", name);
    kept[kept_count].value[0] = '\0';
    kept[kept_count].index = -1;
    kept[kept_count].seen = -1;
    kept[kept_count].puts_back = 0;
    return kept_count++;
}

static void keep_from_file(const char *key, const char *value, void *user)
{
    int slot;

    (void)user;
    if (strlen(key) >= NAME_CHARS)
        return;
    slot = kept_slot(key, 1);
    if (slot >= 0)
        snprintf(kept[slot].value, VALUE_CHARS, "%s", value);
}

static void load_kept(void)
{
    if (kept_loaded)
        return;
    kept_loaded = 1;
    bro_settings_each("cvars.", keep_from_file, NULL);
    if (kept_count)
        fprintf(stderr, "[f2] %d of the game's cvars kept in bro.toml's [cvars]\n", kept_count);
}

const char *bro_game_cvar_kept(const char *name)
{
    int slot;

    load_kept();
    slot = kept_slot(name, 0);
    return slot >= 0 ? kept[slot].value : NULL;
}

static void queue(const char *name, const char *value, int is_command)
{
    if (queued_count == MAX_QUEUED) {
        fprintf(stderr, "[f2] too many changes in one frame; \"%s\" left out\n", name);
        return;
    }
    snprintf(queued[queued_count].name, NAME_CHARS, "%s", name);
    snprintf(queued[queued_count].value, VALUE_CHARS, "%s", value ? value : "");
    queued[queued_count].is_command = is_command;
    queued_count++;
}

void bro_game_cvar_set(const char *name, const char *value)
{
    char key[NAME_CHARS + 8];
    int slot;

    load_kept();
    if (strlen(name) >= NAME_CHARS || strlen(value) >= VALUE_CHARS)
        return;
    queue(name, value, 0);
    slot = kept_slot(name, 1);
    if (slot >= 0) {
        snprintf(kept[slot].value, VALUE_CHARS, "%s", value);
        kept[slot].seen = -1;      /* put in by the queue; its next modification count is ours */
    }
    snprintf(key, sizeof key, "cvars.%s", name);
    bro_settings_set(key, value);
}

uint32_t bro_game_peek32(uint32_t address)
{
    return address >= 0x10000u && address < 0x03FFFFFCu ? MEM32(address) : 0;
}

void bro_game_command(const char *text)
{
    char line[VALUE_CHARS];

    snprintf(line, sizeof line, "%s\n", text);
    queue("", line, 1);
}

int bro_game_cvars_take_adopted(void)
{
    int was = adopted;

    adopted = 0;
    return was;
}

/* ---- once a frame ------------------------------------------------------------------------------------------------- */

static void put_in(const char *name, const char *value)
{
    struct bro_cvar c;

    if (bro_game_cvar_find(name, &c)) {
        if (bro_game_cvar_read_only(c.flags)) {
            fprintf(stderr, "[f2] %s is the game's own (read-only); left at \"%s\"\n", c.name, c.value);
            return;
        }
        game_set(name, value, (c.flags & BRO_CVAR_CHEAT) != 0);
    } else
        game_set(name, value, 0);   /* the game makes it, as its own "set" command would */
}

static void watch_kept(void)
{
    int i;

    for (i = 0; i < kept_count; i++) {
        struct bro_cvar c;

        if (kept[i].index < 0 || !bro_game_cvar_at(kept[i].index, &c) || !same_name(c.name, kept[i].name)) {
            kept[i].index = find_index(kept[i].name);
            if (kept[i].index < 0 || !bro_game_cvar_at(kept[i].index, &c))
                continue;           /* the game hasn't made it yet */
        }
        if (bro_game_cvar_read_only(c.flags))
            continue;
        if (kept[i].seen >= 0 && c.modification_count == kept[i].seen)
            continue;               /* nothing changed since our last look */
        if (strcmp(c.value, kept[i].value) != 0 && !(c.latched && !strcmp(c.latched, kept[i].value))) {
            if (kept[i].seen < 0 || !strcmp(c.value, c.reset)) {
                /* first seen this run, or the game put it back to its default (a level registering it again: a
                 * CHEAT cvar while sv_cheats is 0; a script's "back to normal") */
                if (kept[i].seen >= 0 && kept[i].puts_back++ < 3)
                    fprintf(stderr, "[f2] the game put %s back to \"%s\"; it is \"%s\" again\n", c.name, c.value,
                            kept[i].value);
                game_set(kept[i].name, kept[i].value, (c.flags & BRO_CVAR_CHEAT) != 0);
                bro_game_cvar_at(kept[i].index, &c);
            } else if (c.flags & BRO_CVAR_ARCHIVE) {
                /* an ARCHIVE cvar (the player's profile) the game set to another value itself: its own menu or the
                 * profile it loaded. F2 follows it and keeps it. */
                char key[NAME_CHARS + 8];

                fprintf(stderr, "[f2] the game set %s to \"%s\" (was \"%s\" in F2); kept\n", c.name, c.value,
                        kept[i].value);
                snprintf(kept[i].value, VALUE_CHARS, "%s", c.value);
                snprintf(key, sizeof key, "cvars.%s", kept[i].name);
                bro_settings_set(key, c.value);
                adopted = 1;
            }
            /* any other cvar the game set to a value of its own (a level's script, for a scene) is left so; the
             * player's value goes back in when the game puts the cvar back to its default */
        }
        kept[i].seen = c.modification_count;
    }
}

void bro_game_cvars_frame(void)
{
    int i;

    load_kept();
    if (bro_game_cvars_count() == 0)
        return;                     /* the game hasn't started its cvars */
    for (i = 0; i < queued_count; i++) {
        if (queued[i].is_command) {
            if (MEM32(CBUF_DATA))
                game_command(queued[i].value);
        } else
            put_in(queued[i].name, queued[i].value);
    }
    queued_count = 0;
    watch_kept();
}
