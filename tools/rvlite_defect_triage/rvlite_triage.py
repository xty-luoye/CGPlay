#!/usr/bin/env python3
"""Route an RVLite defect to a likely subsystem using local Ollama/Clef Flash.

The tool deliberately uses only the Python 3.11 standard library.  Clef Flash
is called through Ollama's System One decision endpoint, and a deterministic
keyword router is used when the local model cannot be reached or its response
does not satisfy the expected schema.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
import tempfile
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


MIN_OLLAMA_VERSION = (0, 35, 1)
DEFAULT_MODEL = "clef-flash"
DEFAULT_OLLAMA_URL = "http://127.0.0.1:11434"
# Leave room for base64 expansion and the JSON envelope under System One's
# 32 MiB image-request limit.
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_TEXT_BYTES = 60 * 1024
CONFIDENCE_THRESHOLD = 0.65
AMBIGUITY_MARGIN = 0.15

MODULE_LABELS: dict[str, str] = {
    "playback": "播放、解码、时间线、音视频同步或寻帧",
    "video_export": "视频导出、渲染、FFmpeg 或编码器",
    "exr_color": "EXR/OpenEXR、OpenColorIO、HDR、色彩管理、Gamma 或 LUT",
    "qt_ui": "Qt 界面、窗口、菜单、布局、按钮或快捷键",
    "other": "以上模块都不是主要归属，或信息不足以判断",
}

# System One score criteria must be ordered from least to most severe.
SEVERITY_LABELS = ("low", "medium", "high", "blocker")
SEVERITY_DESCRIPTIONS = [
    "轻微外观、文案或低影响问题，基本不阻塞工作",
    "有明确影响但存在可接受绕过方式",
    "核心工作流明显失败或大多数用户无法正常完成任务",
    "无法启动、数据丢失/损坏，或关键工作流完全不可用",
]

MODULE_KEYWORDS: dict[str, tuple[str, ...]] = {
    "playback": (
        "播放", "卡顿", "卡住", "seek", "寻帧", "解码", "drop frame", "掉帧",
        "timeline", "时间线", "音视频", "同步", "av1", "h264", "h265", "打不开",
    ),
    "video_export": (
        "导出", "export", "渲染", "render", "ffmpeg", "h264", "h265", "hevc",
        "prores", "编码", "encoder", "输出视频", "mp4",
    ),
    "exr_color": (
        "exr", "openexr", "ocio", "opencolorio", "色彩", "色彩管理", "偏色",
        "gamma", "伽马", "lut", "aces", "hdr", "高动态范围", "色域",
    ),
    "qt_ui": (
        "qt", "界面", "ui", "窗口", "菜单", "按钮", "布局", "快捷键", "shortcut",
        "标题栏", "面板", "字体", "对齐", "dock",
    ),
}

SEVERITY_KEYWORDS: dict[str, tuple[str, ...]] = {
    "blocker": (
        "崩溃", "crash", "闪退", "无法启动", "启动失败", "数据丢失", "数据损坏",
        "无法打开", "打不开", "完全不能", "lost data", "corrupt",
    ),
    "high": (
        "失败", "错误", "error", "无法", "不工作", "不可用", "卡死", "hang", "严重",
        "核心功能", "every time", "必现",
    ),
    "medium": (
        "卡顿", "慢", "延迟", "不一致", "偏色", "闪烁", "失效", "绕过", "偶发",
    ),
    "low": (
        "轻微", "文案", "拼写", "间距", "对齐", "外观", "cosmetic", "minor",
    ),
}

VISUAL_CLAIM_KEYWORDS = (
    "截图", "见图", "如图", "图片", "颜色", "色彩", "偏色", "gamma", "界面", "布局",
    "闪烁", "粉", "画面", "screenshot", "image", "attached",
)


class TriageFailure(Exception):
    """A recoverable local-model failure with a stable machine-readable code."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def parse_version(value: Any) -> tuple[int, int, int] | None:
    match = re.search(r"(?:^|[^0-9])([0-9]+)\.([0-9]+)\.([0-9]+)", str(value or ""))
    if not match:
        return None
    return tuple(int(part) for part in match.groups())


