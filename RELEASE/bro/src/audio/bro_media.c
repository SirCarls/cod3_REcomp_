/* [acct2-bro] Big Red One port: the media objects the game streams sound through, native.
 *
 * Movies and music are streams. The game's SndStream (SndStream_SetFormat 0x000C7020, SndStream_Update 0x000C7540)
 * pairs a file media object (XFileCreateMediaObjectAsync, 0x003C7A83) that reads packets with a DirectSound stream
 * (DirectSoundCreateStream, 0x003C95FB) that plays them. Every frame SndStream_Update calls the file object's DoWork
 * (vtable +0x24) and GetStatus (+0x0C); while that has XMO_STATUSF_ACCEPT_OUTPUT_DATA it reads a packet into each
 * free slot (Process, +0x10) and hands it to the DirectSound stream (its Process, +0x10, at 0x000C7654). A slot stays
 * busy while its packet status is E_PENDING. The second movie (Propgnda.xbv) keeps time by its sound, so with no
 * working stream it never gets past its first frame.
 *
 * Both objects are native here. The vtables are addresses at 0xFEF00000 (file objects) and 0xFEF00100 (streams):
 * at or above 0xFE000000 the generated code treats an indirect call target as code and asks recomp_lookup_manual()
 * first, which sends them here.
 *  - A file object reads each packet it is given from its file and completes it at once: the game's own
 *    XAPI ReadFile (0x001E7F2A) at the object's own offset (an OVERLAPPED), so the read goes through the same kernel
 *    file layer as every other read. Seek and GetLength use that offset and SetFilePointer (0x001E8171). At the end
 *    of the file a packet comes back short (or empty), which is how the game sees the end.
 *  - A stream keeps each packet pending and completes it when its play time is over, from the stream format's
 *    average byte rate, checked on every DirectSoundDoWork and stream call, which is what the game times movies by.
 * A stream hands each packet to the mixer (bro_sound.c) as it is queued, so what a stream is given is heard. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc */
#include "bro_media.h"
#include "bro_sound.h"

#define FILE_METHOD_BASE    0xFEF00000u
#define STREAM_METHOD_BASE  0xFEF00100u
#define METHOD_COUNT        10
#define OBJECT_BYTES        0x20u
/* an object: +0x00 vtable, +0x04 references, +0x08 file handle or stream number, +0x0C packets seen; a file
 * object also +0x10 its read offset, +0x14 its file's length (UNKNOWN_LENGTH until asked) */
#define OBJECT_HANDLE       0x08u
#define OBJECT_PACKETS      0x0Cu
#define FILE_OFFSET         0x10u
#define FILE_LENGTH         0x14u
#define UNKNOWN_LENGTH      0xFFFFFFFFu

/* the game's own XAPI file functions (stdcall) */
#define XAPI_READ_FILE          0x001E7F2Au     /* ReadFile(file, buffer, bytes, *read, *overlapped) */
#define XAPI_SET_FILE_POINTER   0x001E8171u     /* SetFilePointer(file, distance, *distance high, method) */
#define FILE_BEGIN_METHOD       0u
#define FILE_CURRENT_METHOD     1u
#define FILE_END_METHOD         2u
/* guest scratch for a read: an OVERLAPPED {Internal, InternalHigh, Offset, OffsetHigh, hEvent}, then the count */
#define SCRATCH_BYTES           0x20u
#define SCRATCH_DONE            0x04u           /* InternalHigh: what an overlapped read delivered */
#define SCRATCH_OFFSET          0x08u
#define SCRATCH_READ            0x18u

#define S_OK                0x00000000u
#define E_OUTOFMEMORY       0x8007000Eu
#define E_PENDING           0x8000000Au
#define XMO_STATUSF_ACCEPT_INPUT_DATA  0x00000001u
#define XMO_STATUSF_ACCEPT_OUTPUT_DATA 0x00000002u
#define XMEDIAPACKET_STATUS_SUCCESS    0x00000000u

/* XMEDIAPACKET */
#define PACKET_BUFFER       0x00u
#define PACKET_MAX_SIZE     0x04u
#define PACKET_COMPLETED_SIZE 0x08u
#define PACKET_STATUS       0x0Cu
#define PACKET_EVENT        0x10u
/* DSSTREAMDESC: flags, max attached packets, format, callback, context; WAVEFORMATEX: average bytes/s at +8 */
#define DESC_FORMAT         0x08u
#define DESC_CALLBACK       0x0Cu
#define FORMAT_AVG_BYTES    0x08u
#define FORMAT_COPY_BYTES   0x14u   /* WAVEFORMATEX and an ADPCM format's samples-per-block word */
#define WAVE_FORMAT_PCM     0x0001u
#define WAVE_FORMAT_XBOX_ADPCM 0x0069u
#define ADPCM_BLOCK_BYTES   36u
#define ADPCM_BLOCK_SAMPLES 64u

