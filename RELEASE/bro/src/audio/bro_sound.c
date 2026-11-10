/* [acct2-bro] Big Red One port: DirectSound buffers and the mixer, native.
 *
 * The game's sound effects are DirectSound buffers: DirectSoundCreateBuffer, then SetFormat / SetBufferData to
 * point the buffer at sound data the game keeps itself (PCM or Xbox ADPCM), then Play / PlayEx with or without
 * looping, Stop / StopEx, GetStatus, SetVolume, SetFrequency, SetCurrentPosition and the play and loop regions.
 * On the Xbox the library hands all of that to the APU; here every one of those functions is a stand-in
 * (config/stand_in_functions.tsv) that calls this file, and the buffers are voices of a small software mixer.
 *
 * - A buffer is a 0x20-byte block in guest memory ("BROS", its voice number), so the game's pointer is real.
 * - The voices read the game's own sound data while they play, so a buffer the game keeps writing into (the
 *   movies' sound) plays what it writes.
 * - Streams (bro_media.c) hand their packets here as they are queued; those are decoded at once into a ring.
 * - Output is SDL3 when the build has it (the SDL CoD3's SDK builds): 48 kHz stereo, float, mixed on SDL's audio
 *   thread under its stream's lock. Without SDL3, or when no device opens, DirectSoundDoWork moves the voices on by the time that has passed, so
 *   GetStatus still says when a sound has ended.
 * - BRO_SOUND_MUTE=1 sends silence to the device (test runs); BRO_SOUND_WAV=<file> also writes the mix to a WAV
 *   file, so a test can check that something played; BRO_SOUND_TRACE=1 logs the first few hundred buffer calls.
 * - A voice's level is its volume times its headroom (SetHeadroom; a new buffer starts at 600, 6 dB, taken to be
 *   the library's default: the game only sets it on a few buffers). A voice the game places in 3D
 *   (SetAllParameters, SetMode) is also turned down with distance (min distance / distance, as DirectSound does,
 *   no quieter past the max distance) and panned left and right from the listener (IDirectSound_SetPosition and
 *   SetOrientation). The mix goes through a soft limiter above 0.8 so many loud voices do not clip.
 * Not yet: mix bins (only logged), cones, Doppler, reverb and envelopes. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc */
#include "bro_sound.h"

#if defined(BRO_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#define S_OK                0x00000000u
#define E_OUTOFMEMORY       0x8007000Eu
#define E_INVALIDARG        0x80070057u

#define BUFFER_MAGIC        0x534F5242u     /* "BROS" in guest memory */
#define BUFFER_BYTES        0x20u
#define MAX_VOICES          512
#define MAX_STREAMS         32
#define OUTPUT_RATE         48000
#define STREAM_RING_FRAMES  (OUTPUT_RATE * 4)     /* a stream's first ring, frames at its own rate */
#define STREAM_RING_MOST    (OUTPUT_RATE * 64)    /* how far it grows: the game queues two 5.5 s movie packets */

#define DSBPLAY_LOOPING     0x00000001u
#define DSBSTATUS_PLAYING   0x00000001u
#define DSBSTATUS_PAUSED    0x00000002u
#define DSBSTATUS_LOOPING   0x00000004u
#define DSBSTOPEX_RELEASEWAVEFORM 0x00000002u

#define DS3DMODE_NORMAL         0u
#define DS3DMODE_HEADRELATIVE   1u
#define DS3DMODE_DISABLE        2u
#define NOT_PLACED              -1          /* a voice the game never put in 3D */
#define DEFAULT_HEADROOM        600u        /* hundredths of a decibel */
#define LIMIT_KNEE              0.8f
#define REPORT_FRAMES           (OUTPUT_RATE * 10)
#define MAX_REPORTS             30
#define MAX_TRACE               400

#define WAVE_FORMAT_PCM         0x0001u
#define WAVE_FORMAT_XBOX_ADPCM  0x0069u
#define ADPCM_BLOCK_BYTES       36u     /* a channel's share of a block */
#define ADPCM_BLOCK_FRAMES      64u

struct format {
    uint16_t tag, channels, block_align, bits;
    uint32_t rate;
};

struct voice {
    int used;
    uint32_t object;
    struct format format;
    uint32_t data, bytes;                   /* the game's sound data */
    uint32_t play_start, play_length;       /* frames; length 0: to the end */
    uint32_t loop_start, loop_length;       /* frames; length 0: the play region */
    double position;                        /* frames from the start of the data */
    int playing, looping, paused, release;  /* release: stop looping and play out */
    float gain;                             /* SetVolume */
    float headroom;                         /* SetHeadroom, as a gain */
    uint32_t frequency;                     /* 0: the format's */
    int32_t cached_block;                   /* ADPCM: the block decoded into cache */
    int16_t cache[2][ADPCM_BLOCK_FRAMES];
    /* 3D */
    int mode;                               /* DS3DMODE_*, or NOT_PLACED */
    float where[3], min_distance, max_distance;
    float left, right;                      /* distance and pan, from place() */
};

