"""Upstream 43adf523: Debugger::Step skips the +1 when HookedCPU() == n. The fork keeps the +1 for a step armed
from a watch callback (the accessing instruction is not counted), even when that watch fires inside a breakpoint
callback's memory.execute, where the CPU is in Hook context."""
import unittest

from melonds_mcp import native

from .base import CODE9, DATA, ConsoleTest
from .test_api_fixes import adds

STR_R1_R0 = 0xE5801000   # str r1, [r0]


class StepFromAWatchCallbackInsideMemoryExecute(ConsoleTest):
    def test_step_from_a_watch_callback_inside_memory_execute(self):
        self.load_program(adds(64))
        self.c.set_reg("r1", 0)
        self.c.set_reg("r0", DATA)
        self.c.lua_exec("""
            event.onmemoryexecute(function() memory.execute(0x%x) end, 0x%x)
            event.onmemorywrite(function() debugger.step(2) end, 0x%x, 'w', 'arm9', 4)
        """ % (STR_R1_R0, CODE9 + 12, DATA))
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual(b.kind, "step")
        # the injected store is not counted and the watch keeps the +1: step(2) + 1 = 3 adds from the hooked one (upstream rule would give (5, CODE9 + 20))
        self.assertEqual((self.c.regs()["r1"], b.pc), (6, CODE9 + 24))


if __name__ == "__main__":
    unittest.main()
