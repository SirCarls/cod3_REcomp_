/* [acct2-fh] 2026-11-13: Finest Hour's films played by FFmpeg (owner 11-13 19:05: "for the bink movies, could we
 * just use ffmpeg instead?", 19:33: "sure on the movies").
 *
 * All 68 films on the disc (SHELL/MOVIES and its FRENCH, ITALIAN and SPANISH folders) are Bink 1 files: 640 x 480
 * pictures at 30 a second and one 48 kHz stereo sound track (ffprobe, 11-13). The game plays them through the Bink
 * library linked into default.xbe; its own movie code (0x0012C340-0x0012D010 in the Europe v3 default.xbe, driven by
 * the movie loop at 0x000B1BA0) calls eleven of the library's functions, read from those calls:
 *
 *   0x00251A80 BinkSetSoundTrack(2, {0, 1})          before every open (0x0012C39C)
 *   0x00251CC0 BinkOpen("d:\<name>", 0x4000)          the name made with "d:\%s" (0x0012C3C5); 0 if it failed
 *   0x00251610 BinkGetError()                         its text goes into "Cant load Bink Movie: %s -- %s" (0x0012CE5B)
 *   0x00253850 BinkGetTrackID(bink, index)            for each of the handle's +0xF0 tracks, then
 *   0x00253800 BinkSetVolume(bink, track, volume)     32768 at the open (0x0012C400); 0x10000 or 0 later (0x0012CFFF)
 *   0x00253500 BinkWait(bink)                         not 0: too soon for the next frame (0x0012CEDE, 0x0012CFD7)
 *   0x002539F0 BinkDoFrame(bink)                      decode the frame (0x0012CEF7)
 *   0x002539B0 BinkCopyToBuffer(bink, bits, pitch, height, 0, 0, 0x80000003)
 *                                                     into the back buffer, locked with LockRect (0x0012CF9D)
 *   0x00253FD0 BinkNextFrame(bink)                    on to the next frame (0x0012CF2F)
 *   0x00253700 BinkPause(bink, pause)                 (0x0012CFC1)
 *   0x00254170 BinkClose(bink)                        (0x0012C4BC)
 *
 * The game reads five fields of the handle: +0 the width and +4 the height (against the screen's, 0x0012C426),
 * +8 the frame count and +0xC the frame it is on (the end of a film that doesn't loop: the frame on reaching the
 * count less one, 0x0012CF23), and +0xF0 the number of sound tracks (0x0012C3EA). Each of the eleven is a stand-in
 * (fh/config/stand_in_functions.tsv, group "bink") answered here, so none of the library's own code runs: not its
 * decoder, its timer and file threads (the timer thread whose start crashed boot22 and boot34), or its DirectSound.
 *
 * The handle is a block of guest memory with those fields. Behind it: the film's file, read here (the layout of a
 * Bink file is at read_film_header), and FFmpeg's Bink decoders, libavcodec's binkvideo for the pictures and
 * binkaudio for the sound. The picture is turned into the surface's 32-bit pixels when the game asks
 * (BinkCopyToBuffer, picture_row); the sound goes to SDL3 as it is decoded. Frame n is due (n - 1) frames after the
 * first DoFrame, by the clock, as Bink's own BinkWait times it; a pause moves that on.
 *
 * FFmpeg here is the FFmpeg 4.4 that Call of Duty 3's SDK carries (work/rexglue-sdk/thirdparty/FFmpeg), built with
 * the port by fh/cmake/fh_ffmpeg.cmake: nothing is downloaded for it and nothing of the system's is used (owner
 * 11-13 19:49: "remember our portable plan you can't depend on the user having something"). Only libavutil and
 * libavcodec are built, so this file reads the Bink file and converts the pictures itself (no libavformat, no
 * libswscale). Licence: FFmpeg is LGPL as built (its configuration has CONFIG_GPL 0); it is linked into the port as
 * the SDK links the same FFmpeg into Call of Duty 3, and the player builds both from source on their own PC.
 *
 * Settings: FH_SOUND=0 plays the films without sound (the test runs set it: they are muted). Built without FFmpeg
 * (FH_HAVE_FFMPEG unset: the SDK's FFmpeg source wasn't found), every open fails with a message and the game goes on
 * without its films, as it does when a film is missing. */
