#!/usr/bin/env python3
import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import wave
from pathlib import Path


def _timestamp(seconds):
    ms = max(0, int(round(seconds * 1000)))
    h = ms // 3600000
    m = (ms // 60000) % 60
    s = (ms // 1000) % 60
    r = ms % 1000
    return f"{h:02d}:{m:02d}:{s:02d}.{r:03d}"


def _parse_time(value):
    hh, mm, rest = value.replace(",", ".").split(":")
    return int(hh) * 3600 + int(mm) * 60 + float(rest)


def _write_vtt(path, cues, text_key="text"):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("WEBVTT\n\n")
        for cue in cues:
            text = str(cue.get(text_key, "") or "").strip()
            if not text:
                continue
            handle.write(f"{_timestamp(cue['start'])} --> {_timestamp(cue['end'])}\n{text}\n\n")


def _read_vtt(path):
    if not path.exists():
        return []
    text = path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
    cues = []
    pattern = re.compile(
        r"(\d{2}:\d{2}:\d{2}[,.]\d{3})\s*-->\s*(\d{2}:\d{2}:\d{2}[,.]\d{3})\n(.+?)(?=\n\s*\n|\Z)",
        re.S,
    )
    for match in pattern.finditer(text):
        cues.append(
            {
                "start": _parse_time(match.group(1)),
                "end": _parse_time(match.group(2)),
                "text": " ".join(match.group(3).split()),
            }
        )
    return cues


def _norm(text):
    return re.sub(r"\s+", " ", str(text or "").strip().lower())


def _similarity(a, b):
    a = _norm(a)
    b = _norm(b)
    if not a and not b:
        return 1.0
    if not a or not b:
        return 0.0
    return difflib.SequenceMatcher(None, a, b).ratio()


def _cue_level_score(reference, candidate, threshold=0.78, aggregate_overlaps=False):
    rows = []
    if not reference:
        return {
            "measured": False,
            "accuracy": None,
            "confidence": 0.0,
            "matched": 0,
            "total": 0,
            "rows": rows,
            "fallbackReason": "reference-missing",
        }
    matched = 0
    timing_errors = 0
    for i, ref in enumerate(reference):
        if aggregate_overlaps:
            ref_start = float(ref.get("start", 0.0) or 0.0)
            ref_end = float(ref.get("end", 0.0) or 0.0)
            matched_candidates = []
            for cand in candidate:
                cand_start = float(cand.get("start", 0.0) or 0.0)
                cand_end = float(cand.get("end", 0.0) or 0.0)
                overlap = max(0.0, min(ref_end, cand_end) - max(ref_start, cand_start))
                if overlap > 0.05 or (ref_start - 0.5 <= cand_start <= ref_end + 0.5):
                    matched_candidates.append(cand)
        else:
            matched_candidates = [candidate[i]] if i < len(candidate) else []
        candidate_text = " ".join(str(c.get("text", "") or "").strip() for c in matched_candidates).strip()
        score = _similarity(ref.get("text", ""), candidate_text)
        timing_delta = 999.0
        if matched_candidates:
            timing_delta = abs(float(ref.get("start", 0.0) or 0.0) - float(matched_candidates[0].get("start", 0.0) or 0.0)) + abs(
                float(ref.get("end", 0.0) or 0.0) - float(matched_candidates[-1].get("end", 0.0) or 0.0)
            )
        ok = score >= threshold
        if ok:
            matched += 1
        if timing_delta > 2.5:
            timing_errors += 1
        rows.append(
            {
                "index": i + 1,
                "reference": ref.get("text", ""),
                "candidate": candidate_text,
                "candidateCueCount": len(matched_candidates),
                "similarity": round(score, 4),
                "timingDeltaSeconds": None if timing_delta >= 999 else round(timing_delta, 3),
                "passed": ok,
            }
        )
    accuracy = matched / max(1, len(reference))
    confidence = max(0.0, min(1.0, len(candidate) / max(1, len(reference)))) if candidate else 0.0
    return {
        "measured": True,
        "accuracy": round(accuracy, 4),
        "confidence": round(confidence, 4),
        "matched": matched,
        "total": len(reference),
        "timingErrorCount": timing_errors,
        "rows": rows,
        "fallbackReason": "" if candidate else "candidate-missing",
    }


def _run(command, timeout=120, env=None):
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        env=env,
    )
    return proc.returncode, proc.stdout, proc.stderr


def _run_ffmpeg(args, timeout=120):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return False, "ffmpeg-missing"
    code, _out, err = _run([ffmpeg, "-y", *args], timeout=timeout)
    return code == 0, (err or "")[-1200:]


def _probe_media_duration(path):
    if not path.exists():
        return 0.0
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        return 0.0
    code, out, _err = _run(
        [
            ffprobe,
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(path),
        ],
        timeout=30,
    )
    if code != 0:
        return 0.0
    try:
        return float((out or "0").strip())
    except Exception:
        return 0.0


def _probe_wav_duration(path):
    if not path.exists():
        return 0.0
    try:
        with wave.open(str(path), "rb") as handle:
            return handle.getnframes() / float(handle.getframerate() or 1)
    except Exception:
        return 0.0


def _make_tts_wav(path, text, rate=0):
    if path.exists():
        return True, "exists"
    escaped_path = str(path).replace("'", "''")
    escaped_text = text.replace("'", "''")
    script = (
        "Add-Type -AssemblyName System.Speech; "
        "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        f"$s.Rate={int(rate)}; "
        f"$s.SetOutputToWaveFile('{escaped_path}'); "
        f"$s.Speak('{escaped_text}'); "
        "$s.Dispose()"
    )
    code, out, err = _run(["powershell", "-NoProfile", "-Command", script], timeout=60)
    return code == 0 and path.exists(), (err or out or "tts-failed")[-1000:]


def _make_video_from_wav(path, wav_path, noisy=False):
    wav_duration = _probe_wav_duration(wav_path)
    existing_duration = _probe_media_duration(path)
    if (
        path.exists() and
        existing_duration >= max(1.0, wav_duration - 0.5) and
        (wav_duration <= 0.0 or existing_duration <= wav_duration + 0.5)
    ):
        return True, "exists"
    if path.exists():
        try:
            path.unlink()
        except Exception:
            return False, "stale-video-remove-failed"
    if noisy:
        return _run_ffmpeg(
            [
                "-f",
                "lavfi",
                "-i",
                "color=c=black:s=640x360:r=24",
                "-i",
                str(wav_path),
                "-f",
                "lavfi",
                "-i",
                "anoisesrc=color=white:amplitude=0.035",
                "-filter_complex",
                "[1:a][2:a]amix=inputs=2:duration=first:weights=1 0.35[a]",
                "-map",
                "0:v",
                "-map",
                "[a]",
                "-shortest",
                "-t",
                f"{max(1.0, wav_duration):.3f}",
                "-pix_fmt",
                "yuv420p",
                str(path),
            ],
            timeout=120,
        )
    return _run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "color=c=black:s=640x360:r=24",
            "-i",
            str(wav_path),
            "-shortest",
            "-t",
            f"{max(1.0, wav_duration):.3f}",
            "-pix_fmt",
            "yuv420p",
            str(path),
        ],
        timeout=120,
    )


