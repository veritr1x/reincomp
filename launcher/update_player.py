"""Rebuild a portable update with Bunker's published browser compiler and local inputs."""
import contextlib
import hashlib
import hmac
import http.server
import json
import mimetypes
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request


SITE = "https://veritr1x.github.io/bunker/"
SHA = re.compile(r"[0-9a-f]{64}")
LIMIT = 4 * 1024**3
CHUNK = 1024**2


def fetch(base, name, limit, digest=None):
    with urllib.request.urlopen(base + name, timeout=90) as response:
        data = response.read(limit + 1)
    if len(data) > limit or (digest and hashlib.sha256(data).hexdigest() != digest):
        raise ValueError("Release file failed verification: " + name)
    return data


def edge_path():
    for variable in ("ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"):
        edge = Path(os.environ.get(variable, "")) / "Microsoft/Edge/Application/msedge.exe"
        if edge.is_file():
            return edge
    raise FileNotFoundError("Install Microsoft Edge to build this update.")


@contextlib.contextmanager
def browser_job(args):
    """Own only this build's Edge process tree, including a relaunched browser."""
    import _winapi
    import ctypes as c
    from ctypes import wintypes as w

    class BasicLimits(c.Structure):
        _fields_ = [("time_limits", c.c_int64 * 2), ("flags", w.DWORD),
                    ("working_set_limits", c.c_size_t * 2), ("active_limit", w.DWORD),
                    ("affinity", c.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]

    class ExtendedLimits(c.Structure):
        _fields_ = [("basic", BasicLimits), ("io_counters", c.c_uint64 * 6),
                    ("memory_limits", c.c_size_t * 4)]

    class BasicAccounting(c.Structure):
        _fields_ = [("times", c.c_int64 * 4), ("page_faults", w.DWORD), ("total", w.DWORD),
                    ("active", w.DWORD), ("terminated", w.DWORD)]

    kernel = c.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.restype = w.HANDLE
    job = w.HANDLE(kernel.CreateJobObjectW(None, None))
    if not job:
        raise c.WinError(c.get_last_error())

    def active():
        info = BasicAccounting()
        if not kernel.QueryInformationJobObject(job, 1, c.byref(info), c.sizeof(info), None):
            raise c.WinError(c.get_last_error())
        return info.active != 0

    try:
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)):
            raise c.WinError(c.get_last_error())
        # Assign Edge before its first thread can spawn another process and exit.
        process, thread, _, _ = _winapi.CreateProcess(str(args[0]), subprocess.list2cmdline(args),
            None, None, False, 0x4 | subprocess.CREATE_NO_WINDOW, None, None, subprocess.STARTUPINFO())
        try:
            if not kernel.AssignProcessToJobObject(job, w.HANDLE(process)):
                error = c.WinError(c.get_last_error())
                _winapi.TerminateProcess(process, 1)
                raise error
            if kernel.ResumeThread(w.HANDLE(thread)) == -1:
                raise c.WinError(c.get_last_error())
        finally:
            _winapi.CloseHandle(thread)
            _winapi.CloseHandle(process)
        try:
            yield active
        finally:
            if not kernel.TerminateJobObject(job, 0):
                raise c.WinError(c.get_last_error())
            # Let the browser release its private profile before removing it.
            deadline = time.monotonic() + 5
            while active() and time.monotonic() < deadline:
                time.sleep(.05)
    finally:
        _winapi.CloseHandle(job.value)


