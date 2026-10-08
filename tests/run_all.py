#!/usr/bin/env python3
"""CGPlay Automated Test Suite — run_all.py
用法: python tests/run_all.py [--quick] [--verbose] [--html report.html]
"""

import sys
import os
import time
import json
import argparse
import subprocess
import traceback
from pathlib import Path
from datetime import datetime

# Ensure tools/ is on path for cgplay.export imports
TESTS_DIR = Path(__file__).resolve().parent
ROOT_DIR = TESTS_DIR.parent
TOOLS_DIR = ROOT_DIR / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

# ─── Test Framework ───────────────────────────────────────────────────────
class TestResult:
    def __init__(self, name):
        self.name = name
        self.status = "PENDING"  # PASS / FAIL / SKIP / ERROR
        self.duration_ms = 0
        self.message = ""
        self.details = {}
        self.traceback = ""

    def to_dict(self):
        return {
            "name": self.name,
            "status": self.status,
            "duration_ms": round(self.duration_ms, 1),
            "message": self.message,
            "details": self.details,
        }


class TestSuite:
    def __init__(self, quick_mode=False, verbose=False):
        self.results = []
        self.quick_mode = quick_mode
        self.verbose = verbose
        self.start_time = time.time()

    def run(self, name, func, *args, **kwargs):
        result = TestResult(name)
        t0 = time.perf_counter()
        try:
            func(result, *args, **kwargs)
        except Exception as e:
            result.status = "ERROR"
            result.message = str(e)[:200]
            result.traceback = traceback.format_exc()
        result.duration_ms = (time.perf_counter() - t0) * 1000
        self.results.append(result)

        # Print result
        icon = {"PASS": "[OK]", "FAIL": "[XX]", "SKIP": "[--]", "ERROR": "[EE]"}.get(result.status, "[??]")
        extra = f" — {result.message}" if result.message else ""
        if result.status != "PASS" or self.verbose:
            print(f"  {icon} {result.name} ({result.duration_ms:.0f}ms){extra}")
        else:
            print(f"  {icon} {result.name} ({result.duration_ms:.0f}ms)")

        if result.traceback:
            print(f"      {result.traceback.strip().split(chr(10))[-1]}")

    def summary(self):
        total = len(self.results)
        passed = sum(1 for r in self.results if r.status == "PASS")
        failed = sum(1 for r in self.results if r.status in ("FAIL", "ERROR"))
        skipped = sum(1 for r in self.results if r.status == "SKIP")
        elapsed = time.time() - self.start_time

        print(f"\n{'='*60}")
        print(f"  Results: {passed}/{total} PASS | {failed} FAIL | {skipped} SKIP")
        print(f"  Duration: {elapsed:.1f}s")
        print(f"{'='*60}")

        # Summary table
        for r in self.results:
            if r.status != "PASS":
                icon = {"FAIL": "[XX]", "ERROR": "[EE]", "SKIP": "[--]"}.get(r.status, "[??]")
                print(f"  {icon} {r.name} — {r.message}")

        return 0 if failed == 0 else 1

    def export_html(self, path):
        total = len(self.results)
        passed = sum(1 for r in self.results if r.status == "PASS")
        rows = ""
        for r in self.results:
            color = {"PASS": "#4caf50", "FAIL": "#f44336", "ERROR": "#ff9800", "SKIP": "#9e9e9e"}.get(r.status, "#9e9e9e")
            rows += f"""<tr>
                <td style="color:{color};font-weight:bold">{r.status}</td>
                <td>{r.name}</td>
                <td>{r.duration_ms:.0f}ms</td>
                <td>{r.message}</td>
            </tr>"""

        html = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>CGPlay Test Report</title>
