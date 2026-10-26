"""Qt frontend startup options: --state, --symbols, --labels, --break, --pause.

Needs a Qt build configured with -DMELONDS_AUTOMATION=ON, named by MELONDS_QT_AUTOMATION_EXE (skipped otherwise):
the test copies the exe into a temporary folder with a `portable` folder beside it, so the run has its own config and
the user's melonDS.toml is never touched, starts it with a synthetic ROM and the options, and reads the result back
through the automation command folder (MELONDS_AUTOMATION). The savestate is made with the headless library from the
same synthetic ROM.

    MELONDS_QT_AUTOMATION_EXE=build/qt-auto/melonDS.exe MELONDS_HEADLESS_LIB=... python -m unittest tests.test_qt_startup
"""
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from melonds_mcp import native

from .base import have_library
from .test_consoleid import make_rom

QT_EXE = os.environ.get("MELONDS_QT_AUTOMATION_EXE")
MARKER_ADDR = 0x02200000
MARKER = 0x5CE7A12F
BP1 = 0x02100000
BP2 = 0x02100040   # given by label name
BP3 = 0x02100080   # given by symbol name
LOOP = 0x02100100  # the state's ARM9 spins here (b .)


class Qt:
    """One melonDS Qt process driven through its automation folder."""

    def __init__(self, folder, args):
        self.folder = Path(folder)
        self.auto = self.folder / "auto"
        self.auto.mkdir()
        exe = self.folder / "melonDS.exe"
        shutil.copyfile(QT_EXE, exe)
        (self.folder / "portable").mkdir()
        env = dict(os.environ)
        env["MELONDS_AUTOMATION"] = str(self.auto)
        env.setdefault("QT_QPA_PLATFORM", "offscreen")
        extra = os.environ.get("MELONDS_DLL_DIRS", r"D:\msys64\ucrt64\bin")
        env["PATH"] = extra + os.pathsep + env.get("PATH", "")
        self.log = open(self.folder / "qt.log", "w")
        self.proc = subprocess.Popen([str(exe), *map(str, args)], cwd=self.folder, env=env,
                                     stdout=self.log, stderr=subprocess.STDOUT)

    def cmd(self, *lines, timeout=20):
        out = self.auto / "out"
        out.unlink(missing_ok=True)
        tmp = self.auto / "cmd.tmp"
        tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
        os.replace(tmp, self.auto / "cmd")
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.proc.poll() is not None:
                raise AssertionError(f"melonDS exited ({self.proc.returncode}): {self.output()}")
            if out.exists():
                return out.read_text(encoding="utf-8").splitlines()
            time.sleep(0.05)
        raise AssertionError(f"no automation reply to {lines}: {self.output()}")

    def wait_for(self, pred, *lines, timeout=20):
        end = time.monotonic() + timeout
        last = None
        while time.monotonic() < end:
            last = self.cmd(*lines)
            if pred(last):
                return last
            time.sleep(0.1)
        raise AssertionError(f"{lines}: last reply {last}")

    def output(self):
        self.log.flush()
        return (self.folder / "qt.log").read_text(errors="replace")[-3000:]

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.cmd("quit", timeout=5)
            except AssertionError:
                pass
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(10)
        self.log.close()


@unittest.skipUnless(QT_EXE and have_library(), "set MELONDS_QT_AUTOMATION_EXE (a -DMELONDS_AUTOMATION=ON Qt build) "
                                                "and the headless library")
class QtStartupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.rom = root / "synthetic.nds"
        make_rom(cls.rom)
        cls.state = root / "marked.mln"
        c = native.Console()
        try:
            c.load_rom(str(cls.rom), save=str(root / "headless.sav"))
            c.reset()
            c.run(5)
            c.write(MARKER_ADDR, MARKER.to_bytes(4, "little"))
            c.write(LOOP, (0xEAFFFFFE).to_bytes(4, "little"))  # b .
            c.set_reg("pc", LOOP)
            c.run(1)
            c.save_state(str(cls.state))
        finally:
            c.close()
        cls.symbols = root / "syms.txt"
        cls.symbols.write_text(f"{BP1:08x} start_sym 64\n{BP3:08x} third_sym 64\n", encoding="utf-8")
        cls.labels = root / "labels.txt"
        cls.labels.write_text(f"{BP2:08x} second_label\n", encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def launch(self, *args):
        folder = tempfile.mkdtemp(dir=self.tmp.name)
        qt = Qt(folder, [*args, self.rom])
        self.addCleanup(qt.stop)
        return qt

    def test_state_breakpoints_and_labels_at_startup(self):
        qt = self.launch("--symbols", self.symbols, "--labels", self.labels, "--state", self.state,
                         "--break", f"0x{BP1:08x}", "--break", "second_label", "--break", "third_sym", "--pause")
        points = qt.wait_for(lambda r: "02100080" in " ".join(r).lower(), "points")
        addrs = sorted(line.split()[1].lower() for line in points if line.strip())
        self.assertEqual(addrs, [f"{BP1:08x}", f"{BP2:08x}", f"{BP3:08x}"], points)
        self.assertEqual([v.lower() for v in qt.cmd(f"peek {MARKER_ADDR:08x}")], [f"{MARKER:08x}"],
                         "the state was not loaded")
        self.assertIn("running=0", qt.cmd("state")[0], "--pause should leave the console paused")
        self.assertEqual(qt.cmd(f"symname {BP2:08x}"), ["second_label"])

    def test_breakpoints_hit_after_unpause(self):
        # the state's ARM9 spins on LOOP, so a breakpoint there is hit as soon as the console runs
        qt = self.launch("--state", self.state, "--break", f"0x{LOOP:08x}")
        r = qt.wait_for(lambda r: "broken=1" in r[0], "state")
        self.assertIn("broken=1", r[0])

    def test_bad_break_is_reported_not_fatal(self):
        qt = self.launch("--break", "no_such_symbol", "--break", "0x02100000", "--pause")
        points = qt.wait_for(lambda r: any("02100000" in line for line in r), "points")
        self.assertEqual([line.split()[1].lower() for line in points if line.strip()], ["02100000"])
        self.assertIn("no_such_symbol", qt.output())

    def test_missing_state_is_reported(self):
        qt = self.launch("--state", Path(self.tmp.name) / "missing.mln", "--pause")
        qt.wait_for(lambda r: "console=1" in r[0], "state")
        self.assertIn("missing.mln", qt.output())


if __name__ == "__main__":
    unittest.main()
