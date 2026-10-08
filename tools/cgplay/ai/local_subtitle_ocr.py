#!/usr/bin/env python3
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile


class _NoSubtitleDetected(RuntimeError):
    pass


_EXTRACTOR_CACHE = None
_EXTRACTOR_BACKEND_DETAIL = "rapid_videocr single-frame OCR"
_PADDLE_OCR_CACHE = {}
_DEFAULT_PADDLE_MODEL_ROOT = os.path.join("E:/rjkf", "models", "paddleocr")


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


def _normalize_text(text):
    normalized = str(text or "").replace("\r", "\n")
    normalized = "\n".join(line.strip() for line in normalized.splitlines() if line.strip())
    return normalized.strip()


def _fake_vsf_filename():
    return "0_00_00_000__0_00_00_500_000000000000000000000000.png"


def _candidate_source_roots(env_name, leaf_name, package_dir):
    candidates = []

    env_value = os.environ.get(env_name, "").strip()
    if env_value:
        candidates.extend(part.strip() for part in env_value.split(os.pathsep) if part.strip())

    ai_sources_root = os.environ.get("CGPLAY_AI_SOURCES_ROOT", "").strip()
    if ai_sources_root:
        candidates.append(os.path.join(ai_sources_root, leaf_name))

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


def _candidate_model_roots():
    candidates = []
    for env_name in (
        "CGPLAY_PADDLEOCR_MODEL_ROOT",
        "CGPLAY_OCR_MODEL_ROOT",
        "CGPLAY_AI_MODELS_ROOT",
        "CGPLAY_MODELS_ROOT",
    ):
        value = os.environ.get(env_name, "").strip()
        if value:
            candidates.append(value)
    candidates.append(_DEFAULT_PADDLE_MODEL_ROOT)

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


def _candidate_model_path(*parts):
    for root in _candidate_model_roots():
        candidate = os.path.join(root, *parts)
        if os.path.isdir(candidate):
            return candidate
    return ""


def _paddle_model_paths():
    det_model_dir = _candidate_model_path("PP-OCRv6_medium_det")
    rec_model_dir = _candidate_model_path("PP-OCRv6_medium_rec")
    return det_model_dir, rec_model_dir


def _import_rapidvideocr():
    global _EXTRACTOR_BACKEND_DETAIL

    can_fallback_to_source = True
    try:
        from rapid_videocr import RapidVideOCR, RapidVideOCRInput

        _EXTRACTOR_BACKEND_DETAIL = "rapid_videocr single-frame OCR"
        return RapidVideOCR, RapidVideOCRInput
    except ModuleNotFoundError as first_error:
        last_error = first_error
        missing_name = str(getattr(first_error, "name", "") or "").strip()
        can_fallback_to_source = (
            not missing_name or
            missing_name == "rapid_videocr" or
            missing_name.startswith("rapid_videocr.")
        )

    if can_fallback_to_source:
        for source_root in _candidate_source_roots(
            "CGPLAY_RAPIDVIDEOCR_SOURCE",
            "RapidVideOCR-main",
            "rapid_videocr",
        ):
            _prepend_sys_path_once(source_root)
            try:
                from rapid_videocr import RapidVideOCR, RapidVideOCRInput

                _EXTRACTOR_BACKEND_DETAIL = f"rapid_videocr single-frame OCR (source:{source_root})"
                return RapidVideOCR, RapidVideOCRInput
            except ModuleNotFoundError as exc:
                last_error = exc

    raise last_error


def _import_paddleocr():
    last_error = None
    try:
        from paddleocr import PaddleOCR  # type: ignore
        return PaddleOCR
    except Exception as exc:
        last_error = exc

    for source_root in _candidate_source_roots(
        "CGPLAY_PADDLEOCR_SOURCE",
        "PaddleOCR-main",
        "paddleocr",
    ):
        _prepend_sys_path_once(source_root)
        try:
            from paddleocr import PaddleOCR  # type: ignore
            return PaddleOCR
        except Exception as exc:
            last_error = exc

    raise last_error


def _load_extractor():
    global _EXTRACTOR_CACHE
    if _EXTRACTOR_CACHE is not None:
        return _EXTRACTOR_CACHE

    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        RapidVideOCR, RapidVideOCRInput = _import_rapidvideocr()

        input_args = RapidVideOCRInput(
            is_batch_rec=False,
            out_format="txt",
        )
        _EXTRACTOR_CACHE = RapidVideOCR(input_args)
    return _EXTRACTOR_CACHE


