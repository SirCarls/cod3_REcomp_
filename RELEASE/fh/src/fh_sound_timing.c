/* [waw-decomp-fh2] r17 2026-11-24: Finest Hour's DirectSound buffers keep their play time, though nothing is heard yet.
 *
 * Every DirectSound function of the game is still a stand-in (fh/README.md: sound is a later phase), and
 * IDirectSoundBuffer_GetStatus left the game's status at 0, so every sound the game played had ended at once. The game
 * times things by its sounds: its "is it playing" (0x00161EB0: GetStatus, bit 0) keeps a voice line's subtitle up
 * (0x00069F30 sets the HUD's subtitle again every frame while the line's buffer plays, else drops it), and the
 * scripted intros wait for their lines to end. So the subtitles showed for one frame and the intros ran on without
 * waiting. Big Red One's port has the same rule for its streams (bro/src/audio/bro_media.c: "a stream keeps each
 * packet pending and completes it when its play time is over ... which is what the game times movies by").
 *
 * Here each buffer the game makes (IDirectSound_CreateSoundBuffer) gets its own guest block, and its format's byte
 * rate (the WAVEFORMATEX's nAvgBytesPerSec, which the Xbox ADPCM format fills in too) and length
 * (DSBUFFERDESC.dwBufferBytes, then SetBufferData) are kept. Play starts its clock; GetStatus says it plays
 * (DSBSTATUS_PLAYING, and DSBSTATUS_LOOPING for a looping play) until its length has played, or until Stop / StopEx
 * for a looping one; GetCurrentPosition says where it is. Nothing is mixed: this is the timing only, for the day the
 * port plays the game's sound. A buffer with no rate known plays for no time (as before).
 *
 * [waw-decomp-fh2] r18 2026-11-24 (read from 0x00162480-0x001628C4 and 0x001636F0-0x001637B4, after mov19 showed every
 * play as the same 3,072,000 bytes, 32 s): the game makes one buffer over a whole sound bank (CreateSoundBuffer with no
 * length, then SetBufferData on the bank: 3,072,000 bytes), and before each sound sets that sound's own format
 * (0x00235A49, SetFormat: PCM, or Xbox ADPCM 0x69 with its nAvgBytesPerSec worked out at 0x0016268B), its part of the
 * bank (0x00235457, SetPlayRegion(buffer, offset into the bank, length)), a loop part (0x00234A15) and the position in
 * that part (0x00234A71, SetCurrentPosition: 0 at the start, a kept position when a paused sound goes on). So a sound
 * lasts (its part's length - the position) / its own byte rate, not the bank's length / the bank's rate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32 */
#include "xbox_memory_layout.h"     /* XBOX_PTR, xbox_HeapAlloc */
#include "fh_sound_timing.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define MAX_BUFFERS         1024
#define BUFFER_BLOCK_BYTES  0x100u          /* the guest object the game holds (no field of it is read) */
/* DSBUFFERDESC (Xbox): dwSize, dwFlags, dwBufferBytes, lpwfxFormat, ... ; WAVEFORMATEX: wFormatTag, nChannels,
 * nSamplesPerSec, nAvgBytesPerSec, ... */
#define DESC_BUFFER_BYTES   0x08u
#define DESC_FORMAT         0x0Cu
#define FORMAT_BYTE_RATE    0x08u
#define DSBPLAY_LOOPING     0x00000001u
#define DSBSTATUS_PLAYING   0x00000001u
#define DSBSTATUS_LOOPING   0x00000004u
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY       0x8007000Eu
#endif
#define MAX_BYTE_RATE       (48000u * 8u * 4u)  /* more than 8 channels of 32-bit 48 kHz: not a real format */

