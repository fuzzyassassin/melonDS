"""Interrupt only acts while running; parking and the end of a run cancel stale break requests but keep step
countdowns (upstream 03905f0). The fork's Interrupt must also keep the waiting-for-input frame-boundary case."""
import time
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE7, CODE9, ConsoleTest
from .test_api_fixes2 import spin
from .test_input import Akey, InputTest


class InterruptCleanup(ConsoleTest):
    def setUp(self):
        super().setUp()
        self.load_program(spin(CODE9))
        self.load_program(spin(CODE7), CODE7, cpu=7)

    def test_late_interrupt_does_not_park_the_next_run(self):
        # An interrupt racing the end of a short run must leave nothing armed for the run after it.
        for i in range(150):
            self.c.start(1)
            if i % 3:
                time.sleep(0.0005 * (i % 3))
            self.c.interrupt()
            timeout, states = self.c.wait(10000)
            self.assertFalse(timeout)
            self.assertIn(states[0], (native.IDLE, native.PARKED), i)
            self.c.take_break()
            self.assertEqual(self.c.run(2), (False, [native.IDLE]), f"iteration {i}")
            self.assertIsNone(self.c.take_break(), f"iteration {i}")

    def test_interrupt_when_not_running_is_dropped(self):
        for _ in range(3):
            self.c.interrupt()
        self.assertEqual(self.c.run(2), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())

    def test_interrupt_after_run_ended_keeps_nothing(self):
        self.assertEqual(self.c.run(1), (False, [native.IDLE]))
        self.c.interrupt()
        self.assertEqual(self.c.run(2), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())

    def test_step_armed_before_run_still_fires_after_interrupt_cycles(self):
        self.c.step(5)
        self.c.interrupt()                                  # idle: ignored, countdown kept
        self.assertEqual(self.c.run(3)[1], [native.PARKED])
        self.assertEqual(self.c.take_break().kind, "step")

    def test_step_survives_an_interrupt_park(self):
        self.c.step(100000000)
        self.c.start()
        time.sleep(0.05)
        self.c.interrupt()
        self.assertEqual(self.c.wait(10000), (False, [native.PARKED]))
        self.assertEqual(self.c.take_break().kind, "request")
        self.c.step(3)
        self.assertEqual(self.c.run(2)[1], [native.PARKED])
        self.assertEqual(self.c.take_break().kind, "step")
        self.assertEqual(self.c.run(2), (False, [native.IDLE]))


class WaitingInterrupt(InputTest):
    def setUp(self):
        super().setUp()
        self.c.input_mode("wait")

    def test_waiting_for_input_interrupt_parks_and_resumes(self):
        self.c.submit_input(Akey)
        self.c.start(100)
        self.wait_until(lambda: self.c.input_waiting(), what="the console to wait")
        self.c.interrupt()
        self.assertEqual(self.c.wait(10000), (False, [native.PARKED]))
        self.assertEqual(self.c.take_break().kind, "request")
        self.assertEqual(self.c.frames, 1)
        self.c.start(0)
        self.assertEqual(self.c.wait(10000), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())


if __name__ == "__main__":
    unittest.main()