def _load_paddleocr():
    det_model_dir, rec_model_dir = _paddle_model_paths()
    if not det_model_dir or not rec_model_dir:
        raise ModuleNotFoundError("paddleocr model directories are missing")

    cache_key = f"{det_model_dir}::{rec_model_dir}"
    if cache_key in _PADDLE_OCR_CACHE:
        return _PADDLE_OCR_CACHE[cache_key]

    PaddleOCR = _import_paddleocr()
    kwargs = {
        "use_angle_cls": False,
        "show_log": False,
        "det_model_dir": det_model_dir,
        "rec_model_dir": rec_model_dir,
        "use_gpu": False,
        "lang": "ch",
    }
    try:
        extractor = PaddleOCR(**kwargs)
    except TypeError:
        kwargs.pop("use_gpu", None)
        extractor = PaddleOCR(**kwargs)
    _PADDLE_OCR_CACHE[cache_key] = extractor
    return extractor


def _run_paddleocr(image_path):
    extractor = _load_paddleocr()
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        try:
            result = extractor.ocr(image_path, cls=False)
        except TypeError:
            result = extractor.ocr(image_path)

    text_parts = []
    if isinstance(result, list):
        for block in result:
            if not block:
                continue
            for item in block:
                if not item or len(item) < 2:
                    continue
                payload = item[1]
                if isinstance(payload, (list, tuple)) and payload:
                    text = str(payload[0] or "").strip()
                else:
                    text = str(payload or "").strip()
                if text:
                    text_parts.append(text)

    text = _normalize_text(" ".join(text_parts))
    if not text:
        raise _NoSubtitleDetected("NO_SUBTITLE")

    return {
        "backend": "paddleocr",
        "backendDetail": f"paddleocr det={_paddle_model_paths()[0]} rec={_paddle_model_paths()[1]}",
        "text": text,
    }


def _run_rapidvideocr(image_path):
    extractor = _load_extractor()
    with tempfile.TemporaryDirectory(prefix="cgplay_vsf_") as vsf_dir, tempfile.TemporaryDirectory(
        prefix="cgplay_vsf_out_"
    ) as out_dir:
        staged_path = os.path.join(vsf_dir, _fake_vsf_filename())
        shutil.copyfile(image_path, staged_path)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            txt_result = extractor(vsf_dir, out_dir, save_name="cgplay")
        if isinstance(txt_result, list):
            text = _normalize_text(" ".join(str(item).strip() for item in txt_result if str(item).strip()))
        else:
            text = _normalize_text(txt_result)

        if not text:
            fallback_txt = os.path.join(out_dir, "cgplay.txt")
            if os.path.exists(fallback_txt):
                with open(fallback_txt, "r", encoding="utf-8", errors="ignore") as handle:
                    text = _normalize_text(handle.read())

        if not text:
            raise _NoSubtitleDetected("NO_SUBTITLE")

        return {
            "backend": "rapid_videocr",
            "backendDetail": _EXTRACTOR_BACKEND_DETAIL,
            "text": text,
        }


def _handle_ocr_request(image_path, language_hint):
    if not image_path:
        return {"success": False, "error": "missing image path", "backend": "rapid_videocr_failed"}
    if not os.path.exists(image_path):
        return {
            "success": False,
            "error": "image file does not exist",
            "backend": "rapid_videocr_failed",
        }

    try:
        try:
            payload = _run_paddleocr(image_path)
        except Exception as paddle_exc:
            payload = _run_rapidvideocr(image_path)
            if "backendDetail" in payload:
                payload["backendDetail"] = f"{payload['backendDetail']} | paddleocr_fallback={paddle_exc.__class__.__name__}"
        payload["success"] = True
        if language_hint:
            payload["languageHint"] = language_hint
        return payload
    except ModuleNotFoundError as exc:
        return {
            "success": False,
            "error": "subtitle OCR backend dependencies are missing",
            "backend": "subtitle_ocr_unavailable",
            "backendDetail": str(exc),
        }
    except _NoSubtitleDetected:
        return {
            "success": False,
            "error": "NO_SUBTITLE",
            "backend": "rapid_videocr_no_text",
            "backendDetail": "empty_ocr_result",
        }
    except Exception as exc:
        return {
            "success": False,
            "error": str(exc) if str(exc) else "rapid_videocr failed",
            "backend": "rapid_videocr_failed",
            "backendDetail": exc.__class__.__name__,
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
                "backend": "rapid_videocr_failed",
                "backendDetail": "invalid_json",
            }
        else:
            response = _handle_ocr_request(
                str(request.get("image_path", "")),
                str(request.get("language_hint", "")),
            )
        sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
        sys.stdout.flush()
    return 0


def main():
    _configure_stdio()

    if len(sys.argv) >= 2 and sys.argv[1] == "--serve":
        return _serve()

    if len(sys.argv) < 2:
        return _result(False, error="missing image path")

    image_path = sys.argv[1]
    language_hint = sys.argv[2].strip() if len(sys.argv) > 2 else ""
    payload = _handle_ocr_request(image_path, language_hint)
    success = bool(payload.pop("success", False))
    return _result(success, **payload)


if __name__ == "__main__":
    raise SystemExit(main())
