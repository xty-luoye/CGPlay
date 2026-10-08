#!/usr/bin/env python3
"""ProRes 导出回归测试 — EXR→ProRes 422/4444"""

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


def test_prores_exr_422(result, quick=False):
    """EXR → ProRes 422"""
    exr = MEDIA / "exr_seq" / "test_0001.exr"
    if not exr.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mov")
        run_export(str(exr), out, 24.0, 1, 5, codec="prores_422")
        _cleanup()
        ok = os.path.exists(out) and os.path.getsize(out) > 500
        result.status = "PASS" if ok else "FAIL"
        result.message = f"ProRes 422: {os.path.getsize(out)//1024}KB" if ok else "ProRes failed"


def test_prores_exr_4444(result, quick=False):
    """EXR → ProRes 4444"""
    exr = MEDIA / "exr_seq" / "test_0001.exr"
    if not exr.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mov")
        run_export(str(exr), out, 24.0, 1, 5, codec="prores_4444")
        _cleanup()
        ok = os.path.exists(out) and os.path.getsize(out) > 500
        result.status = "PASS" if ok else "FAIL"
        result.message = f"ProRes 4444: {os.path.getsize(out)//1024}KB" if ok else "ProRes 4444 failed"


def test_prores_source_to_h264(result, quick=False):
    """ProRes源 → H264 转码"""
    src = MEDIA / "prores_1080p.mov"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mp4")
        run_export(str(src), out, 24.0, 0, 4)
        _cleanup()
        ok = os.path.exists(out) and os.path.getsize(out) > 500
        result.status = "PASS" if ok else "FAIL"
        result.message = f"ProRes→H264: {os.path.getsize(out)//1024}KB" if ok else "Transcode failed"


def test_png_to_prores(result, quick=False):
    """PNG序列 → ProRes"""
    png = MEDIA / "png_seq" / "frame.0101.png"
    if not png.exists():
        result.status = "SKIP"; result.message = "No media"; return
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.mov")
        run_export(str(png), out, 24.0, 101, 105, codec="prores_422")
        _cleanup()
        ok = os.path.exists(out) and os.path.getsize(out) > 500
        result.status = "PASS" if ok else "FAIL"
        result.message = f"PNG→ProRes: {os.path.getsize(out)//1024}KB" if ok else "Failed"
