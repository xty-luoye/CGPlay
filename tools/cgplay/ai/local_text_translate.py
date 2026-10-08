#!/usr/bin/env python3
import json
import os
import sys
import threading
import warnings
from functools import lru_cache


def _configure_stdio():
    for stream_name in ("stdin", "stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        if stream is None or not hasattr(stream, "reconfigure"):
            continue
        try:
            stream.reconfigure(encoding="utf-8")
        except Exception:
            pass
    warnings.filterwarnings("ignore")


def _result(success, **kwargs):
    payload = {"success": success}
    payload.update(kwargs)
    sys.stdout.write(json.dumps(payload, ensure_ascii=False))
    return 0 if success else 1


def _normalize_lang(code):
    normalized = str(code or "").strip().lower().replace("_", "-")
    if not normalized or normalized == "auto":
        return ""
    if normalized.startswith("zh"):
        if normalized in ("zh-tw", "zh-hk", "zh-mo", "zh-hant"):
            return "zh-hant"
        return "zh-hans"
    if normalized.startswith("ja") or normalized.startswith("jp"):
        return "ja"
    if normalized.startswith("ko"):
        return "ko"
    if normalized.startswith("en"):
        return "en"
    return normalized.split("-", 1)[0] if "-" in normalized else normalized


def _infer_source_language(text):
    han = 0
    kana = 0
    hangul = 0
    latin = 0
    for ch in str(text or ""):
        code = ord(ch)
        if 0x3400 <= code <= 0x9FFF or 0xF900 <= code <= 0xFAFF:
            han += 1
        elif 0x3040 <= code <= 0x30FF:
            kana += 1
        elif 0xAC00 <= code <= 0xD7AF:
            hangul += 1
        elif ch.isalpha() and code < 128:
            latin += 1

    if kana > 0:
        return "ja"
    if hangul > 0:
        return "ko"
    if han >= 2:
        return _infer_chinese_variant(text)
    if latin >= 2:
        return "en"
    return ""


def _model_candidates(from_code, to_code):
    direct = {
        ("ja", "en"): ["Helsinki-NLP/opus-mt-ja-en", "Helsinki-NLP/opus-mt-jap-en"],
        ("ja", "zh"): ["shun89/opus-mt-ja-zh", "Helsinki-NLP/opus-mt-ja-zh"],
        ("en", "ja"): ["Helsinki-NLP/opus-mt-en-jap"],
        ("en", "zh"): ["Helsinki-NLP/opus-mt-en-zh"],
        ("zh", "en"): ["Helsinki-NLP/opus-mt-zh-en"],
        ("zh", "ja"): ["shun89/opus-mt-zh-ja", "Helsinki-NLP/opus-mt-zh-ja", "Helsinki-NLP/opus-mt-tc-big-zh-ja"],
        ("ko", "en"): ["Helsinki-NLP/opus-mt-ko-en", "Helsinki-NLP/opus-mt-tc-big-ko-en"],
        ("ko", "zh"): ["shun89/opus-mt-ko-zh", "Helsinki-NLP/opus-mt-ko-zh"],
        ("en", "ko"): ["Helsinki-NLP/opus-mt-tc-big-en-ko"],
        ("zh", "ko"): ["shun89/opus-mt-zh-ko", "Helsinki-NLP/opus-mt-zh-ko"],
    }
    local_map = {
        "Helsinki-NLP/opus-mt-ja-en": "Helsinki-NLP__opus-mt-ja-en",
        "Helsinki-NLP/opus-mt-jap-en": "Helsinki-NLP__opus-mt-jap-en",
        "shun89/opus-mt-ja-zh": "shun89__opus-mt-ja-zh",
        "Helsinki-NLP/opus-mt-ja-zh": "Helsinki-NLP__opus-mt-ja-zh",
        "Helsinki-NLP/opus-mt-en-jap": "Helsinki-NLP__opus-mt-en-jap",
        "Helsinki-NLP/opus-mt-en-zh": "opus-mt-en-zh",
        "Helsinki-NLP/opus-mt-zh-en": "opus-mt-zh-en",
        "shun89/opus-mt-zh-ja": "shun89__opus-mt-zh-ja",
        "Helsinki-NLP/opus-mt-zh-ja": "Helsinki-NLP__opus-mt-zh-ja",
        "Helsinki-NLP/opus-mt-tc-big-zh-ja": "Helsinki-NLP__opus-mt-tc-big-zh-ja",
        "Helsinki-NLP/opus-mt-ko-en": "Helsinki-NLP__opus-mt-ko-en",
        "Helsinki-NLP/opus-mt-tc-big-ko-en": "Helsinki-NLP__opus-mt-tc-big-ko-en",
        "shun89/opus-mt-ko-zh": "shun89__opus-mt-ko-zh",
        "Helsinki-NLP/opus-mt-ko-zh": "Helsinki-NLP__opus-mt-ko-zh",
        "Helsinki-NLP/opus-mt-tc-big-en-ko": "Helsinki-NLP__opus-mt-tc-big-en-ko",
        "shun89/opus-mt-zh-ko": "shun89__opus-mt-zh-ko",
        "Helsinki-NLP/opus-mt-zh-ko": "Helsinki-NLP__opus-mt-zh-ko",
    }
    candidates = []
    for model_id in direct.get((from_code, to_code), []):
        candidates.append(model_id)
        local_name = local_map.get(model_id, "")
        if local_name:
            candidates.append(local_name)
    return candidates


def _import_transformers():
    import torch
    from transformers import AutoModelForSeq2SeqLM, AutoTokenizer

    return torch, AutoTokenizer, AutoModelForSeq2SeqLM


_MODEL_CACHE = {}
_MODEL_LOCK = threading.Lock()
_OPENCC_CACHE = {}
_OPENCC_LOCK = threading.Lock()
_DEFAULT_MODEL_ROOT = os.path.join("E:/rjkf", "models", "translation")
_DEFAULT_MARIAN_NUM_BEAMS = 1
_DEFAULT_MARIAN_MAX_NEW_TOKENS = 128


def _try_load_opencc():
    try:
        from opencc import OpenCC  # type: ignore
        return OpenCC
    except Exception:
        pass
    try:
        from opencc_python_reimplemented import OpenCC  # type: ignore
        return OpenCC
    except Exception:
        return None


def _prepend_sys_path_once(path_value):
    normalized = os.path.abspath(os.path.expanduser(path_value))
    current = [os.path.abspath(entry).lower() for entry in sys.path if entry]
    if normalized.lower() not in current:
        sys.path.insert(0, normalized)


def _candidate_model_roots():
    candidates = []
    for env_name in (
        "CGPLAY_TRANSLATION_MODEL_ROOT",
        "CGPLAY_AI_MODELS_ROOT",
        "CGPLAY_MODELS_ROOT",
    ):
        value = os.environ.get(env_name, "").strip()
        if value:
            candidates.append(value)
    candidates.append(_DEFAULT_MODEL_ROOT)

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


def _candidate_model_paths(model_id):
    local_name = model_id.replace("/", "__")
    for root in _candidate_model_roots():
        yield os.path.join(root, local_name)
        yield os.path.join(root, os.path.basename(model_id))
        yield os.path.join(root, model_id)


def _get_opencc(config_name):
    with _OPENCC_LOCK:
        if config_name in _OPENCC_CACHE:
            return _OPENCC_CACHE[config_name]
        opencc_cls = _try_load_opencc()
        if opencc_cls is None:
            _OPENCC_CACHE[config_name] = None
            return None
        try:
            converter = opencc_cls(config_name)
        except Exception:
            converter = None
        _OPENCC_CACHE[config_name] = converter
        return converter


def _count_variant_changes(original, converted):
    limit = min(len(original), len(converted))
    delta = sum(1 for i in range(limit) if original[i] != converted[i])
    delta += abs(len(original) - len(converted))
    return delta


def _infer_chinese_variant(text):
    cleaned = str(text or "").strip()
    if not cleaned:
        return "zh-hans"

    trad_hint_chars = set("卻無覺點體臺與後會專業畫龍門開關顯機應讓聲書對氣燈為實際變數")
    simp_hint_chars = set("却无觉点体台与后会专业画龙门开关显机应让声书对气灯为实际变数")

    trad_hits = sum(1 for ch in cleaned if ch in trad_hint_chars)
    simp_hits = sum(1 for ch in cleaned if ch in simp_hint_chars)
    if trad_hits > simp_hits:
        return "zh-hant"
    if simp_hits > trad_hits:
        return "zh-hans"

    t2s = _get_opencc("t2s")
    s2t = _get_opencc("s2t")
    if t2s is not None and s2t is not None:
        try:
            t2s_changes = _count_variant_changes(cleaned, t2s.convert(cleaned))
            s2t_changes = _count_variant_changes(cleaned, s2t.convert(cleaned))
            if t2s_changes > s2t_changes and t2s_changes > 0:
                return "zh-hant"
            if s2t_changes > t2s_changes and s2t_changes > 0:
                return "zh-hans"
        except Exception:
            pass

    return "zh-hans"


def _convert_chinese_variant(text, source_variant, target_variant):
    if source_variant == target_variant:
        return str(text or "").strip(), f"same-variant:{source_variant}"

    config_name = "t2s" if target_variant == "zh-hans" else "s2t"
    converter = _get_opencc(config_name)
    if converter is None:
        raise RuntimeError("traditional/simplified conversion backend unavailable")

    converted = converter.convert(str(text or "").strip()).strip()
    if not converted:
        raise RuntimeError("traditional/simplified conversion returned empty text")
    return converted, f"opencc:{config_name}"


def _load_model(model_id):
    with _MODEL_LOCK:
        if model_id in _MODEL_CACHE:
            return _MODEL_CACHE[model_id]

        torch, AutoTokenizer, AutoModelForSeq2SeqLM = _import_transformers()
        local_path = ""
        for candidate_path in _candidate_model_paths(model_id):
            if os.path.isdir(candidate_path):
                local_path = candidate_path
                break
        if local_path:
            _prepend_sys_path_once(local_path)
        try:
            if local_path:
                tokenizer = AutoTokenizer.from_pretrained(local_path, local_files_only=True)
                model = AutoModelForSeq2SeqLM.from_pretrained(local_path, local_files_only=True)
            else:
                tokenizer = AutoTokenizer.from_pretrained(model_id, local_files_only=True)
                model = AutoModelForSeq2SeqLM.from_pretrained(model_id, local_files_only=True)
        except Exception:
            tokenizer = AutoTokenizer.from_pretrained(local_path or model_id)
            model = AutoModelForSeq2SeqLM.from_pretrained(local_path or model_id)
        model.eval()
        _MODEL_CACHE[model_id] = (torch, tokenizer, model)
        return _MODEL_CACHE[model_id]


def _env_int(name, default_value, minimum=None, maximum=None):
    raw_value = str(os.environ.get(name, "")).strip()
    if not raw_value:
        return default_value
    try:
        value = int(raw_value)
    except Exception:
        return default_value
    if minimum is not None:
        value = max(minimum, value)
    if maximum is not None:
        value = min(maximum, value)
    return value


def _run_model(model_id, text):
    torch, tokenizer, model = _load_model(model_id)
    batch = tokenizer([text], return_tensors="pt", truncation=True)
    max_new_tokens = _env_int(
        "CGPLAY_MARIAN_MAX_NEW_TOKENS",
        min(_DEFAULT_MARIAN_MAX_NEW_TOKENS, max(32, len(text) * 2 + 16)),
        minimum=16,
        maximum=256,
    )
    num_beams = _env_int("CGPLAY_MARIAN_NUM_BEAMS", _DEFAULT_MARIAN_NUM_BEAMS, minimum=1, maximum=4)
    with torch.no_grad():
        output = model.generate(
            **batch,
            max_new_tokens=max_new_tokens,
            num_beams=num_beams,
            do_sample=False,
            early_stopping=True,
        )
    decoded = tokenizer.batch_decode(output, skip_special_tokens=True)
    return str(decoded[0] if decoded else "").strip()


@lru_cache(maxsize=64)
def _resolve_route(from_code, to_code):
    direct = _model_candidates(from_code, to_code)
    if direct:
        return [direct[0]]

    if from_code != "en" and to_code != "en":
        first = _model_candidates(from_code, "en")
        second = _model_candidates("en", to_code)
        if first and second:
            return [first[0], second[0]]

    return []


def _translate_text(text, source_language, target_language):
    cleaned = str(text or "").strip()
    if not cleaned:
        raise RuntimeError("missing text")

    source = _normalize_lang(source_language) or _infer_source_language(cleaned)
    target = _normalize_lang(target_language) or "zh-hans"

    if not source:
        raise RuntimeError("cannot infer source language")
    if source.startswith("zh") and target.startswith("zh"):
        converted_text, backend_detail = _convert_chinese_variant(cleaned, source, target)
        return {
            "translatedText": converted_text,
            "sourceLanguage": source,
            "targetLanguage": target,
            "backendDetail": backend_detail,
        }
    if source == target:
        return {
            "translatedText": cleaned,
            "sourceLanguage": source,
            "targetLanguage": target,
            "backendDetail": f"same-language:{source}",
        }

    route_source = "zh" if source.startswith("zh") else source
    route_target = "zh" if target.startswith("zh") else target
    route = _resolve_route(route_source, route_target)
    if not route:
        raise RuntimeError(f"no local translation model route for {route_source}->{route_target}")

    prepared_text = cleaned
    backend_steps = []
    if source == "zh-hant" and route_source == "zh":
        prepared_text, source_backend = _convert_chinese_variant(cleaned, "zh-hant", "zh-hans")
        backend_steps.append(source_backend)

    segments = [segment.strip() for segment in prepared_text.replace("\r", "\n").split("\n") if segment.strip()]
    if not segments:
        segments = [prepared_text]

    translated_segments = []
    last_translated = ""
    for segment in segments:
        current = segment
        for model_id in route:
            current = _run_model(model_id, current)
            if not current:
                raise RuntimeError(f"empty translation result from {model_id}")
            if model_id not in backend_steps:
                backend_steps.append(model_id)

        if target == "zh-hant":
            current, target_backend = _convert_chinese_variant(current, "zh-hans", "zh-hant")
            if target_backend not in backend_steps:
                backend_steps.append(target_backend)
        if current != last_translated:
            translated_segments.append(current)
            last_translated = current

    return {
        "translatedText": "\n".join(translated_segments),
        "sourceLanguage": source,
        "targetLanguage": target,
        "backendDetail": " -> ".join(backend_steps),
    }


def _handle_request(text, source_language, target_language):
    try:
        payload = _translate_text(text, source_language, target_language)
        payload["success"] = True
        payload["backend"] = "local_marian_translate"
        return payload
    except ModuleNotFoundError as exc:
        return {
            "success": False,
            "error": "local translation dependencies are missing",
            "backend": "local_marian_unavailable",
            "backendDetail": str(exc),
        }
    except Exception as exc:
        return {
            "success": False,
            "error": str(exc) if str(exc) else "local text translation failed",
            "backend": "local_marian_failed",
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
                "backend": "local_marian_failed",
                "backendDetail": "invalid_json",
            }
        else:
            response = _handle_request(
                str(request.get("text", "")),
                str(request.get("source_language", "")),
                str(request.get("target_language", "")),
            )
        sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
        sys.stdout.flush()
    return 0


def main():
    _configure_stdio()
    os.environ.setdefault("PYTHONUTF8", "1")
    os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")
    os.environ.setdefault("TRANSFORMERS_VERBOSITY", "error")
    os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")

    if len(sys.argv) >= 2 and sys.argv[1] == "--serve":
        return _serve()

    if len(sys.argv) < 2:
        return _result(False, error="missing text")

    text = sys.argv[1]
    source_language = sys.argv[2].strip() if len(sys.argv) > 2 else ""
    target_language = sys.argv[3].strip() if len(sys.argv) > 3 else "zh"
    payload = _handle_request(text, source_language, target_language)
    success = bool(payload.pop("success", False))
    return _result(success, **payload)


if __name__ == "__main__":
    raise SystemExit(main())
