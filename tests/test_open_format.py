#!/usr/bin/env python3
"""CGPlay 格式打开测试 — 验证各格式能否被 tlRender/OIIO 加载
测试所有素材：EXR/PNG/JPG/MP4/MOV
"""

import sys, os, time
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))
MEDIA = Path(__file__).resolve().parent / "media"


def test_open_exr(result, quick=False):
    """OIIO 能否打开 EXR"""
    import OpenImageIO as oiio
    exr = MEDIA / "exr_seq" / "test_0001.exr"
    if not exr.exists():
        result.status = "SKIP"; result.message = "No EXR"; return
    t0 = time.perf_counter()
    img = oiio.ImageInput.open(str(exr))
    if not img:
        result.status = "FAIL"; result.message = "OIIO cannot open EXR"; return
    spec = img.spec()
    img.close()
    ms = (time.perf_counter() - t0) * 1000
    result.status = "PASS"
    result.message = f"EXR {spec.width}x{spec.height} ({ms:.0f}ms)"


def test_open_png(result, quick=False):
    """OIIO 能否打开 PNG"""
    import OpenImageIO as oiio
    png = MEDIA / "png_seq" / "frame.0101.png"
    if not png.exists():
        result.status = "SKIP"; result.message = "No PNG"; return
    t0 = time.perf_counter()
    img = oiio.ImageInput.open(str(png))
    if not img:
        result.status = "FAIL"; result.message = "OIIO cannot open PNG"; return
    spec = img.spec()
    img.close()
    ms = (time.perf_counter() - t0) * 1000
    result.status = "PASS"
    result.message = f"PNG {spec.width}x{spec.height} ({ms:.0f}ms)"


def test_open_jpg(result, quick=False):
    """OIIO 能否打开 JPG"""
    import OpenImageIO as oiio
    jpg = MEDIA / "jpg_seq" / "frame.0101.jpg"
    if not jpg.exists():
        result.status = "SKIP"; result.message = "No JPG"; return
    t0 = time.perf_counter()
    img = oiio.ImageInput.open(str(jpg))
    if not img:
        result.status = "FAIL"; result.message = "OIIO cannot open JPG"; return
    spec = img.spec()
    img.close()
    ms = (time.perf_counter() - t0) * 1000
    result.status = "PASS"
    result.message = f"JPG {spec.width}x{spec.height} ({ms:.0f}ms)"


def test_open_via_ffmpeg(result, format_name, path, quick=False):
    """通过 PyAV/FFmpeg 打开文件，模拟 tlRender 解码"""
    import av
    if not path.exists():
        result.status = "SKIP"; result.message = f"No {format_name}"
        return

    try:
        container = av.open(str(path))
        # Check what demuxer was used
        fmt_name = container.format.name if container.format else "?"
        video_stream = None
        for s in container.streams:
            if s.type == 'video':
                video_stream = s
                break

        if video_stream is None:
            result.status = "FAIL"
            result.message = f"{format_name}: no video stream found"
            container.close()
            return

        # Try decoding one frame
        count = 0
        for frame in container.decode(video_stream):
            count += 1
            w, h = frame.width, frame.height
            if count >= 3:
                break

        container.close()

        if count > 0:
            result.status = "PASS"
            result.message = f"{format_name}: {fmt_name} {w}x{h} → {count} frames decoded"
        else:
            result.status = "FAIL"
            result.message = f"{format_name}: no frames decoded from {video_stream.codec.name}"

    except Exception as e:
        result.status = "FAIL"
        result.message = f"{format_name}: {str(e)[:120]}"


def test_tlrender_png_sequence(result, quick=False):
    """模拟 tlRender 的 image2 demuxer：PNG 序列模式匹配"""
    import av
    png_seq = MEDIA / "png_seq"

    if not png_seq.exists():
        result.status = "SKIP"; result.message = "No PNG seq"; return

    # tlRender 用 image2 demuxer 读取图像序列
    # FFmpeg expects: frame.%04d.png
    pattern = str(png_seq / "frame.%04d.png")
    try:
        container = av.open(pattern, format="image2",
                            options={"framerate": "24", "start_number": "101"})
        count = 0
        for frame in container.decode(video=0):
            count += 1
            w, h = frame.width, frame.height
            if count >= 5:
                break
        container.close()

        if count > 0:
            result.status = "PASS"
            result.message = f"PNG seq: {w}x{h} → {count}+ frames via image2"
        else:
            result.status = "FAIL"
            result.message = "PNG seq: no frames via image2 demuxer"
    except Exception as e:
        result.status = "FAIL"
        result.message = f"PNG seq image2: {str(e)[:120]}"


def test_tlrender_png_single(result, quick=False):
    """单张 PNG 能否被 FFmpeg 打开（模拟 tlRender 打开单张）"""
    import av
    png = MEDIA / "png_seq" / "frame.0101.png"
    return test_open_via_ffmpeg(result, "PNG single", png, quick)
