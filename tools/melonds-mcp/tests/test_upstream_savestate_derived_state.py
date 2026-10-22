"""Upstream change: a state load recomputes IRQ lines, clears IdleLoop and sets up ARM9 code memory after CP15
is restored (03905f0eb575).

Nothing from the run before a load may survive it: the register traces (cycle, pc, instruction, cpsr of both
CPUs) of the frames after a load must be identical on a fresh console, on a console that ran an idle loop
first and on the console that saved the state and ran on. IdleLoop itself is only set by the JIT (the headless
library runs the interpreter), so it is covered through its effect: a console that ran idle loops before the
load must not skip time in the code that follows. ARM9 fetch timing (cached code) and the pending ARM7 timer
IRQ are covered by the traces too.
"""
import os
import tempfile
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE7, CODE9, have_library
from .test_savestate_reuse import COUNT7, cached_block, pending_irq_then_enable

FRAMES = 2
TRACE = 3000


@unittest.skipUnless(have_library(), "melonds_headless library not found")
class LoadLeavesNothingOfTheRunBefore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.state = os.path.join(self.tmp.name, "s.mln")

    def console(self):
        c = native.Console()
        c.reset()
        return c

    def idle_program(self, base):
        a = A.Asm(base)
        a.mov(1, 0)
        a.label("idle")
        a.add(1, 1, 1)
        a.b("idle")
        return a

    def run_traced(self, c):
        c.trace(TRACE, cpu="both")
        timeout, _ = c.run(FRAMES)
        self.assertFalse(timeout)
        return c.take_trace()

    def load_and_trace(self, c):
        c.load_state(self.state)
        return self.run_traced(c)

    def test_traces_after_a_load_are_independent_of_the_previous_run(self):
        p9, p7 = cached_block(), pending_irq_then_enable()
        a = self.console()
        try:
            a.write(p9.base, p9.assemble(), cpu=9)
            a.set_reg("pc", p9.base, cpu=9)
            a.write(p7.base, p7.assemble(), cpu=7)
            a.set_reg("pc", p7.base, cpu=7)
            timeout, _ = a.run(1)
            self.assertFalse(timeout)
            a.save_state(self.state)
            same = self.load_and_trace(a)            # the saving console, after it ran on
            a.close()
        except BaseException:
            a.close()
            raise

        fresh = self.console()
        try:
            fresh_trace = self.load_and_trace(fresh)
        finally:
            fresh.close()

        dirty = self.console()                       # ran idle loops on both CPUs, caches on, first
        try:
            for cpu, base in ((9, CODE9), (7, CODE7)):
                prog = self.idle_program(base)
                dirty.write(base, prog.assemble(), cpu=cpu)
                dirty.set_reg("pc", base, cpu=cpu)
            timeout, _ = dirty.run(3)
            self.assertFalse(timeout)
            dirty_trace = self.load_and_trace(dirty)
        finally:
            dirty.close()

        self.assertGreater(len(fresh_trace), 100)
        self.assertEqual(len(set(t[1] for t in fresh_trace)), 2, "both CPUs ran")
        self.assertEqual(fresh_trace, same)
        self.assertEqual(fresh_trace, dirty_trace)

    def test_pending_irq_is_taken_without_an_io_write(self):
        """The loaded IE/IME/IF alone decide: the ARM7 handler runs in the first frame after the load."""
        p7 = pending_irq_then_enable()
        a = self.console()
        try:
            a.write(p7.base, p7.assemble(), cpu=7)
            a.set_reg("pc", p7.base, cpu=7)
            a.run(1)
            a.save_state(self.state)
        finally:
            a.close()
        for prior in (False, True):
            with self.subTest(dirty=prior):
                c = self.console()
                try:
                    if prior:
                        prog = self.idle_program(CODE7)
                        c.write(CODE7, prog.assemble(), cpu=7)
                        c.set_reg("pc", CODE7, cpu=7)
                        c.run(2)
                    handler = p7.labels["handler"]
                    c.load_state(self.state)
                    c.trace(100, cpu=7, start=handler, end=handler + 0x40)   # only the handler
                    timeout, _ = c.run(8)
                    self.assertFalse(timeout)
                    self.assertTrue(c.take_trace(), "the pending IRQ was not taken")
                    self.assertEqual(int.from_bytes(c.read(COUNT7, 4, cpu=7), "little"), 1)
                finally:
                    c.close()
