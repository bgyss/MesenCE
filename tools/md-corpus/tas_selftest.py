#!/usr/bin/env python3
"""tas.py self-test: a .bk2 and a .gmv written from a known input must convert back to it.

The movies are made here in each format's documented layout (tas.py's docstring); this checks the
converters' column mapping, frame numbering and change-event compression, not agreement with
BizHawk or Gens themselves (that needs a published movie).

Usage: tas_selftest.py [--write-bk2 OUT.bk2 --sha1 ROM_SHA1 --input INPUT.json --frames N]
  (--write-bk2: write a .bk2 replaying INPUT.json, for an end-to-end run of `run --session tas`)
"""
import argparse
import io
import json
import pathlib
import random
import sys
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import tas  # noqa: E402

BK2_ORDER = ("Up", "Down", "Left", "Right", "A", "B", "C", "Start", "X", "Y", "Z", "Mode")


def frames_from_input(data, count):
    """mesen-headless input JSON -> [ {port: set} per frame ]"""
    events = {int(f): pads for f, pads in data["frames"].items()}
    held, frames = {}, []
    for i in range(1, count + 1):
        for port, names in events.get(i, {}).items():
            held[int(port)] = set(names)
        frames.append({p: set(s) for p, s in held.items() if s})
    return frames


def write_bk2(frames, sha1, six=False):
    key = "LogKey:#Reset|Power|" + "".join("#" + "|".join("P%d %s" % (p, b) for b in BK2_ORDER) + "|" for p in (1, 2))
    lines = ["[Input]", key]
    for pads in frames:
        cols = ["..",]
        for p in (1, 2):
            cols.append("".join(b[0] if b.upper() in pads.get(p, ()) else "." for b in BK2_ORDER))
        lines.append("|" + "|".join(cols) + "|")
    lines.append("[/Input]")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("Header.txt", "MovieVersion BizHawk v2.0\nPlatform GEN\nGameName Test\nSHA1 %s\nCore Genplus-gx\n" % sha1.upper())
        z.writestr("SyncSettings.json", json.dumps({"o": {"UseSixButton": six, "Region": "Autodetect"}}))
        z.writestr("Input Log.txt", "\n".join(lines) + "\n")
    return buf.getvalue()


def write_gmv(frames, six=False):
    head = bytearray(0x40)
    head[0:16] = b"Gens Movie TESTA"
    head[0x14:0x16] = b"66" if six else b"33"
    out = bytearray(head)
    names = ("UP", "DOWN", "LEFT", "RIGHT", "A", "B", "C", "START")
    for pads in frames:
        b = [0xFF, 0xFF, 0xFF]
        for i, p in enumerate((1, 2)):
            for bit, n in enumerate(names):
                if n in pads.get(p, ()):
                    b[i] &= ~(1 << bit)
            for bit, n in enumerate(("X", "Y", "Z", "MODE")):
                if n in pads.get(p, ()):
                    b[2] &= ~(1 << (bit + 4 * i))
        out += bytes(b)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write-bk2")
    ap.add_argument("--sha1")
    ap.add_argument("--input")
    ap.add_argument("--frames", type=int, default=3600)
    args = ap.parse_args()
    if args.write_bk2:
        frames = frames_from_input(json.loads(pathlib.Path(args.input).read_text()), args.frames)
        pathlib.Path(args.write_bk2).write_bytes(write_bk2(frames, args.sha1))
        return 0

    rng = random.Random(1)
    buttons = tas.BUTTONS
    frames = []
    for _ in range(500):
        if not frames or rng.random() < 0.1:
            pads = {p: set(rng.sample(buttons, rng.randint(0, 3))) for p in (1, 2)}
            pads = {p: s for p, s in pads.items() if s}
        frames.append(pads)
    expected = tas.events_from_frames(frames)
    failed = False
    for name, movie in (("bk2", tas.parse_bk2(write_bk2(frames, "ab" * 20, six=True))),
                        ("gmv", tas.parse_gmv(write_gmv(frames, six=True)))):
        got = tas.events_from_frames(movie["frames"])
        ok = got == expected and len(movie["frames"]) == len(frames) and movie["pads"] == 6
        failed |= not ok
        print("%s tas %s: %d frames, %d input events" % ("ok  " if ok else "FAIL", name, len(movie["frames"]), len(got["frames"])))
    # Round trip through mesen-headless' input format
    again = tas.events_from_frames(frames_from_input(expected, len(frames)))
    ok = again == expected
    failed |= not ok
    print("%s tas input.json round trip" % ("ok  " if ok else "FAIL"))
    # GPGX's per-movie input shift (runner.calibrate_tas): later, earlier (collapsing onto frame 1)
    import runner
    script = {"frames": {"1": {"1": ["A"]}, "2": {"1": ["B"]}, "10": {"1": []}}}
    ok = (runner.shift_input(script, 2) == {"frames": {"3": {"1": ["A"]}, "4": {"1": ["B"]}, "12": {"1": []}}}
          and runner.shift_input(script, -2) == {"frames": {"1": {"1": ["B"]}, "8": {"1": []}}}
          and runner.shift_input(script, 0) == script)
    failed |= not ok
    print("%s tas input shift" % ("ok  " if ok else "FAIL"))
    failed |= not audio_base_check()
    failed |= not audio_limit_check()
    return 1 if failed else 0