struct stream_voice {
    float (*ring)[2];                       /* capacity frames, made and grown as packets come */
    uint32_t capacity;
    uint32_t head, count;                   /* frames */
    double fraction;
    uint32_t rate;
    float gain;                             /* IDirectSoundStream_SetVolume */
    float headroom;                         /* IDirectSoundStream_SetHeadroom, as a gain (0 dB until set) */
    float peak;                             /* the loudest sample queued, for the log */
    unsigned long packets, dropped;         /* dropped: frames that did not fit the ring */
};

static struct voice voices[MAX_VOICES];
static struct stream_voice *streams[MAX_STREAMS];
static int started, muted;
static FILE *wav;
static uint32_t wav_frames;
static double last_pump;
static unsigned long buffers_made, plays;
static int tracing;
static unsigned long traced;

/* the listener (IDirectSound_SetPosition / SetOrientation), DirectSound's left-handed axes */
static float listener[3], listener_front[3] = {0.0f, 0.0f, 1.0f}, listener_top[3] = {0.0f, 1.0f, 0.0f};

/* what the last 10 s of mixing looked like, for the report */
static uint32_t report_frames;
static float report_peak;
static unsigned long report_limited, reports;

#if defined(BRO_HAVE_SDL3)
static SDL_AudioStream *device;     /* the output: a stream on the default playback device, fed by audio_callback */
#define LOCK()   do { if (device) SDL_LockAudioStream(device); } while (0)
#define UNLOCK() do { if (device) SDL_UnlockAudioStream(device); } while (0)
#else
#define LOCK()   do { } while (0)
#define UNLOCK() do { } while (0)
#endif

static double now_seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static float as_float(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof f);
    return isfinite(f) ? f : 0.0f;
}

/* hundredths of a decibel (0 or less) as a gain */
static float gain_of(int32_t hundredths)
{
    if (hundredths > 0)
        hundredths = 0;
    return hundredths <= -10000 ? 0.0f : powf(10.0f, hundredths / 2000.0f);
}

