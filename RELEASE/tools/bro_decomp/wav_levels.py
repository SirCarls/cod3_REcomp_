#!/usr/bin/env python3
"""Levels of a 16-bit PCM WAV file, window by window: loudness (RMS, in dB below full scale), peak, how many
samples clip, and the zero-crossing rate (near 0.5 is noise; real sound is far lower).

A test run with BRO_SOUND_WAV=<file> writes the game's sound mix to a WAV file (bro/src/audio/bro_sound.c); this
checks that something played, and that it is not too loud or garbage. The same check fits CoD3's sound later.

    python3 tools/bro_decomp/wav_levels.py runs/r84/prologue_sound/sound_mix.wav [--window 5]

Exit status: 0 when some window has sound, 1 when the whole file is silent, 2 when the file can't be read."""
import argparse
import array
import math
import struct
import sys


def read_wav(path):
    """(channels, rate, samples as a list of arrays, one a channel) of a 16-bit PCM WAV file."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('not a WAV file')
    at, fmt, body = 12, None, None
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack('<I', data[at + 4:at + 8])[0]
        if tag == b'fmt ':
            fmt = struct.unpack('<HHIIHH', data[at + 8:at + 24])
        elif tag == b'data':
            body = data[at + 8:at + 8 + size]
            break
        at += 8 + size + (size & 1)
    if not fmt or body is None or fmt[0] != 1 or fmt[5] != 16:
        raise ValueError('not 16-bit PCM')
    channels, rate = fmt[1], fmt[2]
    samples = array.array('h', body[:len(body) // 2 * 2])
    if sys.byteorder == 'big':
        samples.byteswap()
    return channels, rate, [samples[c::channels] for c in range(channels)]


def window_levels(channel, rate, seconds):
    """[(start second, rms dBFS, peak, clipped share, zero-crossing rate)] a window of the channel at a time."""
    size = max(1, int(rate * seconds))
    out = []
    for start in range(0, len(channel), size):
        part = channel[start:start + size]
        if not part:
            break
        squares = sum(x * x for x in part)
        rms = math.sqrt(squares / len(part))
        peak = max(abs(x) for x in part)
        clipped = sum(1 for x in part if x >= 32767 or x <= -32768) / len(part)
        crossings = sum(1 for a, b in zip(part, part[1:]) if (a < 0) != (b < 0)) / max(1, len(part) - 1)
        db = 20 * math.log10(rms / 32768) if rms > 0 else float('-inf')
        out.append((start / rate, db, peak, clipped, crossings))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('wav', help='the WAV file')
    parser.add_argument('--window', type=float, default=5.0, help='seconds a window (default 5)')
    args = parser.parse_args()
    try:
        channels, rate, data = read_wav(args.wav)
    except (OSError, ValueError, struct.error) as e:
        print(f'{args.wav}: {e}', file=sys.stderr)
        return 2
    seconds = len(data[0]) / rate if data and rate else 0
    print(f'{channels} channel(s), {rate} Hz, {seconds:.1f} s')
    heard = False
    for c, channel in enumerate(data):
        print(f'channel {c}:')
        for start, db, peak, clipped, crossings in window_levels(channel, rate, args.window):
            heard |= peak > 0
            loud = 'silent' if db == float('-inf') else f'{db:6.1f} dB'
            print(f'  {start:7.1f} s  rms {loud:>9}  peak {peak:5d}  clipped {100 * clipped:5.2f}%  '
                  f'crossings {crossings:.3f}')
    return 0 if heard else 1


if __name__ == '__main__':
    sys.exit(main())
