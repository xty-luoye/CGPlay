#!/usr/bin/env python3
"""Compare a CGPlay screenshot against a reference image and write an overlay report."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError as exc:  # pragma: no cover
    raise SystemExit("Pillow is required: python -m pip install pillow") from exc


def _fit_to_reference(current: Image.Image, reference: Image.Image) -> Image.Image:
    if current.size == reference.size:
        return current
    return current.resize(reference.size, Image.Resampling.LANCZOS)


def _diff_metrics(current: Image.Image, reference: Image.Image) -> dict:
    diff = ImageChops.difference(current, reference)
    stat = ImageStat.Stat(diff)
    mean_channel = stat.mean
    rms_channel = stat.rms
    mean_abs = sum(mean_channel) / 3.0
    rms = sum(rms_channel) / 3.0
    mask = diff.convert("L").point(lambda value: 255 if value > 12 else 0)
    bbox = mask.getbbox()
    changed_pixels = mask.histogram()[255] if bbox else 0
    total_pixels = reference.size[0] * reference.size[1]
    return {
        "mean_abs_rgb": round(mean_abs, 4),
        "rms_rgb": round(rms, 4),
        "color_error_pct": round(mean_abs / 255.0 * 100.0, 4),
        "changed_pixels_gt12_pct": round(changed_pixels / max(1, total_pixels) * 100.0, 4),
        "diff_bbox": list(bbox) if bbox else None,
    }


def _region_boxes(size: tuple[int, int]) -> dict[str, tuple[int, int, int, int]]:
    width, height = size
    return {
        "top_bar": (0, 0, width, min(height, 56)),
        "left_nav": (0, 56, min(width, 88), height),
        "playlist_panel": (88, 56, min(width, 408), max(56, height - 34)),
        "viewer_toolbar": (408, 56, max(408, width - 320), min(height, 132)),
        "viewer_body": (408, 132, max(408, width - 320), max(132, height - 142)),
        "timeline_playback": (408, max(132, height - 142), max(408, width - 320), max(132, height - 34)),
        "review_panel": (max(0, width - 320), 56, width, max(56, height - 34)),
        "status_bar": (0, max(0, height - 34), width, height),
    }


def compare(current_path: Path, reference_path: Path, output_dir: Path) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)

    current = Image.open(current_path).convert("RGB")
    reference = Image.open(reference_path).convert("RGB")
    current_fit = _fit_to_reference(current, reference)

    diff = ImageChops.difference(current_fit, reference)
    metrics = _diff_metrics(current_fit, reference)
    mean_abs = metrics["mean_abs_rgb"]

    mask = diff.convert("L").point(lambda value: 255 if value > 12 else 0)
    bbox = mask.getbbox()
    changed_pixels = 0
    if bbox:
        histogram = mask.histogram()
        changed_pixels = histogram[255]
    total_pixels = reference.size[0] * reference.size[1]
    changed_pct = changed_pixels / max(1, total_pixels) * 100.0

    heat = Image.new("RGB", reference.size, (0, 0, 0))
    heat.putdata([(v, 0, 0) for v in diff.convert("L").getdata()])
    overlay = Image.blend(reference, current_fit, 0.50)
    overlay = Image.blend(overlay, heat, 0.35)

    diff_path = output_dir / "ui_diff_heat.png"
    overlay_path = output_dir / "ui_overlay.png"
    current_fit_path = output_dir / "ui_current_fit.png"
    diff.save(diff_path)
    overlay.save(overlay_path)
    current_fit.save(current_fit_path)

    max_dx = 0
    max_dy = 0
    if bbox:
        left, top, right, bottom = bbox
        max_dx = max(left, reference.size[0] - right)
        max_dy = max(top, reference.size[1] - bottom)

    report = {
        "current": str(current_path),
        "reference": str(reference_path),
        "current_size": list(current.size),
        "reference_size": list(reference.size),
        "resized_current_to_reference": current.size != reference.size,
        **metrics,
        "outer_margin_without_diff_px": {"x": max_dx, "y": max_dy},
        "overlay": str(overlay_path),
        "diff_heat": str(diff_path),
        "current_fit": str(current_fit_path),
        "regions": {},
    }

    for name, box in _region_boxes(reference.size).items():
        left, top, right, bottom = box
        if right <= left or bottom <= top:
            continue
        report["regions"][name] = _diff_metrics(
            current_fit.crop(box),
            reference.crop(box),
        )

    report_path = output_dir / "ui_compare_report.json"
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare CGPlay UI screenshot with a reference image.")
    parser.add_argument("--current", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    report = compare(args.current, args.reference, args.output_dir)
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
