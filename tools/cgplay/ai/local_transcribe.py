#!/usr/bin/env python3
import importlib.util
import json
import os
import site
import sys

import requests


class _NoSpeechDetected(RuntimeError):
    pass


_MODEL_CACHE = {}
_WHISPERLIVE_SOURCE_MODEL_CACHE = {}
_DEFAULT_LOCAL_TRANSCRIBE_MODEL = "large-v3-turbo-ct2"


def _requested_device():
    value = os.environ.get("CGPLAY_FASTER_WHISPER_DEVICE", "").strip().lower()
    return value or "auto"


def _requested_compute_type(device):
    value = os.environ.get("CGPLAY_FASTER_WHISPER_COMPUTE", "").strip().lower()
    if value:
        return value
    return "float16" if device == "cuda" else "int8"


def _device_candidates():
    device = _requested_device()
    if device == "auto":
        return ["cuda", "cpu"]
    if device == "cuda":
        return ["cuda", "cpu"]
    return [device]


def _configure_stdio():
    for stream_name in ("stdin", "stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        if stream is None or not hasattr(stream, "reconfigure"):
            continue
        try:
            stream.reconfigure(encoding="utf-8")
        except Exception:
            pass


def _result(success, **kwargs):
    payload = {"success": success}
    payload.update(kwargs)
    sys.stdout.write(json.dumps(payload, ensure_ascii=False))
    return 0 if success else 1


def _candidate_source_roots(env_name, leaf_name, package_dir):
    candidates = []

    env_value = os.environ.get(env_name, "").strip()
    if env_value:
        candidates.extend(part.strip() for part in env_value.split(os.pathsep) if part.strip())

    ai_sources_root = os.environ.get("CGPLAY_AI_SOURCES_ROOT", "").strip()
    if ai_sources_root:
        candidates.append(os.path.join(ai_sources_root, leaf_name))

    candidates.append(os.path.join("E:/rjkf", leaf_name))
    candidates.append(os.path.join(os.path.expanduser("~"), "Desktop", "AI_Sources", leaf_name))

    resolved = []
    seen = set()
    for candidate in candidates:
        expanded = os.path.abspath(os.path.expanduser(candidate))
        package_init = os.path.join(expanded, package_dir, "__init__.py")
        if not os.path.exists(package_init):
            continue
        lowered = expanded.lower()
        if lowered in seen:
            continue
        seen.add(lowered)
        resolved.append(expanded)
    return resolved


def _prepend_sys_path_once(path_value):
    normalized = os.path.abspath(os.path.expanduser(path_value))
    current = [os.path.abspath(entry).lower() for entry in sys.path if entry]
    if normalized.lower() not in current:
        sys.path.insert(0, normalized)


def _add_dll_directory_once(path_value):
    normalized = os.path.abspath(os.path.expanduser(path_value))
    if not os.path.isdir(normalized):
        return
    added = getattr(_add_dll_directory_once, "_added", None)
    if added is None:
        added = set()
        _add_dll_directory_once._added = added
    lowered = normalized.lower()
    if lowered in added:
        return
    try:
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(normalized)
    except Exception:
        pass
    added.add(lowered)


def _prepare_cuda_dll_paths():
    candidates = []
    for env_name in ("CUDA_PATH", "CUDA_PATH_V12_0", "CUDA_PATH_V12_1", "CUDA_PATH_V12_2", "CUDA_PATH_V12_3"):
        value = os.environ.get(env_name, "").strip()
        if value:
            candidates.extend(
                [
                    os.path.join(value, "bin"),
                    os.path.join(value, "lib", "x64"),
                ]
            )

    for root in filter(None, [sys.prefix, getattr(sys, "base_prefix", "")]):
        candidates.extend(
            [
                os.path.join(root, "Library", "bin"),
                os.path.join(root, "DLLs"),
                os.path.join(root, "Lib", "site-packages"),
            ]
        )

    for package_root in site.getsitepackages() + [site.getusersitepackages()]:
        if package_root:
            candidates.append(package_root)

    for candidate in candidates:
        if not os.path.isdir(candidate):
            continue
        for dll_name in ("cublas64_12.dll", "cudnn64_9.dll", "cudart64_12.dll", "nvrtc64_12.dll", "nvToolsExt64_1.dll"):
            found = os.path.join(candidate, dll_name)
            if os.path.exists(found):
                _add_dll_directory_once(candidate)
                break
        else:
            for child in os.listdir(candidate):
                child_path = os.path.join(candidate, child)
                if not os.path.isdir(child_path):
                    continue
                for dll_name in ("cublas64_12.dll", "cudnn64_9.dll", "cudart64_12.dll", "nvrtc64_12.dll", "nvToolsExt64_1.dll"):
                    found = os.path.join(child_path, dll_name)
                    if os.path.exists(found):
                        _add_dll_directory_once(child_path)
                        break


def _candidate_model_roots():
    candidates = []
    for env_name in (
        "CGPLAY_FASTER_WHISPER_MODEL_ROOT",
        "CGPLAY_LOCAL_TRANSCRIBE_MODEL_ROOT",
        "CGPLAY_AI_MODELS_ROOT",
        "CGPLAY_MODELS_ROOT",
    ):
        value = os.environ.get(env_name, "").strip()
        if value:
            candidates.append(value)

    candidates.extend(
        [
            os.path.join("E:/rjkf", "models"),
            os.path.join(os.path.expanduser("~"), "Desktop", "AI_Sources", "models"),
        ]
    )

    resolved = []
    seen = set()
    for candidate in candidates:
        expanded = os.path.abspath(os.path.expanduser(candidate))
        lowered = expanded.lower()
        if lowered in seen:
            continue
        seen.add(lowered)
        resolved.append(expanded)
    return resolved


def _faster_whisper_model_root():
    for candidate in _candidate_model_roots():
        direct = os.path.join(candidate, "faster-whisper")
        if os.path.isdir(direct):
            return direct
        if os.path.isdir(candidate) and os.path.basename(candidate).lower() == "faster-whisper":
            return candidate
    return ""


def _resolve_faster_whisper_model_name(model_name):
    requested = str(model_name or "").strip()
    model_root = _faster_whisper_model_root()

    if model_root:
        preferred_local = os.path.join(model_root, _DEFAULT_LOCAL_TRANSCRIBE_MODEL)
        if not requested or requested.lower() in {"tiny", "base", "small"}:
            if os.path.isdir(preferred_local):
                return preferred_local

        if requested:
            if os.path.isdir(requested):
                return os.path.abspath(requested)
            normalized = requested.replace("\\", "/").strip("/")
            local_candidate = os.path.join(model_root, normalized.replace("/", os.sep))
            if os.path.isdir(local_candidate):
                return local_candidate
            basename_candidate = os.path.join(model_root, os.path.basename(normalized))
            if os.path.isdir(basename_candidate):
                return basename_candidate

    if requested:
        return requested
    return _DEFAULT_LOCAL_TRANSCRIBE_MODEL


def _whisperlive_source_root():
    candidates = _candidate_source_roots(
        "CGPLAY_WHISPERLIVE_SOURCE",
        "WhisperLive-main",
        "whisper_live",
    )
    return candidates[0] if candidates else ""


def _faster_whisper_source_root():
    candidates = _candidate_source_roots(
        "CGPLAY_FASTER_WHISPER_SOURCE",
        "faster-whisper-master",
        "faster_whisper",
    )
    return candidates[0] if candidates else ""


def _whisperlive_source_probe():
    source_root = _whisperlive_source_root()
    if not source_root:
        return {
            "available": False,
            "source_root": "",
            "missing_modules": [],
            "reason": "whisperlive_source_not_found",
        }

    _prepend_sys_path_once(source_root)
    required_modules = [
        "whisper_live.transcriber.transcriber_faster_whisper",
        "ctranslate2",
        "faster_whisper",
        "numpy",
        "tokenizers",
        "tqdm",
    ]
    missing_modules = [name for name in required_modules if importlib.util.find_spec(name) is None]
    return {
        "available": not missing_modules,
        "source_root": source_root,
        "missing_modules": missing_modules,
        "reason": "ready" if not missing_modules else "missing_dependencies",
    }


def _load_faster_whisper_model(model_name):
    _prepare_cuda_dll_paths()
    source_root = _faster_whisper_source_root()
    if source_root:
        _prepend_sys_path_once(source_root)

    from faster_whisper import WhisperModel

    resolved_model_name = _resolve_faster_whisper_model_name(model_name)
    cache_key = resolved_model_name or _DEFAULT_LOCAL_TRANSCRIBE_MODEL
    for device in _device_candidates():
        device_cache_key = f"{cache_key}::{device}"
        if device_cache_key in _MODEL_CACHE:
            return _MODEL_CACHE[device_cache_key]
        compute_type = _requested_compute_type(device)
        try:
            model = WhisperModel(cache_key, device=device, compute_type=compute_type)
        except Exception:
            continue
        _MODEL_CACHE[device_cache_key] = model
        return model
    raise RuntimeError("failed to load faster-whisper model on any available device")


def _load_whisperlive_source_model(model_name):
    _prepare_cuda_dll_paths()
    probe = _whisperlive_source_probe()
    if not probe["available"]:
        missing = ",".join(probe["missing_modules"])
        if missing:
            raise ModuleNotFoundError(f"whisperlive source missing dependencies: {missing}")
        raise ModuleNotFoundError("whisperlive source is unavailable")

    resolved_model_name = _resolve_faster_whisper_model_name(model_name)
    cache_base_key = f"{probe['source_root']}::{resolved_model_name or _DEFAULT_LOCAL_TRANSCRIBE_MODEL}"
    for device in _device_candidates():
        cache_key = f"{cache_base_key}::{device}"
        if cache_key in _WHISPERLIVE_SOURCE_MODEL_CACHE:
            return _WHISPERLIVE_SOURCE_MODEL_CACHE[cache_key], probe["source_root"]

    from whisper_live.transcriber.transcriber_faster_whisper import WhisperModel

    for device in _device_candidates():
        compute_type = _requested_compute_type(device)
        try:
            model = WhisperModel(resolved_model_name or _DEFAULT_LOCAL_TRANSCRIBE_MODEL, device=device, compute_type=compute_type)
        except Exception:
            continue
        cache_key = f"{cache_base_key}::{device}"
        _WHISPERLIVE_SOURCE_MODEL_CACHE[cache_key] = model
        return model, probe["source_root"]
    raise RuntimeError("failed to load whisperlive source model on any available device")


def _transcribe_with_faster_whisper(audio_path, language_hint, model_name):
    model = _load_faster_whisper_model(model_name)
    segments, info = model.transcribe(
        audio_path,
        language=None if not language_hint or language_hint == "auto" else language_hint,
        vad_filter=True,
        beam_size=1,
        best_of=1,
        condition_on_previous_text=False,
    )

    text_parts = []
    segment_items = []
    last_text = ""
    for segment in segments:
        text = (segment.text or "").strip()
        if text and text != last_text:
            text_parts.append(text)
            segment_items.append(
                {
                    "start": float(getattr(segment, "start", 0.0) or 0.0),
                    "end": float(getattr(segment, "end", 0.0) or 0.0),
                    "text": text,
                }
            )
            last_text = text

    text = "\n".join(text_parts).strip()
    if not text:
        raise _NoSpeechDetected("NO_SPEECH")

    detected_language = getattr(info, "language", "") or ""
    return {
        "backend": "local_faster_whisper",
        "backendDetail": f"faster-whisper:{model_name}",
        "text": text,
        "segments": segment_items,
        "detectedLanguage": detected_language,
    }


def _transcribe_with_whisperlive_source(audio_path, language_hint, model_name):
    model, source_root = _load_whisperlive_source_model(model_name)
    segments, info = model.transcribe(
        audio_path,
        language=None if not language_hint or language_hint == "auto" else language_hint,
        vad_filter=True,
        beam_size=1,
        best_of=1,
        condition_on_previous_text=False,
    )

    text_parts = []
    segment_items = []
    last_text = ""
    for segment in segments:
        text = (getattr(segment, "text", "") or "").strip()
        if text and text != last_text:
            text_parts.append(text)
            segment_items.append(
                {
                    "start": float(getattr(segment, "start", 0.0) or 0.0),
                    "end": float(getattr(segment, "end", 0.0) or 0.0),
                    "text": text,
                }
            )
            last_text = text

    text = "\n".join(text_parts).strip()
    if not text:
        raise _NoSpeechDetected("NO_SPEECH")

    detected_language = getattr(info, "language", "") or ""
    return {
        "backend": "whisper_live_source",
        "backendDetail": f"{source_root}::{model_name or 'tiny'}",
        "text": text,
        "segments": segment_items,
        "detectedLanguage": detected_language,
    }


def _resolve_whisperlive_base_url(explicit_base_url):
    base_url = (explicit_base_url or os.environ.get("CGPLAY_WHISPERLIVE_URL", "")).strip()
    if not base_url:
        return ""
    return base_url.rstrip("/")


def _transcribe_with_whisperlive_rest(
    audio_path,
    language_hint,
    model_name,
    base_url,
    api_key,
):
    resolved_model = (model_name or os.environ.get("CGPLAY_WHISPERLIVE_MODEL", "small")).strip() or "small"
    endpoint = f"{base_url}/v1/audio/transcriptions"
    headers = {}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    data = {
        "model": resolved_model,
        "response_format": "verbose_json",
    }
    if language_hint and language_hint.lower() != "auto":
        data["language"] = language_hint

    with open(audio_path, "rb") as handle:
        response = requests.post(
            endpoint,
            headers=headers,
            files={"file": (os.path.basename(audio_path), handle, "audio/wav")},
            data=data,
            timeout=(5, 120),
        )

    response.raise_for_status()
    payload = response.json() if response.content else {}
    text = str(payload.get("text", "")).strip()
    if not text:
        raise _NoSpeechDetected("NO_SPEECH")

    detected_language = str(payload.get("language", "")).strip()
    segment_items = []
    for segment in payload.get("segments", []) if isinstance(payload.get("segments", []), list) else []:
        if not isinstance(segment, dict):
            continue
        segment_text = str(segment.get("text", "")).strip()
        if not segment_text:
            continue
        segment_items.append(
            {
                "start": float(segment.get("start", 0.0) or 0.0),
                "end": float(segment.get("end", 0.0) or 0.0),
                "text": segment_text,
            }
        )
    return {
        "backend": "whisper_live_rest",
        "backendDetail": f"{base_url}::{resolved_model}",
        "text": text,
        "segments": segment_items,
        "detectedLanguage": detected_language,
    }


def _handle_transcribe_request(
    audio_path,
    language_hint,
    model_name,
    prefer_backend="auto",
    whisperlive_url="",
    whisperlive_api_key="",
):
    if not model_name:
        model_name = os.environ.get("CGPLAY_LOCAL_TRANSCRIBE_MODEL", "")

    if not os.path.exists(audio_path):
        return {"success": False, "error": "audio file does not exist"}

    backend_mode = (prefer_backend or os.environ.get("CGPLAY_LOCAL_TRANSCRIBE_BACKEND", "auto")).strip().lower()
    whisperlive_base_url = _resolve_whisperlive_base_url(whisperlive_url)
    whisperlive_source = _whisperlive_source_probe()
    last_whisperlive_error = ""
    last_source_error = ""

    if whisperlive_base_url and backend_mode in ("auto", "whisperlive", "whisperlive_only"):
        try:
            payload = _transcribe_with_whisperlive_rest(
                audio_path,
                language_hint,
                model_name,
                whisperlive_base_url,
                whisperlive_api_key.strip(),
            )
            payload["success"] = True
            return payload
        except _NoSpeechDetected:
            return {
                "success": False,
                "error": "NO_SPEECH",
                "backend": "whisper_live_rest",
                "backendDetail": f"{whisperlive_base_url}::empty_transcript",
            }
        except ModuleNotFoundError as exc:
            last_whisperlive_error = f"whisperlive dependency missing: {exc}"
        except requests.RequestException as exc:
            last_whisperlive_error = f"whisperlive rest failed: {exc}"
        except Exception as exc:
            last_whisperlive_error = f"whisperlive failed: {exc}"

        if backend_mode == "whisperlive_only":
            return {
                "success": False,
                "error": last_whisperlive_error or "whisperlive unavailable",
                "backend": "whisper_live_rest",
                "backendDetail": whisperlive_base_url or "whisperlive_unavailable",
            }

    if backend_mode in ("whisperlive_source", "whisperlive_source_only"):
        try:
            payload = _transcribe_with_whisperlive_source(audio_path, language_hint, model_name)
            payload["success"] = True
            return payload
        except _NoSpeechDetected:
            return {
                "success": False,
                "error": "NO_SPEECH",
                "backend": "whisper_live_source",
                "backendDetail": whisperlive_source["source_root"] or "whisperlive_source",
            }
        except ModuleNotFoundError as exc:
            last_source_error = str(exc)
        except Exception as exc:
            last_source_error = f"whisperlive source failed: {exc}"

        if backend_mode == "whisperlive_source_only":
            return {
                "success": False,
                "error": last_source_error or "whisperlive source unavailable",
                "backend": "whisper_live_source",
                "backendDetail": whisperlive_source["source_root"] or whisperlive_source["reason"],
            }

    try:
        payload = _transcribe_with_faster_whisper(audio_path, language_hint, model_name)
        if last_whisperlive_error:
            payload["backendDetail"] = (
                f"{payload['backendDetail']} | fallback_after_whisperlive={last_whisperlive_error}"
            )
        if last_source_error:
            payload["backendDetail"] = (
                f"{payload['backendDetail']} | fallback_after_whisperlive_source={last_source_error}"
            )
        elif whisperlive_source["source_root"]:
            if whisperlive_source["available"]:
                payload["backendDetail"] = (
                    f"{payload['backendDetail']} | whisperlive_source_ready={whisperlive_source['source_root']}"
                )
            else:
                payload["backendDetail"] = (
                    f"{payload['backendDetail']} | whisperlive_source_found_but_missing="
                    f"{','.join(whisperlive_source['missing_modules'])}"
                )
        payload["success"] = True
        return payload
    except ModuleNotFoundError as exc:
        return {
            "success": False,
            "error": "local faster-whisper dependency is missing",
            "backend": "local_unavailable",
            "backendDetail": str(exc),
        }
    except _NoSpeechDetected:
        return {
            "success": False,
            "error": "NO_SPEECH",
            "backend": "local_no_speech",
            "backendDetail": "empty_transcript",
        }
    except Exception as exc:
        return {
            "success": False,
            "error": str(exc) if str(exc) else "local transcription failed",
            "backend": "local_failed",
            "backendDetail": last_whisperlive_error or exc.__class__.__name__,
        }


def _serve():
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except Exception as exc:
            response = {
                "success": False,
                "error": f"invalid request: {exc}",
                "backend": "local_failed",
                "backendDetail": "invalid_json",
            }
        else:
            response = _handle_transcribe_request(
                str(request.get("audio_path", "")),
                str(request.get("language_hint", "")),
                str(request.get("model_name", "")),
                str(request.get("prefer_backend", "")),
                str(request.get("whisperlive_url", "")),
                str(request.get("whisperlive_api_key", "")),
            )
        sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
        sys.stdout.flush()
    return 0


def main():
    _configure_stdio()

    if len(sys.argv) >= 2 and sys.argv[1] == "--serve":
        return _serve()

    if len(sys.argv) < 2:
        return _result(False, error="missing audio path")

    audio_path = sys.argv[1]
    language_hint = sys.argv[2].strip() if len(sys.argv) > 2 else ""
    model_name = sys.argv[3].strip() if len(sys.argv) > 3 else ""
    payload = _handle_transcribe_request(audio_path, language_hint, model_name)
    success = bool(payload.pop("success", False))
    return _result(success, **payload)


if __name__ == "__main__":
    raise SystemExit(main())