/* BRO_SOUND_TRACE=1: the first MAX_TRACE buffer calls */
static void trace(const char *format, ...)
{
    va_list args;

    if (!tracing || traced >= MAX_TRACE)
        return;
    traced++;
    va_start(args, format);
    fputs("[sound] trace ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* ---- formats and sample reading ---- */

static int read_format(uint32_t address, struct format *f)
{
    if (!address)
        return 0;
    f->tag = MEM16(address);
    f->channels = MEM16(address + 2);
    f->rate = MEM32(address + 4);
    f->block_align = MEM16(address + 12);
    f->bits = MEM16(address + 14);
    if (f->channels < 1 || f->channels > 2 || f->rate < 1000 || f->rate > 96000)
        return 0;
    if (f->tag == WAVE_FORMAT_PCM)
        return (f->bits == 8 || f->bits == 16) && f->block_align == f->channels * f->bits / 8;
    return f->tag == WAVE_FORMAT_XBOX_ADPCM;
}

/* frames in `bytes` bytes of the format */
static uint32_t frames_of(const struct format *f, uint32_t bytes)
{
    if (f->tag == WAVE_FORMAT_XBOX_ADPCM)
        return bytes / (ADPCM_BLOCK_BYTES * f->channels) * ADPCM_BLOCK_FRAMES;
    return f->block_align ? bytes / f->block_align : 0;
}

static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

/* One Xbox ADPCM block (36 bytes a channel: a 4-byte header {first sample, step index}, then 4-byte groups of 8
 * nibbles, the channels' groups in turn) into 64 frames: the header's sample, then 63 nibbles. */
static void decode_adpcm_block(const uint8_t *block, int channels, int16_t out[2][ADPCM_BLOCK_FRAMES])
{
    int c;

    for (c = 0; c < channels; c++) {
        const uint8_t *header = block + 4 * c;
        int predictor = (int16_t)(header[0] | header[1] << 8);
        int index = header[2] > 88 ? 88 : header[2];
        int n;

        out[c][0] = (int16_t)predictor;
        for (n = 1; n < (int)ADPCM_BLOCK_FRAMES; n++) {
            int nibble_number = n - 1;
            const uint8_t *group = block + 4 * channels + 4 * (channels * (nibble_number / 8) + c);
            int byte = group[(nibble_number % 8) / 2];
            int nibble = nibble_number % 2 ? byte >> 4 : byte & 15;
            int step = step_table[index], diff = step >> 3;

            if (nibble & 1)
                diff += step >> 2;
            if (nibble & 2)
                diff += step >> 1;
            if (nibble & 4)
                diff += step;
            predictor += nibble & 8 ? -diff : diff;
            predictor = predictor < -32768 ? -32768 : predictor > 32767 ? 32767 : predictor;
            index += index_table[nibble];
            index = index < 0 ? 0 : index > 88 ? 88 : index;
            out[c][n] = (int16_t)predictor;
        }
    }
}

/* the frame's two channels (a mono voice gives the same to both), -1..1 */
static void read_frame(struct voice *v, uint32_t frame, float *left, float *right)
{
    const struct format *f = &v->format;
    float l = 0.0f, r;

    if (f->tag == WAVE_FORMAT_XBOX_ADPCM) {
        int32_t block = (int32_t)(frame / ADPCM_BLOCK_FRAMES);

        if (block != v->cached_block) {
            uint32_t at = v->data + (uint32_t)block * ADPCM_BLOCK_BYTES * f->channels;

            decode_adpcm_block((const uint8_t *)(uintptr_t)XBOX_PTR(at), f->channels, v->cache);
            v->cached_block = block;
        }
        l = v->cache[0][frame % ADPCM_BLOCK_FRAMES] / 32768.0f;
        r = f->channels == 2 ? v->cache[1][frame % ADPCM_BLOCK_FRAMES] / 32768.0f : l;
    } else if (f->bits == 16) {
        uint32_t at = v->data + frame * f->block_align;

        l = (int16_t)MEM16(at) / 32768.0f;
        r = f->channels == 2 ? (int16_t)MEM16(at + 2) / 32768.0f : l;
    } else {
        uint32_t at = v->data + frame * f->block_align;

        l = (MEM8(at) - 128) / 128.0f;
        r = f->channels == 2 ? (MEM8(at + 1) - 128) / 128.0f : l;
    }
    *left = l;
    *right = r;
}

/* ---- the mixer ---- */

static uint32_t voice_frames(const struct voice *v)
{
    return v->data ? frames_of(&v->format, v->bytes) : 0;
}

/* A 3D voice's two speaker gains from where it is and where the listener is: min distance / distance past the min
 * distance (no quieter past the max distance), then an equal-power pan on the listener's right axis. A voice that
 * is not placed, or has 3D turned off, plays in both speakers as it is. Called under the lock. */
static void place(struct voice *v)
{
    float away[3], right[3], distance, near_distance, far_distance, fade = 1.0f, pan = 0.0f, angle;
    const float *front = listener_front, *top = listener_top;
    int k;

    if (v->mode == NOT_PLACED || v->mode == (int)DS3DMODE_DISABLE) {
        v->left = v->right = 1.0f;
        return;
    }
    for (k = 0; k < 3; k++)
        away[k] = v->mode == (int)DS3DMODE_HEADRELATIVE ? v->where[k] : v->where[k] - listener[k];
    if (v->mode == (int)DS3DMODE_HEADRELATIVE) {
        static const float ahead[3] = {0.0f, 0.0f, 1.0f}, up[3] = {0.0f, 1.0f, 0.0f};

        front = ahead;
        top = up;
    }
    /* right = top x front (left-handed: front +z and top +y give right +x) */
    right[0] = top[1] * front[2] - top[2] * front[1];
    right[1] = top[2] * front[0] - top[0] * front[2];
    right[2] = top[0] * front[1] - top[1] * front[0];
    distance = sqrtf(dot3(away, away));
    near_distance = v->min_distance > 0.0f ? v->min_distance : 1.0f;
    far_distance = v->max_distance > near_distance ? v->max_distance : 0.0f;
    if (distance > near_distance)
        fade = near_distance / (far_distance && distance > far_distance ? far_distance : distance);
    if (distance > 1e-4f) {
        float length = sqrtf(dot3(right, right));

        if (length > 1e-6f)
            pan = dot3(away, right) / (distance * length);
    }
    pan = pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan;
    angle = (pan + 1.0f) * 0.78539816f;     /* 0 .. pi/2 */
    v->left = fade * cosf(angle);
    v->right = fade * sinf(angle);
}

static void place_all(void)
{
    int i;

    for (i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].mode != NOT_PLACED)
            place(&voices[i]);
}

static float soft_limit(float x)
{
    float a = fabsf(x);

    if (a <= LIMIT_KNEE)
        return x;
    a = LIMIT_KNEE + (1.0f - LIMIT_KNEE) * tanhf((a - LIMIT_KNEE) / (1.0f - LIMIT_KNEE));
    return x < 0.0f ? -a : a;
}

static void mix_voice(struct voice *v, float *out, int frames)
{
    uint32_t total = voice_frames(v);
    uint32_t end = v->play_length ? v->play_start + v->play_length : total;
    uint32_t loop_start = v->loop_length ? v->loop_start : v->play_start;
    uint32_t loop_end = v->loop_length ? v->loop_start + v->loop_length : end;
    double step = (double)(v->frequency ? v->frequency : v->format.rate) / OUTPUT_RATE;
    float level = v->gain * v->headroom, left = level * v->left, right = level * v->right;
    int mono = v->mode != NOT_PLACED && v->mode != (int)DS3DMODE_DISABLE;     /* 3D: one source, panned */
    int i;

    if (end > total)
        end = total;
    if (loop_end > total)
        loop_end = total;
    for (i = 0; i < frames; i++) {
        uint32_t frame = (uint32_t)v->position;
        uint32_t stop = v->looping && !v->release ? loop_end : end;
        float l, r;

        if (frame >= stop) {
            if (v->looping && !v->release && loop_end > loop_start) {
                v->position = loop_start + (v->position - stop);
                frame = (uint32_t)v->position;
                if (frame >= loop_end)
                    frame = loop_start;
            } else {
                v->playing = 0;
                v->position = v->play_start;
                return;
            }
        }
        if (out) {
            read_frame(v, frame, &l, &r);
            if (mono)
                l = r = 0.5f * (l + r);
            out[2 * i] += l * left;
            out[2 * i + 1] += r * right;
        }
        v->position += step;
    }
}

static void mix_stream(struct stream_voice *s, float *out, int frames)
{
    double step = (double)(s->rate ? s->rate : OUTPUT_RATE) / OUTPUT_RATE;
    int i;

    for (i = 0; i < frames && s->count; i++) {
        if (out) {
            out[2 * i] += s->ring[s->head][0] * s->gain * s->headroom;
            out[2 * i + 1] += s->ring[s->head][1] * s->gain * s->headroom;
        }
        s->fraction += step;
        while (s->fraction >= 1.0 && s->count) {
            s->fraction -= 1.0;
            s->head = (s->head + 1) % s->capacity;
            s->count--;
        }
    }
}

static void write_wav_header(uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};

    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, wav);
    fseek(wav, 0, SEEK_END);
}

