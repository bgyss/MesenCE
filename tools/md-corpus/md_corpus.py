#!/usr/bin/env python3
"""md-corpus: library-wide comparison of MesenCE's Mega Drive family against oracle emulators.

Strategy and methodology: docs/research/md-oracle-testing-strategy.md. Everything is written
under one output root (default bin/md-corpus); every stage is restartable, and finished runs are
reused (a run directory with its done marker is not run again).

    md_corpus.py inventory ROM_DIR... --dat DAT_OR_ZIP... [--out ROOT]
    md_corpus.py tas import [MOVIE_FILE_OR_DIR...] [--sha1 ROM_SHA1]   # .bk2/.gmv you downloaded (tas.py;
                                                                      #  default: OUT/movies)
    md_corpus.py tas list
    md_corpus.py run   [--session attract|walker|tas] [--frames N] [--system md,32x,mcd] [--match TEXT]
                       [--all] [--limit N] [--jobs N] [--oracles gpgx,picodrive] [--no-compare] [--rerun] [--out ROOT]
    md_corpus.py compare [--out ROOT] [--jobs N] [--keep-audio] [--force]
    md_corpus.py report  [--out ROOT]
    md_corpus.py diff    [--session NAME]   # verdicts changed since the previous mesen-headless build

`run` needs bin/mesen-headless (mise run headless) and the libretro cores (RetroArch's, or
GPGX_CORE / PICODRIVE_CORE). Sega CD runs need a BIOS (MESEN_MCD_BIOS_DIR, default
~/mame/roms/segacd); 32X runs use Sega's boot ROMs from the same folder when present.
"""
import argparse
import json
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))


def cmd_inventory(args):
    import inventory
    out = pathlib.Path(args.out)
    entries = inventory.scan(args.roms, args.dat, out / "cache" / "hashes.json")
    (out / "corpus.json").write_text(json.dumps({"schema": "md-corpus/1", "entries": entries}, indent=1))
    by = {}
    for e in entries:
        s = by.setdefault(e["system"], [0, 0, 0])
        s[0] += 1
        s[1] += e["verified"]
        s[2] += e.get("primary", False)
    print("%-8s %7s %9s %8s" % ("system", "images", "verified", "primary"))
    for system, (n, v, p) in sorted(by.items()):
        print("%-8s %7d %9d %8d" % (system, n, v, p))
    print("wrote", out / "corpus.json")
    return 0


def cmd_run(args):
    import runner
    return runner.run_corpus(args)


def cmd_compare(args):
    import compare
    return compare.compare_all(args)


def cmd_report(args):
    import report
    return report.write_report(args)


def cmd_tas(args):
    import tas
    out = pathlib.Path(args.out)
    if args.action == "import":
        movies = args.movies or [out / "movies"]  # default: bin/md-corpus/movies (the download folder)
        return tas.import_movies(movies, out, args.sha1 or None)
    return tas.list_movies(out)


def cmd_diff(args):
    import report
    return report.write_diff(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.environ.get("MD_CORPUS_OUT", str(ROOT / "bin/md-corpus")))
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("inventory")
    p.add_argument("roms", nargs="+")
    p.add_argument("--dat", action="append", default=[], required=True)
    p.set_defaults(fn=cmd_inventory)

    p = sub.add_parser("run")
    p.add_argument("--session", default="attract", choices=["attract", "walker", "tas"])
    p.add_argument("--frames", type=int, default=3600)
    p.add_argument("--system", default="md,32x,mcd")
    p.add_argument("--match", default="")
    p.add_argument("--all", action="store_true", help="every image, not only the primary version per title")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--jobs", type=int, default=os.cpu_count())
    p.add_argument("--oracles", default="gpgx,picodrive")
    p.add_argument("--snap-every", type=int, default=300)
    p.add_argument("--no-compare", action="store_true", help="keep the audio; compare later with `compare`")
    p.add_argument("--rerun", action="store_true", help="run the selected sessions again even if this build already ran them")
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("tas")
    p.add_argument("action", choices=["import", "list"])
    p.add_argument("movies", nargs="*", help="movie files or folders (default: OUT/movies, i.e. bin/md-corpus/movies)")
    p.add_argument("--sha1", default="", help="the ROM's SHA-1, for movies without one (.gmv)")
    p.set_defaults(fn=cmd_tas)

    p = sub.add_parser("compare")
    p.add_argument("--jobs", type=int, default=os.cpu_count())
    p.add_argument("--keep-audio", action="store_true", help="keep WAV/raw audio after comparing")
    p.add_argument("--force", action="store_true", help="compare again sessions that already have a result")
    p.set_defaults(fn=cmd_compare)

    p = sub.add_parser("report")
    p.set_defaults(fn=cmd_report)

    p = sub.add_parser("diff")
    p.add_argument("--session", default="")
    p.set_defaults(fn=cmd_diff)

    args = ap.parse_args()
    args.out = str(pathlib.Path(args.out).expanduser().resolve())
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
