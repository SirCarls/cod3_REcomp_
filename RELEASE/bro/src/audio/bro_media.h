/* [acct2-bro] Big Red One port: the media objects the game streams sound through, native. See bro_media.c. */
#pragma once
#include <stdint.h>

#include "recomp_types.h"

/* XFileCreateMediaObjectAsync(file, max_packets, out): makes a file media object, writes it to out. */
uint32_t bro_media_create(uint32_t file, uint32_t max_packets, uint32_t out);

/* DirectSoundCreateStream(desc, out): makes a stream object that plays its packets out in real time. */
uint32_t bro_media_create_stream(uint32_t desc, uint32_t out);
/* IDirectSoundStream_SetFormat(stream, format) and IDirectSoundStream_FlushEx(stream, ...) */
uint32_t bro_media_stream_set_format(uint32_t stream, uint32_t format);
uint32_t bro_media_stream_flush(uint32_t stream);
/* IDirectSoundStream_SetVolume(stream, volume) and SetHeadroom(stream, headroom), handed to the mixer */
uint32_t bro_media_stream_volume(uint32_t stream, uint32_t volume);
uint32_t bro_media_stream_headroom(uint32_t stream, uint32_t headroom);

/* XAudioCreatePcmFormat(channels, rate, bits, format) and XAudioCreateAdpcmFormat(channels, rate, format): fill a
 * WAVEFORMATEX (Xbox ADPCM: 64 samples in 36 bytes per channel) */
uint32_t bro_media_pcm_format(uint32_t channels, uint32_t rate, uint32_t bits, uint32_t format);
uint32_t bro_media_adpcm_format(uint32_t channels, uint32_t rate, uint32_t format);

/* DirectSoundDoWork(): completes every stream packet whose play time is over. */
uint32_t bro_media_pump(void);

/* For recomp_lookup_manual(): the method behind one of the objects' vtable addresses, or NULL. */
recomp_func_t bro_media_method_for(uint32_t xbox_va);