/* Mixes `frames` frames of every playing voice into out (stereo float, cleared here); out may be NULL to only
 * move the voices on. */
static void mix(float *out, int frames)
{
    int i;

    if (out)
        memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    for (i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].playing && !voices[i].paused)
            mix_voice(&voices[i], out, frames);
    for (i = 0; i < MAX_STREAMS; i++)
        if (streams[i])
            mix_stream(streams[i], out, frames);
    if (out) {
        for (i = 0; i < 2 * frames; i++) {
            float a = fabsf(out[i]);

            if (a > report_peak)
                report_peak = a;
            if (a > LIMIT_KNEE)
                report_limited++;
            out[i] = soft_limit(out[i]);
        }
    }
    report_frames += (uint32_t)frames;
    if (report_frames >= REPORT_FRAMES) {
        if (reports++ < MAX_REPORTS) {
            int playing = 0, placed = 0, flowing = 0;
            unsigned long dropped = 0;

            for (i = 0; i < MAX_VOICES; i++)
                if (voices[i].used && voices[i].playing && !voices[i].paused) {
                    playing++;
                    placed += voices[i].mode != NOT_PLACED && voices[i].mode != (int)DS3DMODE_DISABLE;
                }
            for (i = 0; i < MAX_STREAMS; i++)
                if (streams[i]) {
                    flowing += streams[i]->count > 0;
                    dropped += streams[i]->dropped;
                }
            fprintf(stderr, "[sound] mix: %d voice(s) playing (%d in 3D), %d stream(s) with sound queued (%lu frames "
                    "dropped so far), peak %.2f, %.2f%% of samples limited\n", playing, placed, flowing, dropped,
                    report_peak, 100.0 * report_limited / (2.0 * report_frames));
        }
        report_frames = 0;
        report_peak = 0.0f;
        report_limited = 0;
    }
    if (wav && out) {
        int16_t pcm[2 * 1024];
        int done = 0;

        while (done < frames) {
            int n = frames - done > 1024 ? 1024 : frames - done, k;

            for (k = 0; k < 2 * n; k++) {
                float s = out[2 * done + k];

                pcm[k] = (int16_t)(s > 1.0f ? 32767 : s < -1.0f ? -32768 : s * 32767.0f);
            }
            fwrite(pcm, 4, (size_t)n, wav);
            done += n;
        }
        wav_frames += (uint32_t)frames;
        if (wav_frames % OUTPUT_RATE < (uint32_t)frames)
            write_wav_header(wav_frames);   /* about once a second, so the file is whole if the game is killed */
    }
}

