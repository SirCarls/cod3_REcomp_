/* [acct2-fh] 2026-11-13 Finest Hour port: guest memory words watched from frame to frame, for finding what a screen
 * waits for.
 *
 * The title screen did not answer START in acct2-fh-boot3, though the pad's presses arrived. Reading the game's code
 * gives the words that decide it (the controller in use, the "controller pulled" flag, the pad records; see
 * dev/agents/acct2-fh/NOTES.md), but not their values while the game runs. This prints them as they change.
 *
 * Setting (environment):
 *   FH_WATCH=<address>[:<bytes>][,...]   hexadecimal guest addresses, each read as 1, 2 or 4 bytes (4 if not given),
 *                                        at most 32 of them, below 64 MB (the console's memory). At each Swap every
 *                                        word is read; the first frame prints them all, later frames print the ones
 *                                        that changed:
 *       [watch] frame 812: 0x003B67E0 (4) = 0x00000000, was 0xFFFFFFFF
 *   Example: FH_WATCH=3B67E0,3B67EC,3EC534:2 (the controller in use, the "controller pulled" flag, the first pad's
 *   buttons). Reading only: nothing in the game changes. */
#include "fh_watch.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM8 / MEM16 / MEM32: guest memory */

#define MAX_WATCHES     32
#define GUEST_LIMIT     0x04000000u     /* the Xbox's 64 MB */

struct watch {
    uint32_t address;
    unsigned bytes;
    uint32_t last;
};

static struct watch watches[MAX_WATCHES];
static int watch_count = -1;            /* -1: FH_WATCH not read yet */

static void read_setting(void)
{
    const char *setting = getenv("FH_WATCH");
    char *copy, *item, *save = NULL;

    watch_count = 0;
    if (!setting || !*setting)
        return;
    copy = strdup(setting);
    if (!copy)
        return;
    for (item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(item, ':');
        unsigned long address;
        unsigned bytes = 4;

        if (colon) {
            *colon++ = '\0';
            bytes = (unsigned)strtoul(colon, NULL, 10);
        }
        address = strtoul(item, NULL, 16);
        if ((bytes != 1 && bytes != 2 && bytes != 4) || address + bytes > GUEST_LIMIT) {
            fprintf(stderr, "[watch] FH_WATCH: \"%s\" left out (an address below 0x%X, then :1, :2 or :4)\n", item,
                    GUEST_LIMIT);
            continue;
        }
        if (watch_count == MAX_WATCHES) {
            fprintf(stderr, "[watch] FH_WATCH: more than %d words; the rest left out\n", MAX_WATCHES);
            break;
        }
        watches[watch_count].address = (uint32_t)address;
        watches[watch_count].bytes = bytes;
        watch_count++;
    }
    free(copy);
    if (watch_count)
        fprintf(stderr, "[watch] FH_WATCH: %d word(s) watched at each Swap\n", watch_count);
}

static uint32_t read_word(const struct watch *w)
{
    switch (w->bytes) {
    case 1: return MEM8(w->address);
    case 2: return MEM16(w->address);
    default: return MEM32(w->address);
    }
}

void fh_watch_frame(unsigned long frame)
{
    static int first = 1;
    int i;

    if (watch_count < 0)
        read_setting();
    if (!watch_count)
        return;
    for (i = 0; i < watch_count; i++) {
        struct watch *w = &watches[i];
        uint32_t now = read_word(w);

        if (first)
            fprintf(stderr, "[watch] frame %lu: 0x%08X (%u) = 0x%0*X\n", frame, w->address, w->bytes,
                    (int)w->bytes * 2, now);
        else if (now != w->last)
            fprintf(stderr, "[watch] frame %lu: 0x%08X (%u) = 0x%0*X, was 0x%0*X\n", frame, w->address, w->bytes,
                    (int)w->bytes * 2, now, (int)w->bytes * 2, w->last);
        w->last = now;
    }
    first = 0;
}
