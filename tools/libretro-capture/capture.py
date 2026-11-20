#!/usr/bin/env python3
"""Headless libretro host: runs a core for N frames with a scripted input and writes the audio.

Used to record a reference emulator's audio (PicoDrive for the Mega Drive / 32X) for
tools/audio-compare/compare.py. The input script is mesen-headless' `--input` format:
{"frames": {"300": {"1": ["START"]}, "304": {"1": []}}} (pad 1 only; buttons hold until the next
entry). Output: OUT/audio.wav (the core's mix, 16-bit stereo) and OUT/manifest.json.

    capture.py CORE ROM OUT --frames N [--input input.json] [--system DIR] [--option key=value]
               [--snap-every N]   # OUT/snaps/NNNNNN.png (frame numbers as mesen-headless)

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
    """frame -> bitmask of libretro joypad buttons for pad 1, held until the next entry."""
    if not path:
        return {}
    data = json.loads(pathlib.Path(path).read_text())
    events = {}
    for frame, pads in data.get("frames", {}).items():
        mask = 0
        for button in pads.get("1", []):
            mask |= 1 << JOYPAD[MD_TO_RETRO[button.upper()]]
        events[int(frame)] = mask
    return events


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
    args = ap.parse_args()

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
            option_values[var.key] = C.c_char_p(value)
            C.cast(data, C.POINTER(Variable))[0].value = option_values[var.key].value
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
    buttons = [0]

    def input_state(port, device, index, button_id):
        if port != 0 or device != DEVICE_JOYPAD:
            return 0
        if button_id == DEVICE_ID_JOYPAD_MASK:
            return buttons[0]
        return 1 if buttons[0] >> button_id & 1 else 0

    def video(data, width, height, pitch):
        n = frame_no[0] + 1  # mesen-headless numbers frames from 1
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
    av = AvInfo()
    core.retro_get_system_av_info(C.byref(av))

    for frame in range(args.frames):
        if frame in script:
            buttons[0] = script[frame]
        frame_no[0] = frame
        core.retro_run()

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
        "sample_rate": av.timing.sample_rate, "options": {k.decode(): v.decode() for k, v in options.items()},
        "log": log_lines[-50:]}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