<style>
body {{ font-family: Segoe UI, sans-serif; background: #1e1e1e; color: #ccc; padding: 20px; }}
h1 {{ color: #007acc; }}
table {{ border-collapse: collapse; width: 100%; }}
th, td {{ padding: 8px 12px; text-align: left; border-bottom: 1px solid #333; }}
th {{ background: #2d2d2d; color: #fff; }}
.pass {{ color: #4caf50; font-weight: bold; }}
.fail {{ color: #f44336; font-weight: bold; }}
.summary {{ font-size: 18px; margin: 10px 0; }}
</style></head><body>
<h1>CGPlay Test Report</h1>
<p>{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>
<p class="summary">{passed}/{total} PASS</p>
<table>
<tr><th>Status</th><th>Test</th><th>Duration</th><th>Message</th></tr>
{rows}
</table>
</body></html>"""

        with open(path, "w", encoding="utf-8") as f:
            f.write(html)
        print(f"\nReport saved: {path}")


# ─── Test Data Discovery ──────────────────────────────────────────────────
def find_test_media():
    """Find available test media (tests/media/ or fallback to desktop)"""
    MEDIA_DIR = TESTS_DIR / "media"
    found = {}

    # Prefer generated media
    candidates = [
        (MEDIA_DIR / "exr_seq" / "test_0001.exr", "exr"),
        (MEDIA_DIR / "1080p_h264.mp4", "video"),
    ]
    for p, key in candidates:
        if p.exists():
            found[key] = str(p)

    # Fallback to desktop files
    if "exr" not in found:
        fallback = ROOT_DIR.parent / "v2" / "sc380.0101.exr"
        if fallback.exists(): found["exr"] = str(fallback)
    if "video" not in found:
        fallback = ROOT_DIR.parent / "bbb_sunflower_2160p_30fps_normal.mp4"
        if fallback.exists(): found["video"] = str(fallback)

    return found


# ─── Test Modules ─────────────────────────────────────────────────────────
# Import test modules dynamically
def load_test_modules():
    sys.path.insert(0, str(TESTS_DIR))
    modules = {}
    for mod_name in ["test_export", "test_seek", "test_memory", "test_decoder"]:
        try:
            modules[mod_name] = __import__(mod_name)
        except ImportError as e:
            print(f"  Warning: Cannot import {mod_name}: {e}")
    return modules


# ─── Main ─────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="CGPlay Automated Test Suite")
    parser.add_argument("--quick", action="store_true", help="Quick mode (fewer frames)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    parser.add_argument("--html", type=str, help="Export HTML report")
    parser.add_argument("--test", type=str, help="Run specific test only")
    args = parser.parse_args()

    print("=" * 60)
    print("  CGPlay Automated Test Suite")
    print(f"  {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"  Python: {sys.version.split()[0]}")
    print("=" * 60)

    suite = TestSuite(quick_mode=args.quick, verbose=args.verbose)
    q = args.quick

    # ════════════════════════════════════════════════════════
    # 1. Environment Check
    # ════════════════════════════════════════════════════════
    print("\n[1/8] Environment Check")
    suite.run("Python 3.11+", check_python)
    suite.run("OpenImageIO", check_oiio)
    suite.run("PyAV", check_pyav)
    suite.run("NumPy", check_numpy)

    # ════════════════════════════════════════════════════════
    # 2. Format Open Tests (验证各格式能否打开)
    # ════════════════════════════════════════════════════════
    print("\n[2/9] Format Open Tests")
    from test_open_format import (
        test_open_exr, test_open_png, test_open_jpg,
        test_tlrender_png_sequence, test_tlrender_png_single
    )
    suite.run("Open: EXR (OIIO)", test_open_exr, q)
    suite.run("Open: PNG (OIIO)", test_open_png, q)
    suite.run("Open: JPG (OIIO)", test_open_jpg, q)
    suite.run("Open: PNG single (FFmpeg)", test_tlrender_png_single, q)
    suite.run("Open: PNG seq (FFmpeg image2)", test_tlrender_png_sequence, q)

    # ════════════════════════════════════════════════════════
    # 3. H264 Export Regression
    # ════════════════════════════════════════════════════════
    print("\n[2/8] H264 Export Regression")
    from test_export_h264 import (
        test_h264_exr_to_mp4, test_h264_1080p_to_mp4,
        test_h264_4k_to_mp4, test_h264_png_to_mp4
    )
    suite.run("H264: EXR→MP4", test_h264_exr_to_mp4, q)
    suite.run("H264: 1080p→MP4", test_h264_1080p_to_mp4, q)
    suite.run("H264: 4K→MP4", test_h264_4k_to_mp4, q)
    suite.run("H264: PNG→MP4", test_h264_png_to_mp4, q)

    # ════════════════════════════════════════════════════════
    # 3. H265 Export Regression
    # ════════════════════════════════════════════════════════
    print("\n[4/9] H265 Export Regression")
    from test_export_h265 import (
        test_h265_exr_to_mp4, test_h265_4k_to_mp4,
        test_h265_10bit_to_mp4, test_h265_h264_to_hevc
    )
    suite.run("H265: EXR→HEVC", test_h265_exr_to_mp4, q)
    suite.run("H265: 4K→HEVC", test_h265_4k_to_mp4, q)
    suite.run("H265: 10bit→HEVC", test_h265_10bit_to_mp4, q)
    suite.run("H265: H264→HEVC", test_h265_h264_to_hevc, q)

    # ════════════════════════════════════════════════════════
    # 5. ProRes Export Regression
    # ════════════════════════════════════════════════════════
    print("\n[5/9] ProRes Export Regression")
    from test_export_prores import (
        test_prores_exr_422, test_prores_exr_4444,
        test_prores_source_to_h264, test_png_to_prores
    )
    suite.run("ProRes: EXR→422", test_prores_exr_422, q)
    suite.run("ProRes: EXR→4444", test_prores_exr_4444, q)
    suite.run("ProRes: Src→H264", test_prores_source_to_h264, q)
    suite.run("ProRes: PNG→ProRes", test_png_to_prores, q)

    # ════════════════════════════════════════════════════════
    # 6. EXR/PNG/JPG Export Regression
    # ════════════════════════════════════════════════════════
    print("\n[6/9] EXR/PNG/JPG Export Regression")
    from test_export_exr import (
        test_exr_large_seq, test_png_seq_export, test_jpg_seq_export
    )
    suite.run("EXR: 27f Sequence", test_exr_large_seq, q)
    suite.run("PNG: Seq→H264", test_png_seq_export, q)
    suite.run("JPG: Seq→H264", test_jpg_seq_export, q)

    # ════════════════════════════════════════════════════════
    # 7. Seek Tests
    # ════════════════════════════════════════════════════════
    print("\n[7/9] Seek Tests")
    media = find_test_media()
    from test_seek import test_seek_random, test_seek_sequential
    if "exr" in media:
        suite.run("Random Seek", test_seek_random, media["exr"], q)
        suite.run("Sequential Seek", test_seek_sequential, media["exr"], q)
    else:
        suite.run("Seek Tests", skip_test, "No EXR data")

    # ════════════════════════════════════════════════════════
    # 8. Memory Test
    # ════════════════════════════════════════════════════════
    print("\n[8/9] Memory Test")
    from test_memory import test_memory_export
    if "exr" in media:
        suite.run("Memory (Export)", test_memory_export, media["exr"], q)
    else:
        suite.run("Memory (Export)", skip_test, "No test data")

    # ════════════════════════════════════════════════════════
    # 9. Hardware Decode Verification
    # ════════════════════════════════════════════════════════
    print("\n[9/9] Hardware Decode Verification")
    from test_decoder import test_gpu_detection, test_hwaccel_config
    suite.run("GPU Detection", test_gpu_detection)
    suite.run("HwAccel Config", test_hwaccel_config)

    from test_hwdecode_real import (
        test_decoder_used_h264, test_cpu_usage_decode, test_dropped_frames_decode
    )
    suite.run("Decoder Used (H264)", test_decoder_used_h264, q)
    suite.run("CPU Usage (SW vs HW)", test_cpu_usage_decode, q)
    suite.run("Dropped Frames (4K)", test_dropped_frames_decode, q)

    # Summary
    exit_code = suite.summary()

    if args.html:
        suite.export_html(args.html)

    sys.exit(exit_code)


# ─── Built-in Checks ─────────────────────────────────────────────────────
def check_python(result):
    ver = tuple(map(int, sys.version.split(".")[:2]))
    if ver >= (3, 11):
        result.status = "PASS"
        result.message = f"Python {sys.version.split()[0]}"
    else:
        result.status = "FAIL"
        result.message = f"Need 3.11+, got {sys.version}"

def check_oiio(result):
    try:
        import OpenImageIO
        result.status = "PASS"
        result.message = f"OIIO {OpenImageIO.__version__}"
    except ImportError:
        result.status = "FAIL"
        result.message = "OpenImageIO not installed"

def check_pyav(result):
    try:
        import av
        result.status = "PASS"
        result.message = f"PyAV {av.__version__}"
    except ImportError:
        result.status = "FAIL"
        result.message = "PyAV not installed"

def check_numpy(result):
    try:
        import numpy
        result.status = "PASS"
        result.message = f"NumPy {numpy.__version__}"
    except ImportError:
        result.status = "FAIL"
        result.message = "NumPy not installed"

def skip_test(result, reason):
    result.status = "SKIP"
    result.message = reason


if __name__ == "__main__":
    main()
