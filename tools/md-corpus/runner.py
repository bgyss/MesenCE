"""`md_corpus.py run`: run sessions in MesenCE and the oracles.

One session = (corpus entry, session kind, frame count). Its directory is
runs/<kind>-<frames>/<entry id>/ with one sub-directory per emulator (mesen, gpgx, picodrive),
each holding the emulator's outputs and a run.json written last (the done marker: an emulator
whose run.json exists is not run again).

Runs are keyed by the mesen-headless build (SHA-256 of the binary, in MesenCE's run.json). A
session last run with another build is run again, all emulators (its audio went when it was
compared); its previous result moves to results/<session>/history/<id>.<build>.json, which
`md_corpus.py diff` compares with the new one.

Same conditions everywhere (strategy section 2.2): the region comes from the corpus entry, not
auto-detection; 3-button pads; TMSS off; no sprite-limit removal, overclocking, filters or
overscan in the oracles; the same Sega CD BIOS file in every emulator. Differences that remain,
by design: PicoDrive boots 32X games without Sega's boot ROMs (it has no option for them), and
MesenCE uses them (MESEN_32X_BIOS_DIR).
"""
import concurrent.futures
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
MESEN = ROOT / "bin/mesen-headless"
CAPTURE = ROOT / "tools/libretro-capture/capture.py"

CORE_DIRS = [pathlib.Path.home() / "Library/Application Support/RetroArch/cores", pathlib.Path.home() / ".config/retroarch/cores"]
ORACLE_SYSTEMS = {"gpgx": {"md", "mcd"}, "picodrive": {"md", "32x", "mcd", "mcd32x"}}
CORE_NAMES = {"gpgx": "genesis_plus_gx_libretro", "picodrive": "picodrive_libretro"}
CORE_ENV = {"gpgx": "GPGX_CORE", "picodrive": "PICODRIVE_CORE"}

BIOS_DIR = pathlib.Path(os.environ.get("MESEN_MCD_BIOS_DIR", pathlib.Path.home() / "mame/roms/segacd")).expanduser()
MCD_BIOS = {  # region -> file in BIOS_DIR (Model 1; the S5 qualification used US v1.10)
    "ntsc": "SegaCD Model 1 BIOS V1.10 (U) (Oct 1992).bin",
    "pal": "MegaCD Model 1 BIOS V1.00 (E) (Oct 1992) [!].bin",
    "ntsc-j": "MegaCD Model 1 BIOS V1.00 (J) (Dec 1991) [!].bin",
}
M32X_BIOS_DIR = pathlib.Path(os.environ.get("MESEN_32X_BIOS_DIR", BIOS_DIR)).expanduser()

GPGX_REGION = {"ntsc": "ntsc-u", "pal": "pal", "ntsc-j": "ntsc-j", "auto": "auto"}
PICO_REGION = {"ntsc": "US", "pal": "Europe", "ntsc-j": "Japan NTSC", "auto": "Auto"}


def core_path(name):
    if os.environ.get(CORE_ENV[name]):
        return pathlib.Path(os.environ[CORE_ENV[name]])
    for d in CORE_DIRS:
        for ext in (".dylib", ".so"):
            p = d / (CORE_NAMES[name] + ext)
            if p.exists():
                return p
    return None


def load_corpus(out):
    path = pathlib.Path(out) / "corpus.json"
    if not path.exists():
        sys.exit("no corpus: run `md_corpus.py inventory` first")
    return json.loads(path.read_text())["entries"]


def select(entries, args):
    systems = set(args.system.split(","))
    chosen = [e for e in entries if e["system"] in systems and (args.all or e.get("primary"))]
    if args.match:
        chosen = [e for e in chosen if args.match.lower() in e["title"].lower()]
    chosen.sort(key=lambda e: (e["system"], e["title"].lower()))
    return chosen[:args.limit] if args.limit else chosen