#if defined(BRO_HAVE_SDL3)
/* SDL asks for more sound: mix that much (8 bytes a stereo frame) and hand it over. SDL holds the stream's lock. */
static void audio_callback(void *user, SDL_AudioStream *stream, int wanted, int total)
{
    static float chunk[2 * 1024];
    const int most = (int)(sizeof chunk / 8);

    (void)user;
    (void)total;
    while (wanted > 0) {
        int frames = wanted / 8 < most ? wanted / 8 : most;

        if (frames <= 0)
            frames = 1;
        mix(chunk, frames);
        if (muted)
            memset(chunk, 0, (size_t)frames * 8);
        SDL_PutAudioStreamData(stream, chunk, frames * 8);
        wanted -= frames * 8;
    }
}
#endif

static void start(void)
{
    const char *path = getenv("BRO_SOUND_WAV");

    if (started)
        return;
    started = 1;
    muted = getenv("BRO_SOUND_MUTE") && getenv("BRO_SOUND_MUTE")[0] == '1';
    tracing = getenv("BRO_SOUND_TRACE") && getenv("BRO_SOUND_TRACE")[0] == '1';
    if (path && path[0] && (wav = fopen(path, "wb")) != NULL)
        write_wav_header(0);
#if defined(BRO_HAVE_SDL3)
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, OUTPUT_RATE };

        device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
        if (device)
            SDL_ResumeAudioStreamDevice(device);
    }
    fprintf(stderr, "[sound] output: %s%s%s\n", device ? "SDL3 device opened" : "no device (voices kept by the clock)",
            muted ? ", muted" : "", wav ? ", also to the WAV file" : "");
#else
    fprintf(stderr, "[sound] output: none in this build (voices kept by the clock)%s%s\n", muted ? ", muted" : "",
            wav ? ", mix to the WAV file" : "");
#endif
    last_pump = now_seconds();
}

void bro_sound_pump(void)
{
    double now;
    int frames;

    start();
#if defined(BRO_HAVE_SDL3)
    if (device)
        return;
#endif
    now = now_seconds();
    frames = (int)((now - last_pump) * OUTPUT_RATE);
    if (frames <= 0)
        return;
    if (frames > OUTPUT_RATE)
        frames = OUTPUT_RATE;
    last_pump += (double)frames / OUTPUT_RATE;
    if (wav) {
        static float scratch[2 * OUTPUT_RATE];

        mix(scratch, frames);
    } else {
        mix(NULL, frames);
    }
}

/* ---- buffers ---- */

static struct voice *voice_of(uint32_t object)
{
    uint32_t n;

    if (!object || MEM32(object) != BUFFER_MAGIC)
        return NULL;
    n = MEM32(object + 4);
    return n < MAX_VOICES && voices[n].used && voices[n].object == object ? &voices[n] : NULL;
}

/* DSBUFFERDESC {size, flags, buffer bytes, format, mix bins, input mix bin}; some layouts put a reserved word before
 * the format, so the format is taken from +0x0C, else +0x10, whichever is a WAVEFORMATEX this mixer reads. */
uint32_t bro_sound_create_buffer(uint32_t desc, uint32_t out)
{
    struct format f;
    uint32_t object, bytes;
    int n;

    start();
    if (!desc || !out)
        return E_INVALIDARG;
    for (n = 0; n < MAX_VOICES && voices[n].used; n++)
        ;
    if (n == MAX_VOICES || !(object = xbox_HeapAlloc(BUFFER_BYTES, 16)))
        return E_OUTOFMEMORY;
    memset(&voices[n], 0, sizeof voices[n]);
    if (!read_format(MEM32(desc + 0x0C), &f) && !read_format(MEM32(desc + 0x10), &f))
        memset(&f, 0, sizeof f);
    bytes = MEM32(desc + 8);
    LOCK();
    voices[n].used = 1;
    voices[n].object = object;
    voices[n].format = f;
    voices[n].gain = 1.0f;
    voices[n].headroom = gain_of(-(int32_t)DEFAULT_HEADROOM);
    voices[n].mode = NOT_PLACED;
    voices[n].left = voices[n].right = 1.0f;
    voices[n].cached_block = -1;
    if (bytes && bytes < (64u << 20) && (voices[n].data = xbox_HeapAlloc(bytes, 16)) != 0) {
        memset((void *)(uintptr_t)XBOX_PTR(voices[n].data), f.tag == WAVE_FORMAT_PCM && f.bits == 8 ? 128 : 0,
               bytes);
        voices[n].bytes = bytes;
    }
    UNLOCK();
    MEM32(object + 0) = BUFFER_MAGIC;
    MEM32(object + 4) = (uint32_t)n;
    MEM32(out) = object;
    if (buffers_made++ < 8)
        fprintf(stderr, "[sound] buffer %d (0x%08X): format %u, %u channel(s), %u Hz, %u bytes of its own\n", n, object,
                f.tag, f.channels, f.rate, bytes);
    return S_OK;
}

