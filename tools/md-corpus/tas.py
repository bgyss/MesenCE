"""TAS movies (strategy section 3.2, P3): convert BizHawk .bk2 and Gens .gmv movies to md-corpus
sessions.

Movies are not fetched: TASVideos' robots.txt disallows automated movie downloads
(`/*?handler=Download`) for every client and blocks AI agents (Claude-Code among them) from the
whole site. Download the movies yourself (publication pages, "Download") into a folder, then:

    md_corpus.py tas import FOLDER...      # parse, match each movie's ROM hash to corpus.json
    md_corpus.py tas list                  # imported movies, their titles and status
    md_corpus.py run --session tas         # run every imported movie in MesenCE and the oracles

Each movie becomes bin/md-corpus/tas/<id>/ with tas.json (source, ROM match, region, pads, frames,
notes) and input.json (mesen-headless input format: frame -> port -> held buttons, change events
only).

Formats:
- .bk2 (BizHawk): a zip. Header.txt ("Key Value" lines: Platform, GameName, SHA1, Core,
  StartsFromSavestate, StartsFromSaveRam), SyncSettings.json (region, pad types), Input Log.txt
  ("LogKey:" naming the columns in '#'-separated groups, then one "|...|" line per frame, one
  character per button, '.' = released). Columns are mapped by name, not position. Line i is the
  input of frame i + 1.
- .gmv (Gens): a 64-byte header ("Gens Movie TEST", rerecords at $10, '3'/'6' pad types at
  $14/$15, flags at $16: bit 6 PAL, bit 7 starts from a save state), then 3 bytes per frame:
  P1 and P2 (bit 0-7 Up Down Left Right A B C Start), then X Y Z Mode for P1 (bits 0-3) and P2
  (bits 4-7); a clear bit is a pressed button. A .gmv has no ROM hash: match it with --sha1.
  (Layout per the Gens movie format notes on TASVideos; not yet checked against a published
  movie.)
"""
import hashlib
import io
import json
import pathlib
import re
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
    m = re.search(r'"Region"\s*:\s*"?([A-Za-z]+)', sync_text)
    if m:
        region = {"usa": "ntsc", "japan": "ntsc-j", "europe": "pal", "japanpal": "pal"}.get(m.group(1).lower())
    return {
        "format": "bk2", "platform": header.get("Platform"), "game": header.get("GameName"),
        "sha1": (header.get("SHA1") or "").lower() or None, "core": header.get("Core"),
        "from_savestate": header.get("StartsFromSavestate", "False").lower() == "true",
        "from_saveram": header.get("StartsFromSaveRam", "False").lower() == "true",
        "rerecords": header.get("rerecordCount"), "pads": 6 if six else 3, "region": region,
        "resets": resets, "notes": notes, "frames": frames,
    }


def parse_gmv(data):
    if not data.startswith(b"Gens Movie TEST"):
        raise ValueError("not a Gens movie")
    flags = data[0x16]
    pads = 6 if b"6" in data[0x14:0x16] else 3
    frames = []
    names = ("UP", "DOWN", "LEFT", "RIGHT", "A", "B", "C", "START")
    for off in range(0x40, len(data) - 2, 3):
        b0, b1, b2 = data[off:off + 3]
        pressed = {}
        for port, byte, extra in ((1, b0, b2 & 0x0F), (2, b1, b2 >> 4)):
            s = {n for bit, n in enumerate(names) if not byte & (1 << bit)}
            if pads == 6:
                s |= {n for bit, n in enumerate(("X", "Y", "Z", "MODE")) if not extra & (1 << bit)}
            if s:
                pressed[port] = s
        frames.append(pressed)
    return {
        "format": "gmv", "platform": "GEN", "game": data[0x18:0x40].split(b"\0")[0].decode("latin-1").strip(),
        "sha1": None, "core": "Gens", "from_savestate": bool(flags & 0x80), "from_saveram": False,
        "rerecords": int.from_bytes(data[0x10:0x14], "little"), "pads": pads,
        "region": "pal" if flags & 0x40 else None, "resets": [], "notes": [], "frames": frames,
    }


def import_movies(paths, out, sha1=None):
    corpus = json.loads((out / "corpus.json").read_text())["entries"]
    by_sha1 = {}
    for e in corpus:
        by_sha1.setdefault(e["sha1"], e)
    files = []
    for p in paths:
        p = pathlib.Path(p).expanduser()
        files += sorted(f for f in (p.rglob("*") if p.is_dir() else [p]) if f.suffix.lower() in (".bk2", ".gmv"))
    rows = []
    for f in files:
        data = f.read_bytes()
        mid = hashlib.sha1(data).hexdigest()[:12]
        try:
            movie = parse_bk2(data) if f.suffix.lower() == ".bk2" else parse_gmv(data)
        except (ValueError, KeyError, zipfile.BadZipFile) as ex:
            rows.append((f.name, "unreadable: %s" % ex))
            continue
        rom_sha1 = sha1 or movie["sha1"]
        entry = by_sha1.get(rom_sha1) if rom_sha1 else None
        status = "ok"
        if movie["from_savestate"]:
            status = "unsupported: starts from a save state"
        elif movie["from_saveram"]:
            status = "unsupported: starts from save RAM"
        elif any(kind == "reset" for kind, _ in movie["resets"]) or any(kind == "power" for kind, _ in movie["resets"]):
            status = "unsupported: resets or power-cycles during the movie (frames %s)" % [f for _, f in movie["resets"]][:5]
        elif not entry:
            status = "no ROM: %s not in the corpus" % (rom_sha1 or "no hash (use --sha1)")
        d = out / "tas" / mid
        d.mkdir(parents=True, exist_ok=True)
        meta = {k: v for k, v in movie.items() if k != "frames"}
        meta.update({"id": mid, "file": str(f), "movie_sha1": hashlib.sha1(data).hexdigest(), "rom_sha1": rom_sha1,
                     "input_frames": len(movie["frames"]), "frames": len(movie["frames"]) + MARGIN_FRAMES,
                     "status": status, "entry": entry["id"] if entry else None,
                     "title": entry["title"] if entry else movie["game"]})
        if entry and not movie["region"]:
            meta["region"] = entry["region"]
        (d / "input.json").write_text(json.dumps(events_from_frames(movie["frames"])))
        (d / "tas.json").write_text(json.dumps(meta, indent=1))
        rows.append((f.name, "%s -> %s (%s, %d frames, %d-button)" % (status, meta["title"], mid, len(movie["frames"]), movie["pads"])))
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
