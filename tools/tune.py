#!/usr/bin/env python3
"""
Tune DIANA over the USB serial port — no typing on the tiny keyboard.

  python tools/tune.py                               # list every tunable with its range
  python tools/tune.py jitter_max_ms=6000 jitter_ms=1000
  python tools/tune.py jitter_max_ms=6000 --save     # set, then persist across reboots
  python tools/tune.py --save                        # persist what is set now
  python tools/tune.py --get jitter_ms               # print one value
  python tools/tune.py --preset slow-network         # a named group of settings
  python tools/tune.py --say "The quick brown fox." --repeat 3   # listening test with a report
  python tools/tune.py spool=0 --say "Compare me." ; python tools/tune.py spool=1 --say "Compare me."
  python tools/tune.py --sweep pop_fix=0,1,2,4,7 --say "Variant {v}."   # A/B by ear: she names each value
    (--sweep sets each value, waits --gap seconds so hands-free listening restarts, then speaks;
     the setting is put back to its original value afterwards)

Persistence: runtime tunables go to /diana/tune.fs on the SD card (loaded at boot, before
boot.fs); config-backed ones (vad, silence_ms, mic_gain, volume, brightness, idle_sleep)
go to config.json / NVS. Without --save a change lasts until the next reboot.

Needs pyserial. PlatformIO's own Python already has it:
  ~/.platformio/penv/bin/python tools/tune.py ...
or: pip install pyserial

The port is found automatically (/dev/cu.usbmodem* on macOS, /dev/ttyACM* on Linux,
the Espressif USB device on Windows); override with --port. Opening the port does not
reset the device.
"""
import argparse
import glob
import re
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python, or pip install pyserial")

PRESETS = {
    # measured 2026-09-25 on a 0.45-0.86x link: fewer pauses, longer wait before the first word
    "slow-network": {"spool": 1, "jitter_ms": 1000, "jitter_max_ms": 6000},
    # the shipped defaults
    "default":      {"spool": 1, "jitter_ms": 500, "jitter_max_ms": 4000, "speech_cps": 15,
                     "stream_chunk": 2400, "stream_prebuf": 1, "codec_hold": 1},
    # start talking as soon as possible; accept pauses on a slow link
    "fast-start":   {"spool": 1, "jitter_ms": 200, "jitter_max_ms": 1500},
    # the pre-spool behaviour, for A/B comparison
    "direct":       {"spool": 0},
}


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:                      # Espressif USB JTAG/serial (Cardputer ADV)
            return p.device
    for pat in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


class Diana:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)

    def _read_until(self, done, timeout):
        # The console is served between turns, so while DIANA is speaking or thinking the
        # answer can take a while. Say so rather than failing.
        buf, start, end, told = "", time.time(), time.time() + timeout, False
        while time.time() < end:
            if not told and not buf and time.time() - start > 3:
                print("(DIANA is busy - waiting for her to finish...)", file=sys.stderr)
                told = True
            chunk = self.s.read(4096).decode("utf-8", "replace")
            if chunk:
                buf += chunk
                if done(buf):
                    time.sleep(0.2)
                    buf += self.s.read(4096).decode("utf-8", "replace")
                    break
        return buf

    def send(self, line, done, timeout=30.0):
        self.s.reset_input_buffer()
        self.s.write((line + "\n").encode())
        return self._read_until(done, timeout)

    def tune_lines(self, out):
        return [l for l in out.splitlines() if l.startswith("[TUNE]")]


