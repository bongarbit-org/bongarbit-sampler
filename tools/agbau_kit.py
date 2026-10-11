#!/usr/bin/env python3
"""
agbau_kit.py - Turn the Agbau sample folder into something the ESP32-S3 can
hold, and report whether it fits.

The Agbau's samples are float32 mono WAVs at 44.1kHz, written for a Bela that
had memory to spare. 8,995 MB of int16 does not fit the N16R8's 8 MiB of PSRAM
(docs/agbau-migration.md, step 0). This tool is the single place where that
material is converted and cut, so both storage options START from identical
bits. They do not necessarily END there: M16's Wav::load() fades the first and
last 10ms of every sample after reading it (CLAUDE.md, "Embedding a sample in
flash"), and the flash image is never faded. The SD path holds the same bits
only if those edges are re-read after the load - src/test_agbau_storage.cpp
does, and compares the two sources sample by sample on the board.

Two outputs, matching the two storage options being measured on the bench:

  --sd DIR          16-bit mono WAVs, one per take, for the SD -> PSRAM path.
                    M16's Wav::load() reads 16-bit as-is, so the conversion
                    below is the only one that ever happens - and the boot reads
                    half the bytes it would for float32. (The only conversion,
                    not the only change: see the edge fade above.)
  --partition FILE  One binary image for a flash data partition, read in place
                    through esp_partition_mmap(). Format below; parsed and
                    bounds-checked by src/kit_image.h.

CONVERSION

float32 -> int16 with exactly the arithmetic of audioToI16() in
src/audio_block.h: scale by 32768, round half away from zero in float32, clamp
to [-32768, 32767]. Not M16's own float path, which truncates after scaling by
32767: converting here keeps one definition of "a sample in int16" for the
whole engine, the way tools/wav_to_header.py keeps one for flash and SD.

CUTS (both off by default - option A, the flash partition, needs neither)

  --trim-envelope   Truncate each non-ganza percussion take where its envelope
                    reaches zero: attack + decay, 441 + 132300 = 132741 frames.
                    From there on the port multiplies every frame by exactly 0,
                    so this changes nothing the instrument can output. It is
                    only safe while the percussion preset stays (0.01, 3.0, 0.0,
                    3.0); PRESETS below has to follow src/agbau_kit.h.
  --trim-tail-db DB Cut each take after its last sample above DB dBFS (e.g.
                    -60), keeping a 10ms linear fade past that point. Not
                    lossless, and not only in level: a take ending earlier
                    moves the frame where the voice freezes its envelope
                    (src/voice.h), which is where the NEXT hit's attack starts
                    from. On p4.3.1 that is 0.83 instead of 0.57, on p8.3.1
                    0.82 instead of 0.49. --trim-envelope has no such effect -
                    where it cuts, the envelope is already at 0.

The ganza loops (p3.1, p7.1, ~20s each) are 40% of the kit and are NOT cut by
anything here. Where to shorten a played rhythmic loop is a musical decision
(close it on a bar line), and only option B needs it.

PARTITION IMAGE FORMAT (version 1, little-endian)

  0   char[4]  magic "AGBK"
  4   u16      version = 1
  6   u16      take_count
  8   u32      crc32 (zlib) of every byte from the end of the take table to
               the end of the image
  12  u32      image_bytes
  16  take_count x { char name[24] NUL-padded; u32 offset; u32 frames;
                     u32 sample_rate }       (36 bytes each)
  ... int16 mono frames, each take starting on a 4-byte boundary

Usage:
    python3 tools/agbau_kit.py <agbau samples dir> [--trim-envelope]
        [--trim-tail-db -60] [--sd OUTDIR] [--partition OUT.bin]

With no output flag it only prints the budget table.
"""

import argparse
import array
import math
import os
import struct
import sys
import zlib

SAMPLE_RATE = 44100

# Envelope presets, (attack s, decay s, sustain, release s), from
# agbau/Bela/agbau-sampler-poly/render.cpp:84-104. src/agbau_kit.h is the
# definition the firmware uses; this copy exists only to know where the
# percussion envelope ends, and has to follow it.
PRESETS = {
    "bass":  (0.01, 0.0, 1.0, 1.0),
    "perc":  (0.01, 3.0, 0.0, 3.0),
    "ganza": (0.01, 0.0, 1.0, 0.1),
}