def rebuild(root, output, version, manifest_hash, base=SITE, cancel=None, expected_exe=None, launcher_origin=None):
    if launcher_origin is not None and not re.fullmatch(r"http://127\.0\.0\.1:[0-9]{1,5}", launcher_origin):
        raise ValueError("Invalid launcher origin")
    if not SHA.fullmatch(manifest_hash):
        raise ValueError("Invalid Windows runtime digest")
    runtime_bytes = fetch(base, "windows/runtime/runtime.json", CHUNK, manifest_hash)
    runtime = json.loads(runtime_bytes)
    expected = runtime["sourceExeSHA256"]
    if expected_exe is not None and expected != expected_exe:
        raise ValueError("The Windows runtime does not match the published reincomp player")
    if not SHA.fullmatch(expected) or not SHA.fullmatch(runtime["kitSHA256"]):
        raise ValueError("Invalid Windows release manifest")
    kit_bytes = fetch(base, "windows/kit.json", CHUNK, runtime["kitSHA256"])
    kit = json.loads(kit_bytes)
    if kit["bunkerVersion"] != version or kit.get("preparedRuntimeFormat") != 1:
        raise ValueError("The Windows kit does not match this release")
    files = {"windows/" + key: digest for key, digest in kit["files"].items()
             if re.fullmatch(r"[\w.+/-]+", key) and ".." not in key.split("/") and SHA.fullmatch(digest)}
    for i, part in enumerate(runtime["parts"], 1):
        if not SHA.fullmatch(part["sha256"]):
            raise ValueError("Invalid runtime part checksum")
        files[f"windows/runtime/runtime.part{i:03}"] = part["sha256"]
    notices = fetch(base, "bundles/notices.json", CHUNK)
    notice_parts = {"bundles/" + p for p in json.loads(notices)["parts"]
                    if re.fullmatch(r"notices\.zip(?:\.\d+)?", p)}
    memory = {"windows/runtime/runtime.json": runtime_bytes, "windows/kit.json": kit_bytes,
              "bundles/notices.json": notices}
    inputs = {"apk": root / "data/game-sound.apk", "master": root / "data/original-master.bin.e"}
    prefix = "/" + secrets.token_hex(24) + "/"
    worker = b"""
import {compileWindows} from './windows/compiler.mjs';
const read = async name => { const r=await fetch(name); if(!r.ok)throw Error('Could not read '+name); return r.blob(); };
try {
  const blob=await compileWindows({apk:await read('apk'),master:await read('master'),
    preparedRuntime:HASH,log:value=>fetch('progress',{method:'POST',body:value})});
  const r=await fetch('result',{method:'POST',body:blob});
  if(!r.ok)throw Error(await r.text());
} catch(e) { await fetch('failed',{method:'POST',body:String(e.message||e)}); }
""".replace(b"HASH", json.dumps(manifest_hash).encode())
    memory.update({"": b'<script src="start.js"></script>', "build.mjs": worker,
                   "start.js": b"new Worker('./build.mjs',{type:'module'}).onerror=e=>fetch('failed',{method:'POST',body:e.message});"})
    finished = threading.Event()
    worker_loaded = threading.Event()
    failure = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, length, mime="text/plain"):
            self.send_response(code)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(length))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Security-Policy", "default-src 'none'; script-src 'self' 'unsafe-eval'; "
                             "worker-src 'self' blob:; connect-src 'self'; frame-ancestors " + (launcher_origin or "'none'"))
            self.end_headers()

        def route(self):
            if self.headers.get("Host") == authority and self.path.startswith(prefix):
                return self.path[len(prefix):]
            return None

        def do_GET(self):
            key = self.route()
            if key == "build.mjs":
                worker_loaded.set()
            if key not in inputs and key not in memory and key not in files and key not in notice_parts:
                self.reply(404, 0)
                return
            try:
                if key in inputs:
                    with inputs[key].open("rb") as src:
                        self.reply(200, os.fstat(src.fileno()).st_size, "application/octet-stream")
                        shutil.copyfileobj(src, self.wfile, CHUNK)
                    return
                body = memory[key] if key in memory else fetch(base, key, 128 * CHUNK, files.get(key))
                mime = ("application/javascript" if key.endswith((".js", ".mjs")) else
                        "text/html" if not key else mimetypes.guess_type(key)[0] or "application/octet-stream")
                self.reply(200, len(body), mime)
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                return
            except Exception as err:
                failure.append(str(err))
                finished.set()

        def do_POST(self):
            key = self.route()
            if key not in ("progress", "failed", "result") or self.headers.get("Origin") != "http://" + authority:
                self.reply(403, 0)
                return
            try:
                size = int(self.headers.get("Content-Length", "-1"))
                if size < 0 or size > (LIMIT if key == "result" else 8192):
                    raise ValueError("Invalid update response size")
                if key == "result":
                    digest = hashlib.sha256()
                    with output.open("xb") as out:
                        while size:
                            block = self.rfile.read(min(size, CHUNK))
                            if not block:
                                raise ValueError("Incomplete Windows update")
                            size -= len(block)
                            digest.update(block)
                            out.write(block)
                        out.flush()
                        os.fsync(out.fileno())
                    if not hmac.compare_digest(digest.hexdigest(), expected):
                        raise ValueError("The rebuilt executable differs from the published release")
                else:
                    message = self.rfile.read(size).decode("utf-8", "replace")
                    if key == "failed":
                        raise ValueError(message)
                    print(message, flush=True)
                self.reply(200, 0)
            except Exception as err:
                failure.append(str(err))
                self.reply(400, 0)
            finally:
                if key != "progress" or failure:
                    finished.set()

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        authority = "127.0.0.1:" + str(server.server_port)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            url = "http://" + authority + prefix
            with contextlib.ExitStack() as stack:
                browser_active = None
                if launcher_origin:
                    print("UPDATE_PAGE " + url, flush=True)
                else:
                    # Standalone developer builds have no launcher window.
                    profile = stack.enter_context(tempfile.TemporaryDirectory(prefix="bunker-update-edge-", ignore_cleanup_errors=True))
                    browser_active = stack.enter_context(browser_job([
                        edge_path(), "--headless=new", "--no-first-run", "--disable-extensions",
                        "--disable-background-networking", "--disable-sync", "--no-proxy-server",
                        "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1", "--user-data-dir=" + profile, url]))
                start = time.monotonic()
                while not finished.wait(.5):
                    if cancel and cancel.is_set():
                        raise RuntimeError("Update canceled")
                    if launcher_origin and not worker_loaded.is_set() and time.monotonic() - start > 90:
                        raise RuntimeError("The launcher could not open the update page. Reopen the launcher and try again.")
                    if (browser_active and not browser_active()) or time.monotonic() - start > 1800:
                        raise RuntimeError("The local update build did not finish")
                if failure:
                    raise RuntimeError(failure[0])
        finally:
            server.shutdown()
    return expected


