"""TAS movies (strategy section 3.2, P3): convert BizHawk .bk2 and Gens .gmv movies to md-corpus
sessions.

Movies are not fetched: TASVideos' robots.txt disallows automated movie downloads
(`/*?handler=Download`) for every client and blocks AI agents (Claude-Code among them) from the
whole site. Download the movies yourself (publication pages, "Download") into bin/md-corpus/movies
(git-ignored), then:

    md_corpus.py tas import [FOLDER...]    # parse, match each movie's ROM hash to corpus.json
                                           # (default folder: bin/md-corpus/movies)
    md_corpus.py tas list                  # imported movies, their titles and status
    md_corpus.py run --session tas         # run every imported movie in MesenCE and the oracles

Each movie becomes bin/md-corpus/tas/<id>/ with tas.json (source, ROM match, region, pads, frames,
notes) and input.json (mesen-headless input format: frame -> port -> held buttons, change events
only).

Formats:
- .bk2 (BizHawk): a zip. Header.txt ("Key Value" lines: Platform, GameName, SHA1, Core,
  StartsFromSavestate, StartsFromSaveRam; for Genplus-gx movies the "SHA1" line holds the ROM's
  MD5, 32 hex digits), SyncSettings.json (UseSixButton; Region, a number: 0 auto-detect,
  1 Japan NTSC, 2 Japan PAL, 3 USA, 4 Europe), Input Log.txt
  ("LogKey:" naming the columns in '#'-separated groups, then one "|...|" line per frame, one
  character per button, '.' = released). Columns are mapped by name, not position. Line i is the
  input of frame i + 1.
- .gmv (Gens): a 64-byte header ("Gens Movie TEST" and a version letter, rerecords at $10,
  '3'/'6' pad types of P1 and P2 at $14/$15, flags at $16: bit 7 starts from a save state, bit 6
  PAL, bit 5 three players; a comment from $18), then 3 bytes per frame: P1 and P2 (bit 0-7 Up
  Down Left Right A B C Start), then X Y Z Mode for P1 (bits 0-3) and P2 (bits 4-7); a clear bit
  is a pressed button. A .gmv does not name its ROM: give it in movies/roms.txt (below) or with
  --sha1. (Checked against published movies: the header fields; frame sync not yet confirmed.)

Movies that do not carry a usable hash are matched through movies/roms.txt, one line per movie:

    aglar-sonic.gmv = Sonic The Hedgehog (USA, Europe)        # exact corpus title, or a ROM SHA-1

The ROM is the one the publication page names ("Game: ... ROM"); GoodGen names map to No-Intro
ones (e.g. "(W) (REV00)" -> "(USA, Europe)", "(W) (REV01)" -> "(Japan, Europe, Korea) (En)
(Rev A)").

Region: the movie's when it records one, else what the recording emulator's auto-detection would
pick from the cartridge header ($1F0): Genesis Plus GX (BizHawk) and Gens both prefer USA, then
Japan, then Europe.
"""
import hashlib
import io
import json
import pathlib
import re
import subprocess
import zipfile

BUTTONS = ("UP", "DOWN", "LEFT", "RIGHT", "START", "A", "B", "C", "X", "Y", "Z", "MODE")
BK2_NAMES = {b.title(): b for b in BUTTONS}  # "P1 Up" ... "P1 Mode"
MARGIN_FRAMES = 600  # run past the movie's last input, to see where it ends


def events_from_frames(frames):
    """[ {port: set(buttons)} per frame ] -> mesen-headless input {"frames": {n: {port: [...]}}}."""
    out, held = {}, {}
    for i, pads in enumerate(frames, 1):
        for port in sorted(set(held) | set(pads)):
            now = sorted(pads.get(port, ()), key=BUTTONS.index)
            if held.get(port, []) != now:
                out.setdefault(str(i), {})[str(port)] = now
                held[port] = now
    return {"frames": out}