# Voice -> takes, same order and grouping as render.cpp's bassFilenames and
# percussionFilenames. Percussion 3 and 7 are the ganza loops.
VOICES = [
    ("bass", ["b1"]), ("bass", ["b2"]), ("bass", ["b3"]), ("bass", ["b4"]),
    ("bass", ["b5"]), ("bass", ["b6"]), ("bass", ["b7"]), ("bass", ["b8"]),
    ("perc", ["p1.3.1", "p1.3.2", "p1.3.3", "p1.3.4", "p1.3.5"]),
    ("perc", ["p2.2.1"]),
    ("ganza", ["p3.1"]),
    ("perc", ["p4.3.1"]),
    ("perc", ["p5.3.1", "p5.3.2", "p5.3.3"]),
    ("perc", ["p6.3.1", "p6.3.2", "p6.3.3"]),
    ("ganza", ["p7.1"]),
    ("perc", ["p8.3.1"]),
]

PSRAM_BYTES = 8 * 1024 * 1024
# The "samples" partition in partitions_agbau.csv.
PARTITION_BYTES = 0xCE0000

MAGIC = b"AGBK"
VERSION = 1
NAME_BYTES = 24
HEADER_FMT = "<4sHHII"
ENTRY_FMT = "<%dsIII" % NAME_BYTES


def seconds_to_frames(seconds):
    # Round to nearest, as src/envelope.h does. Bela truncates
    # (int)(rate * time) instead; for every preset in use the two agree.
    return int(seconds * SAMPLE_RATE + 0.5)