#include "fh_movie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32: guest memory */
#include "xbox_memory_layout.h"     /* XBOX_PTR, xbox_HeapAlloc */
#include "kernel.h"                 /* xbox_translate_path: d:\ to the game's folder on the PC */

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#if FH_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#endif
#if FH_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

/* The handle's fields the game reads (above) */
#define HANDLE_WIDTH        0x00
#define HANDLE_HEIGHT       0x04
#define HANDLE_FRAMES       0x08
#define HANDLE_FRAME_NUM    0x0C
#define HANDLE_TRACKS       0xF0
#define HANDLE_BYTES        0x200   /* room past the last field read */

#define MAX_MOVIES          2       /* the game has one film open at a time; one more for safety */
#define COPY_32_BITS        3       /* BinkCopyToBuffer's flags, low 4 bits: 32-bit pixels (the game's 0x80000003) */
#define NORMAL_VOLUME       32768.0f    /* BinkSetVolume's "as recorded"; the game also asks for 0x10000, twice it */
#define ERROR_BYTES         160

/* A Bink 1 file, as FFmpeg's own reader (libavformat/bink.c, not built here) and the Bink file pages of
 * wiki.multimedia.cx describe it: a header of little-endian numbers, ... */
#define BINK_HEADER_BYTES   44
#define BINK_SIGNATURE      0       /* 'B' 'I' 'K' and the revision letter ('b' to 'k'); 'K' 'B' '2' is Bink 2 */
#define BINK_FILE_SIZE      4       /* the file's size less 8 */
#define BINK_FRAMES         8       /* frames */
#define BINK_WIDTH          20      /* (12: the largest frame's size, 16: not used) */
#define BINK_HEIGHT         24
#define BINK_RATE_NUM       28      /* frames a second: this over the next */
#define BINK_RATE_DEN       32
#define BINK_VIDEO_FLAGS    36      /* 4 bytes the picture decoder is set up with */
#define BINK_TRACKS         40      /* sound tracks */
/* ... then (revision 'k' only) 4 bytes not known; then, for the sound tracks, each one's largest decoded size
 * (4 bytes each), each one's sample rate and flags (2 + 2 bytes each), and each one's ID (4 bytes each); then where
 * each frame starts in the file (4 bytes each, bit 0 set on a key frame). A frame is, for each sound track, the
 * size of its part (4 bytes) and the part (a packet for the sound decoder when it is 4 bytes or more), then the
 * picture's packet in the rest. The last frame ends at the end of the file. */
#define BINK_TRACK_STEREO   0x2000  /* in a track's flags: two channels, else one */
#define BINK_TRACK_DCT      0x1000  /* the DCT sound decoder, else the RDFT one */
#define BINK_MAX_FRAMES     1000000
#define BINK_MAX_TRACKS     256
#define BINK_MAX_WIDTH      7680
#define BINK_MAX_HEIGHT     4800
#define MAX_FRAME_BYTES     (64u << 20)     /* more than this is a damaged frame table */
/* [r17] 2026-11-24: a call this many pictures behind the clock or more decodes the pictures that are due first,
 * at most CATCH_UP_MOST in one call; one more than RESTART_BEHIND behind (a load) starts the clock again */
#define CATCH_UP_BEHIND     2u
#define CATCH_UP_MOST       8u
#define RESTART_BEHIND      15u

struct movie {
    uint32_t handle;                /* the guest block the game holds, 0 when the slot is free */
    uint32_t frames;                /* the film's frame count */
    uint32_t frame_num;             /* the frame the game is on, from 1 (copied into the handle) */
    double frame_ns;                /* a frame's time */
    uint64_t started_ns;            /* when frame 1 was shown (the first DoFrame), moved on by any pause */
    uint64_t paused_ns;             /* when a pause began */
    int paused;
    uint32_t pictures;              /* pictures decoded, for the log */
    uint32_t caught_up;             /* [r17] pictures decoded but not shown, to keep to the clock */
    uint32_t restarts;              /* [r17] times the clock started again (a call far behind) */
    uint32_t loops;                 /* [r17] times the film went on from its last frame to its first */
    char path[1024];                /* the file on the PC, as UTF-8, for the log */
#if FH_HAVE_FFMPEG
    FILE *file;
    uint32_t width, height;
    uint32_t file_end;              /* the file's size: where the last frame ends */
    uint32_t *frame_starts;         /* where each frame starts in the file, as the table has it (bit 0: key frame) */
    uint32_t next_frame;            /* the next frame to read, from 0 */
    uint32_t tracks;                /* sound tracks in each frame; the first one is played */
    uint8_t *frame_data;            /* one frame as read from the file */
    uint32_t frame_room;
    AVCodecContext *video, *audio;
    AVFrame *picture;               /* the last picture decoded: what BinkCopyToBuffer copies */
    AVFrame *decoded;               /* the decoders' frame */
    AVPacket *packet;
    int have_picture;
    int y_low, y_high;              /* the brightness (Y) seen, every 30th picture, for the log */
#endif
#if FH_HAVE_SDL3
    SDL_AudioStream *sound;
#endif
};

