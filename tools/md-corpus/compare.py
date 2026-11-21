"""`md_corpus.py compare`: compare every finished session's runs and write results/<session>/<id>.json.

Levels (docs/research/md-oracle-testing-strategy.md, section 2.4):

L0 health, per emulator, from its own outputs: exit code; "frozen" (work RAM and frame unchanged
over the last 10 s); "black" (the last snapshot is black); "no-input" (MesenCE only: the game
stopped reading its pads for the last 20 s).

L2 state, per pair of emulators, from the per-block work RAM hashes (--state-blocks): for each
frame of the first run, the frame of the second within a small window around the current offset
whose 256 block hashes agree best. The offset follows lag differences (it may drift a few frames,
e.g. while a game loads); `offset_changes` counts the drifts. Blocks that differ in more than 20 %
of otherwise well-matched frames are "noisy" (stacks, timers caught mid-update, sprite tables)
and left out of `agreement` (equal non-noisy blocks / non-noisy blocks). `diverged_at` is the first
frame from which the 2 s rolling agreement stays below 0.75 without recovering to 0.9.

L1 functional: snapshots taken at the same frame numbers, mapped back to the VDP's 15 colour steps
(normal level k = step 2k, shadow k = k, highlight k = 7 + k: MesenCE's ladder in
Core/MD/MdDefaultVideoFilter.h; GPGX and PicoDrive output RGB565 on a linear ladder whose top
step is 232), downscaled to 80x56 and compared as mean absolute difference in steps / 14 (0-1);
plus audio through tools/audio-compare against the main oracle.

Verdict (section 2.6): a MesenCE finding counts when the oracles agree with each other more than
with MesenCE. The thresholds below are judgment calls, to be tuned on real results.
"""
import concurrent.futures
import json
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]

AUDIO_FILES = ("audio.wav", "ym2612.raw", "psg.raw", "pwm.raw", "pcm.raw", "cdda.raw")
EMULATORS = ("mesen", "gpgx", "picodrive")  # run directories; renamed leftovers (*.stale-*) are ignored
MAIN_ORACLE = {"md": "gpgx", "mcd": "gpgx", "32x": "picodrive", "mcd32x": "picodrive"}
WINDOW = 120          # frames in the rolling agreement (2 s)
FROZEN_FRAMES = 600   # 10 s
NO_INPUT_FRAMES = 1200

# Known library-wide chip level differences against the main oracle, in dB (the reference's level
# relative to MesenCE's, FM = 0), subtracted before flagging so that per-title outliers stand out.
# Judgment calls with no hardware measurement behind either side; see the findings (C1).
KNOWN_LEVEL_DB = {"mcd": {"pcm": 7.2}, "md": {"psg": -2.8}}

SCORES = {"mesen-failed": 100, "mesen-frozen": 80, "mesen-black": 80, "mesen-no-input": 70, "state-diverges": 60,
          "state-diverges-single-oracle": 25, "visual": 40, "audio": 30, "audio-not-compared": 5, "unsupported": 5}


def read_log(run):
    path = run / "state.log"
    if not path.exists():
        return None
    lines = path.read_text().splitlines()
    cols = lines[1][2:].split()
    rows = [l.split() for l in lines[2:]]
    return {c: [r[i] for r in rows] for i, c in enumerate(cols) if all(len(r) > i for r in rows)}


def read_blocks(run):
    path = run / "state.log.blocks"
    if not path.exists() or path.stat().st_size < 512:
        return None
    return np.fromfile(path, dtype="<u2").reshape(-1, 256)


