"""The headless console table is never destroyed: exiting with live consoles must not hang or crash.

Upstream commit 03905f0e: Api.cpp Slots is a leaked map, because at process exit Windows has already killed the
emulator threads before static destructors run, so destroying instances there could hang or crash.
"""
import subprocess
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]

SCRIPTS = {
    "many consoles, half running": """
cs = [native.Console() for _ in range(8)]
for i, c in enumerate(cs):
    c.reset()
    c.run(2)
    if i % 2:
        c.start()
""",
    "running consoles and a server": """
cs = [native.Console() for _ in range(3)]
for c in cs:
    c.reset()
    c.start()
native.set_pacing(60)
native.serve(0)
""",
    "parked at a breakpoint plus a running one": """
a = native.Console()
b = native.Console()
a.reset()
b.reset()
a.add_breakpoint(0x02100000)
a.write(0x02100000, bytes(8))
a.set_reg("pc", 0x02100000)
a.run(5)
b.start()
""",
    "destroyed one, leaked another": """
a = native.Console()
b = native.Console()
a.reset()
b.reset()
b.start()
del a
""",
}


class SlotsNeverDestroyedTests(unittest.TestCase):
    def run_script(self, body):
        code = "from melonds_mcp import native\n" + body + "\nprint('body done', flush=True)\n"
        proc = subprocess.Popen([sys.executable, "-c", code], cwd=str(TOOLS), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            self.fail(f"process hung at exit (static destruction of the console table?):\n{out}")
        self.assertIn("body done", out)
        self.assertEqual(proc.returncode, 0, f"crash at exit: {proc.returncode:#x}\n{out}")

    def test_exit_with_live_consoles(self):
        for name, body in SCRIPTS.items():
            for attempt in range(3):
                with self.subTest(name, attempt=attempt):
                    self.run_script(body)


if __name__ == "__main__":
    unittest.main()