static struct movie movies[MAX_MOVIES];
#if FH_HAVE_FFMPEG
static uint32_t handle_blocks[MAX_MOVIES];  /* each slot's guest block, made once and used again */
#endif
static uint32_t error_text;                 /* BinkGetError's guest string */

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

static struct movie *find(uint32_t handle)
{
    for (int i = 0; i < MAX_MOVIES; i++)
        if (handle && movies[i].handle == handle)
            return &movies[i];
    return NULL;
}

/* BinkGetError's text, kept in guest memory for the game's sprintf */
static void set_error(const char *text)
{
    if (!error_text)
        error_text = xbox_HeapAlloc(ERROR_BYTES, 16);
    if (error_text) {
        char *to = (char *)XBOX_PTR(error_text);

        strncpy(to, text, ERROR_BYTES - 1);
        to[ERROR_BYTES - 1] = '\0';
    }
}

#if FH_HAVE_FFMPEG
static int sound_wanted(void)
{
    const char *v = getenv("FH_SOUND");

    return !(v && strcmp(v, "0") == 0);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/* The film's file on the PC, from the game's "d:\...", opened to read; shown gets its name as UTF-8 */
static FILE *open_host_file(const char *xbox_path, char *shown, size_t shown_size)
{
    xbox_host_char host[1024];

    shown[0] = '\0';
    if (!xbox_translate_path(xbox_path, host, (DWORD)(sizeof host / sizeof host[0])))
        return NULL;
#ifdef _WIN32
    WideCharToMultiByte(CP_UTF8, 0, host, -1, shown, (int)shown_size, NULL, NULL);
    return _wfopen(host, L"rb");
#else
    snprintf(shown, shown_size, "%s", host);
    return fopen(host, "rb");
#endif
}

static int skip(FILE *f, long bytes)
{
    return fseek(f, bytes, SEEK_CUR) == 0;
}
#endif

#if FH_HAVE_FFMPEG && FH_HAVE_SDL3
static SDL_AudioStream *open_sound(int rate, int channels)
{
    static int said;
    SDL_AudioSpec spec;
    SDL_AudioStream *s;

    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (!said++)
            fprintf(stderr, "[movie] no sound: SDL's audio did not start (%s)\n", SDL_GetError());
        return NULL;
    }
    spec.format = SDL_AUDIO_F32;
    spec.channels = channels;
    spec.freq = rate;
    s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s) {
        if (!said++)
            fprintf(stderr, "[movie] no sound: no audio device (%s)\n", SDL_GetError());
        return NULL;
    }
    SDL_ResumeAudioStreamDevice(s);
    return s;
}
#endif

#if FH_HAVE_FFMPEG
/* A decoder set up with the 4 bytes of the file its codec wants (FFmpeg calls them extradata), not yet opened */
static AVCodecContext *new_decoder(enum AVCodecID id, const uint8_t setup[4], uint32_t tag)
{
    const AVCodec *codec = avcodec_find_decoder(id);
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;

    if (!c)
        return NULL;
    c->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!c->extradata) {
        avcodec_free_context(&c);
        return NULL;
    }
    memcpy(c->extradata, setup, 4);
    c->extradata_size = 4;
    c->codec_tag = tag;
    c->thread_count = 1;            /* a picture out for each packet in, at once */
    return c;
}

static int open_decoder(AVCodecContext **c)
{
    if (*c && avcodec_open2(*c, (*c)->codec, NULL) == 0)
        return 1;
    avcodec_free_context(c);
    return 0;
}

