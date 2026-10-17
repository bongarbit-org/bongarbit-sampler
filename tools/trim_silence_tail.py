#!/usr/bin/env python3
"""
trim_silence_tail.py - finds where each WAV's amplitude envelope drops to a
negligible level and trims everything after it, replacing the cut with a
short cosine fade down to exact zero (no click at the new end).

Only trims the TAIL (trailing near-silence/noise floor) - never touches the
start or any pause in the middle of the clip. Operates on mono 16-bit PCM
WAVs (what tools/wav_to_header.py and the sampler firmware expect).

Usage:
    python3 tools/trim_silence_tail.py src/samples/*.wav
    python3 tools/trim_silence_tail.py --threshold-db -45 --dry-run src/samples/*.wav
"""

import argparse
import struct
import sys
import wave

import numpy as np


def envelope_rms(samples: np.ndarray, window: int, hop: int) -> np.ndarray:
    """RMS amplitude per window, hop-spaced. Pads the tail so every sample
    is covered by at least one window."""
    n = len(samples)
    if n == 0:
        return np.array([])
    starts = np.arange(0, max(n - window, 0) + 1, hop)
    if len(starts) == 0 or starts[-1] + window < n:
        starts = np.append(starts, max(n - window, 0))
    out = np.empty(len(starts))
    s64 = samples.astype(np.float64)
    for idx, start in enumerate(starts):
        chunk = s64[start:start + window]
        out[idx] = np.sqrt(np.mean(chunk * chunk)) if len(chunk) else 0.0
    return out, starts


def find_tail_cut(samples: np.ndarray, rate: int, threshold_db: float,
                   window_ms: float, hop_ms: float, release_ms: float) -> int:
    """Returns the sample index where meaningful content ends (last point
    with headroom above the noise floor, plus a release tail)."""
    window = max(1, int(rate * window_ms / 1000))
    hop = max(1, int(rate * hop_ms / 1000))
    env, starts = envelope_rms(samples, window, hop)
    if len(env) == 0:
        return len(samples)

    peak = np.max(np.abs(samples)) or 1
    env_db = 20 * np.log10(np.maximum(env, 1e-9) / peak)

    above = np.where(env_db > threshold_db)[0]
    if len(above) == 0:
        # Whole file is already below the threshold - nothing meaningful to keep.
        return 0
    last_active_window = above[-1]
    last_active_sample = starts[last_active_window] + window

    release_samples = int(rate * release_ms / 1000)
    return min(len(samples), last_active_sample + release_samples)


def apply_fade_and_trim(samples: np.ndarray, cut_point: int, fade_samples: int,
                         floor_sample: int) -> np.ndarray:
    fade_samples = min(fade_samples, cut_point - floor_sample) if cut_point > floor_sample else 0
    trimmed = samples[:cut_point].astype(np.float64)
    if fade_samples > 0:
        fade_start = cut_point - fade_samples
        t = np.linspace(0, 1, fade_samples, endpoint=True)
        gain = 0.5 * (1 + np.cos(np.pi * t))  # 1 -> 0, same cosine shape M16 uses for its own edge fades
        trimmed[fade_start:cut_point] *= gain
        trimmed[cut_point - 1] = 0.0  # guarantee exact zero at the new end, not just "close"
    return np.clip(trimmed, -32768, 32767).astype(np.int16)


def process_file(path: str, threshold_db: float, window_ms: float, hop_ms: float,
                  release_ms: float, fade_ms: float, dry_run: bool) -> None:
    with wave.open(path, "rb") as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        rate = wf.getframerate()
        n_frames = wf.getnframes()
        raw = wf.readframes(n_frames)

    if sampwidth != 2:
        print(f"{path}: skipped - expected 16-bit PCM, got {sampwidth * 8}-bit")
        return
    if channels != 1:
        print(f"{path}: skipped - expected mono, got {channels} channels")
        return

    samples = np.frombuffer(raw, dtype="<i2")
    original_duration = len(samples) / rate

    cut_point = find_tail_cut(samples, rate, threshold_db, window_ms, hop_ms, release_ms)
    fade_samples = int(rate * fade_ms / 1000)
    # Never fade/trim into content still above the floor - re-derive the
    # last-active point without the release margin as a hard floor.
    floor_cut = find_tail_cut(samples, rate, threshold_db, window_ms, hop_ms, 0)
    trimmed = apply_fade_and_trim(samples, cut_point, fade_samples, floor_cut)

    new_duration = len(trimmed) / rate
    saved = original_duration - new_duration
    print(f"{path}: {original_duration:.2f}s -> {new_duration:.2f}s "
          f"(trimmed {saved:.2f}s, {100 * saved / original_duration:.0f}%)"
          + (" [dry-run]" if dry_run else ""))

    if dry_run:
        return

    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(trimmed.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="WAV files to process (mono 16-bit PCM)")
    ap.add_argument("--threshold-db", type=float, default=-50.0,
                     help="amplitude level (dBFS relative to the file's own peak) considered negligible (default -50)")
    ap.add_argument("--window-ms", type=float, default=30.0, help="RMS envelope window size in ms (default 30)")
    ap.add_argument("--hop-ms", type=float, default=10.0, help="envelope hop size in ms (default 10)")
    ap.add_argument("--release-ms", type=float, default=80.0,
                     help="natural tail kept after the last point above threshold, before the fade starts (default 80)")
    ap.add_argument("--fade-ms", type=float, default=30.0, help="fade-out duration ending at exact zero (default 30)")
    ap.add_argument("--dry-run", action="store_true", help="report what would change without writing files")
    args = ap.parse_args()

    for path in args.files:
        try:
            process_file(path, args.threshold_db, args.window_ms, args.hop_ms,
                         args.release_ms, args.fade_ms, args.dry_run)
        except Exception as e:
            print(f"{path}: ERROR - {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
