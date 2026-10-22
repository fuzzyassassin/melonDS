"""The fix/api-quirks fixes: debug-off errors, count checks, idle ROM loads, Lua on the emulator thread, exact
steps, DMA watchpoint breaks, last-writer queries, feature detection, bind addresses, and what savestates
restore (held input, debugger data)."""
import ctypes
import os
import socket
import tempfile
import threading
import unittest
from ctypes import c_char, c_int, c_uint8, c_uint32, c_uint64

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE7, CODE9, DATA, ConsoleTest
from .test_input import BUF7, Akey, Bkey, Recorder, released

DMA0 = 0x040000B0


def dma_program(src, dst, units=4):
    """ARM9: start DMA0 (immediate, 32-bit, `units` words) from src to dst, then spin at "done"."""
    a = A.Asm(CODE9)
    a.ldr_const(0, DMA0)
    a.ldr_const(1, src)
    a.str(1, 0, 0)
    a.ldr_const(1, dst)
    a.str(1, 0, 4)
    a.ldr_const(1, 0x84000000 | units)
    a.str(1, 0, 8)
    a.label("done")
    a.b("done")
    return a


def adds(n, base=CODE9):
    """n x `add r1, r1, #1`, then a spin: r1 counts the instructions executed."""
    a = A.Asm(base)
    for _ in range(n):
        a.add(1, 1, 1)
    a.label("done")
    a.b("done")
    return a


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def lan_address():
    """A local IPv4 address that is not loopback, or None."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))         # no packet is sent
        ip = s.getsockname()[0]
        s.close()
        return None if ip.startswith("127.") or ip == "0.0.0.0" else ip
    except OSError:
        return None


def can_connect(host, port):
    s = socket.socket()
    s.settimeout(2)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


class DebugOff(unittest.TestCase):
    """A console created with debug = 0 has no debugger hooks: every debugger call fails instead of silently
    doing nothing."""

    def setUp(self):
        self.c = native.Console(debug=False)
        self.c.reset()
        self.addCleanup(self.c.close)

    def test_debugger_calls_fail_with_a_clear_message(self):
        c = self.c
        for name, fn in [
            ("breakpoint", lambda: c.add_breakpoint(CODE9)),
            ("watchpoint", lambda: c.add_watchpoint(DATA, 4)),
            ("remove", lambda: c.remove_point(1)),
            ("enable", lambda: c.enable_point(1, False)),
            ("points", c.points),
            ("step", c.step),
            ("trace", lambda: c.trace(16)),
            ("take_trace", c.take_trace),
            ("calls", c.track_calls),
            ("callstack", c.callstack),
            ("call counts", c.take_call_counts),
            ("coverage", c.coverage),
            ("take coverage", c.take_coverage),
            ("writers", c.track_writers),
            ("last writer", lambda: c.last_writer(DATA)),
            ("interrupt", c.interrupt),
            ("gdb", lambda: c.gdb(free_port(), free_port())),
        ]:
            with self.subTest(name):
                with self.assertRaises(MelonError) as cm:
                    fn()
                self.assertIn(f"console {c.id} was created without the debugger", str(cm.exception))

    def test_lua_debugger_functions_fail(self):
        for code in ("event.onmemorywrite(function() end, 0x02200000)",
                     "event.onmemoryexecute(function() end, 0x02100000)",
                     "debugger.trackcalls(true)", "debugger.coverage(true)", "debugger.trace(16)",
                     "debugger.trackwriters(true)", "debugger.lastwriter(0x02200000)", "debugger.pause()",
                     "debugger.step(1)", "debugger.callstack()", "debugger.threadstacks()",
                     "memory.execute(0xE12FFF1E)"):
            with self.subTest(code):
                with self.assertRaises(MelonError) as cm:
                    self.c.lua_exec(code)
                self.assertIn("without the debugger", str(cm.exception))

    def test_everything_else_still_works(self):
        c = self.c
        c.write(DATA, b"\x01\x02\x03\x04")
        self.assertEqual(c.read(DATA, 4), b"\x01\x02\x03\x04")
        self.assertEqual(c.run(2), (False, [native.IDLE]))
        c.packet_log(True)
        self.assertEqual(c.take_packets(), [])
        self.assertEqual(c.lua_exec("print(emu.framecount())"), "2\n")
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "s.mln")
            c.save_state(path)
            c.load_state(path)

    def test_debug_console_accepts_the_same_calls(self):
        d = native.Console()
        d.reset()
        self.addCleanup(d.close)
        d.add_breakpoint(CODE9)
        d.track_calls(True)
        d.coverage(True)
        d.track_writers(True)
        self.assertIsNone(d.last_writer(DATA))


class NegativeCounts(ConsoleTest):
    """Every take/list function refuses a negative max (it used to throw through the C boundary or copy a
    negative length)."""

    def test_negative_max_fails(self):
        lib, i = native.lib(), self.c.id
        buf = (c_uint8 * 64)()
        u32 = (c_uint32 * 4)()
        u64 = (c_uint64 * 4)()
        calls = {
            "mds_trace_take": lambda: lib.mds_trace_take(i, ctypes.cast(buf, ctypes.POINTER(native.MdsTrace)), -1),
            "mds_callcounts_take": lambda: lib.mds_callcounts_take(i, 9, u32, u64, -1),
            "mds_coverage_take": lambda: lib.mds_coverage_take(i, 9, u32, -1),
            "mds_lua_output": lambda: lib.mds_lua_output(i, ctypes.cast(buf, ctypes.POINTER(c_char)), -1),
            "mds_callstack": lambda: lib.mds_callstack(i, 9, ctypes.cast(buf, ctypes.POINTER(native.MdsFrame)), -1),
            "mds_point_list": lambda: lib.mds_point_list(i, ctypes.cast(buf, ctypes.POINTER(native.MdsPoint)), -1),
            "mds_packets_take(max_packets)": lambda: lib.mds_packets_take(
                i, ctypes.cast(buf, ctypes.POINTER(native.MdsPacket)), buf, -1, 64),
            "mds_packets_take(max_data)": lambda: lib.mds_packets_take(
                i, ctypes.cast(buf, ctypes.POINTER(native.MdsPacket)), buf, 1, -5),
            "mds_input_log": lambda: lib.mds_input_log(
                i, 0, ctypes.cast(buf, ctypes.POINTER(native.MdsInputLogEntry)), -1),
            "mds_run": lambda: lib.mds_run((c_int * 1)(i), -1, 1, 1000, None),
            "mds_wait": lambda: lib.mds_wait((c_int * 1)(i), -1, 1000, None),
        }
        for name, fn in calls.items():
            with self.subTest(name):
                self.assertEqual(fn(), -1)
                self.assertIn("must not be negative", lib.mds_error().decode())
        # the console is unharmed
        self.run_frames(1)

    def test_zero_max_is_fine(self):
        lib, i = native.lib(), self.c.id
        buf = (c_uint8 * 64)()
        self.assertEqual(lib.mds_coverage_take(i, 9, ctypes.cast(buf, ctypes.POINTER(c_uint32)), 0), 0)
        self.assertEqual(lib.mds_lua_output(i, ctypes.cast(buf, ctypes.POINTER(c_char)), 0), 0)


class LoadRomNeedsIdle(ConsoleTest):
    def test_parked_console_refuses_a_rom_load(self):
        self.load_program(adds(8))
        self.c.add_breakpoint(CODE9 + 8)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        lib = native.lib()
        self.assertEqual(lib.mds_load_rom(self.c.id, b"missing.nds", None), -1)
        self.assertIn("needs an idle console", lib.mds_error().decode())
        self.assertEqual(self.c.state, native.PARKED)

    def test_idle_console_gets_as_far_as_reading_the_rom(self):
        with self.assertRaises(MelonError) as cm:
            self.c.load_rom("definitely-missing.nds")
        self.assertIn("cannot read ROM", str(cm.exception))


class LuaOnTheEmulatorThread(ConsoleTest):
    def test_error_in_the_first_step_fails_the_call(self):
        with self.assertRaises(MelonError) as cm:
            self.c.lua_run("local x = 1\nerror('boom at start')")
        self.assertIn("boom at start", str(cm.exception))
        self.assertEqual(self.c.lua_output(), "")      # reported once, as the error

    def test_error_after_a_yield_goes_to_the_output(self):
        self.c.lua_run("emu.frameadvance()\nerror('boom later')")
        self.run_frames(2)
        self.assertIn("boom later", self.c.lua_output())

    def test_odd_error_objects_still_fail(self):
        for code in ("error('')", "error({})", "error(nil)"):
            with self.subTest(code):
                with self.assertRaises(MelonError):
                    self.c.lua_run(code)
                with self.assertRaises(MelonError):
                    self.c.lua_exec(code)

    def test_syntax_error_still_fails(self):
        with self.assertRaises(MelonError):
            self.c.lua_run("this is not lua")

    def test_lua_is_serialised_with_other_threads(self):
        # Lua and memory calls from several threads at once all run on the emulator thread, one at a time.
        self.c.write(DATA, bytes(4))
        errors = []

        def bump():
            try:
                for _ in range(50):
                    self.c.lua_exec("memory.write_u32_le(0x%x, memory.read_u32_le(0x%x) + 1)" % (DATA, DATA))
            except Exception as e:     # pragma: no cover - reported below
                errors.append(e)

        threads = [threading.Thread(target=bump) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        self.assertEqual(self.word(DATA), 200)      # no read-modify-write was lost

    def test_stop_and_run_from_another_thread(self):
        out = []
        t = threading.Thread(target=lambda: out.append(self.c.lua_exec("print('hi')")))
        t.start()
        t.join()
        self.assertEqual(out, ["hi\n"])
        self.c.lua_stop()


class StepExact(ConsoleTest):
    """mds_step(n) executes exactly n instructions of that CPU, from idle and from parked."""

    def setUp(self):
        super().setUp()
        self.load_program(adds(64))
        self.c.set_reg("r1", 0)

    def step(self, n):
        self.c.step(n)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual(b.kind, "step")
        return self.c.regs()["r1"], b.pc

    def test_step_one_from_idle_executes_one(self):
        self.assertEqual(self.step(1), (1, CODE9 + 4))

    def test_step_from_idle_and_from_parked_agree(self):
        self.assertEqual(self.step(5), (5, CODE9 + 20))
        self.assertEqual(self.step(3), (8, CODE9 + 32))
        self.assertEqual(self.step(1), (9, CODE9 + 36))

    def test_step_after_a_breakpoint(self):
        bp = self.c.add_breakpoint(CODE9 + 12)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        self.assertEqual(self.c.take_break().kind, "exec")
        self.assertEqual(self.c.regs()["r1"], 3)
        self.c.remove_point(bp)
        self.assertEqual(self.step(2), (5, CODE9 + 20))

    def test_lua_step_matches(self):
        self.c.lua_exec("debugger.step(4)")
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        self.assertEqual(self.c.regs()["r1"], 4)

    def test_zero_disarms(self):
        self.c.step(3)
        self.c.step(0)
        self.run_frames(1)
        self.assertEqual(self.c.regs()["r1"], 64)


class DmaWatchpoint(ConsoleTest):
    SRC, DST = DATA + 0x500, DATA + 0x600

    def setUp(self):
        super().setUp()
        self.c.write(self.SRC, bytes(range(1, 17)))
        self.c.write(self.DST, bytes(16))

    def test_dma_write_breaks_at_once_and_reports_the_channel(self):
        wp = self.c.add_watchpoint(self.DST, 16, write=True)
        self.load_program(dma_program(self.SRC, self.DST))
        _, states = self.c.run(3)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.cpu, b.dma, b.id, b.size), ("write", 9, 0, wp, 4))
        self.assertEqual(b.pc, native.DMA_TAG)                 # channel 0 of the ARM9: no instruction
        self.assertEqual((b.addr, b.value), (self.DST, 0x04030201))
        # parked between two DMA units: the first word is written, the next one not yet
        self.assertEqual(self.c.read(self.DST, 8), bytes([1, 2, 3, 4, 0, 0, 0, 0]))
        self.c.remove_point(wp)
        self.run_frames(1)
        self.assertEqual(self.c.read(self.DST, 16), bytes(range(1, 17)))

    def test_one_break_per_transfer(self):
        # the first watched unit parks; the other units of that transfer only count hits. A new transfer of the
        # channel breaks again.
        wp = self.c.add_watchpoint(self.DST, 16, write=True)
        for _ in range(2):
            self.load_program(dma_program(self.SRC, self.DST))
            _, states = self.c.run(3)
            self.assertEqual(states, [native.PARKED])
            b = self.c.take_break()
            self.assertEqual((b.addr, b.dma), (self.DST, 0))
            self.run_frames(1)
            self.assertEqual(self.c.read(self.DST, 16), bytes(range(1, 17)))
        hits = next(p["hits"] for p in self.c.points() if p["id"] == wp)
        self.assertEqual(hits, 8)

    def test_last_writer_names_the_channel(self):
        self.c.track_writers(True)
        self.load_program(dma_program(self.SRC, self.DST))
        self.run_frames(2)
        self.assertEqual(self.c.last_writer(self.DST + 12), ("dma", 0, 9))
        self.assertEqual(self.c.lua_exec("print(debugger.lastwriter(0x%x))" % self.DST), "nil\t9\t0\n")


class LastWriterQuery(ConsoleTest):
    def test_errors_are_not_no_record(self):
        lib = native.lib()
        raw, dma = c_uint32(7), c_int(7)
        with self.assertRaises(MelonError) as cm:
            self.c.last_writer(DATA)                           # tracking is off
        self.assertIn("tracking is off", str(cm.exception))
        self.assertEqual(lib.mds_lastwriter_query(99, DATA, ctypes.byref(raw), ctypes.byref(dma)), -1)
        self.assertEqual(lib.mds_lastwriter_get(99, DATA), 0)  # legacy: 0 for errors too

    def test_cpu_writer_and_no_record(self):
        a = A.Asm(CODE9)
        a.ldr_const(0, DATA + 0x40)
        a.mov(1, 5)
        store = a.pc
        a.str(1, 0)
        a.label("done")
        a.b("done")
        self.c.track_writers(True)
        self.load_program(a)
        self.run_frames(1)
        self.assertEqual(self.c.last_writer(DATA + 0x40), (store, 9))
        self.assertIsNone(self.c.last_writer(DATA + 0x80))
        lib = native.lib()
        raw, dma = c_uint32(0), c_int(0)
        self.assertEqual(lib.mds_lastwriter_query(self.c.id, DATA + 0x40, ctypes.byref(raw), ctypes.byref(dma)), 1)
        self.assertEqual((raw.value, dma.value), (store, -1))
        self.assertEqual(lib.mds_lastwriter_query(self.c.id, DATA + 0x80, None, None), 0)
        self.assertEqual(self.c.lua_exec("print(debugger.lastwriter(0x%x))" % (DATA + 0x40)), f"{store}\t9\n")


class Features(unittest.TestCase):
    def test_version_and_features(self):
        self.assertGreaterEqual(native.api_version(), 2)
        for name in ("api_version", "debug_off_errors", "savestate_input", "mic_noise_emulated", "serve_host",
                     "gdb_host", "count_checks", "dma_break", "load_rom_idle", "lua_queue", "step_exact",
                     "lastwriter_query", "mds_serve_host", "mds_gdb_host", "mds_lastwriter_query", "mds_read"):
            with self.subTest(name):
                self.assertTrue(native.has(name))
        self.assertFalse(native.has("no_such_feature"))
        self.assertFalse(native.has("mds_no_such_function"))

    def test_missing_function_fails_only_when_called(self):
        with self.assertRaises(MelonError) as cm:
            native.lib().mds_no_such_function(1)
        self.assertIn("not exported", str(cm.exception))


class BindAddresses(unittest.TestCase):
    def test_server_bad_host(self):
        with self.assertRaises(MelonError) as cm:
            native.serve(0, host="not-an-address")
        self.assertIn("bad host", str(cm.exception))

    def test_server_defaults_to_loopback_and_listens_on_request(self):
        lan = lan_address()
        port = native.serve(0)
        try:
            self.assertTrue(can_connect("127.0.0.1", port))
            if lan:
                self.assertFalse(can_connect(lan, port))
        finally:
            native.serve_stop()
        port = native.serve(0, host="0.0.0.0")
        try:
            self.assertTrue(can_connect("127.0.0.1", port))
            if lan:
                self.assertTrue(can_connect(lan, port))
        finally:
            native.serve_stop()

    def test_gdb_defaults_to_loopback_and_listens_on_request(self):
        c = native.Console()
        c.reset()
        self.addCleanup(c.close)
        lan = lan_address()
        p9, p7 = free_port(), free_port()
        c.gdb(p9, p7)
        try:
            self.assertTrue(can_connect("127.0.0.1", p9))
            if lan:
                self.assertFalse(can_connect(lan, p9))
        finally:
            c.gdb(0, 0)
        p9, p7 = free_port(), free_port()
        c.gdb(p9, p7, host="0.0.0.0")
        try:
            self.assertTrue(can_connect("127.0.0.1", p7))
            if lan:
                self.assertTrue(can_connect(lan, p7))
        finally:
            c.gdb(0, 0)
        with self.assertRaises(MelonError):
            c.gdb(p9, p7, host="999.1.1.1")


class SavestateInput(Recorder, ConsoleTest):
    """Savestates 14.2 carry the input: after a load the empty-queue rule repeats the loaded state's keys and
    pen, not what was held before the load."""

    def setUp(self):
        super().setUp()
        self.install(self.c)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "s.mln")

    def test_load_repeats_the_states_keys(self):
        self.c.set_keys(["A"])
        self.run_frames(3)
        self.c.save_state(self.path)
        saved = len(self.samples())
        self.c.set_keys(["B"])
        self.run_frames(3)
        self.c.load_state(self.path)
        self.run_frames(3)                                  # empty queue: repeat
        after = self.samples()[saved:]
        self.assertEqual(after, [released(Akey)] * 3)

    def test_load_restores_the_pen(self):
        self.c.touch(100, 50)
        self.run_frames(2)
        self.c.save_state(self.path)
        saved = len(self.samples(buf=BUF7))
        self.c.touch(None)
        self.run_frames(2)
        self.assertEqual(self.samples(buf=BUF7)[-1] & 0x40, 0x40)    # pen up
        self.c.load_state(self.path)
        self.run_frames(2)
        after = self.samples(buf=BUF7)[saved:]
        self.assertTrue(after and all(v & 0x40 == 0 for v in after), after)     # pen down again

    def test_explicit_input_after_a_load_still_wins(self):
        self.c.set_keys(["A"])
        self.run_frames(2)
        self.c.save_state(self.path)
        saved = len(self.samples())
        self.c.load_state(self.path)
        self.c.submit_input(["B"])
        self.run_frames(2)
        self.assertEqual(self.samples()[saved:], [released(Bkey)] * 2)



class SavestateDebuggerData(ConsoleTest):
    """A load clears what described the execution before it; points stay."""

    def setUp(self):
        super().setUp()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "s.mln")

    def test_call_stack_and_last_writer_do_not_survive_a_load(self):
        self.load_program(adds(2))                          # no calls, no stores
        self.c.track_calls(True)
        self.c.track_writers(True)
        self.run_frames(1)
        self.c.save_state(self.path)

        a = A.Asm(CODE9)
        a.ldr_const(0, DATA + 0x40)
        a.str(0, 0)
        a.bl("f")
        a.label("back")
        a.b("back")
        a.label("f")
        a.b("f")
        bp = self.c.add_breakpoint(CODE9 + 0x400)           # a point, to see it kept
        self.load_program(a)
        self.run_frames(2)
        self.assertEqual(len(self.c.callstack()), 1)
        self.assertIsNotNone(self.c.last_writer(DATA + 0x40))

        self.c.load_state(self.path)
        self.assertEqual(self.c.callstack(), [])
        self.assertIsNone(self.c.last_writer(DATA + 0x40))
        self.assertEqual([p["id"] for p in self.c.points()], [bp])


if __name__ == "__main__":
    unittest.main()