def parse_bk2(data):
    z = zipfile.ZipFile(io.BytesIO(data))
    names = {n.lower(): n for n in z.namelist()}
    header = {}
    for line in z.read(names["header.txt"]).decode("utf-8", "replace").splitlines():
        if " " in line.strip():
            k, v = line.strip().split(" ", 1)
            header[k] = v.strip()
    sync = {}
    if "syncsettings.json" in names:
        try:
            sync = json.loads(z.read(names["syncsettings.json"]).decode("utf-8", "replace") or "{}")
        except ValueError:
            sync = {}
    log = z.read(names["input log.txt"]).decode("utf-8", "replace").splitlines()
    key = next((l[len("LogKey:"):] for l in log if l.startswith("LogKey:")), None)
    if key is None:
        raise ValueError("Input Log.txt has no LogKey")
    groups = [[n for n in g.split("|") if n] for g in key.split("#") if g.strip("|")]
    notes, frames, resets = [], [], []
    columns = []  # per group: [(port, button) or ("power"/"reset", None) or None]
    for g in groups:
        cols = []
        for name in g:
            m = re.match(r"P(\d+) (.+)$", name)
            if m and m.group(2) in BK2_NAMES:
                cols.append((int(m.group(1)), BK2_NAMES[m.group(2)]))
            elif name in ("Reset", "Power"):
                cols.append((name.lower(), None))
            else:
                cols.append(None)
                if name not in notes:
                    notes.append("column ignored: " + name)
        columns.append(cols)
    for line in log:
        if not line.startswith("|"):
            continue
        parts = line.strip().strip("|").split("|")
        pads = {}
        for cols, text in zip(columns, parts):
            for col, ch in zip(cols, text):
                if col is None or ch in ". ":
                    continue
                if col[1] is None:
                    if len(frames) > 0:
                        resets.append((col[0], len(frames) + 1))
                else:
                    pads.setdefault(col[0], set()).add(col[1])
        frames.append(pads)
    sync_text = json.dumps(sync)
    six = bool(re.search(r'"(UseSixButton|SixButton)"\s*:\s*true|Gamepad6B|6 ?button', sync_text, re.I))
    region = None
    m = re.search(r'"Region"\s*:\s*"?([A-Za-z_]+|\d+)', sync_text)
    if m:
        region = {"0": None, "1": "ntsc-j", "2": "pal", "3": "ntsc", "4": "pal", "autodetect": None,
                  "japan_ntsc": "ntsc-j", "japan_pal": "pal", "usa": "ntsc", "europe": "pal"}.get(m.group(1).lower())
        if m.group(1).lower() in ("2", "japan_pal"):
            notes.append("Japan PAL console: run as PAL")
    rom_hash = (header.get("SHA1") or header.get("MD5") or "").lower() or None
    return {
        "format": "bk2", "platform": header.get("Platform"), "game": header.get("GameName"),
        "sha1": rom_hash, "core": header.get("Core"),
        "from_savestate": header.get("StartsFromSavestate", "False").lower() == "true",
        "from_saveram": header.get("StartsFromSaveRam", "False").lower() == "true",
        "rerecords": header.get("rerecordCount"), "pads": 6 if six else 3, "pads2": 6 if six else 3, "region": region,
        "resets": resets, "notes": notes, "frames": frames,
    }


def parse_gmv(data):
    if not data.startswith(b"Gens Movie TEST"):
        raise ValueError("not a Gens movie")
    flags = data[0x16] if data[15:16] >= b"A" else 0  # flags from version A on
    pads, pads2 = (6 if data[i] == ord("6") else 3 for i in (0x14, 0x15))
    frames = []
    names = ("UP", "DOWN", "LEFT", "RIGHT", "A", "B", "C", "START")
    for off in range(0x40, len(data) - 2, 3):
        b0, b1, b2 = data[off:off + 3]
        pressed = {}
        for port, byte, extra, kind in ((1, b0, b2 & 0x0F, pads), (2, b1, b2 >> 4, pads2)):
            s = {n for bit, n in enumerate(names) if not byte & (1 << bit)}
            if kind == 6:
                s |= {n for bit, n in enumerate(("X", "Y", "Z", "MODE")) if not extra & (1 << bit)}
            if s:
                pressed[port] = s
        frames.append(pressed)
    return {
        "format": "gmv", "platform": "GEN", "game": None,
        "comment": data[0x18:0x40].split(b"\0")[0].decode("latin-1").strip(),
        "sha1": None, "core": "Gens", "from_savestate": bool(flags & 0x80), "from_saveram": False,
        "three_players": bool(flags & 0x20),
        "rerecords": int.from_bytes(data[0x10:0x14], "little"), "pads": pads, "pads2": pads2,
        # PAL flag clear: an NTSC console, USA or Japan from the header (import_movies)
        "region": "pal" if flags & 0x40 else "ntsc?", "resets": [], "notes": [], "frames": frames,
    }


def read_rom(entry):
    """A cartridge entry's ROM bytes, from its zip/7z member or plain file."""
    path = pathlib.Path(entry["path"])
    if entry["member"] is None:
        return path.read_bytes()
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as z:
            return z.read(entry["member"])
    return subprocess.run(["7z", "e", "-so", str(path), entry["member"]], capture_output=True, check=True).stdout


def md5_index(out, corpus):
    """MD5 -> corpus entry, for the cartridges (BizHawk's Genplus-gx movies name their ROM by MD5).
    Cached in cache/md5.json by SHA-1; the first use reads every cartridge once."""
    cache_file = out / "cache" / "md5.json"
    cache = json.loads(cache_file.read_text()) if cache_file.exists() else {}
    index = {}
    for e in corpus:
        if e["system"] not in ("md", "32x") or not e["sha1"]:
            continue
        if e["sha1"] not in cache:
            try:
                cache[e["sha1"]] = hashlib.md5(read_rom(e)).hexdigest()
            except Exception as ex:  # unreadable archive member
                print("md5: skip %s: %s" % (e["title"], ex))
                continue
        index.setdefault(cache[e["sha1"]], e)
    cache_file.parent.mkdir(parents=True, exist_ok=True)
    cache_file.write_text(json.dumps(cache))
    return index


