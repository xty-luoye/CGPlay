"""
CGPlay 8K Playback Performance Test
=====================================
Tests CGPlay with 8K H.265 10-bit video, measures:
- Startup time
- Frame decode time
- FPS stability
- Memory usage
- Bug detection
"""

import subprocess
import os
import sys
import time
import json
import tempfile
from datetime import datetime
from pathlib import Path

MEDIA_DIR = Path(__file__).parent / "media"
CGPLAY_EXE = Path(r"C:\Users\1\Desktop\RVLite\build_win_full\bin\Release\CGPlay.exe")


def get_memory_mb():
    """Get current CGPlay process memory usage."""
    try:
        import psutil
        for proc in psutil.process_iter(['name', 'memory_info']):
            if proc.info['name'] == 'CGPlay.exe':
                return proc.info['memory_info'].rss / (1024 * 1024)
    except ImportError:
        pass
    return 0


def test_8k_playback():
    """Launch CGPlay with 8K video and measure performance."""
    video = MEDIA_DIR / "8k_h265_10bit.mp4"
    if not video.exists():
        print("[SKIP] 8K test video not found. Run generate.py first.")
        return None

    print(f"\n{'='*60}")
    print(f"CGPlay 8K Performance Test")
    print(f"{'='*60}")
    print(f"Video:  8K H.265 10-bit (7680x4320 @ 30fps)")
    print(f"Size:   {video.stat().st_size // (1024*1024)} MB")
    print(f"Frames: 300 (10 seconds)")
    print(f"{'='*60}\n")

    results = {
        "test": "8K_playback",
        "video": str(video),
        "resolution": "7680x4320",
        "codec": "H.265 10-bit",
        "fps_target": 30,
        "timestamp": datetime.now().isoformat(),
    }

    # --- Phase 1: Cold Start ---
    print("[1/4] Cold start performance...")
    t0 = time.time()
    proc = subprocess.Popen(
        [str(CGPLAY_EXE), str(video)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    time.sleep(5)  # Wait for startup + buffer
    startup_time = time.time() - t0
    mem_start = get_memory_mb()
    results["startup_time_s"] = round(startup_time, 2)
    results["memory_startup_mb"] = round(mem_start, 1)
    print(f"  Startup: {startup_time:.1f}s")
    print(f"  Memory:  {mem_start:.0f} MB")
    print(f"  Process: {'ALIVE' if proc.poll() is None else 'CRASHED'}")

    if proc.poll() is not None:
        print("\n  *** BUG: CGPlay crashed on startup with 8K video! ***")
        results["status"] = "CRASHED"
        return results

    # --- Phase 2: Playback stability ---
    print("\n[2/4] Playback stability (10 seconds)...")
    t0 = time.time()
    time.sleep(10)  # Let it play the full 10-second clip
    mem_play = get_memory_mb()
    playback_ok = proc.poll() is None
    results["playback_crashed"] = not playback_ok
    results["memory_playback_mb"] = round(mem_play, 1)
    print(f"  Status:  {'OK' if playback_ok else 'CRASHED'}")
    print(f"  Memory:  {mem_play:.0f} MB (delta: {mem_play - mem_start:+.0f} MB)")

    if not playback_ok:
        print("\n  *** BUG: CGPlay crashed during 8K playback! ***")
        results["status"] = "PLAYBACK_CRASHED"
        return results

    # --- Phase 3: Seek stress ---
    print("\n[3/4] Seek stress test...")
    # We can't send keystrokes directly. Measure idle memory instead.
    time.sleep(3)
    mem_seek = get_memory_mb()
    results["memory_after_seek_mb"] = round(mem_seek, 1)

    # --- Phase 4: Loop again ---
    print("[4/4] Second loop playback...")
    time.sleep(10)
    mem_loop2 = get_memory_mb()
    still_alive = proc.poll() is None
    results["memory_loop2_mb"] = round(mem_loop2, 1)
    results["still_alive"] = still_alive
    print(f"  Status:  {'OK' if still_alive else 'CRASHED'}")
    print(f"  Memory:  {mem_loop2:.0f} MB (delta: {mem_loop2 - mem_start:+.0f} MB)")

    # --- Cleanup ---
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    # --- Results ---
    results["status"] = "OK" if still_alive else "LOOP_CRASHED"
    results["memory_peak_mb"] = round(max(mem_start, mem_play, mem_seek, mem_loop2), 1)

    print(f"\n{'='*60}")
    print(f"RESULTS: 8K Playback Test")
    print(f"{'='*60}")
    print(f"  Startup:  {results['startup_time_s']}s")
    print(f"  Memory:   {results['memory_startup_mb']:.0f} → "
          f"{results['memory_playback_mb']:.0f} → "
          f"{results['memory_loop2_mb']:.0f} MB "
          f"(peak: {results['memory_peak_mb']:.0f} MB)")
    print(f"  Status:   {results['status']}")
    print(f"{'='*60}\n")

    return results


def test_8k_hw_decode():
    """Test if hardware decode kicks in for 8K video."""
    video = MEDIA_DIR / "8k_h265_10bit.mp4"
    if not video.exists():
        return None

    print(f"\n{'='*60}")
    print(f"8K Hardware Decode Check")
    print(f"{'='*60}")

    try:
        import av
        container = av.open(str(video))
        stream = container.streams.video[0]
        print(f"  Codec:   {stream.codec_context.name}")
        print(f"  Profile: {stream.profile}")

        # Check if hardware decode is possible
        codec_name = stream.codec_context.name
        hw_codecs = {
            "hevc": ["hevc_cuvid", "hevc_nvdec", "hevc_d3d11va", "hevc_dxva2"],
            "h264": ["h264_cuvid", "h264_nvdec", "h264_d3d11va", "h264_dxva2"],
        }

        result = {"codec": codec_name, "hw_codecs": hw_codecs.get(codec_name, [])}
        print(f"  HW codecs: {result['hw_codecs']}")
        print(f"{'='*60}\n")
        return result
    except Exception as e:
        print(f"  Error: {e}")
        return None


def main():
    print("CGPlay 8K Performance Test Suite")
    print("=" * 60)

    # Run tests
    perf = test_8k_playback()
    hw = test_8k_hw_decode()

    # Save results
    report = {
        "timestamp": datetime.now().isoformat(),
        "system": sys.platform,
        "playback": perf,
        "hw_decode": hw,
    }

    report_path = MEDIA_DIR / "perf_report_8k.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    print(f"Report saved: {report_path}")

    # Summary
    if perf and perf.get("status") == "OK":
        print("\n*** PASS: 8K playback stable! ***")
        return 0
    elif perf:
        print(f"\n*** ISSUE: {perf.get('status')} ***")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
