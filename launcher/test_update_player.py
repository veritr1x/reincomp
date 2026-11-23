"""Regression tests for building a portable update in the launcher or a standalone browser."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import queue
import sys
import tempfile
from threading import Event
import time
import unittest
from unittest.mock import patch
import urllib.request
from urllib.parse import urlsplit

from update_player import browser_job, rebuild


class LauncherWebViewTest(unittest.TestCase):
    def test_existing_webview_builds_without_edge(self):
        result = b"portable update"
        digest = hashlib.sha256(result).hexdigest()
        kit = json.dumps(dict(bunkerVersion="1.2.3", preparedRuntimeFormat=1, files={})).encode()
        manifest = json.dumps(dict(sourceExeSHA256=digest, kitSHA256=hashlib.sha256(kit).hexdigest(), parts=[])).encode()
        files = {"windows/runtime/runtime.json": manifest, "windows/kit.json": kit, "bundles/notices.json": b'{"parts":[]}'}
        origin = "http://127.0.0.1:43210"
        pages = queue.Queue()

        def progress(line, **kwargs):
            if line.startswith("UPDATE_PAGE "):
                pages.put(line.removeprefix("UPDATE_PAGE "))

        cancel = Event()
        with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(max_workers=1) as pool, \
                patch("update_player.edge_path", side_effect=AssertionError("looked for Edge")), \
                patch("update_player.fetch", side_effect=lambda base, name, *args: files[name]), \
                patch("builtins.print", side_effect=progress):
            output = Path(tmp) / "player.exe"
            build = pool.submit(rebuild, Path(tmp), output, "1.2.3", hashlib.sha256(manifest).hexdigest(),
                                cancel=cancel, expected_exe=digest, launcher_origin=origin)
            try:
                page = pages.get(timeout=5)
                with urllib.request.urlopen(page, timeout=5) as response:
                    self.assertIn("frame-ancestors " + origin, response.headers["Content-Security-Policy"])
                    self.assertIn(b"start.js", response.read())
                with urllib.request.urlopen(page + "build.mjs", timeout=5) as response:
                    self.assertIn(b"compileWindows", response.read())
                request = urllib.request.Request(page + "result", data=result,
                                                 headers={"Origin": "http://" + urlsplit(page).netloc})
                with urllib.request.urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                self.assertEqual(build.result(timeout=5), digest)
                self.assertEqual(output.read_bytes(), result)
            finally:
                cancel.set()


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
