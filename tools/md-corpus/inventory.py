"""Corpus inventory: hash every Mega Drive / 32X / Sega CD / Sega CD 32X image under the given
folders, match it against No-Intro (cartridges) and Redump (discs) DATs, and write corpus.json.

Cartridges are hashed as stored (raw files, or the member of a .zip/.7z). Discs are matched on
their first (data) track: Redump lists one file per track, while many local dumps are a single
BIN with every track, so for a single-file cue the track 1 span is cut out at the next track's
INDEX 00 or INDEX 01 (whichever matches) and hashed.

Each entry gets a system (md, 32x, mcd, mcd32x), a region for the emulators (from the DAT name's
region tags, else the folder or file name) and `primary`: one preferred version per title (USA,
then World, then Europe, then Japan; latest revision; no betas, prototypes, pirates or
unlicensed dumps), which is what library-wide passes run by default.
"""
import hashlib
import json
import pathlib
import re
import subprocess
import xml.etree.ElementTree as ET
import zipfile

CART_EXT = {".md", ".bin", ".gen", ".smd", ".32x"}
SECTOR = 2352

EXCLUDE_TAGS = re.compile(r"\((Beta|Proto|Pirate|Unl|Sample|Demo|Hack|Program|Kiosk|Test Program|Aftermarket)[^)]*\)", re.I)


def sha1_bytes(data):
    return hashlib.sha1(data).hexdigest()


def sha1_file(path, start=0, length=None):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        f.seek(start)
        left = length
        while True:
            n = 1 << 20 if left is None else min(1 << 20, left)
            if n == 0:
                break
            chunk = f.read(n)
            if not chunk:
                break
            h.update(chunk)
            if left is not None:
                left -= len(chunk)
    return h.hexdigest()


def load_dat(path):
    """sha1 -> (game name, file name, dat header name). Accepts a .dat or a .zip holding one."""
    path = pathlib.Path(path).expanduser()
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as z:
            name = next(n for n in z.namelist() if n.lower().endswith(".dat"))
            data = z.read(name)
    else:
        data = path.read_bytes()
    root = ET.fromstring(data)
    header = root.findtext("header/name") or path.stem
    out = {}
    for game in root.iter("game"):
        for rom in game.iter("rom"):
            sha1 = (rom.get("sha1") or "").lower()
            if sha1:
                out[sha1] = (game.get("name"), rom.get("name"), header)
    return out


def msf(text):
    m, s, f = (int(x) for x in text.split(":"))
    return (m * 60 + s) * 75 + f


def parse_cue(cue):
    """[(file, [(track number, type, {index: frame})])]"""
    files = []
    for line in cue.read_text(errors="replace").splitlines():
        parts = line.strip().split()
        if not parts:
            continue
        if parts[0].upper() == "FILE":
            name = line.strip()[5:].rsplit(" ", 1)[0].strip().strip('"')
            files.append((name, []))
        elif parts[0].upper() == "TRACK" and files:
            files[-1][1].append((int(parts[1]), parts[2].upper(), {}))
        elif parts[0].upper() == "INDEX" and files and files[-1][1]:
            files[-1][1][-1][2][int(parts[1])] = msf(parts[2])
    return files


def disc_track1_hashes(cue):
    """Candidate SHA-1s of the first track, and its path and byte span."""
    files = parse_cue(cue)
    if not files:
        return [], None
    first_file = cue.parent / files[0][0]
    if not first_file.exists():
        return [], None
    tracks = files[0][1]
    if len(files) > 1 or len(tracks) <= 1:
        return [sha1_file(first_file)], first_file
    nxt = tracks[1][2]
    cuts = sorted({nxt[i] * SECTOR for i in (0, 1) if i in nxt})
    return [sha1_file(first_file, 0, c) for c in cuts], first_file


def disc_header(path):
    """System name and region field of a Sega CD data track (MODE1/2352: user data at +16)."""
    with open(path, "rb") as f:
        f.seek(16)
        head = f.read(0x200)
    return head[0:16].decode("ascii", "replace"), head[0x1F0:0x1F3].decode("ascii", "replace")


def region_from_name(name):
    tags = re.findall(r"\(([^)]*)\)", name)
    text = " ".join(tags)
    if re.search(r"\b(USA|World|Brazil)\b", text) or re.search(r"\((U|JU|UE|JUE)\)", name):
        return "ntsc"
    if re.search(r"\b(Europe|Germany|France|Spain|Italy|UK|Australia|Sweden|Netherlands)\b", text) or re.search(r"\((E)\)", name):
        return "pal"
    if re.search(r"\b(Japan|Korea|Asia|Taiwan|China)\b", text) or re.search(r"\((J)\)", name):
        return "ntsc-j"
    return "auto"


def title_key(name):
    return re.sub(r"\s*\(.*$", "", name).strip().lower()


def preference(name):
    tags = re.findall(r"\(([^)]*)\)", name)
    # NTSC first (60 Hz, and what most TAS movies use): USA, World, then Europe, then Japan
    regions = ",".join(tags)
    region_rank = 0 if "USA" in regions else 1 if "World" in regions else 2 if "Europe" in regions else 3 if "Japan" in regions else 4
    rev = 0
    for t in tags:
        m = re.match(r"Rev (\w+)", t)
        if m:
            rev = int(m.group(1), 36)
    # Tags other than regions, languages and revisions mark variants (compilations, re-releases,
    # alternates): plain releases first
    plain = re.compile(r"^(Rev \w+|[A-Z][a-z]{1,2}(,[A-Z][a-z]{1,2})*|(USA|World|Europe|Japan|Korea|Brazil|Asia|Taiwan|China|Australia|Germany|France|Spain|Italy|UK|Sweden|Netherlands)(, ?\w+)*)$")
    extra = sum(1 for t in tags if not plain.match(t))
    return (1 if EXCLUDE_TAGS.search(name) else 0, region_rank, extra, -rev, len(name))


