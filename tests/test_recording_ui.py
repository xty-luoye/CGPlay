"""Real hidden application recording UI + private OBS synthetic end-to-end test.

No real display, window pixels, microphone or loopback audio are captured.
CGPLAY_RECORDER_ROOT is forwarded only as an explicit background-smoke CLI input;
ordinary application startup never accepts it as a recording runtime fallback.
"""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = Path(os.environ.get("CGPLAY_RECORDING_UI_ARTIFACTS",
                              ROOT / "tests/artifacts/migration_1_0_8_20261025/recording_ui_validation"))


def visible_pids():
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    values = []
    @callback_type
    def callback(window, unused):
        if user.IsWindowVisible(window):
            pid = wintypes.DWORD(); user.GetWindowThreadProcessId(window, ctypes.byref(pid)); values.append(pid.value)
        return True
    user.EnumWindows(callback, 0)
    return values


def descendant_pids(pid):
    class Entry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("uses", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("name", wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    values = []; entry = Entry(); entry.size = ctypes.sizeof(entry)
    try:
        valid = kernel.Process32FirstW(handle, ctypes.byref(entry))
        while valid:
            values.append((entry.pid, entry.parent)); valid = kernel.Process32NextW(handle, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(handle)
    known = {pid}
    for _ in range(len(values)):
        added = {child for child, parent in values if parent in known} - known
        if not added: break
        known.update(added)
    return known


def terminate_test_tree(process):
    if process.poll() is not None:
        return
    # Only the process created by this test and its descendants; never by name.
    subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32/taskkill.exe"),
                    "/PID", str(process.pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW, timeout=10)
    if process.poll() is None:
        process.kill()
    process.wait(timeout=10)


class RecordingUiAcceptance(unittest.TestCase):
    def test_real_application_synthetic_record_pause_recover_and_close(self):
        self.assertEqual(os.name, "nt", "This real application recording acceptance requires Windows")
        app = Path(os.environ.get("CGPLAY_APP", ROOT / "build_win_full/bin/Release/CGPlay.exe"))
        self.assertTrue(app.is_file(), "Build the current player before running UI acceptance")
        directory = ARTIFACTS / str(time.time_ns()); directory.mkdir(parents=True)
        report_path = directory / "report.json"
        command = [str(app), "--recording-smoke", "--recording-smoke-output", str(report_path),
                   "--automation-background", "--automation-settings-namespace", "recording-ui-" + directory.name]
        recorder_root = os.environ.get("CGPLAY_RECORDER_ROOT", "").strip()
        if recorder_root:
            command += ["--recording-smoke-runtime", str(Path(recorder_root).resolve())]
        started = time.monotonic(); known = set(); maximum = 0; before = len(visible_pids()); timed_out = False
        with (directory / "application.log").open("wb") as output:
            process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            known.add(process.pid)
            try:
                while process.poll() is None:
                    known.update(descendant_pids(process.pid))
                    maximum = max(maximum, sum(pid in known for pid in visible_pids()))
                    if time.monotonic() - started > 210:
                        timed_out = True; terminate_test_tree(process); break
                    time.sleep(.05)
            finally:
                if process.poll() is None:
                    terminate_test_tree(process)
        survivors = sorted(descendant_pids(process.pid) - {process.pid})
        outer = {"backgroundMode": True, "visibleAutomationWindowsMax": maximum,
                 "platformVisibleTopLevelWindowsBefore": before, "platformVisibleTopLevelWindowsAfter": len(visible_pids()),
                 "exitCode": process.returncode, "elapsedSeconds": round(time.monotonic()-started, 2),
                 "command": command, "report": str(report_path), "timedOut": timed_out, "survivingChildPids": survivors}
        (directory / "runner.json").write_text(json.dumps(outer, indent=2), encoding="utf-8")
        self.assertEqual(maximum, 0, "Automated recording must never expose an application window")
        self.assertFalse(timed_out, "Recording UI process exceeded its finite acceptance deadline")
        self.assertEqual(survivors, [], "Recording UI exited before its child processes")
        self.assertEqual(process.returncode, 0, str(directory / "application.log"))
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertTrue(report["backgroundMode"])
        self.assertEqual(report["visibleAutomationWindowsMax"], 0)
        self.assertTrue(report["passed"], json.dumps(report, ensure_ascii=False))
        self.assertEqual(report["summary"]["fail"], 0)
        checks = {item["name"]: item for item in report["checks"]}
        for name in ("real synthetic recording started", "real MP4 frames CFR and audio gain mute",
                     "desktop recording is default and single monitor selected", "multiple monitors require an explicit selection",
                     "recording save picker prefills a new MP4 filename",
                     "recording output directory receives a filename and MP4 suffix",
                     "recording save picker widget evidence", "recording picker preserves a custom filename and cancel",
                     "recording picker suggests a new name after an existing recording",
                     "recording picker does not overwrite existing files", "active recording cannot reopen the output picker",
                     "native resolution is default without preset dimensions", "all resolution and frame rate controls up to 8K120",
                     "8K120 panel widget evidence", "old recording component cannot start new formats",
                     "actual recording statistics reach service and panel",
                     "native 120FPS UI request survives real save and close",
                     "full stable-region synthetic audio continuity",
                     "pause settles encoded output within bounded drain",
                     "pause freezes recorded clock and resume is available", "resume advances recorded clock",
                     "recovery paths are discoverable and copyable", "recovery picker defaults to displayed descriptor",
                     "real recovery button publishes second new MP4", "window close defers teardown while recording",
                     "pending close rejects same-window start and recovery reentry", "pending-close completion hook was exercised",
                     "close completes after recorder finalization"):
            self.assertTrue(checks[name]["passed"], name)
        self.assertTrue(Path(checks["recording save picker widget evidence"]["details"]["path"]).is_file())
        pause = checks["pause settles encoded output within bounded drain"]["details"]
        self.assertGreaterEqual(pause["inflightNs"], 0)
        self.assertLessEqual(pause["settleMs"], pause["deadlineMs"])
        self.assertEqual(pause["deadlineMs"], 2500)
        self.assertGreaterEqual(pause["stableMs"], 500)
        self.assertGreaterEqual(pause["progressSamples"], 2)
        frozen = checks["pause freezes recorded clock and resume is available"]["details"]
        self.assertEqual(frozen["durationAfterWait"], frozen["settledDuration"])
        self.assertGreaterEqual(frozen["waitMs"], 300)
        media = report["evidence"]["mediaProbe"]
        self.assertTrue(media["audioContinuityPassed"])
        measurement = json.loads(Path(media["audioMeasurementFile"]).read_text(encoding="utf-8"))
        self.assertEqual([item["segment"] for item in measurement["stableRegions"]], ["before", "after"])
        for segment in measurement["stableRegions"]:
            self.assertTrue(segment["passed"])
            self.assertTrue(segment["allToneWindows"], "Measure the complete stable region")
            for key in ("lowEnergyRuns", "toneDropouts", "timestampHoles", "timestampOverlaps", "timestampIssues"):
                self.assertEqual(segment[key], [], key)
        (ARTIFACTS / "latest.json").write_text(json.dumps({"passed": True, "report": str(report_path), "runner": outer}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