def rom_file(entry, cache):
    """A plain file for the emulators: the cue for discs, else the ROM extracted by hash."""
    if entry["system"] in ("mcd", "mcd32x"):
        return pathlib.Path(entry["path"])
    path = pathlib.Path(entry["path"])
    if entry["member"] is None:
        return path
    ext = pathlib.Path(entry["member"]).suffix.lower()
    target = cache / (entry["sha1"] + ext)
    if not target.exists():
        cache.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix(".part")
        if path.suffix.lower() == ".zip":
            with zipfile.ZipFile(path) as z:
                tmp.write_bytes(z.read(entry["member"]))
        else:
            tmp.write_bytes(subprocess.run(["7z", "e", "-so", str(path), entry["member"]], capture_output=True, check=True).stdout)
        tmp.rename(target)
    return target


def system_dir(out):
    """BIOS folder for the libretro cores, with the names GPGX and PicoDrive look for."""
    d = pathlib.Path(out) / "cache" / "system"
    d.mkdir(parents=True, exist_ok=True)
    for region, name in MCD_BIOS.items():
        src = BIOS_DIR / name
        dst = d / {"ntsc": "bios_CD_U.bin", "pal": "bios_CD_E.bin", "ntsc-j": "bios_CD_J.bin"}[region]
        if src.exists() and not dst.exists():
            shutil.copyfile(src, dst)
    return d


# The last 64 bytes of a freshly formatted Sega CD backup RAM (the BIOS's own format, the same
# image GPGX creates): directory header, free-block counts and the SEGA_CD_ROM / RAM_CARTRIDGE
# signature. MesenCE starts Sega CD sessions from it, as the oracles do; with blank backup RAM,
# games stop on "BACKUP RAM NOT INITIALIZED" (Road Rash).
BRAM_FORMAT = (b"___________" + bytes(4) + b"\x40\x00\x7d\x00\x7d\x00\x7d\x00\x7d\x00" + bytes(7)
               + b"SEGA_CD_ROM\x00\x01" + bytes(3) + b"RAM_CARTRIDGE___")


def formatted_bram(out):
    path = pathlib.Path(out) / "cache" / "system" / "formatted.brm"
    if not path.exists():
        assert len(BRAM_FORMAT) == 0x40
        path.write_bytes(bytes(0x2000 - 0x40) + BRAM_FORMAT)
    return path


