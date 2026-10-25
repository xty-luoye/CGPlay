"""Native private libobs helper acceptance. Only self-generated pixels/tones."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
import shutil

import av
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = Path(os.environ.get("CGPLAY_RECORDER_ROOT", "E:/rjkf/CGPlay-deps/obs-33.0.0/stage"))
ARTIFACTS = Path(os.environ.get("CGPLAY_RECORDER_ARTIFACTS", ROOT / "tests/artifacts/migration_1_0_8_20261025/recording_backend_validation"))
ARTIFACTS.mkdir(parents=True, exist_ok=True)


def visible_windows(pid):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    result = []
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    @callback_type
    def callback(window, unused):
        actual = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(actual))
        if actual.value == pid and user32.IsWindowVisible(window):
            result.append(int(window))
        return True
    user32.EnumWindows(callback, 0)
    return result


def descendants(pid):
    class Entry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("name", wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    entry = Entry(); entry.dwSize = ctypes.sizeof(Entry)
    processes = []
    try:
        valid = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while valid:
            processes.append((entry.pid, entry.parent, entry.name))
            valid = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    known, result = {pid}, []
    for _ in range(8):
        added = [(child, name) for child, parent, name in processes if parent in known and child not in known]
        if not added:
            break
        known.update(child for child, name in added); result.extend(added)
    return result


def process_alive(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x00100000, False, pid)
    if not handle:
        return False
    try:
        return kernel.WaitForSingleObject(handle, 0) == 0x102
    finally:
        kernel.CloseHandle(handle)


class Helper:
    def __init__(self, directory, synthetic=True, clean=True, runtime=RUNTIME):
        self.directory = Path(directory)
        self.session = str(uuid.uuid4())
        self.sequence = 0
        self.events = []
        self.inbox = queue.Queue()
        self.visible_max = 0
        self.output = self.directory / ("test.recording." + self.session + ".mkv")
        self.log = (self.directory / "stderr.log").open("wb")
        environment = os.environ.copy()
        if clean:
            environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
            environment["OBS_PLUGINS_PATH"] = str(self.directory / "forbidden-user-plugins")
        self.process = subprocess.Popen([str(runtime / "CGPlayRecorder.exe")] + (["--synthetic"] if synthetic else []),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                        cwd=self.directory, env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.read_gate = threading.Event()
        self.read_gate.set()
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            self.read_gate.wait()
            try:
                event = json.loads(line.decode("utf-8"))
            except Exception:
                event = {"malformed": line.decode("utf-8", errors="replace")}
            self.events.append(event)
            self.inbox.put(event)
        self.inbox.put({"processEnded": True})

    def send(self, command, **values):
        self.sequence += 1
        self.raw(dict(protocol=1, session=self.session, seq=self.sequence, command=command, **values))

    def raw(self, value):
        payload = value if isinstance(value, bytes) else (json.dumps(value) + "\n").encode()
        self.process.stdin.write(payload)
        self.process.stdin.flush()

    def wait(self, state=None, event=None, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.visible_max = max(self.visible_max, len(visible_windows(self.process.pid)))
            try:
                value = self.inbox.get(timeout=.1)
            except queue.Empty:
                continue
            if "malformed" in value:
                raise AssertionError("non-NDJSON stdout: " + repr(value))
            if value.get("processEnded"):
                raise AssertionError("helper exited before expected event; " + str(self.events[-5:]) + "; " + (self.directory / "stderr.log").read_text(errors="replace")[-4000:])
            if (state is None or value.get("state") == state) and (event is None or value.get("event") == event):
                return value
        raise AssertionError("helper event timeout: " + str(self.events[-8:]))

    def plan(self, **values):
        result = dict(source="synthetic", output=str(self.output), width=320, height=180, fps=30,
                      sourceWidth=321, sourceHeight=181, systemEnabled=True, micEnabled=True)
        result.update(values)
        return result

    def start(self, **values):
        self.wait(event="ready")
        self.send("start", plan=self.plan(**values))
        return self.wait(state="Recording", event="state")

    def stop(self):
        self.send("stop")
        result = self.wait(state="Completed", event="finalized")
        self.finish(0)
        return result

    def finish(self, expected=None):
        code = self.process.wait(timeout=25)
        self.reader.join(timeout=2)
        if self.process.stdin and not self.process.stdin.closed:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        self.process.stdout.close()
        self.log.close()
        report = dict(backgroundMode=True, visibleAutomationWindowsMax=self.visible_max,
                      exitCode=code, syntheticOnly=True, events=self.events)
        (self.directory / "protocol.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        if expected is not None:
            if code != expected:
                raise AssertionError(f"unexpected helper exit {code}, expected {expected}")
        if self.visible_max:
            raise AssertionError("visible helper top-level window")
        return code

    def close(self):
        self.read_gate.set()
        if self.process.poll() is None:
            try:
                self.send("stop")
                self.process.wait(timeout=20)
            except Exception:
                self.process.kill()
                self.process.wait(timeout=10)
        if not self.log.closed:
            self.finish()


def media(path):
    result = dict(video=[], audio=[], packets=[])
    with av.open(str(path)) as container:
        for stream in container.streams:
            previous = None
            packets = []
            for packet in container.demux(stream):
                if packet.dts is not None:
                    if previous is not None:
                        assert packet.dts >= previous, "DTS moved backwards"
                    previous = packet.dts
                    packets.append(dict(pts=packet.pts, dts=packet.dts, duration=packet.duration))
            result["packets"].append(packets)
            container.seek(0)
            frames = list(container.decode(stream))
            times = [float(frame.pts * frame.time_base) for frame in frames if frame.pts is not None]
            assert times == sorted(times), "decoded PTS moved backwards"
            if stream.type == "video":
                pixels = [frame.to_ndarray(format="rgb24") for frame in frames]
                result["video"].append(dict(count=len(frames), times=times, width=stream.width, height=stream.height,
                                            changed=bool(pixels and np.any(pixels[0] != pixels[-1]))))
            elif stream.type == "audio":
                data = np.concatenate([frame.to_ndarray()[0] for frame in frames]) if frames else np.array([])
                result["audio"].append(dict(rate=stream.codec_context.sample_rate, samples=data, times=times))
            container.seek(0)
    return result


class RecordingHelperTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix=self._testMethodName + "-", dir=ARTIFACTS))
        self.helpers = []

    def helper(self, **values):
        directory = self.directory / ("helper_" + str(len(self.helpers)))
        directory.mkdir()
        result = Helper(directory, **values)
        self.helpers.append(result)
        return result

    def tearDown(self):
        for helper in self.helpers:
            helper.close()

    def test_real_synthetic_encode_pause_flush_and_monotonic_timestamps(self):
        helper = self.helper()
        helper.start()
        time.sleep(1.5)
        helper.send("pause")
        paused = helper.wait(state="Paused", event="state")
        time.sleep(.8)
        helper.send("resume")
        helper.wait(state="Recording", event="state")
        time.sleep(1.5)
        completed = helper.stop()
        self.assertEqual(0, completed["liveSourceThreads"])
        value = media(helper.output)
        video = value["video"][0]
        self.assertEqual((320, 180), (video["width"], video["height"]))
        self.assertTrue(video["changed"])
        self.assertGreater(video["count"], 65)
        self.assertLess(video["count"], 115)
        gaps = np.diff(video["times"])
        self.assertTrue(np.all(np.abs(gaps - 1 / 30) < .002), gaps.tolist())
        self.assertEqual(1, len(value["audio"]))
        audio = value["audio"][0]
        self.assertEqual(48000, audio["rate"])
        self.assertLess(abs(len(audio["samples"]) / 48000 - video["count"] / 30), .18)
        self.assertGreater(completed["durationNs"], paused["durationNs"])
        self.assertEqual("Completed", json.loads(Path(str(helper.output) + ".json").read_text())["state"])
        (self.directory / "media_report.json").write_text(json.dumps(dict(video=video, audioSampleCount=len(audio["samples"]),
                                                                         audioRate=audio["rate"], final=completed), indent=2), encoding="utf-8")

    def test_real_two_tones_gain_mute_and_independent_tracks(self):
        helper = self.helper()
        helper.start(splitTracks=True)
        time.sleep(1.5)
        helper.send("setAudio", systemGain=.25)
        helper.wait(event="audio")
        time.sleep(1.5)
        helper.send("setAudio", systemMute=True)
        helper.wait(event="audio")
        time.sleep(1.5)
        helper.send("setAudio", systemMute=False, systemGain=1., micMute=True)
        helper.wait(event="audio")
        time.sleep(1.8)
        helper.stop()
        value = media(helper.output)
        self.assertEqual(3, len(value["audio"]))
        mixed = value["audio"][0]
        def magnitude(samples, start, frequency):
            segment = samples[int(start * 48000):int((start + .4) * 48000)]
            window = np.hanning(len(segment))
            spectrum = np.fft.rfft(segment * window)
            frequencies = np.fft.rfftfreq(len(segment), 1 / 48000)
            band = np.abs(frequencies - frequency) <= 40
            # Integrate a narrow band: clock correction may shift phase inside
            # a block, which must not be mistaken for a gain change.
            return np.sqrt(4 * np.sum(np.abs(spectrum[band]) ** 2) / (len(segment) * np.sum(window ** 2)))
        segments = [(.5, 440), (2.0, 440), (3.5, 440), (5.0, 440), (.5, 880), (2.0, 880), (3.5, 880), (5.0, 880)]
        actual = [float(magnitude(mixed["samples"], start, frequency)) for start, frequency in segments]
        (self.directory / "tone_report.json").write_text(json.dumps(dict(segments=segments, magnitudes=actual, bandHalfWidthHz=40), indent=2), encoding="utf-8")
        self.assertGreater(actual[0], .06)
        self.assertAlmostEqual(.25, actual[1] / actual[0], delta=.06)
        self.assertLess(actual[2], actual[0] * .05)
        self.assertAlmostEqual(1., actual[3] / actual[0], delta=.08)
        self.assertAlmostEqual(1., actual[6] / actual[4], delta=.08)
        self.assertLess(actual[7], actual[4] * .05)
        self.assertLess(magnitude(value["audio"][1]["samples"], .5, 880), .003)
        self.assertLess(magnitude(value["audio"][2]["samples"], .5, 440), .003)

    def test_existing_output_is_preserved_and_hdr_is_explicitly_rejected(self):
        helper = self.helper()
        helper.output.write_bytes(b"existing source-or-output must survive")
        before = hashlib.sha256(helper.output.read_bytes()).hexdigest()
        helper.wait(event="ready")
        helper.send("start", plan=helper.plan())
        result = helper.wait(state="Failed", event="state")
        self.assertIn("output-already-exists", result["error"])
        helper.finish(1)
        self.assertEqual(before, hashlib.sha256(helper.output.read_bytes()).hexdigest())
        another = self.helper()
        another.wait(event="ready")
        another.send("start", plan=another.plan(hdr=True))
        result = another.wait(state="Failed", event="state")
        self.assertIn("hdr-capture-not-supported", result["error"])
        another.finish(1)
        self.assertFalse(another.output.exists())

    def test_invalid_window_identity_is_rejected_before_capture(self):
        helper = self.helper(synthetic=False)
        helper.wait(event="ready")
        helper.send("start", plan=helper.plan(source="window", hwnd="1", pid=1, processCreated="1", windowClass="invalid", windowToken="invalid"))
        result = helper.wait(state="Failed", event="state")
        self.assertIn("window-identity-lost", result["error"])
        helper.finish(1)

    def test_stale_session_and_seq_are_ignored_and_input_limits_stop_cleanly(self):
        helper = self.helper()
        helper.start()
        helper.raw(dict(protocol=1, session=str(uuid.uuid4()), seq=999, command="stop"))
        helper.raw(dict(protocol=1, session=helper.session, seq=1, command="stop"))
        helper.send("pause")
        helper.wait(state="Paused", event="state")
        helper.send("resume")
        helper.wait(state="Recording", event="state")
        time.sleep(.7)
        helper.raw(b"x" * 65537)
        result = helper.wait(state="Recoverable", event="finalized")
        self.assertIn("protocol-limit", result["error"])
        self.assertEqual(0, result["liveSourceThreads"])
        helper.finish(1)
        self.assertGreater(media(helper.output)["video"][0]["count"], 0)

    def test_controller_disconnect_and_injected_device_loss_preserve_mkv(self):
        for failure in ("disconnect", "device"):
            with self.subTest(failure=failure):
                helper = self.helper()
                helper.start()
                time.sleep(1.1)
                if failure == "disconnect":
                    helper.process.stdin.close()
                else:
                    helper.send("injectFailure", target="audio-device")
                result = helper.wait(state="Recoverable", event="finalized")
                self.assertEqual(0, result["liveSourceThreads"])
                helper.finish(1)
                self.assertGreater(media(helper.output)["video"][0]["count"], 0)

    def test_forced_exit_leaves_decodable_mkv_clusters_for_recovery(self):
        helper = self.helper()
        helper.start()
        time.sleep(3.2)
        children = descendants(helper.process.pid)
        self.assertTrue(any(name == "obs-ffmpeg-mux.exe" for pid, name in children), children)
        helper.visible_max = max(helper.visible_max, *(len(visible_windows(pid)) for pid, name in children))
        helper.process.kill()
        helper.finish()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and any(process_alive(pid) for pid, name in children):
            time.sleep(.05)
        self.assertFalse([pid for pid, name in children if process_alive(pid)])
        value = media(helper.output)
        self.assertGreater(value["video"][0]["count"], 25)
        repaired = self.directory / "recovered.mp4"
        with av.open(str(helper.output)) as source, av.open(str(repaired), "w") as output:
            mapping = {stream.index: output.add_stream_from_template(stream) for stream in source.streams}
            for packet in source.demux():
                if packet.dts is None:
                    continue
                packet.stream = mapping[packet.stream.index]
                output.mux(packet)
        recovered = media(repaired)
        self.assertGreater(recovered["video"][0]["count"], 25)
        (self.directory / "recovery_report.json").write_text(json.dumps(dict(backgroundMode=True, childrenBeforeKill=children, liveChildrenAfterKill=[], sourceFrames=value["video"][0]["count"],
                                                                            recoveredFrames=recovered["video"][0]["count"]), indent=2), encoding="utf-8")

    def test_stdout_backpressure_does_not_block_stop_or_thread_join(self):
        helper = self.helper()
        helper.start()
        time.sleep(.8)
        helper.read_gate.clear()
        for _ in range(20):
            helper.send("unknown-but-bounded-command")
            time.sleep(.015)
        helper.send("stop")
        start = time.monotonic()
        code = helper.process.wait(timeout=8)
        elapsed = time.monotonic() - start
        helper.read_gate.set()
        helper.finish(0)
        self.assertEqual(0, code)
        self.assertLess(elapsed, 8)
        self.assertEqual("Completed", json.loads(Path(str(helper.output) + ".json").read_text())["state"])
        (self.directory / "backpressure_report.json").write_text(json.dumps(dict(backgroundMode=True, outputDrainPaused=True, elapsed=elapsed, exitCode=code)), encoding="utf-8")

    def test_pause_then_stop_and_preparing_stop_are_closed_lifecycles(self):
        helper = self.helper()
        helper.start()
        time.sleep(.8)
        helper.send("pause")
        helper.wait(state="Paused", event="state")
        result = helper.stop()
        self.assertEqual(0, result["liveSourceThreads"])
        self.assertGreater(media(helper.output)["video"][0]["count"], 10)
        early = self.helper()
        early.wait(event="ready")
        early.send("start", plan=early.plan())
        early.send("stop")
        result = early.wait(state="Failed", event="finalized")
        self.assertIn("stopped-before-recording", result["error"])
        self.assertEqual(0, result["liveSourceThreads"])
        early.finish(1)

    def test_1080p_static_source_stays_cfr_and_letterboxed(self):
        helper = self.helper()
        helper.start(width=1920, height=1080, sourceWidth=641, sourceHeight=301, testStatic=True)
        time.sleep(1.4)
        helper.stop()
        value = media(helper.output)
        video = value["video"][0]
        self.assertEqual((1920, 1080), (video["width"], video["height"]))
        self.assertGreater(video["count"], 30)
        self.assertTrue(np.all(np.abs(np.diff(video["times"]) - 1 / 30) < .002))
        with av.open(str(helper.output)) as container:
            frames = list(container.decode(video=0))
            image = frames[0].to_ndarray(format="rgb24")
            last = frames[-1].to_ndarray(format="rgb24")
        temporal_error = np.abs(image.astype(np.float32) - last.astype(np.float32))
        # H264 CRF may quantize the same static pixels differently in I/P
        # frames. Check bounded decoded error, not impossible byte identity.
        video["temporalMeanError"] = float(temporal_error.mean())
        video["temporalP99Error"] = float(np.percentile(temporal_error, 99))
        self.assertLess(video["temporalMeanError"], .5)
        self.assertLessEqual(video["temporalP99Error"], 3.)
        self.assertLess(float(image[:20].mean()), 2.)
        self.assertGreater(float(image[540].mean()), 20.)
        (self.directory / "static_1080p_report.json").write_text(json.dumps(video, indent=2), encoding="utf-8")

    def test_crop_epochs_use_edge_rescaling_and_ignore_old_geometry(self):
        helper = self.helper()
        helper.start(sourceWidth=640, sourceHeight=360, geometryEpoch=10,
                     cropLeft=160, cropTop=0, cropWidth=320, cropHeight=360, clientWidth=640, clientHeight=360)
        helper.send("crop", geometryEpoch=9, cropLeft=10000, cropTop=0, cropWidth=1, cropHeight=1, clientWidth=1, clientHeight=1)
        helper.wait(event="geometry")
        helper.send("crop", geometryEpoch=11, cropLeft=240, cropTop=0, cropWidth=480, cropHeight=540, clientWidth=960, clientHeight=540)
        helper.wait(event="geometry")
        time.sleep(.9)
        helper.stop()
        with av.open(str(helper.output)) as container:
            image = next(container.decode(video=0)).to_ndarray(format="rgb24")
        # A 320x360 crop is centered inside a 320x180 canvas, leaving side bars.
        self.assertLess(float(image[:, :40].mean()), 2.)
        self.assertGreater(float(image[:, 155:165].mean()), 10.)

    def test_silent_recording_opens_no_audio_devices_and_still_finalizes(self):
        helper = self.helper()
        helper.start(systemEnabled=False, micEnabled=False)
        time.sleep(1.1)
        helper.stop()
        value = media(helper.output)
        self.assertGreater(value["video"][0]["count"], 15)
        self.assertEqual(1, len(value["audio"]))
        self.assertLess(float(np.max(np.abs(value["audio"][0]["samples"]))), 1e-5)

    def test_sidecar_update_failure_still_flushes_and_joins(self):
        helper = self.helper()
        helper.start()
        time.sleep(.8)
        obstacle = Path(str(helper.output) + ".json.new")
        obstacle.mkdir()
        helper.send("stop")
        result = helper.wait(state="Recoverable", event="finalized")
        self.assertIn("sidecar-create-failed", result["error"])
        self.assertEqual(0, result["liveSourceThreads"])
        helper.finish(1)
        self.assertTrue(obstacle.is_dir())
        self.assertGreater(media(helper.output)["video"][0]["count"], 10)

    def test_blocked_stop_watchdog_terminates_its_mux_tree_for_recovery(self):
        helper = self.helper()
        helper.start()
        time.sleep(2.5)
        children = descendants(helper.process.pid)
        self.assertTrue(any(name == "obs-ffmpeg-mux.exe" for pid, name in children))
        helper.send("injectFailure", target="blocked-stop")
        helper.wait(state="Stopping", event="state")
        start = time.monotonic()
        helper.finish(74)
        self.assertLess(time.monotonic() - start, 5)
        for _ in range(100):
            if not any(process_alive(pid) for pid, name in children):
                break
            time.sleep(.02)
        self.assertFalse([pid for pid, name in children if process_alive(pid)])
        frames = media(helper.output)["video"][0]["count"]
        self.assertGreater(frames, 25)
        (self.directory / "watchdog_report.json").write_text(json.dumps(dict(backgroundMode=True, exitCode=74, normalFlush=False,
                                                                            children=children, liveChildrenAfterExit=[], recoverableFrames=frames), indent=2), encoding="utf-8")

    def test_rejected_audio_and_crop_commands_do_not_partially_apply(self):
        helper = self.helper()
        helper.start(sourceWidth=640, sourceHeight=360, geometryEpoch=1,
                     cropLeft=160, cropTop=0, cropWidth=320, cropHeight=360, clientWidth=640, clientHeight=360)
        helper.send("setAudio", systemGain=.25, systemMute="invalid")
        rejected = helper.wait(event="commandError")
        self.assertIn("invalid-type:systemMute", rejected["error"])
        helper.send("crop", geometryEpoch=2, cropLeft=0, cropTop=0, cropWidth=1, cropHeight=1, clientWidth=32768, clientHeight=32768)
        rejected = helper.wait(event="commandError")
        self.assertIn("empty-player-crop", rejected["error"])
        helper.send("crop", geometryEpoch=2, cropLeft=0, cropTop=0, cropWidth=640, cropHeight=360, clientWidth=640, clientHeight=360)
        helper.wait(event="geometry")
        time.sleep(1.3)
        helper.stop()
        with av.open(str(helper.output)) as container:
            frames = list(container.decode(video=0))
            image = frames[-1].to_ndarray(format="rgb24")
        self.assertGreater(float(image[:, :40].mean()), 10.)
        value = media(helper.output)
        audio = value["audio"][0]["samples"][24000:48000]
        spectrum = np.abs(np.fft.rfft(audio * np.hanning(len(audio))))
        frequencies = np.fft.rfftfreq(len(audio), 1 / 48000)
        system = float(np.sum(spectrum[np.abs(frequencies - 440) <= 40] ** 2))
        mic = float(np.sum(spectrum[np.abs(frequencies - 880) <= 40] ** 2))
        self.assertGreater(system / mic, .7)

    def test_runtime_hash_and_unlisted_binary_are_rejected_under_clean_path(self):
        # Native stage copies belong beside the dependency, never in product
        # output. Hard links keep the immutable baseline untouched and cheap.
        private = RUNTIME.parent / "validation-runtimes" / str(uuid.uuid4())
        private.mkdir(parents=True)
        for source in RUNTIME.rglob("*"):
            target = private / source.relative_to(RUNTIME)
            if source.is_dir():
                target.mkdir(exist_ok=True)
            elif source.is_file():
                os.link(source, target)
        manifest = private / "runtime-manifest.json"
        original_manifest = manifest.read_bytes()
        manifest.unlink()
        reports = []
        for failure in ("unlisted", "hash"):
            value = json.loads(original_manifest)
            if failure == "unlisted":
                value["files"] = [entry for entry in value["files"] if entry["path"] != "core/win-wasapi/win-wasapi.dll"]
            else:
                next(entry for entry in value["files"] if entry["path"] == "core/win-wasapi/win-wasapi.dll")["sha256"] = "0" * 64
            manifest.write_text(json.dumps(value), encoding="utf-8")
            environment = os.environ.copy(); environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
            process = subprocess.Popen([str(private / "CGPlayRecorder.exe"), "--synthetic"], stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=self.directory, env=environment,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(2, process.returncode)
            self.assertEqual(b"", stdout)
            self.assertIn(b"unlisted-runtime-binary" if failure == "unlisted" else b"runtime-integrity", stderr)
            reports.append(dict(case=failure, exitCode=process.returncode, stderr=stderr.decode(errors="replace")))
        (self.directory / "runtime_integrity_report.json").write_text(json.dumps(dict(backgroundMode=True, privateStage=str(private), cases=reports), indent=2), encoding="utf-8")

    def test_native_two_hour_equivalent_frame_and_sample_clock(self):
        result = subprocess.run([str(RUNTIME / "CGPlayRecorder.exe"), "--clock-selftest"],
                                capture_output=True, cwd=self.directory, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(0, result.returncode, result.stderr.decode(errors="replace"))
        value = json.loads(result.stdout)
        self.assertTrue(value["clockSelfTest"])
        self.assertEqual(7200000000000, value["twoHourVideoNs"])
        self.assertEqual(7200000000000, value["twoHourAudioNs"])
        (self.directory / "two_hour_equivalent_clock.json").write_text(json.dumps(value, indent=2), encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
