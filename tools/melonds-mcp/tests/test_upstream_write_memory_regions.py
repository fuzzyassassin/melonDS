"""Debugger::WriteMemory (mds_write and the Lua memory writes): a whole buffer, region-aware errors.

I/O is refused ('cannot be written by the debugger'), ROM and BIOS are 'is read-only', ITCM/DTCM stay writable, the
ARM9 palette/VRAM/OAM (where the bus drops byte writes) are written with halfwords that keep the neighbouring byte,
every write is read back, and a failure names the region and the address. Lua memory.write* raise
'memory write failed: ...'."""
import re
import unittest

from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE9, DATA, ConsoleTest

DTCM = 0x00800000


def mcr15(rd, crn, crm, opc2):
    return 0xEE000F10 | (crn << 16) | (rd << 12) | (opc2 << 5) | crm


def mrc15(rd, crn, crm, opc2):
    return 0xEE100F10 | (crn << 16) | (rd << 12) | (opc2 << 5) | crm


POWCNT1 = 0x04000304
VRAMCNT_A = 0x04000240


class WriteMemoryRegions(ConsoleTest):
    def setUp(self):
        super().setUp()
        a = A.Asm(CODE9)
        a.ldr_const(0, POWCNT1)
        a.ldr_const(1, 0x820F)
        a.strh(1, 0, 0)
        a.ldr_const(0, VRAMCNT_A)
        a.mov(1, 0x80)                          # bank A at LCDC 0x06800000
        a.strb(1, 0, 0)
        a.ldr_const(0, DTCM | 6)                # DTCM 4KB at 0x00800000, ITCM 4KB at 0, both enabled
        a.word(mcr15(0, 9, 1, 0))
        a.mov(0, 6)
        a.word(mcr15(0, 9, 1, 1))
        a.word(mrc15(0, 1, 0, 0))
        a.ldr_const(1, 0x50000)
        a.word(0xE1800001)                      # orr r0, r0, r1
        a.word(mcr15(0, 1, 0, 0))
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        self.run_frames(1)

    def fails(self, addr, data=b"\x01\x02", cpu=9):
        with self.assertRaises(MelonError) as cm:
            self.c.write(addr, data, cpu=cpu)
        return str(cm.exception)

    def test_io_is_refused_by_the_debugger(self):
        self.assertIn("I/O at 0x04000208 cannot be written by the debugger", self.fails(0x04000208))
        self.assertIn("I/O at 0x04000208 cannot be written by the debugger", self.fails(0x04000208, cpu=7))

    def test_rom_and_bios_are_read_only(self):
        self.assertIn("ARM9 BIOS at 0xFFFF0010 is read-only", self.fails(0xFFFF0010))
        self.assertIn("ARM7 BIOS at 0x00000010 is read-only", self.fails(0x00000010, cpu=7))
        self.assertIn("the GBA ROM area at 0x08000000 is read-only", self.fails(0x08000000))
        self.assertIn("the GBA ROM area at 0x09FFFFFE is read-only", self.fails(0x09FFFFFE))

    def test_error_names_the_failing_address_inside_a_buffer(self):
        # the first two bytes are OAM (fine), the buffer then runs into the GBA ROM area
        msg = self.fails(0x07FFFFFE, b"")
        self.assertIn("the GBA ROM area at 0x08000000 is read-only", msg)

    def test_itcm_dtcm_and_main_ram_take_bytes(self):
        self.c.write(DATA + 1, bytes([1, 2, 3]))
        self.assertEqual(self.c.read(DATA + 1, 3), bytes([1, 2, 3]))
        self.c.write(DTCM + 0x11, bytes([0xB1, 0xB2, 0xB3]))
        self.assertEqual(self.c.read(DTCM + 0x11, 3), bytes([0xB1, 0xB2, 0xB3]))
        # ITCM is at 0 on the ARM9 (the same address is BIOS on the ARM7)
        self.c.write(0x00000011, bytes([0xA1, 0xA2, 0xA3]))
        self.assertEqual(self.c.read(0x00000011, 3), bytes([0xA1, 0xA2, 0xA3]))

    def test_palette_vram_oam_halfword_writes_keep_the_neighbour(self):
        for base in (0x05000100, 0x06800100, 0x07000100):
            with self.subTest(hex(base)):
                self.c.write(base, bytes([0xAA] * 8))
                self.c.write(base + 1, bytes([1, 2, 3]))
                self.assertEqual(self.c.read(base, 8), bytes([0xAA, 1, 2, 3, 0xAA, 0xAA, 0xAA, 0xAA]))
                self.c.write(base + 6, bytes([0x55]))
                self.assertEqual(self.c.read(base + 6, 2), bytes([0x55, 0xAA]))
                self.c.write(base + 7, bytes([0x66]))
                self.assertEqual(self.c.read(base + 6, 2), bytes([0x55, 0x66]))

    def test_a_long_buffer_is_written_whole(self):
        data = bytes((i * 7 + 3) & 0xFF for i in range(0x201))
        self.c.write(0x06800001, data)
        self.assertEqual(self.c.read(0x06800001, len(data)), data)

    def test_unmapped_vram_names_region_and_address(self):
        msg = self.fails(0x06820000, b"\x12\x34")
        self.assertIn("VRAM at 0x06820000 did not take the write (no VRAM bank mapped there?)", msg)

    def test_lua_writes_raise_memory_write_failed(self):
        out = self.c.lua_exec("memory.write_u8(0x05000021, 0x5A, 'arm9')\n"
                              "memory.write_bytes_as_array(0x06800011, {7, 8, 9}, 'arm9')\n"
                              "print(memory.read_u8(0x05000021, 'arm9'), memory.read_u8(0x06800013, 'arm9'))")
        self.assertEqual(out, "90\t9\n")
        for code, text in (("memory.write_u32_le(0x04000208, 1)", "memory write failed: I/O at 0x04000208"),
                           ("memory.write_u8(0x08000000, 1, 'arm9')", "memory write failed: the GBA ROM area at 0x08000000"),
                           ("memory.write_u8(0x06820000, 1, 'arm9')", "memory write failed: VRAM at 0x06820000")):
            with self.subTest(code):
                with self.assertRaises(MelonError) as cm:
                    self.c.lua_exec(code)
                self.assertIn(text, str(cm.exception))


if __name__ == "__main__":
    unittest.main()
