"""Lua errors whose value is not a string, or is empty, are reported as placeholder text (ErrorText) instead of
building a std::string from NULL."""
import unittest

from melonds_mcp.native import MelonError

from .base import ConsoleTest

NOT_A_STRING = "(error object is not a string)"
EMPTY = "(empty error message)"


class LuaErrorText(ConsoleTest):
    def setUp(self):
        super().setUp()
        # error(nil): Lua 5.4 raises nil (the placeholder); Lua 5.5 raises the string "<no error object>" itself
        # (lua_error replaces a nil error object), so ErrorText gets a string. Expect whatever this Lua raises.
        self.c.lua_output()
        raised = self.c.lua_exec("print(select(2, pcall(error, nil)))").strip()
        self.CASES = (("error('', 0)", EMPTY), ("error({})", NOT_A_STRING),
                      ("error(nil)", NOT_A_STRING if raised == "nil" else raised))

    def test_exec_reports_placeholder_text(self):
        for code, text in self.CASES:
            with self.subTest(code):
                with self.assertRaises(MelonError) as cm:
                    self.c.lua_exec(code)
                self.assertIn(text, str(cm.exception))

    def test_run_reports_placeholder_text(self):
        for code, text in self.CASES:
            with self.subTest(code):
                with self.assertRaises(MelonError) as cm:
                    self.c.lua_run(code)
                self.assertIn(text, str(cm.exception))

    def test_error_after_a_yield_reports_placeholder_text(self):
        # Resume path: the error happens in the coroutine after emu.frameadvance().
        for code, text in self.CASES:
            with self.subTest(code):
                self.c.lua_stop()
                self.c.lua_output()
                self.c.lua_run("emu.frameadvance()\n" + code)
                self.run_frames(2)
                self.assertIn("lua: " + text, self.c.lua_output())


if __name__ == "__main__":
    unittest.main()
