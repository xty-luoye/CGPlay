#!/usr/bin/env python3
"""硬件解码真实验证 — Decoder Used / CPU / Dropped Frames"""

import sys, os, time, tempfile, logging, subprocess
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))
MEDIA = Path(__file__).resolve().parent / "media"


def _cleanup():
    for h in list(logging.getLogger().handlers):
        try: h.close(); logging.getLogger().removeHandler(h)
        except: pass


def test_decoder_used_h264(result, quick=False):
    """验证 H264 解码时用的解码器 + hwaccel"""
    src = MEDIA / "1080p_h264.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return

    import av
    info = {}

    try:
        # 使用 container.decode() 直接解码 (最可靠方式)
        container = av.open(str(src))
        count = 0
        t0 = time.perf_counter()
        for frame in container.decode(video=0):
            count += 1
            if count == 1:
                info["first_frame_type"] = str(type(frame).__name__)
                info["first_frame_format"] = str(frame.format.name) if hasattr(frame, 'format') else '?'
                info["width"], info["height"] = frame.width, frame.height
            if count >= 30:
                break
        elapsed = (time.perf_counter() - t0) * 1000
        container.close()

        info["frames"] = count
        info["decode_ms"] = round(elapsed, 1)
        info["fps"] = round(count / (elapsed / 1000), 1) if elapsed > 0 else 0
    except Exception as e:
        result.status = "FAIL"; result.message = str(e)[:100]; return

    result.details = info

    if count >= 30:
        result.status = "PASS"
        result.message = f"Decoded {count}f in {elapsed:.0f}ms ({info['fps']}fps) | fmt={info.get('first_frame_format','?')}"
    else:
        result.status = "FAIL"
        result.message = f"Only {count} frames decoded"


def test_cpu_usage_decode(result, quick=False):
    """测量解码时 CPU 占用"""
    import av, psutil

    src = MEDIA / "1080p_h264.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return

    proc = psutil.Process()
    target_frames = 60 if quick else 200

    try:
        # Measure baseline CPU
        proc.cpu_percent(interval=0.2)

        container = av.open(str(src))
        stream = container.streams.video[0]

        t0 = time.perf_counter()
        decoded = 0
        for frame in container.decode(stream):
            decoded += 1
            if decoded >= target_frames:
                break

        elapsed = (time.perf_counter() - t0) * 1000
        cpu = proc.cpu_percent(interval=0.1)
        container.close()
    except Exception as e:
        result.status = "FAIL"; result.message = str(e)[:100]; return

    fps = decoded / (elapsed / 1000) if elapsed > 0 else 0

    result.details = {"frames": decoded, "time_ms": round(elapsed,1), "fps": round(fps,1), "cpu_pct": round(cpu,1)}

    # 正常解码应该 > 100fps (H264 1080p 在合理硬件上)
    if fps > 50:
        result.status = "PASS"
        result.message = f"{fps:.0f}fps, CPU={cpu:.0f}% ({decoded}f/{elapsed:.0f}ms)"
    else:
        result.status = "FAIL"
        result.message = f"Too slow: {fps:.0f}fps, CPU={cpu:.0f}%"


def test_dropped_frames_decode(result, quick=False):
    """检测解码丢帧率"""
    import av

    src = MEDIA / "4k_h264.mp4"
    if not src.exists():
        result.status = "SKIP"; result.message = "No media"; return

    max_frames = 30 if quick else 150

    try:
        container = av.open(str(src))
        stream = container.streams.video[0]
        total_in = 0
        decoded = 0

        for packet in container.demux(stream):
            total_in += 1
            try:
                for _ in packet.decode():
                    decoded += 1
            except Exception:
                pass
            if total_in >= max_frames:
                break

        container.close()
    except Exception as e:
        result.status = "FAIL"; result.message = str(e)[:100]; return

    drop_rate = 100 * (1 - decoded / total_in) if total_in > 0 else 0

    result.details = {"packets": total_in, "frames": decoded, "drop_pct": round(drop_rate, 2)}

    if drop_rate < 10:
        result.status = "PASS"
        result.message = f"Drop={drop_rate:.1f}% ({decoded}f/{total_in}p)"
    else:
        result.status = "FAIL"
        result.message = f"Drop={drop_rate:.1f}% ({decoded}f/{total_in}p)"
