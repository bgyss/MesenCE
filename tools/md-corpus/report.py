"""`md_corpus.py report`: ROOT/report.html and ROOT/summary.tsv from every result.

The page is local and private (it shows screenshots of commercial games): one table per session
kind, sessions sorted by score (most severe first), with MesenCE's and the main oracle's last
common snapshot side by side, the flags and the key numbers. Nothing is uploaded.
"""
import html
import json
import pathlib

FLAG_HELP = {
    "mesen-failed": "MesenCE exited with an error or timed out; the oracles didn't",
    "mesen-frozen": "MesenCE's work RAM and picture stopped changing for the last 10 s; the oracles' didn't",
    "mesen-black": "MesenCE's last snapshot is black; the oracles' isn't",
    "mesen-no-input": "the game stopped reading its pads in MesenCE for the last 20 s",
    "state-diverges": "work RAM diverges from both oracles, which agree with each other",
    "state-diverges-single-oracle": "work RAM diverges from the only oracle (needs a source before any fix)",
    "visual": "snapshots differ from the oracles more than the oracles differ from each other",
    "audio": "audio flags from tools/audio-compare against the main oracle",
    "unsupported": "Sega CD 32X: not emulated yet",
    "audio-not-compared": "an audio file was missing (deleted by an earlier comparison): re-run the whole session",
}


def load(out):
    results = {}
    for f in sorted((out / "results").glob("*/*.json")):
        results.setdefault(f.parent.name, []).append(json.loads(f.read_text()))
    return results


def snap_pair(out, session, r):
    run = out / "runs" / session / r["id"]
    main = r.get("main_oracle")
    vis = r["visual"].get("mesen~" + main) if main else None
    frames = vis["frames"] if vis else []
    if not frames:
        snaps = sorted((run / "mesen" / "snaps").glob("*.png")) if (run / "mesen" / "snaps").exists() else []
        return [(s.relative_to(out).as_posix(), "MesenCE") for s in snaps[-1:]]
    name = "%06d.png" % frames[-1]
    return [((run / e / "snaps" / name).relative_to(out).as_posix(), "MesenCE" if e == "mesen" else e) for e in ("mesen", main)]


def fmt_state(r):
    parts = []
    for pair, p in r["state"].items():
        if not p:
            continue
        div = p["diverged_at"]
        parts.append("%s: agree %.2f, lockstep %.0f%%%s, offset ≤%d (%d drifts)" % (
            pair, p["agreement_median"], 100 * p["lockstep"], ", <b>diverged at %d</b>" % div if div else "",
            p["offset_max"], p["offset_changes"]))
    return "<br>".join(parts)


def fmt_visual(r):
    return "<br>".join("%s: %.3f" % (k, v["median"]) for k, v in r["visual"].items() if v)


def fmt_audio(r):
    a = r.get("audio")
    if not a:
        return ""
    if a.get("error"):
        return "error: " + html.escape(a["error"])
    if a.get("silent"):
        return "both silent"
    chips = ", ".join("%s %+.1f dB" % (k, v) for k, v in (a.get("chip_level_db_vs_mesen") or {}).items() if k != "ym2612")
    return "spectral %.2f dB, envelope %.2f%s%s" % (a["spectral_distance_db"], a["envelope_correlation"],
                                                    "<br>" + chips if chips else "",
                                                    "<br><b>" + ", ".join(a["flags"]) + "</b>" if a.get("flags") else "")


def write_report(args):
    out = pathlib.Path(args.out)
    results = load(out)
    rows_tsv = ["session\tid\tsystem\ttitle\tscore\tflags\tstate\tvisual_median\taudio_flags"]
    body = []
    for session, rs in sorted(results.items()):
        rs.sort(key=lambda r: (-r["score"], r["system"], r["title"]))
        by_system = {}
        for r in rs:
            s = by_system.setdefault(r["system"], [0, 0])
            s[0] += 1
            s[1] += 1 if r["score"] == 0 else 0
        summary = " · ".join("%s: %d/%d clean" % (k, v[1], v[0]) for k, v in sorted(by_system.items()))
        body.append("<h2>%s</h2><p>%s</p>" % (html.escape(session), summary))
        body.append("<table><tr><th>#</th><th>title</th><th>flags</th><th>snapshots (last common frame)</th>"
                    "<th>state (work RAM)</th><th>visual</th><th>audio</th></tr>")
        for i, r in enumerate(rs, 1):
            flags = " ".join('<span class="flag" title="%s">%s</span>' % (html.escape(FLAG_HELP.get(f, "")), f) for f in r["flags"])
            imgs = "".join('<figure><img src="%s" loading="lazy"><figcaption>%s</figcaption></figure>' % (html.escape(p), html.escape(l))
                           for p, l in snap_pair(out, session, r))
            body.append("<tr class='%s'><td>%d<br><small>%d</small></td><td><b>%s</b><br><small>%s · %s · %s</small></td>"
                        "<td>%s</td><td class='snaps'>%s</td><td><small>%s</small></td><td><small>%s</small></td><td><small>%s</small></td></tr>" % (
                            "bad" if r["score"] >= 40 else "warn" if r["score"] else "ok", i, r["score"], html.escape(r["title"]),
                            r["system"], r["region"], r["id"], flags, imgs, fmt_state(r), fmt_visual(r), fmt_audio(r)))
            vm = [v["median"] for k, v in r["visual"].items() if v and k.startswith("mesen~")]
            rows_tsv.append("\t".join(str(x) for x in (
                session, r["id"], r["system"], r["title"], r["score"], ",".join(r["flags"]),
                ";".join("%s=%s" % (k, p and p["diverged_at"]) for k, p in r["state"].items()),
                max(vm) if vm else "", ",".join((r.get("audio") or {}).get("flags") or []))))
        body.append("</table>")
    page = """<!doctype html><html><head><meta charset="utf-8"><title>md-corpus report</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root { --bg:#fff; --fg:#1b1b1b; --muted:#666; --line:#ddd; --bad:#fde8e8; --warn:#fff6dd; --flag:#8a1c1c; }
@media (prefers-color-scheme: dark) { :root { --bg:#151515; --fg:#e8e8e8; --muted:#999; --line:#333; --bad:#3a1d1d; --warn:#332b14; --flag:#ff8a8a; } }
body { background:var(--bg); color:var(--fg); font:14px/1.4 system-ui, sans-serif; margin:16px; }
table { border-collapse:collapse; width:100%%; } td, th { border-bottom:1px solid var(--line); padding:6px; vertical-align:top; text-align:left; }
tr.bad { background:var(--bad); } tr.warn { background:var(--warn); }
.flag { color:var(--flag); font-weight:600; margin-right:6px; white-space:nowrap; cursor:help; }
.snaps { white-space:nowrap; } figure { display:inline-block; margin:0 4px 0 0; } img { width:160px; image-rendering:pixelated; }
figcaption, small { color:var(--muted); font-size:12px; }
</style></head><body><h1>md-corpus report</h1>
<p>MesenCE vs oracle emulators (docs/research/md-oracle-testing-strategy.md). Scores rank findings; flags are judgment-call
heuristics, not verdicts. Hover a flag for its meaning.</p>%s</body></html>""" % "\n".join(body)
    (out / "report.html").write_text(page)
    (out / "summary.tsv").write_text("\n".join(rows_tsv) + "\n")
    print("wrote", out / "report.html", "and", out / "summary.tsv")
    return 0