struct buffer {
    uint32_t handle;                /* the guest block, 0: a free slot (block kept in freed_block for the next) */
    uint32_t freed_block;
    uint32_t bytes;                 /* its length */
    uint32_t byte_rate;             /* bytes a second, 0 if not known */
    uint32_t region_start, region_len;  /* r18: the part SetPlayRegion gives (0 long: from its start to the end) */
    uint32_t start_pos;             /* r18: where in that part Play begins (SetCurrentPosition) */
    uint64_t started_ns;            /* when Play began */
    int playing, looping;
};

static struct buffer buffers[MAX_BUFFERS];
static uint32_t buffer_count;       /* slots used so far */
static uint32_t plays, no_rate_said;

static uint64_t now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER t;

    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

static struct buffer *find(uint32_t handle)
{
    if (!handle)
        return NULL;
    for (uint32_t i = 0; i < buffer_count; i++)
        if (buffers[i].handle == handle)
            return &buffers[i];
    return NULL;
}

/* r18: the bytes the play region holds, and those left from the play position */
static uint32_t span_bytes(const struct buffer *b)
{
    if (b->region_len)
        return b->region_len;
    return b->bytes > b->region_start ? b->bytes - b->region_start : 0;
}

static uint32_t left_bytes(const struct buffer *b)
{
    const uint32_t span = span_bytes(b);

    return span > b->start_pos ? span - b->start_pos : 0;
}

/* how far a playing buffer has gone since Play (bytes); with no rate known, all it had left */
static uint64_t played_bytes(const struct buffer *b, uint64_t now)
{
    if (!b->byte_rate || now <= b->started_ns)
        return b->byte_rate ? 0 : left_bytes(b);
    return (now - b->started_ns) / 1000ull * b->byte_rate / 1000000ull;
}

static int still_playing(struct buffer *b)
{
    if (!b->playing)
        return 0;
    if (b->looping)
        return b->byte_rate && span_bytes(b) ? 1 : 0;
    if (played_bytes(b, now_ns()) >= left_bytes(b))
        b->playing = 0;
    return b->playing;
}

uint32_t fh_sound_create_buffer(uint32_t dsound, uint32_t desc, uint32_t out_buffer)
{
    struct buffer *b = NULL;
    uint32_t format;

    (void)dsound;
    for (uint32_t i = 0; i < buffer_count && !b; i++)
        if (!buffers[i].handle)
            b = &buffers[i];
    if (!b && buffer_count < MAX_BUFFERS)
        b = &buffers[buffer_count++];
    if (!b) {
        fprintf(stderr, "[sound] more than %u sound buffers at once: this one keeps no time\n", MAX_BUFFERS);
        return 0;           /* the stand-in's own block stays in *out_buffer */
    }
    {
        uint32_t block = b->freed_block ? b->freed_block : xbox_HeapAlloc(BUFFER_BLOCK_BYTES, 16);

        if (!block)
            return E_OUTOFMEMORY;
        memset((void *)XBOX_PTR(block), 0, BUFFER_BLOCK_BYTES);
        memset(b, 0, sizeof *b);
        b->handle = block;
    }
    if (desc) {
        b->bytes = MEM32(desc + DESC_BUFFER_BYTES);
        format = MEM32(desc + DESC_FORMAT);
        if (format) {
            b->byte_rate = MEM32(format + FORMAT_BYTE_RATE);
            if (b->byte_rate > MAX_BYTE_RATE)
                b->byte_rate = 0;
        }
    }
    if (!b->byte_rate && !no_rate_said++)
        fprintf(stderr, "[sound] a sound buffer with no byte rate in its format: it plays for no time\n");
    if (out_buffer)
        MEM32(out_buffer) = b->handle;
    return 0;
}

uint32_t fh_sound_set_buffer_data(uint32_t buffer, uint32_t data, uint32_t bytes)
{
    struct buffer *b = find(buffer);

    (void)data;
    if (b) {
        b->bytes = bytes;
        b->region_start = b->region_len = b->start_pos = 0;     /* r18: new data, the whole of it */
    }
    return 0;
}