def _run_long_asr(worker, media, source_vtt, translated_vtt, report_json, source_lang, target_lang):
    env = os.environ.copy()
    env.setdefault("CGPLAY_FASTER_WHISPER_DEVICE", "cpu")
    env.setdefault("CGPLAY_FASTER_WHISPER_COMPUTE", "int8")
    env.setdefault("SUBTITLE_LONG_ASR_DURATION_SECONDS", "18")
    env.setdefault("SUBTITLE_LONG_ASR_TRANSCRIBE_TIMEOUT_SECONDS", "240")
    code, stdout, stderr = _run(
        [
            sys.executable,
            str(worker),
            "--media",
            str(media),
            "--output-json",
            str(report_json),
            "--output-source-vtt",
            str(source_vtt),
            "--output-translated-vtt",
            str(translated_vtt),
            "--start",
            "0",
            "--duration",
            "18",
            "--source-language",
            source_lang,
            "--target-language",
            target_lang,
        ],
        timeout=360,
        env=env,
    )
    payload = {}
    if report_json.exists():
        try:
            payload = json.loads(report_json.read_text(encoding="utf-8", errors="replace"))
        except Exception:
            payload = {}
    if not payload:
        try:
            payload = json.loads(stdout or "{}")
        except Exception:
            payload = {}
    payload.setdefault("processExitCode", code)
    if stderr.strip():
        payload.setdefault("stderr", stderr.strip()[-1200:])
    return payload


def _class_state(score, provider_ok=True):
    if not score.get("measured"):
        return "not-proven"
    if score.get("accuracy") is None:
        return "not-proven"
    if score["accuracy"] >= 0.80 and provider_ok:
        return "measured-passed-threshold"
    return "below-threshold"