uint32_t bro_sound_buffer_set_format(uint32_t buffer, uint32_t format)
{
    struct voice *v = voice_of(buffer);
    struct format f;

    if (!v)
        return E_INVALIDARG;
    if (!read_format(format, &f))
        return S_OK;
    LOCK();
    v->format = f;
    v->cached_block = -1;
    UNLOCK();
    return S_OK;
}

uint32_t bro_sound_buffer_set_data(uint32_t buffer, uint32_t data, uint32_t bytes)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->data = data;
    v->bytes = data ? bytes : 0;
    v->play_start = v->play_length = v->loop_start = v->loop_length = 0;
    v->position = 0.0;
    v->cached_block = -1;
    if (!v->data)
        v->playing = 0;
    UNLOCK();
    trace("v%u data %08X, %u bytes", MEM32(buffer + 4), data, bytes);
    return S_OK;
}

uint32_t bro_sound_buffer_play(uint32_t buffer, uint32_t flags)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->looping = (flags & DSBPLAY_LOOPING) != 0;
    v->release = 0;
    v->paused = 0;
    v->playing = v->data != 0 && v->format.rate != 0;
    UNLOCK();
    trace("v%u play%s at sample %u (region %u+%u), volume %.3f, headroom %.3f, 3D %d", MEM32(buffer + 4),
          v->looping ? " looping" : "", (uint32_t)v->position, v->play_start, v->play_length, v->gain, v->headroom,
          v->mode);
    if (plays++ < 8)
        fprintf(stderr, "[sound] play buffer %u: %u bytes, format %u, %u Hz%s\n", MEM32(buffer + 4), v->bytes,
                v->format.tag, v->format.rate, v->looping ? ", looping" : "");
    return S_OK;
}

uint32_t bro_sound_buffer_stop(uint32_t buffer, uint32_t flags)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    if (flags & DSBSTOPEX_RELEASEWAVEFORM)
        v->release = 1;     /* play out to the end of the region, without looping */
    else
        v->playing = 0;
    UNLOCK();
    trace("v%u stop%s at sample %u", MEM32(buffer + 4), flags & DSBSTOPEX_RELEASEWAVEFORM ? " (release)" : "",
          (uint32_t)v->position);
    return S_OK;
}

uint32_t bro_sound_buffer_pause(uint32_t buffer, uint32_t pause)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->paused = pause == 1;     /* DSBPAUSE_PAUSE */
    UNLOCK();
    return S_OK;
}

uint32_t bro_sound_buffer_get_status(uint32_t buffer, uint32_t out)
{
    struct voice *v = voice_of(buffer);
    uint32_t status = 0;

    bro_sound_pump();
    if (v) {
        LOCK();
        if (v->playing)
            status = DSBSTATUS_PLAYING | (v->looping && !v->release ? DSBSTATUS_LOOPING : 0) |
                     (v->paused ? DSBSTATUS_PAUSED : 0);
        UNLOCK();
    }
    if (out)
        MEM32(out) = status;
    return v ? S_OK : E_INVALIDARG;
}

uint32_t bro_sound_buffer_set_position(uint32_t buffer, uint32_t offset)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->position = frames_of(&v->format, offset);
    UNLOCK();
    trace("v%u position %u bytes", MEM32(buffer + 4), offset);
    return S_OK;
}

/* hundredths of a decibel, 0 down to -10000 */
uint32_t bro_sound_buffer_set_volume(uint32_t buffer, uint32_t volume)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->gain = gain_of((int32_t)volume);
    UNLOCK();
    trace("v%u volume %d", MEM32(buffer + 4), (int32_t)volume);
    return S_OK;
}

uint32_t bro_sound_buffer_set_frequency(uint32_t buffer, uint32_t hertz)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->frequency = hertz > 200000 ? 0 : hertz;
    UNLOCK();
    return S_OK;
}

uint32_t bro_sound_buffer_set_loop_region(uint32_t buffer, uint32_t start, uint32_t length)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->loop_start = frames_of(&v->format, start);
    v->loop_length = frames_of(&v->format, length);
    UNLOCK();
    return S_OK;
}

uint32_t bro_sound_buffer_set_play_region(uint32_t buffer, uint32_t start, uint32_t length)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->play_start = frames_of(&v->format, start);
    v->play_length = frames_of(&v->format, length);
    /* A stopped voice given a new region starts it from its beginning; before, a region earlier in the data than
     * the last one kept the old position and played the wrong sound, or nothing. */
    if (!v->playing || v->position < v->play_start ||
        (v->play_length && v->position >= (double)v->play_start + v->play_length))
        v->position = v->play_start;
    UNLOCK();
    trace("v%u region %u+%u bytes", MEM32(buffer + 4), start, length);
    return S_OK;
}

/* hundredths of a decibel of attenuation, 0 to 10000 */
uint32_t bro_sound_buffer_set_headroom(uint32_t buffer, uint32_t headroom)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->headroom = gain_of(-(int32_t)(headroom > 10000 ? 10000 : headroom));
    UNLOCK();
    trace("v%u headroom %u", MEM32(buffer + 4), headroom);
    return S_OK;
}