def health(run, log, is_mesen):
    flags = []
    result = json.loads((run / "run.json").read_text())
    if result["exit"] != 0:
        flags.append("failed")
    if log and len(log.get("frame", [])) > FROZEN_FRAMES:
        tail_ram = set(log["MdWorkRam"][-FROZEN_FRAMES:]) if "MdWorkRam" in log else {None}
        tail_frame = set(log["frame"][-FROZEN_FRAMES:])
        if len(tail_ram) == 1 and len(tail_frame) == 1:
            flags.append("frozen")
            # Where the freeze began: the state tracker can't date it (frozen RAM still matches
            # the oracles' slowly changing RAM at some offset; Fatal Rewind froze at 314 and was
            # reported as diverging at 1727)
            ram, frame = log.get("MdWorkRam", log["frame"]), log["frame"]
            start = len(frame) - 1
            while start > 0 and ram[start - 1] == ram[-1] and frame[start - 1] == frame[-1]:
                start -= 1
            frozen_from = start + 1
        if is_mesen and "polled" in log and len(log["polled"]) > NO_INPUT_FRAMES:
            polled = [p == "1" for p in log["polled"]]
            if any(polled) and not any(polled[-NO_INPUT_FRAMES:]):
                flags.append("no-input")
    snaps = sorted((run / "snaps").glob("*.png")) if (run / "snaps").exists() else []
    if snaps and float(image(snaps[-1], run.name).max()) < 0.06:
        flags.append("black")
    out = {"exit": result["exit"], "wall_seconds": result["wall_seconds"], "flags": flags,
           "frames_logged": len(log["frame"]) if log else 0}
    if "frozen" in flags:
        out["frozen_from"] = frozen_from
    return out


MESEN_STEPS = np.array([0, 29, 52, 70, 87, 101, 116, 130, 144, 158, 172, 187, 206, 228, 255], dtype=np.float32)


def steps(rgb, emulator):
    """8-bit channel values -> VDP colour steps 0-14 (fractional after downscaling)."""
    if emulator == "mesen":
        return np.interp(rgb, MESEN_STEPS, np.arange(15, dtype=np.float32))
    return np.clip(rgb / 232.0 * 14.0, 0, 14)


def image(path, emulator="mesen", size=(80, 56)):
    from PIL import Image
    with Image.open(path) as im:
        rgb = np.asarray(im.convert("RGB"), dtype=np.float32)
    s = steps(rgb, emulator) / 14.0
    small = Image.fromarray((s * 255).astype(np.uint8)).resize(size, Image.BOX)
    return np.asarray(small, dtype=np.float32) / 255.0


def visual(a, b):
    sa = {p.name: p for p in (a / "snaps").glob("*.png")} if (a / "snaps").exists() else {}
    sb = {p.name: p for p in (b / "snaps").glob("*.png")} if (b / "snaps").exists() else {}
    common = sorted(set(sa) & set(sb))
    if not common:
        return None
    diffs = [float(np.abs(image(sa[n], a.name) - image(sb[n], b.name)).mean()) for n in common]
    return {"median": round(float(np.median(diffs)), 3), "last": round(diffs[-1], 3), "per_snap": [round(d, 3) for d in diffs],
            "frames": [int(n.split(".")[0]) for n in common]}


def track(A, B, max_step=4, wide=60):
    """Per frame of A: best matching frame offset into B and the number of equal blocks."""
    n = len(A)
    offsets = np.zeros(n, dtype=np.int32)
    agree = np.zeros(n, dtype=np.int32)
    d = 0
    for f in range(n):
        best, best_d = -1, d
        for cand in range(d - max_step, d + max_step + 1):
            g = f + cand
            if 0 <= g < len(B):
                eq = int((A[f] == B[g]).sum())
                if eq > best or (eq == best and abs(cand - d) < abs(best_d - d)):
                    best, best_d = eq, cand
        if best < 128:
            for cand in range(-wide, wide + 1):
                g = f + cand
                if 0 <= g < len(B):
                    eq = int((A[f] == B[g]).sum())
                    if eq > best:
                        best, best_d = eq, cand
        # Bounded: with static RAM many offsets tie, and the walk must not run away
        best_d = max(-wide, min(wide, best_d))
        offsets[f], agree[f], d = best_d, max(best, 0), best_d
    return offsets, agree


