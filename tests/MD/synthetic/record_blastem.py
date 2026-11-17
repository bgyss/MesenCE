#!/usr/bin/env python3
"""Record the BlastEm reference outputs for the synthetic cartridges (M0 baseline).

  record_blastem.py ROM_DIR [--nativekit DIR] [--out tests/MD/synthetic/reference]

Runs nativekit's exploratory libretro runner (consoles/md/tests/oracle/run.py) with its pinned,
locally patched BlastEm core on every cartridge gen.py built, and stores:

  reference/baseline.json  core identity, per-case SHA-256 of every runner output, frame sizes,
                           and the 68000 data reads outside the interpreter's own code/steps
                           region (address, width, value) in bus order;
  reference/<case>/        final frame as PNG for render cases, and zlib-compressed chip PCM
                           (psg.raw.z, ym2612.raw.z, audio.raw.z) for audio cases.

BlastEm is a transitional differential oracle, not a hardware authority; see
docs/research/md-verification-findings.md. Outputs are evidence for comparison only.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def png_from_ppm(ppm):
    # P6 binary PPM -> 8-bit RGB PNG (no external dependencies).
    parts = ppm.split(b"\n", 3)
    if parts[0] != b"P6":
        raise ValueError("not a P6 PPM")
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    raw = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def m68k_reads(trace_bytes):
    header = struct.Struct("<4sHHQ")
    record = struct.Struct("<QBBBBII")
    magic, version, size, count = header.unpack_from(trace_bytes, 0)
    if magic != b"NMDT" or version != 2 or size != record.size:
        raise ValueError("unexpected trace format")
    reads = []
    for i in range(count):
        clock, device, op, width, _, address, value = record.unpack_from(trace_bytes, header.size + i * record.size)
        if device == 1 and op == 2 and not (0x200 <= address < 0x4000):
            reads.append([address, width, value])
    return reads


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("--nativekit", type=pathlib.Path, default=pathlib.Path.home() / "src/nativekit")
    ap.add_argument("--out", type=pathlib.Path, default=HERE / "reference")
    args = ap.parse_args()

    nk = args.nativekit
    core = nk / "build/toolchains/blastem/libblastem.dylib"
    if not core.exists():
        core = nk / "build/toolchains/blastem/libblastem.so"
    runner = nk / "consoles/md/tests/oracle/run.py"
    vasm = nk / "build/toolchains/vasm/vasmm68k_mot"
    import importlib.util
    sys.path.insert(0, str(nk / "consoles/md/tests/oracle"))
    sys.path.insert(0, str(nk / "consoles/md/tests/rom"))
    spec_ = importlib.util.spec_from_file_location("nk_rom_run", nk / "consoles/md/tests/rom/run.py")
    nk_run = importlib.util.module_from_spec(spec_)  # nativekit's case runner: only its TMSS stub helper
    spec_.loader.exec_module(nk_run)

    nk_commit = subprocess.run(["git", "-C", str(nk), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    args.out.mkdir(parents=True, exist_ok=True)
    baseline = {"format": "mesen-md-synthetic-reference/1", "oracle": "BlastEm (nativekit pinned candidate, patches 0001-0005)",
                "core_sha256": sha256(core.read_bytes()), "nativekit_commit": nk_commit,
                "qualification": "exploratory differential reference, not hardware evidence", "cases": {}}

    specs = sorted(args.roms.glob("*.json"))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        system_dir = nk_run._build_tmss_test_stub(tmp, vasm)
        for spec_path in specs:
            spec = json.loads(spec_path.read_text())
            name = spec["name"]
            rom = spec_path.with_suffix(".md")
            out = tmp / name
            cmd = [sys.executable, str(runner), "--core", str(core), "--rom", str(rom), "--frames", str(spec["frames"]),
                   "--region", spec["region"], "--model", spec["model"], "--pad1", spec["pad1"], "--pad2", spec["pad2"],
                   "--out", str(out), "--trace"]
            if spec["model"] == "md1va6":
                cmd += ["--system-dir", str(system_dir)]
            if spec["input"]["frames"]:
                inp = tmp / (name + ".input.json")
                inp.write_text(json.dumps(spec["input"]["frames"]))
                cmd += ["--input", str(inp)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode:
                print(f"FAIL {name}: {result.stderr.strip()[-400:]}")
                baseline["cases"][name] = {"status": "runner-failed"}
                continue
            manifest = json.loads((out / "manifest.json").read_text())
            outputs = {p.name: sha256(p.read_bytes()) for p in sorted(out.iterdir()) if p.name != "manifest.json"}
            entry = {"status": "ok", "rom_sha256": sha256(rom.read_bytes()), "frames": spec["frames"],
                     "frame_sizes": manifest.get("frame_sizes"), "outputs": outputs,
                     "m68k_reads": m68k_reads((out / "bus.trace").read_bytes())}
            case_dir = args.out / name
            if "frames" in spec["capture"]:
                case_dir.mkdir(exist_ok=True)
                last = sorted(out.glob("frame_*.ppm"))[-1]
                (case_dir / "frame_last.png").write_bytes(png_from_ppm(last.read_bytes()))
            audio = [k for k in spec["capture"] if k in ("psg", "ym", "mixed")]
            if audio:
                case_dir.mkdir(exist_ok=True)
                for raw in ("psg.raw", "ym2612.raw", "audio.raw"):
                    if (out / raw).exists():
                        (case_dir / (raw + ".z")).write_bytes(zlib.compress((out / raw).read_bytes(), 9))
                entry["sample_rate"] = manifest.get("sample_rate")
            baseline["cases"][name] = entry
            print(f"ok   {name}")
    (args.out / "baseline.json").write_text(json.dumps(baseline, indent=1, sort_keys=True) + "\n")
    print(f"{len(baseline['cases'])} cases -> {args.out / 'baseline.json'}")


if __name__ == "__main__":
    main()
