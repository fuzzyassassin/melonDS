import atexit
import ctypes
import os
import sys
import weakref
from ctypes import (POINTER, Structure, c_char, c_char_p, c_int, c_int16, c_int32, c_uint8, c_uint16, c_uint32,
                    c_uint64)
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

IDLE, RUNNING, PARKED = 0, 1, 2
# "reset" (API level 6) is not a park: a GDB client reset the running console or one CPU
BREAK_KINDS = ["exec", "read", "write", "step", "request", "fault", "reset"]
KEYS = ["A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y"]
WATCH_READ, WATCH_WRITE = 1, 2
POINT_LUA = 0x100       # mds_point.flags: a Lua memory hook (API level 6)
# pc of a DMA watchpoint break and tag of a DMA last-writer record: DMA_TAG | channel << 1 | (1 for ARM7)
DMA_TAG = 0xFFFFFFF0
# mds_frames for an unknown console (API level 6; older libraries return 0)
FRAMES_ERROR = 0xFFFFFFFFFFFFFFFF
# mds_set_render flags (API level 8)
RENDER_SKIP_2D, RENDER_SKIP_3D = 1, 2
RENDER_SKIP_ALL = RENDER_SKIP_2D | RENDER_SKIP_3D
# frames to draw before a screenshot after skipping: 2D shows a frame's lines at once, 3D the frame drawn the frame
# before (docs/headless-api.md, section 4, "Render control")
RENDER_FRAMES_2D, RENDER_FRAMES_3D = 1, 2
RENDER_STAT_NAMES = ["skip_wanted", "skip", "threaded_wanted", "threaded", "lines_2d_skipped", "frames_3d_skipped",
                     "capture_lines", "frames_3d_for_capture", "late_3d_frames"]


class MdsBreak(Structure):
    _fields_ = [("kind", c_int32), ("cpu", c_int32), ("size", c_int32), ("id", c_int32),
                ("pc", c_uint32), ("addr", c_uint32), ("value", c_uint32), ("dma", c_int32)]


class MdsPoint(Structure):
    _fields_ = [("id", c_int32), ("watch", c_int32), ("cpumask", c_uint32), ("flags", c_uint32),
                ("enabled", c_uint32), ("start", c_uint32), ("end", c_uint32), ("hits", c_uint64)]


class MdsTrace(Structure):
    _fields_ = [("cycle", c_uint64), ("pc", c_uint32), ("instr", c_uint32), ("cpsr", c_uint32), ("cpu", c_uint32)]


class MdsFrame(Structure):
    _fields_ = [("site", c_uint32), ("entry", c_uint32), ("ret", c_uint32), ("sp", c_uint32),
                ("mode", c_uint32), ("exception", c_uint32)]


class MdsPacket(Structure):
    # total (API level 6) sits in what was the struct's padding: older libraries leave it 0
    _fields_ = [("timestamp", c_uint64), ("sent", c_uint32), ("type", c_uint32), ("length", c_uint32),
                ("total", c_uint32)]


class MdsCheat(Structure):
    _fields_ = [("cheat", c_int32), ("enabled", c_int32), ("words", c_int32), ("name", c_char * 116)]


class MdsTouchPoint(Structure):
    _fields_ = [("offset", c_uint32), ("x", c_uint16), ("y", c_uint16), ("down", c_uint8), ("pad", c_uint8 * 3)]


class MdsInput(Structure):
    _fields_ = [("seq", c_uint32), ("buttons", c_uint16), ("lid", c_uint8), ("touch_count", c_uint8),
                ("mic_kind", c_uint8), ("flags", c_uint8), ("mic_count", c_uint16), ("mic_level", c_uint16),
                ("pad2", c_uint16)]


class MdsInputLogEntry(Structure):
    _fields_ = [("frame", c_uint64), ("seq", c_uint32), ("buttons", c_uint16), ("lid", c_uint8),
                ("touch_count", c_uint8), ("mic_kind", c_uint8), ("pad", c_uint8), ("mic_level", c_uint16),
                ("pad2", c_uint16), ("pad3", c_uint16)]


INPUT_FLAG_CLIENT = 1
INPUT_MODES = {"repeat": 0, "neutral": 1, "wait": 2}


class MelonError(RuntimeError):
    pass


@dataclass
class Break:
    kind: str
    cpu: int
    pc: int
    addr: int
    value: int
    size: int
    id: int
    dma: int = -1


def _find_library():
    env = os.environ.get("MELONDS_HEADLESS_LIB")
    if env:
        return Path(env)
    root = Path(__file__).resolve().parents[3]
    for name in ("melonds_headless.dll", "melonds_headless.so", "melonds_headless.dylib"):
        for candidate in root.glob(f"build/*/{name}"):
            return candidate
    raise MelonError("melonds_headless library not found; set MELONDS_HEADLESS_LIB")


