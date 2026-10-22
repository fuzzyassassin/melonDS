"""Upstream 03905f0e: mds_packets_take cuts a first packet longer than max_data short and reports the full size in
mds_packet.total. The struct is five fields (timestamp u64, sent, type, length, total u32) in melonds_api.h and in
the ctypes mirror. Independent of test_api_fixes2.PacketsNeverBlock: it checks the ABI and the edge cases."""
import re
import sys
import unittest
from ctypes import c_uint8, c_uint32, c_uint64, sizeof
from pathlib import Path

from melonds_mcp import native

from .base import ConsoleTest

HEADER = Path(__file__).resolve().parents[3] / "src" / "frontend" / "headless" / "melonds_api.h"


class PacketStructLayout(unittest.TestCase):
    def test_ctypes_struct_has_exactly_five_fields_like_the_header(self):
        self.assertEqual(native.MdsPacket._fields_, [
            ("timestamp", c_uint64), ("sent", c_uint32), ("type", c_uint32), ("length", c_uint32),
            ("total", c_uint32)])
        self.assertEqual(sizeof(native.MdsPacket), 24)
        text = HEADER.read_text()
        body = re.search(r"typedef struct\s*\{([^}]*)\}\s*mds_packet;", text).group(1)
        body = re.sub(r"//[^\n]*", "", body)
        fields = re.findall(r"(uint(?:32|64)_t)\s+(\w+)\s*;", body)
        self.assertEqual(fields, [("uint64_t", "timestamp"), ("uint32_t", "sent"), ("uint32_t", "type"),
                                  ("uint32_t", "length"), ("uint32_t", "total")])


class PacketsTakeTotal(ConsoleTest):
    def log(self, size, fill, sent=1, type_=0):
        data = (c_uint8 * size)(*([fill] * size))
        self.assertEqual(native.lib().mds_debug_packet(self.c.id, sent, type_, data, size), 0)

    def take(self, max_packets, max_data):
        hdrs, data = (native.MdsPacket * max_packets)(), (c_uint8 * max(max_data, 1))()
        n = native.lib().mds_packets_take(self.c.id, hdrs, data, max_packets, max_data)
        self.assertGreaterEqual(n, 0)
        out, off = [], 0
        for h in hdrs[:n]:
            out.append((h.length, h.total, bytes(data[off:off + h.length])))
            off += h.length
        return out

    def test_first_packet_too_long_is_truncated_with_full_total(self):
        self.c.packet_log(True)
        self.log(50, 9)
        self.assertEqual(self.take(4, 20), [(20, 50, bytes([9] * 20))])
        self.assertEqual(self.take(4, 20), [])          # the cut part is dropped, not re-queued

    def test_exact_fit_is_not_truncated(self):
        self.c.packet_log(True)
        self.log(20, 5)
        self.assertEqual(self.take(4, 20), [(20, 20, bytes([5] * 20))])

    def test_later_packet_that_does_not_fit_waits_whole(self):
        self.c.packet_log(True)
        self.log(8, 1)
        self.log(40, 2)
        self.log(4, 3)
        self.assertEqual(self.take(4, 20), [(8, 8, bytes([1] * 8))])
        self.assertEqual(self.take(4, 20), [(20, 40, bytes([2] * 20))])    # now first: cut
        self.assertEqual(self.take(4, 20), [(4, 4, bytes([3] * 4))])

    def test_header_fields_survive_next_to_total(self):
        self.c.packet_log(True)
        self.log(30, 7, sent=0, type_=3)
        hdrs, data = (native.MdsPacket * 1)(), (c_uint8 * 10)()
        self.assertEqual(native.lib().mds_packets_take(self.c.id, hdrs, data, 1, 10), 1)
        h = hdrs[0]
        self.assertEqual((h.sent, h.type, h.length, h.total), (0, 3, 10, 30))

    def test_zero_max_data_reports_an_empty_cut_packet(self):
        self.c.packet_log(True)
        self.log(5, 1)
        self.assertEqual(self.take(2, 0), [(0, 5, b"")])
        self.assertEqual(self.take(2, 16), [])


if __name__ == "__main__":
    unittest.main()