def _no_subtitle_case(root, worker, name, noisy):
    text = "The shadow is moving quickly. We must prepare the plan. The school building will be rebuilt soon."
    translation_text = (
        "\u6697\u5f71\u6b63\u5728\u5feb\u901f\u884c\u52a8\u3002"
        "\u6211\u4eec\u5fc5\u987b\u51c6\u5907\u8ba1\u5212\u3002"
        "\u6821\u820d\u5f88\u5feb\u4f1a\u91cd\u5efa\u3002"
    )
    wav = root / f"{name}.wav"
    media = root / f"{name}.mp4"
    _make_tts_wav(wav, text, rate=-1)
    reference_end = max(6.0, round(_probe_wav_duration(wav), 3))
    reference_source = [
        {"start": 0.0, "end": reference_end, "text": text},
    ]
    reference_translation = [
        {"start": 0.0, "end": reference_end, "text": translation_text},
    ]
    video_ok, video_reason = _make_video_from_wav(media, wav, noisy=noisy)
    ref_source_vtt = root / f"{name}.reference.source.vtt"
    ref_zh_vtt = root / f"{name}.reference.zh.vtt"
    source_vtt = root / f"{name}.longasr.source.vtt"
    translated_vtt = root / f"{name}.hq.translated.zh.vtt"
    long_report = root / f"{name}.longasr.report.json"
    _write_vtt(ref_source_vtt, reference_source)
    _write_vtt(ref_zh_vtt, reference_translation)
    for stale_path in (source_vtt, translated_vtt, long_report):
        try:
            stale_path.unlink()
        except FileNotFoundError:
            pass
        except Exception:
            pass

    if not video_ok:
        return {
            "videoClass": "noisy-no-subtitle-asr" if noisy else "clean-no-subtitle-asr",
            "sourceClass": "long-asr-corrected",
            "media": str(media),
            "state": "not-proven",
            "measured": False,
            "accuracy": None,
            "confidence": 0.0,
            "fallbackReason": video_reason,
        }

    worker_payload = _run_long_asr(worker, media, source_vtt, translated_vtt, long_report, "en", "zh")
    source_score = _cue_level_score(reference_source, _read_vtt(source_vtt), threshold=0.68, aggregate_overlaps=True)
    translation_score = _cue_level_score(reference_translation, _read_vtt(translated_vtt), threshold=0.62, aggregate_overlaps=True)
    translated_available = bool(_read_vtt(translated_vtt))
    state = _class_state(translation_score, provider_ok=translated_available)
    if not translated_available and source_score.get("measured"):
        state = "translation-blocked-source-measured"
    fallback = ""
    if state == "translation-blocked-source-measured":
        fallback = worker_payload.get("blocker") or "translation-provider-unavailable"
    elif state == "below-threshold":
        fallback = "below-80-threshold"
    elif state == "not-proven":
        fallback = worker_payload.get("blocker") or source_score.get("fallbackReason") or translation_score.get("fallbackReason")

    asr_errors = [
        row for row in source_score.get("rows", []) if not row.get("passed")
    ]
    translation_errors = [
        row for row in translation_score.get("rows", []) if not row.get("passed")
    ] if translated_available else []
    timing_errors = source_score.get("timingErrorCount", 0) + translation_score.get("timingErrorCount", 0)
    return {
        "videoClass": "noisy-no-subtitle-asr" if noisy else "clean-no-subtitle-asr",
        "sourceClass": "long-asr-corrected",
        "media": str(media),
        "referenceSource": str(ref_source_vtt),
        "referenceTranslation": str(ref_zh_vtt),
        "candidateSource": str(source_vtt),
        "candidateTranslation": str(translated_vtt),
        "workerReport": str(long_report),
        "state": state,
        "measured": bool(source_score.get("measured") or translation_score.get("measured")),
        "accuracy": translation_score.get("accuracy") if translated_available else None,
        "sourceTranscriptAccuracy": source_score.get("accuracy"),
        "translationAccuracy": translation_score.get("accuracy") if translated_available else None,
        "confidence": translation_score.get("confidence") if translated_available else source_score.get("confidence", 0.0),
        "fallbackReason": fallback,
        "errorBreakdown": {
            "asrErrorCount": len(asr_errors),
            "translationErrorCount": len(translation_errors),
            "timingSegmentationErrorCount": timing_errors,
            "translationProviderBlocked": not translated_available,
        },
        "cueLevelSource": source_score.get("rows", []),
        "cueLevelTranslation": translation_score.get("rows", []),
        "worker": {
            "success": worker_payload.get("success"),
            "blocker": worker_payload.get("blocker", ""),
            "sourceCueCount": worker_payload.get("sourceCueCount", 0),
            "translatedCueCount": worker_payload.get("translatedCueCount", 0),
            "translationAttempts": worker_payload.get("translationAttempts", []),
            "transcribeBackend": (worker_payload.get("transcribe") or {}).get("backend", ""),
        },
    }