def session_input(kind, frames, session_dir):
    if kind == "attract":
        return None
    import importlib.util  # tools/audio-compare/make_input.py, by path (its folder also has a compare.py)
    spec = importlib.util.spec_from_file_location("make_input", ROOT / "tools/audio-compare/make_input.py")
    make_input = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(make_input)
    path = session_dir / "input.json"
    if not path.exists():
        saved = sys.argv
        sys.argv = ["make_input.py", str(path), "--frames", str(frames), "--play-from", str(min(1800, frames // 2))]
        try:
            make_input.main()
        finally:
            sys.argv = saved
    return path


def mesen_build():
    """The build key: the first 16 hex digits of mesen-headless' SHA-256."""
    return hashlib.sha256(MESEN.read_bytes()).hexdigest()[:16]


def invalidate_if_stale(sdir, rdir, build, force=False):
    """A session run with another mesen-headless build: keep its result in history, remove its runs.
    Also removes leftovers of earlier manual re-runs (*.stale-*, *.silent-*)."""
    done = sdir / "mesen" / "run.json"
    for leftover in sdir.glob("*.*-*"):
        if leftover.is_dir():
            shutil.rmtree(leftover)
    if not done.exists():
        return
    old = json.loads(done.read_text()).get("build", "unknown")
    if old == build and not force:
        return
    result = rdir / (sdir.name + ".json")
    if result.exists():
        (rdir / "history").mkdir(exist_ok=True)
        result.rename(rdir / "history" / ("%s.%s.json" % (sdir.name, old)))
    for emu_dir in sdir.iterdir():
        if emu_dir.is_dir():
            shutil.rmtree(emu_dir)


def run_one(job):
    emu, entry, rom, out_dir, frames, input_path, snap_every, sysdir, build = job
    done = out_dir / "run.json"
    if done.exists():
        return emu, entry["id"], json.loads(done.read_text())
    if out_dir.exists():
        # An unfinished run (interrupted): move it aside, the runner needs a new directory
        out_dir.rename(out_dir.with_name(out_dir.name + ".stale-%d" % int(time.time())))
    region = entry["region"]
    pads = entry.get("pads", 3)
    pads2 = entry.get("pads2", pads)  # port 2 (a .gmv records each port's type)
    timeout = max(120, frames / 60 * 4)
    if emu == "mesen":
        # mesen-headless creates the directory itself and refuses an existing non-empty one
        cmd = [str(MESEN), "run", "--rom", str(rom), "--frames", str(frames), "--out", str(out_dir), "--allow-dirty",
               "--state-log", "state.log", "--state-blocks", "--snap-every", str(snap_every), "--chip-audio",
               "--md-pads", "%d,%d" % (pads, pads2), "--region", region, "--timeout", str(int(timeout))]
        if entry.get("input_latch"):
            cmd += ["--md-input-latch", entry["input_latch"]]
        if entry["system"] in ("mcd", "mcd32x"):
            bios = BIOS_DIR / MCD_BIOS.get(region, MCD_BIOS["ntsc"])
            cmd += ["--md-mcd-bios", str(bios), "--battery", str(formatted_bram(sysdir.parent.parent))]
        if entry["system"] == "32x" and (M32X_BIOS_DIR / "32X_M_BIOS.bin").exists():
            cmd += ["--md-32x-bios", str(M32X_BIOS_DIR)]
    else:
        cmd = [sys.executable, str(CAPTURE), str(core_path(emu)), str(rom), str(out_dir), "--frames", str(frames),
               "--state-log", "state.log", "--state-blocks", "--swap-words", "--snap-every", str(snap_every),
               "--system", str(sysdir)]
        if emu == "gpgx":
            # Only options GPGX applies without reinitialising: in this host, setting region_detect,
            # bios, force_dtack or addr_error at load leaves GPGX silent, and overscan hangs it. Its
            # defaults already match (BIOS off, overscan off); the region is auto-detected from the
            # header (a mismatch with MesenCE's forced region shows as an early state divergence).
            cmd += ["--option", "genesis_plus_gx_no_sprite_limit=disabled", "--option", "genesis_plus_gx_audio_filter=disabled"]
            if 6 in (pads, pads2):
                cmd += ["--pads", "%d,%d" % (pads, pads2)]
        else:
            cmd += ["--option", "picodrive_region=" + PICO_REGION[region], "--option", "picodrive_input1=%d button pad" % pads,
                    "--option", "picodrive_input2=%d button pad" % pads2, "--pads", "%d,%d" % (pads, pads2), "--option", "picodrive_sprlim=disabled",
                    "--option", "picodrive_audio_filter=disabled", "--option", "picodrive_overclk68k=disabled",
                    "--option", "picodrive_ramcart=disabled"]
    if input_path:
        cmd += ["--input", str(input_path)]
    start = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 60)
        code, err = p.returncode, p.stderr[-2000:]
    except subprocess.TimeoutExpired:
        code, err = -1, "killed after %d s" % (timeout + 60)
    out_dir.mkdir(parents=True, exist_ok=True)
    result = {"emulator": emu, "exit": code, "wall_seconds": round(time.time() - start, 2), "command": cmd, "stderr": err,
              "frames": frames, "rom": str(rom)}
    if emu == "mesen":
        result["build"] = build
    done.write_text(json.dumps(result, indent=1))
    return emu, entry["id"], result


def run_corpus(args):
    out = pathlib.Path(args.out)
    if not MESEN.exists():
        sys.exit("bin/mesen-headless missing: mise run headless")
    oracles = [o for o in args.oracles.split(",") if o]
    for o in oracles:
        if not core_path(o):
            sys.exit("no %s core (set %s)" % (o, CORE_ENV[o]))
    if args.session == "tas":
        return run_tas(args, out, oracles)
    entries = select(load_corpus(out), args)
    session = "%s-%d" % (args.session, args.frames)
    sysdir = system_dir(out)
    build = mesen_build()
    jobs = []
    for e in entries:
        (out / "runs" / session / e["id"]).mkdir(parents=True, exist_ok=True)
        input_path = session_input(args.session, args.frames, out / "runs" / session / e["id"])
        jobs += session_jobs(e, out / "runs" / session / e["id"], out, oracles, args.frames, input_path, args.snap_every, sysdir, build)
    return run_jobs(args, out, session, jobs, entries, build)


def run_tas(args, out, oracles):
    """Every imported movie with status ok (tas.py), as session tas: its own frame count, region and
    pads; the session directory is named after the movie. GPGX gets the movie's input shifted by
    the offset calibrate_tas picked (against PicoDrive, never MesenCE)."""
    import tas
    corpus = {e["id"]: e for e in load_corpus(out)}
    sysdir, build = system_dir(out), mesen_build()
    movies = []
    for m in tas.load_movies(out):
        if m["status"] != "ok" or (args.match and args.match.lower() not in (m["title"] + " " + m["id"] + " " + pathlib.Path(m["file"]).name).lower()):
            continue
        e = dict(corpus[m["entry"]], id=m["id"], rom_id=m["entry"], region=m["region"], pads=m["pads"], pads2=m.get("pads2", m["pads"]),
                 source={"kind": "tas", "file": m["file"], "movie_sha1": m["movie_sha1"]})
        movies.append((m, e))
    shifts = calibrate_tas(args, out, oracles, movies, sysdir)
    jobs, entries = [], []
    for m, e in movies:
        sdir = out / "runs" / "tas" / m["id"]
        inputs = {"*": out / "tas" / m["id"] / "input.json"}
        shift = shifts.get(m["id"], 0)
        e["gpgx_input_shift"] = shift
        # MesenCE takes each frame's input when the movie's emulator did: BizHawk's Genplus-gx
        # updates the pads at the first VBlank line, Gens (like MesenCE's default) at the frame
        # boundary. It matters when a game reads the pads after the wrap to line 0 in a long VBlank
        # handler (The Lion King's title screen, findings C4)
        e["input_latch"] = "vblank" if recorded_in_gpgx(m) else "frame"
        if shift:
            sdir.mkdir(parents=True, exist_ok=True)
            inputs["gpgx"] = sdir / ("gpgx-input%+d.json" % shift)
            if not inputs["gpgx"].exists():
                inputs["gpgx"].write_text(json.dumps(shift_input(json.loads(inputs["*"].read_text()), shift)))
        drop_if_input_changed(sdir, out / "results" / "tas", inputs)
        entries.append(e)
        jobs += session_jobs(e, sdir, out, oracles, m["frames"], inputs, args.snap_every, sysdir, build)
    return run_jobs(args, out, "tas", jobs, entries, build)


CALIBRATE_STAGES = (5400, 36000)          # 90 s; then 10 min for the shifts still tied
CALIBRATE_SHIFTS = (0, 1, -1, 2, -2, 3, -3)  # in order of preference on a tie
CALIBRATE_VERSION = 3                        # the choice rule; cached choices from another rule are redone


def recorded_in_gpgx(m):
    """A movie made in BizHawk's Genplus-gx core (the Genesis Plus GX code the gpgx oracle runs)."""
    return m["format"] == "bk2" and (m.get("core") or "Genplus-gx").lower().replace("-", "").replace(" ", "") in ("genplusgx", "gpgx")


def shift_input(data, shift):
    """An --input script with every change moved `shift` frames later (earlier if negative);
    changes pushed before frame 1 collapse onto it, the latest winning."""
    frames = {}
    for f in sorted(data["frames"], key=int):
        frames.setdefault(str(max(1, int(f) + shift)), {}).update(data["frames"][f])
    return {"frames": frames}


def calibrate_tas(args, out, oracles, movies, sysdir):
    """Per movie, the frame offset to apply to GPGX's input. Libretro GPGX acts on some games' pad
    reads a couple of frames later than PicoDrive and MesenCE (Sonic 3: 2 frames; Sonic 1: none;
    findings C4), so a movie made in one emulator can desync in another from its first press.
    MesenCE never takes part, so its comparison stays independent.

    Stage 1 runs the first 5,400 frames in PicoDrive (as recorded) and in GPGX with every shift;
    a shift's score is how long its work RAM follows PicoDrive's (compare.state_pair diverged_at).
    The shifts tied for the latest divergence (within 6 frames) go on to stage 2 (36,000 frames, or the movie). A
    tie that remains goes to the first in CALIBRATE_SHIFTS' order (0, +1, -1, ...): lockstep
    counts picked noise (+3/-3 on Sonic 1/2 and Shinobi III, findings C4).

    Movies recorded in BizHawk's Genplus-gx core are not calibrated: GPGX is that core, so it
    replays them with no shift. Cached in tas/<id>/calibration.json."""
    if not {"gpgx", "picodrive"} <= set(oracles):
        return {}
    import compare
    shifts, todo = {}, []
    for m, e in movies:
        cache = out / "tas" / m["id"] / "calibration.json"
        if e["system"] != "md" or recorded_in_gpgx(m):
            continue
        if cache.exists():
            c = json.loads(cache.read_text())
            if c.get("movie_sha1") == m["movie_sha1"] and c.get("stages") == list(CALIBRATE_STAGES) and c.get("version") == CALIBRATE_VERSION:
                shifts[m["id"]] = c["shift"]
                continue
        todo.append((m, e))
    if not todo:
        return shifts
    print("calibrating GPGX input offsets for %d movies (stages %s, shifts %s)" % (len(todo), list(CALIBRATE_STAGES), list(CALIBRATE_SHIFTS)), flush=True)
    cands = {m["id"]: list(CALIBRATE_SHIFTS) for m, e in todo}
    tables = {m["id"]: [] for m, e in todo}
    for stage, window in enumerate(CALIBRATE_STAGES):
        active = [(m, e) for m, e in todo if len(cands[m["id"]]) > 1 and (stage == 0 or m["frames"] > CALIBRATE_STAGES[stage - 1])]
        if not active:
            break
        runs = []
        for m, e in active:
            cdir = out / "tas" / m["id"] / ("calibrate-%d" % window)
            if cdir.exists():
                shutil.rmtree(cdir)
            cdir.mkdir(parents=True)
            data = json.loads((out / "tas" / m["id"] / "input.json").read_text())
            rom = rom_file(e, out / "cache" / "roms")
            frames = min(window, m["frames"])
            runs.append(("picodrive", e, rom, cdir / "picodrive", frames, out / "tas" / m["id"] / "input.json", frames, sysdir, None))
            for s in cands[m["id"]]:
                path = cdir / ("input%+d.json" % s)
                path.write_text(json.dumps(shift_input(data, s)))
                runs.append(("gpgx", e, rom, cdir / ("gpgx%+d" % s), frames, path, frames, sysdir, None))
        print("  stage %d: %d frames, %d movies, %d runs" % (stage + 1, window, len(active), len(runs)), flush=True)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            list(pool.map(run_one, runs))
        for m, e in active:
            cdir = out / "tas" / m["id"] / ("calibrate-%d" % window)
            ref = compare.read_blocks(cdir / "picodrive")
            frames = min(window, m["frames"])
            table = {}
            for s in cands[m["id"]]:
                r = compare.state_pair(ref, compare.read_blocks(cdir / ("gpgx%+d" % s)))
                table[s] = None if r is None else {k: r[k] for k in ("diverged_at", "lockstep", "agreement_median")}
            tables[m["id"]].append({"frames": frames, "shifts": {"%+d" % s: v for s, v in table.items()}})
            scored = {s: (v["diverged_at"] or frames + 1) for s, v in table.items() if v}
            top = max(scored.values()) if scored else None
            # Within 2 x the largest shift counts as a tie: when the movie desyncs at the same game
            # moment under every shift, the divergence frame just moves with the shift (Gens
            # Sonic 1: -3 at 35,247 ... +3 at 35,253)
            slack = 2 * max(CALIBRATE_SHIFTS)
            cands[m["id"]] = [s for s in cands[m["id"]] if s in scored and scored[s] >= top - slack] or [0]
            shutil.rmtree(cdir)
    for m, e in todo:
        tied = cands[m["id"]]
        best = tied[0]
        shifts[m["id"]] = best
        (out / "tas" / m["id"] / "calibration.json").write_text(json.dumps(
            {"movie_sha1": m["movie_sha1"], "stages": list(CALIBRATE_STAGES), "version": CALIBRATE_VERSION, "shift": best, "against": "picodrive",
             # Shifts still tied after the last stage (the first in CALIBRATE_SHIFTS' order won)
             "tied": ["%+d" % s for s in tied], "edge": abs(best) == max(CALIBRATE_SHIFTS), "results": tables[m["id"]]}, indent=1))
        last = tables[m["id"]][-1]
        print("  %-12s %-44s GPGX input %+d%s  [%d frames] %s" % (m["id"], m["title"][:44], best, " (edge of range)" if abs(best) == max(CALIBRATE_SHIFTS) else "",
              last["frames"], "  ".join("%s:%s" % (k, "-" if v is None else (v["diverged_at"] or "ok")) for k, v in last["shifts"].items())), flush=True)
    return shifts


def drop_if_input_changed(sdir, rdir, inputs):
    """A session whose runs used other input files (a new calibration): run it again, all emulators
    (comparison deleted its audio)."""
    for emu_dir in (sdir.iterdir() if sdir.exists() else []):
        done = emu_dir / "run.json"
        if emu_dir.name not in ("mesen", "gpgx", "picodrive") or not done.exists():
            continue
        cmd = json.loads(done.read_text())["command"]
        used = cmd[cmd.index("--input") + 1] if "--input" in cmd else None
        if used != str(inputs.get(emu_dir.name, inputs["*"])):
            for d in sdir.iterdir():
                if d.is_dir():
                    shutil.rmtree(d)
            result = rdir / (sdir.name + ".json")
            if result.exists():
                result.unlink()
            return


def session_jobs(e, sdir, out, oracles, frames, input_path, snap_every, sysdir, build):
    """MesenCE and every oracle that runs the entry's system, in session directory sdir."""
    sdir.mkdir(parents=True, exist_ok=True)
    (sdir / "entry.json").write_text(json.dumps(e, indent=1))
    try:
        rom = rom_file(e, out / "cache" / "roms")
    except Exception as ex:  # unreadable archive
        print("skip %s: %s" % (e["title"], ex))
        return []
    def inp(emu):  # input_path: one file, or {emulator or "*": file}
        return input_path.get(emu, input_path["*"]) if isinstance(input_path, dict) else input_path
    import compare

    def snaps(emu):  # oracles of offset-tracked systems: dense snapshots (compare.visual)
        if emu != "mesen" and snap_every and e["system"] in compare.OFFSET_SNAP_SYSTEMS:
            return min(snap_every, compare.OFFSET_SNAP_EVERY)
        return snap_every
    return [(emu, e, rom, sdir / emu, frames, inp(emu), snaps(emu), sysdir, build)
            for emu in ["mesen"] + [o for o in oracles if e["system"] in ORACLE_SYSTEMS[o]]]


def run_jobs(args, out, session, jobs, entries, build):
    print("%d titles, %d runs (%s) with %d jobs, mesen-headless build %s" % (len(entries), len(jobs), session, args.jobs, build), flush=True)
    # One worker per session: its emulators run one after the other, then the session is compared
    # at once (unless --no-compare), so its audio files can go before the next session's are written
    by_session = {}
    for job in jobs:
        by_session.setdefault(job[3].parent, []).append(job)
    import compare
    rdir = out / "results" / session
    rdir.mkdir(parents=True, exist_ok=True)

    def work(session_jobs):
        sdir = session_jobs[0][3].parent
        invalidate_if_stale(sdir, rdir, build, args.rerun)
        complete = all((j[3] / "run.json").exists() for j in session_jobs)
        if complete and (rdir / (sdir.name + ".json")).exists():
            # Already run and compared with this build (comparing again would find the audio gone)
            return [run_one(j) for j in session_jobs]
        results = [run_one(j) for j in session_jobs]
        if not args.no_compare:
            r = compare.compare_session(sdir, keep_audio=False)
            if r:
                (rdir / (sdir.name + ".json")).write_text(json.dumps(r, indent=1))
        return results

    failed = done = 0
    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for i, results in enumerate(pool.map(work, by_session.values()), 1):
            failed += sum(1 for _, _, r in results if r["exit"] != 0)
            done += len(results)
            if i % 25 == 0 or i == len(by_session):
                print("  %d/%d sessions, %d runs, %d nonzero exits, %.0f s" % (i, len(by_session), done, failed, time.time() - start), flush=True)
    return 0