# Every function's ctypes signature. Functions are bound on first use (_Lib), so the wrapper loads a library
# that lacks some of them (an older build) and only a call to a missing one fails.
_SIGNATURES = {
    "mds_version": (c_int, []),
    "mds_error": (c_char_p, []),
    "mds_set_local_dir": (None, [c_char_p]),
    "mds_set_log_level": (None, [c_int]),
    "mds_mp_timeout": (c_int, [c_int]),
    "mds_set_pacing": (None, [c_int]),
    "mds_create": (c_int, [c_char_p, c_char_p, c_char_p, c_char_p, c_int]),
    "mds_destroy": (c_int, [c_int]),     # void before API level 6: the value is then meaningless
    "mds_load_rom": (c_int, [c_int, c_char_p, c_char_p]),
    "mds_reset": (c_int, [c_int]),
    "mds_set_rtc": (c_int, [c_int, c_int, c_int, c_int, c_int, c_int, c_int]),
    "mds_get_rtc": (c_int, [c_int, POINTER(c_int)]),
    "mds_run": (c_int, [POINTER(c_int), c_int, c_uint32, c_int, POINTER(c_int)]),
    "mds_wait": (c_int, [POINTER(c_int), c_int, c_int, POINTER(c_int)]),
    "mds_gdb": (c_int, [c_int, c_int, c_int, c_int]),
    "mds_start": (c_int, [c_int, c_uint32]),
    "mds_stop_after": (c_int, [c_int, c_uint32]),
    "mds_interrupt": (c_int, [c_int]),
    "mds_state": (c_int, [c_int]),
    "mds_take_break": (c_int, [c_int, POINTER(MdsBreak)]),
    "mds_frames": (c_uint64, [c_int]),
    "mds_screen": (c_int, [c_int, POINTER(c_uint32)]),
    "mds_set_keys": (c_int, [c_int, c_uint32]),
    "mds_touch": (c_int, [c_int, c_int, c_int]),
    "mds_set_lid": (c_int, [c_int, c_int]),
    "mds_input_submit": (c_int, [c_int, POINTER(MdsInput), POINTER(MdsTouchPoint), POINTER(c_int16)]),
    "mds_input_submit_multi": (c_int, [POINTER(c_int), c_int, POINTER(MdsInput), POINTER(POINTER(MdsTouchPoint)),
                                       POINTER(POINTER(c_int16)), POINTER(c_int)]),
    "mds_debug_recv_pending": (c_int, [c_int, c_int]),
    "mds_input_mode": (c_int, [c_int, c_int, c_int]),
    "mds_input_depth": (c_int, [c_int]),
    "mds_input_waiting": (c_int, [c_int]),
    "mds_input_clear": (c_int, [c_int]),
    "mds_input_log": (c_int, [c_int, c_uint64, POINTER(MdsInputLogEntry), c_int]),
    "mds_read": (c_int, [c_int, c_int, c_uint32, c_uint32, POINTER(c_uint8)]),
    "mds_write": (c_int, [c_int, c_int, c_uint32, c_uint32, POINTER(c_uint8)]),
    "mds_get_regs": (c_int, [c_int, c_int, POINTER(c_uint32)]),
    "mds_set_reg": (c_int, [c_int, c_int, c_int, c_uint32]),
    "mds_bp_add": (c_int, [c_int, c_uint32, c_uint32]),
    "mds_wp_add": (c_int, [c_int, c_uint32, c_uint32, c_uint32, c_uint32]),
    "mds_point_remove": (c_int, [c_int, c_int]),
    "mds_point_enable": (c_int, [c_int, c_int, c_int]),
    "mds_point_list": (c_int, [c_int, POINTER(MdsPoint), c_int]),
    "mds_step": (c_int, [c_int, c_int, c_uint32]),
    "mds_trace": (c_int, [c_int, c_uint32, c_uint32, c_uint32, c_uint32]),
    "mds_trace_take": (c_int, [c_int, POINTER(MdsTrace), c_int]),
    "mds_calls": (c_int, [c_int, c_int]),
    "mds_callstack": (c_int, [c_int, c_int, POINTER(MdsFrame), c_int]),
    "mds_threadstacks": (c_int, [c_int, c_int, POINTER(c_uint32), POINTER(c_uint32), c_int, POINTER(MdsFrame), c_int]),
    "mds_overlays": (c_int, [c_int, POINTER(c_int), POINTER(c_uint32), POINTER(c_uint32), c_int]),
    "mds_callcounts_take": (c_int, [c_int, c_int, POINTER(c_uint32), POINTER(c_uint64), c_int]),
    "mds_coverage": (c_int, [c_int, c_int]),
    "mds_coverage_take": (c_int, [c_int, c_int, POINTER(c_uint32), c_int]),
    "mds_lastwriter": (c_int, [c_int, c_int]),
    "mds_lastwriter_get": (c_uint32, [c_int, c_uint32]),
    "mds_savestate": (c_int, [c_int, c_char_p]),
    "mds_loadstate": (c_int, [c_int, c_char_p]),
    "mds_lua_run": (c_int, [c_int, c_char_p, c_int]),
    "mds_lua_exec": (c_int, [c_int, c_char_p]),
    "mds_lua_stop": (c_int, [c_int]),
    "mds_lua_output": (c_int, [c_int, POINTER(c_char), c_int]),
    "mds_packets": (c_int, [c_int, c_int, c_int]),
    "mds_packets_take": (c_int, [c_int, POINTER(MdsPacket), POINTER(c_uint8), c_int, c_int]),
}
_SIGNATURES |= {
    "mds_serve": (c_int, [c_int, c_int, POINTER(c_int)]),
    "mds_serve_stop": (None, []),
    "mds_serve_wait": (c_int, [c_int]),
    "mds_serve_name": (None, [c_char_p]),
    "mds_set_instance_base": (c_int, [c_int]),
    "mds_instance_base": (c_int, []),
    "mds_mac": (c_int, [c_int, POINTER(c_uint8)]),
    "mds_save_path": (c_int, [c_int, POINTER(c_char), c_int]),
}
# API level 2 (fix/api-quirks)
_SIGNATURES |= {
    "mds_has": (c_int, [c_char_p]),
    "mds_serve_host": (c_int, [c_char_p, c_int, c_int, POINTER(c_int)]),
    "mds_gdb_host": (c_int, [c_int, c_char_p, c_int, c_int, c_int]),
    "mds_lastwriter_query": (c_int, [c_int, c_uint32, POINTER(c_uint32), POINTER(c_int)]),
}
# API level 3 (feature/cheats)
_SIGNATURES |= {
    "mds_cheat_add": (c_int, [c_int, c_char_p, c_char_p, c_int]),
    "mds_cheat_remove": (c_int, [c_int, c_int]),
    "mds_cheat_enable": (c_int, [c_int, c_int, c_int]),
    "mds_cheat_clear": (c_int, [c_int]),
    "mds_cheat_list": (c_int, [c_int, POINTER(MdsCheat), c_int]),
    "mds_cheat_get": (c_int, [c_int, c_int, POINTER(c_char), c_int, POINTER(c_uint32), c_int]),
}
# API level 6 (fix/api-quirks-2)
_SIGNATURES |= {
    "mds_debug_packet": (c_int, [c_int, c_int, c_int, POINTER(c_uint8), c_int]),
    "mds_point_count": (c_int, [c_int]),
    "mds_reset_count": (c_int, [c_int]),
}
# API level 8 (feature/render-control)
_SIGNATURES |= {
    "mds_set_render": (c_int, [c_int, c_uint32]),
    "mds_set_threaded_3d": (c_int, [c_int, c_int]),
    "mds_render_stats": (c_int, [c_int, POINTER(c_uint64), c_int]),
}


# API level 7 (feature/probes; melonds_probes.h): probes, memory access log, breakpoint actions, overlay tags
class MdsProbeRead(Structure):
    _fields_ = [("reg", c_int32), ("offset", c_int32), ("len", c_uint32), ("when", c_uint32)]


class MdsProbeSpec(Structure):
    _fields_ = [("cpumask", c_uint32), ("addr", c_uint32), ("lr_start", c_uint32), ("lr_end", c_uint32),
                ("flags", c_uint32), ("reads", MdsProbeRead * 4)]


class MdsProbeRecord(Structure):
    _fields_ = [("seq", c_uint64), ("frame", c_uint64), ("cycle", c_uint64), ("ret_frame", c_uint64),
                ("ret_cycle", c_uint64), ("probe", c_int32), ("cpu", c_int32), ("kind", c_int32), ("overlay", c_int32),
                ("depth", c_int32), ("addr", c_uint32), ("lr", c_uint32), ("sp", c_uint32), ("args", c_uint32 * 4),
                ("returned", c_uint32), ("ret", c_uint32 * 2), ("flags", c_uint32), ("entry_len", c_uint16 * 4),
                ("ret_len", c_uint16 * 4), ("data_len", c_uint32), ("pad", c_uint32)]