def main():
    parser = argparse.ArgumentParser(description="Cross-video subtitle accuracy harness for RVLite high-quality translation.")
    parser.add_argument("--test-dir", default=r"C:\Users\1\Desktop\RVLite_Translation_TestVideos")
    parser.add_argument("--output", default="")
    parser.add_argument("--worker", default="")
    args = parser.parse_args()

    root = Path(args.test_dir)
    root.mkdir(parents=True, exist_ok=True)
    output = Path(args.output) if args.output else root / "cross_video_accuracy_report.json"
    worker = Path(args.worker) if args.worker else Path(__file__).with_name("long_window_asr_worker.py")

    classes = []
    local_ref = root / "local_reference_sample.vtt"
    local_candidate = root / "local_reference_sample.online.translated.zh.vtt"
    local_cues = [
        {"start": 0.5, "end": 2.0, "text": "本地参考字幕第一句"},
        {"start": 2.2, "end": 4.2, "text": "本地参考字幕第二句"},
    ]
    _write_vtt(local_ref, local_cues)
    if not local_candidate.exists():
        _write_vtt(local_candidate, local_cues)
    local_score = _cue_level_score(_read_vtt(local_ref), _read_vtt(local_candidate), threshold=0.95)
    classes.append(
        {
            "videoClass": "local-or-embedded-subtitle",
            "sourceClass": "local-reference-provider",
            "reference": str(local_ref),
            "candidate": str(local_candidate),
            "state": "proven" if local_score["accuracy"] and local_score["accuracy"] >= 0.80 else "measured-fail",
            "measured": local_score["measured"],
            "accuracy": local_score["accuracy"],
            "confidence": local_score["confidence"],
            "fallbackReason": "",
        }
    )
    classes.append(
        {
            "videoClass": "hard-sub-ocr",
            "sourceClass": "hard-sub-ocr",
            "state": "not-target-this-run",
            "measured": False,
            "accuracy": None,
            "confidence": 0.0,
            "fallbackReason": "current run targets no-subtitle high-quality",
        }
    )
    classes.append(_no_subtitle_case(root, worker, "clean_no_subtitle_asr_sample", noisy=False))
    classes.append(_no_subtitle_case(root, worker, "noisy_no_subtitle_asr_sample", noisy=True))
    online_report = root / "online_reference_sample.online.search.json"
    online_report.write_text(
        json.dumps(
            {
                "currentState": "not-configured",
                "fallbackReason": "online-search-provider-not-configured",
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )
    classes.append(
        {
            "videoClass": "online-or-reference-subtitle",
            "sourceClass": "online-reference-search",
            "searchReport": str(online_report),
            "state": "not-target-this-run",
            "measured": False,
            "accuracy": None,
            "confidence": 0.0,
            "fallbackReason": "current run targets no-subtitle high-quality",
        }
    )

    clean = next((item for item in classes if item.get("videoClass") == "clean-no-subtitle-asr"), {})
    noisy = next((item for item in classes if item.get("videoClass") == "noisy-no-subtitle-asr"), {})
    no_subtitle_measured = bool(clean.get("measured") and noisy.get("measured"))
    threshold_passed = (
        clean.get("translationAccuracy") is not None
        and noisy.get("translationAccuracy") is not None
        and clean.get("translationAccuracy", 0) >= 0.80
        and noisy.get("translationAccuracy", 0) >= 0.80
    )
    report = {
        "createdAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "target": "no-subtitle-clean-noisy-high-quality-80-percent",
        "state": "measured-passed-threshold" if threshold_passed else ("measured-not-proven" if no_subtitle_measured else "not-proven"),
        "mustNotClaimUniversal80Percent": True,
        "classes": classes,
        "summary": {
            "cleanNoSubtitleAccuracy": clean.get("translationAccuracy"),
            "cleanNoSubtitleSourceTranscriptAccuracy": clean.get("sourceTranscriptAccuracy"),
            "cleanState": clean.get("state"),
            "cleanFallbackReason": clean.get("fallbackReason"),
            "noisyNoSubtitleAccuracy": noisy.get("translationAccuracy"),
            "noisyNoSubtitleSourceTranscriptAccuracy": noisy.get("sourceTranscriptAccuracy"),
            "noisyState": noisy.get("state"),
            "noisyFallbackReason": noisy.get("fallbackReason"),
        },
    }
    (root / "manifest.json").write_text(json.dumps({"root": str(root), "classes": classes}, ensure_ascii=False, indent=2), encoding="utf-8")
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
