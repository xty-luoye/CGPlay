#!/usr/bin/env python3
"""H264 导出回归测试 — 验证 EXR→H264 各种分辨率的导出"""

import sys, os, tempfile, logging
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))
from cgplay.export.export_engine import run_export

MEDIA = Path(__file__).resolve().parent / "media"


def _cleanup_logging():
    for h in list(logging.getLogger().handlers):
        try: h.close(); logging.getLogger().removeHandler(h)
        except: pass


def test_h264_exr_to_mp4(result, quick=False):
    """EXR序列 → H264 MP4（生产分辨率 2560x1080）"""
    exr = MEDIA / "exr_seq" / "test_0001.exr"
    if not exr.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(exr), out, 24.0, 1, 5)
        _cleanup_logging()
        _verify_output(result, out, 5, "h264_exr")


def test_h264_1080p_to_mp4(result, quick=False):
    """1080p H264 → H264 MP4（同编码器转码）"""
    src = MEDIA / "1080p_h264.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(src), out, 24.0, 0, 4)
        _cleanup_logging()
        _verify_output(result, out, 5, "h264_1080p")


def test_h264_4k_to_mp4(result, quick=False):
    """4K H264 → H264 MP4"""
    src = MEDIA / "4k_h264.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(src), out, 30.0, 0, 4)
        _cleanup_logging()
        _verify_output(result, out, 5, "h264_4k")


def test_h264_png_to_mp4(result, quick=False):
    """PNG序列 → H264 MP4"""
    png = MEDIA / "png_seq" / "frame.0101.png"
    if not png.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(png), out, 24.0, 101, 105)
        _cleanup_logging()
        _verify_output(result, out, 5, "h264_png")


def _verify_output(result, path, expected_frames, label):
    if not os.path.exists(path):
        result.status = "FAIL"; result.message = f"{label}: output not created"; return
    size_kb = os.path.getsize(path) // 1024
    import av
    c = av.open(path)
    s = next(s for s in c.streams if s.type == 'video')
    frames = s.frames; w = s.width; h = s.height
    c.close()
    result.details = {"frames": frames, "size_kb": size_kb, "res": f"{w}x{h}"}
    if frames == expected_frames:
        result.status = "PASS"
        result.message = f"{label}: {frames}f {w}x{h} {size_kb}KB"
    else:
        result.status = "FAIL"
        result.message = f"{label}: {frames}f≠{expected_frames}f"