class MdsMemlogSpec(Structure):
    _fields_ = [("cpumask", c_uint32), ("start", c_uint32), ("size", c_uint32), ("flags", c_uint32),
                ("pc_start", c_uint32), ("pc_end", c_uint32)]


class MdsMemlogRecord(Structure):
    _fields_ = [("seq", c_uint64), ("frame", c_uint64), ("cycle", c_uint64), ("pc", c_uint32), ("addr", c_uint32),
                ("value", c_uint32), ("range", c_int32), ("cpu", c_uint8), ("size", c_uint8), ("flags", c_uint8),
                ("dma", c_uint8), ("pad", c_uint32)]


class MdsActionSpec(Structure):
    _fields_ = [("cpumask", c_uint32), ("addr", c_uint32), ("kind", c_int32), ("reg", c_int32), ("value", c_uint32),
                ("value2", c_uint32), ("target", c_uint32), ("size", c_uint32), ("lr", c_uint32), ("first", c_uint32),
                ("times", c_uint32), ("flags", c_uint32)]


class MdsActionRecord(Structure):
    _fields_ = [("seq", c_uint64), ("frame", c_uint64), ("cycle", c_uint64), ("hit", c_uint64), ("action", c_int32),
                ("cpu", c_int32), ("kind", c_int32), ("pc", c_uint32), ("lr", c_uint32), ("regs", c_uint32 * 4),
                ("value", c_uint32), ("target", c_uint32), ("pad", c_uint32)]


PROBE_READ_ENTRY, PROBE_READ_RETURN = 1, 2
PROBE_NO_RETURN = 1
PROBE_FORCED, PROBE_RETURN_LOST = 1, 2
OVERLAY_STATIC, OVERLAY_AMBIGUOUS, OVERLAY_UNKNOWN = -1, -2, -3
MEMLOG_READ, MEMLOG_WRITE, MEMLOG_DMA = 1, 2, 4
ACTION_KINDS = {"return": 1, "set_reg": 2, "write": 3, "skip": 4}
ACTION_SET_R1 = 1
OVERLAY_AT_ERROR = -(2 ** 31)
_REGS = {f"r{k}": k for k in range(16)} | {"sp": 13, "lr": 14, "pc": 15}

_SIGNATURES |= {
    "mds_probe_add": (c_int, [c_int, POINTER(MdsProbeSpec)]),
    "mds_probe_remove": (c_int, [c_int, c_int]),
    "mds_probe_list": (c_int, [c_int, POINTER(c_int), POINTER(MdsProbeSpec), POINTER(c_uint64), c_int]),
    "mds_probe_capacity": (c_int, [c_int, c_int]),
    "mds_probe_take": (c_int, [c_int, POINTER(MdsProbeRecord), POINTER(c_uint8), c_int, c_int]),
    "mds_probe_stats": (c_int, [c_int, POINTER(c_uint64)]),
    "mds_memlog_add": (c_int, [c_int, POINTER(MdsMemlogSpec)]),
    "mds_memlog_remove": (c_int, [c_int, c_int]),
    "mds_memlog_list": (c_int, [c_int, POINTER(c_int), POINTER(MdsMemlogSpec), POINTER(c_uint64), c_int]),
    "mds_memlog_capacity": (c_int, [c_int, c_int]),
    "mds_memlog_take": (c_int, [c_int, POINTER(MdsMemlogRecord), c_int]),
    "mds_memlog_stats": (c_int, [c_int, POINTER(c_uint64)]),
    "mds_action_add": (c_int, [c_int, POINTER(MdsActionSpec)]),
    "mds_action_remove": (c_int, [c_int, c_int]),
    "mds_action_list": (c_int, [c_int, POINTER(c_int), POINTER(MdsActionSpec), POINTER(c_uint64), POINTER(c_uint64),
                                c_int]),
    "mds_action_take": (c_int, [c_int, POINTER(MdsActionRecord), c_int]),
    "mds_action_stats": (c_int, [c_int, POINTER(c_uint64)]),
    "mds_overlay_tags": (c_int, [c_int, c_int]),
    "mds_overlay_at": (c_int, [c_int, c_uint32]),
    "mds_callcounts_take_ov": (c_int, [c_int, c_int, POINTER(c_uint32), POINTER(c_int32), POINTER(c_uint64), c_int]),
    "mds_coverage_take_ov": (c_int, [c_int, c_int, POINTER(c_uint32), POINTER(c_int32), c_int]),
}


class _Lib:
    """The loaded library. Attribute access binds the function with its signature the first time; a function
    the library does not export raises MelonError then, not when the library is loaded."""

    def __init__(self, cdll):
        self._cdll = cdll

    def has_symbol(self, name):
        try:
            getattr(self._cdll, name)
            return True
        except AttributeError:
            return False

    def __getattr__(self, name):
        try:
            fn = getattr(self._cdll, name)
        except AttributeError:
            raise MelonError(f"{name} is not exported by this melonds_headless library (an older build?)") from None
        if name in _SIGNATURES:
            fn.restype, fn.argtypes = _SIGNATURES[name]
        setattr(self, name, fn)
        return fn


def _load():
    path = _find_library()
    if sys.platform == "win32":
        for extra in os.environ.get("MELONDS_DLL_DIRS", r"C:\msys64\ucrt64\bin").split(os.pathsep):
            if extra and os.path.isdir(extra):
                os.add_dll_directory(extra)
        os.add_dll_directory(str(path.parent))
    return _Lib(ctypes.CDLL(str(path)))


_lib = None


def lib():
    global _lib
    if _lib is None:
        _lib = _load()
    return _lib


def _check(ret):
    if ret < 0:
        raise MelonError(lib().mds_error().decode(errors="replace"))
    return ret


def _b(s):
    return s.encode() if s else None


def _cpu(cpu):
    if cpu in (7, 1, "arm7", "ARM7"):
        return 7
    if cpu in (9, 0, "arm9", "ARM9"):
        return 9
    raise MelonError(f"cpu must be 9 or 7, not {cpu!r}")


def _key_mask(keys):
    if isinstance(keys, int):
        return keys & 0xFFF
    mask = 0
    for k in keys:
        mask |= 1 << KEYS.index(k)
    return mask


def _mask(cpu):
    if cpu in ("both", 3):
        return 3
    return 2 if _cpu(cpu) == 7 else 1


