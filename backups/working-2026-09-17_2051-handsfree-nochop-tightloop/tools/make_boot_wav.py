#!/usr/bin/env python3
"""
Convert Diana's theme (or any music file) into the boot jingle the Cardputer plays
during the welcome protocol: 16 kHz, mono, 16-bit PCM WAV, trimmed with a fade-out.

  python tools/make_boot_wav.py                 # uses the PC build's Pragmata theme
  python tools/make_boot_wav.py song.mp3 --seconds 20

Output: sdcard/diana/boot.wav   (~32 KB per second; 20 s = 640 KB)
Requires ffmpeg: found next to the PC Diana project (ffmpeg.exe) or on PATH.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PC_DIANA = os.path.join(os.path.dirname(ROOT), "Diana")
DEFAULT_SRC = os.path.join(PC_DIANA, "Pragmata Sketchbook OST - Title Screen _ Menu Theme.mp3")
OUT = os.path.join(ROOT, "sdcard", "diana", "boot.wav")


def find_ffmpeg():
    for c in (os.path.join(PC_DIANA, "ffmpeg.exe"), shutil.which("ffmpeg")):
        if c and os.path.exists(c):
            return c
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?", default=DEFAULT_SRC)
    ap.add_argument("--seconds", type=float, default=14.0, help="length to keep (default 14)")
    ap.add_argument("--start", type=float, default=0.0, help="start offset in the source")
    ap.add_argument("--gain", type=float, default=-3.0, help="gain in dB (default -3)")
    args = ap.parse_args()

    ff = find_ffmpeg()
    if not ff:
        sys.exit("ffmpeg not found (expected ../Diana/ffmpeg.exe or ffmpeg on PATH)")
    if not os.path.exists(args.source):
        sys.exit(f"source not found: {args.source}")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fade_start = max(0.0, args.seconds - 2.5)
    cmd = [ff, "-y", "-ss", str(args.start), "-t", str(args.seconds), "-i", args.source,
           "-ac", "1", "-ar", "16000", "-sample_fmt", "s16",
           "-af", f"volume={args.gain}dB,afade=t=out:st={fade_start}:d=2.5",
           "-f", "wav", OUT]
    print("+", " ".join(cmd))
    subprocess.check_call(cmd)
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    main()