def main():
    ap = argparse.ArgumentParser(description="Tune DIANA over USB serial.")
    ap.add_argument("settings", nargs="*", help="name=value pairs")
    ap.add_argument("--port")
    ap.add_argument("--save", action="store_true", help="persist across reboots")
    ap.add_argument("--get", metavar="NAME")
    ap.add_argument("--preset", choices=sorted(PRESETS))
    ap.add_argument("--say", metavar="TEXT", help="speak TEXT and report delay and pauses")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--sweep", metavar="NAME=V1,V2,...", help="try each value in turn with --say ({v} = value)")
    ap.add_argument("--gap", type=float, default=4.0, help="seconds between sweep steps (default 4)")
    a = ap.parse_args()

    port = a.port or find_port()
    if not port:
        sys.exit("no DIANA serial port found (is the Cardputer on and plugged in?); use --port")
    d = Diana(port)

    pairs = []
    if a.preset:
        pairs += [f"{k}={v}" for k, v in PRESETS[a.preset].items()]
    for s in a.settings:
        if not re.fullmatch(r"[A-Za-z_]+=-?\d+", s):
            sys.exit(f"bad setting '{s}' (use name=value, e.g. jitter_ms=1000)")
        pairs.append(s)

    if pairs:
        out = d.send("!tune " + " ".join(pairs),
                     lambda b: len([l for l in b.splitlines() if l.startswith("[TUNE]")]) >= len(pairs))
        for l in d.tune_lines(out):
            print(l[7:])
        if any("ERR" in l for l in d.tune_lines(out)):
            sys.exit(1)

    if a.save:
        out = d.send("!tune save", lambda b: "[TUNE]" in b)
        print("\n".join(l[7:] for l in d.tune_lines(out)) or "no answer to !tune save")

    if a.get:
        # retry once: serial output right after a reboot or mid-reply can drop lines
        for attempt in range(2):
            out = d.send("!tune", lambda b: "[TUNE] END" in b)
            vals = {}
            for l in d.tune_lines(out):
                m = re.match(r"\[TUNE\] (\w+)=(-?\d+)", l)
                if m:
                    vals[m.group(1).lower()] = m.group(2)
            if a.get.lower() in vals:
                print(vals[a.get.lower()])
                break
            if "[TUNE] END" in out and len(vals) > 5:
                sys.exit(f"no tunable '{a.get}'")      # complete list, genuinely absent
        else:
            sys.exit(f"could not read '{a.get}' (no complete answer from DIANA)")

    if a.sweep:
        m = re.fullmatch(r"([A-Za-z_]+)=(-?\d+(?:,-?\d+)*)", a.sweep)
        if not m or not a.say:
            sys.exit("use --sweep name=v1,v2,... together with --say \"text with {v}\"")
        name, vals = m.group(1), m.group(2).split(",")
        out = d.send("!tune", lambda b: "[TUNE] END" in b)
        orig = next((re.match(r"\[TUNE\] \w+=(-?\d+)", l).group(1) for l in d.tune_lines(out)
                     if l.startswith(f"[TUNE] {name}=")), None)
        if orig is None:
            sys.exit(f"no tunable '{name}'")
        for v in vals:
            d.send(f"!tune {name}={v}", lambda b: "[TUNE]" in b)
            time.sleep(a.gap)                        # let hands-free listening restart: the real switch
            out = d.send("!say " + a.say.replace("{v}", v), lambda b: "[SAY] done" in b, timeout=90)
            tts = re.search(r"heard=(\d+)ms.*underruns=(\d+)", out)
            print(f"{name}={v}: " + (f"heard after {int(tts.group(1)) / 1000:.1f}s, {tts.group(2)} pauses" if tts else "no [TTS] report"))
        d.send(f"!tune {name}={orig}", lambda b: "[TUNE]" in b)
        print(f"{name} restored to {orig}")
        a.say = None                                  # handled

    if a.say:
        for i in range(a.repeat):
            out = d.send("!say " + a.say, lambda b: "[SAY] done" in b, timeout=90)
            spool = re.search(r"\[AUDIO\] spool ([\d.]+)s audio, start after (\d+)ms .*pauses=(\d+)", out)
            tts = re.search(r"heard=(\d+)ms total=(\d+)ms underruns=(\d+)", out)
            if tts:
                audio = f"{spool.group(1)}s audio, " if spool else ""
                mode = "spool" if spool else "direct"
                print(f"say #{i + 1}: {audio}heard after {int(tts.group(1)) / 1000:.1f}s, "
                      f"{tts.group(3)} {'pauses' if spool else 'clicks'} ({mode})")
            else:
                print(f"say #{i + 1}: no [TTS] report (offline? voice off?)")
            if i + 1 < a.repeat:
                time.sleep(2)

    if not (pairs or a.save or a.get or a.say or a.sweep):
        out = d.send("!tune", lambda b: "[TUNE] END" in b)
        rows = [l[7:] for l in d.tune_lines(out) if not l.endswith("END")]
        if not rows:
            sys.exit("no answer - is this DIANA firmware with !tune (develop, 2026-09-25 or later)?")
        for r in rows:
            print(r)


if __name__ == "__main__":
    main()
