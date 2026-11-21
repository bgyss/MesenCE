#!/usr/bin/env python3
"""Headless libretro host: runs a core for N frames with a scripted input and writes the audio.

Used to record a reference emulator's audio (PicoDrive for the Mega Drive / 32X) for
tools/audio-compare/compare.py. The input script is mesen-headless' `--input` format:
{"frames": {"300": {"1": ["START"]}, "304": {"1": []}}} (pad 1 only; buttons hold until the next
entry). Output: OUT/audio.wav (the core's mix, 16-bit stereo) and OUT/manifest.json.

    capture.py CORE ROM OUT --frames N [--input input.json] [--system DIR] [--option key=value]
               [--snap-every N]   # OUT/snaps/NNNNNN.png (frame numbers as mesen-headless)
               [--state-log FILE] # OUT/FILE: per-frame CRC32s, mesen-state-log/1 layout (below)
               [--state-blocks]   # with --state-log: OUT/FILE.blocks, as mesen-headless --state-blocks
               [--dump-at F1,F2]  # OUT/dumps/<frame>/<region>.bin

State log: the same layout as mesen-headless --state-log (docs/headless.md), with the memories a
libretro core exposes: MdWorkRam (RETRO_MEMORY_SYSTEM_RAM) and, where the core gives it,
MdVideoRam (RETRO_MEMORY_VIDEO_RAM), both as the core stores them (GPGX and PicoDrive keep 68000
memory as host-order 16-bit words, i.e. byte-swapped on little-endian hosts; --swap-words undoes
that before hashing and dumping). polled, pc and subpc are "-": a libretro host can't see them.
The frame column is the CRC32 of the core's raw video buffer (its own pixel format), useful only
within one core.

Nothing here is specific to MesenCE; it implements the small part of the libretro API that a core
needs to run headless.
"""
import argparse
import ctypes as C
import json
import pathlib
import sys
import struct
import wave
import zlib

# libretro.h constants
ENV_GET_VARIABLE = 15
ENV_SET_VARIABLES = 16
ENV_GET_VARIABLE_UPDATE = 17
ENV_GET_SYSTEM_DIRECTORY = 9
ENV_SET_PIXEL_FORMAT = 10
ENV_GET_LOG_INTERFACE = 27
ENV_GET_SAVE_DIRECTORY = 31
ENV_GET_CORE_OPTIONS_VERSION = 52
ENV_GET_INPUT_BITMASKS = 51 | 0x10000
ENV_GET_CAN_DUPE = 3
DEVICE_JOYPAD = 1
DEVICE_ID_JOYPAD_MASK = 256

# libretro joypad ids, and PicoDrive's Mega Drive mapping (libretro.c: Y = A, B = B, A = C,
# L = X, X = Y, R = Z, SELECT = MODE)
JOYPAD = {"B": 0, "Y": 1, "SELECT": 2, "START": 3, "UP": 4, "DOWN": 5, "LEFT": 6, "RIGHT": 7, "A": 8, "X": 9, "L": 10, "R": 11}
MD_TO_RETRO = {"A": "Y", "B": "B", "C": "A", "X": "L", "Y": "X", "Z": "R", "MODE": "SELECT",
               "START": "START", "UP": "UP", "DOWN": "DOWN", "LEFT": "LEFT", "RIGHT": "RIGHT"}


