#!/usr/bin/env python3
"""
Guided listening test for the pop before DIANA speaks. Run it with the Cardputer
beside you; each case is announced here, then she says "Case N." Note which case
numbers popped (the pop, if any, comes just BEFORE her words).

  ~/.platformio/penv/bin/python tools/pop_test.py
  ~/.platformio/penv/bin/python tools/pop_test.py --cases 1,5,9   # just some

Everything it changes is put back at the end (pop_fix, hands-free).
"""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tune as T  # reuse the port finder and serial helper

CASES = [
    # (label, pop_fix, hands_free)
    ("today's behaviour, after listening",               0, True),
    ("clocks kept on",                                   1, True),
    ("DAC muted across the switch",                      2, True),
    ("clocks on + muted",                                3, True),
    ("output driver gated",                              4, True),
    ("clocks on + muted + driver gated",                 7, True),
    ("volume never moves (no ramp)",                     8, True),
    ("no ramp + clocks on",                              9, True),
    ("no ramp + driver gated",                          12, True),
    ("no ramp + clocks on + driver gated",              13, True),
    ("today's behaviour, mic OFF (speaker only)",        0, False),
    ("no ramp, mic OFF (speaker only)",                  8, False),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--cases", help="comma list of case numbers (default all)")
    ap.add_argument("--gap", type=float, default=4.0)
    a = ap.parse_args()
    port = a.port or T.find_port()
    if not port:
        sys.exit("no DIANA serial port found")
    d = T.Diana(port)
    out = d.send("!tune", lambda b: "[TUNE] END" in b)
    orig = re.search(r"\[TUNE\] pop_fix=(\d+)", out)
    if not orig:
        sys.exit("this firmware has no pop_fix tunable - flash develop first")
    orig = orig.group(1)
    pick = set(int(x) for x in a.cases.split(",")) if a.cases else None
    hands = True
    print("Listen for a pop just BEFORE each 'Case N'. Note the numbers that popped.\n")
    try:
        for n, (label, pf, hf) in enumerate(CASES, 1):
            if pick and n not in pick:
                continue
            if hf != hands:
                d.send(f"!tune pop_fix={pf}", lambda b: "[TUNE]" in b)
                d.send("/handsfree " + ("on" if hf else "off"), lambda b: "Hands-free" in b or "hands" in b.lower(), timeout=10)
                hands = hf
            d.send(f"!tune pop_fix={pf}", lambda b: "[TUNE]" in b)
            print(f"Case {n:2d}: {label}  (pop_fix={pf}, mic {'on' if hf else 'off'})", flush=True)
            time.sleep(a.gap)
            d.send(f"!say Case {n}.", lambda b: "[SAY] done" in b, timeout=90)
            time.sleep(1.5)
    finally:
        d.send(f"!tune pop_fix={orig}", lambda b: "[TUNE]" in b)
        if not hands:
            d.send("/handsfree on", lambda b: "Hands-free" in b, timeout=10)
        print(f"\nrestored pop_fix={orig}, hands-free on")


if __name__ == "__main__":
    main()
