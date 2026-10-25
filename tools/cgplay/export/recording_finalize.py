"""Recording-only prepare/recovery/remux. A final MP4 is never overwritten."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import math
from fractions import Fraction
import os
import re
import subprocess
import sys
import tempfile
import time
import uuid
import queue
import threading

MAX_DESCRIPTOR = 64 * 1024 * 1024
MAX_PROCESS_OUTPUT = 128 * 1024 * 1024
_job = None
class RetainedRecordingError(RuntimeError):
    pass


def emit(event, **values):
    text = json.dumps(dict(schema=1, event=event, **values), ensure_ascii=False)
    if len(text.encode("utf-8")) > 65535:
        raise ValueError("result-too-large")
    print(text, flush=True)


def load_json(path, limit=65536):
    with open(path, "rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError("json-size-limit")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate-json-key")
            result[key] = value
        return result
    def invalid_constant(value):
        raise ValueError("nonfinite-json-number")
    result = json.loads(data.decode("utf-8"), object_pairs_hook=unique, parse_constant=invalid_constant)
    if not isinstance(result, dict):
        raise ValueError("json-object-required")
    return result


def safe_path(path, *, exists=False):
    if not isinstance(path, (str, os.PathLike)) or not str(path) or "\0" in str(path):
        raise ValueError("invalid-path")
    value = Path(os.path.abspath(path))
    if not Path(path).is_absolute() or any(part == ".." for part in Path(path).parts):
        raise ValueError("absolute-normal-path-required")
    current = Path(value.anchor)
    for part in value.parts[1:]:
        current /= part
        try:
            info = os.lstat(current)
        except FileNotFoundError:
            continue
        if os.path.islink(current) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("reparse-path-rejected")
    if exists and not value.is_file():
        raise ValueError("required-file-missing")
    return value


def canonical(path):
    return os.path.normcase(os.path.realpath(os.path.abspath(path)))


def snapshot(path, *, read_only=False):
    if read_only:
        if not isinstance(path, (str, os.PathLike)) or not str(path) or "\0" in str(path) or not Path(path).is_absolute():
            raise ValueError("invalid-protected-input-path")
        path = Path(os.path.realpath(os.path.abspath(path)))
    else:
        path = safe_path(path)
    try:
        info = os.stat(path)
    except FileNotFoundError:
        return dict(path=canonical(path), device=None, inode=None)
    return dict(path=canonical(path), device=info.st_dev, inode=info.st_ino)


def protected_inputs(paths):
    if not isinstance(paths, list) or len(paths) > 128 or not all(isinstance(path, str) for path in paths):
        raise ValueError("invalid-protected-media-list")
    result = {}
    for path in paths:
        info = snapshot(path, read_only=True)
        path = Path(info["path"])
        result[canonical(path)] = info
        match = re.match(r"^(.+?)[._-](\d+)\.(.+)$", path.name)
        if match and path.suffix.lower() in (".exr", ".png", ".jpg", ".jpeg", ".tif", ".tiff", ".dpx", ".cin", ".bmp", ".webp"):
            pattern = re.compile(rf"^{re.escape(match[1])}[._-]\d+\.{re.escape(match[3])}$", re.IGNORECASE if os.name == "nt" else 0)
            for member in path.parent.iterdir():
                if pattern.match(member.name):
                    result[canonical(member)] = snapshot(member, read_only=True)
                    if len(result) > 500000:
                        raise ValueError("protected-sequence-size-limit")
    return list(result.values())


def assert_target(path, protected):
    path = safe_path(path)
    if path.suffix.lower() != ".mp4" or not path.parent.is_dir():
        raise ValueError("new-mp4-in-existing-directory-required")
    current = snapshot(path)
    for source in protected:
        if current["path"] == source["path"] or (current["inode"] is not None and
                (current["device"], current["inode"]) == (source.get("device"), source.get("inode"))):
            raise ValueError("output-aliases-protected-input")
    if path.exists():
        raise FileExistsError("recording-never-overwrites-existing-target")
    return path


def write_exclusive(path, value):
    data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
    if len(data) > MAX_DESCRIPTOR:
        raise ValueError("protected-session-metadata-size-limit-before-capture")
    with open(path, "xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def session_paths(directory, session):
    if str(uuid.UUID(session)) != session.lower():
        raise ValueError("invalid-session-uuid")
    directory = safe_path(directory)
    if directory.name != ".cgplay-recording-" + session or not directory.is_dir():
        raise ValueError("invalid-session-directory")
    return directory, directory / "session.json", directory / ("capture.recording." + session + ".mkv")


def prepare(request):
    session = request["sessionId"]
    requested_directory = safe_path(request["sessionDirectory"])
    if requested_directory.name != ".cgplay-recording-" + session or not requested_directory.parent.is_dir():
        raise ValueError("invalid-session-directory")
    requested_directory.mkdir(exist_ok=False)
    directory, descriptor, mkv = session_paths(request["sessionDirectory"], session)
    protected = protected_inputs(request.get("protectedMediaPaths", []))
    output = assert_target(request["finalOutputPath"], protected)
    if mkv.exists() or descriptor.exists() or Path(str(mkv) + ".json").exists():
        raise FileExistsError("session-already-exists")
    plan = request.get("helperPlan", {})
    if not isinstance(plan, dict):
        raise ValueError("invalid-helper-plan")
    native_resolution = plan.get("nativeResolution", False)
    if not isinstance(native_resolution, bool):
        raise ValueError("invalid-native-resolution-selection")
    audio_count = 3 if plan.get("splitTracks") and (plan.get("systemEnabled") or plan.get("micEnabled")) else 1
    value = dict(schema=1, kind="cgplay-recording-session", sessionId=session,
                 mkvFile=mkv.name, nativeSidecarFile=mkv.name + ".json", finalOutputPath=str(output),
                 protectedInputs=protected, protectedMediaPaths=request.get("protectedMediaPaths", []),
                 width=None if native_resolution else plan.get("width", 1920),
                 height=None if native_resolution else plan.get("height", 1080), nativeResolution=native_resolution,
                 fps=plan.get("fps", 30), audioStreams=audio_count)
    write_exclusive(descriptor, value)
    return dict(ok=True, sessionId=session, sessionDescriptorPath=str(descriptor), mkvPath=str(mkv), outputPath=str(output))


def observed_file_identity(path):
    if os.name != "nt":
        info = os.stat(path)
        return dict(volume=str(info.st_dev), fileId=str(info.st_ino))
    from ctypes import wintypes
    class Id(ctypes.Structure):
        _fields_ = [("volume", ctypes.c_ulonglong), ("id", ctypes.c_ubyte * 16)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.GetFileInformationByHandleEx.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "recording-still-open-or-unreadable")
    data = Id()
    if not kernel.GetFileInformationByHandleEx(handle, 18, ctypes.byref(data), ctypes.sizeof(data)):
        kernel.CloseHandle(handle)
        raise OSError("recording-file-identity-unavailable")
    return dict(volume=str(data.volume), fileId=bytes(data.id).hex()), handle


def descriptor_paths(path):
    path = safe_path(path, exists=True)
    if path.name != "session.json":
        raise ValueError("select-recording-session-json")
    value = load_json(path, MAX_DESCRIPTOR)
    if value.get("schema") != 1 or value.get("kind") != "cgplay-recording-session":
        raise ValueError("invalid-session-descriptor")
    directory, expected, mkv = session_paths(path.parent, value["sessionId"])
    if expected != path or value.get("mkvFile") != mkv.name or value.get("nativeSidecarFile") != mkv.name + ".json":
        raise ValueError("descriptor-path-mismatch")
    safe_path(mkv, exists=True)
    native = load_json(safe_path(str(mkv) + ".json", exists=True))
    if native.get("schema") != 1 or native.get("session") != value["sessionId"] or canonical(native.get("mkv", "")) != canonical(mkv):
        raise ValueError("native-sidecar-mismatch")
    if not isinstance(native.get("mkvIdentity"), dict):
        raise ValueError("recording-identity-not-yet-recorded")
    if value.get("nativeResolution") is True:
        # Only the helper can resolve the real physical capture dimensions.
        # Keep the request immutable; bind finalization/recovery to its recorded
        # format, then require the actual media probe to match it exactly.
        video_format = native.get("videoFormat", {})
        if not isinstance(video_format, dict) or video_format.get("nativeResolution") is not True:
            raise ValueError("native-recording-format-missing")
        width, height, fps = (video_format.get(key) for key in ("width", "height", "fps"))
        if (any(type(number) is not int for number in (width, height, fps)) or
                not 160 <= width <= 8192 or not 90 <= height <= 8192 or width % 4 or height % 2 or
                width * height > 8192 * 4320 or not 1 <= fps <= 120 or fps != value.get("fps")):
            raise ValueError("invalid-native-recording-format")
        value = dict(value, width=width, height=height)
    return value, mkv, native["mkvIdentity"]


def run(command, directory, label, cancel_path, timeout=600):
    stdout = directory / (label + ".stdout")
    stderr = directory / (label + ".stderr")
    start = time.monotonic()
    with stdout.open("xb") as out, stderr.open("xb") as err:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            while process.poll() is None:
                if cancel_path.exists():
                    raise InterruptedError("finalization-cancelled-mkv-retained")
                if time.monotonic() - start > timeout:
                    raise TimeoutError("finalizer-child-timeout")
                if stdout.stat().st_size > MAX_PROCESS_OUTPUT or stderr.stat().st_size > 1024 * 1024:
                    raise ValueError("finalizer-child-output-limit")
                time.sleep(.05)
            if process.returncode:
                detail = stderr.read_bytes()[-4096:].decode("utf-8", errors="replace")
                raise RuntimeError(label + "-failed:" + detail)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
    return stdout


def scan_packets(command, stage, cancel_path, timeout=600):
    """Bounded transport, unbounded valid duration: never retain a packet list."""
    inbox = queue.Queue(maxsize=32)
    stopping = threading.Event()
    stderr_path = stage / "packets.stderr"
    def put(value):
        while not stopping.is_set():
            try:
                inbox.put(value, timeout=.1)
                return
            except queue.Full:
                pass
    with stderr_path.open("xb") as stderr:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=stderr,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        def read():
            try:
                while not stopping.is_set():
                    line = process.stdout.readline(16385)
                    if not line:
                        break
                    if len(line) > 16384:
                        put(("error", "packet-line-limit")); return
                    put(("line", line))
                put(("end", None))
            except Exception as error:
                put(("error", str(error)))
        reader = threading.Thread(target=read)
        reader.start()
        start = time.monotonic()
        counts, last, first, last_pts = {}, {}, {}, {}
        raw_bytes = 0
        try:
            while True:
                if cancel_path.exists(): raise InterruptedError("finalization-cancelled-mkv-retained")
                if time.monotonic() - start > timeout: raise TimeoutError("packet-probe-timeout")
                if stderr_path.stat().st_size > 1024 * 1024: raise ValueError("packet-probe-error-output-limit")
                try: kind, payload = inbox.get(timeout=.1)
                except queue.Empty: continue
                if kind == "error": raise ValueError(payload)
                if kind == "end": break
                raw_bytes += len(payload)
                line = payload.decode("utf-8")
                fields = dict(item.split("=", 1) for item in line.strip().split("|") if "=" in item)
                if "stream_index" not in fields: continue
                index, dts, pts, size = (int(fields[key]) for key in ("stream_index", "dts", "pts", "size"))
                if size <= 0 or (index in last and dts < last[index]): raise ValueError("invalid-packet-timestamps")
                first.setdefault(index, dict(pts=pts, dts=dts))
                last[index] = dts; last_pts[index] = pts; counts[index] = counts.get(index, 0) + 1
            if process.wait(timeout=10): raise ValueError("packet-probe-failed")
            return dict(counts=counts, first=first, lastDts=last, lastPts=last_pts, rawBytes=raw_bytes, queueLimit=32)
        finally:
            stopping.set()
            if process.poll() is None: process.kill()
            process.wait(timeout=10)
            reader.join(timeout=10)
            if reader.is_alive(): raise RuntimeError("packet-reader-did-not-stop")
            process.stdout.close()


def probe(ffprobe, path, stage, cancel_path, expected):
    metadata = run([ffprobe, "-v", "error", "-protocol_whitelist", "file,pipe", "-show_streams", "-show_format", "-of", "json", str(path)], stage, "metadata", cancel_path)
    value = load_json(metadata, 1024 * 1024)
    streams = value.get("streams", [])
    video = [stream for stream in streams if stream.get("codec_type") == "video"]
    audio = [stream for stream in streams if stream.get("codec_type") == "audio"]
    if len(streams) != 1 + expected["audioStreams"] or len(video) != 1 or video[0].get("codec_name") != "h264" or len(audio) != expected["audioStreams"] or any(s.get("codec_name") != "aac" or s.get("sample_rate") != "48000" or s.get("channels") != 2 for s in audio):
        raise ValueError("unexpected-recording-streams")
    if (video[0].get("width"), video[0].get("height")) != (expected["width"], expected["height"]):
        raise ValueError("unexpected-recording-dimensions")
    if Fraction(video[0].get("r_frame_rate", "0/1")) != expected["fps"]:
        raise ValueError("unexpected-recording-frame-rate")
    duration = float(value.get("format", {}).get("duration", 0))
    if not math.isfinite(duration) or duration <= 0:
        raise ValueError("invalid-recording-duration")
    packet_stats = scan_packets([ffprobe, "-v", "error", "-protocol_whitelist", "file,pipe", "-show_packets", "-show_entries", "packet=stream_index,pts,dts,duration,size", "-of", "compact=p=0:nk=0", str(path)], stage, cancel_path)
    counts = packet_stats["counts"]
    if any(counts.get(stream["index"], 0) == 0 for stream in streams):
        raise ValueError("empty-recording-stream")
    return dict(durationSeconds=duration, packetCounts=counts, videoPackets=counts[video[0]["index"]], packetFirst=packet_stats["first"], packetLastDts=packet_stats["lastDts"], packetLastPts=packet_stats["lastPts"])


def no_replace_publish(staged, output):
    if os.stat(staged.parent).st_dev != os.stat(output.parent).st_dev:
        raise ValueError("publish-must-stay-on-output-volume")
    with staged.open("r+b") as stream:
        os.fsync(stream.fileno())
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.MoveFileExW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
        if not kernel.MoveFileExW(str(staged), str(output), 8):  # WRITE_THROUGH, no REPLACE_EXISTING
            raise OSError(ctypes.get_last_error(), "atomic-publish-conflict-or-failure")
    else:
        os.link(staged, output)
        staged.unlink()


def finalize(request, cancel_path):
    if request.get("operation") == "finalize" and request.get("helperExited") is not True:
        raise ValueError("helper-exit-required-before-finalizer")
    descriptor, mkv, expected_identity = descriptor_paths(request["sessionDescriptorPath"])
    protected = list(descriptor.get("protectedInputs", [])) + protected_inputs(descriptor.get("protectedMediaPaths", [])) + protected_inputs(request.get("protectedMediaPaths", []))
    protected.extend((snapshot(mkv), snapshot(request["sessionDescriptorPath"]), snapshot(str(mkv) + ".json")))
    output = assert_target(request["finalOutputPath"], protected)
    ffmpeg = str(safe_path(request["ffmpeg"], exists=True)); ffprobe = str(safe_path(request["ffprobe"], exists=True))
    stage = Path(tempfile.mkdtemp(prefix=".cgplay-publish-" + descriptor["sessionId"] + "-", dir=output.parent))
    staged = stage / "complete.mp4"
    handle = None
    try:
        observed = observed_file_identity(mkv)
        identity, handle = observed if os.name == "nt" else (observed, None)
        if identity != expected_identity:
            raise ValueError("recording-file-identity-changed")
        emit("progress", phase="remux", sessionId=descriptor["sessionId"], mkvPath=str(mkv))
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-n", "-protocol_whitelist", "file,pipe", "-f", "matroska", "-i", str(mkv), "-map", "0", "-c", "copy", "-movflags", "+faststart", "-f", "mp4", str(staged)], stage, "remux", cancel_path)
        emit("progress", phase="verify", sessionId=descriptor["sessionId"])
        metrics = probe(ffprobe, staged, stage, cancel_path, descriptor)
        if cancel_path.exists():
            raise InterruptedError("finalization-cancelled-mkv-retained")
        assert_target(output, protected)
        no_replace_publish(staged, output)
        for name in ("remux.stdout", "remux.stderr", "metadata.stdout", "metadata.stderr", "packets.stdout", "packets.stderr"):
            try:
                (stage / name).unlink()
            except OSError:
                pass
        try:
            stage.rmdir()
        except OSError:
            pass
        return dict(ok=True, sessionId=descriptor["sessionId"], outputPath=str(output), mkvPath=str(mkv),
                    sessionDescriptorPath=str(request["sessionDescriptorPath"]), outputBytes=output.stat().st_size, **metrics)
    except Exception as error:
        # Keep both original MKV and any complete staged MP4 on conflict.
        retained = RetainedRecordingError(str(error) + "; retainedStage=" + str(stage))
        retained.mkv_path = str(mkv)
        raise retained from error
    finally:
        if handle is not None:
            kernel = ctypes.WinDLL("kernel32"); kernel.CloseHandle.argtypes = [ctypes.c_void_p]; kernel.CloseHandle(handle)


def install_process_job():
    global _job
    if os.name != "nt":
        return
    from ctypes import wintypes
    class Basic(ctypes.Structure):
        _fields_ = [("processTime", ctypes.c_longlong), ("jobTime", ctypes.c_longlong), ("flags", wintypes.DWORD),
                    ("minWorking", ctypes.c_size_t), ("maxWorking", ctypes.c_size_t), ("active", wintypes.DWORD),
                    ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]
    class Io(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in ("read", "write", "other", "readBytes", "writeBytes", "otherBytes")]
    class Extended(ctypes.Structure):
        _fields_ = [("basic", Basic), ("io", Io), ("processMemory", ctypes.c_size_t), ("jobMemory", ctypes.c_size_t), ("peakProcess", ctypes.c_size_t), ("peakJob", ctypes.c_size_t)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]; kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _job = kernel.CreateJobObjectW(None, None)
    limits = Extended(); limits.basic.flags = 0x2000
    if not _job or not kernel.SetInformationJobObject(_job, 9, ctypes.byref(limits), ctypes.sizeof(limits)) or not kernel.AssignProcessToJobObject(_job, kernel.GetCurrentProcess()):
        raise OSError(ctypes.get_last_error(), "finalizer-process-job-required")


def main():
    parser = argparse.ArgumentParser(); parser.add_argument("--request", required=True)
    args = parser.parse_args()
    request_path = safe_path(args.request, exists=True)
    cancel_path = request_path.with_suffix(".cancel")
    try:
        install_process_job()
        request = load_json(request_path)
        if request.get("schema") != 1 or request.get("operation") not in ("prepare", "finalize", "recover"):
            raise ValueError("invalid-finalizer-request")
        result = prepare(request) if request["operation"] == "prepare" else finalize(request, cancel_path)
        emit("result", **result)
        return 0
    except Exception as error:
        emit("result", ok=False, error=str(error), sessionId=request.get("sessionId", "") if "request" in locals() else "",
             mkvRetained=isinstance(error, RetainedRecordingError), mkvPath=getattr(error, "mkv_path", ""))
        return 1


if __name__ == "__main__":
    sys.exit(main())
