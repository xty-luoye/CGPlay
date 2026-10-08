#!/usr/bin/env python3
"""Shared low-cost subtitle-region detector metadata helpers.

The runtime high-quality OCR worker and the headless acceptance gate both use
this module so candidate scan caches, crop rectangles, and media fingerprints
follow the same rules.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Dict, Iterable, Optional


DETECTOR_VERSION = "subtitle-region-detector-v2-onset-aligned"


def media_identity(path: Path) -> Dict[str, object]:
    st = path.stat()
    absolute = str(path.resolve())
    mtime_ms = int(st.st_mtime_ns // 1000000)
    key = f"{absolute.lower()}|{st.st_size}|{mtime_ms}"
    return {
        "mediaPath": absolute,
        "fileSize": st.st_size,
        "mtimeMs": mtime_ms,
        "mediaFingerprint": hashlib.sha256(key.encode("utf-8", errors="replace")).hexdigest(),
    }


def default_regions():
    return [
        {"name": "bottom-subtitle-band", "rect": {"x": 0.0, "y": 0.64, "w": 1.0, "h": 0.34}, "priority": 1},
        {"name": "letterbox-bottom-band", "rect": {"x": 0.0, "y": 0.70, "w": 1.0, "h": 0.24}, "priority": 2},
        {"name": "lower-middle-subtitle-band", "rect": {"x": 0.0, "y": 0.48, "w": 1.0, "h": 0.30}, "priority": 3},
        {"name": "middle-subtitle-band", "rect": {"x": 0.0, "y": 0.34, "w": 1.0, "h": 0.30}, "priority": 4},
        {"name": "upper-subtitle-band", "rect": {"x": 0.0, "y": 0.08, "w": 1.0, "h": 0.28}, "priority": 5},
    ]


def crop_rect(region_name: str = "bottom-subtitle-band", crop_height: str = "") -> Dict[str, object]:
    if str(crop_height or "").strip():
        return {
            "mode": "bottom-pixel-height",
            "x": 0.0,
            "y": None,
            "w": 1.0,
            "h": str(crop_height).strip(),
            "region": "bottom-subtitle-band",
        }
    for item in default_regions():
        if item["name"] == region_name:
            rect = dict(item["rect"])
            rect["mode"] = "normalized"
            rect["region"] = region_name
            return rect
    rect = dict(default_regions()[0]["rect"])
    rect["mode"] = "normalized"
    rect["region"] = "bottom-subtitle-band"
    return rect


def ffmpeg_crop_filter(region_name: str = "bottom-subtitle-band", crop_height: str = "") -> str:
    if str(crop_height or "").strip():
        h = str(crop_height).strip()
        return f"crop=iw:{h}:0:ih-{h}"
    rect = crop_rect(region_name)
    return (
        f"crop=iw*{float(rect['w']):.6f}:ih*{float(rect['h']):.6f}:"
        f"iw*{float(rect['x']):.6f}:ih*{float(rect['y']):.6f}"
    )


def preprocess_filter(base_filter: str, enabled: bool = True) -> str:
    if not enabled:
        return base_filter
    return f"{base_filter},scale=iw*2:ih*2:flags=lanczos,eq=contrast=1.35:brightness=0.02:saturation=0,unsharp=5:5:0.8"


def text_temporal_policy() -> Dict[str, object]:
    return {
        "defaultRegion": "bottom-subtitle-band",
        "dynamicRegions": default_regions(),
        "watermarkPolicy": "reject unless text is inside a subtitle candidate region and behaves like timed subtitles",
        "bilingualPolicy": "allow multiline and bilingual candidates inside the subtitle ROI",
        "apiImagePolicy": "send cropped subtitle-region still frames only; never upload full video or full frames",
    }


def scan_cache_path(media: Path, translations_dir: Path, fingerprint: Optional[str] = None) -> Path:
    fp = fingerprint or str(media_identity(media)["mediaFingerprint"])
    return translations_dir / f"{media.stem}.{fp[:12]}.candidate_scan.json"


def candidate_from_cue(cue: Dict[str, object], index: int, region_name: str = "bottom-subtitle-band") -> Dict[str, object]:
    start = float(cue.get("start", 0.0))
    end = float(cue.get("end", start))
    representative = max(start, min(end, (start + end) * 0.5))
    return {
        "index": index,
        "start": round(start, 3),
        "end": round(end, 3),
        "representativeTime": round(representative, 3),
        "sampleFrame": {"timeSeconds": round(representative, 3), "kind": "candidate-cue-representative"},
        "cropRect": crop_rect(region_name),
        "sourceKind": cue.get("sourceKind", "subtitle-region-candidate"),
        "confidence": max(0.0, min(1.0, float(cue.get("confidence", cue.get("visionConfidence", 0.82))))),
        "text": cue.get("text", ""),
    }


def build_scan_result(
    media: Path,
    translations_dir: Path,
    cues: Iterable[Dict[str, object]],
    duration_seconds: float,
    sample_interval_seconds: float,
    region_name: str = "bottom-subtitle-band",
) -> Dict[str, object]:
    identity = media_identity(media)
    candidates = [candidate_from_cue(cue, i + 1, region_name) for i, cue in enumerate(cues)]
    return {
        "schema": "cgplay-subtitle-region-candidate-scan-v1",
        "detectorVersion": DETECTOR_VERSION,
        "sharedByRuntimeAndHeadless": True,
        "mediaIdentity": identity,
        "mediaFingerprint": identity["mediaFingerprint"],
        "mediaPath": identity["mediaPath"],
        "scanCachePath": str(scan_cache_path(media, translations_dir, str(identity["mediaFingerprint"]))),
        "scanMode": "full-video-low-cost-subtitle-region",
        "durationSec": round(float(duration_seconds or 0.0), 3),
        "sampleIntervalSeconds": float(sample_interval_seconds or 0.0),
        "candidateFrameCount": int(round(float(duration_seconds or 0.0) / sample_interval_seconds)) if sample_interval_seconds else len(candidates),
        "candidateCueCount": len(candidates),
        "defaultCropRect": crop_rect(region_name),
        "regionPolicy": text_temporal_policy(),
        "candidateCues": candidates,
    }


def write_scan_result(path: Path, result: Dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


def load_scan_result(path: Path, media: Path) -> Optional[Dict[str, object]]:
    if not path.exists():
        return None
    try:
        payload = json.loads(path.read_text(encoding="utf-8-sig"))
    except Exception:
        return None
    if payload.get("mediaFingerprint") != media_identity(media).get("mediaFingerprint"):
        return None
    return payload
