"""Local-only update HTTP fixtures. Never starts an installer or visible window."""
from pathlib import Path
import argparse
import base64
import ctypes
import hashlib
import http.server
import json
import os
import socket
import subprocess
import threading
import time

PAYLOAD = b"MZ-CGPlay-update-contract\n" * 4096
VERSION = "1.0.7.12"
NAME = f"CGPlay_Setup_{VERSION}_full.exe"
RELEASE = {"draft": False, "prerelease": False, "tag_name": f"v{VERSION}", "body": "Release notes",
           "assets": [{"name": NAME, "state": "uploaded", "size": len(PAYLOAD),
                       "digest": "sha256:" + hashlib.sha256(PAYLOAD).hexdigest(),
                       "browser_download_url": f"https://github.com/xty-luoye/CGPlay/releases/download/v{VERSION}/{NAME}"}]}


def visible_windows(pid):
    count = 0
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32 = ctypes.windll.user32
    user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
    user32.EnumWindows.argtypes = [callback_type, ctypes.c_void_p]

    @callback_type
    def visit(window, _):
        nonlocal count
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(window):
            count += 1
        return True

    user32.EnumWindows(visit, None)
    return count


def script_contracts(out):
    """Execute the production script with mocked process/hash/launch commands."""
    if os.name != "nt":
        return []
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    script = (out / "installer_launch_script.ps1").read_text(encoding="utf-8")
    exe = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    checks = []
    for case in ("timeout", "hash-mismatch", "success"):
        marker = out / f"script_{case}_mock_launch.json"
        marker.unlink(missing_ok=True)
        actual_hash = "0" * 64 if case == "hash-mismatch" else hashlib.sha256(PAYLOAD).hexdigest().upper()
        wait = "throw 'simulated timeout'" if case == "timeout" else "return"
        mocks = f"""
function Get-Process {{ [CmdletBinding()] param([int]$Id) [pscustomobject]@{{Id=$Id}} }}
function Wait-Process {{ [CmdletBinding()] param([Parameter(ValueFromPipeline=$true)]$InputObject,[int]$Timeout) {wait} }}
function Get-FileHash {{ [CmdletBinding()] param([string]$LiteralPath,[string]$Algorithm) [pscustomobject]@{{Hash='{actual_hash}'}} }}
function Start-Process {{ [CmdletBinding()] param([string]$FilePath) @{{mock=$true;path=$FilePath}} | ConvertTo-Json -Compress | Set-Content -LiteralPath {quote(marker)} -Encoding UTF8 }}
"""
        code = base64.b64encode((mocks + script).encode("utf-16-le")).decode("ascii")
        run = subprocess.Popen([str(exe), "-NoProfile", "-NonInteractive", "-EncodedCommand", code],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        stop = threading.Event()
        max_visible = [0]

        def monitor():
            while not stop.is_set():
                max_visible[0] = max(max_visible[0], visible_windows(run.pid))
                stop.wait(0.005)

        watcher = threading.Thread(target=monitor)
        watcher.start()
        try:
            _, stderr = run.communicate(timeout=10)
        finally:
            stop.set()
            watcher.join()
        (out / f"script_{case}.stderr.log").write_bytes(stderr)
        expected_path = str(out / "path with spaces and 'quote' & $(expression)/installer.exe")
        launched = json.loads(marker.read_text(encoding="utf-8-sig")) if marker.exists() else None
        passed = (run.returncode == 0 and launched == {"mock": True, "path": expected_path}) if case == "success" else (run.returncode == 1 and launched is None)
        checks.append({"name": "PowerShell mocked " + case, "pass": passed and max_visible[0] == 0, "exitCode": run.returncode,
                       "realInstallerLaunched": False, "background": True, "visiblePlatformWindows": max_visible[0]})
    return checks


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        try:
            if self.path.startswith("/redirect"):
                self.send_response(302)
                self.send_header("Location", "/redirect-loop" if self.path == "/redirect-loop" else "/installer")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            body = {"/release": json.dumps(RELEASE).encode(), "/installer": PAYLOAD,
                    "/corrupt": b"X" + PAYLOAD[1:], "/oversize": PAYLOAD + b"overflow",
                    "/truncate": PAYLOAD[:100], "/slow": PAYLOAD,
                    "/metadata-large": b"x" * (1024 * 1024 + 1)}.get(self.path)
            if body is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(PAYLOAD) if self.path == "/truncate" else len(body)))
            self.end_headers()
            if self.path == "/slow":
                self.wfile.write(body[:100])
                self.wfile.flush()
                time.sleep(2)
                self.wfile.write(body[100:])
            else:
                self.wfile.write(body)
            if self.path == "/truncate":
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qt-bin", required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = os.environ.copy()
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env.get("PATH", "")
    try:
        run = subprocess.run([str(args.exe.resolve()), f"http://127.0.0.1:{server.server_port}", str(out)],
                             cwd=out, env=env, capture_output=True, timeout=45,
                             creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        (out / "stdout.log").write_bytes(run.stdout)
        (out / "stderr.log").write_bytes(run.stderr)
        report = json.loads((out / "report.json").read_text(encoding="utf-8"))
        script_checks = script_contracts(out)
        report["scriptExecutionContracts"] = script_checks
        report["passed"] += sum(row["pass"] for row in script_checks)
        report["failed"] += sum(not row["pass"] for row in script_checks)
        (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        summary = {key: report[key] for key in ("background", "visiblePlatformWindows", "passed", "failed", "installerLaunchExecuted")}
        summary["exitCode"] = run.returncode
        print(json.dumps(summary))
        for row in report["results"] + script_checks:
            if not row["pass"]:
                print(json.dumps(row, ensure_ascii=False))
        return run.returncode or int(report["failed"] > 0)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
