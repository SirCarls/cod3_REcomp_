/* [acct2-fh] 2026-11-13 Finest Hour port: the game's own writes to the NV2A push buffer.
 *
 * On the Xbox, Direct3D and RenderWare put the GPU's commands in a push buffer: a 32-bit header (a method address,
 * a count of words) and then the words. Direct3D's device keeps where the next word goes at 0x002309C0 and where the
 * room ends at 0x002309C4 (the device is the global at 0x002309C0: 0x002309B8 points at it, +8 is its flags byte);
 * D3DDevice_MakeSpace (0x002252C0) makes room when a writer finds the two equal or crossed.
 *
 * The port answers the Direct3D functions natively, so almost nothing reaches the push buffer. One thing does:
 * RenderWare's skinned pipeline writes two vertex constants straight into it before each skinned draw (0x001ECF70,
 * the only code outside Direct3D that names 0x002309C0): NV2A 0x1EA4 (the constant to load) = 5, then 0x0B80 with four
 * floats, the ambient light (c-91 in Direct3D's numbering); and 0x1EA4 = 191, then the material's color (c95). The
 * skinning programs end with oD0 = c95 x the light they add up, so without these two the soldiers' arms and weapons
 * came out black (boot29-boot31). MakeSpace was a stand-in that returned 0, and the writes went to guest address 0.
 *
 * Here: MakeSpace hands out a push area of the port's own (64 KB, from the guest heap), with the device's end of room
 * set 1 KB short of its end (writers put up to 9 words after their check). Before each draw and each vertex constant
 * write, fh_push_read() goes through the words written since the last read, in order: 0x1EA4 sets the constant to
 * load, and 0x0B80-0x0BFC load its words, four to a register, the next register after each four (as the NV2A does).
 * Every other method is passed over. A jump, a call or a return (no count of words to pass over) ends the read.
 *
 * Settings: FH_PUSH=0 reads nothing (the constants as before); FH_PUSH_LOG=1 prints the first 20 reads that found
 * words, and at the 1000th read a count of what the reads found. */
#include "fh_push.h"

#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"           /* MEM32: guest memory */
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc */
#include "fh_vsh.h"                 /* fh_vsh_constant_word */

#define PUSH_PUT            0x002309C0u     /* the device's next word */
#define PUSH_END            0x002309C4u     /* the device's end of room */
#define AREA_BYTES          0x10000u
#define AREA_SLACK          0x400u
#define LOGGED_READS        20u

#define METHOD_CONSTANT_LOAD    0x1EA4u     /* NV097_SET_TRANSFORM_CONSTANT_LOAD: the register to load next */
#define METHOD_CONSTANT_FIRST   0x0B80u     /* NV097_SET_TRANSFORM_CONSTANT(0-31): words of the registers */
#define METHOD_CONSTANT_LAST    0x0BFCu

static uint32_t area;               /* the push area's guest address; 0 until the first MakeSpace */
static int reading = -1;            /* FH_PUSH: -1 not looked at yet */
static int logging;
static uint32_t load_register, load_word;
static unsigned long reads, reads_with_words, words_read, constant_words;

static void method_word(uint32_t method, uint32_t value)
{
    if (method == METHOD_CONSTANT_LOAD) {
        load_register = value;
        load_word = 0;
    } else if (method >= METHOD_CONSTANT_FIRST && method <= METHOD_CONSTANT_LAST) {
        fh_vsh_constant_word(load_register, load_word, value);
        constant_words++;
        if (++load_word == 4) {
            load_word = 0;
            load_register++;
        }
    }
}

void fh_push_read(void)
{
    uint32_t put, at;
    unsigned long words_before = words_read;

    if (!area)
        return;
    if (reading < 0) {
        const char *v = getenv("FH_PUSH"), *l = getenv("FH_PUSH_LOG");

        reading = !(v && v[0] == '0' && v[1] == 0);
        logging = l && l[0] && !(l[0] == '0' && l[1] == 0);
    }
    put = MEM32(PUSH_PUT);
    reads++;
    if (put < area || put > area + AREA_BYTES) {    /* moved by something else: start over */
        if (logging)
            fprintf(stderr, "[fh] push: the next word was at 0x%08X, outside the push area 0x%08X: started over\n", put,
                    area);
        MEM32(PUSH_PUT) = area;
        return;
    }
    for (at = area; reading && at + 4 <= put;) {
        uint32_t head = MEM32(at), method = head & 0x1FFCu, count = (head >> 18) & 0x7FFu;
        int same_method = (head & 0x40000000u) != 0;

        if ((head & 3u) != 0 || (head & 0x20000000u) != 0 || head == 0x00020000u) {
            if (logging)
                fprintf(stderr, "[fh] push: a jump, call or return (0x%08X) at 0x%08X: the rest passed over\n", head, at);
            break;
        }
        at += 4;
        words_read++;
        for (uint32_t i = 0; i < count && at + 4 <= put; i++, at += 4) {
            method_word(same_method ? method : method + 4 * i, MEM32(at));
            words_read++;
        }
    }
    if (words_read != words_before) {
        reads_with_words++;
        if (logging && reads_with_words <= LOGGED_READS)
            fprintf(stderr, "[fh] push: read %lu words (0x%08X-0x%08X); the next constant register %u\n",
                    words_read - words_before, area, put, load_register);
    }
    if (logging && reads == 1000)
        fprintf(stderr, "[fh] push: 1000 reads, %lu with words, %lu words, %lu of them constant words\n",
                reads_with_words, words_read, constant_words);
    MEM32(PUSH_PUT) = area;
}

uint32_t fh_push_make_space(void)
{
    if (!area) {
        area = xbox_HeapAlloc(AREA_BYTES, 16);
        if (!area)
            return 0;   /* as the stand-in did */
        MEM32(PUSH_PUT) = area;
        MEM32(PUSH_END) = area + AREA_BYTES - AREA_SLACK;
        fprintf(stderr, "[fh] push: the push area is 0x%08X (%u bytes); the game's writes to it are read before each "
                        "draw\n", area, AREA_BYTES);
        return area;
    }
    fh_push_read();
    return area;
}
