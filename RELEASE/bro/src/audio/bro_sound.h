/* [acct2-bro] Big Red One port: DirectSound buffers and the mixer, native. See bro_sound.c. */
#pragma once
#include <stdint.h>

/* DirectSoundCreateBuffer(desc, out) and IDirectSound_CreateSoundBuffer(dsound, desc, out, unknown) */
uint32_t bro_sound_create_buffer(uint32_t desc, uint32_t out);

/* IDirectSoundBuffer_*: each returns the HRESULT the game gets. */
uint32_t bro_sound_buffer_set_format(uint32_t buffer, uint32_t format);
uint32_t bro_sound_buffer_set_data(uint32_t buffer, uint32_t data, uint32_t bytes);
uint32_t bro_sound_buffer_play(uint32_t buffer, uint32_t flags);
uint32_t bro_sound_buffer_stop(uint32_t buffer, uint32_t flags);
uint32_t bro_sound_buffer_pause(uint32_t buffer, uint32_t pause);
uint32_t bro_sound_buffer_get_status(uint32_t buffer, uint32_t out);
uint32_t bro_sound_buffer_set_position(uint32_t buffer, uint32_t offset);
uint32_t bro_sound_buffer_set_volume(uint32_t buffer, uint32_t volume);
uint32_t bro_sound_buffer_set_frequency(uint32_t buffer, uint32_t hertz);
uint32_t bro_sound_buffer_set_loop_region(uint32_t buffer, uint32_t start, uint32_t length);
uint32_t bro_sound_buffer_set_play_region(uint32_t buffer, uint32_t start, uint32_t length);
uint32_t bro_sound_buffer_set_headroom(uint32_t buffer, uint32_t headroom);
uint32_t bro_sound_buffer_set_mix_bins(uint32_t buffer, uint32_t bins);
uint32_t bro_sound_buffer_set_3d(uint32_t buffer, uint32_t params);     /* SetAllParameters(buffer, DS3DBUFFER *) */
uint32_t bro_sound_buffer_set_mode(uint32_t buffer, uint32_t mode);

/* IDirectSound_SetPosition / SetOrientation: the listener, as the floats' bits */
void bro_sound_listener_position(uint32_t x, uint32_t y, uint32_t z);
void bro_sound_listener_orientation(uint32_t front_x, uint32_t front_y, uint32_t front_z, uint32_t top_x,
                                    uint32_t top_y, uint32_t top_z);

/* A DirectSound stream's packet, copied in as it is queued (bro_media.c): the stream's number, its format (a
 * WAVEFORMATEX in guest memory) and the packet's bytes. Plays after the stream's earlier packets. */
void bro_sound_stream_packet(int stream, uint32_t format, uint32_t data, uint32_t bytes);
/* IDirectSoundStream_SetVolume / SetHeadroom by stream number (bro_media.c maps the objects), and a flush (drops
 * what is queued). */
void bro_sound_stream_volume(int stream, int32_t volume);
void bro_sound_stream_headroom(int stream, uint32_t headroom);
void bro_sound_stream_flush(int stream);

/* DirectSoundDoWork(): starts the output on first use, and without an output device moves the voices on by the
 * time that has passed. */
void bro_sound_pump(void);
