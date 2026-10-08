#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request


def _result(success, **kwargs):
    payload = {"success": success}
    payload.update(kwargs)
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if success else 1


def _timestamp(seconds):
    ms = max(0, int(round(seconds * 1000)))
    h = ms // 3600000
    m = (ms // 60000) % 60
    s = (ms // 1000) % 60
    r = ms % 1000
    return f"{h:02d}:{m:02d}:{s:02d}.{r:03d}"


def _write_vtt(path, cues, text_key="text"):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("WEBVTT\n\n")
        for cue in cues:
            text = str(cue.get(text_key, "") or "").strip()
            if not text:
                continue
            handle.write(f"{_timestamp(cue['start'])} --> {_timestamp(cue['end'])}\n{text}\n\n")


def _run_json(command, timeout, env=None):
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
    try:
        payload = json.loads(proc.stdout or "{}")
    except Exception:
        payload = {
            "success": False,
            "error": proc.stderr.strip() or "invalid JSON response",
        }
    payload.setdefault("exitCode", proc.returncode)
    if proc.stderr.strip():
        payload.setdefault("stderr", proc.stderr.strip()[-1000:])
    return payload


def _translation_provider_configured():
    return bool(
        _translation_base_url()
        and _translation_api_key()
    )


def _first_env(*names):
    for name in names:
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return ""


def _translation_base_url():
    return _first_env(
        "SUBTITLE_TRANSLATION_BASE_URL",
        "QWEN_BASE_URL",
        "DASHSCOPE_BASE_URL",
        "MIMO_BASE_URL",
        "XIAOMI_MIMO_BASE_URL",
        "AI_BASE_URL",
        "OPENAI_BASE_URL",
    ).rstrip("/")


def _translation_api_key():
    return _first_env(
        "SUBTITLE_TRANSLATION_API_KEY",
        "QWEN_API_KEY",
        "DASHSCOPE_API_KEY",
        "MIMO_API_KEY",
        "XIAOMI_MIMO_API_KEY",
        "AI_API_KEY",
        "OPENAI_API_KEY",
    )


def _translation_model():
    return _first_env(
        "SUBTITLE_TRANSLATION_MODEL",
        "QWEN_MODEL",
        "DASHSCOPE_MODEL",
        "MIMO_MODEL",
        "XIAOMI_MIMO_MODEL",
        "AI_MODEL",
        "OPENAI_MODEL",
    ) or ("qwen-plus" if (_translation_base_url().lower().find("dashscope") >= 0 or _translation_base_url().lower().find("aliyuncs.com") >= 0) else "gpt-4o-mini")


def _translate_with_openai_compatible(text, source_language, target_language):
    base_url = _translation_base_url()
    api_key = _translation_api_key()
    model = _translation_model()
    if not base_url or not api_key:
        return {
            "success": False,
            "error": "translation-provider-unavailable",
            "backend": "openai_compatible_unavailable",
        }
    endpoint = base_url
    if not endpoint.endswith("/chat/completions"):
        endpoint = endpoint.rstrip("/") + "/v1/chat/completions"
    payload = {
        "model": model,
        "messages": [
            {
                "role": "system",
                "content": (
                    "Translate the current subtitle dialogue faithfully into Simplified Chinese. "
                    "Return only the translated subtitle text, no explanation. Do not polish, expand, summarize, "
                    "infer plot context, add subjects/speakers, merge nearby dialogue, or change subject/action/tone/polarity. "
                    "If the source is already Chinese or Traditional Chinese, only normalize to Simplified Chinese, punctuation, and spacing. "
                    "Short cues must stay short and equivalent."
                ),
            },
            {
                "role": "user",
                "content": f"Source language: {source_language}\nTarget language: {target_language}\nText:\n{text}",
            },
        ],
        "temperature": 0,
    }
    request = urllib.request.Request(
        endpoint,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            data = json.loads(response.read().decode("utf-8", errors="replace"))
        choices = data.get("choices") or []
        message = (choices[0].get("message") or {}) if choices else {}
        translated = str(message.get("content", "") or "").strip()
        if not translated:
            return {
                "success": False,
                "error": "translation-provider-empty-response",
                "backend": "openai_compatible_chat",
                "backendDetail": model,
            }
        return {
            "success": True,
            "translatedText": translated,
            "backend": "openai_compatible_chat",
            "backendDetail": f"provider={'qwen' if 'dashscope' in endpoint.lower() or 'aliyuncs.com' in endpoint.lower() else 'openai-compatible'} model={model} endpoint={endpoint}",
        }
    except Exception as exc:
        return {
            "success": False,
            "error": str(exc) or "translation-provider-request-failed",
            "backend": "openai_compatible_chat_failed",
            "backendDetail": f"provider={'qwen' if 'dashscope' in endpoint.lower() or 'aliyuncs.com' in endpoint.lower() else 'openai-compatible'} model={model} endpoint={endpoint}",
        }


def _extract_audio(ffmpeg, media, start, duration, output):
    proc = subprocess.run(
        [
            ffmpeg,
            "-y",
            "-ss",
            f"{start:.3f}",
            "-i",
            media,
            "-t",
            f"{duration:.3f}",
            "-vn",
            "-ac",
            "1",
            "-ar",
            "16000",
            "-f",
            "wav",
            output,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=max(30, int(duration) + 30),
    )
    return proc.returncode == 0 and os.path.exists(output), proc.stderr.strip()[-1000:]


def _normalize_segments(payload, absolute_start):
    segments = payload.get("segments") or []
    cues = []
    if isinstance(segments, list):
        for item in segments:
            text = str(item.get("text", "") or "").strip()
            if not text:
                continue
            start = absolute_start + max(0.0, float(item.get("start", 0.0) or 0.0))
            end = absolute_start + max(0.0, float(item.get("end", 0.0) or 0.0))
            if end <= start:
                end = start + 2.0
            cues.append({"start": start, "end": end, "text": text})
    if not cues:
        text = str(payload.get("text", "") or "").strip()
        if text:
            cues.append({"start": absolute_start, "end": absolute_start + 4.0, "text": text})
    return cues


def main():
    parser = argparse.ArgumentParser(description="Manual high-quality long-window ASR worker for CGPlay.")
    parser.add_argument("--media", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-source-vtt", required=True)
    parser.add_argument("--output-translated-vtt", required=True)
    parser.add_argument("--start", type=float, default=0.0)
    parser.add_argument("--duration", type=float, default=24.0)
    parser.add_argument("--source-language", default="ja")
    parser.add_argument("--target-language", default="zh")
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--transcribe-helper", default=os.path.join(os.path.dirname(__file__), "local_transcribe.py"))
    parser.add_argument("--translate-helper", default=os.path.join(os.path.dirname(__file__), "local_text_translate.py"))
    parser.add_argument("--model", default=os.environ.get("SUBTITLE_LONG_ASR_MODEL", ""))
    args = parser.parse_args()

    started_at = time.time()
    if not os.path.exists(args.media):
        return _result(False, blocker="media-missing", error="media file does not exist")
    if not os.path.exists(args.transcribe_helper):
        return _result(False, blocker="transcribe-helper-missing", error="local_transcribe.py is missing")
    if not os.path.exists(args.translate_helper):
        return _result(False, blocker="translate-helper-missing", error="local_text_translate.py is missing")

    os.makedirs(os.path.dirname(os.path.abspath(args.output_json)), exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cgplay_longasr_") as tmp:
        wav_path = os.path.join(tmp, "window.wav")
        audio_ok, audio_error = _extract_audio(args.ffmpeg, args.media, args.start, args.duration, wav_path)
        if not audio_ok:
            payload = {
                "success": False,
                "blocker": "audio-extract-failed",
                "error": audio_error or "ffmpeg audio extraction failed",
                "elapsedSeconds": round(time.time() - started_at, 3),
            }
            with open(args.output_json, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, ensure_ascii=False, indent=2)
            return _result(False, **payload)

        transcribe_env = os.environ.copy()
        transcribe_env.setdefault("CGPLAY_FASTER_WHISPER_DEVICE", "cpu")
        transcribe_env.setdefault("CGPLAY_FASTER_WHISPER_COMPUTE", "int8")
        transcribe = _run_json(
            [args.python, args.transcribe_helper, wav_path, args.source_language, args.model],
            int(os.environ.get("SUBTITLE_LONG_ASR_TRANSCRIBE_TIMEOUT_SECONDS", "180")),
            env=transcribe_env,
        )
        source_cues = _normalize_segments(transcribe, args.start) if transcribe.get("success") else []
        if source_cues:
            _write_vtt(args.output_source_vtt, source_cues, "text")

        translated_cues = []
        translate_payloads = []
        if source_cues:
            for cue in source_cues:
                translation = _run_json(
                    [
                        args.python,
                        args.translate_helper,
                        cue["text"],
                        args.source_language,
                        args.target_language,
                    ],
                    int(os.environ.get("SUBTITLE_LONG_ASR_TRANSLATE_TIMEOUT_SECONDS", "90")),
                )
                if not translation.get("success") and _translation_provider_configured():
                    api_translation = _translate_with_openai_compatible(
                        cue["text"],
                        args.source_language,
                        args.target_language,
                    )
                    api_translation["localFallbackReason"] = translation.get("error") or translation.get("backendDetail", "")
                    translation = api_translation
                translate_payloads.append(translation)
                translated_text = str(translation.get("translatedText", "") or "").strip()
                if translation.get("success") and translated_text:
                    translated = dict(cue)
                    translated["translatedText"] = translated_text
                    translated_cues.append(translated)
        if translated_cues:
            _write_vtt(args.output_translated_vtt, translated_cues, "translatedText")

        blocker = ""
        if not source_cues:
            blocker = transcribe.get("error") or "long-asr-source-empty"
        elif not translated_cues:
            if _translation_provider_configured():
                blocker = "translation-provider-available-but-longasr-translation-failed"
            else:
                blocker = "translation-provider-unavailable"
        payload = {
            "success": bool(source_cues and translated_cues),
            "blocker": blocker,
            "sourcePath": args.output_source_vtt,
            "translatedPath": args.output_translated_vtt,
            "sourceCueCount": len(source_cues),
            "translatedCueCount": len(translated_cues),
            "transcribe": transcribe,
            "translationAttempts": translate_payloads,
            "writesQuickZhSidecar": False,
            "usesQuickWorkerLane": False,
            "elapsedSeconds": round(time.time() - started_at, 3),
        }
        with open(args.output_json, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
        success = bool(source_cues and translated_cues)
        payload["success"] = success
        result_payload = dict(payload)
        result_payload.pop("success", None)
        return _result(success, **result_payload)


if __name__ == "__main__":
    raise SystemExit(main())