def audio_limit_check():
    """tools/audio-compare limit_seconds: the reference matches MesenCE for 8 s, then plays something
    else (as after a state divergence, findings C9). Compared up to 7 s the spectrum matches; over
    the whole run it is flagged."""
    import tempfile
    import wave
    import numpy as np
    import compare
    audio_compare, run_library = compare.audio_modules()
    rate, seconds = 8000, 14
    rng = np.random.default_rng(2)
    env = np.repeat(rng.uniform(0.2, 1.0, seconds * 4), rate // 4)
    noise = rng.standard_normal(rate * seconds) * 4000 * env
    t = np.arange(rate * seconds) / rate
    other = np.sin(2 * np.pi * 600 * t) * 12000 * env
    ref = np.where(t < 8, noise, other)
    run = pathlib.Path(tempfile.mkdtemp())
    stereo = lambda x: np.repeat(x.clip(-32768, 32767).astype("<i2"), 2).tobytes()
    (run / "pwm.raw").write_bytes(stereo(noise))
    (run / "manifest.json").write_text(json.dumps({"chip_audio": {"sample_rate": rate}}))
    for name, data in (("audio.wav", noise), ("reference.wav", ref)):
        with wave.open(str(run / name), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(stereo(data))
    cut = audio_compare.compare(run, run / "reference.wav", 1.0, 7.0)
    whole = audio_compare.compare(run, run / "reference.wav", 1.0)
    ok = "spectrum" not in run_library.flags(cut) and "spectrum" in run_library.flags(whole)
    print("%s audio limited to the matching part: %.2f dB, whole run %.2f dB" % ("ok  " if ok else "FAIL", cut["spectral_distance_db"], whole["spectral_distance_db"]))
    return ok


def audio_base_check():
    """tools/audio-compare: when the fit gives FM no weight (the reference is PWM only), the levels
    are based on the next chip, not on FM's zero weight (Tempo read pwm +124 dB, findings C7)."""
    import tempfile
    import wave
    import numpy as np
    import compare
    audio_compare, run_library = compare.audio_modules()
    rate, seconds = 8000, 12
    rng = np.random.default_rng(1)
    env = np.repeat(rng.uniform(0.1, 1.0, seconds * 4), rate // 4)
    pwm = (rng.standard_normal(rate * seconds) * 6000 * env).clip(-32768, 32767)
    fm = (rng.standard_normal(rate * seconds) * 50).clip(-32768, 32767)
    run = pathlib.Path(tempfile.mkdtemp())
    stereo = lambda x: np.repeat(x.astype("<i2"), 2).tobytes()
    (run / "ym2612.raw").write_bytes(stereo(fm))
    (run / "pwm.raw").write_bytes(stereo(pwm))
    (run / "manifest.json").write_text(json.dumps({"chip_audio": {"sample_rate": rate}}))
    for name, data in (("audio.wav", fm + pwm), ("reference.wav", pwm)):
        with wave.open(str(run / name), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(stereo(data.clip(-32768, 32767)))
    r = audio_compare.compare(run, run / "reference.wav", 1.0)
    level = r["chip_level_db_vs_mesen"].get("pwm", 0.0)
    ok = abs(level) < 1.0 and "pwm-level" not in run_library.flags(r)
    print("%s audio level base without FM: pwm %+.1f dB" % ("ok  " if ok else "FAIL", level))
    return ok


if __name__ == "__main__":
    sys.exit(main())