def read_wav_mono(path):
    """Return (sample_rate, float samples in +-1.0) for a mono WAV.

    Handles PCM 16-bit and IEEE float 32-bit, plain or WAVE_FORMAT_EXTENSIBLE.
    The stdlib wave module refuses float WAVs, which is what the Agbau has.
    """
    with open(path, "rb") as f:
        data = f.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    pos, fmt, payload = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", body)
            if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: real tag leads the GUID
                tag = struct.unpack_from("<H", body, 24)[0]
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            payload = body
        pos += 8 + size + (size & 1)
    if fmt is None or payload is None:
        raise ValueError(f"{path}: missing fmt or data chunk")
    tag, channels, rate, bits = fmt
    if channels != 1:
        raise ValueError(f"{path}: {channels} channels, the Agbau kit is mono")
    if tag == 3 and bits == 32:
        samples = array.array("f")
        samples.frombytes(payload[:len(payload) // 4 * 4])
    elif tag == 1 and bits == 16:
        ints = array.array("h")
        ints.frombytes(payload[:len(payload) // 2 * 2])
        samples = array.array("f", (s / 32768.0 for s in ints))
    else:
        raise ValueError(f"{path}: format {tag}, {bits}-bit is not supported")
    if sys.byteorder != "little":
        samples.byteswap()
    return rate, samples


def to_int16(samples):
    """audioToI16() over a whole take, reproducing its float32 arithmetic.

    x * 32768 is exact in float32 (a power of two). Adding +-0.5 is not, near
    the .5 boundary, and Python adds in float64 - so the sum is pushed back
    through a float32 array before truncation. Rounding a float64 sum of two
    float32 values to float32 gives the correctly rounded float32 sum, so this
    matches the C expression bit for bit, edge cases included.
    """
    scaled = [x * 32768.0 for x in samples]
    biased = array.array("f", (s + 0.5 if s >= 0.0 else s - 0.5 for s in scaled))
    out = array.array("h", bytes(2 * len(scaled)))
    for i, s in enumerate(scaled):
        if not (s > -32768.0):
            out[i] = -32768                # also catches NaN
        elif s >= 32767.0:
            out[i] = 32767
        else:
            out[i] = int(biased[i])        # truncates toward zero, like C
    return out


def tail_cut(samples, threshold_db):
    """Frames to keep so nothing above threshold_db is lost, plus a 10ms fade."""
    threshold = 10.0 ** (threshold_db / 20.0)
    last = len(samples)
    while last > 0 and abs(samples[last - 1]) < threshold:
        last -= 1
    return min(len(samples), last + seconds_to_frames(0.010))


def fade_out(samples, keep, fade_frames):
    start = max(0, keep - fade_frames)
    span = keep - start
    for i in range(start, keep):
        samples[i] *= (keep - i) / (span + 1)


def build_kit(src_dir, trim_envelope, trim_tail_db):
    attack, decay, _, _ = PRESETS["perc"]
    envelope_end = seconds_to_frames(attack) + seconds_to_frames(decay)

    takes = []
    for group, names in VOICES:
        for name in names:
            path = os.path.join(src_dir, name + ".wav")
            rate, samples = read_wav_mono(path)
            if rate != SAMPLE_RATE:
                raise ValueError(f"{path}: {rate} Hz - the voices play at "
                                 f"{SAMPLE_RATE} and do not resample")
            source_frames = keep = len(samples)
            why = ""
            if trim_tail_db is not None and group != "ganza":
                cut = tail_cut(samples, trim_tail_db)
                if cut < keep:
                    keep, why = cut, f"tail<{trim_tail_db:g}dB"
            if trim_envelope and group == "perc" and envelope_end < keep:
                keep, why = envelope_end, "envelope end"
            samples = samples[:keep]
            if why.startswith("tail"):
                fade_out(samples, keep, seconds_to_frames(0.010))
            takes.append(dict(name=name, group=group, source_frames=source_frames,
                              frames=keep, pcm=to_int16(samples), why=why))
    return takes


def report(takes):
    print(f"{'take':10s} {'group':6s} {'source':>8s} {'kept':>8s} "
          f"{'int16':>10s}  cut")
    total = 0
    for t in takes:
        nbytes = t["frames"] * 2
        total += nbytes
        print(f"{t['name']:10s} {t['group']:6s} "
              f"{t['source_frames'] / SAMPLE_RATE:7.2f}s "
              f"{t['frames'] / SAMPLE_RATE:7.2f}s {nbytes:10,d}  {t['why']}")
    ganza = sum(t["frames"] * 2 for t in takes if t["group"] == "ganza")
    print(f"\ntotal int16      {total:12,d} bytes ({total / 1e6:.3f} MB)")
    print(f"  of which ganza {ganza:12,d} bytes ({100.0 * ganza / total:.0f}%)")
    print(f"PSRAM (8 MiB)    {PSRAM_BYTES:12,d} bytes -> "
          f"{'fits' if total <= PSRAM_BYTES else 'DOES NOT FIT'}, margin "
          f"{PSRAM_BYTES - total:+,d} before any other allocation")
    print(f"flash partition  {PARTITION_BYTES:12,d} bytes -> "
          f"{'fits' if total <= PARTITION_BYTES else 'DOES NOT FIT'}, margin "
          f"{PARTITION_BYTES - total:+,d} (minus header and alignment)")


def write_sd(takes, out_dir):
    import wave
    os.makedirs(out_dir, exist_ok=True)
    for t in takes:
        pcm = t["pcm"]
        if sys.byteorder != "little":
            pcm = array.array("h", pcm)
            pcm.byteswap()
        with wave.open(os.path.join(out_dir, t["name"] + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(pcm.tobytes())
    print(f"wrote {len(takes)} 16-bit WAVs to {out_dir}")


def write_partition(takes, out_path):
    table_end = struct.calcsize(HEADER_FMT) + len(takes) * struct.calcsize(ENTRY_FMT)
    entries, blobs, offset = [], [], table_end
    for t in takes:
        offset = (offset + 3) & ~3
        name = t["name"].encode("ascii")
        if len(name) >= NAME_BYTES:
            raise ValueError(f"take name '{t['name']}' too long for the image")
        pcm = t["pcm"]
        if sys.byteorder != "little":
            pcm = array.array("h", pcm)
            pcm.byteswap()
        entries.append(struct.pack(ENTRY_FMT, name, offset, t["frames"], SAMPLE_RATE))
        blobs.append((offset, pcm.tobytes()))
        offset += t["frames"] * 2
    image = bytearray(offset)
    for i, e in enumerate(entries):
        pos = struct.calcsize(HEADER_FMT) + i * len(e)
        image[pos:pos + len(e)] = e
    for off, blob in blobs:
        image[off:off + len(blob)] = blob
    crc = zlib.crc32(bytes(image[table_end:])) & 0xFFFFFFFF
    image[0:struct.calcsize(HEADER_FMT)] = struct.pack(
        HEADER_FMT, MAGIC, VERSION, len(takes), crc, len(image))
    if len(image) > PARTITION_BYTES:
        raise ValueError(f"image is {len(image):,d} bytes, partition holds "
                         f"{PARTITION_BYTES:,d}")
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(image)
    print(f"wrote {out_path}: {len(takes)} takes, {len(image):,d} bytes, "
          f"crc32 {crc:08x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("samples", help="folder holding b1.wav ... p8.3.1.wav")
    ap.add_argument("--trim-envelope", action="store_true")
    ap.add_argument("--trim-tail-db", type=float, default=None)
    ap.add_argument("--sd", metavar="OUTDIR")
    ap.add_argument("--partition", metavar="OUT.bin")
    args = ap.parse_args()

    takes = build_kit(args.samples, args.trim_envelope, args.trim_tail_db)
    report(takes)
    if args.sd:
        write_sd(takes, args.sd)
    if args.partition:
        write_partition(takes, args.partition)


if __name__ == "__main__":
    main()