def version_text(version: tuple[int, int, int] | None) -> str | None:
    return ".".join(str(part) for part in version) if version else None


def endpoint_url(base_url: str, path: str) -> str:
    base = base_url.rstrip("/")
    # Accept the documented host URL as well as a host URL ending in /api or
    # /v1, without producing /api/api/... or /v1/v1/... paths.
    for suffix in ("/v1", "/api"):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            break
    if base.endswith(path):
        return base
    return base + path


def request_json(
    url: str,
    *,
    timeout: float,
    payload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    data = None
    headers = {"Accept": "application/json"}
    method = "GET"
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
        method = "POST"
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        try:
            detail = exc.read(512).decode("utf-8", errors="replace").strip()
        except Exception:
            detail = ""
        suffix = f": {detail[:240]}" if detail else ""
        raise TriageFailure("http_error", f"HTTP {exc.code} from {url}{suffix}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise TriageFailure("ollama_unavailable", f"无法连接本地 Ollama: {exc}") from exc
    try:
        result = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise TriageFailure("invalid_json", f"Ollama 返回不是有效 JSON: {exc}") from exc
    if not isinstance(result, dict):
        raise TriageFailure("invalid_json", "Ollama 返回的 JSON 顶层不是对象")
    return result


def image_to_base64(image_path: str | Path) -> tuple[str, str, str]:
    path = Path(image_path)
    try:
        if not path.is_file():
            raise TriageFailure("image_read_error", f"截图不存在或不是文件: {path}")
        suffix = path.suffix.lower()
        if suffix not in {".png", ".jpg", ".jpeg", ".webp"}:
            raise TriageFailure("image_read_error", "截图必须是 PNG、JPEG 或 WebP")
        size = path.stat().st_size
        if size > MAX_IMAGE_BYTES:
            raise TriageFailure("image_too_large", f"截图超过 {MAX_IMAGE_BYTES // (1024 * 1024)} MiB 限制")
        raw = path.read_bytes()
    except TriageFailure:
        raise
    except OSError as exc:
        raise TriageFailure("image_read_error", f"无法读取截图 {path}: {exc}") from exc
    mime = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp"}[suffix]
    return base64.b64encode(raw).decode("ascii"), mime, hashlib.sha256(raw).hexdigest()


def build_systemone_payload(text: str, image_b64: str | None) -> dict[str, Any]:
    state: dict[str, Any] = {
        "description": text,
        "screenshot_attached": bool(image_b64),
    }
    payload: dict[str, Any] = {
        "model": DEFAULT_MODEL,
        "state": state,
        "questions": {
            "module": {
                "type": "choice",
                "instructions": "Which RVLite subsystem should own the defect first? Choose the primary module.",
                "criteria": {key: value for key, value in MODULE_LABELS.items()},
            },
            "severity": {
                "type": "score",
                "instructions": "How severe is the user impact? Score from low to blocker.",
                "criteria": SEVERITY_DESCRIPTIONS,
            },
        },
    }
    if image_b64:
        # System One requires raw base64 and rejects data: URLs.
        payload["images"] = [image_b64]
    return payload


def model_names(tags_response: dict[str, Any]) -> set[str]:
    models = tags_response.get("models")
    if not isinstance(models, list):
        return set()
    names: set[str] = set()
    for item in models:
        if isinstance(item, dict):
            name = item.get("name") or item.get("model")
            if isinstance(name, str):
                names.add(name)
    return names


def model_is_installed(names: Iterable[str], requested: str) -> bool:
    requested_base = requested.split(":", 1)[0]
    return any(name == requested or name.split(":", 1)[0] == requested_base for name in names)


def _finite_number(value: Any) -> float | None:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if number == number and number not in (float("inf"), float("-inf")) else None


def _probabilities(answer: dict[str, Any]) -> dict[str, float]:
    raw = answer.get("probabilities")
    if not isinstance(raw, dict):
        return {}
    output: dict[str, float] = {}
    for key, value in raw.items():
        number = _finite_number(value)
        if number is not None and number >= 0:
            output[str(key)] = number
    return output


def _top_margin(probabilities: dict[str, float]) -> float | None:
    values = sorted(probabilities.values(), reverse=True)
    if len(values) < 2:
        return None
    return values[0] - values[1]


def validate_and_normalize_answers(response: dict[str, Any]) -> dict[str, Any]:
    answers = response.get("answers")
    if not isinstance(answers, dict):
        raise TriageFailure("invalid_model_response", "响应缺少 answers 对象")
    module_answer = answers.get("module")
    severity_answer = answers.get("severity")
    if not isinstance(module_answer, dict) or not isinstance(severity_answer, dict):
        raise TriageFailure("invalid_model_response", "响应缺少 module 或 severity 答案")

    module = module_answer.get("choice")
    if module not in MODULE_LABELS:
        raise TriageFailure("invalid_model_response", f"模型返回未知模块: {module!r}")
    module_probs = _probabilities(module_answer)
    module_conf = _finite_number(module_answer.get("confidence"))
    if module_conf is None:
        module_conf = max(module_probs.values(), default=0.0)
    module_conf = max(0.0, min(1.0, module_conf))

    severity_probs = _probabilities(severity_answer)
    severity_score = _finite_number(severity_answer.get("score"))
    severity_index: int | None = None
    if severity_probs:
        try:
            severity_index = int(max(severity_probs, key=severity_probs.get))
        except (TypeError, ValueError):
            severity_index = None
    if severity_index is None and severity_score is not None:
        severity_index = round(severity_score)
    if severity_index is None or not 0 <= severity_index < len(SEVERITY_LABELS):
        raise TriageFailure("invalid_model_response", "模型返回无法解析的 severity score")
    severity_conf = _finite_number(severity_answer.get("confidence"))
    if severity_conf is None:
        severity_conf = max(severity_probs.values(), default=0.0)
    severity_conf = max(0.0, min(1.0, severity_conf))

    return {
        "module": module,
        "module_confidence": module_conf,
        "module_probabilities": module_probs,
        "severity": SEVERITY_LABELS[severity_index],
        "severity_score": severity_score if severity_score is not None else float(severity_index),
        "severity_confidence": severity_conf,
        "severity_probabilities": severity_probs,
    }


def _keyword_hits(text: str) -> dict[str, int]:
    normalized = text.casefold()
    return {
        key: sum(normalized.count(term.casefold()) for term in terms)
        for key, terms in MODULE_KEYWORDS.items()
    }


def _heuristic_result(text: str) -> tuple[str, str, float, float, dict[str, int]]:
    hits = _keyword_hits(text)
    ranked = sorted(hits.items(), key=lambda item: item[1], reverse=True)
    top_key, top_count = ranked[0]
    second_count = ranked[1][1]
    if top_count == 0:
        module = "other"
        module_conf = 0.2
    elif top_count == second_count:
        module = top_key
        module_conf = 0.3
    else:
        module = top_key
        module_conf = 0.4

    normalized = text.casefold()
    severity = "unknown"
    for label in ("blocker", "high", "medium", "low"):
        if any(term.casefold() in normalized for term in SEVERITY_KEYWORDS[label]):
            severity = label
            break
    severity_conf = 0.35 if severity != "unknown" else 0.2
    return module, severity, module_conf, severity_conf, hits


def _human_reasons(
    *,
    text: str,
    screenshot_attached: bool,
    module: str,
    module_confidence: float,
    module_probabilities: dict[str, float],
    severity: str,
    severity_confidence: float,
    severity_probabilities: dict[str, float],
    degraded: bool,
    errors: list[dict[str, str]],
) -> list[str]:
    reasons: list[str] = []
    if module_confidence < CONFIDENCE_THRESHOLD or severity_confidence < CONFIDENCE_THRESHOLD:
        reasons.append("low_confidence")
    module_margin = _top_margin(module_probabilities)
    if module_margin is not None and module_margin < AMBIGUITY_MARGIN:
        reasons.append("ambiguous_module")
    severity_margin = _top_margin(severity_probabilities)
    if severity_margin is not None and severity_margin < AMBIGUITY_MARGIN:
        reasons.append("ambiguous_severity")
    if module == "other":
        reasons.append("module_other_or_insufficient_context")
    normalized = text.casefold()
    if not screenshot_attached and any(term.casefold() in normalized for term in VISUAL_CLAIM_KEYWORDS):
        reasons.append("missing_screenshot_for_visual_claim")
    hits = _keyword_hits(text)
    if sum(value > 0 for value in hits.values()) >= 2:
        reasons.append("cross_module_signals")
    if degraded:
        reasons.append("heuristic_fallback")
    for error in errors:
        code = error.get("code")
        if code in {"ollama_unavailable", "invalid_json"}:
            reasons.append("ollama_unavailable")
        elif code == "http_error":
            reasons.append("ollama_api_error")
        elif code == "ollama_version_too_old":
            reasons.append("ollama_version_too_old")
        elif code == "model_not_installed":
            reasons.append("model_not_installed")
        elif code == "image_read_error":
            reasons.append("image_read_error")
        elif code == "image_too_large":
            reasons.append("image_too_large")
        elif code == "request_too_large":
            reasons.append("request_too_large")
        elif code == "invalid_model_response":
            reasons.append("invalid_model_response")
    # Keep stable order while avoiding duplicate reasons.
    return list(dict.fromkeys(reasons))


def _error(code: str, message: str) -> dict[str, str]:
    return {"code": code, "message": message}


def _ollama_triage(
    text: str,
    image_b64: str | None,
    *,
    ollama_url: str,
    model: str,
    timeout: float,
) -> tuple[dict[str, Any], dict[str, Any], str | None]:
    version_response = request_json(endpoint_url(ollama_url, "/api/version"), timeout=timeout)
    raw_version = version_response.get("version")
    parsed_version = parse_version(raw_version)
    if parsed_version is None or parsed_version < MIN_OLLAMA_VERSION:
        actual = str(raw_version or "unknown")
        raise TriageFailure(
            "ollama_version_too_old",
            f"Clef Flash 需要 Ollama >= 0.35.1，当前版本为 {actual}",
        )
    tags_response = request_json(endpoint_url(ollama_url, "/api/tags"), timeout=timeout)
    if not model_is_installed(model_names(tags_response), model):
        raise TriageFailure("model_not_installed", f"本地未安装模型 {model!r}，请先运行 ollama pull clef-flash")

    payload = build_systemone_payload(text, image_b64)
    payload["model"] = model
    response = request_json(endpoint_url(ollama_url, "/v1/systemone"), timeout=timeout, payload=payload)
    normalized = validate_and_normalize_answers(response)
    raw_answers = response.get("answers")
    if not isinstance(raw_answers, dict):
        # validate_and_normalize_answers already checks this; retain a defensive
        # type guard for callers that replace it in tests.
        raise TriageFailure("invalid_model_response", "响应 answers 不是对象")
    return normalized, raw_answers, version_text(parsed_version)


def triage_defect(
    text: str,
    *,
    screenshot_path: str | Path | None = None,
    ollama_url: str = DEFAULT_OLLAMA_URL,
    model: str = DEFAULT_MODEL,
    timeout: float = 30.0,
    force_heuristic: bool = False,
) -> dict[str, Any]:
    """Return one JSON-serializable triage record.

    Model failures are intentionally converted to a deterministic local result;
    callers can inspect ``model.mode`` and ``errors`` to decide whether a human
    must review it.
    """

    text = text.strip()
    if not text:
        raise ValueError("缺陷文本不能为空")

    errors: list[dict[str, str]] = []
    image_b64: str | None = None
    image_sha256: str | None = None
    image_mime: str | None = None
    if screenshot_path is not None:
        try:
            image_b64, image_mime, image_sha256 = image_to_base64(screenshot_path)
        except TriageFailure as exc:
            errors.append(_error(exc.code, exc.message))

    if len(text.encode("utf-8")) > MAX_TEXT_BYTES:
        errors.append(_error("request_too_large", f"缺陷文本超过 {MAX_TEXT_BYTES} 字节，已跳过本地模型"))
    if image_b64 is not None and len(image_b64.encode("ascii")) + len(text.encode("utf-8")) > 31 * 1024 * 1024:
        errors.append(_error("request_too_large", "截图编码与文本接近或超过 System One 的 32 MiB 请求限制"))

    normalized: dict[str, Any] | None = None
    raw_answers: dict[str, Any] | None = None
    detected_version: str | None = None
    can_call_ollama = not any(item["code"] == "request_too_large" for item in errors)
    if not force_heuristic and can_call_ollama:
        try:
            normalized, raw_answers, detected_version = _ollama_triage(
                text,
                image_b64,
                ollama_url=ollama_url,
                model=model,
                timeout=timeout,
            )
        except TriageFailure as exc:
            errors.append(_error(exc.code, exc.message))

    if normalized is None:
        module, severity, module_conf, severity_conf, _ = _heuristic_result(text)
        module_probs: dict[str, float] = {}
        severity_probs: dict[str, float] = {}
        mode = "heuristic"
        effective_conf = min(module_conf, severity_conf)
        degraded = True
    else:
        module = normalized["module"]
        severity = normalized["severity"]
        module_conf = normalized["module_confidence"]
        severity_conf = normalized["severity_confidence"]
        module_probs = normalized["module_probabilities"]
        severity_probs = normalized["severity_probabilities"]
        mode = "ollama"
        effective_conf = min(module_conf, severity_conf)
        degraded = False

    reasons = _human_reasons(
        text=text,
        screenshot_attached=image_b64 is not None,
        module=module,
        module_confidence=module_conf,
        module_probabilities=module_probs,
        severity=severity,
        severity_confidence=severity_conf,
        severity_probabilities=severity_probs,
        degraded=degraded,
        errors=errors,
    )
    if force_heuristic and not any(item["code"] == "forced_heuristic" for item in errors):
        errors.append(_error("forced_heuristic", "命令行要求跳过 Ollama"))
        if "heuristic_fallback" not in reasons:
            reasons.append("heuristic_fallback")

    record: dict[str, Any] = {
        "schema_version": "1.0",
        "created_at": utc_now(),
        "input": {
            "description": text,
            "screenshot_path": str(screenshot_path) if screenshot_path is not None else None,
            "screenshot_attached": image_b64 is not None,
            "screenshot_mime": image_mime,
            "screenshot_sha256": image_sha256,
        },
        "model": {
            "name": model,
            "endpoint": endpoint_url(ollama_url, "/v1/systemone"),
            "ollama_version": detected_version,
            "mode": mode,
        },
        "triage": {
            "module": module,
            "severity": severity,
            "confidence": round(effective_conf, 4),
            "module_confidence": round(module_conf, 4),
            "severity_confidence": round(severity_conf, 4),
            "human_confirmation_required": bool(reasons),
            "human_confirmation_reasons": reasons,
        },
        "raw_answers": raw_answers,
        "errors": errors,
    }
    return record


def write_json_atomic(record: dict[str, Any], output_path: str | Path) -> Path:
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Keep the temporary file beside the destination so replace is atomic on Windows too.
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", suffix=".tmp", prefix=f".{output.name}.", dir=output.parent, delete=False
    ) as handle:
        temporary = Path(handle.name)
        json.dump(record, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    temporary.replace(output)
    return output


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="RVLite 离线缺陷分流器（Ollama/Clef Flash + 本地降级）")
    parser.add_argument("--text", required=True, help="缺陷描述")
    parser.add_argument("--image", help="可选截图路径（PNG/JPEG/WebP）")
    parser.add_argument("--output", default="triage.json", help="输出 JSON 路径，默认 triage.json")
    parser.add_argument("--ollama-url", default=DEFAULT_OLLAMA_URL, help="Ollama 地址，默认 http://127.0.0.1:11434")
    parser.add_argument("--model", default=DEFAULT_MODEL, help="模型名，默认 clef-flash")
    parser.add_argument("--timeout", type=float, default=30.0, help="每次本地请求超时秒数，默认 30")
    parser.add_argument("--force-heuristic", action="store_true", help="跳过 Ollama，仅使用本地规则")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        record = triage_defect(
            args.text,
            screenshot_path=args.image,
            ollama_url=args.ollama_url,
            model=args.model,
            timeout=args.timeout,
            force_heuristic=args.force_heuristic,
        )
        output = write_json_atomic(record, args.output)
    except ValueError as exc:
        print(f"输入错误: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"无法写入 JSON: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"output": str(output), "mode": record["model"]["mode"], "module": record["triage"]["module"], "severity": record["triage"]["severity"]}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
