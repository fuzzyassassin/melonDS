"""Review findings on pr/headless-debug: what the debugger should do at and around DMA watchpoint breaks, what a
write hook sees, and step counts armed from inside a hook. Each class names the finding it pins down."""
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE9, DATA, ConsoleTest
from .test_api_fixes import DMA0, adds, dma_program

MOV_R2_1 = 0xE3A02001   # mov r2, #1
MOV_R2_2 = 0xE3A02002   # mov r2, #2


def dma_then(src, dst, units, tail):
    """ARM9: start DMA0 (immediate, 32-bit, `units` words) from src to dst, then run `tail(asm)`."""
    a = A.Asm(CODE9)
    a.ldr_const(0, DMA0)
    a.ldr_const(1, src)
    a.str(1, 0, 0)
    a.ldr_const(1, dst)
    a.str(1, 0, 4)
    a.ldr_const(1, 0x84000000 | units)
    a.str(1, 0, 8)
    a.label("after")
    tail(a)
    return a


class DmaBreakPipelineRefetch(ConsoleTest):
    """Finding 1: code written while parked at a DMA watchpoint break replaces the instruction the ARM9 has
    already fetched, as it does at a CPU break (PipelineRefetch in test_upstream_debug)."""

    SRC, DST = DATA + 0x500, DATA + 0x600

    def test_write_at_a_dma_break_changes_the_next_instruction(self):
        def tail(a):
            a.word(MOV_R2_1)            # the ARM9's next instruction when the DMA parks
            a.label("spin")
            a.b("spin")
        prog = dma_then(self.SRC, self.DST, 4, tail)
        after = prog.labels["after"]
        self.c.write(self.SRC, bytes(range(1, 17)))
        self.c.write(self.DST, bytes(16))
        self.load_program(prog)
        self.c.set_reg("r2", 0)
        wp = self.c.add_watchpoint(self.DST, 16, write=True)
        _, states = self.c.run(3)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.dma, b.pc), ("write", 0, native.DMA_TAG))
        self.assertEqual(self.c.regs()["pc"], after)       # parked before the instruction we patch
        self.c.remove_point(wp)
        self.c.write(after, MOV_R2_2.to_bytes(4, "little"))
        self.run_frames(1)
        self.assertEqual(self.c.regs()["r2"], 2, "the stale prefetched mov r2, #1 ran instead of the patch")


class WriteHookSeesTheOldValue(ConsoleTest):
    """Finding 2: a write hook sees memory before the write, whether a CPU store or a DMA unit makes it."""

    OLD, NEW = 0xAABBCCDD, 0x11223344
    SRC, DST = DATA + 0x500, DATA + 0x600

    def hook(self, addr):
        # prints the event's value and what memory holds at the hook
        self.c.lua_exec("event.onmemorywrite(function(a, v) print(v, memory.read_u32_le(a)) end, 0x%x, 'w', 'arm9', 4)"
                        % addr)

    def cpu_write(self):
        a = A.Asm(CODE9)
        a.ldr_const(0, self.DST)
        a.ldr_const(1, self.NEW)
        a.str(1, 0)
        a.label("spin")
        a.b("spin")
        self.c.write(self.DST, self.OLD.to_bytes(4, "little"))
        self.hook(self.DST)
        self.load_program(a)
        self.run_frames(1)
        self.assertEqual(self.word(self.DST), self.NEW)
        return self.c.lua_output()

    def dma_write(self):
        self.c.write(self.SRC, self.NEW.to_bytes(4, "little"))
        self.c.write(self.DST, self.OLD.to_bytes(4, "little"))
        self.hook(self.DST)
        self.load_program(dma_program(self.SRC, self.DST, units=1))
        self.run_frames(1)
        self.assertEqual(self.word(self.DST), self.NEW)
        return self.c.lua_output()

    def test_cpu_store_hook_sees_the_old_value(self):
        self.assertEqual(self.cpu_write(), f"{self.NEW}\t{self.OLD}\n")

    def test_dma_hook_sees_the_old_value(self):
        self.assertEqual(self.dma_write(), f"{self.NEW}\t{self.OLD}\n")

    def test_cpu_and_dma_hooks_agree(self):
        cpu = self.cpu_write()
        other = native.Console()
        other.reset()
        self.addCleanup(other.close)
        self.c = other
        dma = self.dma_write()
        self.assertEqual(dma, cpu)