def apply_update(stage, exe, digest, parent, args):
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x100000, False, parent)
    if handle:
        try:
            if kernel.WaitForSingleObject(handle, 120000) != 0:
                raise TimeoutError("The previous game is still open")
        finally:
            kernel.CloseHandle(handle)
    with stage.open("rb") as f:
        if hashlib.file_digest(f, "sha256").hexdigest() != digest:
            raise ValueError("The staged game update changed")
    os.replace(stage, exe)
    if stage.parent.parent == exe.parent and stage.parent.name.startswith(".bunker-update-"):
        with contextlib.suppress(OSError):
            stage.parent.rmdir()
    subprocess.Popen([exe, *args], cwd=exe.parent)


if __name__ == "__main__":
    mode, root, *args = sys.argv[1:]
    root = Path(root)
    try:
        if mode == "build":
            cancel = threading.Event()
            def watch_launcher():
                os.read(0, 1)
                cancel.set()
            threading.Thread(target=watch_launcher, daemon=True).start()
            print("VERIFIED " + rebuild(root, Path(args[0]), args[1], args[2], cancel=cancel,
                                        expected_exe=args[3], launcher_origin=args[4]), flush=True)
        elif mode == "apply":
            apply_update(Path(args[0]), Path(args[1]), args[2], int(args[3]), args[4:])
        else:
            raise ValueError("Unknown update operation")
    except Exception as error:
        if mode == "apply":
            (root / "data/update-error.txt").write_text(str(error), encoding="utf-8")
        sys.exit(str(error))
