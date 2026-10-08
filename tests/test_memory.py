#!/usr/bin/env python3
"""CGPlay Memory Tests — 检测内存泄漏
用法: python tests/test_memory.py
"""

import sys
import os
import time
import tempfile
import psutil
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))


def _get_memory_mb():
    """获取当前进程内存使用量 (MB)"""
    try:
        proc = psutil.Process()
        return proc.memory_info().rss / (1024 * 1024)
    except Exception:
        return -1


def test_memory_export(result, exr_path, quick=False):
    """多次导出时内存增长检测"""
    from cgplay.export.export_engine import run_export

    iterations = 3 if quick else 5
    mb_before = _get_memory_mb()

    if mb_before < 0:
        result.status = "SKIP"
        result.message = "psutil not available"
        return

    mb_measurements = [mb_before]

    # Use manual temp dir (ignore PermissionError on cleanup caused by log files)
    import shutil, logging
    tmpdir = os.path.join(tempfile.gettempdir(), f"cgplay_test_{os.getpid()}_{int(time.time())}")
    os.makedirs(tmpdir, exist_ok=True)

    try:
        for i in range(iterations):
            out_path = os.path.join(tmpdir, f"mem_test_{i}.mp4")
            run_export(exr_path, out_path, 24.0, 101, 103)
            mb_measurements.append(_get_memory_mb())
    finally:
        # Close all logging handlers
        for handler in list(logging.getLogger().handlers):
            try:
                handler.close()
                logging.getLogger().removeHandler(handler)
            except Exception:
                pass
        # Cleanup (ignore errors from still-open log files)
        try:
            shutil.rmtree(tmpdir)
        except PermissionError:
            pass

    mb_after = mb_measurements[-1]
    growth = mb_after - mb_before

    result.details = {
        "iterations": iterations,
        "mb_before": round(mb_before, 1),
        "mb_after": round(mb_after, 1),
        "growth_mb": round(growth, 1),
        "measurements": [round(m, 1) for m in mb_measurements],
    }

    # Allow up to 50MB growth (Python GC variance)
    if growth < 50:
        result.status = "PASS"
        result.message = f"+{growth:.1f}MB over {iterations} exports (OK)"
    else:
        result.status = "FAIL"
        result.message = f"Memory leak? +{growth:.1f}MB over {iterations} exports"


def test_memory_baseline(result, quick=False):
    """测量基线内存占用"""
    mb = _get_memory_mb()
    if mb < 0:
        result.status = "SKIP"
        result.message = "psutil not available"
        return

    result.details = {"baseline_mb": round(mb, 1)}

    # Python process with numpy/OIIO loaded should be < 500MB
    if mb < 500:
        result.status = "PASS"
        result.message = f"Baseline: {mb:.1f}MB"
    else:
        result.status = "FAIL"
        result.message = f"High baseline: {mb:.1f}MB"


if __name__ == "__main__":
    from run_all import TestResult
    r1 = TestResult("Memory Baseline")
    test_memory_baseline(r1)
    print(f"Baseline: {r1.status} — {r1.message}")

    media_base = Path(__file__).resolve().parent.parent.parent
    exr = media_base / "v2" / "sc380.0101.exr"
    if exr.exists():
        r2 = TestResult("Memory Export")
        test_memory_export(r2, str(exr), quick=True)
        print(f"Export: {r2.status} — {r2.message}")