class OneBreakPerDmaTransfer(ConsoleTest):
    """Finding 3: one DMA transfer writing many units into a watched range breaks once (reporting the channel),
    as upstream's queued hit did, not once per unit."""

    SRC, DST = DATA + 0x500, DATA + 0x600
    UNITS = 64                          # 0x100 bytes

    def test_a_dma_across_a_watched_range_breaks_once(self):
        self.c.write(self.SRC, bytes(i & 0xFF for i in range(4 * self.UNITS)))
        self.c.write(self.DST, bytes(4 * self.UNITS))
        wp = self.c.add_watchpoint(self.DST, 4 * self.UNITS, write=True)
        self.load_program(dma_program(self.SRC, self.DST, units=self.UNITS))
        breaks = []
        for _ in range(self.UNITS + 2):
            _, states = self.c.run(1)
            if states != [native.PARKED]:
                break
            breaks.append(self.c.take_break())
        self.assertEqual(len(breaks), 1, f"{len(breaks)} breaks for one DMA transfer")
        b = breaks[0]
        self.assertEqual((b.kind, b.cpu, b.dma, b.id, b.addr), ("write", 9, 0, wp, self.DST))
        self.assertEqual(self.c.read(self.DST, 4 * self.UNITS), bytes(i & 0xFF for i in range(4 * self.UNITS)))


class StepFromAHook(ConsoleTest):
    """Finding 4: debugger.step(n) armed inside a hook runs the same instructions as from the break that hook
    would have made: n instructions."""

    def run_to_step(self):
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual(b.kind, "step")
        return self.c.regs()["r1"], b.pc

    def test_step_from_a_breakpoint_callback_executes_n(self):
        # from the breakpoint break at CODE9 + 12, step(2) gives (5, CODE9 + 20): StepExact.test_step_after_a_breakpoint
        self.load_program(adds(64))
        self.c.set_reg("r1", 0)
        self.c.lua_exec("event.onmemoryexecute(function() debugger.step(2) end, 0x%x)" % (CODE9 + 12))
        self.assertEqual(self.run_to_step(), (5, CODE9 + 20))

    def store_then_adds(self):
        """str r1 at CODE9 + 8 (to DATA), then adds; r1 counts the adds executed."""
        a = A.Asm(CODE9)
        a.ldr_const(0, DATA)
        a.mov(1, 0)
        a.str(1, 0)
        for _ in range(16):
            a.add(1, 1, 1)
        a.label("spin")
        a.b("spin")
        return a

    def test_step_from_a_watch_callback_matches_the_watch_break(self):
        # the watch break parks at the instruction after the store; step(2) there runs two adds
        self.load_program(self.store_then_adds())
        wp = self.c.add_watchpoint(DATA, 4, write=True)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.pc), ("write", CODE9 + 8))
        self.assertEqual(self.c.regs()["pc"], CODE9 + 12)
        self.c.remove_point(wp)
        self.c.step(2)
        from_break = self.run_to_step()
        self.assertEqual(from_break, (2, CODE9 + 20))

        other = native.Console()
        other.reset()
        self.addCleanup(other.close)
        self.c = other
        self.load_program(self.store_then_adds())
        self.c.lua_exec("event.onmemorywrite(function() debugger.step(2) end, 0x%x, 'w', 'arm9', 4)" % DATA)
        self.assertEqual(self.run_to_step(), from_break)


if __name__ == "__main__":
    unittest.main()
