"""`md_corpus.py run`: run sessions in MesenCE and the oracles.

One session = (corpus entry, session kind, frame count). Its directory is
runs/<kind>-<frames>/<entry id>/ with one sub-directory per emulator (mesen, gpgx, picodrive),
each holding the emulator's outputs and a run.json written last (the done marker: an emulator
whose run.json exists is not run again).

Same conditions everywhere (strategy section 2.2): the region comes from the corpus entry, not
auto-detection; 3-button pads; TMSS off; no sprite-limit removal, overclocking, filters or
overscan in the oracles; the same Sega CD BIOS file in every emulator. Differences that remain,
by design: PicoDrive boots 32X games without Sega's boot ROMs (it has no option for them), and
MesenCE uses them (MESEN_32X_BIOS_DIR).
"""
import concurrent.futures
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


def run_one(job):
    emu, entry, rom, out_dir, frames, input_path, snap_every, sysdir = job
    done = out_dir / "run.json"
    if done.exists():
        return emu, entry["id"], json.loads(done.read_text())
    if out_dir.exists():
        # An unfinished run (interrupted): move it aside, the runner needs a new directory
        out_dir.rename(out_dir.with_name(out_dir.name + ".stale-%d" % int(time.time())))
    region = entry["region"]
    timeout = max(120, frames / 60 * 4)
    if emu == "mesen":
        # mesen-headless creates the directory itself and refuses an existing non-empty one
        cmd = [str(MESEN), "run", "--rom", str(rom), "--frames", str(frames), "--out", str(out_dir), "--allow-dirty",
               "--state-log", "state.log", "--state-blocks", "--snap-every", str(snap_every), "--chip-audio",
               "--md-pads", "3,3", "--region", region, "--timeout", str(int(timeout))]
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
        else:
            cmd += ["--option", "picodrive_region=" + PICO_REGION[region], "--option", "picodrive_input1=3 button pad",
                    "--option", "picodrive_input2=3 button pad", "--option", "picodrive_sprlim=disabled",
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
    entries = select(load_corpus(out), args)
    session = "%s-%d" % (args.session, args.frames)
    sysdir = system_dir(out)
    jobs = []
    for e in entries:
        sdir = out / "runs" / session / e["id"]
        sdir.mkdir(parents=True, exist_ok=True)
        (sdir / "entry.json").write_text(json.dumps(e, indent=1))
        try:
            rom = rom_file(e, out / "cache" / "roms")
        except Exception as ex:  # unreadable archive
            print("skip %s: %s" % (e["title"], ex))
            continue
        input_path = session_input(args.session, args.frames, sdir)
        for emu in ["mesen"] + [o for o in oracles if e["system"] in ORACLE_SYSTEMS[o]]:
            jobs.append((emu, e, rom, sdir / emu, args.frames, input_path, args.snap_every, sysdir))
    print("%d titles, %d runs (%s) with %d jobs" % (len(entries), len(jobs), session, args.jobs), flush=True)
    # One worker per session: its emulators run one after the other, then the session is compared
    # at once (unless --no-compare), so its audio files can go before the next session's are written
    by_session = {}
    for job in jobs:
        by_session.setdefault(job[3].parent, []).append(job)
    import compare
    rdir = out / "results" / session
    rdir.mkdir(parents=True, exist_ok=True)

    def work(session_jobs):
        results = [run_one(j) for j in session_jobs]
        sdir = session_jobs[0][3].parent
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