/* One packet to a decoder, from bytes of the frame (FFmpeg wants packets with padding after them, which
 * av_new_packet gives) */
static int send_packet(AVCodecContext *c, AVPacket *p, const uint8_t *data, uint32_t size)
{
    int r;

    if (av_new_packet(p, (int)size) < 0)
        return -1;
    memcpy(p->data, data, size);
    r = avcodec_send_packet(c, p);
    av_packet_unref(p);
    return r;
}

/* Decoded sound to SDL: Bink's sound comes as planar floats from the DCT decoder (and as floats one channel after
 * the other from the RDFT one); SDL takes them one sample of each channel after the other */
static void queue_sound(struct movie *m, const AVFrame *f)
{
#if FH_HAVE_SDL3
    static float *mixed;
    static size_t room;
    const int channels = f->channels;
    const size_t n = (size_t)f->nb_samples * (size_t)channels;

    if (!m->sound || channels <= 0 || !f->nb_samples)
        return;
    if (f->format == AV_SAMPLE_FMT_FLT) {
        SDL_PutAudioStreamData(m->sound, f->data[0], (int)(n * sizeof(float)));
        return;
    }
    if (f->format != AV_SAMPLE_FMT_FLTP)
        return;
    if (n > room) {
        float *bigger = realloc(mixed, n * sizeof(float));

        if (!bigger)
            return;
        mixed = bigger;
        room = n;
    }
    for (int c = 0; c < channels; c++) {
        const float *from = (const float *)f->extended_data[c];

        for (int i = 0; i < f->nb_samples; i++)
            mixed[(size_t)i * channels + c] = from[i];
    }
    SDL_PutAudioStreamData(m->sound, mixed, (int)(n * sizeof(float)));
#else
    (void)m;
    (void)f;
#endif
}

/* The lowest and highest brightness (Y) in the picture, for the log at the close: Y from 16 to 235 is what
 * picture_row takes; a film from 0 to 255 would want other sums */
static void note_brightness(struct movie *m)
{
    const AVFrame *p = m->picture;

    for (int r = 0; r < p->height; r++) {
        const uint8_t *y = p->data[0] + (size_t)r * (size_t)p->linesize[0];

        for (int x = 0; x < p->width; x++) {
            if (y[x] < m->y_low)
                m->y_low = y[x];
            if (y[x] > m->y_high)
                m->y_high = y[x];
        }
    }
}

/* Frame i of the file into m->frame_data; its size, 0 if it can't be read */
static uint32_t read_frame(struct movie *m, uint32_t i)
{
    const uint32_t start = m->frame_starts[i] & ~1u;
    const uint32_t end = i + 1 < m->frames ? m->frame_starts[i + 1] & ~1u : m->file_end;
    const uint32_t size = end > start ? end - start : 0;

    if (!size || size > MAX_FRAME_BYTES)
        return 0;
    if (size > m->frame_room) {
        uint8_t *bigger = realloc(m->frame_data, size);

        if (!bigger)
            return 0;
        m->frame_data = bigger;
        m->frame_room = size;
    }
    if (fseek(m->file, (long)start, SEEK_SET) != 0 || fread(m->frame_data, 1, size, m->file) != size)
        return 0;
    return size;
}

/* The film's next frame: the first sound track's part decoded and queued, the picture decoded into m->picture.
 * At the end the last picture stays. */
static void decode_next_picture(struct movie *m)
{
    uint32_t size, at = 0;

    if (m->next_frame >= m->frames)
        return;
    size = read_frame(m, m->next_frame++);
    if (!size)
        return;
    for (uint32_t t = 0; t < m->tracks; t++) {
        uint32_t part;

        if (size - at < 4)
            return;
        part = le32(m->frame_data + at);
        at += 4;
        if (part > size - at)
            return;
        if (t == 0 && m->audio && part >= 4 && send_packet(m->audio, m->packet, m->frame_data + at, part) == 0)
            while (avcodec_receive_frame(m->audio, m->decoded) == 0) {
                queue_sound(m, m->decoded);
                av_frame_unref(m->decoded);
            }
        at += part;
    }
    if (size > at && send_packet(m->video, m->packet, m->frame_data + at, size - at) == 0 &&
        avcodec_receive_frame(m->video, m->decoded) == 0) {
        av_frame_unref(m->picture);
        av_frame_move_ref(m->picture, m->decoded);
        m->have_picture = 1;
        if (m->pictures++ % 30 == 0)
            note_brightness(m);
    }
}