#define MAX_STREAMS         32
#define MAX_PENDING         32
#define DEFAULT_BYTE_RATE   32000u

struct pending {
    uint32_t completed_size, status, size;
};

static struct stream {
    uint32_t object;
    uint32_t byte_rate;
    uint32_t format;        /* its WAVEFORMATEX, for the mixer */
    struct pending queue[MAX_PENDING];
    int head, count;
    double head_start;      /* seconds: when the packet at the head began playing */
    unsigned long packets_done;
} streams[MAX_STREAMS];
static int stream_count;

static uint32_t file_vtable, stream_vtable;
static int files_made;

static double now_seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void pop_args(int count, uint32_t result)
{
    g_eax = result;
    g_esp += 4 + 4 * (uint32_t)count;      /* return address, then the stdcall arguments */
}

static uint32_t arg(int n)
{
    return MEM32(g_esp + 4 + 4 * (uint32_t)n);
}

static void complete_packet(uint32_t completed_size_ptr, uint32_t status_ptr, uint32_t size)
{
    if (completed_size_ptr)
        MEM32(completed_size_ptr) = size;
    if (status_ptr)
        MEM32(status_ptr) = XMEDIAPACKET_STATUS_SUCCESS;
}

static struct stream *stream_of(uint32_t object)
{
    uint32_t n;

    if (!object || MEM32(object) != stream_vtable)
        return NULL;
    n = MEM32(object + 8);
    return n < (uint32_t)stream_count ? &streams[n] : NULL;
}

/* Completes the stream's packets whose play time is over. */
static void stream_play(struct stream *s, double now)
{
    while (s->count) {
        struct pending *p = &s->queue[s->head];
        double length = (double)p->size / (s->byte_rate ? s->byte_rate : DEFAULT_BYTE_RATE);

        if (now < s->head_start + length)
            break;
        complete_packet(p->completed_size, p->status, p->size);
        s->head_start += length;
        s->head = (s->head + 1) % MAX_PENDING;
        s->count--;
        s->packets_done++;
    }
    if (!s->count)
        s->head_start = now;    /* nothing queued: the next packet starts when it arrives */
}

uint32_t bro_media_pump(void)
{
    double now = now_seconds();
    int i;

    for (i = 0; i < stream_count; i++)
        stream_play(&streams[i], now);
    bro_sound_pump();
    return S_OK;
}

static uint32_t make_vtable(uint32_t base)
{
    uint32_t vtable = xbox_HeapAlloc(METHOD_COUNT * 4u, 16);
    uint32_t i;

    if (vtable)
        for (i = 0; i < METHOD_COUNT; i++)
            MEM32(vtable + 4 * i) = base + 4 * i;
    return vtable;
}

static uint32_t make_object(uint32_t vtable, uint32_t tag)
{
    uint32_t object = xbox_HeapAlloc(OBJECT_BYTES, 16);

    if (object) {
        memset((void *)XBOX_PTR(object), 0, OBJECT_BYTES);
        MEM32(object + 0) = vtable;
        MEM32(object + 4) = 1;
        MEM32(object + OBJECT_HANDLE) = tag;
        MEM32(object + FILE_LENGTH) = UNKNOWN_LENGTH;
    }
    return object;
}

/* Calls the game's stdcall function at address with count arguments, as its own code would: the arguments and a
 * return address on the guest stack, which the callee pops; returns what it leaves in eax. */
