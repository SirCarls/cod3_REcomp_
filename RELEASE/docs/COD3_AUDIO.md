# Call of Duty 3's sound system (Xbox 360)

How the campaign program (`default.xex`, the supported USA build) plays sound: its sound bank
files, Treyarch's sound library "NSL", the FMOD Ex sound engine under it, and the game code that
drives them. Read from the game's own code (the Ghidra decompile in `dev/waw-decomp/cod3/`, the
disassembly) and checked against the retail bank files. Started 2026-10-16 (owner: "keep mapping
shit out"), after the voice language work in [X360_FAMILY.md](X360_FAMILY.md).

Everything here is facts about the program (addresses, layouts, behaviour). Function names are
candidates for the project's names list, with their evidence, in the private
`dev/x360-family/audio/nsl_names.tsv` and `fmod_names.tsv`; nothing here edits `symbols.toml` or
`names.toml`. The bank reader is `tools/x360_family/nslb.py`.

## The layers

| Layer | Where (campaign) | What |
|---|---|---|
| Game | `SoundDevice` (0x82492D18 init, 512 sound slots), `AudioBankMgr` (bank files per level), `CurveManager` (vehicle and physics sound curves), the effect sounds (`AbstractEffectSound`), the level scripts' `Broc_*` sound calls | decides what plays, where and how loud |
| NSL | about 0x82114530-0x82121040 (its first function is not pinned down), with Treyarch's file and memory library ("tl") after it | Treyarch's cross-platform sound library: banks, sound instances, voices, named objects (master, listeners, categories), volume and pitch groups, reverb. Its own names survive in two strings: the game's module table 0x825F6570 calls its update jobs `nslUpdateBanksModule` and `nslUpdateModule`; others are `NSL_MASTER`, `NSL_LISTENER_0`, "NSL: FMOD Realloc failed" |
| FMOD Ex 4.04.18 | 0x82377470-0x823C0280 (`c:/cod/code/tl/sdk/fmod/40418-src/...`) | the software mixer, the XMA codec, the Xbox 360 output; NSL calls its public C API; under it a statically linked XAudio library (0x822BF000-0x822CFFFF, the same code as in World at War's Xbox 360 program) talks to the console's audio driver |
| Memory | dlmalloc 2.8 `mspace` at 0x8211D1D8-0x82120430 | NSL gives FMOD a heap (20 MB by default) made with `create_mspace_with_base` |

NSL is cross-platform: its codec byte knows PCM, IMA ADPCM, PS2 VAG, XMA, MPEG and ATRAC3, of
which the Xbox 360 banks use XMA only.

## Sound banks (NSLB: `.wbk .wde .wfr .wes .wit .wen .wul`)

A level, a squad's battle chatter (`sp/bcen_usa`, `_bri`, `_can`, `_pol`) or the global sounds
are each one bank file. The game's loader (0x8211CDA8) reads the first 0x1000 bytes, checks
`NSLB` and the version bytes 1, 0, reads the whole header, then the resident wave data;
0x82120678 byte-swaps the header (all integers are little-endian) and turns its offsets into
pointers. The language of a bank is only its file extension (see
[X360_FAMILY.md](X360_FAMILY.md), *Voices from another disc*).

**Header** (16 words, then the bank name):

| Offset | Field |
|---|---|
| 0x00 | `NSLB` |
| 0x04 | version: bytes 01 00 (the loader refuses anything else) |
| 0x08 | header size (0x1000-0x8000, a multiple of 0x1000) |
| 0x10 | wave count |
| 0x18 | offset of the name table (one u32 per wave: an offset into the names) |
| 0x1C | offset of the wave table (16 bytes per wave) |
| 0x20, 0x24 | size and offset of the format records |
| 0x28, 0x2C | size and offset of the names (NUL-terminated strings; offset 0 is the empty name) |
| 0x30, 0x34 | size and offset of the resident data (loaded into memory with the bank; offset 0x10000) |
| 0x38, 0x3C | size and offset of the streamed data (played from the file) |
| 0x40 | bank name, char[32] (`BLKBRN`, `GLOBAL`) |
| 0x60 | 32 bytes the loader clears (runtime fields) |
| 0x0C, 0x14 | 0 in every bank seen |

**Wave table entry** (16 bytes): format record offset (in the format area), data offset (in the
resident data, or in the streamed data when the wave is streamed), data size, length in samples.
Several waves can share one name (variants of a sound) and one format record.

**Format record** (0x18 bytes, then one float per set bit of the parameter mask, padded to 16;
the last record of the area may end unpadded):

| Offset | Field |
|---|---|
| +0x00 | u16 sample rate |
| +0x02 | u16 sample rate again (equal in every record seen) |
| +0x04 | codec: 0x11/0x15 PCM16, 0x20 IMA ADPCM, 0x21 VAG, **0x30 XMA**, 0x32/0x33 MPEG, 0x35 ATRAC3 (what NSL asks FMOD for, 0x8211A250) |
| +0x05 | bit 0 streamed, bit 1 looped |
| +0x06 | channel mask (0 = mono, 3 = stereo) |
| +0x07 | bit 0: 3D (the loader clears it on multichannel waves); bit 1: allows a per-instance option the game switches on and off (`nslSoundOptionOn`/`Off`, meaning unknown); bit 2 unused |
| +0x08 | category: an offset into the names (`MUSIC`, `SFX`, `WEAPONS`, `VOICE`, `AMBIENT`, `VEHICLES`...), 0 for none; the loader turns it into NSL's named object of that name |
| +0x0C | group hash: matched against NSL's volume and pitch groups (below) |
| +0x10 | u64 parameter mask: bit *n* set = parameter *n* follows |
| +0x18 | the parameters, in bit order |

NSL's parameter numbers are shared by the format records, sound instances, named objects and
listeners:

| Index | Meaning | Evidence |
|---|---|---|
| 0 | volume | the rolloff curve's near volume (0x82119F10); the game reads and sets it |
| 1 | pitch (frequency factor) | NSL sets FMOD's frequency to pitch × the wave's rate |
| 6 | volume at the maximum distance | the rolloff curve's far end (default 0) |
| 7, 8 | voice priority when quiet / when loud | `nslComputePriority` (0x8211C690): the wave's gain at listener 0 through its rolloff curve, clamped to parameters 9-10, mapped linearly onto 7-8 |
| 9, 10 | the gains that priority range spans | same |
| 19-21 | position | instances and listeners (mask 0x380000) |
| 22-24 | velocity | mask 0x1C00000 |
| 25 | minimum distance | rolloff (default 1); the game reads it (index 0x19) |
| 26 | maximum distance | rolloff (default 100); the game reads it (index 0x1A) |
| 46-48, 49-51 | listener forward and up | `nslSetListenerOrientationN` |
| 2, 4, 5, 12, 27 | in every bank (2 is always 0; 4 and 5 are 0-0.5; 12 is 0, 75 or 100; 27 is 0 or 1) | no code reads them; meaning unknown (4 and 5 look like random volume and pitch ranges) |

**The waves** are complete RIFF/WAVE files in the data areas: format tag 0x0165 (XMA,
`XMAWAVEFORMAT` version 2 with one stream), a `seek` chunk, then `data`. Their rate and channel count
always match the format record. Every XMA stream says loop count 255, and many have a loop region,
looped or not; whether a wave loops is NSL's flag (format byte 5 bit 1), passed to FMOD as
`FMOD_LOOP_NORMAL` or `FMOD_LOOP_OFF`.

**Checked** with `nslb.py` on 29 banks (29,661 waves, every check passed): all 15 English level banks,
the US squad's battle chatter in English and French, Black Baron's and St. Bert's banks from all
three discs (German whole banks, the Fr/Es/It split pairs), the campaign and multiplayer global
banks (English, German, French) and the empty `credits` bank. `sp/global` holds 3,448 waves, 3,280
of them streamed; the battle chatter 2,455, almost all streamed; a level bank 750-1,060 waves,
almost all resident. Unless the game's configuration says otherwise, `nslFmodInit` sets aside
0x23D7840 bytes (about 36 MB) for NSL's memory and 0x1400000 (20 MB) for FMOD's heap.

## How a sound plays

1. **Banks.** The level loader (0x82462790) registers each AUDIO entry of an archive with
   `AudioBankMgr_AddBankFile` (0x82478B80: opens the file, keeps the handle in the bank's language
   slot); `AudioBankMgr_LoadBank` (0x82494E90) loads slot 5 and the game's language slot through
   `nslLoadBank`. NSL has 8 bank slots; `nslUpdateBanks` (0x82119830) runs the loader.
2. **A sound** is found by its name hash (`nslFindSound`, 0x821196E0, searches the loaded banks)
   and made an instance (`nslCreateSound`, 0x82116FF0; up to 512, 0x148 bytes each). The game sets
   its volume, pitch, distances, position and velocity (`nslSetSoundInstanceParam`,
   `nslSetSoundPosition`...) and starts it (`nslPlaySound`).
3. **Voices.** `nslUpdate` (0x82117710, each frame) gives playing instances voices (0x140 bytes;
   2D sounds get one at creation). `nslFmodUpdate` (0x8211A250) turns each voice into FMOD calls:
   - `System::createSound` from memory (`FMOD_OPENMEMORY_POINT | FMOD_CREATECOMPRESSEDSAMPLE`) for
     resident waves, or streamed from the bank file at the wave's offset (`FMOD_CREATESTREAM |
     FMOD_NONBLOCKING`); 3D waves with `FMOD_3D | FMOD_3D_CUSTOMROLLOFF`, others `FMOD_2D`;
   - `System::playSound` paused, then the channel group (music, or the reverb groups `Reverb` /
     `Reverb_Voice` for voices of the `VOICE` category), user data, the end callback;
   - for 3D waves a 128-point rolloff curve (`nslSetCustomRolloff`, 0x82119F10): from the minimum
     to the maximum distance, from the volume down to the far volume, quadratic;
   - each frame the changed parameters: `Channel::setVolume`, `setFrequency`, `set3DAttributes`,
     `set3DMinMaxDistance`;
   - then `System::set3DListenerAttributes` for listener 0 and `System::update`.
4. **Mixing.** `nslUpdateSounds` (0x82116248) gives each voice its volume = master × the product
   (clamped) of the wave's volume, the instance's, its category object's and two global factors:
   the **volume group** level for every sound whose group hash is *not* in the group (scripts duck
   everything but a dialogue this way; `SoundDevice_FadeVolumeGroup` fades it) and the pause level
   (`nslPauseAllSounds` is a volume factor, not a stop). Pitch works the same way with the **pitch
   group** (slow motion for everything but the listed sounds).
5. **Priority.** Before a voice is given, `nslComputePriority` rates the sound (parameters 7-10 above);
   when more than 247 voices are busy only priority 51 and up may start (`nslCanPlay`, 0x8211C638).
6. **Stall watchdog.** For a resident, non-looping XMA wave, NSL reads the channel's position (ms)
   every update; when it has not moved for more than 32 ms (0x825A1800) **and** more than 4 updates
   (0x825A1804), NSL stops the voice as finished (the check around 0x8211ABFC-0x8211AC30). On
   the console's 30 frames per second that is at least 133 ms.

## The listener

`CG_DrawActiveFrame` (0x82450C90), after drawing the view, sends the listener to
`SoundDevice_SetListener` (0x8246BB70, which drops non-finite values) and from there to NSL's
listener 0:

- position = the camera (`cg.refdef.vieworg`, cg_t + 0x1180) moved 10 units (the float at
  0x820635D0) back along the view's horizontal direction (`AngleVectors` of
  `cg.refdefViewAngles` + 0x11C0 with the pitch set to 0);
- forward and up = `cg.refdef.viewaxis[0]` and `[2]` (+0x1190, +0x11A8);
- converted from the game's axes to FMOD's as (x, y, z) → (−y, z, x).

Sound positions get the same conversion (`SoundHandle_SetPosition`, 0x8247D968); velocities do
not (`SoundHandle_SetVelocity`, 0x8247D8B8, passes them unconverted), which only matters for
Doppler.

So the listener is wherever the view is drawn from: in a vehicle, on a turret or in a scripted
camera it follows that camera, not the player's body. NSL has four listeners
(`NSL_LISTENER_0`-`3`) but only listener 0 reaches FMOD.

## Occlusion and reverb

There is **no occlusion** in this sound path: NSL never calls FMOD's occlusion function
(`FMOD_Channel_Set3DOcclusion`, 0x82379880, is linked but unused), FMOD's geometry functions are
compiled out in this build (each returns `FMOD_ERR_UNSUPPORTED`, 0x43), and nothing between the
game and NSL traces the world for a sound. A 3D sound's loudness
is its distance through the wave's rolloff curve, and its direction comes from the listener above.
**Reverb** is two FMOD SFX reverb units on the `Reverb` and `Reverb_Voice` channel groups; the
level scripts choose presets with the `reverb "<preset>" <time> <...>` client command
(`Broc_Reverb`, level slot 0x9C0), and `SoundDevice_UpdateReverbBlend` (0x82465FC0) blends the 12
values over the given time into `nslSetReverb` (0x8211B2F8). The preset follows the script's
trigger zones (where the player is), not the camera.

For the port (the owner's 2026-10-15 report "Sound occlusion doesn't account for where the camera
is currently"): moving the listener cannot add occlusion, because the game has none. Occlusion
would be new work, for instance a trace from `cg.refdef.vieworg` to each 3D voice's position in
`nslFmodUpdate`, applied with `FMOD_Channel_Set3DOcclusion` (direct and reverb occlusion), which
this FMOD has.

## "Some sounds play twice"

The owner's other report. Leads from the code on both sides, for whoever takes the fix (the
board's claude-setup audio work); none is tested in the game yet.

- **The emulator's XMA "output valid" bit (the strongest lead).** Every XMA sound is decoded by
  FMOD's own software XMA codec: NSL asks for software sounds and FMOD has no hardware voices here
  (`FMOD_System_SetHardwareChannels(0, 0, 0, 0)`), and each sound gets one XMA context with two
  2 KB input packets and a 2 KB output ring (`CodecXMA_resetContext` 0x823B7ED8). On each read,
  `CodecXMA_pumpBuffers` (0x823B80A8) treats "output read offset == write offset and output
  **not** valid" as a **full** ring and copies all 2 KB, and it reports the end of the sound only
  when that test finds the ring empty (offsets equal, output **valid**) with both input packets
  used up and the input read offset back at 32. The SDK's decoder (`src/audio/xma_context.cpp`,
  `XmaContext::Work`) clears the output-valid bit whenever the offsets are equal after a pass,
  including a pass that decoded nothing because FMOD had no more input to give it. FMOD then reads
  that empty ring as full and plays its last 2 KB again (11-43 ms of PCM), and it never sees the
  end, so it keeps doing so until it reaches the sound's length. FMOD estimates that length from the
  `seek` table as one packet past the last entry (`CodecXMA_parseRiff` 0x823B83A0), so the tail of
  a sound repeats for up to about a packet's worth of time. On the console the bit is cleared only
  when the decoder fills the ring. The fix to try: in `Work`, clear it only when that pass wrote
  output and the write offset reached the read offset.
- **NSL's stall watchdog** (above) depends on frames as well as time: at a high frame rate four
  updates pass quickly, so only the 32 ms condition is left. A decoder whose position advances in
  steps longer than 32 ms would have sounds stopped early. The game's data-driven sound events
  (0x8248A958, run from `Com_Frame`; their data has `KeyType`, `Event`, `Conditions`, `Volume`,
  `Pitch`) only update a sound while NSL reports it playing and otherwise start a new one, so an
  event sound stopped early while its event still holds starts again from the beginning.
- **Not a cause:** XMA looping. FMOD never gives the decoder loop data (the context's loop fields
  stay zero); it loops a sound by setting the context up again, seeking to the packet and
  discarding samples up to the loop start. The loop count of 255 in every stream's `fmt` chunk is
  only read for hardware voices, which this game doesn't use.

## FMOD in the program

FMOD Ex 4.04.18, built from `c:/cod/code/tl/sdk/fmod/40418-src` with the Xbox 360 parts
(`xbox360/src/fmod_codec_xma.cpp`, `fmod_output_xenon.cpp`, `fmod_sample_xenon.cpp`,
`fmod_os_misc.cpp`). It spans 0x82377470-0x823C0280 (1,471 functions; Ghidra's function list
misses or mis-sizes hundreds of the small ones there, so the bounds come from `.pdata`, call targets
and padding). First come the C API (`FMOD_System_Init` ...) and the C++ API (`FMOD::System::init`
...) in the order `fmod.hpp` declares them: each C function checks the handle and tail-calls the C++
one, which validates the object and calls the internal method (`SystemI::init`). Then the
internals, placed module by module by the source-file strings their memory calls pass: `SystemI`,
memory, threads, files, the software output and mixer, the codecs (raw, MPEG, WAV, VAG, the DSP
codec pool, RIFF), `SoundI`, `ChannelI`, `ChannelGroupI`, `DSPI`, the DSP connection pool, the
resampler, the SFX reverb, the XMA codec (0x823B7E10-0x823B92C8) and the Xenon output. Its threads:
"FMOD stream thread", "FMOD file thread", "FMOD thread for FMOD_NONBLOCKING", "FMOD output polling
thread" and "FMOD XMP GetStatus thread" (pauses a channel group while a user soundtrack plays).

How sound reaches the console:

- **Decoding** is FMOD's software XMA codec, one XMA context per sound (resident waves are
  compressed samples played through the DSP codec pool's contexts, streamed waves get their own):
  two 2 KB input packets, a 2 KB output ring (8 blocks of 256 bytes), 4 subframes per decode, loop
  fields zero. Each read is `XMADisableContext`, refill and harvest (`CodecXMA_pumpBuffers`), copy
  the PCM, `XMAEnableContext`; `SoundI::readData` keeps reading until it has the bytes it asked
  for, so a decoder that makes no progress is a busy wait. FMOD calls the kernel's `XMA*`
  functions directly, only from this file.
- **Output** is FMOD's software mix, written into a looping ring buffer that one XAudio voice plays
  (filled by the output polling thread). FMOD reaches XAudio through the statically linked XAudio
  library at 0x822BF000-0x822CFFFF, which makes the render-driver calls
  (`XAudioRegisterRenderDriverClient`, `XAudioSubmitRenderDriverFrame` ...). NSL sets no hardware
  channels, so FMOD's hardware voices (`ChannelXenon` with XMA voices) are never used.

The public functions NSL calls (C API names; the full table, with the C++ wrappers, the internals
and their evidence, is `fmod_names.tsv`):

| Address | FMOD API | NSL's use |
|---|---|---|
| 0x82377470 | `FMOD_Memory_Initialize` | its mspace, with alloc/realloc/free callbacks |
| 0x823775D8 | `FMOD_Memory_GetStats` | every update |
| 0x82377610 | `FMOD_System_Create` | |
| 0x82377C08 | `FMOD_System_SetAdvancedSettings` | |
| 0x82377788 / 0x823777D0 / 0x823778F0 | `FMOD_System_GetOutput` / `GetNumDrivers` / `GetDriver` | |
| 0x82377860 | `FMOD_System_GetDriverCaps` | |
| 0x823779C8 / 0x82377938 | `FMOD_System_SetSoftwareChannels` / `SetHardwareChannels` | 256 software, no hardware |
| 0x82377F68 | `FMOD_System_Init` | 256 channels |
| 0x82377FF8 | `FMOD_System_Update` | end of `nslFmodUpdate` |
| 0x82378160 | `FMOD_System_Set3DListenerAttributes` | listener 0: position, velocity, forward, up |
| 0x823781F0 | `FMOD_System_SetSpeakerPosition` | |
| 0x82378280 | `FMOD_System_SetStreamBufferSize` | 0x20000, `FMOD_TIMEUNIT_RAWBYTES` |
| 0x823785C8 | `FMOD_System_CreateSound` | per voice (memory or stream) |
| 0x823786A0 | `FMOD_System_CreateDSPByType` | two `FMOD_DSP_TYPE_SFXREVERB` (18) |
| 0x82378730 | `FMOD_System_CreateChannelGroup` | `Reverb`, `Reverb_Voice`, `music` |
| 0x82378778 | `FMOD_System_PlaySound` | `FMOD_CHANNEL_FREE`, paused |
| 0x82377738 | `FMOD_System_Release` | shutdown |
| 0x82378FD0 / 0x82379180 / 0x823791F8 | `FMOD_Sound_Release` / `GetLength` / `GetOpenState` | |
| 0x82379408 / 0x82379420 | `FMOD_Channel_Stop` / `SetPaused` | |
| 0x823794B0 / 0x823794E0 | `FMOD_Channel_SetVolume` / `SetFrequency` | |
| 0x82379700 | `FMOD_Channel_GetPosition` | the stall watchdog, in ms |
| 0x82379748 | `FMOD_Channel_SetChannelGroup` | music and reverb groups |
| 0x82379778 | `FMOD_Channel_SetCallback` | end callback `nslFmodChannelCallback` |
| 0x82379790 / 0x823797C0 | `FMOD_Channel_Set3DAttributes` / `Set3DMinMaxDistance` | |
| 0x82379850 | `FMOD_Channel_Set3DCustomRolloff` | 129 points |
| 0x82379910 | `FMOD_Channel_IsPlaying` | |
| 0x82379AF8 | `FMOD_Channel_SetUserData` | the voice number |
| 0x82379B28 / 0x82379D28 | `FMOD_ChannelGroup_Release` / `AddDSP` | |
| 0x82379F20 / 0x82379DE8 / 0x8237A068 | `FMOD_DSP_SetActive` / `Release` / `SetParameter` | the 12 reverb values |

## Function names

The candidates (address, name, evidence) are in `dev/x360-family/audio/nsl_names.tsv` (NSL, the
game's sound device and bank manager, dlmalloc) and `fmod_names.tsv` (FMOD). Highlights:

| Address | Name | |
|---|---|---|
| 0x82117710 | `nslUpdate` | Treyarch's own name (module table) |
| 0x82119830 | `nslUpdateBanks` | Treyarch's own name (module table) |
| 0x82117E80 / 0x82118028 | `nslStartup` / `nslShutdown` | |
| 0x8211AD98 / 0x8211A250 | `nslFmodInit` / `nslFmodUpdate` | |
| 0x82118888 / 0x82119510 | `nslLoadBank` / `nslUnloadBank` | |
| 0x821196E0 | `nslFindSound` | by name hash |
| 0x82116FF0 / 0x821151B8 | `nslCreateSound` / `nslPlaySound` | |
| 0x82115920 | `nslSetSoundInstanceParam` | |
| 0x82114BB0 / 0x82114BC0 | `nslSetListenerOrientation` / `nslSetListenerPosition` | |
| 0x82120678 | `nslFixupBank` | the bank format's definition |
| 0x8246BB70 | `SoundDevice_SetListener` | |
| 0x82478B80 | `AudioBankMgr_AddBankFile` | hooked for the voice languages |