class GameInfo(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


class GameGeometry(C.Structure):
    _fields_ = [("base_width", C.c_uint), ("base_height", C.c_uint), ("max_width", C.c_uint),
                ("max_height", C.c_uint), ("aspect_ratio", C.c_float)]


class SystemTiming(C.Structure):
    _fields_ = [("fps", C.c_double), ("sample_rate", C.c_double)]


class AvInfo(C.Structure):
    _fields_ = [("geometry", GameGeometry), ("timing", SystemTiming)]


class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


ENV_CB = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO_CB = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH_CB = C.CFUNCTYPE(C.c_size_t, C.POINTER(C.c_int16), C.c_size_t)
POLL_CB = C.CFUNCTYPE(None)
STATE_CB = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
LOG_CB = C.CFUNCTYPE(None, C.c_int, C.c_char_p)  # variadic in C; the format string is enough here


class LogInterface(C.Structure):
    _fields_ = [("log", LOG_CB)]


PIXEL_FORMAT_XRGB8888, PIXEL_FORMAT_RGB565 = 1, 2


def write_png(path, width, height, rgb):
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def to_rgb(data, width, height, pitch, fmt):
    out = bytearray(width * height * 3)
    for y in range(height):
        row = data[y * pitch:]
        for x in range(width):
            if fmt == PIXEL_FORMAT_RGB565:
                v = row[x * 2] | row[x * 2 + 1] << 8
                r, g, b = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
            else:
                b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            o = (y * width + x) * 3
            out[o:o + 3] = bytes((r, g, b))
    return bytes(out)


def load_script(path):
    """frame (1-based, as mesen-headless) -> {port index: bitmask of libretro joypad buttons};
    a port's buttons hold until its next entry."""
    if not path:
        return {}
    data = json.loads(pathlib.Path(path).read_text())
    events = {}
    for frame, pads in data.get("frames", {}).items():
        for port, names in pads.items():
            mask = 0
            for button in names:
                mask |= 1 << JOYPAD[MD_TO_RETRO[button.upper()]]
            events.setdefault(int(frame), {})[int(port) - 1] = mask
    return events


RETRO_MEMORY_SYSTEM_RAM, RETRO_MEMORY_VIDEO_RAM = 2, 3


def swap_words(data):
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]
    return bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("core")
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--input")
    ap.add_argument("--system", default="")
    ap.add_argument("--option", action="append", default=[])
    ap.add_argument("--snap-every", type=int, default=0)
    ap.add_argument("--state-log")
    ap.add_argument("--state-blocks", action="store_true")
    ap.add_argument("--dump-at", default="")
    ap.add_argument("--swap-words", action="store_true")
    ap.add_argument("--pads", default="",
                    help="Mega Drive pad types: 3 or 6 for both ports, or P1,P2 (e.g. 3,6) (GPGX and PicoDrive: "
                         "RETRO_DEVICE_SUBCLASS(JOYPAD, 0/1))")
    args = ap.parse_args()
    dump_at = {int(f) for f in args.dump_at.split(",") if f}

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    options = {k.encode(): v.encode() for k, v in (o.split("=", 1) for o in args.option)}
    system_dir = C.c_char_p(str(pathlib.Path(args.system or out).resolve()).encode())
    save_dir = C.c_char_p(str(out.resolve()).encode())
    option_values = {}  # keeps returned strings alive
    log_lines = []
    pixel_format = [0]
    frame_no = [0]
    snaps = out / "snaps"

    def log(level, fmt):
        log_lines.append(fmt.decode(errors="replace").rstrip())

    log_cb = LOG_CB(log)
    log_iface = LogInterface(log_cb)

    def environment(cmd, data):
        cmd &= 0xFFFF | 0x10000
        if cmd == ENV_GET_SYSTEM_DIRECTORY:
            C.cast(data, C.POINTER(C.c_char_p))[0] = system_dir
            return True
        if cmd == ENV_GET_SAVE_DIRECTORY:
            C.cast(data, C.POINTER(C.c_char_p))[0] = save_dir
            return True
        if cmd == ENV_SET_PIXEL_FORMAT:
            pixel_format[0] = C.cast(data, C.POINTER(C.c_int))[0]
            return True
        if cmd == ENV_GET_CAN_DUPE:
            C.cast(data, C.POINTER(C.c_bool))[0] = True
            return True
        if cmd == ENV_GET_LOG_INTERFACE:
            C.cast(data, C.POINTER(LogInterface))[0] = log_iface
            return True
        if cmd == ENV_GET_VARIABLE:
            var = C.cast(data, C.POINTER(Variable))[0]
            value = options.get(var.key)
            if value is None:
                return False
            # The core keeps the pointer: hand it a buffer this host keeps alive (assigning bytes to
            # the c_char_p field would point it at a temporary copy)
            if var.key not in option_values:
                option_values[var.key] = C.create_string_buffer(value)
            C.cast(data, C.POINTER(C.c_void_p))[1] = C.cast(option_values[var.key], C.c_void_p).value
            return True
        if cmd == ENV_GET_VARIABLE_UPDATE:
            C.cast(data, C.POINTER(C.c_bool))[0] = False
            return True
        if cmd == ENV_GET_INPUT_BITMASKS:
            return True
        return False

    samples = bytearray()

    def audio_batch(ptr, frames):
        samples.extend(C.string_at(ptr, frames * 4))
        return frames

    def audio_sample(left, right):
        samples.extend(C.c_int16(left).value.to_bytes(2, "little", signed=True))
        samples.extend(C.c_int16(right).value.to_bytes(2, "little", signed=True))

    script = load_script(args.input)
    buttons = [0, 0]
    last_video = [b"", 0]

    def input_state(port, device, index, button_id):
        if port > 1 or device != DEVICE_JOYPAD:
            return 0
        if button_id == DEVICE_ID_JOYPAD_MASK:
            return buttons[port]
        return 1 if buttons[port] >> button_id & 1 else 0

    def video(data, width, height, pitch):
        n = frame_no[0] + 1  # mesen-headless numbers frames from 1
        if data and args.state_log:
            last_video[0] = C.string_at(data, pitch * height)
        if args.snap_every and data and n % args.snap_every == 0:
            snaps.mkdir(exist_ok=True)
            raw = C.string_at(data, pitch * height)
            write_png(snaps / ("%06d.png" % n), width, height, to_rgb(raw, width, height, pitch, pixel_format[0]))

    callbacks = [ENV_CB(environment), VIDEO_CB(video), AUDIO_CB(audio_sample),
                 AUDIO_BATCH_CB(audio_batch), POLL_CB(lambda: None), STATE_CB(input_state)]
    core = C.CDLL(args.core)
    core.retro_set_environment(callbacks[0])
    core.retro_set_video_refresh(callbacks[1])
    core.retro_set_audio_sample(callbacks[2])
    core.retro_set_audio_sample_batch(callbacks[3])
    core.retro_set_input_poll(callbacks[4])
    core.retro_set_input_state(callbacks[5])
    core.retro_init()

    rom = pathlib.Path(args.rom).read_bytes()
    rom_buf = C.create_string_buffer(rom, len(rom))
    info = GameInfo(str(pathlib.Path(args.rom).resolve()).encode(), C.cast(rom_buf, C.c_void_p), len(rom), None)
    core.retro_load_game.restype = C.c_bool
    if not core.retro_load_game(C.byref(info)):
        print("retro_load_game failed:\n" + "\n".join(log_lines), file=sys.stderr)
        return 1
    if args.pads:
        types = [int(t) for t in args.pads.split(",")]
        for port, kind in enumerate((types * 2)[:2]):
            if kind not in (3, 6):
                print("--pads: 3 or 6, not %d" % kind, file=sys.stderr)
                return 1
            device = ((1 if kind == 3 else 2) << 8) | 1  # RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0 or 1)
            core.retro_set_controller_port_device(port, device)
    av = AvInfo()
    core.retro_get_system_av_info(C.byref(av))

    core.retro_get_memory_data.restype = C.c_void_p
    core.retro_get_memory_size.restype = C.c_size_t
    regions = []
    for name, memory_id in (("MdWorkRam", RETRO_MEMORY_SYSTEM_RAM), ("MdVideoRam", RETRO_MEMORY_VIDEO_RAM)):
        if core.retro_get_memory_data(memory_id) and core.retro_get_memory_size(memory_id):
            regions.append((name, memory_id))

    def read_region(memory_id):
        # Re-read the pointer each time: a core may move its buffers
        data = C.string_at(core.retro_get_memory_data(memory_id), core.retro_get_memory_size(memory_id))
        return swap_words(data) if args.swap_words else data

    log = blocks = None
    if args.state_log:
        log = open(out / pathlib.Path(args.state_log).name, "w")
        if args.state_blocks:
            blocks = open(out / (pathlib.Path(args.state_log).name + ".blocks"), "wb")
        log.write("# mesen-state-log/1 libretro\n# frame clock polled pc subpc %s frame\n" % " ".join(n for n, _ in regions))

    for frame in range(args.frames):
        # Script frame numbers are 1-based like mesen-headless: entry "1" applies to the first retro_run
        for port, mask in script.get(frame + 1, {}).items():
            buttons[port] = mask
        frame_no[0] = frame
        core.retro_run()
        n = frame + 1
        if log:
            mem = {name: read_region(m) for name, m in regions}
            crcs = " ".join("%08x" % zlib.crc32(mem[name]) for name, _ in regions)
            log.write("%d - - - - %s %08x\n" % (n, crcs, zlib.crc32(last_video[0])))
            if blocks:
                wram = mem.get("MdWorkRam", b"")
                blocks.write(struct.pack("<256H", *(zlib.crc32(wram[i * 256:(i + 1) * 256]) & 0xFFFF if (i + 1) * 256 <= len(wram) else 0 for i in range(256))))
        if n in dump_at:
            d = out / "dumps" / str(n)
            d.mkdir(parents=True, exist_ok=True)
            for name, m in regions:
                (d / (name + ".bin")).write_bytes(read_region(m))
    if log:
        log.close()
    if blocks:
        blocks.close()

    core.retro_unload_game()
    core.retro_deinit()

    rate = int(round(av.timing.sample_rate))
    with wave.open(str(out / "audio.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(samples))
    (out / "manifest.json").write_text(json.dumps({
        "core": args.core, "rom": args.rom, "frames": args.frames, "fps": av.timing.fps,
        "sample_rate": av.timing.sample_rate, "swap_words": args.swap_words, "options": {k.decode(): v.decode() for k, v in options.items()},
        "log": log_lines[-50:]}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