/* DSMIXBINS {count, pairs}; a pair is {mix bin, volume}. Logged only for now. */
uint32_t bro_sound_buffer_set_mix_bins(uint32_t buffer, uint32_t bins)
{
    static int shown;
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    if (bins && shown < 12) {
        uint32_t count = MEM32(bins), pairs = MEM32(bins + 4), k;
        char text[256] = "";
        size_t at = 0;

        shown++;
        for (k = 0; k < count && k < 8 && pairs && at < sizeof text - 32; k++)
            at += (size_t)snprintf(text + at, sizeof text - at, " %u:%d", MEM32(pairs + 8 * k),
                                   (int32_t)MEM32(pairs + 8 * k + 4));
        fprintf(stderr, "[sound] mix bins of buffer %u: %u bin(s)%s\n", MEM32(buffer + 4), count, text);
    }
    return S_OK;
}

/* DS3DBUFFER {size, position, velocity, inside and outside cone angles, cone orientation, cone outside volume,
 * min distance, max distance, mode, ...} */
uint32_t bro_sound_buffer_set_3d(uint32_t buffer, uint32_t params)
{
    static int shown;
    struct voice *v = voice_of(buffer);
    uint32_t size;

    if (!v)
        return E_INVALIDARG;
    if (!params || (size = MEM32(params)) < 64) {
        if (shown++ < 8)
            fprintf(stderr, "[sound] 3D buffer %u: parameters of size %u, not read\n", MEM32(buffer + 4),
                    params ? MEM32(params) : 0);
        return S_OK;
    }
    LOCK();
    v->where[0] = as_float(MEM32(params + 4));
    v->where[1] = as_float(MEM32(params + 8));
    v->where[2] = as_float(MEM32(params + 12));
    v->min_distance = as_float(MEM32(params + 52));
    v->max_distance = as_float(MEM32(params + 56));
    v->mode = MEM32(params + 60) <= DS3DMODE_DISABLE ? (int)MEM32(params + 60) : (int)DS3DMODE_NORMAL;
    place(v);
    UNLOCK();
    if (shown++ < 8)
        fprintf(stderr, "[sound] 3D buffer %u: size %u, at %.1f %.1f %.1f, distance %.1f..%.1f, mode %d -> %.2f %.2f\n",
                MEM32(buffer + 4), size, v->where[0], v->where[1], v->where[2], v->min_distance, v->max_distance,
                v->mode, v->left, v->right);
    trace("v%u 3D at %.1f %.1f %.1f, %.1f..%.1f, mode %d", MEM32(buffer + 4), v->where[0], v->where[1], v->where[2],
          v->min_distance, v->max_distance, v->mode);
    return S_OK;
}

uint32_t bro_sound_buffer_set_mode(uint32_t buffer, uint32_t mode)
{
    struct voice *v = voice_of(buffer);

    if (!v)
        return E_INVALIDARG;
    LOCK();
    v->mode = mode <= DS3DMODE_DISABLE ? (int)mode : (int)DS3DMODE_NORMAL;
    place(v);
    UNLOCK();
    trace("v%u 3D mode %u", MEM32(buffer + 4), mode);
    return S_OK;
}

/* ---- the listener ---- */

void bro_sound_listener_position(uint32_t x, uint32_t y, uint32_t z)
{
    static int shown;

    LOCK();
    listener[0] = as_float(x);
    listener[1] = as_float(y);
    listener[2] = as_float(z);
    place_all();
    UNLOCK();
    if (shown++ < 3)
        fprintf(stderr, "[sound] listener at %.1f %.1f %.1f\n", listener[0], listener[1], listener[2]);
}

void bro_sound_listener_orientation(uint32_t front_x, uint32_t front_y, uint32_t front_z, uint32_t top_x,
                                    uint32_t top_y, uint32_t top_z)
{
    static int shown;
    float front[3] = {as_float(front_x), as_float(front_y), as_float(front_z)};
    float top[3] = {as_float(top_x), as_float(top_y), as_float(top_z)};

    if (dot3(front, front) < 1e-8f || dot3(top, top) < 1e-8f)
        return;
    LOCK();
    memcpy(listener_front, front, sizeof front);
    memcpy(listener_top, top, sizeof top);
    place_all();
    UNLOCK();
    if (shown++ < 3)
        fprintf(stderr, "[sound] listener facing %.2f %.2f %.2f, top %.2f %.2f %.2f\n", front[0], front[1], front[2],
                top[0], top[1], top[2]);
}

/* ---- streams ---- */

/* The stream's voice, made on first use (a volume can come before the first packet); NULL if out of range. */
static struct stream_voice *stream_voice(int stream)
{
    struct stream_voice *made;

