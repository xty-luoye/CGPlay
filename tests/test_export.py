#!/usr/bin/env python3
"""CGPlay Export Tests — 验证导出管线
用法: python tests/test_export.py
"""

import sys
import os
import time
import tempfile
from pathlib import Path

# Ensure tools/ is on path
TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from cgplay.export.export_engine import run_export


def test_export_exr_sequence(result, exr_path, quick=False):
    """导出 EXR 序列 → MP4，验证帧数、分辨率、文件大小"""
    with tempfile.TemporaryDirectory() as tmpdir:
        out_path = os.path.join(tmpdir, "test_output.mp4")

        # 导出少量帧进行快速测试
        frame_count = 5 if quick else 10
        start_frame = 101
        end_frame = start_frame + frame_count - 1

        output = run_export(
            exr_path, out_path, 24.0, start_frame, end_frame
        )

        # 验证输出文件
        if not os.path.exists(out_path):
            result.status = "FAIL"
            result.message = "Output file not created"
            return

        file_size = os.path.getsize(out_path)
        if file_size < 1000:
            result.status = "FAIL"
            result.message = f"Output too small: {file_size} bytes"
            return

        # 用 PyAV 验证帧数
        import av
        container = av.open(out_path)
        video_stream = next(s for s in container.streams if s.type == 'video')
        actual_frames = video_stream.frames
        container.close()

        # Close logging handlers to allow temp dir cleanup
        import logging
        for handler in logging.getLogger().handlers[:]:
            handler.close()
            logging.getLogger().removeHandler(handler)

        result.details = {
            "expected_frames": frame_count,
            "actual_frames": actual_frames,
            "file_size_kb": file_size // 1024,
            "resolution": f"{video_stream.width}x{video_stream.height}",
        }

        if actual_frames == frame_count:
            result.status = "PASS"
            result.message = f"{actual_frames}f/{frame_count}f | {file_size//1024}KB | {video_stream.width}x{video_stream.height}"
        else:
            result.status = "FAIL"
            result.message = f"Frame mismatch: got {actual_frames}, expected {frame_count}"


def test_export_video(result, video_path, quick=False):
    """导出视频 → MP4，验证帧数"""
    with tempfile.TemporaryDirectory() as tmpdir:
        out_path = os.path.join(tmpdir, "test_vid_output.mp4")
        max_frames = 5 if quick else 10

        output = run_export(
            video_path, out_path, 30.0, 0, max_frames - 1
        )

        if not os.path.exists(out_path):
            result.status = "FAIL"
            result.message = "Output file not created"
            return

        file_size = os.path.getsize(out_path)
        import av
        container = av.open(out_path)
        video_stream = next(s for s in container.streams if s.type == 'video')
        actual_frames = video_stream.frames
        container.close()

        # Close logging handlers to allow temp dir cleanup
        import logging
        for handler in logging.getLogger().handlers[:]:
            handler.close()
            logging.getLogger().removeHandler(handler)

        result.details = {
            "expected_frames": max_frames,
            "actual_frames": actual_frames,
            "file_size_kb": file_size // 1024,
            "resolution": f"{video_stream.width}x{video_stream.height}",
        }

        if actual_frames == max_frames:
            result.status = "PASS"
            result.message = f"{actual_frames}f | {file_size//1024}KB"
        else:
            result.status = "FAIL"
            result.message = f"Frame mismatch: got {actual_frames}, expected {max_frames}"


if __name__ == "__main__":
    # Quick self-test
    from run_all import TestResult
    r = TestResult("Export Self-Test")
    print("Running export self-test...")
    media_base = Path(__file__).resolve().parent.parent.parent
    exr = media_base / "v2" / "sc380.0101.exr"
    if exr.exists():
        test_export_exr_sequence(r, str(exr), quick=True)
    else:
        r.status = "SKIP"
        r.message = f"No test EXR at {exr}"
    print(f"Result: {r.status} — {r.message}")
