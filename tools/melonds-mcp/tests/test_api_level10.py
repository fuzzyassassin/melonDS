"""API level 10 (feature names for the review fixes of 2026-10-22, which changed behaviour at level 9): one DMA
watchpoint break per transfer, DMA hooks see the old value, code patched at a DMA break runs, and step(n) from a
breakpoint callback. The behaviours themselves are tested in test_pr_review.py and
test_upstream_dma_break_service_context.py; this checks that the library reports them."""
import unittest

from melonds_mcp import native

from .base import have_library

LEVEL10 = ("dma_break_once", "dma_hook_old_value", "dma_break_patch", "step_from_hook")


@unittest.skipUnless(have_library(), "melonds_headless library not found")
class Level10Features(unittest.TestCase):
    def test_version_is_10(self):
        self.assertEqual(native.api_version(), 10)

    def test_features(self):
        for name in LEVEL10:
            with self.subTest(name):
                self.assertTrue(native.has(name))

    def test_unknown_feature_is_still_absent(self):
        self.assertFalse(native.has("dma_break_twice"))