def list_7z(path):
    out = subprocess.run(["7z", "l", "-slt", str(path)], capture_output=True, text=True).stdout
    # Entries follow the "----------" line; directories have a "D" attribute
    members, cur, started = [], None, False
    for line in out.splitlines():
        if line.startswith("----------"):
            started = True
        elif started and line.startswith("Path = "):
            cur = line[7:]
            members.append(cur)
        elif started and line.startswith("Attributes = ") and "D" in line[13:] and cur in members:
            members.remove(cur)
    return [m for m in members if pathlib.Path(m).suffix.lower() in CART_EXT]


def read_member(path, member):
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as z:
            return z.read(member)
    return subprocess.run(["7z", "e", "-so", str(path), member], capture_output=True, check=True).stdout


def scan(roots, dats, cache_path):
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    index = {}
    for d in dats:
        index.update(load_dat(d))
    entries = []

    def cached(path, key_extra, compute):
        st = path.stat()
        key = "%s|%d|%d|%s" % (path, st.st_size, int(st.st_mtime), key_extra)
        if key not in cache:
            cache[key] = compute()
        return cache[key]

    for root in roots:
        root = pathlib.Path(root).expanduser()
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            ext = path.suffix.lower()
            folder = str(path.parent.relative_to(root)) if path.parent != root else ""
            if ext == ".cue":
                result = cached(path, "cue", lambda: disc_track1_hashes(path)[0])
                hit = next((index[h] for h in result if h in index), None)
                files = parse_cue(path)
                first = path.parent / files[0][0] if files else None
                header = disc_header(first) if first and first.exists() else ("", "")
                if not header[0].startswith("SEGADISCSYSTEM") and not header[0].startswith("SEGA"):
                    continue  # not a Sega CD disc (audio CD, other system)
                name = hit[0] if hit else "%s / %s" % (path.parent.name, path.stem)
                is32x = bool(re.search(r"32X", name + " " + str(path), re.I))
                entries.append({
                    "sha1": result[0] if result else None, "track1_sha1": result, "system": "mcd32x" if is32x else "mcd",
                    "title": name, "dat": hit[2] if hit else None, "verified": bool(hit),
                    "path": str(path), "member": None, "region": region_from_name(name if hit else path.parent.name + " " + path.name),
                    "disc_region_field": header[1].strip(), "folder": folder})
            elif ext in CART_EXT or ext in (".zip", ".7z"):
                if ext == ".bin" and (path.with_suffix(".cue").exists() or any(path.parent.glob("*.cue"))):
                    continue  # a disc track
                if ext in CART_EXT:
                    members = [None]
                elif ext == ".zip":
                    with zipfile.ZipFile(path) as z:
                        members = [n for n in z.namelist() if pathlib.Path(n).suffix.lower() in CART_EXT]
                else:
                    members = cached(path, "7zlist2", lambda: list_7z(path))
                for member in members:
                    def compute(member=member):
                        data = path.read_bytes() if member is None else read_member(path, member)
                        return [sha1_bytes(data), len(data), data[0x100:0x110].decode("ascii", "replace")]
                    sha1, size, console = cached(path, "m:%s" % member, compute)
                    hit = index.get(sha1)
                    shown = member or path.name
                    name = hit[0] if hit else pathlib.Path(shown).stem
                    dat = hit[2] if hit else None
                    is32x = (dat and "32X" in dat) or pathlib.Path(shown).suffix.lower() == ".32x" or "32X" in console
                    flags = []
                    if re.search(r"\[(f|b|h|o|t|p)\d*\]", shown):
                        flags.append("goodtools-modified")
                    entries.append({
                        "sha1": sha1, "system": "32x" if is32x else "md", "title": name, "dat": dat, "verified": bool(hit),
                        "path": str(path), "member": member, "size": size, "region": region_from_name(name if hit else shown),
                        "flags": flags, "folder": folder})
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(cache))

    # Same image found twice (e.g. a .32x and its .7z): keep the first
    seen, unique = set(), []
    for e in entries:
        key = (e["sha1"], e["system"])
        if e["sha1"] and key in seen:
            continue
        seen.add(key)
        unique.append(e)

    # One preferred entry per title and system; discs of one game (Disc 1, Disc 2) stay separate
    groups = {}
    for e in unique:
        if "goodtools-modified" in e.get("flags", []) or "[BIOS]" in e["title"]:
            e["primary"] = False
            continue
        disc = re.search(r"\(Dis[ck] ?(\d)|disc[ -]?(\d)|CD ?(\d)of", e["title"] + " " + e["path"], re.I)
        key = (e["system"], title_key(e["title"]), disc.group(0).lower() if disc else "")
        groups.setdefault(key, []).append(e)
    for members in groups.values():
        members.sort(key=lambda e: (0 if e["verified"] else 1,) + preference(e["title"]))
        for i, e in enumerate(members):
            e["primary"] = i == 0 and not EXCLUDE_TAGS.search(e["title"])
    for e in unique:
        e["id"] = (e["sha1"] or hashlib.sha1(e["path"].encode()).hexdigest())[:12]
    return unique