def api_version():
    """The library's API level (mds_version): 1 up to the IPC merge, 2 from fix/api-quirks, 3 from feature/cheats,
    4 from merge/upstream-debug, 5 from merge/upstream-debug-2, 6 from fix/api-quirks-2, 7 from feature/probes,
    8 from feature/render-control, 9 from fix/savestate-irq, 10 from feature/api-level-10."""
    return lib().mds_version()


def has(name):
    """Whether the library has a feature (e.g. "serve_host", "dma_break"; docs/headless-api.md, section 1) or
    exports a function ("mds_serve_host"). Libraries older than API level 2 have no feature list: there only
    function names can be answered."""
    if lib().has_symbol("mds_has"):
        return bool(lib().mds_has(name.encode()))
    return name.startswith("mds_") and lib().has_symbol(name)


def _frame(f):
    return {"entry": f.entry, "site": f.site, "ret": f.ret, "sp": f.sp, "mode": f.mode,
            "exception": bool(f.exception)}


def mp_timeout(ms=-1):
    return _check(lib().mds_mp_timeout(ms))


def set_pacing(fps):
    lib().mds_set_pacing(fps)


def serve(port=0, port_range=0, name=None, host=None):
    """Start the socket server thread (flags low 16 bits = port range, 0 = default 32).
    port 0 lets the OS pick. host: the IPv4 address to listen on (None = 127.0.0.1; "0.0.0.0" = every
    interface, so anyone who can reach the port controls the emulator). Returns the bound port."""
    if name:
        lib().mds_serve_name(name.encode())
    bound = c_int(0)
    if host is None:
        _check(lib().mds_serve(port, port_range, ctypes.byref(bound)))
    else:
        _check(lib().mds_serve_host(host.encode(), port, port_range, ctypes.byref(bound)))
    return bound.value


def serve_stop():
    lib().mds_serve_stop()


def serve_wait(timeout_ms=0):
    """1 once a client has sent shutdown, else 0 after timeout_ms."""
    return lib().mds_serve_wait(timeout_ms)


def set_instance_base(base):
    """Process-wide offset for console ids (MAC, save numbering, emu.getconsoleid()). Call before
    creating consoles."""
    _check(lib().mds_set_instance_base(base))


def instance_base():
    return lib().mds_instance_base()


def run_group(consoles, frames, timeout_ms):
    ids = (c_int * len(consoles))(*[c.id for c in consoles])
    states = (c_int * len(consoles))()
    ret = _check(lib().mds_run(ids, len(consoles), frames, timeout_ms, states))
    return ret == 1, list(states)


def wait_group(consoles, timeout_ms):
    ids = (c_int * len(consoles))(*[c.id for c in consoles])
    states = (c_int * len(consoles))()
    ret = _check(lib().mds_wait(ids, len(consoles), timeout_ms, states))
    return ret == 1, list(states)


def _make_input(buttons, lid, touch, mic, mic_level, seq, client):
    if touch and not isinstance(touch[0], (tuple, list)):
        touch = [(0, touch[0], touch[1], 1)]
    pts = (MdsTouchPoint * max(len(touch or []), 1))()
    for i, (offset, x, y, down) in enumerate(touch or []):
        pts[i].offset, pts[i].x, pts[i].y, pts[i].down = offset, x, y, int(down)
    samples = list(mic or [])
    buf = (c_int16 * max(len(samples), 1))(*samples)
    hdr = MdsInput()
    hdr.seq = seq
    hdr.buttons = _key_mask(buttons)
    hdr.lid = int(lid)
    hdr.touch_count = len(touch or [])
    hdr.mic_kind = 2 if mic_level is not None else (1 if mic is not None else 0)
    hdr.flags = INPUT_FLAG_CLIENT if client else 0
    hdr.mic_count = len(samples)
    hdr.mic_level = mic_level or 0
    return hdr, pts, buf


def submit_multi(entries, client=False):
    """input_multi without a socket: entries is a list of (console, kwargs) where kwargs are
    Console.submit_input's arguments (buttons, lid, touch, mic, mic_level, seq). The payloads form a
    barrier group: each console waits at its payload until all of them are there, then they start the
    frame together. All or none are queued. Returns each console's queue depth."""
    n = len(entries)
    made = [_make_input(client=client, **{"buttons": 0, "lid": False, "touch": None, "mic": None,
                                           "mic_level": None, "seq": 0, **kw}) for _, kw in entries]
    ids = (c_int * n)(*[c.id for c, _ in entries])
    hdrs = (MdsInput * n)(*[m[0] for m in made])
    pts = (POINTER(MdsTouchPoint) * n)(*[ctypes.cast(m[1], POINTER(MdsTouchPoint)) for m in made])
    bufs = (POINTER(c_int16) * n)(*[ctypes.cast(m[2], POINTER(c_int16)) for m in made])
    depths = (c_int * n)()
    _check(lib().mds_input_submit_multi(ids, n, hdrs, pts, bufs, depths))
    return list(depths)


def debug_recv_pending(console, pending):
    """Test hook: pretend the console is (True) or is no longer (False) blocked in a wireless receive."""
    _check(lib().mds_debug_recv_pending(console.id, int(pending)))


_live = weakref.WeakSet()


def _shutdown():
    """At interpreter exit: stop the server and destroy the consoles still open, from a normal thread
    context (the library also survives a process that skips this, see Api.cpp)."""
    if _lib is None:
        return
    try:
        _lib.mds_serve_stop()
        for con in list(_live):
            con.close()
    except Exception:
        pass


atexit.register(_shutdown)


