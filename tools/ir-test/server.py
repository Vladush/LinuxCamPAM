#!/usr/bin/env python3
"""Local test page for the LinuxCamPAM IR camera.

Live feed and face test, as your normal user:

    python3 tools/ir-test/server.py

Adds live scores from the real login code (build once with build.sh first):

    tools/ir-test/build.sh
    sudo python3 tools/ir-test/server.py

With sudo, only the live_scan program stays root, because enrolled faces are
root-only. This server drops to your user before it handles any request.

Browsers generally can't open GREY-only IR cameras, so ffmpeg reads the device
and this server passes the raw frames to the page. The page holds the camera
while the feed runs, which makes real face logins fail, so close the tab or press
Stop when you're done. Listens on localhost only and needs the printed token.
"""

import argparse
import json
import os
import pwd
import re
import secrets
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

HERE = Path(__file__).resolve().parent
PAGE = HERE / "index.html"
LIVE_SCAN = HERE / "live_scan"
CHUNK_BYTES = 65536
TEST_TIMEOUT_SEC = 30
SCAN_TIMEOUT_SEC = 30
JOURNAL_FLUSH_SEC = 0.3
# Daemon log lines worth showing next to a test result.
TEST_LOG_LINE = re.compile(
    r"Camera |Mandatory|locked out|timed out|timeout|No cameras|Profiling: Detection|Multiple faces"
)


class CameraOwner:
    """Makes sure the feed and anything else using the cameras never overlap."""

    def __init__(self, device, size):
        self.device = device
        self.size = size
        self.lock = threading.Lock()
        self.proc = None
        self.busy = False

    def _stop_locked(self):
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def open_stream(self):
        """Start ffmpeg, replacing any older stream. Returns None while busy."""
        with self.lock:
            if self.busy:
                return None
            self._stop_locked()
            errors = tempfile.TemporaryFile()
            self.proc = subprocess.Popen(
                [
                    "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
                    "-f", "v4l2", "-input_format", "gray",
                    "-video_size", self.size, "-framerate", "30", "-i", self.device,
                    # Passthrough keeps ffmpeg from duplicating or dropping frames,
                    # which would distort the emitter's strobe pattern.
                    "-fps_mode", "passthrough",
                    "-f", "rawvideo", "-pix_fmt", "gray", "-",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=errors,
                bufsize=0,
            )
            return self.proc, errors

    def release(self, proc):
        with self.lock:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            if self.proc is proc:
                self.proc = None

    def stop(self):
        with self.lock:
            self._stop_locked()

    def run_exclusive(self, action):
        """Stop the feed and run action. Returns None if something else is running."""
        with self.lock:
            if self.busy:
                return None
            self.busy = True
            self._stop_locked()
        try:
            return action()
        finally:
            with self.lock:
                self.busy = False


class LiveScanner:
    """Asks the root live_scan process, started before dropping privileges, to scan."""

    def __init__(self, proc):
        self.proc = proc
        self.failed = None

    @staticmethod
    def _failure(reason):
        return json.dumps({"would_pass": False, "reason": reason, "cameras": []}).encode()

    def scan(self):
        if self.failed:
            return self._failure(self.failed)
        try:
            self.proc.stdin.write(b"scan\n")
            self.proc.stdin.flush()
            ready, _, _ = select.select([self.proc.stdout], [], [], SCAN_TIMEOUT_SEC)
            line = self.proc.stdout.readline() if ready else b""
        except (BrokenPipeError, ValueError):
            line = b""
        if line:
            return line
        # It's root and this process isn't, so closing its stdin is how it gets stopped.
        self.close()
        self.failed = "live_scan stopped responding. Restart server.py."
        return self._failure(self.failed)

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass


def run_linuxcampam_test():
    started = int(time.time())
    try:
        result = subprocess.run(
            ["linuxcampam", "test"],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=TEST_TIMEOUT_SEC,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "response": f"linuxcampam test timed out after {TEST_TIMEOUT_SEC}s", "log": []}

    time.sleep(JOURNAL_FLUSH_SEC)
    # Syslog entries only: the daemon's buffered stdout can reach the journal late,
    # carrying old lines with a new journal timestamp.
    journal = subprocess.run(
        ["journalctl", "-u", "linuxcampam.service", "_TRANSPORT=syslog", "--since", f"@{started}",
         "--no-pager", "-q", "-o", "cat"],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
    )
    response = (result.stdout + result.stderr).strip()
    return {
        "ok": "AUTH_SUCCESS" in response,
        "response": response,
        "log": [line for line in journal.stdout.splitlines() if TEST_LOG_LINE.search(line)],
    }


def make_handler(camera, token, info, scanner):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # Request lines contain the token.

        def _authorized(self):
            supplied = parse_qs(urlsplit(self.path).query).get("t", [""])[0]
            if secrets.compare_digest(supplied.encode(), token.encode()):
                return True
            self._send(403, "text/plain", b"Use the full URL printed by server.py\n")
            return False

        def _send(self, status, content_type, body):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _send_json(self, status, payload):
            self._send(status, "application/json", json.dumps(payload).encode())

        def do_GET(self):
            if not self._authorized():
                return
            path = urlsplit(self.path).path
            if path == "/":
                self._send(200, "text/html; charset=utf-8", PAGE.read_bytes())
            elif path == "/api/info":
                self._send_json(200, info)
            elif path == "/api/stream":
                self._stream()
            else:
                self._send(404, "text/plain", b"Not found\n")

        def do_POST(self):
            if not self._authorized():
                return
            path = urlsplit(self.path).path
            if path == "/api/test":
                result = camera.run_exclusive(run_linuxcampam_test)
                if result is None:
                    self._send(409, "text/plain", b"The cameras are busy\n")
                else:
                    self._send_json(200, result)
            elif path == "/api/scan" and scanner is not None:
                result = camera.run_exclusive(scanner.scan)
                if result is None:
                    self._send(409, "text/plain", b"The cameras are busy\n")
                else:
                    self._send(200, "application/json", result)
            else:
                self._send(404, "text/plain", b"Not found\n")

        def _stream(self):
            opened = camera.open_stream()
            if opened is None:
                self._send(409, "text/plain", b"The cameras are busy\n")
                return
            proc, errors = opened
            try:
                chunk = proc.stdout.read(CHUNK_BYTES)
                if not chunk:
                    # Killed by a newer stream or a test, or ffmpeg couldn't open the camera.
                    proc.wait()
                    errors.seek(0)
                    message = errors.read().strip() or b"Camera stream ended"
                    self._send(503, "text/plain", message)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                while chunk:
                    self.wfile.write(chunk)
                    chunk = proc.stdout.read(CHUNK_BYTES)
            except (BrokenPipeError, ConnectionResetError):
                pass  # The tab closed or pressed Stop.
            finally:
                camera.release(proc)
                proc.stdout.close()
                errors.close()

    return Handler


def start_live_scan(user, config):
    """Start the one process that stays root. It exits when its stdin closes."""
    command = [str(LIVE_SCAN), user, config]
    # The service starts the daemon through this wrapper, which sets up OpenCL.
    wrapper = shutil.which("detect_opencl.sh")
    if wrapper:
        command.insert(0, wrapper)
    return subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        # The IR emitter's saved config is in root's home, as it is for the daemon.
        env={**os.environ, "HOME": "/root"},
    )