/* r18: SetFormat (0x00235A49): the sound's own format, and so its byte rate */
uint32_t fh_sound_set_format(uint32_t buffer, uint32_t format)
{
    struct buffer *b = find(buffer);

    if (b && format) {
        b->byte_rate = MEM32(format + FORMAT_BYTE_RATE);
        if (b->byte_rate > MAX_BYTE_RATE)
            b->byte_rate = 0;
    }
    return 0;
}

/* r18: SetPlayRegion (0x00235457): the part of the buffer a play covers; 0 long is to the buffer's end */
uint32_t fh_sound_set_play_region(uint32_t buffer, uint32_t start, uint32_t length)
{
    struct buffer *b = find(buffer);

    if (b) {
        b->region_start = !b->bytes || start < b->bytes ? start : 0;
        b->region_len = !b->bytes || (uint64_t)b->region_start + length <= b->bytes ? length
                                                                                  : b->bytes - b->region_start;
        b->start_pos = 0;
    }
    return 0;
}

/* r18: SetCurrentPosition (0x00234A71): where in the play region the next play starts (or, playing, from now) */
uint32_t fh_sound_set_position(uint32_t buffer, uint32_t position)
{
    struct buffer *b = find(buffer);

    if (b) {
        b->start_pos = position;
        if (b->playing)
            b->started_ns = now_ns();
    }
    return 0;
}

uint32_t fh_sound_play(uint32_t buffer, uint32_t flags)
{
    struct buffer *b = find(buffer);

    if (b) {
        b->started_ns = now_ns();
        b->playing = 1;
        b->looping = (flags & DSBPLAY_LOOPING) != 0;
        static int log_plays = -1;

        if (log_plays < 0) {
            const char *v = getenv("FH_SOUND_LOG");     /* each of the first 60 plays, with its length */

            log_plays = v && v[0] && v[0] != '0';
        }
        if (!plays++)
            fprintf(stderr, "[sound] sound buffers keep their play time (nothing is heard yet): the game's subtitles "
                            "and scripted scenes wait for their lines\n");
        if (log_plays && plays <= 60)
            fprintf(stderr, "[sound] play %u: buffer 0x%08X, %u of its %u bytes from %u (+%u) at %u a second "
                    "(%.2f s)%s\n", plays, buffer, left_bytes(b), b->bytes, b->region_start, b->start_pos,
                    b->byte_rate, b->byte_rate ? (double)left_bytes(b) / b->byte_rate : 0.0,
                    b->looping ? ", looping" : "");
    }
    return 0;
}

uint32_t fh_sound_stop(uint32_t buffer)
{
    struct buffer *b = find(buffer);

    if (b)
        b->playing = 0;
    return 0;
}

uint32_t fh_sound_get_status(uint32_t buffer, uint32_t out_status)
{
    struct buffer *b = find(buffer);
    uint32_t status = 0;

    if (b && still_playing(b))
        status = DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0);
    if (out_status)
        MEM32(out_status) = status;
    return 0;
}

uint32_t fh_sound_get_position(uint32_t buffer, uint32_t out_play, uint32_t out_write)
{
    struct buffer *b = find(buffer);
    uint32_t at = 0;

    if (b && span_bytes(b) && still_playing(b)) {
        const uint32_t span = span_bytes(b);
        const uint64_t p = b->start_pos + played_bytes(b, now_ns());   /* r18: in the play region, from its start */

        at = (uint32_t)(b->looping ? p % span : (p < span ? p : span - 1));
    }
    if (out_play)
        MEM32(out_play) = at;
    if (out_write)
        MEM32(out_write) = at;
    return 0;
}

uint32_t fh_sound_release(uint32_t buffer)
{
    struct buffer *b = find(buffer);

    if (b) {
        b->freed_block = b->handle;
        b->handle = 0;
        b->playing = 0;
    }
    return 0;
}