def rom_map(folders):
    """movies/roms.txt in each folder: "movie file name = corpus title or ROM SHA-1" lines."""
    out = {}
    for d in folders:
        f = pathlib.Path(d) / "roms.txt"
        if not f.is_file():
            continue
        for line in f.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                name, rom = (x.strip() for x in line.split("=", 1))
                out[name] = rom
    return out


def auto_region(entry, ntsc_only=False):
    """Genesis Plus GX's and Gens' header auto-detection: USA, then Japan, then Europe."""
    import inventory
    try:
        allowed = inventory.header_regions(read_rom(entry)[0x1F0:0x200].decode("ascii", "replace"))
    except Exception:
        allowed = set()
    for region in ("ntsc", "ntsc-j") + (() if ntsc_only else ("pal",)):
        if region in allowed:
            return region
    return None


def import_movies(paths, out, sha1=None):
    corpus = json.loads((out / "corpus.json").read_text())["entries"]
    by_sha1, by_title = {}, {}
    for e in corpus:
        by_sha1.setdefault(e["sha1"], e)
        by_title.setdefault(e["title"].lower(), e)
    files, folders = [], []
    for p in paths:
        p = pathlib.Path(p).expanduser()
        folders.append(p if p.is_dir() else p.parent)
        files += sorted(f for f in (p.rglob("*") if p.is_dir() else [p]) if f.suffix.lower() in (".bk2", ".gmv"))
    if not files:
        print("no .bk2/.gmv movies in %s" % ", ".join(str(p) for p in paths))
        return 0
    mapping = rom_map(folders)
    by_md5 = None
    rows = []
    for f in files:
        data = f.read_bytes()
        mid = hashlib.sha1(data).hexdigest()[:12]
        try:
            movie = parse_bk2(data) if f.suffix.lower() == ".bk2" else parse_gmv(data)
        except (ValueError, KeyError, zipfile.BadZipFile) as ex:
            rows.append((f.name, "unreadable: %s" % ex))
            continue
        rom = sha1 or mapping.get(f.name) or movie["sha1"]
        entry = None
        if rom:
            if re.fullmatch(r"[0-9a-fA-F]{32}", rom):
                if by_md5 is None:
                    by_md5 = md5_index(out, corpus)
                entry = by_md5.get(rom.lower())
            elif re.fullmatch(r"[0-9a-fA-F]{40}", rom):
                entry = by_sha1.get(rom.lower())
            else:
                entry = by_title.get(rom.lower())
        status = "ok"
        if movie["from_savestate"]:
            status = "unsupported: starts from a save state"
        elif movie["from_saveram"]:
            status = "unsupported: starts from save RAM"
        elif movie.get("three_players"):
            status = "unsupported: three players"
        elif movie["resets"]:
            status = "unsupported: resets or power-cycles during the movie (frames %s)" % [f for _, f in movie["resets"]][:5]
        elif not entry:
            status = ("no ROM: %s not in the corpus" % rom if rom else
                      "no ROM: the movie does not name it; add \"%s = <ROM title>\" to roms.txt" % f.name)
        region = movie["region"]
        if entry and region in (None, "ntsc?"):
            region = auto_region(entry, ntsc_only=region == "ntsc?") or ("ntsc" if region == "ntsc?" else entry["region"])
        d = out / "tas" / mid
        d.mkdir(parents=True, exist_ok=True)
        meta = {k: v for k, v in movie.items() if k != "frames"}
        meta.update({"id": mid, "file": str(f), "movie_sha1": hashlib.sha1(data).hexdigest(), "rom": rom,
                     "rom_sha1": entry["sha1"] if entry else None, "region": region,
                     "input_frames": len(movie["frames"]), "frames": len(movie["frames"]) + MARGIN_FRAMES,
                     "status": status, "entry": entry["id"] if entry else None,
                     "title": entry["title"] if entry else (movie["game"] or f.stem)})
        (d / "input.json").write_text(json.dumps(events_from_frames(movie["frames"])))
        (d / "tas.json").write_text(json.dumps(meta, indent=1))
        pads = "%d-button" % movie["pads"] if movie["pads"] == movie["pads2"] else "%d/%d-button" % (movie["pads"], movie["pads2"])
        rows.append((f.name, "%s -> %s (%s, %s, %d frames, %s)" % (status, meta["title"], mid, region, len(movie["frames"]), pads)))
    for name, text in rows:
        print("%-50s %s" % (name[:50], text))
    return 0


def load_movies(out):
    movies = []
    for f in sorted((out / "tas").glob("*/tas.json")):
        movies.append(json.loads(f.read_text()))
    return movies


def list_movies(out):
    for m in load_movies(out):
        print("%s  %-8s %7d  %s  [%s]" % (m["id"], m["format"], m["input_frames"], m["title"], m["status"]))
    return 0