def state_pair(A, B):
    if A is None or B is None:
        return None
    offsets, agree = track(A, B)
    n = len(A)
    valid = np.array([0 <= f + offsets[f] < len(B) for f in range(n)])
    diff = np.zeros((n, 256), dtype=bool)
    for f in np.where(valid)[0]:
        diff[f] = A[f] != B[f + offsets[f]]
    good = valid & (agree >= 200)
    rate = diff[good].mean(axis=0) if good.any() else np.zeros(256)
    noisy = rate > 0.20
    keep = ~noisy
    clean = np.where(valid, (~diff[:, keep]).sum(axis=1) / max(int(keep.sum()), 1), 0.0)
    rolling = np.convolve(clean, np.ones(WINDOW) / WINDOW, mode="valid") if n >= WINDOW else clean
    diverged = None
    low = np.where(rolling < 0.75)[0]
    for f in low:
        if not (rolling[f:] >= 0.9).any():
            diverged = int(f) + 1
            break
    changes = int((np.diff(offsets) != 0).sum())
    return {"agreement_median": round(float(np.median(clean)), 3), "agreement_p10": round(float(np.percentile(clean, 10)), 3),
            "lockstep": round(float((clean >= 0.999).mean()), 3), "diverged_at": diverged,
            "offset_final": int(offsets[-1]), "offset_changes": changes, "offset_max": int(np.abs(offsets).max()),
            "noisy_blocks": ["%06x" % (0xFF0000 + b * 256) for b in np.where(noisy)[0]],
            "rolling_2s": [round(float(x), 2) for x in rolling[::60]]}


_AUDIO = {}


def audio_modules():
    """tools/audio-compare's compare.py and run_library.py, loaded by path: this module is also
    called compare, and run_library imports its sibling by that name."""
    if not _AUDIO:
        import importlib.util
        for name, file in (("audio_compare", "compare.py"), ("audio_run_library", "run_library.py")):
            spec = importlib.util.spec_from_file_location(name, ROOT / "tools/audio-compare" / file)
            module = importlib.util.module_from_spec(spec)
            saved = sys.modules.get("compare")
            sys.modules["compare"] = _AUDIO.get("audio_compare", saved)
            try:
                spec.loader.exec_module(module)
            finally:
                sys.modules["compare"] = saved
            _AUDIO[name] = module
    return _AUDIO["audio_compare"], _AUDIO["audio_run_library"]


def audio(mesen, oracle):
    audio_compare, run_library = audio_modules()
    wav = oracle / "audio.wav"
    if not (mesen / "audio.wav").exists() or not wav.exists():
        # Audio is deleted after a comparison: re-running one emulator of a session needs the others
        # re-run too (move the whole session directory aside)
        return {"error": "audio missing (%s)" % ", ".join(str(p) for p in (mesen / "audio.wav", wav) if not p.exists())}
    try:
        r = audio_compare.compare(mesen, wav, 5.0)
    except Exception as ex:  # unreadable or too short
        return {"error": str(ex)}
    for k in ("run", "reference", "missing_windows", "extra_windows"):
        r.pop(k, None)
    return r


def audio_flags(a, system):
    """tools/audio-compare's flags, after removing the known level differences."""
    if not a or a.get("error") or a.get("silent"):
        return []
    _, run_library = audio_modules()
    adjusted = dict(a)
    known = KNOWN_LEVEL_DB.get(system, {})
    adjusted["chip_level_db_vs_mesen"] = {k: v - known.get(k, 0.0) for k, v in a["chip_level_db_vs_mesen"].items()}
    return run_library.flags(adjusted)


def compare_session(sdir, keep_audio, previous=None):
    """previous: this session's earlier result, if any (its audio metrics are reused when the audio
    files are gone)."""
    entry = json.loads((sdir / "entry.json").read_text())
    emus = [p.name for p in sorted(sdir.iterdir()) if p.name in EMULATORS and (p / "run.json").exists()]
    if "mesen" not in emus:
        return None
    logs = {e: read_log(sdir / e) for e in emus}
    blocks = {e: read_blocks(sdir / e) for e in emus}
    out = {"id": entry["id"], "title": entry["title"], "system": entry["system"], "region": entry["region"],
           "mesen_build": json.loads((sdir / "mesen" / "run.json").read_text()).get("build"),
           "health": {e: health(sdir / e, logs[e], e == "mesen") for e in emus}, "state": {}, "visual": {}}
    oracles = [e for e in emus if e != "mesen"]
    for o in oracles:
        out["state"]["mesen~" + o] = state_pair(blocks["mesen"], blocks[o])
        out["visual"]["mesen~" + o] = visual(sdir / "mesen", sdir / o)
    if len(oracles) >= 2:
        out["state"]["%s~%s" % (oracles[0], oracles[1])] = state_pair(blocks[oracles[0]], blocks[oracles[1]])
        out["visual"]["%s~%s" % (oracles[0], oracles[1])] = visual(sdir / oracles[0], sdir / oracles[1])
    main = MAIN_ORACLE[entry["system"]] if MAIN_ORACLE[entry["system"]] in oracles else (oracles[0] if oracles else None)
    out["audio"] = audio(sdir / "mesen", sdir / main) if main else None
    if out["audio"] and out["audio"].get("error") and previous and previous.get("audio") and not previous["audio"].get("error"):
        # The audio went after the earlier comparison; its metrics still hold for the same runs
        out["audio"] = previous["audio"]
    if out["audio"]:
        out["audio"]["flags"] = audio_flags(out["audio"], entry["system"])
    out["main_oracle"] = main
    out["flags"] = verdict(out, oracles, main)
    out["score"] = score(out)
    if not keep_audio:
        for e in emus:
            for name in AUDIO_FILES:
                p = sdir / e / name
                if p.exists():
                    p.unlink()
    return out