static uint32_t call_game(uint32_t address, int count, const uint32_t *args)
{
    recomp_func_t fn = recomp_lookup(address);
    uint32_t esp = g_esp;
    int i;

    if (!fn) {
        static int said;

        if (!said++)
            fprintf(stderr, "[media] 0x%08X has no recompiled code\n", address);
        return 0;
    }
    for (i = count - 1; i >= 0; i--) {
        g_esp -= 4u;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4u;
    MEM32(g_esp) = address;             /* the return address; only the callee's ret reads it */
    fn();
    g_esp = esp;
    return g_eax;
}

static uint32_t scratch;    /* guest memory for the reads, made on first use */

static uint32_t file_length(uint32_t self)
{
    if (MEM32(self + FILE_LENGTH) == UNKNOWN_LENGTH) {
        uint32_t args[4] = {MEM32(self + OBJECT_HANDLE), 0, 0, FILE_END_METHOD};

        MEM32(self + FILE_LENGTH) = call_game(XAPI_SET_FILE_POINTER, 4, args);
    }
    return MEM32(self + FILE_LENGTH) == UNKNOWN_LENGTH ? 0 : MEM32(self + FILE_LENGTH);
}

/* Reads up to bytes at the object's offset into buffer (guest memory) and moves the offset on; returns what came. */
static uint32_t file_read(uint32_t self, uint32_t buffer, uint32_t bytes)
{
    uint32_t args[5], ok, got;

    if (!scratch && !(scratch = xbox_HeapAlloc(SCRATCH_BYTES, 16)))
        return 0;
    memset((void *)XBOX_PTR(scratch), 0, SCRATCH_BYTES);
    MEM32(scratch + SCRATCH_OFFSET) = MEM32(self + FILE_OFFSET);
    args[0] = MEM32(self + OBJECT_HANDLE);
    args[1] = buffer;
    args[2] = bytes;
    args[3] = scratch + SCRATCH_READ;
    args[4] = scratch;
    ok = call_game(XAPI_READ_FILE, 5, args);
    got = ok ? MEM32(scratch + SCRATCH_READ) : 0;
    if (!got)
        got = MEM32(scratch + SCRATCH_DONE);
    if (got > bytes)
        got = bytes;
    MEM32(self + FILE_OFFSET) += got;
    return got;
}

/* ---- methods both kinds share: 0 AddRef, 1 Release, 2 GetInfo, 5 Discontinuity ---- */

static void media_add_ref(void)
{
    uint32_t self = arg(0);

    MEM32(self + 4) += 1;
    pop_args(1, MEM32(self + 4));
}

/* the object stays allocated (the guest heap has no matching free here); it just stops counting */
static void media_release(void)
{
    uint32_t self = arg(0);

    if (MEM32(self + 4))
        MEM32(self + 4) -= 1;
    pop_args(1, MEM32(self + 4));
}

static void media_get_info(void)
{
    uint32_t info = arg(1), i;

    if (info)
        for (i = 0; i < 16; i += 4)
            MEM32(info + i) = 0;
    pop_args(2, S_OK);
}

static void media_one_arg(void)
{
    pop_args(1, S_OK);
}

/* ---- the file object: 3 GetStatus, 4 Process, 6 Flush, 7 Seek, 8 GetLength, 9 DoWork ---- */

static void file_get_status(void)
{
    uint32_t status = arg(1);

    if (status)
        MEM32(status) = XMO_STATUSF_ACCEPT_OUTPUT_DATA;
    pop_args(2, S_OK);
}

/* Process(this, XMEDIAPACKET *input, XMEDIAPACKET *output): the output fills from the file and completes; past
 * the end it comes back short, the rest zeroed */
static void file_process(void)
{
    uint32_t self = arg(0), output = arg(2);

    if (output) {
        uint32_t buffer = MEM32(output + PACKET_BUFFER), size = MEM32(output + PACKET_MAX_SIZE), got = 0;
        uint32_t offset = MEM32(self + FILE_OFFSET), seen = MEM32(self + OBJECT_PACKETS);

        if (buffer && size) {
            got = file_read(self, buffer, size);
            if (got < size)
                memset((void *)XBOX_PTR(buffer + got), 0, size - got);
        }
        complete_packet(MEM32(output + PACKET_COMPLETED_SIZE), MEM32(output + PACKET_STATUS), got);
        MEM32(self + OBJECT_PACKETS) = seen + 1;
        if (seen == 0 || (got < size && seen < 64))
            fprintf(stderr, "[media] file 0x%08X: packet %u, %u of %u bytes read at offset %u\n",
                    MEM32(self + OBJECT_HANDLE), seen, got, size, offset);
    }
    pop_args(3, S_OK);
}

/* Seek(this, offset, origin, *absolute): origin 0 the start, 1 where it is, 2 the end */
static void file_seek(void)
{
    uint32_t self = arg(0), origin = arg(2), absolute = arg(3);
    int64_t base = origin == FILE_CURRENT_METHOD ? (int64_t)MEM32(self + FILE_OFFSET)
                   : origin == FILE_END_METHOD   ? (int64_t)file_length(self) : 0;
    int64_t to = base + (int32_t)arg(1);

    static int shown;

    if (to < 0)
        to = 0;
    if (shown++ < 16)
        fprintf(stderr, "[media] file 0x%08X: seek %d from %s to %u\n", MEM32(self + OBJECT_HANDLE), (int32_t)arg(1),
                origin == FILE_CURRENT_METHOD ? "here" : origin == FILE_END_METHOD ? "the end" : "the start",
                (uint32_t)to);
    MEM32(self + FILE_OFFSET) = (uint32_t)to;
    if (absolute)
        MEM32(absolute) = (uint32_t)to;
    pop_args(4, S_OK);
}

static void file_get_length(void)
{
    uint32_t self = arg(0), length = arg(1);

    if (length)
        MEM32(length) = file_length(self);
    pop_args(2, S_OK);
}

/* ---- the stream: 3 GetStatus, 4 Process, 6 Flush ---- */

static void stream_get_status(void)
{
    struct stream *s = stream_of(arg(0));
    uint32_t status = arg(1);

    if (s)
        stream_play(s, now_seconds());
    if (status)
        MEM32(status) = s && s->count < MAX_PENDING ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
    pop_args(2, S_OK);
}

/* Process(this, XMEDIAPACKET *input, XMEDIAPACKET *output): the input waits its turn to play */
static void stream_process(void)
{
    uint32_t self = arg(0), input = arg(1);
    struct stream *s = stream_of(self);

    if (s && input) {
        uint32_t size = MEM32(input + PACKET_MAX_SIZE);
        uint32_t completed = MEM32(input + PACKET_COMPLETED_SIZE), status = MEM32(input + PACKET_STATUS);

        if (MEM32(input + PACKET_EVENT) && MEM32(self + 0xC) == 0)
            fprintf(stderr, "[media] stream 0x%08X: packets carry an event, which is not signaled yet\n", self);
        if (MEM32(self + 0xC)++ == 0)
            fprintf(stderr, "[media] stream 0x%08X: first packet, %u bytes at %u bytes/s\n", self, size,
                    s->byte_rate);
        if (s->count == MAX_PENDING) {
            complete_packet(completed, status, size);       /* no room: played at once */
        } else {
            struct pending *p = &s->queue[(s->head + s->count) % MAX_PENDING];

            if (!s->count)
                s->head_start = now_seconds();
            p->completed_size = completed;
            p->status = status;
            p->size = size;
            s->count++;
            bro_sound_stream_packet((int)(s - streams), s->format, MEM32(input + PACKET_BUFFER), size);
            if (completed)
                MEM32(completed) = 0;
            if (status)
                MEM32(status) = E_PENDING;
        }
    }
    pop_args(3, S_OK);
}

static void stream_flush(void)
{
    bro_media_stream_flush(arg(0));
    pop_args(1, S_OK);
}

static const recomp_func_t file_methods[METHOD_COUNT] = {
    media_add_ref, media_release, media_get_info, file_get_status, file_process,
    media_one_arg, media_one_arg, file_seek, file_get_length, media_one_arg,
};
static const recomp_func_t stream_methods[METHOD_COUNT] = {
    media_add_ref, media_release, media_get_info, stream_get_status, stream_process,
    media_one_arg, stream_flush, media_one_arg, media_one_arg, media_one_arg,
};

uint32_t bro_media_create(uint32_t file, uint32_t max_packets, uint32_t out)
{
    uint32_t object;

    (void)max_packets;
    if (!file_vtable && !(file_vtable = make_vtable(FILE_METHOD_BASE)))
        return E_OUTOFMEMORY;
    if (!(object = make_object(file_vtable, file)))
        return E_OUTOFMEMORY;
    {
        /* several objects can share one file (seen: six on one handle), each from where the file stood when it
         * was made */
        uint32_t args[4] = {file, 0, 0, FILE_CURRENT_METHOD};
        uint32_t at = call_game(XAPI_SET_FILE_POINTER, 4, args);

        MEM32(object + FILE_OFFSET) = at == UNKNOWN_LENGTH ? 0 : at;
    }
    if (out)
        MEM32(out) = object;
    if (files_made++ < 8)
        fprintf(stderr, "[media] XFileCreateMediaObjectAsync(file 0x%08X): object 0x%08X, from offset %u\n", file,
                object, MEM32(object + FILE_OFFSET));
    return S_OK;
}

uint32_t bro_media_create_stream(uint32_t desc, uint32_t out)
{
    struct stream *s;
    uint32_t object;

    if (stream_count == MAX_STREAMS)
        return E_OUTOFMEMORY;
    if (!stream_vtable && !(stream_vtable = make_vtable(STREAM_METHOD_BASE)))
        return E_OUTOFMEMORY;
    if (!(object = make_object(stream_vtable, (uint32_t)stream_count)))
        return E_OUTOFMEMORY;
    s = &streams[stream_count++];
    memset(s, 0, sizeof *s);
    s->object = object;
    s->byte_rate = DEFAULT_BYTE_RATE;
    if (desc) {
        if (MEM32(desc + DESC_FORMAT))
            bro_media_stream_set_format(object, MEM32(desc + DESC_FORMAT));
        if (MEM32(desc + DESC_CALLBACK))
            fprintf(stderr, "[media] stream 0x%08X has a completion callback 0x%08X, not called yet\n", object,
                    MEM32(desc + DESC_CALLBACK));
    }
    if (out)
        MEM32(out) = object;
    return S_OK;
}

/* The format is copied: the game's WAVEFORMATEX can be a local that is gone (run 86 read tags 2, then 51696, at one
 * address) by the time the packets come. */
uint32_t bro_media_stream_set_format(uint32_t stream, uint32_t format)
{
    static int shown;
    struct stream *s = stream_of(stream);

    if (!s || !format)
        return S_OK;
    if (!s->format && !(s->format = xbox_HeapAlloc(FORMAT_COPY_BYTES, 16)))
        return E_OUTOFMEMORY;
    memcpy((void *)XBOX_PTR(s->format), (const void *)XBOX_PTR(format), FORMAT_COPY_BYTES);
    if (MEM32(format + FORMAT_AVG_BYTES))
        s->byte_rate = MEM32(format + FORMAT_AVG_BYTES);
    if (shown++ < 8)
        fprintf(stderr, "[media] stream 0x%08X: format %u, %u channel(s), %u Hz, %u bytes/s\n", stream,
                MEM16(format), MEM16(format + 2), MEM32(format + 4), MEM32(format + FORMAT_AVG_BYTES));
    return S_OK;
}

static void fill_format(uint32_t format, uint32_t tag, uint32_t channels, uint32_t rate, uint32_t block_align,
                        uint32_t bits, uint32_t average)
{
    if (!format)
        return;
    MEM16(format + 0x0) = (uint16_t)tag;
    MEM16(format + 0x2) = (uint16_t)channels;
    MEM32(format + 0x4) = rate;
    MEM32(format + FORMAT_AVG_BYTES) = average;
    MEM16(format + 0xC) = (uint16_t)block_align;
    MEM16(format + 0xE) = (uint16_t)bits;
    MEM16(format + 0x10) = 0;          /* cbSize */
}

uint32_t bro_media_pcm_format(uint32_t channels, uint32_t rate, uint32_t bits, uint32_t format)
{
    channels &= 0xFFFF;
    bits &= 0xFFFF;
    fill_format(format, WAVE_FORMAT_PCM, channels, rate, channels * bits / 8, bits, rate * channels * bits / 8);
    return S_OK;
}

uint32_t bro_media_adpcm_format(uint32_t channels, uint32_t rate, uint32_t format)
{
    channels &= 0xFFFF;
    fill_format(format, WAVE_FORMAT_XBOX_ADPCM, channels, rate, ADPCM_BLOCK_BYTES * channels, 4,
                rate * ADPCM_BLOCK_BYTES * channels / ADPCM_BLOCK_SAMPLES);
    if (format) {
        MEM16(format + 0x10) = 2;                       /* cbSize: wSamplesPerBlock follows */
        MEM16(format + 0x12) = ADPCM_BLOCK_SAMPLES;
    }
    return S_OK;
}

uint32_t bro_media_stream_volume(uint32_t stream, uint32_t volume)
{
    struct stream *s = stream_of(stream);

    if (s)
        bro_sound_stream_volume((int)(s - streams), (int32_t)volume);
    return S_OK;
}

uint32_t bro_media_stream_headroom(uint32_t stream, uint32_t headroom)
{
    struct stream *s = stream_of(stream);

    if (s)
        bro_sound_stream_headroom((int)(s - streams), headroom);
    return S_OK;
}

uint32_t bro_media_stream_flush(uint32_t stream)
{
    struct stream *s = stream_of(stream);

    if (s)
        bro_sound_stream_flush((int)(s - streams));
    while (s && s->count) {
        struct pending *p = &s->queue[s->head];

        complete_packet(p->completed_size, p->status, 0);
        s->head = (s->head + 1) % MAX_PENDING;
        s->count--;
    }
    return S_OK;
}

recomp_func_t bro_media_method_for(uint32_t xbox_va)
{
    if ((xbox_va & 3) == 0) {
        if (xbox_va >= FILE_METHOD_BASE && xbox_va < FILE_METHOD_BASE + 4 * METHOD_COUNT)
            return file_methods[(xbox_va - FILE_METHOD_BASE) / 4];
        if (xbox_va >= STREAM_METHOD_BASE && xbox_va < STREAM_METHOD_BASE + 4 * METHOD_COUNT)
            return stream_methods[(xbox_va - STREAM_METHOD_BASE) / 4];
    }
    return NULL;
}