    if (stream < 0 || stream >= MAX_STREAMS)
        return NULL;
    if (streams[stream])
        return streams[stream];
    if (!(made = calloc(1, sizeof *made)))
        return NULL;
    made->gain = 1.0f;
    made->headroom = 1.0f;
    LOCK();
    streams[stream] = made;
    UNLOCK();
    return made;
}

void bro_sound_stream_packet(int stream, uint32_t format, uint32_t data, uint32_t bytes)
{
    struct format f;
    struct stream_voice *s;
    uint32_t frames, i;

    start();
    if (stream < 0 || stream >= MAX_STREAMS || !data || !bytes)
        return;
    if (!read_format(format, &f)) {
        static int said;

        if (said++ < 8)
            fprintf(stderr, "[sound] stream %d: a packet of %u bytes in a format not read (at 0x%08X: tag %u)\n",
                    stream, bytes, format, format ? MEM16(format) : 0);
        return;
    }
    if (!(s = stream_voice(stream)))
        return;
    frames = frames_of(&f, bytes);
    LOCK();
    if (s->count + frames > s->capacity && s->capacity < STREAM_RING_MOST) {
        /* grow the ring (in order, from its head) so the packet fits */
        uint32_t want = s->capacity ? s->capacity : STREAM_RING_FRAMES, k;
        float (*grown)[2];

        while (want < s->count + frames && want < STREAM_RING_MOST)
            want *= 2;
        if (want > STREAM_RING_MOST)
            want = STREAM_RING_MOST;
        if ((grown = malloc(sizeof *grown * want)) != NULL) {
            for (k = 0; k < s->count; k++) {
                grown[k][0] = s->ring[(s->head + k) % s->capacity][0];
                grown[k][1] = s->ring[(s->head + k) % s->capacity][1];
            }
            free(s->ring);
            s->ring = grown;
            s->capacity = want;
            s->head = 0;
        }
    }
    s->rate = f.rate;
    for (i = 0; i < frames && s->count < s->capacity; i++) {
        uint32_t at = (s->head + s->count) % s->capacity;
        float l, r;

        if (f.tag == WAVE_FORMAT_XBOX_ADPCM) {
            static int16_t block[2][ADPCM_BLOCK_FRAMES];

            if (i % ADPCM_BLOCK_FRAMES == 0)
                decode_adpcm_block((const uint8_t *)(uintptr_t)XBOX_PTR(data + i / ADPCM_BLOCK_FRAMES *
                                                                         ADPCM_BLOCK_BYTES * f.channels),
                                   f.channels, block);
            l = block[0][i % ADPCM_BLOCK_FRAMES] / 32768.0f;
            r = f.channels == 2 ? block[1][i % ADPCM_BLOCK_FRAMES] / 32768.0f : l;
        } else if (f.bits == 16) {
            l = (int16_t)MEM16(data + i * f.block_align) / 32768.0f;
            r = f.channels == 2 ? (int16_t)MEM16(data + i * f.block_align + 2) / 32768.0f : l;
        } else {
            l = (MEM8(data + i * f.block_align) - 128) / 128.0f;
            r = f.channels == 2 ? (MEM8(data + i * f.block_align + 1) - 128) / 128.0f : l;
        }
        s->ring[at][0] = l;
        s->ring[at][1] = r;
        s->count++;
        if (fabsf(l) > s->peak)
            s->peak = fabsf(l);
    }
    s->dropped += frames - i;
    UNLOCK();
    if (s->packets++ < 4)
        fprintf(stderr, "[sound] stream %d: packet of %u bytes, format %u, %u channel(s), %u Hz: %u frames, %u kept "
                "(ring %u), peak so far %.2f\n", stream, bytes, f.tag, f.channels, f.rate, frames, i, s->count, s->peak);
}

void bro_sound_stream_volume(int stream, int32_t volume)
{
    static int shown;
    struct stream_voice *s = stream_voice(stream);

    if (!s)
        return;
    LOCK();
    s->gain = gain_of(volume);
    UNLOCK();
    if (shown++ < 8)
        fprintf(stderr, "[sound] stream %d: volume %d\n", stream, volume);
}

void bro_sound_stream_headroom(int stream, uint32_t headroom)
{
    static int shown;
    struct stream_voice *s = stream_voice(stream);

    if (!s)
        return;
    LOCK();
    s->headroom = gain_of(-(int32_t)(headroom > 10000 ? 10000 : headroom));
    UNLOCK();
    if (shown++ < 8)
        fprintf(stderr, "[sound] stream %d: headroom %u\n", stream, headroom);
}

void bro_sound_stream_flush(int stream)
{
    if (stream < 0 || stream >= MAX_STREAMS || !streams[stream])
        return;
    LOCK();
    streams[stream]->count = 0;
    streams[stream]->fraction = 0.0;
    UNLOCK();
}
