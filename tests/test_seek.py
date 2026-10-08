#!/usr/bin/env python3
"""CGPlay Seek Tests — 验证随机/顺序 Seek 延迟
用法: python tests/test_seek.py
"""

import sys
import os
import time
import random
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))


def test_seek_random(result, exr_path, quick=False):
    """随机 Seek 延迟测试 (OIIO 层面)"""
    import numpy as np
    import OpenImageIO as oiio

    # 扫描序列
    from cgplay.export.utils.image_reader import ImageReader
    reader = ImageReader()
    frames = reader.scan_sequence(exr_path)
    frame_nums = sorted(frames.keys())
    frame_count = len(frame_nums)

    if frame_count < 5:
        result.status = "SKIP"
        result.message = f"Too few frames: {frame_count}"
        return

    iterations = 10 if quick else 30
    seek_times = []

    for _ in range(iterations):
        fnum = random.choice(frame_nums)
        t0 = time.perf_counter()
        img = oiio.ImageInput.open(frames[fnum])
        data = img.read_image(oiio.FLOAT)
        img.close()
        seek_times.append((time.perf_counter() - t0) * 1000)

    avg_ms = sum(seek_times) / len(seek_times)
    max_ms = max(seek_times)

    result.details = {
        "iterations": iterations,
        "frame_count": frame_count,
        "avg_ms": round(avg_ms, 2),
        "max_ms": round(max_ms, 2),
        "min_ms": round(min(seek_times), 2),
    }

    # EXR read should be < 250ms for reasonable performance
    if avg_ms < 300:
        result.status = "PASS"
        result.message = f"avg={avg_ms:.1f}ms max={max_ms:.1f}ms ({iterations} seeks)"
    else:
        result.status = "FAIL"
        result.message = f"Seek too slow: avg={avg_ms:.1f}ms > 300ms"


def test_seek_sequential(result, exr_path, quick=False):
    """顺序 Seek 延迟测试"""
    import numpy as np
    import OpenImageIO as oiio

    from cgplay.export.utils.image_reader import ImageReader
    reader = ImageReader()
    frames = reader.scan_sequence(exr_path)
    frame_nums = sorted(frames.keys())

    if len(frame_nums) < 5:
        result.status = "SKIP"
        result.message = "Too few frames"
        return

    # Pick continuous range of frames
    count = 5 if quick else len(frame_nums)
    sub_nums = frame_nums[:count]
    seek_times = []

    for fnum in sub_nums:
        t0 = time.perf_counter()
        img = oiio.ImageInput.open(frames[fnum])
        data = img.read_image(oiio.FLOAT)
        img.close()
        seek_times.append((time.perf_counter() - t0) * 1000)

    avg_ms = sum(seek_times) / len(seek_times)

    result.details = {
        "frame_count": count,
        "avg_ms": round(avg_ms, 2),
        "total_ms": round(sum(seek_times), 2),
    }

    if avg_ms < 300:
        result.status = "PASS"
        result.message = f"avg={avg_ms:.1f}ms ({count} frames)"
    else:
        result.status = "FAIL"
        result.message = f"avg={avg_ms:.1f}ms too slow"


if __name__ == "__main__":
    from run_all import TestResult
    r = TestResult("Seek Self-Test")
    media_base = Path(__file__).resolve().parent.parent.parent
    exr = media_base / "v2" / "sc380.0101.exr"
    if exr.exists():
        test_seek_random(r, str(exr), quick=True)
    else:
        r.status = "SKIP"
        r.message = "No test EXR"
    print(f"Result: {r.status} — {r.message}")
