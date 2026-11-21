"""Exercise the released Windows setup against an isolated copy of the supported base."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory

from run_state_regression import run


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--setup", required=True, type=Path)
    parser.add_argument("--base-dir", required=True, type=Path)
    parser.add_argument("--expected-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    files = ("CGPlay.exe", "CGPlayQuickLook.exe", "CGPlayThumbnailProvider.dll")
    anchors = ("Qt6Core.dll", "platforms/qwindows.dll", "ffmpeg.exe", "runtime/python/python.exe")
    with TemporaryDirectory(dir=args.output) as temp:
        target = Path(temp).resolve()
        for name in (*files, *anchors):
            path = target / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.base_dir / name, path)

        def exercise(label, expected, success):
            result = run([args.setup.resolve(), "/S", "/D=" + str(target)], args.output / (label + ".json"))
            assert result["backgroundMode"] and result["visibleAutomationWindowsMax"] == 0
            assert not result["timedOut"] and result["passed"] == success, result
            for name in files:
                assert digest(target / name) == digest(expected / name), name

        # Disallow delete sharing on the second replacement and verify rollback of the first.
        win = ctypes.WinDLL("kernel32", use_last_error=True)
        create = win.CreateFileW
        create.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                           ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        create.restype = ctypes.c_void_p
        handle = create(str(target / files[2]), 0x80000000, 1, None, 3, 0, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            exercise("locked-rollback", args.base_dir, False)
        finally:
            win.CloseHandle(ctypes.c_void_p(handle))
        exercise("upgrade", args.expected_dir, True)
        exercise("repeat", args.expected_dir, True)


if __name__ == "__main__":
    main()