def verdict(r, oracles, main):
    flags = []
    mh = r["health"]["mesen"]["flags"]
    oracle_flags = [set(r["health"][o]["flags"]) for o in oracles]
    for f in ("failed", "frozen", "black"):
        if f in mh and not any(f in of for of in oracle_flags):
            flags.append("mesen-" + f)
    if "no-input" in mh:
        flags.append("mesen-no-input")
    if r["system"] == "mcd32x":
        flags.append("unsupported")  # Sega CD 32X is not emulated (strategy 10.1)
    pairs = {o: r["state"].get("mesen~" + o) for o in oracles}
    if len(oracles) >= 2:
        base = r["state"].get("%s~%s" % (oracles[0], oracles[1]))
        mesen_worse = all(p and base and (
            (p["diverged_at"] is not None and (base["diverged_at"] is None or p["diverged_at"] + 300 < base["diverged_at"]))
            or p["agreement_median"] < base["agreement_median"] - 0.1) for p in pairs.values())
        if mesen_worse:
            flags.append("state-diverges")
        vb = r["visual"].get("%s~%s" % (oracles[0], oracles[1]))
        vm = [r["visual"].get("mesen~" + o) for o in oracles]
        if vb and all(v and v["median"] > max(0.08, vb["median"] + 0.05) for v in vm):
            flags.append("visual")
    elif oracles:
        p = pairs[oracles[0]]
        if p and p["diverged_at"] is not None:
            flags.append("state-diverges-single-oracle")
        v = r["visual"].get("mesen~" + oracles[0])
        if v and v["median"] > 0.12:
            flags.append("visual")
    if r.get("audio") and r["audio"].get("flags"):
        flags.append("audio")
    if r.get("audio") and r["audio"].get("error"):
        flags.append("audio-not-compared")
    return flags


def score(r):
    s = sum(SCORES.get(f, 0) for f in r["flags"])
    diverged = [p["diverged_at"] for k, p in r["state"].items() if k.startswith("mesen~") and p and p["diverged_at"]]
    if not any(f.startswith("state-diverges") for f in r["flags"]):
        diverged = []
    if "mesen-frozen" in r["flags"] and r["health"]["mesen"].get("frozen_from"):
        diverged.append(r["health"]["mesen"]["frozen_from"])
    if diverged:
        s += max(0, 20 - min(diverged) // 300)  # earlier divergence hides more
    return s


def compare_all(args):
    out = pathlib.Path(args.out)
    sessions = sorted(d for d in (out / "runs").iterdir() if d.is_dir()) if (out / "runs").exists() else []
    total = 0
    for session in sessions:
        rdir = out / "results" / session.name
        rdir.mkdir(parents=True, exist_ok=True)
        todo = [d for d in sorted(session.iterdir()) if d.is_dir() and (d / "entry.json").exists()
                and (args.force or not (rdir / (d.name + ".json")).exists())]
        with concurrent.futures.ProcessPoolExecutor(args.jobs) as pool:
            def previous(d):
                f = rdir / (d.name + ".json")
                return json.loads(f.read_text()) if f.exists() else None
            futures = {pool.submit(compare_session, d, args.keep_audio, previous(d)): d for d in todo}
            for fut in concurrent.futures.as_completed(futures):
                d = futures[fut]
                try:
                    r = fut.result()
                except Exception as ex:
                    print("compare failed for %s: %s" % (d, ex))
                    continue
                if r:
                    (rdir / (d.name + ".json")).write_text(json.dumps(r, indent=1))
                    total += 1
        print("%s: %d sessions compared" % (session.name, len(todo)))
    return 0