class Console:
    def __init__(self, bios9=None, bios7=None, firmware=None, nickname=None, debug=True):
        self.id = _check(lib().mds_create(_b(bios9), _b(bios7), _b(firmware), _b(nickname), int(debug)))
        self.rom = None
        _live.add(self)

    def close(self):
        if self.id is not None:
            lib().mds_destroy(self.id)
            self.id = None

    def load_rom(self, path, save=None):
        _check(lib().mds_load_rom(self.id, _b(str(path)), _b(str(save) if save else None)))
        self.rom = str(path)

    def reset(self):
        _check(lib().mds_reset(self.id))

    def set_rtc(self, when):
        """Set the clock from a datetime (year 2000-2099). The console must be stopped."""
        _check(lib().mds_set_rtc(self.id, when.year, when.month, when.day, when.hour, when.minute, when.second))

    def rtc(self):
        out = (c_int * 6)()
        _check(lib().mds_get_rtc(self.id, out))
        return datetime(*out)

    def run(self, frames, timeout_ms=60000):
        return run_group([self], frames, timeout_ms)

    def wait(self, timeout_ms=60000):
        return wait_group([self], timeout_ms)

    def interrupt(self):
        _check(lib().mds_interrupt(self.id))

    def gdb(self, port9=3333, port7=3334, break_on_start=False, host=None):
        """Start the GDB stubs (both ports <= 0 stops them). host: the IPv4 address they listen on (None =
        127.0.0.1)."""
        if host is None:
            _check(lib().mds_gdb(self.id, port9, port7, int(break_on_start)))
        else:
            _check(lib().mds_gdb_host(self.id, host.encode(), port9, port7, int(break_on_start)))

    def start(self, frames=0xFFFFFFFF):
        _check(lib().mds_start(self.id, frames))

    def stop_after(self, frames=0):
        _check(lib().mds_stop_after(self.id, frames))

    @property
    def state(self):
        return _check(lib().mds_state(self.id))

    @property
    def frames(self):
        n = lib().mds_frames(self.id)
        if n == FRAMES_ERROR and api_version() >= 6:
            raise MelonError(lib().mds_error().decode(errors="replace"))
        return n

    def take_break(self):
        b = MdsBreak()
        if _check(lib().mds_take_break(self.id, ctypes.byref(b))) == 0:
            return None
        kind = BREAK_KINDS[b.kind] if 0 <= b.kind < len(BREAK_KINDS) else str(b.kind)
        return Break(kind, b.cpu, b.pc, b.addr, b.value, b.size, b.id, b.dma)

    def reset_count(self):
        """How many times the console was reset: reset(), load_rom() and GDB restarts (API level 6). A GDB restart
        also queues a Break of kind "reset" (value 1 for the whole console, 0 for one CPU) for take_break()."""
        return _check(lib().mds_reset_count(self.id))

    def screen(self):
        buf = (c_uint32 * (256 * 384))()
        _check(lib().mds_screen(self.id, buf))
        return buf

    # Render control (API level 8; docs/headless-api.md, section 4). Applied at the next frame boundary; neither
    # setting changes emulated state.

    def set_render(self, skip_2d=False, skip_3d=False):
        """Skip 2D drawing (BG/OBJ, the framebuffer) and/or 3D rasterising from the next frame on. Display capture
        still gets what it needs. Draw again for RENDER_FRAMES_3D (2) frames before a screenshot."""
        _check(lib().mds_set_render(self.id, (RENDER_SKIP_2D if skip_2d else 0) | (RENDER_SKIP_3D if skip_3d else 0)))

    def set_render_flags(self, flags):
        """mds_set_render with the raw flags (RENDER_SKIP_2D | RENDER_SKIP_3D)."""
        _check(lib().mds_set_render(self.id, flags))

    def set_threaded_3d(self, on=True):
        """Draw 3D on a thread of its own from the next frame on (same pixels, less wall time)."""
        _check(lib().mds_set_threaded_3d(self.id, int(on)))

    def render_stats(self):
        """{name: value} for RENDER_STAT_NAMES: the settings asked for and in force, and the skip counters."""
        out = (c_uint64 * 32)()
        n = min(_check(lib().mds_render_stats(self.id, out, 32)), 32)
        return {RENDER_STAT_NAMES[i] if i < len(RENDER_STAT_NAMES) else f"stat{i}": out[i] for i in range(n)}

    def set_keys(self, pressed):
        _check(lib().mds_set_keys(self.id, _key_mask(pressed)))

    def touch(self, x=None, y=None):
        _check(lib().mds_touch(self.id, -1 if x is None else x, -1 if y is None else y))

    def set_lid(self, closed):
        _check(lib().mds_set_lid(self.id, int(closed)))

    def submit_input(self, buttons=0, lid=False, touch=None, mic=None, mic_level=None, seq=0, client=False):
        """Queue one frame of input; returns the queue depth. buttons: key names or a bitmask.
        touch: list of (offset, x, y, down) or a single (x, y). mic: list of int16 samples.
        client=True marks the payload as coming from a socket client (subject to the stall timeout)."""
        hdr, pts, buf = _make_input(buttons, lid, touch, mic, mic_level, seq, client)
        return _check(lib().mds_input_submit(self.id, ctypes.byref(hdr), pts, buf))

    def input_mode(self, mode=None, stall_frames=None):
        """mode: "repeat", "neutral" or "wait" (None = unchanged). stall_frames: repeat-mode timeout, 0 = off."""
        m = -1 if mode is None else INPUT_MODES[mode]
        _check(lib().mds_input_mode(self.id, m, -1 if stall_frames is None else stall_frames))

    def input_depth(self):
        return _check(lib().mds_input_depth(self.id))

    def input_waiting(self):
        return bool(_check(lib().mds_input_waiting(self.id)))

    def input_clear(self):
        _check(lib().mds_input_clear(self.id))

    def input_log(self, from_frame=0, max=4096):
        buf = (MdsInputLogEntry * max)()
        n = _check(lib().mds_input_log(self.id, from_frame, buf, max))
        return [{"frame": e.frame, "seq": e.seq, "buttons": e.buttons, "lid": e.lid, "touch_count": e.touch_count,
                 "mic_kind": e.mic_kind, "mic_level": e.mic_level} for e in buf[:n]]

    def read(self, addr, length, cpu=9):
        buf = (c_uint8 * length)()
        _check(lib().mds_read(self.id, _cpu(cpu), addr, length, buf))
        return bytes(buf)

    def write(self, addr, data, cpu=9):
        buf = (c_uint8 * len(data)).from_buffer_copy(data)
        _check(lib().mds_write(self.id, _cpu(cpu), addr, len(data), buf))

    def regs(self, cpu=9):
        out = (c_uint32 * 19)()
        _check(lib().mds_get_regs(self.id, _cpu(cpu), out))
        names = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "cpsr", "spsr", "r15_raw"]
        return dict(zip(names, out))

    def set_reg(self, reg, value, cpu=9):
        names = {f"r{i}": i for i in range(16)} | {"sp": 13, "lr": 14, "pc": 15}
        idx = names[reg] if isinstance(reg, str) else reg
        _check(lib().mds_set_reg(self.id, _cpu(cpu), idx, value))

    def add_breakpoint(self, addr, cpu=9):
        return _check(lib().mds_bp_add(self.id, _mask(cpu), addr))

    def add_watchpoint(self, addr, size, read=False, write=True, cpu=9):
        flags = (WATCH_READ if read else 0) | (WATCH_WRITE if write else 0)
        return _check(lib().mds_wp_add(self.id, _mask(cpu), addr, size, flags))

    def remove_point(self, point):
        _check(lib().mds_point_remove(self.id, point))

    def enable_point(self, point, enabled):
        _check(lib().mds_point_enable(self.id, point, int(enabled)))

    def point_count(self):
        """How many points (breakpoints, watchpoints, Lua memory hooks) the console has (API level 6)."""
        return _check(lib().mds_point_count(self.id))

    def points(self):
        """Every point; "lua" marks a Lua memory hook (API level 6), which remove_point refuses."""
        max_n = max(self.point_count(), 1) if lib().has_symbol("mds_point_count") else 4096
        out = (MdsPoint * max_n)()
        n = _check(lib().mds_point_list(self.id, out, max_n))
        return [{"id": p.id, "kind": "watch" if p.watch else "exec",
                 "cpu": {1: 9, 2: 7, 3: "both"}.get(p.cpumask, p.cpumask),
                 "read": bool(p.flags & 1), "write": bool(p.flags & 2), "lua": bool(p.flags & POINT_LUA),
                 "enabled": bool(p.enabled), "start": p.start, "end": p.end, "hits": p.hits} for p in out[:n]]

    def step(self, count=1, cpu=9):
        _check(lib().mds_step(self.id, _cpu(cpu), count))

    def trace(self, capacity, cpu="both", start=0, end=0xFFFFFFFF):
        _check(lib().mds_trace(self.id, _mask(cpu), start, end, capacity))

    def take_trace(self):
        out, buf = [], (MdsTrace * 8192)()
        while n := _check(lib().mds_trace_take(self.id, buf, 8192)):
            out.extend((t.cycle, t.cpu, t.pc, t.instr, t.cpsr) for t in buf[:n])
        return out

    def track_calls(self, enabled=True):
        _check(lib().mds_calls(self.id, int(enabled)))

    def callstack(self, cpu=9):
        out = (MdsFrame * 4096)()
        n = _check(lib().mds_callstack(self.id, _cpu(cpu), out, 4096))
        return [_frame(f) for f in out[:n]]

    def thread_stacks(self, cpu=9):
        sps, depths, out = (c_uint32 * 64)(), (c_uint32 * 64)(), (MdsFrame * 8192)()
        n = _check(lib().mds_threadstacks(self.id, _cpu(cpu), sps, depths, 64, out, 8192))
        stacks, at = [], 0
        for i in range(n):
            stacks.append({"sp": sps[i], "frames": [_frame(f) for f in out[at:at + depths[i]]]})
            at += depths[i]
        return stacks

    def overlays(self):
        """The cart's ARM9 overlays resident now (API level 5): [{"id", "base", "size"}], in ROM table order."""
        max_n = 256
        while True:
            ids, bases, sizes = (c_int * max_n)(), (c_uint32 * max_n)(), (c_uint32 * max_n)()
            n = _check(lib().mds_overlays(self.id, ids, bases, sizes, max_n))
            if n <= max_n:
                return [{"id": ids[i], "base": bases[i], "size": sizes[i]} for i in range(n)]
            max_n = n

    def take_call_counts(self, cpu=9):
        counts = {}
        entries, hits = (c_uint32 * 8192)(), (c_uint64 * 8192)()
        while n := _check(lib().mds_callcounts_take(self.id, _cpu(cpu), entries, hits, 8192)):
            for i in range(n):
                counts[entries[i]] = counts.get(entries[i], 0) + hits[i]
        return counts

    def coverage(self, enabled=True):
        _check(lib().mds_coverage(self.id, int(enabled)))

    def take_coverage(self, cpu=9):
        out, buf = [], (c_uint32 * 65536)()
        while n := _check(lib().mds_coverage_take(self.id, _cpu(cpu), buf, 65536)):
            out.extend(buf[:n])
        return out

    def track_writers(self, enabled=True):
        _check(lib().mds_lastwriter(self.id, int(enabled)))

    def last_writer(self, addr):
        """None if nothing wrote the word since tracking started; (pc, cpu) for an instruction; ("dma", channel,
        cpu) for a DMA write. Raises MelonError for errors (tracking off, running, no debugger). With a library
        older than API level 2 errors cannot be told apart and also give None."""
        if not lib().has_symbol("mds_lastwriter_query"):
            w = lib().mds_lastwriter_get(self.id, addr)
        else:
            raw, dma = c_uint32(0), c_int(-1)
            if _check(lib().mds_lastwriter_query(self.id, addr, ctypes.byref(raw), ctypes.byref(dma))) == 0:
                return None
            w = raw.value
        if w == 0:
            return None
        if w >= DMA_TAG:
            return ("dma", (w >> 1) & 3, 7 if w & 1 else 9)
        return (w & ~1, 7 if w & 1 else 9)

    # Action Replay codes (API level 3, feature "cheats"; docs/headless-api.md, section 6a). The core's engine runs
    # the enabled ones every frame. A state load or ROM load clears the list: re-add codes after loading.

    def add_cheat(self, code, name="", enabled=True):
        """Add an Action Replay code: text of "XXXXXXXX YYYYYYYY" lines, or a list of such lines. Returns its id.
        A malformed code raises MelonError ("cheat: line N: ...") and adds nothing."""
        if not isinstance(code, str):
            code = "\n".join(code)
        return _check(lib().mds_cheat_add(self.id, (name or "").encode(), code.encode(), int(enabled)))

    def remove_cheat(self, cheat):
        _check(lib().mds_cheat_remove(self.id, cheat))

    def enable_cheat(self, cheat, enabled=True):
        _check(lib().mds_cheat_enable(self.id, cheat, int(enabled)))

    def clear_cheats(self):
        """Remove every code; returns how many there were."""
        return _check(lib().mds_cheat_clear(self.id))

    def cheat(self, cheat):
        """{"id", "name", "enabled", "words", "code"} of one code; "code" is its text, one line per word pair."""
        for c in self.cheats():
            if c["id"] == cheat:
                return c
        raise MelonError(f"no cheat {cheat}")

    def cheats(self):
        """Every code in list order, as cheat() describes it."""
        entries = (MdsCheat * 1024)()
        out = []
        for e in entries[:_check(lib().mds_cheat_list(self.id, entries, 1024))]:
            name, words = ctypes.create_string_buffer(4097), (c_uint32 * max(e.words, 1))()
            n = _check(lib().mds_cheat_get(self.id, e.cheat, name, 4097, words, e.words))
            w = list(words[:n])
            out.append({"id": e.cheat, "name": name.value.decode(errors="replace"), "enabled": bool(e.enabled),
                        "words": n, "code": "\n".join(f"{a:08X} {b:08X}" for a, b in zip(w[0::2], w[1::2]))})
        return out

    def save_state(self, path):
        _check(lib().mds_savestate(self.id, _b(str(path))))

    def load_state(self, path):
        _check(lib().mds_loadstate(self.id, _b(str(path))))

    def lua_run(self, source, is_file=False):
        _check(lib().mds_lua_run(self.id, _b(str(source)), int(is_file)))

    def lua_exec(self, code):
        _check(lib().mds_lua_exec(self.id, _b(code)))
        return self.lua_output()

    def lua_stop(self):
        _check(lib().mds_lua_stop(self.id))

    def mac(self):
        buf = (c_uint8 * 6)()
        _check(lib().mds_mac(self.id, buf))
        return bytes(buf)

    def save_path(self):
        buf = ctypes.create_string_buffer(4096)
        _check(lib().mds_save_path(self.id, buf, 4096))
        return buf.value.decode()

    def lua_output(self):
        chunks, buf = [], (c_char * 65536)()
        while n := _check(lib().mds_lua_output(self.id, buf, 65536)):
            chunks.append(buf.raw[:n])
        return b"".join(chunks).decode(errors="replace")

    def packet_log(self, enabled=True, capacity=4096):
        _check(lib().mds_packets(self.id, int(enabled), capacity))

    def take_packets(self):
        out = []
        hdrs, data = (MdsPacket * 256)(), (c_uint8 * (256 * 2048))()
        while n := _check(lib().mds_packets_take(self.id, hdrs, data, 256, 256 * 2048)):
            off = 0
            for h in hdrs[:n]:
                p = {"timestamp": h.timestamp, "sent": bool(h.sent), "type": h.type,
                     "data": bytes(data[off:off + h.length])}
                if h.total > h.length:      # longer than the buffer (level 6): the rest was dropped
                    p["size"] = h.total
                out.append(p)
                off += h.length
        return out

    # Probes, the memory access log, breakpoint actions and overlay tags (API level 7; docs/headless-api.md,
    # section 5b). Nothing here parks the console; probes and the log change nothing in the run.

    def add_probe(self, addr, cpu=9, reads=(), lr_range=None, returns=True):
        """Log every call of the function at addr. reads: up to 4 (reg, length[, offset[, when]]) memory reads
        behind registers ("r1" or 1), when = "entry" (default), "return" or "both". lr_range: (lo, hi) to log only
        calls whose return address is in it. returns=False: do not wait for returns. Returns the probe id."""
        spec = MdsProbeSpec(cpumask=_mask(cpu), addr=addr, flags=0 if returns else PROBE_NO_RETURN)
        if lr_range:
            spec.lr_start, spec.lr_end = lr_range
        if len(reads) > 4:
            raise MelonError("at most 4 reads per probe")
        for i in range(4):
            spec.reads[i].reg = -1
        for i, r in enumerate(reads):
            reg, length, offset, when = (tuple(r) + (0, "entry")[len(r) - 2:])[:4]
            w = {"entry": PROBE_READ_ENTRY, "return": PROBE_READ_RETURN, "both": 3}.get(when, when)
            spec.reads[i] = MdsProbeRead(_REGS.get(reg, reg), offset, length, w)
        return _check(lib().mds_probe_add(self.id, ctypes.byref(spec)))

    def remove_probe(self, probe):
        _check(lib().mds_probe_remove(self.id, probe))

    def probes(self):
        """[{"id", "addr", "cpu", "hits", "lr_range", "returns"}]."""
        n = _check(lib().mds_probe_list(self.id, None, None, None, 0))
        m = max(n, 1)
        ids, specs, hits = (c_int * m)(), (MdsProbeSpec * m)(), (c_uint64 * m)()
        n = min(n, _check(lib().mds_probe_list(self.id, ids, specs, hits, n)))
        return [{"id": ids[i], "addr": specs[i].addr, "cpu": {1: 9, 2: 7, 3: "both"}.get(specs[i].cpumask),
                 "hits": hits[i], "lr_range": (specs[i].lr_start, specs[i].lr_end) if specs[i].lr_end else None,
                 "returns": not specs[i].flags & PROBE_NO_RETURN} for i in range(n)]

    def probe_capacity(self, records):
        """Size the ring of probe records (default 65536; the oldest are dropped when full). Empties it."""
        _check(lib().mds_probe_capacity(self.id, records))

    def take_probes(self, pending=None):
        """Every probe record, oldest first: dicts "seq", "probe", "cpu", "addr", "frame", "cycle", "lr", "sp",
        "depth", "overlay", "args" (r0-r3), "returned", "ret" ([r0, r1] or None), "ret_frame", "forced", "lost",
        "reads" (bytes or None per read, at entry) and "ret_reads" (at return). A call taken before it returned has
        "returned" False and its return comes later as a late record: pass the same dict as `pending` to every
        take and the late return is merged into the call's dict (taken earlier); without it, or when the call's
        dict is gone, the late record is returned with "late": True."""
        out = []
        recs, data = (MdsProbeRecord * 4096)(), (c_uint8 * (4096 * 512))()
        while n := _check(lib().mds_probe_take(self.id, recs, data, 4096, len(data))):
            off = 0
            for r in recs[:n]:
                blob = bytes(data[off:off + r.data_len])
                off += r.data_len
                at, reads, ret_reads = 0, [], []
                for lens, dest in ((r.entry_len, reads), (r.ret_len, ret_reads)):
                    for k in range(4):
                        dest.append(blob[at:at + lens[k]] if lens[k] else None)
                        at += lens[k]
                rec = {"seq": r.seq, "probe": r.probe, "cpu": r.cpu, "addr": r.addr, "frame": r.frame,
                       "cycle": r.cycle, "lr": r.lr, "sp": r.sp, "depth": r.depth, "overlay": r.overlay,
                       "args": list(r.args), "returned": bool(r.returned),
                       "ret": list(r.ret) if r.returned else None, "ret_frame": r.ret_frame if r.returned else None,
                       "forced": bool(r.flags & PROBE_FORCED), "lost": bool(r.flags & PROBE_RETURN_LOST),
                       "reads": reads, "ret_reads": ret_reads}
                if r.kind == 1:
                    earlier = pending.pop(r.seq, None) if pending is not None else None
                    if earlier is not None:
                        earlier.update(returned=True, ret=rec["ret"], ret_frame=rec["ret_frame"], ret_reads=ret_reads)
                        continue
                    rec["late"] = True
                elif pending is not None and not r.returned and not r.flags & PROBE_RETURN_LOST:
                    pending[r.seq] = rec
                out.append(rec)
        return out

    def probe_stats(self):
        """{"recorded", "dropped", "pending" (calls awaiting their return), "in_ring"}."""
        return self._stats("mds_probe_stats", ("recorded", "dropped", "pending", "in_ring"))

    def _stats(self, fn, names):
        out = (c_uint64 * 4)()
        _check(getattr(lib(), fn)(self.id, out))
        return dict(zip(names, out))

    def memlog_add(self, start, size, read=True, write=True, dma=True, cpu=9, pc_range=None):
        """Log every access to [start, start + size) by the CPU (and its DMA units with dma=True) without parking.
        pc_range: (lo, hi) to keep only accesses by instructions there. Returns the range id."""
        flags = (MEMLOG_READ if read else 0) | (MEMLOG_WRITE if write else 0) | (MEMLOG_DMA if dma else 0)
        spec = MdsMemlogSpec(cpumask=_mask(cpu), start=start, size=size, flags=flags)
        if pc_range:
            spec.pc_start, spec.pc_end = pc_range
        return _check(lib().mds_memlog_add(self.id, ctypes.byref(spec)))

    def memlog_remove(self, rng):
        _check(lib().mds_memlog_remove(self.id, rng))

    def memlog_ranges(self):
        n = _check(lib().mds_memlog_list(self.id, None, None, None, 0))
        m = max(n, 1)
        ids, specs, hits = (c_int * m)(), (MdsMemlogSpec * m)(), (c_uint64 * m)()
        n = min(n, _check(lib().mds_memlog_list(self.id, ids, specs, hits, n)))
        return [{"id": ids[i], "start": specs[i].start, "size": specs[i].size, "flags": specs[i].flags,
                 "hits": hits[i]} for i in range(n)]

    def memlog_capacity(self, records):
        """Size the access log ring (default 1 << 20; the oldest are dropped when full). Empties it."""
        _check(lib().mds_memlog_capacity(self.id, records))

    def take_memlog(self):
        """Every logged access, oldest first: dicts "seq", "frame", "cycle", "cpu", "pc", "addr", "size", "value",
        "write" (else a read), "dma" (the channel, or None for an instruction; for DMA "pc" is the DMA tag),
        "range"."""
        out, buf = [], (MdsMemlogRecord * 16384)()
        while n := _check(lib().mds_memlog_take(self.id, buf, 16384)):
            out.extend({"seq": r.seq, "frame": r.frame, "cycle": r.cycle, "cpu": r.cpu, "pc": r.pc, "addr": r.addr,
                        "size": r.size, "value": r.value, "write": bool(r.flags & MEMLOG_WRITE),
                        "dma": r.dma if r.flags & MEMLOG_DMA else None, "range": r.range} for r in buf[:n])
        return out

    def memlog_stats(self):
        """{"recorded", "dropped", "in_ring"}."""
        s = self._stats("mds_memlog_stats", ("recorded", "dropped", "pending", "in_ring"))
        s.pop("pending")
        return s

    def add_action(self, addr, kind, value=0, cpu=9, reg=-1, value2=None, target=0, size=0, lr=None, first=1,
                   times=0):
        """A breakpoint action at addr, applied without parking: kind "return" (r0 = value, r1 = value2 if given,
        PC = LR), "set_reg" (register reg = value), "write" (size bytes of value at target) or "skip" (size
        instructions). lr: only hits from that return address; first: from the first-th matching hit; times: at
        most that many (0 = no limit). Every application is logged (take_actions). Returns the action id."""
        spec = MdsActionSpec(cpumask=_mask(cpu), addr=addr, kind=ACTION_KINDS.get(kind, kind), reg=_REGS.get(reg, reg),
                             value=value & 0xFFFFFFFF, value2=(value2 or 0) & 0xFFFFFFFF, target=target, size=size,
                             lr=(lr or 0) & ~1, first=first, times=times,
                             flags=ACTION_SET_R1 if value2 is not None else 0)
        return _check(lib().mds_action_add(self.id, ctypes.byref(spec)))

    def remove_action(self, action):
        _check(lib().mds_action_remove(self.id, action))

    def actions(self):
        """[{"id", "addr", "kind", "value", "lr", "first", "times", "matched", "fired"}]."""
        n = _check(lib().mds_action_list(self.id, None, None, None, None, 0))
        m = max(n, 1)
        ids, specs, matched, fired = (c_int * m)(), (MdsActionSpec * m)(), (c_uint64 * m)(), (c_uint64 * m)()
        n = min(n, _check(lib().mds_action_list(self.id, ids, specs, matched, fired, n)))
        kinds = {v: k for k, v in ACTION_KINDS.items()}
        return [{"id": ids[i], "addr": specs[i].addr, "kind": kinds.get(specs[i].kind, specs[i].kind),
                 "value": specs[i].value, "lr": specs[i].lr or None, "first": specs[i].first, "times": specs[i].times,
                 "matched": matched[i], "fired": fired[i]} for i in range(n)]

    def take_actions(self):
        """The log of applied actions, oldest first: dicts "seq", "action", "kind", "cpu", "pc", "lr", "hit",
        "frame", "cycle", "regs" (r0-r3 before), "value", "target"."""
        out, buf = [], (MdsActionRecord * 4096)()
        kinds = {v: k for k, v in ACTION_KINDS.items()}
        while n := _check(lib().mds_action_take(self.id, buf, 4096)):
            out.extend({"seq": r.seq, "action": r.action, "kind": kinds.get(r.kind, r.kind), "cpu": r.cpu,
                        "pc": r.pc, "lr": r.lr, "hit": r.hit, "frame": r.frame, "cycle": r.cycle,
                        "regs": list(r.regs), "value": r.value, "target": r.target} for r in buf[:n])
        return out

    def action_stats(self):
        """{"applied" (since the console was created), "dropped", "in_ring"}."""
        s = self._stats("mds_action_stats", ("applied", "dropped", "pending", "in_ring"))
        s.pop("pending")
        return s

    def overlay_tags(self, enabled=True):
        """Also keep call counts and coverage per (address, overlay): take_call_counts_ov / take_coverage_ov."""
        _check(lib().mds_overlay_tags(self.id, int(enabled)))

    def overlay_at(self, addr):
        """The cart overlay whose code is at addr now: its id, or OVERLAY_STATIC (-1), OVERLAY_AMBIGUOUS (-2),
        OVERLAY_UNKNOWN (-3)."""
        r = lib().mds_overlay_at(self.id, addr)
        if r == OVERLAY_AT_ERROR:
            raise MelonError(lib().mds_error().decode(errors="replace"))
        return r

    def take_call_counts_ov(self, cpu=9):
        """{(entry, overlay): count} since overlay_tags(True) (or the last take)."""
        counts = {}
        e, o, h = (c_uint32 * 8192)(), (c_int32 * 8192)(), (c_uint64 * 8192)()
        while n := _check(lib().mds_callcounts_take_ov(self.id, _cpu(cpu), e, o, h, 8192)):
            for i in range(n):
                counts[(e[i], o[i])] = counts.get((e[i], o[i]), 0) + h[i]
        return counts

    def take_coverage_ov(self, cpu=9):
        """[(addr, overlay)] executed since overlay_tags(True), with coverage on; sorted."""
        out, a, o = [], (c_uint32 * 65536)(), (c_int32 * 65536)()
        while n := _check(lib().mds_coverage_take_ov(self.id, _cpu(cpu), a, o, 65536)):
            out.extend(zip(a[:n], o[:n]))
        return out
