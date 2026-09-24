#!/usr/bin/env python3
"""
Package DIANA for the Cardputer ADV.

  python tools/package_sd.py            # build with PlatformIO, then package
  python tools/package_sd.py --no-build # package the last build

Produces:
  sdcard/Diana.bin                          app image for M5Launcher (copy sdcard/* to the SD root)
  build/Diana-cardputer-adv-<ver>.bin       same app image
  build/Diana-cardputer-adv-<ver>-full.bin  merged full-flash image for esptool / web flashers (offset 0x0)
  build/Diana-SD-<ver>.zip                  the sdcard/ folder zipped
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = "cardputer-adv"
BUILD_DIR = os.path.join(ROOT, ".pio", "build", ENV)
OUT_DIR = os.path.join(ROOT, "build")
SD_DIR = os.path.join(ROOT, "sdcard")


def version():
    ini = open(os.path.join(ROOT, "platformio.ini"), encoding="utf-8").read()
    m = re.search(r"DIANA_VERSION='\"([^\"]+)\"'", ini)
    return m.group(1) if m else "dev"


def run(cmd, **kw):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=ROOT, **kw)


def find_esptool():
    home = os.path.expanduser("~")
    for c in (os.path.join(home, ".platformio", "packages", "tool-esptoolpy", "esptool.py"),):
        if os.path.exists(c):
            return c
    return None


def find_pio_python():
    """esptool's dependencies (intelhex, cryptography) live in PlatformIO's own venv,
    not necessarily in the interpreter running this script."""
    home = os.path.expanduser("~")
    for c in (os.path.join(home, ".platformio", "penv", "bin", "python"),
              os.path.join(home, ".platformio", "penv", "Scripts", "python.exe")):
        if os.path.exists(c):
            return c
    return sys.executable


def find_boot_app0():
    home = os.path.expanduser("~")
    hits = glob.glob(os.path.join(home, ".platformio", "packages", "framework-arduinoespressif32*", "tools", "partitions", "boot_app0.bin"))
    return hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    if not args.no_build:
        pio = shutil.which("pio") or shutil.which("platformio")
        if not pio:
            sys.exit("PlatformIO not found in PATH (pip install platformio)")
        run([pio, "run", "-e", ENV])

    fw = os.path.join(BUILD_DIR, "firmware.bin")
    if not os.path.exists(fw):
        sys.exit(f"missing {fw} - build first")

    ver = version()
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(os.path.join(SD_DIR, "diana"), exist_ok=True)

    app_out = os.path.join(OUT_DIR, f"Diana-cardputer-adv-{ver}.bin")
    shutil.copyfile(fw, app_out)
    shutil.copyfile(fw, os.path.join(SD_DIR, "Diana.bin"))
    print(f"app image: {app_out} ({os.path.getsize(fw)} bytes)")

    # merged full-flash image (bootloader 0x0, partitions 0x8000, boot_app0 0xe000, app 0x10000)
    esptool = find_esptool()
    boot_app0 = find_boot_app0()
    bl = os.path.join(BUILD_DIR, "bootloader.bin")
    pt = os.path.join(BUILD_DIR, "partitions.bin")
    if esptool and boot_app0 and os.path.exists(bl) and os.path.exists(pt):
        full = os.path.join(OUT_DIR, f"Diana-cardputer-adv-{ver}-full.bin")
        try:
            run([find_pio_python(), esptool, "--chip", "esp32s3", "merge_bin", "-o", full,
                 "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "8MB",
                 "0x0", bl, "0x8000", pt, "0xe000", boot_app0, "0x10000", fw])
            print(f"full image: {full} ({os.path.getsize(full)} bytes)")
        except subprocess.CalledProcessError as e:
            print(f"WARNING: merged image not built ({e}); the SD package below is unaffected")
    else:
        print("skipping merged image (esptool / boot_app0 not found)")

    zpath = os.path.join(OUT_DIR, f"Diana-SD-{ver}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for dirpath, _, files in os.walk(SD_DIR):
            for f in files:
                p = os.path.join(dirpath, f)
                z.write(p, os.path.relpath(p, SD_DIR))
    print(f"SD package: {zpath}")
    print("\nCopy the contents of sdcard/ to the ROOT of a FAT32 microSD card, edit diana/config.json, "
          "then install Diana.bin from M5Launcher (OTA > SD card).")


if __name__ == "__main__":
    main()
