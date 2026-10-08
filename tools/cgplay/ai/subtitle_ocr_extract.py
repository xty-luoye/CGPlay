#!/usr/bin/env python3
import argparse
import base64
import io
import json
import os
import re
from concurrent.futures import ThreadPoolExecutor, as_completed
from difflib import SequenceMatcher
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

from subtitle_region_detector import (
    DETECTOR_VERSION,
    build_scan_result,
    crop_rect,
    ffmpeg_crop_filter,
    preprocess_filter,
    scan_cache_path,
    load_scan_result,
    text_temporal_policy,
    write_scan_result,
)


def _result(success, **kwargs):
    payload = {"success": success}
    payload.update(kwargs)
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if success else 1


def _write_progress(path, payload):
    if not path:
        return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    temporary = f"{path}.tmp.{os.getpid()}"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
    os.replace(temporary, path)


def _timestamp(seconds, vtt=True):
    ms = max(0, int(round(seconds * 1000)))
    h = ms // 3600000
    m = (ms // 60000) % 60
    s = (ms // 1000) % 60
    r = ms % 1000
    sep = "." if vtt else ","
    return f"{h:02d}:{m:02d}:{s:02d}{sep}{r:03d}"


def _normalize_text(text):
    return " ".join(str(text or "").replace("\r", "\n").split()).strip()


def _clean_ocr_text(text):
    value = _normalize_text(text)
    if not value:
        return ""
    value = value.replace("，", "，").replace("。", "。")
    value = re.sub(r"[\u200b\u200c\u200d\ufeff]", "", value)
    value = re.sub(r"\s+", " ", value).strip()
    # Drop one- or two-letter OCR specks before/after a mostly CJK subtitle line.
    if re.search(r"[\u4e00-\u9fff]", value):
        value = re.sub(r"^(?:[A-Za-z]{1,2}|[\\/|_.:;~-])\s+", "", value).strip()
        value = re.sub(r"\s+(?:[A-Za-z]{1,2}|[\\/|_.:;~-])$", "", value).strip()
    value = re.sub(r"([，。！？,.!?])\1{1,}", r"\1", value)
    value = re.sub(r"([。！？!?])\s*([。！？!?])+", r"\1", value)
    value = re.sub(r"\.{4,}", "...", value)
    value = re.sub(r"\s+([，。！？,.!?])", r"\1", value)
    value = re.sub(r"([（(])\s+", r"\1", value)
    value = re.sub(r"\s+([）)])", r"\1", value)
    value = _strip_leading_isolated_han_ocr_noise(value)
    return value.strip()


def _strip_leading_isolated_han_ocr_noise(value):
    text = str(value or "").strip()
    match = re.match(r"^([\u4e00-\u9fff])\s+([\u4e00-\u9fff].*)$", text)
    if not match:
        return text
    prefix = match.group(1)
    rest = match.group(2).strip()
    rest_han = sum(1 for ch in rest if "\u4e00" <= ch <= "\u9fff")
    allowed = set("啊嗯呃哦喂是不我你他她它这那好来去别看听说走住快")
    if rest_han >= 2 and prefix not in allowed:
        return rest
    return text


def _clean_subtitle_region_candidate(text):
    value = _clean_ocr_text(text)
    rejected = []
    if not value:
        return "", rejected
    watermark_pattern = re.compile(
        r"(?:©|\(c\)|copyright)?\s*solo\s+leveling\s+animation\s+partners",
        re.IGNORECASE,
    )
    stripped_value = watermark_pattern.sub("", value)
    stripped_value = re.sub(r"\s+", " ", stripped_value).strip(" -–—|/\\")
    if stripped_value != value:
        rejected.append({
            "text": value,
            "rejectionReason": "copyright-watermark-removed",
        })
        value = stripped_value
    if not value:
        return "", rejected

    han_total = sum(1 for ch in value if "\u4e00" <= ch <= "\u9fff")
    latin_total = sum(1 for ch in value if ("A" <= ch <= "Z") or ("a" <= ch <= "z"))
    alpha_words = re.findall(r"[A-Za-z][A-Za-z'’.-]*", value)
    if han_total == 0 and latin_total >= 3 and len(alpha_words) >= 2:
        noise_chars = sum(1 for ch in value if ch in "\\/|_~{}[]<>")
        allowed_chars = sum(
            1
            for ch in value
            if ch.isalnum() or ch.isspace() or ch in "'’.,!?;:-–—\"()"
        )
        symbol_ratio = 1.0 - (allowed_chars / max(1, len(value)))
        odd_single_letters = [
            word for word in re.findall(r"\b[A-Za-z]\b", value)
            if word.lower() not in {"a", "i"}
        ]
        has_non_english_q = re.search(r"\b\w*q(?!u)\w*\b", value, re.IGNORECASE) is not None
        if (
            noise_chars <= 2 and
            symbol_ratio <= 0.18 and
            not odd_single_letters and
            not has_non_english_q
        ):
            return value.strip(), rejected
        rejected.append({
            "text": value,
            "rejectionReason": "latin-symbol-noise",
        })
        return "", rejected

    kept = []
    for token in value.split():
        han = sum(1 for ch in token if "\u4e00" <= ch <= "\u9fff")
        latin = sum(1 for ch in token if ("A" <= ch <= "Z") or ("a" <= ch <= "z"))
        digit = sum(1 for ch in token if "0" <= ch <= "9")
        symbol = sum(1 for ch in token if not ch.isspace() and not ("\u4e00" <= ch <= "\u9fff") and not ch.isalnum())
        no_han_noise = han == 0 and (latin + digit + symbol) > 0
        mixed_garbage = han > 0 and (latin + digit + symbol) > max(2, han) and (latin + digit) > 2
        if no_han_noise or mixed_garbage:
            rejected.append({
                "text": token,
                "rejectionReason": "latin-symbol-noise" if no_han_noise else "candidate-merged-with-garbage",
            })
            continue
        kept.append(token)
    cleaned = "\n".join(kept).strip()
    if not any("\u4e00" <= ch <= "\u9fff" for ch in cleaned):
        rejected.append({
            "text": value,
            "rejectionReason": "background-text-outside-subtitle-roi-or-no-cjk-subtitle",
        })
        cleaned = ""
    if cleaned and rejected:
        rejected.append({
            "text": value,
            "rejectionReason": "candidate-merged-with-garbage",
        })
    return cleaned, rejected


def _text_similarity(a, b):
    a = _clean_ocr_text(a)
    b = _clean_ocr_text(b)
    if not a or not b:
        return 0.0
    if a == b:
        return 1.0
    if a in b or b in a:
        return min(len(a), len(b)) / max(1, max(len(a), len(b)))
    return SequenceMatcher(None, a, b).ratio()


def _text_quality_score(text):
    value = _clean_ocr_text(text)
    han = sum(1 for ch in value if "\u4e00" <= ch <= "\u9fff")
    latin = sum(1 for ch in value if "A" <= ch <= "z")
    noise = sum(value.count(ch) for ch in "\\/|_~")
    return han * 3 - latin - noise * 2 - abs(len(value) - 18) * 0.05


def _choose_vote_text(variants):
    if not variants:
        return ""
    best_text = ""
    best_score = -10**9
    for text, count in variants.items():
        score = count * 10 + _text_quality_score(text)
        if score > best_score:
            best_score = score
            best_text = text
    return best_text


def _finalize_active(active):
    if not active:
        return None
    text = _choose_vote_text(active.get("variants", {}))
    if not text:
        return None
    source_kinds = active.get("sourceKinds", {})
    source_kind = max(source_kinds, key=source_kinds.get) if source_kinds else "unknown"
    return {
        "start": active["start"],
        "end": active["end"],
        "text": text,
        "sourceKind": source_kind,
        "sampleCount": active.get("sampleCount", 0),
        "variantCount": len(active.get("variants", {})),
    }


_MOJIBAKE_MARKERS = (
    "闂鎴鍙鐨鍦鍏鈮锛铇绗濂瑭鐪閭榄鍊浜涓嶆槸"
    "骞绋妤琛嫊剾楹挡伅搳鎺夐洔鐒℃硶"
)


def _mojibake_score(text):
    value = str(text or "")
    if not value:
        return 999.0
    marker_count = sum(value.count(ch) for ch in _MOJIBAKE_MARKERS)
    replacement_count = value.count("\ufffd") + value.count("?")
    han_count = sum(1 for ch in value if "\u4e00" <= ch <= "\u9fff")
    kana_count = sum(1 for ch in value if "\u3040" <= ch <= "\u30ff")
    return marker_count * 4.0 + replacement_count * 6.0 - han_count * 0.02 - kana_count * 0.5


def _repair_mojibake(text):
    original = _normalize_text(text)
    best = original
    best_score = _mojibake_score(best)
    best_encoding = ""
    for encoding in ("gbk", "cp936", "big5", "big5hkscs"):
        try:
            candidate = _normalize_text(original.encode(encoding).decode("utf-8"))
        except Exception:
            continue
        score = _mojibake_score(candidate)
        if score + 1.0 < best_score:
            best = candidate
            best_score = score
            best_encoding = encoding
    return {
        "text": best,
        "fixed": best != original,
        "encoding": best_encoding,
        "rawScore": _mojibake_score(original),
        "score": best_score,
        "markerCount": sum(best.count(ch) for ch in _MOJIBAKE_MARKERS),
        "replacementCount": best.count("\ufffd") + best.count("?"),
    }


def _write_vtt(path, cues):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("WEBVTT\n\n")
        for cue in cues:
            handle.write(f"{_timestamp(cue['start'])} --> {_timestamp(cue['end'])}\n{cue['text']}\n\n")


def _run_ocr(helper, python_exe, image_path, language):
    proc = subprocess.run(
        [python_exe, helper, image_path, language],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=45,
    )
    try:
        payload = json.loads(proc.stdout or "{}")
    except Exception:
        payload = {"success": False, "error": proc.stderr.strip() or "invalid OCR response"}
    return payload


def _chat_endpoint(base_url):
    value = (base_url or "").rstrip("/")
    if not value:
        return ""
    if value.endswith("/chat/completions"):
        return value
    if value.endswith("/v1"):
        return value + "/chat/completions"
    return value + "/v1/chat/completions"


def _responses_endpoint(base_url):
    value = (base_url or "").rstrip("/")
    if not value:
        return ""
    if value.endswith("/responses"):
        return value
    if value.endswith("/v1"):
        return value + "/responses"
    return value + "/v1/responses"


def _json_from_text(text):
    value = (text or "").strip()
    if value.startswith("```"):
        value = value.strip("`")
        if value.lower().startswith("json"):
            value = value[4:].strip()
    start = value.find("{")
    end = value.rfind("}")
    if start >= 0 and end > start:
        value = value[start:end + 1]
    return json.loads(value)


def _chat_content(data):
    choices = data.get("choices") or []
    if choices:
        message = choices[0].get("message") or {}
        content = message.get("content", "")
        if isinstance(content, list):
            return "\n".join(part.get("text", "") for part in content if isinstance(part, dict))
        return str(content or "")
    direct = str(data.get("output_text") or data.get("text") or "")
    if direct:
        return direct
    parts = []
    for output in data.get("output") or []:
        if not isinstance(output, dict):
            continue
        for content in output.get("content") or []:
            if isinstance(content, dict) and content.get("type") in {"output_text", "text"}:
                parts.append(str(content.get("text", "") or ""))
    return "\n".join(part for part in parts if part)


def _request_workbench_vision(prompt, image_b64, base_url, api_key, model, timeout):
    attempts = [
        (
            "workbench_vision_chat",
            _chat_endpoint(base_url),
            {
                "model": model or "gpt-4o-mini",
                "messages": [{
                    "role": "user",
                    "content": [
                        {"type": "text", "text": prompt},
                        {"type": "image_url", "image_url": {"url": "data:image/png;base64," + image_b64}},
                    ],
                }],
                "temperature": 0,
            },
        ),
        (
            "workbench_vision_responses",
            _responses_endpoint(base_url),
            {
                "model": model or "gpt-4o-mini",
                "input": [{
                    "role": "user",
                    "content": [
                        {"type": "input_text", "text": prompt},
                        {"type": "input_image", "image_url": "data:image/png;base64," + image_b64},
                    ],
                }],
                "max_output_tokens": 3000,
            },
        ),
    ]
    errors = []
    for backend, endpoint, payload in attempts:
        if not endpoint:
            continue
        req = urllib.request.Request(
            endpoint,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                status = getattr(resp, "status", 200)
                body = resp.read().decode("utf-8", errors="replace")
            data = json.loads(body)
            content = _chat_content(data)
            if content.strip():
                return {
                    "success": True,
                    "content": content,
                    "backend": backend,
                    "httpStatus": status,
                }
            errors.append(f"{backend}: empty response")
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace") if exc.fp else ""
            errors.append(f"{backend}: HTTP {exc.code}: {body[:300] or str(exc)}")
        except Exception as exc:
            errors.append(f"{backend}: {exc}")
    return {
        "success": False,
        "content": "",
        "backend": "workbench_vision_failed",
        "error": " | ".join(errors)[-1200:],
    }


def _run_workbench_vision(image_path, base_url, api_key, model, timeout):
    if not base_url or not api_key:
        return {
            "success": False,
            "text": "",
            "backend": "workbench_vision",
            "error": "workbench-vision-api-key-or-base-url-missing",
        }
    with open(image_path, "rb") as handle:
        image_b64 = base64.b64encode(handle.read()).decode("ascii")
    prompt = (
        "Read only the text that is visibly rendered as subtitles inside this cropped subtitle region. "
        "Be literal: transcribe exactly what is visible, preserving wording and line order. "
        "Do not explain, infer, complete missing words, add context, translate, rewrite, or merge nearby dialogue. "
        "Ignore watermarks, credits, UI, signs, and background text unless the text is inside the subtitle candidate area "
        "and behaves like timed subtitles. Return JSON only: {\"text\":\"...\", \"confidence\":0-1}. "
        "If no subtitle is visible, return {\"text\":\"\", \"confidence\":0}."
    )
    response = _request_workbench_vision(prompt, image_b64, base_url, api_key, model, timeout)
    try:
        if not response.get("success"):
            raise RuntimeError(response.get("error", "workbench vision failed"))
        parsed = _json_from_text(response.get("content", ""))
        return {
            "success": True,
            "text": str(parsed.get("text", "")).strip(),
            "confidence": parsed.get("confidence", 0),
            "backend": response.get("backend", "workbench_vision"),
            "httpStatus": response.get("httpStatus", 200),
        }
    except Exception as exc:
        return {
            "success": False,
            "text": "",
            "backend": "workbench_vision",
            "error": str(exc),
        }


def _run_workbench_vision_batch(image_paths, base_url, api_key, model, timeout):
    if not base_url or not api_key:
        return {
            "success": False,
            "items": [],
            "backend": "workbench_vision_batch",
            "error": "workbench-vision-api-key-or-base-url-missing",
        }
    try:
        from PIL import Image, ImageDraw

        loaded = []
        for path in image_paths:
            with Image.open(path) as image:
                loaded.append(image.convert("RGB"))
        dense_sheet = len(loaded) > 16
        cell_width = 360 if dense_sheet else 480
        cell_image_height = 96 if dense_sheet else 128
        label_height = 24
        columns = 6 if dense_sheet else 4
        rows = max(1, (len(loaded) + columns - 1) // columns)
        sheet = Image.new("RGB", (cell_width * columns, (cell_image_height + label_height) * rows), "black")
        draw = ImageDraw.Draw(sheet)
        for index, image in enumerate(loaded):
            image.thumbnail((cell_width, cell_image_height))
            x = (index % columns) * cell_width
            y = (index // columns) * (cell_image_height + label_height)
            px = x + (cell_width - image.width) // 2
            py = y + label_height + (cell_image_height - image.height) // 2
            sheet.paste(image, (px, py))
            draw.rectangle((x, y, x + 58, y + label_height), fill="black")
            draw.text((x + 5, y + 4), f"S{index:02d}", fill="white")
        buffer = io.BytesIO()
        sheet.save(buffer, format="PNG", optimize=True)
        image_b64 = base64.b64encode(buffer.getvalue()).decode("ascii")
    except Exception as exc:
        return {
            "success": False,
            "items": [],
            "backend": "workbench_vision_batch",
            "error": f"contact-sheet-failed: {exc}",
        }

    prompt = (
        "This contact sheet contains cropped frames from the same fixed bottom subtitle region. "
        "Each panel is labeled S00, S01, and so on. Read only timed dialogue subtitles visible in each panel. "
        "Ignore faces, scenery, signs, watermarks, credits, and UI. Transcribe literally; do not translate, infer, "
        "rewrite, merge panels, or add context. Return JSON only: "
        "{\"items\":[{\"slot\":0,\"text\":\"...\",\"confidence\":0.0}]}. "
        "Return one item for every slot in order. Use empty text and confidence 0 when no subtitle is visible."
    )
    response = _request_workbench_vision(prompt, image_b64, base_url, api_key, model, timeout)
    try:
        if not response.get("success"):
            raise RuntimeError(response.get("error", "workbench vision batch failed"))
        parsed = _json_from_text(response.get("content", ""))
        raw_items = parsed.get("items") or []
        items = []
        by_slot = {}
        for item in raw_items:
            if not isinstance(item, dict):
                continue
            slot = int(item.get("slot", -1))
            if 0 <= slot < len(image_paths):
                by_slot[slot] = {
                    "slot": slot,
                    "text": str(item.get("text", "") or "").strip(),
                    "confidence": float(item.get("confidence", 0) or 0),
                }
        for slot in range(len(image_paths)):
            items.append(by_slot.get(slot, {"slot": slot, "text": "", "confidence": 0.0}))
        return {
            "success": len(items) == len(image_paths),
            "items": items,
            "backend": response.get("backend", "workbench_vision_batch"),
            "httpStatus": response.get("httpStatus", 200),
        }
    except Exception as exc:
        return {
            "success": False,
            "items": [],
            "backend": "workbench_vision_batch",
            "error": str(exc),
        }


def _toml_string(value):
    return json.dumps(str(value or ""), ensure_ascii=False)


def _prepare_codex_vision_home(codex_home, base_url, model):
    home = os.path.abspath(codex_home or os.path.join(
        os.environ.get("LOCALAPPDATA", tempfile.gettempdir()),
        "CGPlay", "CGPlay", "subtitle-codex-home",
    ))
    os.makedirs(home, exist_ok=True)
    config_path = os.path.join(home, "config.toml")
    config = (
        f"model = {_toml_string(model)}\n"
        'model_provider = "cgplay-subtitle-vision"\n\n'
        '[model_providers.cgplay-subtitle-vision]\n'
        'name = "CGPlay subtitle vision"\n'
        f"base_url = {_toml_string(str(base_url or '').rstrip('/'))}\n"
        'wire_api = "responses"\n'
        'env_key = "CGPLAY_AI_WORKSPACE_API_KEY"\n'
        'requires_openai_auth = false\n'
    )
    temporary_path = f"{config_path}.{os.getpid()}.{threading.get_ident()}.tmp"
    with open(temporary_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(config)
    os.replace(temporary_path, config_path)
    return home


def _run_codex_cli_vision_batch(
    image_paths, base_url, api_key, model, timeout, codex_executable, codex_home,
):
    if not codex_executable or not os.path.isfile(codex_executable):
        return {
            "success": False,
            "items": [],
            "backend": "codex_cli_vision_batch",
            "error": "codex-cli-executable-missing",
        }
    if not base_url or not api_key or not model:
        return {
            "success": False,
            "items": [],
            "backend": "codex_cli_vision_batch",
            "error": "codex-cli-provider-config-missing",
        }
    try:
        from PIL import Image, ImageDraw

        loaded = []
        for path in image_paths:
            with Image.open(path) as image:
                loaded.append(image.convert("RGB"))
        cell_width = 480
        cell_image_height = 128
        label_height = 24
        columns = 4
        rows = max(1, (len(loaded) + columns - 1) // columns)
        sheet = Image.new("RGB", (cell_width * columns, (cell_image_height + label_height) * rows), "black")
        draw = ImageDraw.Draw(sheet)
        for index, image in enumerate(loaded):
            image.thumbnail((cell_width, cell_image_height))
            x = (index % columns) * cell_width
            y = (index // columns) * (cell_image_height + label_height)
            px = x + (cell_width - image.width) // 2
            py = y + label_height + (cell_image_height - image.height) // 2
            sheet.paste(image, (px, py))
            draw.rectangle((x, y, x + 58, y + label_height), fill="black")
            draw.text((x + 5, y + 4), f"S{index:02d}", fill="white")
        work_dir = tempfile.mkdtemp(prefix="cgplay_codex_vision_")
        sheet_path = os.path.join(work_dir, "contact_sheet.png")
        output_path = os.path.join(work_dir, "result.txt")
        sheet.save(sheet_path, format="PNG", optimize=True)
        home = _prepare_codex_vision_home(codex_home, base_url, model)
        prompt = (
            "Read the attached contact sheet only. It contains cropped frames from one fixed bottom subtitle region. "
            "Panels are labeled S00, S01, and so on. Read only timed dialogue subtitles visible in each panel. "
            "Ignore faces, scenery, signs, watermarks, credits, and UI. Transcribe literally; do not translate, infer, "
            "rewrite, merge panels, use tools, inspect files, or add context. Return JSON only: "
            '{"items":[{"slot":0,"text":"...","confidence":0.0}]}. '
            "Return one item for every slot in order. Use empty text and confidence 0 when no subtitle is visible."
        )
        command = [
            codex_executable, "exec", "--ephemeral", "--skip-git-repo-check", "--ignore-rules",
            "--color", "never", "-s", "read-only", "-m", model,
            "-i", sheet_path, "-o", output_path, prompt,
        ]
        env = os.environ.copy()
        env["CODEX_HOME"] = home
        env["CGPLAY_AI_WORKSPACE_API_KEY"] = api_key
        started = time.perf_counter()
        proc = subprocess.run(
            command,
            cwd=work_dir,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=max(15.0, float(timeout)),
        )
        elapsed = time.perf_counter() - started
        content = ""
        if os.path.isfile(output_path):
            with open(output_path, "r", encoding="utf-8", errors="replace") as handle:
                content = handle.read()
        if proc.returncode != 0:
            raise RuntimeError((proc.stderr or proc.stdout or f"exit={proc.returncode}")[-1200:])
        parsed = _json_from_text(content or proc.stdout)
        by_slot = {}
        for item in parsed.get("items") or []:
            if not isinstance(item, dict):
                continue
            slot = int(item.get("slot", -1))
            if 0 <= slot < len(image_paths):
                by_slot[slot] = {
                    "slot": slot,
                    "text": str(item.get("text", "") or "").strip(),
                    "confidence": float(item.get("confidence", 0) or 0),
                }
        items = [by_slot.get(slot, {"slot": slot, "text": "", "confidence": 0.0})
                 for slot in range(len(image_paths))]
        return {
            "success": len(by_slot) == len(image_paths),
            "items": items,
            "backend": "codex_cli_vision_batch",
            "elapsedSeconds": round(elapsed, 3),
            "firstResponseSeconds": round(elapsed, 3),
        }
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "items": [],
            "backend": "codex_cli_vision_batch",
            "error": f"codex-cli-timeout-{max(15.0, float(timeout)):.0f}s",
        }
    except Exception as exc:
        return {
            "success": False,
            "items": [],
            "backend": "codex_cli_vision_batch",
            "error": str(exc)[-1200:],
        }
    finally:
        if "work_dir" in locals():
            try:
                import shutil
                shutil.rmtree(work_dir, ignore_errors=True)
            except Exception:
                pass


class _OcrServer:
    def __init__(self, helper, python_exe, language):
        self.helper = helper
        self.python_exe = python_exe
        self.language = language
        self.proc = None
        self.start_error = ""

    def __enter__(self):
        return self

    def _ensure_started(self):
        if self.proc is not None:
            return
        try:
            self.proc = subprocess.Popen(
                [self.python_exe, self.helper, "--serve"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except Exception as exc:
            self.start_error = str(exc)
            self.proc = None

    def __exit__(self, exc_type, exc, tb):
        if not self.proc:
            return
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass

    def run(self, image_path):
        self._ensure_started()
        if not self.proc or not self.proc.stdin or not self.proc.stdout or self.proc.poll() is not None:
            payload = _run_ocr(self.helper, self.python_exe, image_path, self.language)
            if self.start_error:
                payload.setdefault("backendDetail", f"server_start_failed={self.start_error}")
            return payload
        request = {"image_path": image_path, "language_hint": self.language}
        try:
            self.proc.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("empty OCR server response")
            return json.loads(line)
        except Exception as exc:
            return {
                "success": False,
                "error": f"OCR server request failed: {exc}",
                "backend": "local_subtitle_ocr_server_failed",
            }


def _extract_frame(ffmpeg, media, time_seconds, output, crop, preprocess):
    vf = preprocess_filter(ffmpeg_crop_filter("bottom-subtitle-band", crop), preprocess)
    proc = subprocess.run(
        [ffmpeg, "-y", "-ss", f"{time_seconds:.3f}", "-i", media, "-frames:v", "1", "-vf", vf, output],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    return proc.returncode == 0 and os.path.exists(output)


def _extract_frames_batch(ffmpeg, media, start, duration, output_pattern, crop, preprocess, interval, max_samples):
    vf = preprocess_filter(ffmpeg_crop_filter("bottom-subtitle-band", crop), preprocess)
    vf = f"{vf},fps=1/{max(0.1, interval):.6f}"
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
            "-vf",
            vf,
            "-frames:v",
            str(max_samples),
            "-vsync",
            "0",
            output_pattern,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=max(90, int(duration) + 60),
    )
    output_dir = os.path.dirname(os.path.abspath(output_pattern))
    frames = sorted(
        os.path.join(output_dir, name)
        for name in os.listdir(output_dir)
        if name.startswith("sample_") and name.lower().endswith(".png")
    )
    return proc.returncode == 0 and bool(frames), frames, proc.stderr.strip()[-1000:]


def main():
    parser = argparse.ArgumentParser(description="Sparse background OCR subtitle extraction for CGPlay.")
    parser.add_argument("--media", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-vtt", required=True)
    parser.add_argument("--language", default="zh")
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--start", type=float, default=0.0)
    parser.add_argument("--sample-interval", type=float, default=0.5)
    parser.add_argument("--max-samples", type=int, default=240)
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--helper", default=os.path.join(os.path.dirname(__file__), "local_subtitle_ocr.py"))
    parser.add_argument("--crop-height", default="", help="Pixel crop height from bottom. Empty means bottom 32%%.")
    parser.add_argument("--no-preprocess", action="store_true")
    parser.add_argument("--disabled", action="store_true")
    parser.add_argument("--vision-provider", default="local-ocr", choices=["local-ocr", "workbench", "codex-cli"])
    parser.add_argument("--vision-base-url", default=os.environ.get("SUBTITLE_WORKBENCH_BASE_URL", os.environ.get("SUBTITLE_TRANSLATION_BASE_URL", "")))
    parser.add_argument("--vision-api-key", default=os.environ.get("SUBTITLE_WORKBENCH_API_KEY", os.environ.get("SUBTITLE_TRANSLATION_API_KEY", "")))
    parser.add_argument("--vision-model", default=os.environ.get("SUBTITLE_WORKBENCH_VISION_MODEL", os.environ.get("SUBTITLE_WORKBENCH_MODEL", os.environ.get("SUBTITLE_TRANSLATION_MODEL", "gpt-4o-mini"))))
    parser.add_argument("--vision-timeout", type=float, default=45.0)
    parser.add_argument("--vision-fallback-local-ocr", action="store_true", default=True)
    parser.add_argument("--vision-stop-after-failures", type=int, default=3)
    parser.add_argument("--vision-call-mode", default="batch", choices=["batch", "per-cue", "per-frame"])
    parser.add_argument("--codex-executable", default=os.environ.get("CGPLAY_CODEX_EXECUTABLE", ""))
    parser.add_argument("--codex-home", default=os.environ.get("CGPLAY_SUBTITLE_CODEX_HOME", ""))
    parser.add_argument("--candidate-scan-json", default="", help="Optional media-fingerprint scoped candidate cue scan cache path.")
    args = parser.parse_args()

    if args.disabled:
        return _result(False, ocrEnabled=False, ocrError="OCR disabled")
    if not os.path.exists(args.media):
        return _result(False, ocrEnabled=True, ocrError="media file does not exist")
    candidate_scan_path = Path(args.candidate_scan_json).resolve() if args.candidate_scan_json.strip() else scan_cache_path(
        Path(args.media),
        Path(args.output_vtt).resolve().parent,
    )
    cached_scan = load_scan_result(candidate_scan_path, Path(args.media))
    cached_candidates = cached_scan.get("candidateCues", []) if cached_scan else []
    cached_scan_start = float(cached_scan.get("scanStartSeconds", -1.0)) if cached_scan else -1.0
    cached_scan_end = float(cached_scan.get("scanEndSeconds", -1.0)) if cached_scan else -1.0
    requested_scan_end = float(args.start) + float(args.duration)
    cached_scan_covers_request = (
        cached_scan_start >= 0.0 and
        cached_scan_end > cached_scan_start and
        cached_scan_start <= float(args.start) + 0.5 and
        cached_scan_end + 0.5 >= requested_scan_end
    )
    vision_enabled = args.vision_provider in {"workbench", "codex-cli"}
    cached_vision_authoritative = bool(cached_candidates) and all(
        str(item.get("sourceKind", "")) == "workbench-vision"
        for item in cached_candidates
    )
    if cached_scan and cached_candidates and cached_scan_covers_request and (
        not vision_enabled or cached_vision_authoritative
    ):
        cached_cues = []
        for item in cached_scan.get("candidateCues", []):
            text = _normalize_text(item.get("text", ""))
            if not text:
                continue
            cached_cues.append({
                "start": float(item.get("start", 0.0)),
                "end": float(item.get("end", item.get("start", 0.0))),
                "text": text,
                "sourceKind": item.get("sourceKind", "subtitle-region-candidate-cache"),
                "confidence": item.get("confidence", 0.82),
                "cropRect": item.get("cropRect", crop_rect("bottom-subtitle-band", args.crop_height.strip())),
                "sampleFrame": item.get("sampleFrame", {}),
                "detectorVersion": cached_scan.get("detectorVersion", DETECTOR_VERSION),
                "candidateRegion": "subtitle-region-cue",
                "reusedCandidateScanCache": True,
            })
        if cached_cues:
            _write_vtt(args.output_vtt, cached_cues)
            os.makedirs(os.path.dirname(os.path.abspath(args.output_json)), exist_ok=True)
            cached_payload = {
                "success": True,
                "reusedCandidateScanCache": True,
                "sharedRuntimeHeadlessDetector": True,
                "detectorVersion": cached_scan.get("detectorVersion", DETECTOR_VERSION),
                "candidateScanCachePath": str(candidate_scan_path),
                "candidateScan": cached_scan,
                "visualSubtitleTrackGenerated": True,
                "visualCueCount": len(cached_cues),
                "visualCoverageStart": cached_cues[0]["start"],
                "visualCoverageEnd": cached_cues[-1]["end"],
                "visualDetectedFrameCount": len(cached_cues),
                "visualMissedFrameCount": 0,
                "workbenchCanReadImageText": vision_enabled,
                "visionApiAttempted": False,
                "visionApiSucceeded": False,
                "visionApiFailed": False,
                "visionFrameCount": 0,
                "visionCueCount": 0,
                "localOcrFallbackFrameCount": 0,
                "visionSourceKind": "candidate-scan-cache",
                "ocrEnabled": True,
                "ocrProvider": "candidate_scan_cache",
                "ocrSamples": 0,
                "ocrAcceptedCueCount": len(cached_cues),
                "ocrRejectedCueCount": 0,
                "cues": cached_cues,
                "samples": [],
            }
            with open(args.output_json, "w", encoding="utf-8") as handle:
                json.dump(cached_payload, handle, ensure_ascii=False, indent=2)
            return _result(
                True,
                reusedCandidateScanCache=True,
                sharedRuntimeHeadlessDetector=True,
                detectorVersion=cached_payload["detectorVersion"],
                candidateScanCachePath=str(candidate_scan_path),
                visualSubtitleTrackGenerated=True,
                visualCueCount=len(cached_cues),
                outputJson=args.output_json,
                outputVtt=args.output_vtt,
            )
    if not os.path.exists(args.helper):
        return _result(False, ocrEnabled=True, ocrError="local_subtitle_ocr.py helper is missing")

    samples = []
    accepted = 0
    rejected = 0
    vision_attempted = 0
    vision_succeeded = 0
    vision_failed = 0
    local_fallback_count = 0
    first_vision_error = ""
    consecutive_vision_failures = 0
    vision_stopped_after_failures = False
    mojibake_fixed = 0
    mojibake_marker_total = 0
    cues = []
    active = None
    started_at = time.time()
    progress_base = {
        "success": False,
        "status": "starting",
        "mediaPath": os.path.abspath(args.media),
        "scanStartSeconds": round(float(args.start or 0.0), 3),
        "targetDurationSeconds": round(float(args.duration or 0.0), 3),
        "processedCoverageSeconds": 0.0,
        "processedThroughSeconds": round(float(args.start or 0.0), 3),
        "completedSamples": 0,
        "totalSamples": 0,
    }
    _write_progress(args.output_json, progress_base)
    with tempfile.TemporaryDirectory(prefix="cgplay_ocr_extract_") as tmp:
        count = max(1, min(args.max_samples, int(args.duration / max(0.1, args.sample_interval)) + 1))
        progress_base["status"] = "extracting-frames"
        progress_base["totalSamples"] = count
        _write_progress(args.output_json, progress_base)
        output_pattern = os.path.join(tmp, "sample_%05d.png")
        batch_ok = False
        batch_frames = []
        batch_error = ""
        try:
            batch_ok, batch_frames, batch_error = _extract_frames_batch(
                args.ffmpeg,
                args.media,
                args.start,
                args.duration,
                output_pattern,
                args.crop_height.strip(),
                not args.no_preprocess,
                args.sample_interval,
                count,
            )
        except Exception as exc:
            batch_error = str(exc)
        progress_base["status"] = "recognizing-text"
        progress_base["batchFrameExtraction"] = bool(batch_ok)
        progress_base["batchFrameCount"] = len(batch_frames)
        progress_base["batchFrameError"] = batch_error
        _write_progress(args.output_json, progress_base)
        batched_vision_payloads = {}
        vision_batch_failures = 0
        first_vision_batch_error = ""
        vision_batch_metrics = []
        if vision_enabled and args.vision_call_mode == "batch" and batch_ok:
            default_batch_size = "32" if args.vision_provider == "codex-cli" else "12"
            maximum_batch_size = 32 if args.vision_provider == "codex-cli" else 16
            batch_size = max(4, min(maximum_batch_size, int(os.environ.get("SUBTITLE_WORKBENCH_VISION_BATCH_SIZE", default_batch_size))))
            default_concurrency = "4"
            concurrency = max(1, min(8, int(os.environ.get("SUBTITLE_WORKBENCH_VISION_CONCURRENCY", default_concurrency))))
            groups = []
            for start_index in range(0, len(batch_frames), batch_size):
                groups.append((start_index, batch_frames[start_index:start_index + batch_size]))
            progress_base["status"] = "ai-vision"
            progress_base["visionBatchCount"] = len(groups)
            progress_base["visionBatchSize"] = batch_size
            progress_base["visionConcurrency"] = concurrency
            _write_progress(args.output_json, progress_base)
            with ThreadPoolExecutor(max_workers=concurrency) as executor:
                futures = {}
                for start_index, paths in groups:
                    if args.vision_provider == "codex-cli":
                        future = executor.submit(
                            _run_codex_cli_vision_batch,
                            paths,
                            args.vision_base_url,
                            args.vision_api_key,
                            args.vision_model,
                            args.vision_timeout,
                            args.codex_executable,
                            args.codex_home,
                        )
                    else:
                        future = executor.submit(
                            _run_workbench_vision_batch,
                            paths,
                            args.vision_base_url,
                            args.vision_api_key,
                            args.vision_model,
                            args.vision_timeout,
                        )
                    futures[future] = (start_index, paths)
                completed_batches = 0
                for future in as_completed(futures):
                    start_index, paths = futures[future]
                    try:
                        result = future.result()
                    except Exception as exc:
                        result = {"success": False, "items": [], "error": str(exc)}
                    if not result.get("success") and args.vision_provider == "codex-cli":
                        direct_result = _run_workbench_vision_batch(
                            paths,
                            args.vision_base_url,
                            args.vision_api_key,
                            args.vision_model,
                            args.vision_timeout,
                        )
                        if direct_result.get("success"):
                            direct_result["codexCliError"] = result.get("error", "codex-cli-vision-failed")
                            result = direct_result
                        else:
                            result["directWorkbenchError"] = direct_result.get("error", "workbench-vision-failed")
                    if result.get("success"):
                        vision_batch_metrics.append({
                            "startIndex": start_index,
                            "frameCount": len(paths),
                            "success": True,
                            "backend": result.get("backend", "workbench_vision_batch"),
                            "elapsedSeconds": result.get("elapsedSeconds", 0),
                        })
                        for item in result.get("items", []):
                            slot = int(item.get("slot", -1))
                            if 0 <= slot < len(paths):
                                batched_vision_payloads[start_index + slot] = {
                                    "success": True,
                                    "text": item.get("text", ""),
                                    "confidence": item.get("confidence", 0),
                                    "backend": result.get("backend", "workbench_vision_batch"),
                                    "elapsedSeconds": result.get("elapsedSeconds", 0),
                                }
                    else:
                        vision_batch_metrics.append({
                            "startIndex": start_index,
                            "frameCount": len(paths),
                            "success": False,
                            "backend": result.get("backend", "vision_batch_failed"),
                            "elapsedSeconds": result.get("elapsedSeconds", 0),
                            "error": result.get("error", "vision-batch-failed"),
                        })
                        vision_batch_failures += 1
                        if not first_vision_batch_error:
                            first_vision_batch_error = str(result.get("error", "vision-batch-failed"))
                        for slot in range(len(paths)):
                            batched_vision_payloads[start_index + slot] = {
                                "success": False,
                                "text": "",
                                "confidence": 0,
                                "backend": "workbench_vision_batch_failed",
                                "error": result.get("error", "vision-batch-failed"),
                            }
                    completed_batches += 1
                    completed_frames = min(len(batch_frames), completed_batches * batch_size)
                    processed = min(float(args.duration), completed_frames * float(args.sample_interval))
                    progress_base.update({
                        "status": "ai-vision",
                        "processedCoverageSeconds": round(processed, 3),
                        "processedThroughSeconds": round(float(args.start) + processed, 3),
                        "completedSamples": completed_frames,
                        "completedVisionBatches": completed_batches,
                        "failedVisionBatches": vision_batch_failures,
                        "firstVisionBatchError": first_vision_batch_error,
                    })
                    _write_progress(args.output_json, progress_base)
        with _OcrServer(args.helper, args.python, args.language) as ocr:
            for i in range(count):
                t = args.start + i * args.sample_interval
                image_path = batch_frames[i] if batch_ok and i < len(batch_frames) else os.path.join(tmp, f"fallback_{i:05d}.png")
                if not (batch_ok and i < len(batch_frames)) and not _extract_frame(
                    args.ffmpeg,
                    args.media,
                    t,
                    image_path,
                    args.crop_height.strip(),
                    not args.no_preprocess,
                ):
                    rejected += 1
                    continue
                used_local_fallback = False
                use_batched_vision = (
                    vision_enabled and
                    args.vision_call_mode == "batch" and
                    i in batched_vision_payloads
                )
                use_per_frame_vision = (
                    vision_enabled and
                    args.vision_call_mode == "per-frame" and
                    not vision_stopped_after_failures
                )
                use_direct_vision = use_batched_vision or use_per_frame_vision
                if use_direct_vision:
                    vision_attempted += 1
                    payload = batched_vision_payloads.get(i) if use_batched_vision else _run_workbench_vision(
                        image_path, args.vision_base_url, args.vision_api_key, args.vision_model, args.vision_timeout)
                    if payload.get("success"):
                        vision_succeeded += 1
                        consecutive_vision_failures = 0
                    else:
                        vision_failed += 1
                        consecutive_vision_failures += 1
                        if not first_vision_error:
                            first_vision_error = payload.get("error", "") or payload.get("backendDetail", "")
                        if consecutive_vision_failures >= max(1, args.vision_stop_after_failures):
                            vision_stopped_after_failures = True
                        if args.vision_fallback_local_ocr:
                            local_fallback_count += 1
                            used_local_fallback = True
                            payload = ocr.run(image_path)
                            payload["fallbackReason"] = "workbench-vision-failed/local-ocr-fallback"
                            payload["visionError"] = first_vision_error
                else:
                    if vision_enabled and vision_stopped_after_failures:
                        used_local_fallback = True
                        local_fallback_count += 1
                    payload = ocr.run(image_path)
                    if used_local_fallback:
                        payload["fallbackReason"] = "workbench-vision-failed/local-ocr-fallback"
                        payload["visionError"] = first_vision_error
                raw_text = _normalize_text(payload.get("text", "")) if payload.get("success") else ""
                repaired = _repair_mojibake(raw_text) if raw_text else {
                    "text": "",
                    "fixed": False,
                    "markerCount": 0,
                    "replacementCount": 0,
                    "score": 0,
                    "rawScore": 0,
                    "encoding": "",
                }
                text, rejected_candidates = _clean_subtitle_region_candidate(repaired["text"])
                if repaired["fixed"]:
                    mojibake_fixed += 1
                mojibake_marker_total += int(repaired["markerCount"]) + int(repaired["replacementCount"])
                sample_source_kind = (
                    "workbench-vision"
                    if use_direct_vision and not used_local_fallback and payload.get("success")
                    else "local-ocr-fallback"
                    if used_local_fallback
                    else "local-ocr-prefilter"
                    if args.vision_provider == "workbench" and args.vision_call_mode == "per-cue"
                    else "local-ocr"
                )
                samples.append({
                    "time": t,
                    "success": bool(text),
                    "text": text,
                    "rawText": raw_text if repaired["fixed"] else "",
                    "sourceKind": sample_source_kind,
                    "visionApiAttempted": use_direct_vision,
                    "visionApiSucceeded": vision_enabled and payload.get("success") and not used_local_fallback,
                    "visionApiFailed": vision_enabled and used_local_fallback,
                    "fallbackReason": payload.get("fallbackReason", ""),
                    "candidateRegion": "subtitle-region-cue" if text else "",
                    "detectorVersion": DETECTOR_VERSION,
                    "cropRect": crop_rect("bottom-subtitle-band", args.crop_height.strip()),
                    "sampleFrame": {"timeSeconds": round(t, 3), "kind": "subtitle-region-sparse-sample"},
                    "confidence": payload.get("confidence", 0.82 if text else 0.0),
                    "rejectedCandidates": rejected_candidates,
                    "mojibakeFixed": bool(repaired["fixed"]),
                    "mojibakeEncoding": repaired.get("encoding", ""),
                    "mojibakeMarkerCount": int(repaired["markerCount"]) + int(repaired["replacementCount"]),
                    "backend": payload.get("backend", ""),
                    "backendDetail": payload.get("backendDetail", ""),
                    "visionElapsedSeconds": payload.get("elapsedSeconds", 0),
                })
                if not text:
                    rejected += 1
                    if active:
                        active["end"] = max(active["end"], t)
                        cue = _finalize_active(active)
                        if cue:
                            cues.append(cue)
                        active = None
                    if i == count - 1 or i % 8 == 0:
                        processed = max(
                            float(progress_base.get("processedCoverageSeconds", 0.0) or 0.0),
                            min(float(args.duration), (i + 1) * float(args.sample_interval)),
                        )
                        progress_base.update({
                            "status": "finalizing-ai-vision" if args.vision_call_mode == "batch" else "recognizing-text",
                            "processedCoverageSeconds": round(processed, 3),
                            "processedThroughSeconds": round(float(args.start) + processed, 3),
                            "completedSamples": i + 1,
                            "acceptedSamples": accepted,
                            "rejectedSamples": rejected,
                        })
                        _write_progress(args.output_json, progress_base)
                    continue
                accepted += 1
                if active and t - active["end"] <= args.sample_interval * 2.5 and (
                    _text_similarity(_choose_vote_text(active.get("variants", {})), text) >= 0.72
                ):
                    active["end"] = t + args.sample_interval
                    active["variants"][text] = active["variants"].get(text, 0) + 1
                    active["sourceKinds"][sample_source_kind] = active["sourceKinds"].get(sample_source_kind, 0) + 1
                    active["sampleCount"] += 1
                else:
                    if active:
                        cue = _finalize_active(active)
                        if cue:
                            cues.append(cue)
                    active = {
                        "start": max(args.start, t),
                        "end": t + args.sample_interval,
                        "variants": {text: 1},
                        "sourceKinds": {sample_source_kind: 1},
                        "sampleCount": 1,
                    }
                if i == count - 1 or i % 8 == 0:
                    processed = max(
                        float(progress_base.get("processedCoverageSeconds", 0.0) or 0.0),
                        min(float(args.duration), (i + 1) * float(args.sample_interval)),
                    )
                    progress_base.update({
                        "status": "finalizing-ai-vision" if args.vision_call_mode == "batch" else "recognizing-text",
                        "processedCoverageSeconds": round(processed, 3),
                        "processedThroughSeconds": round(float(args.start) + processed, 3),
                        "completedSamples": i + 1,
                        "acceptedSamples": accepted,
                        "rejectedSamples": rejected,
                    })
                    _write_progress(args.output_json, progress_base)
        if active:
            cue = _finalize_active(active)
            if cue:
                cues.append(cue)

    merged = []
    for cue in cues:
        if not merged:
            merged.append(cue)
            continue
        previous = merged[-1]
        if cue["start"] - previous["end"] <= args.sample_interval * 2.0 and _text_similarity(previous["text"], cue["text"]) >= 0.82:
            previous["end"] = max(previous["end"], cue["end"])
            if _text_quality_score(cue["text"]) > _text_quality_score(previous["text"]):
                previous["text"] = cue["text"]
            previous["sampleCount"] = previous.get("sampleCount", 0) + cue.get("sampleCount", 0)
            previous["variantCount"] = max(previous.get("variantCount", 1), cue.get("variantCount", 1))
        else:
            merged.append(cue)
    cues = [
        cue for cue in merged
        if cue["end"] - cue["start"] >= max(0.4, args.sample_interval) and
        1 <= len(cue["text"]) <= 80 and
        not re.fullmatch(r"[A-Za-z\\/|_.:;~ -]{1,4}", cue["text"]) and
        not re.fullmatch(r"[\d\s.,:;#\\/|_.~()（）-]{1,12}", cue["text"])
    ]
    if args.vision_provider == "workbench" and args.vision_call_mode == "per-cue" and cues:
        with tempfile.TemporaryDirectory(prefix="cgplay_vision_cue_") as vision_tmp:
            for cue_index, cue in enumerate(cues):
                if vision_stopped_after_failures:
                    cue["sourceKind"] = "local-ocr-fallback"
                    cue["ocrRawText"] = cue.get("text", "")
                    cue["visualRawText"] = ""
                    cue["fallbackReason"] = "workbench-vision-failed/local-ocr-fallback"
                    cue["visionApiAttempted"] = False
                    cue["visionApiSucceeded"] = False
                    cue["visionApiFailed"] = True
                    local_fallback_count += 1
                    continue
                representative_time = max(cue["start"], min(cue["end"], (cue["start"] + cue["end"]) * 0.5))
                image_path = os.path.join(vision_tmp, f"cue_{cue_index:04d}.png")
                if not _extract_frame(args.ffmpeg, args.media, representative_time, image_path, args.crop_height.strip(), not args.no_preprocess):
                    cue["sourceKind"] = "local-ocr-fallback"
                    cue["ocrRawText"] = cue.get("text", "")
                    cue["visualRawText"] = ""
                    cue["fallbackReason"] = "workbench-vision-frame-extract-failed/local-ocr-fallback"
                    cue["visionApiAttempted"] = False
                    cue["visionApiSucceeded"] = False
                    cue["visionApiFailed"] = True
                    local_fallback_count += 1
                    continue
                vision_attempted += 1
                payload = _run_workbench_vision(
                    image_path,
                    args.vision_base_url,
                    args.vision_api_key,
                    args.vision_model,
                    args.vision_timeout,
                )
                if payload.get("success"):
                    raw_text = _normalize_text(payload.get("text", ""))
                    repaired = _repair_mojibake(raw_text) if raw_text else {
                        "text": "",
                        "fixed": False,
                        "markerCount": 0,
                        "replacementCount": 0,
                        "score": 0,
                        "rawScore": 0,
                        "encoding": "",
                    }
                    text, rejected_candidates = _clean_subtitle_region_candidate(repaired["text"])
                    if text:
                        vision_succeeded += 1
                        consecutive_vision_failures = 0
                        cue["text"] = text
                        cue["sourceKind"] = "workbench-vision"
                        cue["ocrRawText"] = cue.get("ocrRawText", "")
                        cue["visualRawText"] = raw_text
                        cue["fallbackReason"] = ""
                        cue["visionApiAttempted"] = True
                        cue["visionApiSucceeded"] = True
                        cue["visionApiFailed"] = False
                        cue["visionConfidence"] = payload.get("confidence", 0)
                        cue["visionRepresentativeTime"] = representative_time
                        cue["candidateRegion"] = "subtitle-region-cue"
                        cue["detectorVersion"] = DETECTOR_VERSION
                        cue["cropRect"] = crop_rect("bottom-subtitle-band", args.crop_height.strip())
                        cue["sampleFrame"] = {"timeSeconds": round(representative_time, 3), "kind": "candidate-cue-representative"}
                        cue["confidence"] = payload.get("confidence", 0.92)
                        cue["rejectedCandidates"] = rejected_candidates
                        continue
                    if not first_vision_error:
                        first_vision_error = "workbench-vision-empty-text"
                else:
                    if not first_vision_error:
                        first_vision_error = payload.get("error", "") or payload.get("backendDetail", "")
                vision_failed += 1
                consecutive_vision_failures += 1
                if consecutive_vision_failures >= max(1, args.vision_stop_after_failures):
                    vision_stopped_after_failures = True
                cue["sourceKind"] = "local-ocr-fallback"
                cue["ocrRawText"] = cue.get("text", "")
                cue["visualRawText"] = ""
                cue["fallbackReason"] = "workbench-vision-failed/local-ocr-fallback"
                cue["visionApiAttempted"] = True
                cue["visionApiSucceeded"] = False
                cue["visionApiFailed"] = True
                cue["visionError"] = first_vision_error
                cue["visionRepresentativeTime"] = representative_time
                cue["candidateRegion"] = "subtitle-region-cue"
                cue["detectorVersion"] = DETECTOR_VERSION
                cue["cropRect"] = crop_rect("bottom-subtitle-band", args.crop_height.strip())
                cue["sampleFrame"] = {"timeSeconds": round(representative_time, 3), "kind": "candidate-cue-representative"}
                cue["confidence"] = cue.get("visionConfidence", 0.82)
                cue["rejectedCandidates"] = rejected_candidates if "rejected_candidates" in locals() else []
                local_fallback_count += 1
    else:
        for cue in cues:
            cue.setdefault(
                "sourceKind",
                "workbench-vision" if vision_enabled else "local-ocr",
            )
            cue.setdefault("ocrRawText", cue.get("text", ""))
            cue.setdefault("visualRawText", cue.get("text", "") if cue.get("sourceKind") == "workbench-vision" else "")
            cue.setdefault("fallbackReason", "")
            cue.setdefault("candidateRegion", "subtitle-region-cue")
            representative_time = max(cue["start"], min(cue["end"], (cue["start"] + cue["end"]) * 0.5))
            cue.setdefault("detectorVersion", DETECTOR_VERSION)
            cue.setdefault("cropRect", crop_rect("bottom-subtitle-band", args.crop_height.strip()))
            cue.setdefault("sampleFrame", {"timeSeconds": round(representative_time, 3), "kind": "candidate-cue-representative"})
            cue.setdefault("confidence", cue.get("visionConfidence", 0.82))
            cue.setdefault("rejectedCandidates", [])
            cue.setdefault("visionApiAttempted", args.vision_provider in {"workbench", "codex-cli"})
            cue.setdefault("visionApiSucceeded", args.vision_provider in {"workbench", "codex-cli"} and vision_succeeded > 0)
            cue.setdefault("visionApiFailed", args.vision_provider in {"workbench", "codex-cli"} and vision_failed > 0)
    if cached_candidates:
        retained_cached_cues = []
        cached_detector_version = str(cached_scan.get("detectorVersion", "")) if cached_scan else ""
        cached_sample_interval = float(cached_scan.get("sampleIntervalSeconds", 0.0) or 0.0) if cached_scan else 0.0
        cached_onset_correction = (
            cached_sample_interval * 0.5
            if cached_detector_version == "subtitle-region-detector-v1"
            else 0.0
        )
        for item in cached_candidates:
            text = _normalize_text(item.get("text", ""))
            start = float(item.get("start", 0.0)) + cached_onset_correction
            end = float(item.get("end", start))
            start = min(start, end)
            if not text or end > float(args.start) + 0.01:
                continue
            retained_cached_cues.append({
                "start": start,
                "end": end,
                "text": text,
                "sourceKind": item.get("sourceKind", "subtitle-region-candidate-cache"),
                "confidence": item.get("confidence", 0.82),
                "cropRect": item.get("cropRect", crop_rect("bottom-subtitle-band", args.crop_height.strip())),
                "sampleFrame": item.get("sampleFrame", {}),
                "detectorVersion": cached_scan.get("detectorVersion", DETECTOR_VERSION),
                "candidateRegion": "subtitle-region-cue",
                "reusedCandidateScanCache": True,
            })
        if retained_cached_cues:
            cues = sorted(retained_cached_cues + cues, key=lambda cue: (cue["start"], cue["end"]))

    detected_times = [sample["time"] for sample in samples if sample.get("success")]
    visual_coverage_start = cues[0]["start"] if cues else 0.0
    visual_coverage_end = cues[-1]["end"] if cues else 0.0
    missed_likely = []
    if detected_times:
        first_detected = min(detected_times)
        last_detected = max(detected_times)
        for sample in samples:
            t = sample.get("time", 0)
            if first_detected <= t <= last_detected and not sample.get("success"):
                missed_likely.append({
                    "time": t,
                    "reason": sample.get("backendDetail") or sample.get("visionError") or "empty-vision-ocr-result-between-detected-subtitle-frames",
                    "sourceKind": sample.get("sourceKind", ""),
                })
    candidate_scan = build_scan_result(
        Path(args.media),
        Path(args.output_vtt).resolve().parent,
        cues,
        float(args.duration or 0.0),
        float(args.sample_interval or 0.0),
        "bottom-subtitle-band",
    )
    union_scan_start = float(args.start or 0.0)
    union_scan_end = float(args.start or 0.0) + float(args.duration or 0.0)
    if cached_scan_start >= 0.0 and cached_scan_end > cached_scan_start:
        union_scan_start = min(union_scan_start, cached_scan_start)
        union_scan_end = max(union_scan_end, cached_scan_end)
    candidate_scan["scanStartSeconds"] = round(union_scan_start, 3)
    candidate_scan["scanEndSeconds"] = round(union_scan_end, 3)
    candidate_scan["durationSec"] = round(union_scan_end - union_scan_start, 3)
    candidate_scan_path = Path(args.candidate_scan_json).resolve() if args.candidate_scan_json.strip() else scan_cache_path(
        Path(args.media),
        Path(args.output_vtt).resolve().parent,
        str(candidate_scan.get("mediaFingerprint", "")),
    )
    candidate_scan["scanCachePath"] = str(candidate_scan_path)
    write_scan_result(candidate_scan_path, candidate_scan)
    os.makedirs(os.path.dirname(os.path.abspath(args.output_json)), exist_ok=True)
    final_payload = {
                "success": bool(cues),
                "status": "completed",
                "mediaPath": os.path.abspath(args.media),
                "scanStartSeconds": round(float(args.start or 0.0), 3),
                "targetDurationSeconds": round(float(args.duration or 0.0), 3),
                "processedCoverageSeconds": round(float(args.duration or 0.0), 3),
                "processedThroughSeconds": round(float(args.start or 0.0) + float(args.duration or 0.0), 3),
                "completedSamples": len(samples),
                "totalSamples": max(1, min(args.max_samples, int(args.duration / max(0.1, args.sample_interval)) + 1)),
                "batchFrameExtraction": bool("batch_ok" in locals() and batch_ok),
                "batchFrameCount": len(batch_frames) if "batch_frames" in locals() else 0,
                "visionBatchCount": len(groups) if "groups" in locals() else 0,
                "visionBatchSize": batch_size if "batch_size" in locals() else 0,
                "visionConcurrency": concurrency if "concurrency" in locals() else 0,
                "visionBatchMetrics": vision_batch_metrics if "vision_batch_metrics" in locals() else [],
                "failedVisionBatches": vision_batch_failures if "vision_batch_failures" in locals() else 0,
                "firstVisionBatchError": first_vision_batch_error if "first_vision_batch_error" in locals() else "",
                "sharedRuntimeHeadlessDetector": True,
                "detectorVersion": DETECTOR_VERSION,
                "candidateScanCachePath": str(candidate_scan_path),
                "candidateScan": candidate_scan,
                "visualSubtitleTrackGenerated": bool(cues),
                "visualCueCount": len(cues),
                "visualCoverageStart": visual_coverage_start,
                "visualCoverageEnd": visual_coverage_end,
                "visualDetectedFrameCount": len(detected_times),
                "visualMissedFrameCount": len(missed_likely),
                "missedLikelySubtitleFrames": missed_likely[:80],
                "workbenchCanReadImageText": args.vision_provider in {"workbench", "codex-cli"},
                "workbenchConfigSource": "codex-cli" if args.vision_provider == "codex-cli" else ("smart-recognition" if args.vision_provider == "workbench" else ""),
                "visionProvider": args.vision_provider,
                "visionApiAttempted": args.vision_provider in {"workbench", "codex-cli"} and vision_attempted > 0,
                "visionApiSucceeded": vision_succeeded > 0,
                "visionApiFailed": args.vision_provider in {"workbench", "codex-cli"} and vision_failed > 0,
                "visionModel": args.vision_model if args.vision_provider in {"workbench", "codex-cli"} else "",
                "visionFrameCount": vision_attempted,
                "visionCueCount": sum(1 for cue in cues if cue.get("sourceKind") == "workbench-vision"),
                "localOcrFallbackFrameCount": local_fallback_count,
                "fallbackReason": (
                    f"{args.vision_provider}-vision-failed/local-ocr-fallback"
                    if args.vision_provider in {"workbench", "codex-cli"} and vision_failed > 0 and local_fallback_count > 0
                    else ""
                ),
                "visionSourceKind": (
                    "workbench-vision"
                    if vision_succeeded > 0
                    else ("local-ocr-fallback" if local_fallback_count > 0 else ("workbench-vision-failed" if vision_attempted > 0 else "unknown"))
                ),
                "visionError": first_vision_error,
                "visionStoppedAfterConsecutiveFailures": vision_stopped_after_failures,
                "ocrEnabled": True,
                "ocrProvider": "local_subtitle_ocr",
                "ocrSamples": len(samples),
                "ocrAcceptedCueCount": len(cues),
                "ocrRejectedCueCount": rejected,
                "elapsedSeconds": round(time.time() - started_at, 3),
                    "preprocess": {
                    "enabled": not args.no_preprocess,
                    "crop": args.crop_height.strip() or "shared-bottom-subtitle-band",
                    "cropRect": crop_rect("bottom-subtitle-band", args.crop_height.strip()),
                    "candidateRegionPolicy": text_temporal_policy(),
                    "inputScope": "subtitle-roi-only",
                    "acceptedCandidateKind": "subtitle-region-cue",
                    "sceneTextPolicy": "reject-background-text-outside-subtitle-roi",
                    "scale": "2x",
                    "filters": "contrast+brightness+grayscale+unsharp",
                    "mergePolicy": "adjacent-similar-text-voting",
                    "trackPolicy": "sample-crop-read-text-deduplicate-merge-continuous-visual-subtitle-track",
                },
                "mojibakeStats": {
                    "sampleCount": len(samples),
                    "fixedSampleCount": mojibake_fixed,
                    "remainingMarkerCount": mojibake_marker_total,
                    "remainingMarkerRate": (
                        mojibake_marker_total / max(1, sum(len(sample.get("text", "")) for sample in samples))
                    ),
                },
                "cues": cues,
                "samples": samples,
            }
    _write_progress(args.output_json, final_payload)
    _write_vtt(args.output_vtt, cues)
    return _result(
        bool(cues),
        sharedRuntimeHeadlessDetector=True,
        detectorVersion=DETECTOR_VERSION,
        candidateScanCachePath=str(candidate_scan_path),
        candidateScan=candidate_scan,
        visualSubtitleTrackGenerated=bool(cues),
        visualCueCount=len(cues),
        visualCoverageStart=visual_coverage_start,
        visualCoverageEnd=visual_coverage_end,
        visualDetectedFrameCount=len(detected_times),
        visualMissedFrameCount=len(missed_likely),
        workbenchCanReadImageText=args.vision_provider in {"workbench", "codex-cli"},
        workbenchConfigSource="codex-cli" if args.vision_provider == "codex-cli" else ("smart-recognition" if args.vision_provider == "workbench" else ""),
        visionProvider=args.vision_provider,
        visionApiAttempted=args.vision_provider in {"workbench", "codex-cli"} and vision_attempted > 0,
        visionApiSucceeded=vision_succeeded > 0,
        visionApiFailed=args.vision_provider in {"workbench", "codex-cli"} and vision_failed > 0,
        visionModel=args.vision_model if args.vision_provider in {"workbench", "codex-cli"} else "",
        visionFrameCount=vision_attempted,
        visionCueCount=sum(1 for cue in cues if cue.get("sourceKind") == "workbench-vision"),
        localOcrFallbackFrameCount=local_fallback_count,
        fallbackReason=(
            f"{args.vision_provider}-vision-failed/local-ocr-fallback"
            if args.vision_provider in {"workbench", "codex-cli"} and vision_failed > 0 and local_fallback_count > 0
            else ""
        ),
        visionSourceKind=(
            "workbench-vision"
            if vision_succeeded > 0
            else ("local-ocr-fallback" if local_fallback_count > 0 else ("workbench-vision-failed" if vision_attempted > 0 else "unknown"))
        ),
        visionError=first_vision_error,
        visionStoppedAfterConsecutiveFailures=vision_stopped_after_failures,
        ocrEnabled=True,
        ocrProvider="local_subtitle_ocr",
        ocrSamples=len(samples),
        ocrAcceptedCueCount=len(cues),
        ocrRejectedCueCount=rejected,
        outputJson=args.output_json,
        outputVtt=args.output_vtt,
        elapsedSeconds=round(time.time() - started_at, 3),
        cueSplitMergeReason="adjacent-similar-text-voting",
        visualCandidatePolicy="subtitle-region-cue-only-bottom-subtitle-roi",
        mojibakeFixedSampleCount=mojibake_fixed,
        mojibakeRemainingMarkerCount=mojibake_marker_total,
        ocrError="" if cues else "no OCR subtitle cues accepted",
    )


if __name__ == "__main__":
    raise SystemExit(main())