static void close_film(struct movie *m)
{
#if FH_HAVE_SDL3
    if (m->sound)
        SDL_DestroyAudioStream(m->sound);
    m->sound = NULL;
#endif
    av_frame_free(&m->picture);
    av_frame_free(&m->decoded);
    av_packet_free(&m->packet);
    avcodec_free_context(&m->video);
    avcodec_free_context(&m->audio);
    if (m->file)
        fclose(m->file);
    m->file = NULL;
    free(m->frame_starts);
    m->frame_starts = NULL;
    free(m->frame_data);
    m->frame_data = NULL;
    m->frame_room = 0;
}

/* The header and frame table (the layout is above, at BINK_HEADER_BYTES), and the decoders; 0 with BinkGetError's
 * text set if the film can't be played */
static int open_film(struct movie *m)
{
    uint8_t h[BINK_HEADER_BYTES], track[4], entry[4];
    uint32_t signature, rate_num, rate_den, track_rate = 0, track_flags = 0;

    if (fread(h, 1, sizeof h, m->file) != sizeof h) {
        set_error("the file is too short for a Bink file");
        return 0;
    }
    signature = le32(h + BINK_SIGNATURE);
    if ((signature & 0xFFFFFF) != ('B' | 'I' << 8 | 'K' << 16)) {
        set_error((signature & 0xFFFFFF) == ('K' | 'B' << 8 | '2' << 16) ? "a Bink 2 file: FFmpeg has no decoder for it"
                                                                           : "not a Bink file");
        return 0;
    }
    m->file_end = le32(h + BINK_FILE_SIZE) + 8;
    m->frames = le32(h + BINK_FRAMES);
    m->width = le32(h + BINK_WIDTH);
    m->height = le32(h + BINK_HEIGHT);
    rate_num = le32(h + BINK_RATE_NUM);
    rate_den = le32(h + BINK_RATE_DEN);
    m->tracks = le32(h + BINK_TRACKS);
    if (!m->frames || m->frames > BINK_MAX_FRAMES || !m->width || m->width > BINK_MAX_WIDTH || !m->height ||
        m->height > BINK_MAX_HEIGHT || !rate_num || !rate_den || m->tracks > BINK_MAX_TRACKS) {
        set_error("a damaged Bink header");
        return 0;
    }
    m->frame_ns = 1e9 * rate_den / rate_num;
    m->y_low = 255;
    m->y_high = 0;
    if ((signature >> 24) == 'k' && !skip(m->file, 4)) {
        set_error("a damaged Bink header");
        return 0;
    }
    if (m->tracks) {
        if (!skip(m->file, 4L * m->tracks)) {               /* each track's largest decoded size */
            set_error("a damaged Bink header");
            return 0;
        }
        for (uint32_t t = 0; t < m->tracks; t++) {          /* each track's sample rate and flags */
            if (fread(track, 1, 4, m->file) != 4) {
                set_error("a damaged Bink header");
                return 0;
            }
            if (t == 0) {
                track_rate = le16(track);
                track_flags = le16(track + 2);
            }
        }
        if (!skip(m->file, 4L * m->tracks)) {               /* each track's ID */
            set_error("a damaged Bink header");
            return 0;
        }
    }
    m->frame_starts = malloc((size_t)m->frames * sizeof m->frame_starts[0]);
    if (!m->frame_starts) {
        set_error("out of memory");
        return 0;
    }
    for (uint32_t i = 0; i < m->frames; i++) {
        if (fread(entry, 1, 4, m->file) != 4) {
            set_error("a damaged Bink frame table");
            return 0;
        }
        m->frame_starts[i] = le32(entry);
        if ((m->frame_starts[i] & ~1u) >= m->file_end ||
            (i && (m->frame_starts[i] & ~1u) <= (m->frame_starts[i - 1] & ~1u))) {
            set_error("a damaged Bink frame table");
            return 0;
        }
    }

    /* the picture decoder: the signature (its revision letter) and the 4 bytes of video flags */
    m->video = new_decoder(AV_CODEC_ID_BINKVIDEO, h + BINK_VIDEO_FLAGS, signature);
    if (m->video) {
        m->video->width = (int)m->width;
        m->video->height = (int)m->height;
    }
    if (!open_decoder(&m->video)) {
        set_error("FFmpeg's Bink picture decoder did not start");
        return 0;
    }
    /* the sound decoder for the first track: its rate and channels, and the signature (for the revision letter) */
    if (m->tracks && track_rate && sound_wanted()) {
        const int stereo = (track_flags & BINK_TRACK_STEREO) != 0;

        m->audio = new_decoder(track_flags & BINK_TRACK_DCT ? AV_CODEC_ID_BINKAUDIO_DCT : AV_CODEC_ID_BINKAUDIO_RDFT,
                               h + BINK_SIGNATURE, 0);
        if (m->audio) {
            m->audio->sample_rate = (int)track_rate;
            m->audio->channels = stereo ? 2 : 1;
            m->audio->channel_layout = stereo ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
        }
        if (open_decoder(&m->audio)) {
#if FH_HAVE_SDL3
            m->sound = open_sound(m->audio->sample_rate, m->audio->channels);
#endif
        } else
            fprintf(stderr, "[movie] FFmpeg's Bink sound decoder did not start: the film plays without sound\n");
    }
    m->picture = av_frame_alloc();
    m->decoded = av_frame_alloc();
    m->packet = av_packet_alloc();
    if (!m->picture || !m->decoded || !m->packet) {
        set_error("out of memory");
        return 0;
    }
    return 1;
}

