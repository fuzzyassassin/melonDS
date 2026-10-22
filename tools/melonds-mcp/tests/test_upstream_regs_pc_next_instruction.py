"""Upstream 03905f0eb: mds_get_regs pc (index 15) is the next instruction of each CPU in every state (R15 - 4 in
ARM, - 2 in Thumb; the debugger's last hooked pc is no longer used while parked), and at a fault park of this CPU,
while R15 is unchanged since the park, it is the event's PC (the faulting instruction). The existing
PcIsTheNextInstruction tests cover the plain parks; these add the Thumb state, a CPU that never had hooks, and the
fault park with its release once the pc is changed."""
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE7, CODE9, DATA, ConsoleTest
from .test_api_fixes import adds
from .test_savestate_reuse import mcr15


def data_abort_program():
    """ARM9: PU on with one region over the whole space, code allowed and data refused, then a load that aborts."""
    a = A.Asm(CODE9)
    a.ldr_const(0, DATA)
    a.mov(1, 0x3F)                          # region 0: base 0, 4 GB, enabled
    a.word(mcr15(1, 6, 0, 0))
    a.mov(1, 0x03)
    a.word(mcr15(1, 5, 0, 3))               # instruction access: full
    a.mov(1, 0)
    a.word(mcr15(1, 5, 0, 2))               # data access: none
    a.mov(1, 1)
    a.word(mcr15(1, 1, 0, 0))               # protection unit on
    a.label("fault")
    a.ldr(2, 0)                             # data abort
    a.label("after")
    a.nop()
    a.label("spin")
    a.b("spin")
    return a


class PcAtAFault(ConsoleTest):
    def park_at_fault(self):
        prog = data_abort_program()
        self.load_program(prog)
        self.c.add_breakpoint(0x02FF0000)       # never hit: arms the hooks (faults park in the hooked interpreter)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.cpu), ("fault", 9))
        return prog, b

    def test_fault_reports_the_faulting_instruction(self):
        prog, b = self.park_at_fault()
        fault = prog.labels["fault"]
        self.assertEqual(b.pc, fault)
        r = self.c.regs()
        # R15 - 4 would be the instruction after the load; the fault parks inside the load
        self.assertEqual(r["pc"], fault)
        self.assertEqual(self.c.regs()["pc"], fault)

    def test_pc_change_after_a_fault_is_the_next_instruction(self):
        prog, b = self.park_at_fault()
        self.c.set_reg("pc", prog.labels["spin"])
        r = self.c.regs()
        self.assertEqual(r["pc"], prog.labels["spin"])
        self.assertEqual(r["pc"], r["r15_raw"] - 4)

    def test_other_cpu_is_not_the_fault_pc(self):
        self.load_program(adds(4096, CODE7), CODE7, cpu=7)
        self.c.set_reg("r1", 0, cpu=7)
        prog, b = self.park_at_fault()
        r = self.c.regs(7)
        self.assertEqual(r["pc"], r["r15_raw"] - 4)
        self.assertNotEqual(r["pc"], b.pc)


class PcInThumb(ConsoleTest):
    def test_thumb_next_instruction_is_r15_minus_2(self):
        a = A.Asm(CODE9)
        a.ldr_const(0, CODE9 + 0x100 + 1)
        a.bx(0)
        self.load_program(a)
        t = CODE9 + 0x100
        self.c.write(t, bytes([0x00, 0x1C] * 16 + [0xFE, 0xE7]))      # adds r0,r0,#0 x16; b .
        self.c.add_breakpoint(t + 8)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        self.assertEqual(self.c.take_break().pc, t + 8)
        r = self.c.regs()
        self.assertTrue(r["cpsr"] & 0x20)
        self.assertEqual(r["pc"], t + 8)
        self.assertEqual(r["pc"], r["r15_raw"] - 2)


if __name__ == "__main__":
    unittest.main()
