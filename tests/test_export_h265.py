#!/usr/bin/env python3
"""H265 导出回归测试 — 验证 H265/HEVC 8bit/10bit"""

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


def test_h265_exr_to_mp4(result, quick=False):
    """EXR → H265 MP4"""
    exr = MEDIA / "exr_seq" / "test_0001.exr"
    if not exr.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(exr), out, 24.0, 1, 5, codec="libx265")
        _cleanup()
        if os.path.exists(out) and os.path.getsize(out) > 1000:
            result.status = "PASS"
            result.message = f"H265 EXR: {os.path.getsize(out)//1024}KB"
        else:
            result.status = "FAIL"; result.message = "H265 export failed"


def test_h265_4k_to_mp4(result, quick=False):
    """4K H265 → H265 (同编码器)"""
    src = MEDIA / "4k_h265.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(src), out, 30.0, 0, 4, codec="libx265")
        _cleanup()
        if os.path.exists(out) and os.path.getsize(out) > 1000:
            result.status = "PASS"
            result.message = f"H265 4K: {os.path.getsize(out)//1024}KB"
        else:
            result.status = "FAIL"; result.message = "H265 4K export failed"


def test_h265_10bit_to_mp4(result, quick=False):
    """4K H265 10bit → H265 10bit"""
    src = MEDIA / "4k_h265_10bit.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(src), out, 30.0, 0, 4, codec="libx265")
        _cleanup()
        if os.path.exists(out) and os.path.getsize(out) > 1000:
            result.status = "PASS"
            result.message = f"H265 10bit: {os.path.getsize(out)//1024}KB"
        else:
            result.status = "FAIL"; result.message = "H265 10bit export failed"


def test_h265_h264_to_hevc(result, quick=False):
    """H264 → H265 转码"""
    src = MEDIA / "1080p_h264.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(src), out, 24.0, 0, 4, codec="libx265")
        _cleanup()
        if os.path.exists(out) and os.path.getsize(out) > 500:
            result.status = "PASS"
            result.message = f"H264→HEVC: {os.path.getsize(out)//1024}KB"
        else:
            result.status = "FAIL"; result.message = "Transcode failed"
