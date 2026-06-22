#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins
"""Author GSFX audio clips for the cue player (sfx.c).

A GSFX clip is the simple container the firmware's cue feeder reads from
LittleFS (/clips/<name>.gsfx):

    offset  size  field
    0       4     magic = b"GSFX"
    4       4     sample_rate  (uint32 LE)
    8       4     num_frames   (uint32 LE, mono samples)
    12      ..    int16 LE mono PCM, num_frames samples

Mono 16-bit only. The speaker path is a single GBA driver. The firmware
linearly resamples to the 44.1 kHz pipeline, so authoring at a lower rate
(e.g. 22050 for SFX, 16000 for speech) is fine and saves flash.

Usage:
    # Convert a mono WAV (any rate) to a clip:
    make_clip.py from-wav voice.wav clips/connected.gsfx --rate 16000

    # Generate the bundled built-in test cue (two-note rising chime):
    make_clip.py gen-startup firmware/data/startup.gsfx
"""
import argparse
import math
import struct
import sys
import wave

MAGIC = b"GSFX"


def write_clip(path, sample_rate, samples):
    """samples: iterable of ints in [-32768, 32767]."""
    samples = list(samples)
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<II", sample_rate, len(samples)))
        f.write(struct.pack("<%dh" % len(samples), *samples))
    print("wrote %s: %d Hz, %d frames (%.2f s), %d bytes payload"
          % (path, sample_rate, len(samples), len(samples) / sample_rate,
             len(samples) * 2))


def gen_startup(path):
    """A short pleasant two-note rising cue with attack/decay envelope."""
    sr = 22050
    notes = [(659.25, 0.12), (987.77, 0.20)]  # E5 -> B5
    samples = []
    for freq, dur in notes:
        n = int(sr * dur)
        for i in range(n):
            t = i / sr
            # Quick attack, exponential-ish decay so notes don't click.
            env = min(1.0, t / 0.005) * math.exp(-3.0 * (t / dur))
            samples.append(int(0.6 * 32767 * env * math.sin(2 * math.pi * freq * t)))
    write_clip(path, sr, samples)


def from_wav(wav_path, out_path, rate):
    with wave.open(wav_path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit("error: only 16-bit WAV supported")
        ch = w.getnchannels()
        src_rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)
    pcm = struct.unpack("<%dh" % (len(raw) // 2), raw)
    # Downmix to mono.
    if ch > 1:
        mono = [sum(pcm[i:i + ch]) // ch for i in range(0, len(pcm), ch)]
    else:
        mono = list(pcm)
    # Optional resample (linear) to the requested authoring rate.
    target = rate or src_rate
    if target != src_rate:
        ratio = src_rate / target
        out_n = int(len(mono) / ratio)
        res = []
        for i in range(out_n):
            pos = i * ratio
            j = int(pos)
            frac = pos - j
            a = mono[j]
            b = mono[min(j + 1, len(mono) - 1)]
            res.append(int(a + (b - a) * frac))
        mono = res
    mono = [max(-32768, min(32767, s)) for s in mono]
    write_clip(out_path, target, mono)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-startup", help="write the built-in test chime")
    g.add_argument("out")
    w = sub.add_parser("from-wav", help="convert a 16-bit WAV to a GSFX clip")
    w.add_argument("wav")
    w.add_argument("out")
    w.add_argument("--rate", type=int, default=0,
                   help="resample to this rate (default: keep WAV rate)")
    args = ap.parse_args()
    if args.cmd == "gen-startup":
        gen_startup(args.out)
    elif args.cmd == "from-wav":
        from_wav(args.wav, args.out, args.rate)


if __name__ == "__main__":
    main()
