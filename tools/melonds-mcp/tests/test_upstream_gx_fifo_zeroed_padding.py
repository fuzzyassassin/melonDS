"""Upstream 03905f0e: GX FIFO entries are value-initialised (``CmdFIFOEntry entry {}``), in both
WriteToGXFIFO (the 0x04000400 port) and Write32 (the command ports 0x04000440..0x040005CB).

The 3 padding bytes of each CmdFIFOEntry go into savestates; they used to be stack garbage. So the
padding of every queued entry is zero, whichever path queued it, and two saves of one console
state are byte-identical.
"""
import os
import tempfile
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE9, have_library
from .test_savestate_reuse import sections, skip_drawing, threaded_3d

FIFO_BASE = 12          # GP3D section: 12 bytes, then the 256 entries of 8 bytes (cmd, 3 pad, param)
ENTRIES = 256


def fifo_flood(port):
    """ARM9: power on 3D, then queue geometry commands for ever through `port`, with the stack
    dirtied first (call-site garbage is what the padding used to pick up)."""
    a = A.Asm(CODE9)
    a.ldr_const(4, 0x04000000)
    a.ldr_const(0, 0x820F)
    a.str(0, 4, 0x304)                          # POWCNT1
    a.ldr_const(5, port)
    a.ldr_const(7, 0xA5A5A5A5)
    a.label("again")
    for i in range(48):
        if port == 0x04000400:
            a.ldr_const(0, 0x15151515)          # four packed MTX_IDENTITY
            a.str(0, 5)
        else:
            a.mov(0, i & 3)
            a.str(0, 5)                         # MTX_MODE
            a.str(7, 5, 0x14)                   # MTX_IDENTITY (0x454)
    a.b("again")
    return a


@unittest.skipUnless(have_library(), "melonds_headless library not found")
class GxFifoZeroedPadding(unittest.TestCase):

    def queued_state(self, port, setup):
        c = native.Console()
        c.reset()
        self.addCleanup(c.close)
        if setup:
            setup(c)
        c.write(CODE9, fifo_flood(port).assemble())
        c.set_reg("pc", CODE9)
        timeout, _ = c.run(2)
        self.assertFalse(timeout)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        blobs = []
        for n in ("a", "b"):
            p = os.path.join(tmp.name, n + ".mln")
            c.save_state(p)
            with open(p, "rb") as f:
                blobs.append(f.read())
        return blobs

    def entries(self, blob):
        gp = sections(blob)["GP3D"]
        raw = gp[FIFO_BASE:FIFO_BASE + ENTRIES * 8]
        return [raw[i:i + 8] for i in range(0, len(raw), 8) if raw[i:i + 8] != bytes(8)]

    def test_padding_zero_both_paths(self):
        for port, label in ((0x04000440, "Write32 command ports"), (0x04000400, "WriteToGXFIFO")):
            for name, setup in (("plain", None), ("skip", skip_drawing), ("threaded", threaded_3d)):
                with self.subTest(path=label, mode=name):
                    a, b = self.queued_state(port, setup)
                    written = self.entries(a)
                    self.assertGreater(len(written), 16)    # commands went through the FIFO
                    self.assertEqual([e for e in written if e[5:8] != b"\0\0\0"], [])

    def test_two_saves_identical(self):
        for port in (0x04000440, 0x04000400):
            with self.subTest(port=hex(port)):
                a, b = self.queued_state(port, None)
                self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