def drop_privileges():
    account = pwd.getpwuid(int(os.environ["SUDO_UID"]))
    os.initgroups(account.pw_name, account.pw_gid)
    os.setgid(account.pw_gid)
    os.setuid(account.pw_uid)
    os.environ.update(HOME=account.pw_dir, USER=account.pw_name, LOGNAME=account.pw_name)


def main():
    parser = argparse.ArgumentParser(description="Local test page for the LinuxCamPAM IR camera.")
    parser.add_argument("--device", default="/dev/video2", help="IR camera device (default: %(default)s)")
    parser.add_argument("--size", default="640x360", help="capture size, WIDTHxHEIGHT (default: %(default)s)")
    parser.add_argument("--port", type=int, default=8765, help="localhost port (default: %(default)s)")
    parser.add_argument("--no-open", action="store_true", help="don't open a browser")
    parser.add_argument("--user", help="enrolled user for live scores (default: the user who ran sudo)")
    parser.add_argument("--config", default="/etc/linuxcampam/config.ini",
                        help="config for live scores (default: %(default)s)")
    args = parser.parse_args()

    if not re.fullmatch(r"\d+x\d+", args.size):
        parser.error("--size must look like 640x360")
    for tool in ("ffmpeg", "linuxcampam", "journalctl"):
        if shutil.which(tool) is None:
            sys.exit(f"Missing required command: {tool}")

    scan_process = None
    started_with_sudo = os.geteuid() == 0
    if started_with_sudo:
        if "SUDO_UID" not in os.environ:
            sys.exit("Start this with sudo from your normal account, not from a root shell.")
        if LIVE_SCAN.exists():
            scan_process = start_live_scan(args.user or os.environ["SUDO_USER"], args.config)
        else:
            print(f"Live scores are off until you run {HERE / 'build.sh'}", file=sys.stderr)
        drop_privileges()

    scanner = LiveScanner(scan_process) if scan_process else None
    width, height = (int(value) for value in args.size.split("x"))
    camera = CameraOwner(args.device, args.size)
    token = secrets.token_urlsafe(24)
    account = pwd.getpwuid(os.getuid()).pw_name
    info = {
        "device": args.device,
        "width": width,
        "height": height,
        "user": account,
        "live": scanner is not None,
        "live_user": (args.user or account) if scanner else None,
        "config": args.config,
    }

    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(camera, token, info, scanner))
    url = f"http://127.0.0.1:{args.port}/?t={token}"
    print(f"IR test page: {url}\nPress Ctrl+C to stop.", flush=True)
    if not args.no_open and not started_with_sudo:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        camera.stop()
        if scanner:
            scanner.close()
        server.server_close()


if __name__ == "__main__":
    main()
