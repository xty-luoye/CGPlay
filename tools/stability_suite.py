#!/usr/bin/env python3
"""CGPlay stability smoke suite.

This script verifies the packaged player, bundled tools, media probing,
playback benchmark, UI capture, and optional UI overlay comparison.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MEDIA = ROOT / "tests" / "media"
BIN = ROOT / "build_win_full" / "bin" / "Release"
PACKAGE = ROOT / "build_win_full" / "package" / "CGPlay"
DEFAULT_EXE = PACKAGE / "CGPlay.exe"


def run_cmd(args: list[str], timeout: int = 60) -> tuple[int, str, str, float]:
    start = time.perf_counter()
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    elapsed = time.perf_counter() - start
    return proc.returncode, proc.stdout, proc.stderr, elapsed


def add_result(results: list[dict], name: str, status: str, message: str = "", **details) -> None:
    results.append({
        "name": name,
        "status": status,
        "message": message,
        "details": details,
    })


def probe_media(ffprobe: Path, media_path: Path) -> dict:
    args = [
        str(ffprobe),
        "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,width,height,pix_fmt,avg_frame_rate,nb_frames,duration",
        "-of", "json",
        str(media_path),
    ]
    code, out, err, elapsed = run_cmd(args, timeout=30)
    if code != 0:
        return {"ok": False, "error": err.strip(), "elapsed_s": elapsed}
    data = json.loads(out)
    streams = data.get("streams") or []
    if not streams:
        return {"ok": False, "error": "no video stream", "elapsed_s": elapsed}
    stream = streams[0]
    return {"ok": True, "stream": stream, "elapsed_s": elapsed}


def run_benchmark(exe: Path, media_path: Path, out_path: Path, quick: bool, timeout: int | None = None) -> dict:
    duration = "1500" if quick else "8000"
    warmup = "800" if quick else "2000"
    args = [
        str(exe),
        "--benchmark-playback", str(media_path),
        "--benchmark-output", str(out_path),
        "--benchmark-warmup-ms", warmup,
        "--benchmark-duration-ms", duration,
    ]
    if timeout is None:
        timeout = 45 if quick else 90
    code, out, err, elapsed = run_cmd(args, timeout=timeout)
    report = {}
    if out_path.exists():
        report = json.loads(out_path.read_text(encoding="utf-8"))
    return {
        "ok": code == 0 and out_path.exists(),
        "exit_code": code,
        "stdout": out[-1000:],
        "stderr": err[-1000:],
        "elapsed_s": elapsed,
        "report": report,
    }


def capture_ui(
    exe: Path,
    out_path: Path,
    media_path: Path | None,
    delay_ms: int,
    width: int,
    height: int,
    demo: bool = False,
) -> dict:
    args = [
        str(exe),
        "--capture-ui",
        "--capture-output", str(out_path),
        "--capture-delay-ms", str(delay_ms),
        "--capture-width", str(width),
        "--capture-height", str(height),
    ]
    if demo:
        args.append("--capture-demo")
    if media_path:
        args.extend(["--capture-media", str(media_path)])
    code, out, err, elapsed = run_cmd(args, timeout=45)
    return {
        "ok": code == 0 and out_path.exists() and out_path.stat().st_size > 0,
        "exit_code": code,
        "stdout": out[-1000:],
        "stderr": err[-1000:],
        "elapsed_s": elapsed,
        "output": str(out_path),
        "size": out_path.stat().st_size if out_path.exists() else 0,
    }


def compare_ui(current: Path, reference: Path, output_dir: Path) -> dict:
    script = ROOT / "tools" / "ui_overlay_compare.py"
    args = [
        sys.executable,
        str(script),
        "--current", str(current),
        "--reference", str(reference),
        "--output-dir", str(output_dir),
    ]
    code, out, err, elapsed = run_cmd(args, timeout=30)
    report_path = output_dir / "ui_compare_report.json"
    report = {}
    if report_path.exists():
        report = json.loads(report_path.read_text(encoding="utf-8"))
    return {
        "ok": code == 0 and report_path.exists(),
        "exit_code": code,
        "stdout": out[-1000:],
        "stderr": err[-1000:],
        "elapsed_s": elapsed,
        "report": report,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run CGPlay stability smoke checks.")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--reference-ui", type=Path, default=Path(r"C:\Users\1\Desktop\UI.png"))
    parser.add_argument("--output-dir", type=Path, default=ROOT / "tests" / "reports" / f"stability_{datetime.now().strftime('%Y%m%d_%H%M%S')}")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--skip-benchmark", action="store_true")
    parser.add_argument("--skip-ui", action="store_true")
    parser.add_argument("--ui-no-media", action="store_true", help="Capture UI without opening media.")
    parser.add_argument("--ui-demo", action="store_true", help="Capture a populated demo UI state using real test media.")
    parser.add_argument("--ui-width", type=int, default=1608)
    parser.add_argument("--ui-height", type=int, default=710)
    args = parser.parse_args()

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    results: list[dict] = []

    exe = args.exe
    ffprobe = exe.parent / "ffprobe.exe"
    ffmpeg = exe.parent / "ffmpeg.exe"

    add_result(results, "CGPlay.exe exists", "PASS" if exe.exists() else "FAIL", str(exe), size=exe.stat().st_size if exe.exists() else 0)
    add_result(results, "ffprobe.exe bundled", "PASS" if ffprobe.exists() else "FAIL", str(ffprobe))
    add_result(results, "ffmpeg.exe bundled", "PASS" if ffmpeg.exists() else "FAIL", str(ffmpeg))

    media_targets = [
        MEDIA / "1080p_h264.mp4",
        MEDIA / "4k_60fps.mp4",
        MEDIA / "8k_60fps.mp4",
    ]
    for media_path in media_targets:
        if not media_path.exists():
            add_result(results, f"Media exists: {media_path.name}", "SKIP", "missing")
            continue
        probe = probe_media(ffprobe, media_path)
        status = "PASS" if probe.get("ok") else "FAIL"
        msg = ""
        if probe.get("ok"):
            stream = probe["stream"]
            msg = f"{stream.get('codec_name')} {stream.get('width')}x{stream.get('height')} {stream.get('avg_frame_rate')} {stream.get('nb_frames')}f"
        else:
            msg = probe.get("error", "")
        add_result(results, f"ffprobe: {media_path.name}", status, msg, probe=probe)

    exr_first = MEDIA / "exr_seq" / "test_0001.exr"
    exr_count = len(list((MEDIA / "exr_seq").glob("*.exr"))) if (MEDIA / "exr_seq").exists() else 0
    add_result(results, "EXR sequence fixture", "PASS" if exr_first.exists() and exr_count > 1 else "FAIL", f"{exr_count} frames", first=str(exr_first))

    if not args.skip_benchmark and (MEDIA / "4k_60fps.mp4").exists():
        bench_path = output_dir / "playback_benchmark_4k60.json"
        bench = run_benchmark(exe, MEDIA / "4k_60fps.mp4", bench_path, args.quick)
        report = bench.get("report", {})
        avg = report.get("playback_stats", {}).get("avg_fps", 0)
        render = report.get("render_fps", 0)
        status = "PASS" if bench["ok"] and avg > 0 and render > 0 else "FAIL"
        add_result(results, "Playback benchmark 4K60", status, f"avg={avg:.2f}, render={render:.2f}", benchmark=bench)

    alpha_mov = ROOT / "tests" / "fixtures" / "alpha_mov" / "alpha_png_rgba.mov"
    if not args.skip_benchmark:
        if alpha_mov.exists():
            bench_path = output_dir / "playback_benchmark_alpha_png_rgba_mov.json"
            # First run may build a ProRes 4444 cache, so this timeout is intentionally longer.
            bench = run_benchmark(exe, alpha_mov, bench_path, args.quick, timeout=300)
            report = bench.get("report", {})
            media_fps = report.get("media_fps", 0)
            total_frames = report.get("total_frames", 0)
            frame_events = report.get("frame_events", 0)
            dropped = report.get("playback_stats", {}).get("dropped_frames", 0)
            status = "PASS" if bench["ok"] and media_fps > 0 and total_frames > 0 and frame_events > 0 else "FAIL"
            msg = f"fps={media_fps}, frames={total_frames}, events={frame_events}, dropped={dropped}"
            add_result(results, "Playback benchmark PNG/RGBA MOV alpha", status, msg, benchmark=bench)
        else:
            add_result(results, "Playback benchmark PNG/RGBA MOV alpha", "SKIP", f"missing: {alpha_mov}")

    if not args.skip_ui:
        capture_path = output_dir / "current_ui_capture.png"
        media_for_ui = None if args.ui_no_media else (MEDIA / "1080p_h264.mp4" if (MEDIA / "1080p_h264.mp4").exists() else None)
        if args.ui_demo and media_for_ui is None and (MEDIA / "4k_60fps.mp4").exists():
            media_for_ui = MEDIA / "4k_60fps.mp4"
        cap = capture_ui(exe, capture_path, media_for_ui, 2200 if args.ui_demo else 1600, args.ui_width, args.ui_height, args.ui_demo)
        add_result(results, "UI capture", "PASS" if cap["ok"] else "FAIL", str(capture_path), capture=cap)

        if cap["ok"] and args.reference_ui.exists():
            comp = compare_ui(capture_path, args.reference_ui, output_dir / "ui_compare")
            report = comp.get("report", {})
            msg = f"color_error={report.get('color_error_pct', 0)}%, changed={report.get('changed_pixels_gt12_pct', 0)}%"
            add_result(results, "UI overlay compare", "PASS" if comp["ok"] else "FAIL", msg, compare=comp)
        elif not args.reference_ui.exists():
            add_result(results, "UI overlay compare", "SKIP", f"reference missing: {args.reference_ui}")

    passed = sum(1 for item in results if item["status"] == "PASS")
    failed = sum(1 for item in results if item["status"] == "FAIL")
    skipped = sum(1 for item in results if item["status"] == "SKIP")
    final_report = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "exe": str(exe),
        "output_dir": str(output_dir),
        "summary": {"pass": passed, "fail": failed, "skip": skipped, "total": len(results)},
        "results": results,
    }
    report_path = output_dir / "stability_report.json"
    report_path.write_text(json.dumps(final_report, indent=2, ensure_ascii=False), encoding="utf-8")

    print(json.dumps(final_report["summary"], indent=2, ensure_ascii=False))
    print(f"Report: {report_path}")
    for item in results:
        print(f"[{item['status']}] {item['name']} - {item['message']}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
