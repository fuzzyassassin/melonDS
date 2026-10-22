"""A DMA watchpoint break is raised in the Service hook context (upstream 43adf523): the console is parked
between instructions, not inside an instruction hook. Instruction-only operations are refused there, writes land
before the next instruction, and one transfer gives exactly one break."""
from melonds_mcp import native

from . import armasm as A
from .base import CODE9, DATA, ConsoleTest

DMA3 = 0x040000B0 + 3 * 12
MOV_R2_1 = 0xE3A02001   # mov r2, #1
MOV_R2_2 = 0xE3A02002   # mov r2, #2


def dma3_program(src, dst, units=4):
    """ARM9: start DMA3 (immediate, 32-bit, main RAM to main RAM) of `units` words, then `mov r2, #1`, spin."""
    a = A.Asm(CODE9)
    a.ldr_const(0, DMA3)
    a.ldr_const(1, src)
    a.str(1, 0, 0)
    a.ldr_const(1, dst)
    a.str(1, 0, 4)
    a.ldr_const(1, 0x84000000 | units)
    a.str(1, 0, 8)
    a.label("after")
    a.word(MOV_R2_1)
    a.label("spin")
    a.b("spin")
    return a


class DmaBreakInServiceContext(ConsoleTest):
    SRC, DST = DATA + 0x700, DATA + 0x800

    def park(self):
        self.c.write(self.SRC, bytes(range(1, 17)))
        self.c.write(self.DST, bytes(16))
        prog = dma3_program(self.SRC, self.DST)
        self.load_program(prog)
        self.c.set_reg("r2", 0)
        self.wp = self.c.add_watchpoint(self.DST, 16, write=True)
        _, states = self.c.run(3)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.dma, b.pc), ("write", 3, native.DMA_TAG | (3 << 1) | 0))
        return prog, b

    def test_execute_is_refused_at_the_park(self):
        self.park()
        with self.assertRaises(native.MelonError) as cm:
            self.c.lua_exec("memory.execute(0xE1A00000)")
        self.assertIn("only inside an exec hook", str(cm.exception))

    def test_write_at_the_park_takes_effect_before_the_next_instruction(self):
        prog, _ = self.park()
        after = prog.labels["after"]
        self.assertEqual(self.c.regs()["pc"], after)
        self.c.remove_point(self.wp)
        self.c.write(after, MOV_R2_2.to_bytes(4, "little"))
        self.c.set_reg("r2", 0)
        _, states = self.c.run(1)
        self.assertEqual(states, [native.IDLE])
        self.assertEqual(self.c.regs()["r2"], 2)

    def test_one_break_per_transfer(self):
        breaks = []
        self.c.write(self.SRC, bytes(range(1, 17)))
        self.c.write(self.DST, bytes(16))
        self.load_program(dma3_program(self.SRC, self.DST))
        self.c.add_watchpoint(self.DST, 16, write=True)
        for _ in range(6):
            _, states = self.c.run(1)
            if states != [native.PARKED]:
                break
            breaks.append(self.c.take_break())
        self.assertEqual(len(breaks), 1, f"{len(breaks)} breaks for one DMA transfer")
        self.assertEqual(self.c.read(self.DST, 16), bytes(range(1, 17)))
