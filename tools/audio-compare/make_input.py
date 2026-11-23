#!/usr/bin/env python3
"""Writes a deterministic input script (mesen-headless --input format) that gets through a game's
menus and then plays: START every MENU_STEP frames until --play-from, then a fixed, repeating
pattern of directions and attack buttons (A/B/C, a few frames each) so games make sound effects.

    make_input.py OUT.json --frames N [--play-from F] [--menu-step 90]
"""
import argparse
import json

PATTERN = [["A"], [], ["B"], [], ["C"], [], ["RIGHT", "A"], [], ["LEFT", "B"], [], ["DOWN", "C"], [],
           ["UP"], [], ["A", "B"], [], ["RIGHT"], ["RIGHT"], ["C"], [], ["START"], []]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--play-from", type=int, default=1800)
    ap.add_argument("--menu-step", type=int, default=90)
    ap.add_argument("--hold", type=int, default=6)
    ap.add_argument("--step", type=int, default=12)
    args = ap.parse_args()
    frames = {}
    for f in range(args.menu_step, args.play_from, args.menu_step):
        frames[str(f)] = {"1": ["START"]}
        frames[str(f + args.hold)] = {"1": []}
    i = 0
    for f in range(args.play_from, args.frames, args.step):
        buttons = PATTERN[i % len(PATTERN)]
        if buttons == ["START"] and (i // len(PATTERN)) % 4 != 3:
            buttons = []  # START only now and then (continue screens), not as a pause toggle every cycle
        frames[str(f)] = {"1": buttons}
        frames[str(f + args.hold)] = {"1": []}
        i += 1
    with open(args.out, "w") as fh:
        json.dump({"frames": frames}, fh)


if __name__ == "__main__":
    main()
