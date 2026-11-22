"""Windows regression tests for the browser used to build a portable update."""
from pathlib import Path
import sys
import tempfile
import time
import unittest

from update_player import browser_job


@unittest.skipUnless(sys.platform == "win32", "requires Windows job objects")
class BrowserJobTest(unittest.TestCase):
    def test_edge_handoff_and_cleanup(self):
        import _winapi

        with tempfile.TemporaryDirectory() as folder:
            ready = Path(folder) / "child.pid"
            # The browser stub exits immediately while its replacement keeps building.
            child = ("import os, sys, time; from pathlib import Path; "
                     "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)")
            parent_pid = Path(folder) / "stub.pid"
            stub = ("import os, subprocess, sys; from pathlib import Path; "
                    "Path(sys.argv[1]).write_text(str(os.getpid())); "
                    "subprocess.Popen([sys.executable, '-c', *sys.argv[2:]])")
            with browser_job([sys.executable, "-c", stub, str(parent_pid), child, str(ready)]) as active:
                deadline = time.monotonic() + 10
                while not ready.exists() or not ready.stat().st_size:
                    self.assertLess(time.monotonic(), deadline, "replacement browser did not start")
                    time.sleep(.05)
                try:
                    parent = _winapi.OpenProcess(0x100000, False, int(parent_pid.read_text()))
                except OSError as error:
                    if error.winerror != 87:  # The exited PID no longer exists.
                        raise
                else:
                    self.addCleanup(_winapi.CloseHandle, parent)
                    self.assertEqual(_winapi.WaitForSingleObject(parent, 5000), _winapi.WAIT_OBJECT_0)
                self.assertTrue(active())
                handle = _winapi.OpenProcess(0x100000, False, int(ready.read_text()))
                self.addCleanup(_winapi.CloseHandle, handle)
            self.assertEqual(_winapi.WaitForSingleObject(handle, 5000), _winapi.WAIT_OBJECT_0)

    def test_process_tree_exited_before_result(self):
        with browser_job([sys.executable, "-c", "pass"]) as active:
            deadline = time.monotonic() + 10
            while active():
                self.assertLess(time.monotonic(), deadline, "exited browser still reported active")
                time.sleep(.05)


if __name__ == "__main__":
    unittest.main()
