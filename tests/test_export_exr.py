#!/usr/bin/env python3
"""EXR/PNG/JPG 序列导出回归测试"""

import sys, os, tempfile, logging
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))
from cgplay.export.export_engine import run_export

MEDIA = Path(__file__).resolve().parent / "media"


def _cleanup():
    for h in list(logging.getLogger().handlers):
        try: h.close(); logging.getLogger().removeHandler(h)
        except: pass


def test_exr_large_seq(result, quick=False):
    """EXR 多帧序列导出 (27帧→H264)"""
    exr = MEDIA / "exr_seq" / "test_0001.exr"
    if not exr.exists():
        result.status = "SKIP"; result.message = "No media"; return

    frames = min(27, len(list((MEDIA / "exr_seq").glob("*.exr"))))

    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "exr_large.mp4")
        run_export(str(exr), out, 24.0, 1, frames)
        _cleanup()

        if not os.path.exists(out):
            result.status = "FAIL"; result.message = "No output"; return

        import av
        c = av.open(out)
        s = next(s for s in c.streams if s.type == 'video')
        actual = s.frames
        c.close()

        result.details = {"expected": frames, "actual": actual, "size_kb": os.path.getsize(out)//1024}
        if actual == frames:
            result.status = "PASS"
            result.message = f"EXR {frames}f: {s.width}x{s.height}"
        else:
            result.status = "FAIL"
            result.message = f"Expected {frames}f, got {actual}f"


def test_png_seq_export(result, quick=False):
    """PNG序列 → H264"""
    png = MEDIA / "png_seq" / "frame.0101.png"
    if not png.exists():
        result.status = "SKIP"; result.message = "No media"; return
    import shutil
    d = os.path.join(tempfile.gettempdir(), f"cgplay_png_test_{os.getpid()}")
    os.makedirs(d, exist_ok=True)
    try:
        out = os.path.join(d, "png_out.mp4")
        run_export(str(png), out, 24.0, 101, 127)
        _cleanup()
        import av
        ok = os.path.exists(out) and os.path.getsize(out) > 500
        result.status = "PASS" if ok else "FAIL"
        if ok:
            c = av.open(out); s = c.streams.video[0]; c.close()
            result.message = f"PNG→H264: {s.frames}f {os.path.getsize(out)//1024}KB"
        else:
            result.message = "PNG export failed"
    finally:
        _cleanup()
        try: shutil.rmtree(d)
        except PermissionError: pass


def test_jpg_seq_export(result, quick=False):
    """JPG序列 → H264"""
    jpg = MEDIA / "jpg_seq" / "frame.0101.jpg"
    if not jpg.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "jpg_out.mp4")
        run_export(str(jpg), out, 24.0, 101, 127)
        _cleanup()
        ok = os.path.exists(out) and os.path.getsize(out) > 500
        result.status = "PASS" if ok else "FAIL"
        if ok:
            result.message = f"JPG→H264: {os.path.getsize(out)//1024}KB"
        else:
            result.message = "JPG export failed"