static uint8_t to_byte(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* One row of the picture as 32-bit pixels, bytes B G R A (the surface's X8R8G8B8), from the decoder's YUV 4:2:0
 * (the colour U and V at half the width and height). The sums are ITU-R BT.601's with Y in 16-235 and U and V in
 * 16-240, as FFmpeg's own converter took Bink's pictures; scaled by 256 so they stay in whole numbers. */
static void picture_row(const AVFrame *p, int row, uint8_t *out, int count)
{
    const uint8_t *y = p->data[0] + (size_t)row * (size_t)p->linesize[0];
    const uint8_t *u = p->data[1] + (size_t)(row >> 1) * (size_t)p->linesize[1];
    const uint8_t *v = p->data[2] + (size_t)(row >> 1) * (size_t)p->linesize[2];
    const uint8_t *a = p->format == AV_PIX_FMT_YUVA420P ? p->data[3] + (size_t)row * (size_t)p->linesize[3] : NULL;

    for (int x = 0; x < count; x++) {
        const int c = 298 * (y[x] - 16), d = u[x >> 1] - 128, e = v[x >> 1] - 128;

        out[4 * x + 0] = to_byte((c + 516 * d + 128) / 256);
        out[4 * x + 1] = to_byte((c - 100 * d - 208 * e + 128) / 256);
        out[4 * x + 2] = to_byte((c + 409 * e + 128) / 256);
        out[4 * x + 3] = a ? a[x] : 255;
    }
}
#endif

uint32_t fh_movie_set_sound_track(uint32_t total_tracks, uint32_t tracks)
{
    (void)total_tracks;     /* every film has one track; the first is played whichever the game asks for */
    (void)tracks;
    return 0;
}

uint32_t fh_movie_open(uint32_t name, uint32_t flags)
{
    const char *xbox_path = name ? (const char *)XBOX_PTR(name) : "";
    struct movie *m = NULL;
    int slot;

    (void)flags;
    for (slot = 0; slot < MAX_MOVIES && movies[slot].handle; slot++)
        ;
    if (slot == MAX_MOVIES) {
        set_error("too many films open");
        return 0;
    }
    m = &movies[slot];
    memset(m, 0, sizeof *m);
#if FH_HAVE_FFMPEG
    m->file = open_host_file(xbox_path, m->path, sizeof m->path);
    if (!m->file) {
        set_error("no such file");
        fprintf(stderr, "[movie] %s: no such file\n", xbox_path);
        return 0;
    }
    if (!open_film(m)) {
        fprintf(stderr, "[movie] %s (%s): can't be played\n", xbox_path, m->path);
        close_film(m);
        return 0;
    }
    if (!handle_blocks[slot])
        handle_blocks[slot] = xbox_HeapAlloc(HANDLE_BYTES, 16);
    if (!handle_blocks[slot]) {
        set_error("no guest memory for the handle");
        close_film(m);
        return 0;
    }
    m->handle = handle_blocks[slot];
    m->frame_num = 1;
    memset((void *)XBOX_PTR(m->handle), 0, HANDLE_BYTES);
    MEM32(m->handle + HANDLE_WIDTH) = m->width;
    MEM32(m->handle + HANDLE_HEIGHT) = m->height;
    MEM32(m->handle + HANDLE_FRAMES) = m->frames;
    MEM32(m->handle + HANDLE_FRAME_NUM) = m->frame_num;
    MEM32(m->handle + HANDLE_TRACKS) = m->tracks ? 1 : 0;
    {
        const char *sound = !m->tracks ? "no sound" : !sound_wanted() ? "sound off (FH_SOUND=0)"
                            : !m->audio ? "sound not decoded" : "sound not played";

#if FH_HAVE_SDL3
        if (m->sound)
            sound = "with sound";
#endif
        fprintf(stderr, "[movie] %s: %u x %u, %u frames at %.2f a second, %s\n", xbox_path, m->width, m->height,
                m->frames, 1e9 / m->frame_ns, sound);
    }
    return m->handle;
#else
    {
        static int said;

        if (!said++)
            fprintf(stderr, "[movie] %s: this build has no FFmpeg (FH_HAVE_FFMPEG), so the films are not played\n",
                    xbox_path);
    }
    set_error("this build of the port has no FFmpeg");
    return 0;
#endif
}

uint32_t fh_movie_get_error(void)
{
    if (!error_text)
        set_error("");
    return error_text;
}

uint32_t fh_movie_wait(uint32_t bink)
{
    struct movie *m = find(bink);

    if (!m)
        return 0;
    if (m->paused)
        return 1;
    if (!m->started_ns)
        return 0;           /* the first frame is due at once */
    return now_ns() < m->started_ns + (uint64_t)((double)(m->frame_num - 1) * m->frame_ns) ? 1 : 0;
}

uint32_t fh_movie_pause(uint32_t bink, uint32_t pause)
{
    struct movie *m = find(bink);
    const uint64_t now = now_ns();

    if (!m)
        return 0;
    if (pause && !m->paused) {
        m->paused = 1;
        m->paused_ns = now;
    } else if (!pause && m->paused) {
        m->paused = 0;
        if (m->started_ns)
            m->started_ns += now - m->paused_ns;
    }
#if FH_HAVE_SDL3
    if (m->sound) {
        if (m->paused)
            SDL_PauseAudioStreamDevice(m->sound);
        else
            SDL_ResumeAudioStreamDevice(m->sound);
    }
#endif
    return (uint32_t)m->paused;
}

uint32_t fh_movie_set_volume(uint32_t bink, uint32_t track_id, uint32_t volume)
{
    struct movie *m = find(bink);

    (void)track_id;         /* one track (fh_movie_get_track_id) */
    if (!m)
        return 0;
#if FH_HAVE_SDL3
    if (m->sound) {
        float gain = (float)volume / NORMAL_VOLUME;

        /* [r17] 32768 is the sound as recorded; the game unmutes with 0x10000, and the Xbox's DirectSound
         * can't play louder than recorded (0 dB), so more than 32768 is as recorded */
        SDL_SetAudioStreamGain(m->sound, gain > 1.0f ? 1.0f : gain);
    }
#else
    (void)volume;
#endif
    return 0;
}

uint32_t fh_movie_get_track_id(uint32_t bink, uint32_t track_index)
{
    (void)bink;
    return track_index;
}

uint32_t fh_movie_copy_to_buffer(uint32_t bink, uint32_t dest, uint32_t pitch, uint32_t height, uint32_t x,
                                 uint32_t y, uint32_t flags)
{
    struct movie *m = find(bink);

    if (!m || !dest)
        return 0;
#if FH_HAVE_FFMPEG
    {
        static int said_flags, said_format;
        const AVFrame *p = m->picture;
        uint32_t rows, columns;

        if (!m->have_picture)
            return 0;
        if ((flags & 0xF) != COPY_32_BITS) {
            if (!said_flags++)
                fprintf(stderr, "[movie] BinkCopyToBuffer with flags 0x%08X: only 32-bit copies are made\n", flags);
            return 0;
        }
        if (p->format != AV_PIX_FMT_YUV420P && p->format != AV_PIX_FMT_YUVA420P) {
            if (!said_format++)
                fprintf(stderr, "[movie] the decoder's pictures are in format %d, not YUV 4:2:0: not copied\n",
                        p->format);
            return 0;
        }
        /* into the surface, as much of the picture as fits */
        if (y >= height || x >= pitch / 4)
            return 0;
        rows = (uint32_t)p->height < height - y ? (uint32_t)p->height : height - y;
        columns = (uint32_t)p->width < pitch / 4 - x ? (uint32_t)p->width : pitch / 4 - x;
        for (uint32_t r = 0; r < rows; r++)
            picture_row(p, (int)r, (uint8_t *)XBOX_PTR(dest + (y + r) * pitch + x * 4), (int)columns);
    }
#else
    (void)pitch;
    (void)height;
    (void)x;
    (void)y;
    (void)flags;
#endif
    return 0;
}

uint32_t fh_movie_do_frame(uint32_t bink)
{
    struct movie *m = find(bink);

    if (!m)
        return 0;
    if (!m->started_ns)
        m->started_ns = now_ns();
    else if (!m->paused) {
        /* [r17] the picture the clock is on (from 1); behind it, decode the ones that are due first, so a film
         * keeps its own pace and its sound (decoded with the pictures) keeps coming below 30 frames a second */
        const uint64_t now = now_ns();
        const uint64_t due = now > m->started_ns ? (uint64_t)((double)(now - m->started_ns) / m->frame_ns) + 1 : 1;

        if (due > (uint64_t)m->frame_num + RESTART_BEHIND) {
            m->started_ns = now - (uint64_t)((double)(m->frame_num - 1) * m->frame_ns);
            m->restarts++;
        } else if (due >= (uint64_t)m->frame_num + CATCH_UP_BEHIND) {
            uint32_t n = 0;

            while ((uint64_t)m->frame_num < due && m->frame_num + 1 < m->frames && n++ < CATCH_UP_MOST) {
#if FH_HAVE_FFMPEG
                decode_next_picture(m);
#endif
                m->frame_num++;
                m->caught_up++;
            }
            MEM32(m->handle + HANDLE_FRAME_NUM) = m->frame_num;
        }
    }
#if FH_HAVE_FFMPEG
    decode_next_picture(m);
#endif
    return 0;
}

uint32_t fh_movie_next_frame(uint32_t bink)
{
    struct movie *m = find(bink);

    if (!m)
        return 0;
    if (m->frame_num < m->frames)
        m->frame_num++;
    else {
        /* [r17] on from the last frame to the first, as Bink does: the front end loops its film this way
         * (0x0012CF08: its own count reset at the frame count, then BinkNextFrame); the first frame is due a
         * frame after the last one */
        m->frame_num = 1;
        m->started_ns += (uint64_t)((double)m->frames * m->frame_ns);
        if (!m->loops++)
            fprintf(stderr, "[movie] %s: on from its last frame to its first (the game loops it)\n", m->path);
#if FH_HAVE_FFMPEG
        m->next_frame = 0;
        if (m->video)
            avcodec_flush_buffers(m->video);
        if (m->audio)
            avcodec_flush_buffers(m->audio);
#endif
    }
    MEM32(m->handle + HANDLE_FRAME_NUM) = m->frame_num;
    return 0;
}

uint32_t fh_movie_close(uint32_t bink)
{
    struct movie *m = find(bink);

    if (!m)
        return 0;
#if FH_HAVE_FFMPEG
    fprintf(stderr, "[movie] closed at frame %u of %u (%u pictures decoded, brightness Y from %d to %d; "
            "%u decoded to keep to the clock, %u clock restarts, %u loops)\n",
            m->frame_num, m->frames, m->pictures, m->pictures ? m->y_low : 0, m->pictures ? m->y_high : 0,
            m->caught_up, m->restarts, m->loops);
    close_film(m);
#else
    fprintf(stderr, "[movie] closed at frame %u of %u\n", m->frame_num, m->frames);
#endif
    m->handle = 0;
    return 0;
}
