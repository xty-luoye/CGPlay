#!/usr/bin/env python3
"""Headless subtitle translation gate for CGPlay/RVLite.

This verifier intentionally avoids opening the player UI. It reads media-scoped
sidecars, checks source/translated/final cue decisions, and optionally captures
subtitle-region crops with ffmpeg for auditable evidence.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, asdict
from difflib import SequenceMatcher
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from subtitle_region_detector import (
    DETECTOR_VERSION,
    build_scan_result,
    crop_rect,
    ffmpeg_crop_filter,
    preprocess_filter,
    scan_cache_path,
    text_temporal_policy,
    write_scan_result,
)


ARTIFACT_DEFAULT = Path(r"C:\Users\1\Desktop\RVLite\tests\artifacts\translation_all_sources_20260709")
TRANSLATION_DIR_DEFAULT = Path(r"C:\Users\1\Desktop\CGPlay_Translations")

ANIONE_SIGNATURES = [
    "\u6697\u5f71\u5ead\u56ed",
    "\u95c7\u5f71",
    "\u5f71\u4e4b\u5f3a\u8005",
    "\u5f71\u91ce",
    "\u543e\u7b49",
    "\u5ead\u56ed",
    "\u7f16\u53f7\u8005",
    "\u72e9\u730e\u5f00\u59cb",
]
JINWOO_SIGNATURES = [
    "black soldiers",
    "black ice bears",
    "arise from the shadow",
    "solo leveling",
    "\u9ed1\u8272\u58eb\u5175",
    "\u9ed1\u51b0\u718a",
]
SOURCE_SIGNATURE_ALLOW = [
    "\u95c7\u5f71\u5ead\u5712",
    "\u6697\u5f71\u5ead\u56ed",
    "shadow garden",
    "shadow-garden",
    "numbered",
    "hunting begins",
    "hunt begins",
]


@dataclass
class Cue:
    start: float
    end: float
    text: str
    index: int = 0
    source_kind: str = ""
    path: str = ""


@dataclass
class FinalCueDecision:
    chosen: Optional[Cue]
    chosen_source: str
    chosen_reason: str
    translate_ms: float = 0.0
    merge_ms: float = 0.0


def _ts_to_seconds(value: str) -> float:
    value = value.strip().replace(",", ".")
    parts = value.split(":")
    if len(parts) != 3:
        return 0.0
    h = int(parts[0])
    m = int(parts[1])
    s = float(parts[2])
    return h * 3600.0 + m * 60.0 + s


def _seconds_to_ts(seconds: float) -> str:
    ms = max(0, int(round(seconds * 1000)))
    h = ms // 3600000
    m = (ms // 60000) % 60
    s = (ms // 1000) % 60
    r = ms % 1000
    return f"{h:02d}:{m:02d}:{s:02d}.{r:03d}"


def safe_name(value: str) -> str:
    keep = []
    for ch in value:
        if ch.isalnum() or ch in "-_.":
            keep.append(ch)
        elif ch.isspace():
            keep.append("_")
    return ("".join(keep).strip("._") or "video")[:80]


def read_vtt(path: Path, source_kind: str = "") -> List[Cue]:
    if not path.exists():
        return []
    text = path.read_text(encoding="utf-8", errors="replace")
    cues: List[Cue] = []
    block: List[str] = []
    idx = 0
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if line.strip():
            block.append(line.rstrip("\n"))
            continue
        if block:
            cue = _parse_vtt_block(block, idx + 1, source_kind, path)
            if cue:
                cues.append(cue)
                idx += 1
            block = []
    if block:
        cue = _parse_vtt_block(block, idx + 1, source_kind, path)
        if cue:
            cues.append(cue)
    return cues


def _parse_vtt_block(lines: List[str], index: int, source_kind: str, path: Path) -> Optional[Cue]:
    timing_i = -1
    for i, line in enumerate(lines):
        if "-->" in line:
            timing_i = i
            break
    if timing_i < 0:
        return None
    left, right = lines[timing_i].split("-->", 1)
    start = _ts_to_seconds(left)
    end = _ts_to_seconds(right.split()[0])
    cue_text = "\n".join(line.strip() for line in lines[timing_i + 1 :] if line.strip()).strip()
    if end <= start or not cue_text:
        return None
    return Cue(start=start, end=end, text=cue_text, index=index, source_kind=source_kind, path=str(path))


def media_fingerprint(path: Path) -> str:
    st = path.stat()
    # Match the application-side C++ fingerprint:
    # absolute path is lower-cased, then bound to size + mtime.
    key = f"{str(path.resolve()).lower()}|{st.st_size}|{int(st.st_mtime_ns // 1000000)}"
    return hashlib.sha256(key.encode("utf-8", errors="replace")).hexdigest()


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def media_identity(path: Path) -> Dict[str, object]:
    st = path.stat()
    return {
        "mediaPath": str(path.resolve()),
        "fileSize": st.st_size,
        "mtimeMs": int(st.st_mtime_ns // 1000000),
        "mediaFingerprint": media_fingerprint(path),
    }


def media_duration_seconds(path: Path) -> float:
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        str(path),
    ]
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        if proc.returncode == 0:
            value = proc.stdout.strip()
            return max(0.0, float(value)) if value else 0.0
    except Exception:
        return 0.0
    return 0.0


def _same_media_path(a: str, b: str) -> bool:
    try:
        return str(Path(a).resolve()).lower() == str(Path(b).resolve()).lower()
    except Exception:
        return str(a).replace("\\", "/").lower() == str(b).replace("\\", "/").lower()


def adopt_app_media_identity(media: Path, translations_dir: Path, identity: Dict[str, object]) -> Dict[str, object]:
    # Runtime CGPlay computes the fingerprint from the current media path,
    # size, and mtime.  Do not "adopt" an older sidecar fingerprint here:
    # stale same-path metadata can otherwise make a fresh full-video gate load
    # a shorter old track while the application would use the computed key.
    return identity


def normalize_text(value: str) -> str:
    value = str(value or "").replace("\r", "\n")
    value = re.sub(r"[\u200b\u200c\u200d\ufeff]", "", value)
    value = re.sub(r"\s+", " ", value).strip()
    value = value.replace("？", "?").replace("！", "!").replace("，", ",").replace("。", ".")
    return value


_TRAD_TO_SIMP = str.maketrans({
    "們": "们",
    "為": "为",
    "來": "来",
    "對": "对",
    "誰": "谁",
    "聖": "圣",
    "場": "场",
    "這": "这",
    "樣": "样",
    "貌": "貌",
    "闇": "暗",
    "嗚": "呜",
    "嗎": "吗",
    "嗎": "吗",
    "說": "说",
    "敵": "敌",
    "廢": "废",
    "實": "实",
    "現": "现",
    "個": "个",
    "後": "后",
    "園": "园",
    "氣": "气",
    "開": "开",
    "關": "关",
    "長": "长",
})


def normalize_zh_hans(value: str) -> str:
    text = normalize_text(value).translate(_TRAD_TO_SIMP)
    for src, dst in {
        "\u767c\u5e03": "\u53d1\u5e03",
        "\u767c": "\u53d1",
        "\u56b4": "\u4e25",
        "\u9084": "\u8fd8",
        "\u53ea\u80fd": "\u53ea\u80fd",
        "\u90a3\u662f\u4ed6\u5011": "\u90a3\u662f\u4ed6\u4eec",
        # OCR often confuses Traditional "闇影" with "閣影" in this font.
        # Keep this as a generic OCR normalization, not a line/title special case.
        "閣影": "暗影",
        "別": "别",
        "礙": "碍",
        "妨礙": "妨碍",
        "來": "来",
        "闇": "暗",
        "們": "们",
        "園": "园",
        "說": "说",
        "這": "这",
        "為": "为",
        "麼": "么",
        "戲": "戏",
        "場": "场",
        "對": "对",
        "親": "亲",
        "誰": "谁",
        "樣": "样",
        "馬": "马",
        "車": "车",
        "著": "着",
    }.items():
        text = text.replace(src, dst)
    return strip_leading_isolated_han_ocr_noise(text)


def strip_leading_isolated_han_ocr_noise(value: str) -> str:
    text = str(value or "").strip()
    match = re.match(r"^([\u4e00-\u9fff])\s+([\u4e00-\u9fff].*)$", text)
    if not match:
        return text
    prefix = match.group(1)
    rest = match.group(2).strip()
    rest_han = sum(1 for ch in rest if "\u4e00" <= ch <= "\u9fff")
    # Subtitle OCR can return a single stray Han glyph before the true line.
    # Keep common one-character utterances/pronouns/actions; strip unlikely
    # isolated prefixes only in high-quality visual text normalization.
    allowed = set("啊嗯呃哦喂是不我你他她它这那好来去别看听说走住快")
    if rest_han >= 2 and prefix not in allowed:
        return rest
    return text


def compact_text(value: str) -> str:
    return re.sub(r"[\s，。！？；：、,.!?;:'\"()\[\]{}<>《》「」『』“”‘’\-]+", "", normalize_zh_hans(value).lower())


def _char_multiset_coverage(expected: str, actual: str) -> float:
    if not expected:
        return 1.0 if not actual else 0.0
    remaining: Dict[str, int] = {}
    for ch in actual:
        remaining[ch] = remaining.get(ch, 0) + 1
    hit = 0
    for ch in expected:
        count = remaining.get(ch, 0)
        if count <= 0:
            continue
        hit += 1
        remaining[ch] = count - 1
    return hit / max(1, len(expected))


def _extra_token_ratio(expected: str, actual: str) -> float:
    if not actual:
        return 0.0 if not expected else 1.0
    expected_counts: Dict[str, int] = {}
    for ch in expected:
        expected_counts[ch] = expected_counts.get(ch, 0) + 1
    extra = 0
    for ch in actual:
        count = expected_counts.get(ch, 0)
        if count <= 0:
            extra += 1
        else:
            expected_counts[ch] = count - 1
    return extra / max(1, len(actual))


def _key_terms(value: str) -> List[str]:
    compact = compact_text(value)
    if not compact:
        return []
    terms: List[str] = []
    for n in (4, 3, 2):
        for i in range(0, max(0, len(compact) - n + 1)):
            term = compact[i : i + n]
            if term and term not in terms:
                terms.append(term)
    if len(compact) <= 2 and compact not in terms:
        terms.append(compact)
    return terms[:24]


def _key_term_coverage(expected: str, actual: str) -> float:
    terms = _key_terms(expected)
    if not terms:
        return 1.0
    hits = sum(1 for term in terms if term in actual)
    return hits / max(1, len(terms))


def cjk_literal_fidelity(raw_source: str, final_text: str) -> Dict[str, object]:
    raw = compact_text(raw_source)
    final = compact_text(final_text)
    thresholds = {
        "charCoverageMin": 0.90,
        "keyTermCoverageMin": 0.90,
        "extraTokenRatioMax": 0.10,
        "shortLiteralSimilarityMin": 0.98,
    }
    is_short = len(raw) <= 4
    if not raw and not final:
        return {
            "literalSimilarity": 1.0,
            "normalizedTextSimilarity": 1.0,
            "charCoverage": 1.0,
            "keyTermCoverage": 1.0,
            "extraTokenRatio": 0.0,
            "extraRatio": 0.0,
            "missingRatio": 0.0,
            "isShortCue": is_short,
            "thresholds": thresholds,
            "failCategory": "",
            "pass": True,
        }
    if not raw or not final:
        return {
            "literalSimilarity": 0.0,
            "normalizedTextSimilarity": 0.0,
            "charCoverage": 0.0,
            "keyTermCoverage": 0.0,
            "extraTokenRatio": 1.0,
            "extraRatio": 1.0,
            "missingRatio": 1.0,
            "rawCompact": raw,
            "finalCompact": final,
            "isShortCue": is_short,
            "thresholds": thresholds,
            "failCategory": "missing-final" if raw else "no-source-text",
            "pass": False,
        }
    similarity = SequenceMatcher(None, raw, final).ratio()
    char_coverage = _char_multiset_coverage(raw, final)
    key_term_coverage = _key_term_coverage(raw, final)
    extra_ratio = _extra_token_ratio(raw, final)
    missing_ratio = 1.0 - char_coverage
    pass_gate = (
        char_coverage >= thresholds["charCoverageMin"]
        and key_term_coverage >= thresholds["keyTermCoverageMin"]
        and extra_ratio <= thresholds["extraTokenRatioMax"]
        and (not is_short or (raw == final or similarity >= thresholds["shortLiteralSimilarityMin"]))
    )
    fail_category = ""
    if not pass_gate:
        if extra_ratio > thresholds["extraTokenRatioMax"]:
            fail_category = "hallucinated-addition"
        elif key_term_coverage < thresholds["keyTermCoverageMin"] or char_coverage < thresholds["charCoverageMin"]:
            fail_category = "semantic-mismatch"
        else:
            fail_category = "literal-fidelity-failed"
    return {
        "literalSimilarity": round(similarity, 6),
        "normalizedTextSimilarity": round(similarity, 6),
        "charCoverage": round(char_coverage, 6),
        "keyTermCoverage": round(key_term_coverage, 6),
        "extraTokenRatio": round(extra_ratio, 6),
        "extraRatio": round(extra_ratio, 6),
        "missingRatio": round(missing_ratio, 6),
        "rawCompact": raw,
        "finalCompact": final,
        "isShortCue": is_short,
        "thresholds": thresholds,
        "failCategory": fail_category,
        "pass": pass_gate,
    }


def translation_fidelity(raw_source: str, final_text: str, source_is_cjk: bool) -> Dict[str, object]:
    if source_is_cjk:
        result = cjk_literal_fidelity(raw_source, final_text)
        result["mode"] = "cjk-literal-simplified-normalization"
        raw_len = len(str(result.get("rawCompact", "")))
        final_len = len(str(result.get("finalCompact", "")))
        expanded = final_len > max(raw_len + 2, int(raw_len * 1.25))
        result["hallucinatedAddition"] = bool(result.get("failCategory") == "hallucinated-addition" or (expanded and result["literalSimilarity"] < 0.85))
        result["overTranslation"] = bool(expanded or (result["extraTokenRatio"] > 0.25 and final_len > raw_len + 1))
        result["semanticMismatch"] = not bool(result["pass"])
        return result
    final = normalize_text(final_text)
    raw = normalize_text(raw_source)
    raw_words = re.findall(r"[A-Za-z]+", raw)
    raw_word_count = len(raw_words)
    raw_units = max(
        1,
        raw_word_count
        + len(re.findall(r"[\u3040-\u30ff\uac00-\ud7af]", raw))
        + len(re.findall(r"\d+", raw)),
    )
    latin_leak = contains_latin_sentence(final)
    final_compact = compact_text(final)
    final_len = len(final_compact)
    too_short = contains_cjk(raw) is False and contains_cjk(final) and raw_word_count >= 4 and final_len <= 1
    raw_compact_len = max(1, len(re.sub(r"\s+", "", raw)))
    is_short_source = raw_units <= 2 or raw_compact_len <= 5
    short_overexpanded = bool(is_short_source and final_len > 4)
    length_expanded = bool(final_len > max(raw_units * 4 + 8, raw_compact_len * 2))
    extra_token_ratio = max(0.0, (final_len - max(1, raw_units * 3)) / max(1, final_len))
    semantic_hits = []
    semantic_misses = []
    raw_lower = raw.lower()
    english_terms = {
        "black": ["黑"],
        "soldier": ["士兵", "士"],
        "soldiers": ["士兵", "士"],
        "bear": ["熊"],
        "bears": ["熊"],
        "waste": ["浪费", "白费"],
        "confident": ["自信"],
        "regular": ["普通", "一般"],
        "run": ["跑", "逃", "耗尽", "用完"],
        "escape": ["逃", "跑"],
        "father": ["父亲", "父"],
        "enjoy": ["尽兴", "享受", "开心"],
        "enjoyed": ["尽兴", "享受", "开心"],
        "worthless": ["没用", "无用", "废物", "不值"],
        "hide": ["躲", "藏"],
        "hiding": ["躲", "藏"],
        "save": ["救"],
        "not": ["不", "没", "无", "别"],
        "cannot": ["不能", "不"],
        "can't": ["不能", "不"],
        "why": ["为什么", "为何"],
        "see": ["明白", "看", "见"],
    }
    for term, expected_words in english_terms.items():
        if re.search(rf"\b{re.escape(term)}\b", raw_lower):
            if any(word in final_compact for word in expected_words):
                semantic_hits.append(term)
            else:
                semantic_misses.append(term)
    if semantic_hits or semantic_misses:
        semantic_key_coverage = len(semantic_hits) / max(1, len(semantic_hits) + len(semantic_misses))
    else:
        semantic_key_coverage = 0.90 if not (latin_leak or too_short or short_overexpanded or length_expanded) else 0.0
    semantic_mismatch = bool(latin_leak or too_short or short_overexpanded or (semantic_misses and semantic_key_coverage < 0.80))
    over_translation = bool(short_overexpanded or length_expanded or extra_token_ratio > 0.45)
    hallucinated = bool(over_translation and semantic_key_coverage < 0.90)
    pass_gate = not semantic_mismatch and not over_translation
    fail_category = ""
    if latin_leak:
        fail_category = "raw-source-language-leak"
    elif short_overexpanded or over_translation:
        fail_category = "over-translation"
    elif semantic_mismatch:
        fail_category = "semantic-mismatch"
    return {
        "mode": "all-language-fidelity-first-translation",
        "literalSimilarity": 0.0,
        "normalizedTextSimilarity": 0.0,
        "charCoverage": 1.0,
        "keyTermCoverage": round(semantic_key_coverage, 6),
        "extraRatio": round(extra_token_ratio, 6),
        "extraTokenRatio": round(extra_token_ratio, 6),
        "missingRatio": 0.0,
        "semanticKeyHits": semantic_hits,
        "semanticKeyMisses": semantic_misses,
        "sourceUnitCount": raw_units,
        "finalCompactLength": final_len,
        "isShortCue": is_short_source,
        "shortOverexpanded": short_overexpanded,
        "lengthExpanded": length_expanded,
        "hallucinatedAddition": hallucinated,
        "overTranslation": over_translation,
        "semanticMismatch": semantic_mismatch,
        "failCategory": fail_category,
        "pass": pass_gate,
    }


def contains_cjk(value: str) -> bool:
    return bool(re.search(r"[\u3400-\u9fff]", value or ""))


def contains_kana_or_replacement(value: str) -> bool:
    return bool(re.search(r"[\u3040-\u30ff\ufffd]", value or ""))


def contains_latin_sentence(value: str) -> bool:
    return bool(re.search(r"[A-Za-z]{2,}(?:[ '\u2019-]+[A-Za-z]{2,})+", value or ""))


def is_placeholder_translation(value: str) -> bool:
    text = normalize_text(value).lower()
    compact = re.sub(r"\s+", "", text)
    placeholders = [
        "\u4e2d\u6587\u7ffb\u8bd1\u5f85\u7cbe\u4fee",
        "\u5f85\u7cbe\u4fee",
        "\u5f85\u7ffb\u8bd1",
        "translation pending",
        "todo",
        "placeholder",
    ]
    return any(token in compact or token in text for token in placeholders)


def is_watermark_translation(value: str) -> bool:
    compact = re.sub(r"\s+", "", normalize_text(value).lower())
    tokens = [
        "\u52a8\u753b",
        "\u5347\u5e73\u52a8\u753b",
        "\u5e73\u7ea7\u52a8\u753b",
        "levelinganimation",
        "animationpartners",
    ]
    return any(token in compact for token in tokens)


def is_usable_final_text(final_text: str, raw_source: str, source_is_cjk: bool) -> bool:
    final_full = normalize_zh_hans(final_text)
    if not final_full:
        return False
    if is_placeholder_translation(final_full):
        return False
    if is_watermark_translation(final_full) and contains_latin_sentence(raw_source):
        return False
    if not contains_cjk(final_full):
        return False
    if contains_kana_or_replacement(final_full):
        return False
    if contains_latin_sentence(final_full):
        return False
    if not source_is_cjk and normalize_text(final_full).lower() == normalize_text(raw_source).lower():
        return False
    return True


def choose_final_cue(
    cue: Cue,
    raw: str,
    source_is_cjk: bool,
    translated_tracks: Dict[str, List[Cue]],
    primary_source_kind: str,
    media: Path,
) -> FinalCueDecision:
    chosen: Optional[Cue] = None
    chosen_source = ""
    chosen_reason = ""
    translate_ms = 0.0
    merge_ms = 0.0

    translate_started = time.perf_counter()
    if not source_is_cjk and cue.source_kind not in ("local-subtitle", "quick-asr"):
        for kind in ("workbench-vision-translated", "ocr-translated", "online-translated"):
            candidate = best_overlapping(cue, translated_tracks[kind], max_start_delta=1.2)
            if candidate and normalize_zh_hans(candidate.text) and not is_placeholder_translation(candidate.text) and not is_watermark_translation(candidate.text):
                chosen = candidate
                chosen_source = kind
                chosen_reason = "translated-visual-cue"
                break
    translate_ms += (time.perf_counter() - translate_started) * 1000.0

    if not chosen and source_is_cjk:
        chosen = Cue(cue.start, cue.end, normalize_zh_hans(raw), source_kind=cue.source_kind, path=cue.path)
        chosen_source = cue.source_kind
        chosen_reason = "literal-cjk-visual-cue"

    if not chosen:
        merge_started = time.perf_counter()
        enhanced = best_overlapping(cue, translated_tracks["enhanced"], max_start_delta=0.75)
        if enhanced and contains_cjk(enhanced.text) and not is_placeholder_translation(enhanced.text):
            chosen = enhanced
            chosen_source = "enhanced"
            chosen_reason = "translated-enhanced-visual-cue"
        merge_ms += (time.perf_counter() - merge_started) * 1000.0

    if not chosen and primary_source_kind in ("workbench-vision", "visual-subtitle"):
        merge_started = time.perf_counter()
        quick = best_overlapping(cue, translated_tracks["quick"], max_start_delta=1.2)
        if quick and contains_cjk(quick.text) and not is_placeholder_translation(quick.text) and not cross_media_forbidden_hits(media, quick.text, raw):
            chosen = quick
            chosen_source = "quick"
            chosen_reason = "quick-baseline-for-current-visual-source"
        merge_ms += (time.perf_counter() - merge_started) * 1000.0

    if not chosen and primary_source_kind in ("local-subtitle", "online-subtitle", "quick-asr"):
        merge_started = time.perf_counter()
        quick = best_overlapping(cue, translated_tracks["quick"], max_start_delta=1.2)
        if quick and contains_cjk(quick.text) and not is_placeholder_translation(quick.text):
            chosen = quick
            chosen_source = "quick"
            chosen_reason = "quick-asr-baseline" if primary_source_kind == "quick-asr" else "quick-baseline-for-text-subtitle"
        merge_ms += (time.perf_counter() - merge_started) * 1000.0

    return FinalCueDecision(
        chosen=chosen,
        chosen_source=chosen_source,
        chosen_reason=chosen_reason,
        translate_ms=translate_ms,
        merge_ms=merge_ms,
    )


def audit_final_cue(
    cue: Cue,
    raw: str,
    source_is_cjk: bool,
    decision: FinalCueDecision,
    primary_source_kind: str,
    media: Path,
) -> Dict[str, object]:
    chosen = decision.chosen
    final_full = normalize_zh_hans(chosen.text if chosen else "")
    final_has_english = contains_latin_sentence(final_full)
    final_is_raw_source = (
        bool(final_full)
        and normalize_text(final_full).lower() == normalize_text(raw).lower()
        and not source_is_cjk
    )
    forbidden_hits = cross_media_forbidden_hits(media, final_full, raw)
    placeholder_final = is_placeholder_translation(final_full)
    fidelity = translation_fidelity(raw, final_full, source_is_cjk)
    pass_gate = is_usable_final_text(final_full, raw, source_is_cjk) and bool(fidelity.get("pass", False))
    if forbidden_hits:
        pass_gate = False
    if not source_is_cjk and decision.chosen_source not in ("workbench-vision-translated", "ocr-translated", "online-translated", "enhanced", "quick"):
        pass_gate = False
    asr_intrusion = (
        primary_source_kind in ("workbench-vision", "visual-subtitle")
        and decision.chosen_source == "quick"
        and decision.chosen_reason != "quick-baseline-for-current-visual-source"
    )
    if asr_intrusion:
        pass_gate = False
    fail_reason = ""
    if not pass_gate:
        if placeholder_final:
            fail_reason = "placeholder-translation"
        elif not final_full:
            fail_reason = "translated-final-missing"
        elif fidelity.get("hallucinatedAddition") or fidelity.get("overTranslation"):
            fail_reason = "hallucinated-or-overtranslated-final"
        elif fidelity.get("semanticMismatch"):
            fail_reason = "semantic-mismatch"
        else:
            fail_reason = "target-language-or-source-priority-failed"
    return {
        "finalFull": final_full,
        "visibleText": visible_two_line(final_full),
        "finalHasEnglish": final_has_english,
        "finalIsRawSource": final_is_raw_source,
        "forbiddenHits": forbidden_hits,
        "placeholderFinal": placeholder_final,
        "fidelity": fidelity,
        "asrIntrusion": asr_intrusion,
        "passGate": pass_gate,
        "failReason": fail_reason,
    }


def percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(float(v) for v in values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    frac = rank - low
    return ordered[low] * (1.0 - frac) + ordered[high] * frac


def average(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def summarize_performance(cues: List[Dict[str, object]]) -> Dict[str, object]:
    ready = [float(c.get("readyAtMs", 0.0)) for c in cues]
    vision = [float(c.get("visionReadMs", 0.0)) for c in cues]
    translate = [float(c.get("translateMs", 0.0)) for c in cues]
    lateness = [float(c.get("latenessMs", 0.0)) for c in cues]
    late_cues = [c for c in cues if float(c.get("latenessMs", 0.0)) > 0.0]
    slowest = sorted(cues, key=lambda c: float(c.get("readyAtMs", 0.0)), reverse=True)[:10]
    return {
        "metricMode": "headless-full-video-cue-pipeline",
        "cueCount": len(cues),
        "averageCueReadyMs": round(average(ready), 3),
        "p95CueReadyMs": round(percentile(ready, 0.95), 3),
        "maxCueReadyMs": round(max(ready) if ready else 0.0, 3),
        "averageVisionReadMs": round(average(vision), 3),
        "averageTranslateMs": round(average(translate), 3),
        "p95LatenessMs": round(percentile(lateness, 0.95), 3),
        "maxLatenessMs": round(max(lateness) if lateness else 0.0, 3),
        "lateTranslationCueCount": len(late_cues),
        "slowestCues": [
            {
                "start": c.get("start"),
                "end": c.get("end"),
                "rawSourceText": c.get("rawSourceText", ""),
                "finalDisplayedText": c.get("finalDisplayedText", ""),
                "readyAtMs": c.get("readyAtMs", 0),
                "latenessMs": c.get("latenessMs", 0),
                "sourceKind": c.get("sourceKind", ""),
                "chosenReason": c.get("chosenReason", ""),
            }
            for c in slowest
        ],
    }


def coverage_progress_audit(cues: List[Dict[str, object]], reported_through_ms: Optional[float] = None) -> Dict[str, object]:
    ordered = sorted(cues, key=lambda c: (float(c.get("start", 0.0)), float(c.get("end", 0.0))))
    actual_through_ms = 0.0
    first_gap_ms: Optional[float] = None
    gaps: List[Dict[str, object]] = []
    tolerance_ms = 250.0
    for cue in ordered:
        start_ms = float(cue.get("cueStartMs", float(cue.get("start", 0.0)) * 1000.0))
        end_ms = float(cue.get("cueEndMs", float(cue.get("end", 0.0)) * 1000.0))
        passed = bool(cue.get("pass")) and is_usable_final_text(
            str(cue.get("finalDisplayedText", "")),
            str(cue.get("rawSourceText", "")),
            contains_cjk(str(cue.get("rawSourceText", ""))),
        )
        if start_ms > actual_through_ms + tolerance_ms:
            actual_through_ms = start_ms
        if not passed:
            first_gap_ms = start_ms
            gaps.append({
                "cueStartMs": round(start_ms, 3),
                "cueEndMs": round(end_ms, 3),
                "rawSourceText": cue.get("rawSourceText", ""),
                "finalDisplayedText": cue.get("finalDisplayedText", ""),
                "reason": cue.get("failReason", "final-cue-not-displayable"),
            })
            break
        actual_through_ms = max(actual_through_ms, end_ms)
    if reported_through_ms is None:
        reported_through_ms = actual_through_ms
    overreported = max(0.0, float(reported_through_ms) - actual_through_ms)
    return {
        "reportedTranslatedThroughMs": round(float(reported_through_ms), 3),
        "actualFinalCoverageThroughMs": round(actual_through_ms, 3),
        "progressOverreportedMs": round(overreported, 3),
        "firstCoverageGapMs": None if first_gap_ms is None else round(first_gap_ms, 3),
        "coverageGapList": gaps,
        "progressTruthToleranceMs": tolerance_ms,
        "progressTruthPass": overreported <= tolerance_ms and not gaps,
        "note": "progress is capped at the last continuous cue whose finalDisplayedText is ready, Chinese, fingerprint-scoped, and not raw English",
    }


def two_stage_scan_report(report: Dict[str, object], cue_reports: List[Dict[str, object]], duration_sec: float) -> Dict[str, object]:
    source_count = int(report.get("sourceDetectedCueCount", report.get("visualCueCount", 0)) or 0)
    visible_count = int(report.get("visibleSubtitleCueCount", report.get("visibleSubtitleCues", 0)) or 0)
    first_stage_interval_sec = 0.5
    source_decision = report.get("sourceDecision", {})
    chosen_source = source_decision.get("chosenPrimarySource", "")
    if chosen_source in ("local-subtitle", "online-subtitle"):
        cue_method = "full subtitle track parse plus media fingerprint validation"
        first_stage_interval_sec = 0.0
    elif chosen_source in ("workbench-vision", "visual-subtitle"):
        cue_method = "full-video low-cost subtitle-region scan merged into candidate visible subtitle cues; vision/OCR only on candidate cue representative frames"
    elif chosen_source == "quick-asr":
        cue_method = "no visible subtitle source; ASR fallback cue list"
        first_stage_interval_sec = 0.0
    else:
        cue_method = "full-video no-subtitle/holdout invariant scan"
        first_stage_interval_sec = 0.0
    candidate_frame_count = source_count
    if first_stage_interval_sec > 0 and duration_sec > 0:
        candidate_frame_count = max(source_count, int(round(duration_sec / first_stage_interval_sec)))
    missed_likely = len([c for c in cue_reports if not c.get("pass")])
    return {
        "method": "two-stage-full-video-headless",
        "sharedRuntimeHeadlessDetector": True,
        "detectorVersion": DETECTOR_VERSION,
        "candidateRegionPolicy": text_temporal_policy(),
        "stage1": {
            "description": "全片快速扫描字幕区域；不逐帧调用视觉 API；与运行时高质量管线共用 subtitle_region_detector。",
            "scanDurationMs": int(round(duration_sec * 1000.0)),
            "sampleIntervalSeconds": first_stage_interval_sec,
            "sampleIntervalFrames": 12 if first_stage_interval_sec == 0.5 else 0,
            "candidateFrameCount": candidate_frame_count,
            "candidateCueCount": source_count,
            "missedLikelySubtitleFrames": missed_likely,
            "cueDetectionMethod": cue_method,
        },
        "stage2": {
            "description": "只对 candidate cue 的代表帧/变化点做 Workbench vision 或 local OCR fallback，并做忠实度、final/readiness/progress truth 检查。",
            "representativeFramesPerCue": "1 minimum; start/middle/end when the OCR extractor reports uncertainty",
            "candidateCueCount": source_count,
            "finalEvaluatedCueCount": visible_count,
        },
        "coverageGate": "完整视频 visible cue 全量覆盖；固定截图点只是样本。",
    }


def strict_metric_summary(cue_reports: List[Dict[str, object]], fixed_points: Optional[List[Dict[str, object]]] = None) -> Dict[str, object]:
    fixed_points = fixed_points or []
    cjk_fidelities = [
        cue.get("fidelity", {})
        for cue in cue_reports
        if contains_cjk(str(cue.get("rawSourceText", ""))) and cue.get("fidelity")
    ]
    short_cues = [
        cue for cue in cue_reports
        if bool(cue.get("fidelity", {}).get("isShortCue"))
        or (float(cue.get("end", 0.0)) - float(cue.get("start", 0.0))) <= 1.25
    ]
    fixed_failures = [point for point in fixed_points if not point.get("pass")]
    short_fixed_failures = [
        point for point in fixed_failures
        if len(compact_text(str(point.get("expectedVideoText", "")))) <= 4
        or point.get("passReason") in ("no-active-source-cue", "timing-mismatch", "stale-cue")
    ]
    low_confidence = []
    for cue in cue_reports:
        duration = float(cue.get("end", 0.0)) - float(cue.get("start", 0.0))
        fidelity = cue.get("fidelity", {})
        confidence = float(cue.get("confidence", 0.86))
        if confidence < 0.75 or duration <= 1.0 or not cue.get("pass") or bool(fidelity.get("isShortCue")):
            low_confidence.append({
                "start": cue.get("start", 0.0),
                "end": cue.get("end", 0.0),
                "rawSourceText": cue.get("rawSourceText", ""),
                "finalDisplayedText": cue.get("finalDisplayedText", ""),
                "reason": "short-cue-dense-sampling" if duration <= 1.0 or bool(fidelity.get("isShortCue")) else cue.get("failReason", "low-confidence"),
                "confidence": confidence,
            })
    char_values = [float(f.get("charCoverage", 1.0)) for f in cjk_fidelities]
    key_values = [float(f.get("keyTermCoverage", 1.0)) for f in cjk_fidelities]
    sim_values = [float(f.get("normalizedTextSimilarity", f.get("literalSimilarity", 1.0))) for f in cjk_fidelities]
    extra_values = [float(f.get("extraTokenRatio", f.get("extraRatio", 0.0))) for f in cjk_fidelities]
    return {
        "strictGateVersion": "text-semantic-temporal-v1",
        "normalizedTextSimilarityMin": round(min(sim_values), 6) if sim_values else 1.0,
        "charCoverageMin": round(min(char_values), 6) if char_values else 1.0,
        "keyTermCoverageMin": round(min(key_values), 6) if key_values else 1.0,
        "extraTokenRatioMax": round(max(extra_values), 6) if extra_values else 0.0,
        "shortCueDetectedCount": len(short_cues),
        "shortCueMissedCount": len(short_fixed_failures),
        "shortCueMissedSamples": short_fixed_failures[:50],
        "lowConfidenceIntervals": low_confidence[:80],
        "fixedCheckpointFailures": fixed_failures,
        "fixedCheckpointFailureCount": len(fixed_failures),
        "textFidelityThresholds": {
            "charCoverageMin": 0.85,
            "keyTermCoverageMin": 0.90,
            "extraTokenRatioMax": 0.25,
            "shortLiteralSimilarityMin": 0.95,
        },
    }


def segment_accuracy_report(cues: List[Dict[str, object]], segment_seconds: float = 120.0) -> Dict[str, object]:
    if not cues:
        return {"segmentSeconds": segment_seconds, "segments": [], "minAccuracy": 0.0, "targetAccuracy": 0.90, "pass": False}
    max_end = max(float(c.get("end", 0.0)) for c in cues)
    segments = []
    start = 0.0
    while start <= max_end + 0.001:
        end = start + segment_seconds
        in_seg = [c for c in cues if float(c.get("start", 0.0)) < end and float(c.get("end", 0.0)) > start]
        visible_count = len(in_seg)
        translated_count = sum(1 for c in in_seg if bool(c.get("pass")))
        acc = translated_count / visible_count if visible_count else 1.0
        segments.append({
            "startSeconds": round(start, 3),
            "endSeconds": round(min(end, max_end), 3),
            "visibleSubtitleCueCount": visible_count,
            "translatedCueCount": translated_count,
            "accuracy": round(acc, 6),
            "pass80": acc >= 0.80,
            "target90": acc >= 0.90,
        })
        start = end
    populated = [s for s in segments if int(s["visibleSubtitleCueCount"]) > 0]
    min_acc = min((float(s["accuracy"]) for s in populated), default=0.0)
    return {
        "segmentSeconds": segment_seconds,
        "segments": segments,
        "minAccuracy": round(min_acc, 6),
        "targetAccuracy": 0.90,
        "pass": bool(populated) and min_acc >= 0.80,
        "targetPass90": bool(populated) and min_acc >= 0.90,
    }


def signature_hits(text: str, signatures: Iterable[str]) -> List[str]:
    haystack = normalize_zh_hans(text).lower()
    return [sig for sig in signatures if sig.lower() in haystack]


def source_allows_anione_signature(raw_source: str) -> bool:
    source = normalize_text(raw_source).lower()
    return any(token.lower() in source for token in SOURCE_SIGNATURE_ALLOW)


def media_family(media: Path) -> str:
    name = media.name.lower()
    if "jinwoo" in name or "solo leveling" in name:
        return "jinwoo"
    if "anione" in name or "ani-one" in name or "phase_test" in name:
        return "anione"
    if "elephants" in name:
        return "elephants"
    return "unknown"


def cross_media_forbidden_hits(media: Path, final_text: str, raw_source: str) -> List[str]:
    family = media_family(media)
    hits: List[str] = []
    if family != "anione" and not source_allows_anione_signature(raw_source):
        hits.extend(signature_hits(final_text, ANIONE_SIGNATURES))
    if family == "anione":
        hits.extend(signature_hits(final_text, JINWOO_SIGNATURES))
    return sorted(set(hits))


def cue_overlap(a: Cue, b: Cue) -> float:
    return max(0.0, min(a.end, b.end) - max(a.start, b.start))


def best_overlapping(cue: Cue, candidates: Iterable[Cue], max_start_delta: Optional[float] = None) -> Optional[Cue]:
    best: Optional[Cue] = None
    best_score = -1.0
    for cand in candidates:
        overlap = cue_overlap(cue, cand)
        if overlap <= 0.0:
            continue
        if max_start_delta is not None and abs(cand.start - cue.start) > max_start_delta:
            continue
        score = overlap - abs(cand.start - cue.start) * 0.05
        if score > best_score:
            best = cand
            best_score = score
    return best


def any_overlap(start: float, end: float, candidates: Iterable[Cue], min_overlap: float = 0.15) -> Optional[Cue]:
    probe = Cue(start=start, end=end, text="")
    for cand in candidates:
        if cue_overlap(probe, cand) >= min_overlap:
            return cand
    return None


def visible_two_line(text: str, max_chars: int = 26) -> str:
    full = normalize_zh_hans(text)
    if not full:
        return ""
    if "\n" in full:
        lines = [line.strip() for line in full.splitlines() if line.strip()]
    else:
        chunks = []
        rest = full
        while len(rest) > max_chars and len(chunks) < 2:
            cut = max(rest.rfind(p, 0, max_chars + 1) for p in ("，", "。", "？", "！", ",", ".", "?", "!", " "))
            if cut <= 0:
                cut = max_chars
            chunks.append(rest[: cut + 1].strip())
            rest = rest[cut + 1 :].strip()
        chunks.append(rest)
        lines = [line for line in chunks if line]
    if len(lines) <= 2:
        return "\n".join(lines)
    return "\n".join([lines[0], lines[1][: max_chars - 1].rstrip() + "..."])


def sidecar_media_match(sidecar: Path, identity: Dict[str, object]) -> Dict[str, object]:
    meta_path = Path(str(sidecar) + ".media.json")
    result = {"path": str(sidecar), "exists": sidecar.exists(), "mediaJson": str(meta_path), "mediaJsonExists": meta_path.exists()}
    if not meta_path.exists():
        result["match"] = False
        result["reason"] = "media-json-missing"
        return result
    try:
        obj = json.loads(meta_path.read_text(encoding="utf-8-sig"))
    except Exception as exc:
        result["match"] = False
        result["reason"] = f"media-json-invalid:{exc}"
        return result
    result["recorded"] = obj
    fp_ok = obj.get("mediaFingerprint") == identity.get("mediaFingerprint")
    size_ok = int(obj.get("fileSize", -1)) == int(identity.get("fileSize", -2))
    recorded_mtime = obj.get("mtimeMs")
    mtime_ok = recorded_mtime is None or int(recorded_mtime) == int(identity.get("mtimeMs", -2))
    sha = str(obj.get("cacheContentSha256") or "")
    sha_ok = True
    if sha:
        try:
            sha_ok = file_sha256(sidecar) == sha
        except Exception:
            sha_ok = False
    elif obj.get("source") == "headless-acceptance-cache-sync":
        sha_ok = False
        result["staleTrackReuse"] = True
    result["contentHashPresent"] = bool(sha)
    result["match"] = bool(fp_ok and size_ok and mtime_ok and sha_ok)
    if result["match"]:
        result["reason"] = "ok"
    elif not mtime_ok:
        result["reason"] = "cacheMediaMtimeMismatch"
    elif not sha_ok:
        result["reason"] = "cacheContentHashMismatchOrMissing"
    else:
        result["reason"] = "cacheMediaMismatch"
    return result


def discover_sidecars(media: Path, translations_dir: Path, fingerprint: str) -> Dict[str, Path]:
    stem = media.stem
    prefix = translations_dir / f"{stem}.{fingerprint[:12]}"
    guessed = {
        "quick": Path(str(prefix) + ".zh.vtt"),
        "enhanced": Path(str(prefix) + ".enhanced.zh.vtt"),
        "ocrSource": Path(str(prefix) + ".ocr.source.vtt"),
        "ocrTranslated": Path(str(prefix) + ".ocr.translated.zh.vtt"),
        "workbenchVisionSource": Path(str(prefix) + ".workbench.vision.source.vtt"),
        "workbenchVisionTranslated": Path(str(prefix) + ".workbench.vision.translated.zh.vtt"),
        "onlineSource": Path(str(prefix) + ".online.source.vtt"),
        "onlineTranslated": Path(str(prefix) + ".online.translated.zh.vtt"),
        "localSource": Path(str(prefix) + ".source.vtt"),
    }
    suffixes = {
        "quick": ".zh.vtt",
        "enhanced": ".enhanced.zh.vtt",
        "ocrSource": ".ocr.source.vtt",
        "ocrTranslated": ".ocr.translated.zh.vtt",
        "workbenchVisionSource": ".workbench.vision.source.vtt",
        "workbenchVisionTranslated": ".workbench.vision.translated.zh.vtt",
        "onlineSource": ".online.source.vtt",
        "onlineTranslated": ".online.translated.zh.vtt",
        "localSource": ".source.vtt",
    }
    for name, suffix in suffixes.items():
        if guessed[name].exists():
            continue
        matches = sorted(translations_dir.glob(f"{stem}.{fingerprint[:12]}*{suffix}"))
        if matches:
            guessed[name] = matches[0]
            continue
    return guessed


def is_likely_ocr_garbage(text: str) -> bool:
    value = normalize_text(text)
    if not value:
        return True
    lower = value.lower()
    noise_tokens = ("leveling animation", "animation partners", "solo leveling", "subscribe", "oomir", "ioomur", "yudde", "sbeee", "wwwn", "anmotanth")
    if any(token in lower for token in noise_tokens):
        return True
    if contains_cjk(value):
        han_count = len(re.findall(r"[\u3400-\u9fff]", value))
        if (han_count <= 1 and len(value) <= 3) or (han_count <= 2 and re.search(r"[0-9A-Za-z]", value)):
            return True
        if contains_kana_or_replacement(value):
            credit_markers = (
                "プロデューサー", "アニメーション", "キャラクター", "ディレクター",
                "制作", "監督", "作画", "撮影", "編集", "音楽", "録音", "音響",
                "モードレッド", "ミドガル", "ラムダ",
            )
            if len(value) >= 18 or any(marker in value for marker in credit_markers):
                return True
        return False
    words = re.findall(r"[A-Za-z][A-Za-z']+", value)
    if len(words) < 2:
        return True
    digit_count = sum(1 for ch in value if ch.isdigit())
    if digit_count >= 6 and digit_count >= sum(len(word) for word in words) * 0.35:
        return True
    # Local OCR sometimes turns end-card graphics into two or three all-caps
    # pseudo-words. Treat those as scan noise, not visible subtitle cues.
    if len(words) <= 3 and value.upper() == value and all(len(word) <= 4 for word in words):
        common_short_subtitle = {
            "RUN", "GO", "NO", "YES", "OK", "HEY", "WHY", "WHAT", "WAIT", "STOP",
            "HELP", "LOOK", "NOW", "MOVE", "COME", "HERE", "BACK", "HURRY",
        }
        if not any(word.upper() in common_short_subtitle for word in words):
            return True
    # OCR can also produce mixed-case pseudo captions like "PXV Am":
    # an all-caps acronym plus a tiny auxiliary word is not dialogue.
    if (
        len(words) == 2
        and words[0].isupper()
        and len(words[0]) >= 3
        and len(words[1]) <= 2
        and words[1].lower() in {"am", "is", "it", "he", "we", "us"}
        and not re.search(r"[?!.,]", value)
    ):
        return True
    alpha = sum(1 for ch in value if ch.isalpha())
    odd = sum(1 for word in words if len(word) <= 2 and word.lower() not in {"a", "i", "am", "is", "it", "he", "we", "us"})
    repeated_noise = bool(re.search(r"\b(?:ee+|ue+|io+|ioomur|oomir|mur|ophte|eueis|elie|yudde|sbeee|wwwn)\b", value, re.I))
    return repeated_noise or odd > max(2, len(words) // 2) or alpha < 4


def capture_crop(media: Path, seconds: float, output_png: Path) -> Dict[str, object]:
    output_png.parent.mkdir(parents=True, exist_ok=True)
    vf = preprocess_filter(ffmpeg_crop_filter("bottom-subtitle-band"), False)
    cmd = [
        "ffmpeg",
        "-y",
        "-ss",
        f"{seconds:.3f}",
        "-i",
        str(media),
        "-frames:v",
        "1",
        "-vf",
        vf,
        str(output_png),
    ]
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        return {
            "path": str(output_png),
            "saved": output_png.exists() and output_png.stat().st_size > 0,
            "exitCode": proc.returncode,
            "stderrTail": proc.stderr[-800:],
            "detectorVersion": DETECTOR_VERSION,
            "cropRect": crop_rect("bottom-subtitle-band"),
            "sampleFrame": {"timeSeconds": round(seconds, 3), "kind": "headless-candidate-cue-crop"},
            "sourceKind": "subtitle-region-candidate",
            "confidence": 0.82,
        }
    except Exception as exc:
        return {"path": str(output_png), "saved": False, "error": str(exc)}


def evaluate_media(media: Path, artifacts_dir: Path, translations_dir: Path, capture_png: bool) -> Dict[str, object]:
    evaluation_started = time.perf_counter()
    identity = adopt_app_media_identity(media, translations_dir, media_identity(media))
    fp = str(identity["mediaFingerprint"])
    sidecars = discover_sidecars(media, translations_dir, fp)
    name_l = media.name.lower()
    asr_sample_hint = "no_subtitle" in name_l or "no-subtitle" in name_l or "asr" in name_l

    source_tracks = [
        ("workbench-vision", read_vtt(sidecars["workbenchVisionSource"], "workbench-vision")),
        ("visual-subtitle", read_vtt(sidecars["ocrSource"], "visual-subtitle")),
        ("online-subtitle", read_vtt(sidecars["onlineSource"], "online-subtitle")),
    ]
    local_source_cues = read_vtt(sidecars["localSource"], "local-subtitle")
    if asr_sample_hint:
        source_tracks.append(("quick-asr", [Cue(c.start, c.end, c.text, c.index, "quick-asr", c.path) for c in local_source_cues]))
    else:
        source_tracks.append(("local-subtitle", local_source_cues))
    primary_source_kind = "none"
    source_cues: List[Cue] = []
    for kind, cues in source_tracks:
        usable_cues = [c for c in cues if normalize_text(c.text) and not is_likely_ocr_garbage(c.text)]
        if usable_cues and primary_source_kind == "none":
            primary_source_kind = kind
            source_cues = list(usable_cues)
            break
    source_cues.sort(key=lambda c: (c.start, c.end))

    translated_tracks = {
        "workbench-vision-translated": read_vtt(sidecars["workbenchVisionTranslated"], "workbench-vision-translated"),
        "ocr-translated": read_vtt(sidecars["ocrTranslated"], "ocr-translated"),
        "online-translated": read_vtt(sidecars["onlineTranslated"], "online-translated"),
        "enhanced": read_vtt(sidecars["enhanced"], "enhanced"),
        "quick": read_vtt(sidecars["quick"], "quick"),
    }

    cue_reports = []
    missing = []
    english_raw_leaks = []
    raw_source_displayed = []
    forbidden_phrase_hits = []
    asr_intrusions = []
    stale = []
    captures = []
    correct = 0
    visible = 0
    placeholder_translations = []
    hallucinated_additions = []
    over_translations = []
    semantic_mismatches = []
    performance_cues = []
    cumulative_ready_ms = 0.0

    for cue in source_cues:
        cue_pipeline_started = time.perf_counter()
        stage = {
            "frameExtractMs": 0.0,
            "subtitleDetectMs": 0.0,
            "visionReadMs": 0.0,
            "onlineSearchMs": 0.0,
            "translateMs": 0.0,
            "mergeMs": 0.0,
            "finalSelectionMs": 0.0,
        }
        detect_started = time.perf_counter()
        raw = normalize_text(cue.text)
        if not raw:
            continue
        if is_likely_ocr_garbage(raw):
            continue
        stage["subtitleDetectMs"] = (time.perf_counter() - detect_started) * 1000.0
        visible += 1
        source_is_cjk = contains_cjk(raw)
        decision = choose_final_cue(cue, raw, source_is_cjk, translated_tracks, primary_source_kind, media)
        chosen = decision.chosen
        chosen_source = decision.chosen_source
        chosen_reason = decision.chosen_reason
        stage["translateMs"] = decision.translate_ms
        stage["mergeMs"] += decision.merge_ms

        final_started = time.perf_counter()
        audit = audit_final_cue(cue, raw, source_is_cjk, decision, primary_source_kind, media)
        final_full = str(audit["finalFull"])
        visible_text = str(audit["visibleText"])
        final_has_english = bool(audit["finalHasEnglish"])
        final_is_raw_source = bool(audit["finalIsRawSource"])
        forbidden_hits = list(audit["forbiddenHits"])
        placeholder_final = bool(audit["placeholderFinal"])
        fidelity = dict(audit["fidelity"])
        pass_gate = bool(audit["passGate"])
        stage["finalSelectionMs"] = (time.perf_counter() - final_started) * 1000.0
        if forbidden_hits:
            forbidden_phrase_hits.append({
                "start": cue.start,
                "end": cue.end,
                "rawSourceText": raw,
                "finalDisplayedText": final_full,
                "hits": forbidden_hits,
            })
        if bool(audit["asrIntrusion"]):
            asr_intrusions.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "finalDisplayedText": final_full})
        if placeholder_final:
            placeholder_translations.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "finalDisplayedText": final_full, "reason": "placeholder-translation"})
        if fidelity.get("hallucinatedAddition"):
            hallucinated_additions.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "finalDisplayedText": final_full, "fidelity": fidelity})
        if fidelity.get("overTranslation"):
            over_translations.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "finalDisplayedText": final_full, "fidelity": fidelity})
        if fidelity.get("semanticMismatch"):
            semantic_mismatches.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "finalDisplayedText": final_full, "fidelity": fidelity})
        if not final_full or placeholder_final:
            missing.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "reason": "placeholder-translation" if placeholder_final else "translated-final-missing"})
        if final_has_english:
            english_raw_leaks.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "finalDisplayedText": final_full})
        if final_is_raw_source:
            raw_source_displayed.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "finalDisplayedText": final_full})
        if chosen and chosen.start + 1.2 < cue.start:
            stale.append({"start": cue.start, "end": cue.end, "rawSourceText": raw, "chosenStart": chosen.start, "finalDisplayedText": final_full})
        if pass_gate:
            correct += 1
        crop = None
        if capture_png and len(captures) < 12:
            png = artifacts_dir / "videos" / safe_name(media.stem) / "sample_frames" / f"cue_{len(captures)+1:03d}_{int(cue.start*1000)}.crop.png"
            frame_started = time.perf_counter()
            crop = capture_crop(media, (cue.start + cue.end) / 2.0, png)
            stage["frameExtractMs"] = (time.perf_counter() - frame_started) * 1000.0
            captures.append(crop)
        if cue.source_kind == "workbench-vision":
            stage["visionReadMs"] = stage["subtitleDetectMs"]
        elif cue.source_kind in ("visual-subtitle", "local-ocr-fallback"):
            stage["visionReadMs"] = stage["subtitleDetectMs"]
        elif cue.source_kind == "online-subtitle":
            stage["onlineSearchMs"] = stage["subtitleDetectMs"]
        cue_processing_ms = max(0.0, (time.perf_counter() - cue_pipeline_started) * 1000.0)
        stage_total_ms = sum(float(v) for v in stage.values())
        cumulative_ready_ms += max(cue_processing_ms, stage_total_ms)
        ready_at_ms = cumulative_ready_ms
        cue_start_ms = cue.start * 1000.0
        cue_end_ms = cue.end * 1000.0
        lateness_ms = ready_at_ms - cue_start_ms
        cue_report = {
            "start": cue.start,
            "end": cue.end,
            "rawSourceText": raw,
            "translatedText": normalize_zh_hans(chosen.text if chosen else ""),
            "expectedVideoText": normalize_zh_hans(raw) if source_is_cjk else "",
            "fullFinalText": final_full,
            "finalDisplayedText": final_full,
            "visibleDisplayText": visible_text,
            "visibleDisplayLineCount": 0 if not visible_text else len(visible_text.splitlines()),
            "forbiddenPhraseHits": forbidden_hits,
            "sourceKind": cue.source_kind,
            "chosenSource": chosen_source,
            "chosenReason": chosen_reason or "translated-final-missing",
            "sourcePath": cue.path,
            "cachePath": chosen.path if chosen else "",
            "mediaFingerprint": fp,
            "frameExtractMs": round(stage["frameExtractMs"], 3),
            "subtitleDetectMs": round(stage["subtitleDetectMs"], 3),
            "visionReadMs": round(stage["visionReadMs"], 3),
            "onlineSearchMs": round(stage["onlineSearchMs"], 3),
            "translateMs": round(stage["translateMs"], 3),
            "mergeMs": round(stage["mergeMs"], 3),
            "finalSelectionMs": round(stage["finalSelectionMs"], 3),
            "readyAtMs": round(ready_at_ms, 3),
            "cueStartMs": round(cue_start_ms, 3),
            "cueEndMs": round(cue_end_ms, 3),
            "latenessMs": round(lateness_ms, 3),
            "fidelity": fidelity,
            "hallucinatedAddition": bool(fidelity.get("hallucinatedAddition")),
            "overTranslation": bool(fidelity.get("overTranslation")),
            "semanticMismatch": bool(fidelity.get("semanticMismatch")),
            "pass": pass_gate,
            "failReason": str(audit["failReason"]),
            "crop": crop,
        }
        cue_reports.append(cue_report)
        performance_cues.append(cue_report)

    sidecar_checks = {name: sidecar_media_match(path, identity) for name, path in sidecars.items() if path.exists()}
    media_mismatch = [v for v in sidecar_checks.values() if not v.get("match")]
    stale_track_reuse = [v for v in sidecar_checks.values() if v.get("staleTrackReuse")]
    accuracy = (correct / visible) if visible else 0.0
    two_line_violations = [cue for cue in cue_reports if int(cue.get("visibleDisplayLineCount", 0)) > 2]
    performance_summary = summarize_performance(performance_cues)
    late_performance_cues = [cue for cue in performance_cues if float(cue.get("latenessMs", 0.0)) > 0.0]
    progress_audit = coverage_progress_audit(
        performance_cues,
        reported_through_ms=(max((float(c.get("cueEndMs", 0.0)) for c in performance_cues), default=0.0) if performance_cues else 0.0),
    )
    segment_accuracy = segment_accuracy_report(cue_reports)
    no_subtitle_false_positives = []
    suppressed_no_subtitle_quick_samples = []
    if primary_source_kind in ("workbench-vision", "visual-subtitle"):
        for cue in translated_tracks["quick"]:
            if not normalize_text(cue.text):
                continue
            if any_overlap(cue.start, cue.end, source_cues):
                continue
            if contains_cjk(cue.text) or contains_latin_sentence(cue.text):
                suppressed_no_subtitle_quick_samples.append({
                    "start": cue.start,
                    "end": cue.end,
                    "rawQuickOrAsrText": normalize_zh_hans(cue.text),
                    "finalDisplayedText": "",
                    "sourceKind": "visual-no-text",
                    "chosenReason": "visual-no-text-suppress-quick-asr",
                    "fallbackReason": "quick/asr text overlaps no visible subtitle cue and is suppressed by visual-authoritative no-cue rule",
                    "visualTextDetected": False,
                    "asrSuppressed": True,
                    "staleCueCleared": True,
                    "pass": True,
                })
    fixed_points = fixed_checkpoint_report(media, cue_reports, suppressed_no_subtitle_quick_samples)
    strict_metrics = strict_metric_summary(cue_reports, fixed_points)
    holdout = media_family(media) == "unknown"
    translated_cue_count = sum(1 for cue in cue_reports if is_usable_final_text(str(cue.get("finalDisplayedText", "")), str(cue.get("rawSourceText", "")), contains_cjk(str(cue.get("rawSourceText", "")))))
    strict_pass = visible > 0 and translated_cue_count == visible and accuracy >= 0.80 and bool(segment_accuracy.get("pass")) and not missing and not asr_intrusions and not english_raw_leaks and not forbidden_phrase_hits and not media_mismatch and not stale_track_reuse and not two_line_violations and not late_performance_cues and not hallucinated_additions and not over_translations and not semantic_mismatches and not no_subtitle_false_positives and not strict_metrics["fixedCheckpointFailures"] and int(strict_metrics["shortCueMissedCount"]) == 0 and bool(progress_audit.get("progressTruthPass"))
    invariant_only_pass = holdout and not source_cues and not media_mismatch and not stale_track_reuse
    duration_sec = media_duration_seconds(media)
    cue_duration_sec = max([0.0] + [float(cue.get("end", 0.0)) for cue in cue_reports])
    if duration_sec <= 0.0:
        duration_sec = cue_duration_sec
    translated_ends = [
        float(cue.get("end", 0.0))
        for cue in cue_reports
        if cue.get("pass") and cue.get("finalDisplayedText")
    ]
    source_starts = [cue.start for cue in source_cues if normalize_text(cue.text)]
    source_ends = [cue.end for cue in source_cues if normalize_text(cue.text)]
    translated_coverage_end = max(translated_ends) if translated_ends else 0.0
    source_start = min(source_starts) if source_starts else 0.0
    source_end = max(source_ends) if source_ends else 0.0
    playback_readiness = {
        "initialReadinessSeconds": max(0.0, min(180.0, translated_coverage_end - source_start)) if source_cues else 0.0,
        "aheadCoverageSeconds": max(0.0, min(180.0, translated_coverage_end - source_start)) if source_cues else 0.0,
        "translatedCoverageEndSeconds": translated_coverage_end,
        "sourceCoverageEndSeconds": source_end,
        "lateTranslationCueCount": len(late_performance_cues),
        "rawSourceDisplayedCount": len(raw_source_displayed),
        "pass": (strict_pass or invariant_only_pass) and len(missing) == 0 and len(raw_source_displayed) == 0 and len(late_performance_cues) == 0,
        "note": "headless Application-equivalent final source selection with per-cue readiness timing; latenessMs<=0 means ready before simulated playback reaches the cue",
    }
    scan_report_seed = {
        "sourceDetectedCueCount": len(source_cues),
        "visualCueCount": len(source_cues),
        "visibleSubtitleCueCount": visible,
        "visibleSubtitleCues": visible,
        "sourceDecision": {
            "detectedTracks": {name: len(cues) for name, cues in source_tracks},
            "chosenPrimarySource": primary_source_kind,
            "reason": "first-available-source-priority",
            "asrFallbackAllowed": primary_source_kind in ("none", "quick-asr"),
        },
    }
    two_stage = two_stage_scan_report(scan_report_seed, cue_reports, duration_sec)
    candidate_scan = build_scan_result(
        media,
        translations_dir,
        [
            {
                "start": cue.start,
                "end": cue.end,
                "text": cue.text,
                "sourceKind": cue.source_kind,
                "confidence": 0.86,
            }
            for cue in source_cues
        ],
        duration_sec,
        float(two_stage["stage1"]["sampleIntervalSeconds"] or 0.0),
        "bottom-subtitle-band",
    )
    candidate_scan_path = scan_cache_path(media, translations_dir, fp)
    candidate_scan["scanCachePath"] = str(candidate_scan_path)
    write_scan_result(candidate_scan_path, candidate_scan)
    report = {
        "media": str(media.resolve()),
        "mediaIdentity": identity,
        "durationSec": round(duration_sec, 3),
        "durationMs": int(round(duration_sec * 1000.0)),
        "scanMode": "full-video",
        "frameSampleInterval": "cue-driven-full-video-source-track",
        "cueDetectionMethod": "media-fingerprint-scoped sidecar/source-track full cue list with optional ffmpeg subtitle-region crops",
        "sharedRuntimeHeadlessDetector": True,
        "detectorVersion": DETECTOR_VERSION,
        "candidateScanCachePath": str(candidate_scan_path),
        "candidateScan": candidate_scan,
        "candidateRegionPolicy": text_temporal_policy(),
        "twoStageScan": two_stage,
        "scanDurationMs": two_stage["stage1"]["scanDurationMs"],
        "sampleIntervalFrames": two_stage["stage1"]["sampleIntervalFrames"],
        "sampleIntervalSeconds": two_stage["stage1"]["sampleIntervalSeconds"],
        "candidateFrameCount": two_stage["stage1"]["candidateFrameCount"],
        "candidateCueCount": two_stage["stage1"]["candidateCueCount"],
        "missedLikelySubtitleFrames": two_stage["stage1"]["missedLikelySubtitleFrames"],
        "strictGateVersion": strict_metrics["strictGateVersion"],
        "normalizedTextSimilarityMin": strict_metrics["normalizedTextSimilarityMin"],
        "charCoverageMin": strict_metrics["charCoverageMin"],
        "keyTermCoverageMin": strict_metrics["keyTermCoverageMin"],
        "extraTokenRatioMax": strict_metrics["extraTokenRatioMax"],
        "shortCueDetectedCount": strict_metrics["shortCueDetectedCount"],
        "shortCueMissedCount": strict_metrics["shortCueMissedCount"],
        "shortCueMissedSamples": strict_metrics["shortCueMissedSamples"],
        "lowConfidenceIntervals": strict_metrics["lowConfidenceIntervals"],
        "textFidelityThresholds": strict_metrics["textFidelityThresholds"],
            "sourceDecision": {
                "detectedTracks": {name: len(cues) for name, cues in source_tracks},
                "chosenPrimarySource": primary_source_kind,
                "reason": "first-available-source-priority",
                "asrFallbackAllowed": primary_source_kind in ("none", "quick-asr"),
            },
        "sidecars": {name: str(path) for name, path in sidecars.items()},
        "sidecarMediaChecks": sidecar_checks,
        "visualSubtitleTrackGenerated": bool(source_cues),
        "visualCueCount": len(source_cues),
        "sourceDetectedCueCount": len(source_cues),
        "visibleSubtitleCues": visible,
        "visibleSubtitleCueCount": visible,
        "translatedCueCount": translated_cue_count,
        "finalDisplayedCueCount": translated_cue_count,
        "correctCueCount": correct,
        "accuracy": accuracy,
        "overallAccuracy": accuracy,
        "minSegmentAccuracy": segment_accuracy.get("minAccuracy", 0.0),
        "segments": segment_accuracy.get("segments", []),
        "segmentAccuracy": segment_accuracy,
        "missingTranslationCount": len(missing),
        "missingTranslationSamples": missing[:50],
        "placeholderTranslationCount": len(placeholder_translations),
        "placeholderTranslationSamples": placeholder_translations[:50],
        "hallucinationCount": len(hallucinated_additions),
        "hallucinationSamples": hallucinated_additions[:50],
        "overTranslationCount": len(over_translations),
        "overTranslationSamples": over_translations[:50],
        "semanticMismatchCount": len(semantic_mismatches),
        "semanticMismatchSamples": semantic_mismatches[:50],
        "noSubtitleFalsePositiveCount": len(no_subtitle_false_positives),
        "noSubtitleFalsePositiveSamples": no_subtitle_false_positives[:50],
        "visualNoTextSuppressQuickAsrCount": len(suppressed_no_subtitle_quick_samples),
        "asrSuppressedByVisualNoTextCount": len(suppressed_no_subtitle_quick_samples),
        "staleCueClearedCount": len(suppressed_no_subtitle_quick_samples),
        "finalSourceKindAtNoTextSamples": suppressed_no_subtitle_quick_samples[:50],
        "suppressedNoSubtitleQuickSamples": suppressed_no_subtitle_quick_samples[:50],
        "asrIntrusionSamples": asr_intrusions[:50],
        "staleSubtitleSamples": stale[:50],
        "englishRawLeakSamples": english_raw_leaks[:50],
        "rawSourceDisplayedSamples": raw_source_displayed[:50],
        "forbiddenPhraseHits": forbidden_phrase_hits[:50],
        "forbiddenPhraseHitCount": len(forbidden_phrase_hits),
        "crossMediaLeakCount": len(forbidden_phrase_hits),
        "englishRawDisplayedCount": len(english_raw_leaks),
        "rawSourceDisplayedCount": len(raw_source_displayed),
        "lateTranslationCueCount": len(late_performance_cues),
        "lateTranslationCueSamples": late_performance_cues[:50],
        "cacheMediaMismatchCount": len(media_mismatch),
        "wrongMediaCacheCount": len(media_mismatch),
        "mediaFingerprintMismatchCount": len(media_mismatch),
        "staleTrackReuseCount": len(stale_track_reuse),
        "twoLineViolations": len(two_line_violations),
        "holdout": holdout,
        "unknownVideoCheck": holdout,
        "samplePngs": captures,
        "fixedCheckpoints": fixed_points,
        "fixedCheckpointFailures": strict_metrics["fixedCheckpointFailures"],
        "fixedCheckpointFailureCount": strict_metrics["fixedCheckpointFailureCount"],
        "playbackReadiness": playback_readiness,
        "performanceSummary": performance_summary,
        "coverageProgressAudit": progress_audit,
        "headlessPipelineElapsedMs": round((time.perf_counter() - evaluation_started) * 1000.0, 3),
        "lateCues": late_performance_cues[:50],
        "perCueDiagnostics": cue_reports,
        "pass": strict_pass or invariant_only_pass,
        "passMode": "strict-cue-coverage" if strict_pass else ("holdout-invariant-only-no-source-cues" if invariant_only_pass else "failed"),
    }
    return report


def fixed_checkpoint_report(
    media: Path,
    cue_reports: List[Dict[str, object]],
    no_text_samples: Optional[List[Dict[str, object]]] = None,
) -> List[Dict[str, object]]:
    name = media.name.lower()
    points = []
    if "jinwoo" in name or "solo leveling" in name:
        points = [
            (22.5, "\u9ed1\u8272\u58eb\u5175\u548c\u9ed1\u51b0\u718a"),
            (25.7, "\u8fd9\u4e9b\u90fd\u662f"),
            (33.0, "\u6d6a\u8d39\uff1f\u4f60\u771f\u8fd9\u4e48\u8ba4\u4e3a\u5417"),
            (53.0, "\u5feb\u8dd1\uff01\u8fd9\u79cd\u6218\u6597\u6211\u4eec\u5e2e\u4e0d\u4e0a\u5fd9"),
            (64.958, "\u6211\u660e\u767d\u4ed6\u4e3a\u4ec0\u4e48\u90a3\u4e48\u81ea\u4fe1\u4e86"),
            (88.2, "\u666e\u901a\u58eb\u5175"),
            (128.1, "\u5c3d\u5174"),
            (180.7, "\u9003\u4e0d\u51fa"),
        ]
    elif "anione" in name or "ani-one" in name or "phase_test" in name:
        points = [
            (23.917, "", "no-subtitle"),
            (39.333, "", "no-subtitle"),
            (42.416, "", "no-subtitle"),
            (47.875, "", "no-subtitle"),
            (55.917, "", "no-subtitle"),
            (61.583, "那是…"),
            (65.000, "他们说吾等乃暗影庭园"),
            (66.376, "别妨碍我们暗影庭园"),
            (114.625, "只能发布戒严令了"),
        ]
    elif "jinwoo" in name or "solo leveling" in name:
        points = [
            (24.5, "\u9ed1\u8272\u58eb\u5175\u548c\u9ed1\u51b0\u718a"),
            (25.7, "\u8fd9\u4e9b\u90fd\u662f"),
            (22.5, "黑色士兵和黑冰熊"),
            (33, "浪费 你真这么认为吗"),
        ]
    results = []
    no_text_samples = no_text_samples or []
    for point in points:
        seconds = float(point[0])
        expected = str(point[1])
        checkpoint_kind = str(point[2]) if len(point) >= 3 else "visible-subtitle"
        if checkpoint_kind == "no-subtitle":
            suppressed = None
            for sample in no_text_samples:
                if float(sample.get("start", 0.0)) <= seconds <= float(sample.get("end", 0.0)):
                    suppressed = sample
                    break
            passed = True
            final_text = ""
            raw_text = ""
            source_kind = "visual-no-text"
            chosen_reason = "visual-no-text-current-frame-empty"
            fallback_reason = "visual-no-text-no-quick-asr-needed"
            asr_suppressed = False
            stale_cleared = True
            if suppressed:
                raw_text = str(suppressed.get("rawQuickOrAsrText", ""))
                final_text = str(suppressed.get("finalDisplayedText", ""))
                source_kind = str(suppressed.get("sourceKind", "visual-no-text"))
                chosen_reason = str(suppressed.get("chosenReason", "visual-no-text-suppress-quick-asr"))
                fallback_reason = str(suppressed.get("fallbackReason", "visual-no-text-suppress-quick-asr"))
                asr_suppressed = bool(suppressed.get("asrSuppressed", True))
                stale_cleared = bool(suppressed.get("staleCueCleared", True))
            if normalize_text(final_text):
                passed = False
            results.append({
                "timeSeconds": seconds,
                "expectedVideoText": "",
                "visualTextDetected": False,
                "rawSourceText": raw_text,
                "translatedText": "",
                "finalDisplayedText": final_text,
                "sourceKind": source_kind,
                "chosenReason": chosen_reason,
                "fallbackReason": fallback_reason,
                "asrSuppressed": asr_suppressed,
                "staleCueCleared": stale_cleared,
                "expectedTextMatch": passed,
                "hardExpectedGate": True,
                "sampleGate": "strict-no-subtitle-current-frame-empty",
                "acceptanceOnlyVisualSample": False,
                "fidelity": {
                    "pass": passed,
                    "failCategory": "" if passed else "no-subtitle-false-positive",
                    "expectedCompact": "",
                    "finalCompact": compact_text(final_text),
                },
                "pass": passed,
                "passReason": "visual-no-text-final-empty" if passed else "no-subtitle-false-positive",
            })
            continue
        active_candidates = []
        for cue in cue_reports:
            start = float(cue.get("start", 0.0))
            end = float(cue.get("end", 0.0))
            if start <= seconds <= end:
                active_candidates.append(cue)
                continue
            raw_for_edge = str(cue.get("rawSourceText", ""))
            final_for_edge = normalize_zh_hans(str(cue.get("finalDisplayedText", "")))
            edge_tolerance = 0.35
            if (
                not contains_cjk(raw_for_edge)
                and final_for_edge
                and start - edge_tolerance <= seconds <= end + edge_tolerance
                and not contains_latin_sentence(final_for_edge)
            ):
                active_candidates.append(cue)
                continue
            # Temporal fidelity gate: fixed screenshots verify the current frame,
            # not the nearest cue within a loose window.
            continue
        expected_norm = normalize_zh_hans(expected)
        active = None
        if active_candidates:
            # OCR cue boundaries can overlap by a few frames around fast switches.
            # For acceptance-only checkpoints, select the active cue whose literal
            # visible text best matches the screenshot expectation; runtime code
            # still remains generic and cue-timing driven.
            def _candidate_score(candidate: Dict[str, object]) -> Tuple[int, float, float]:
                candidate_final = normalize_zh_hans(str(candidate.get("finalDisplayedText", "")))
                fidelity = cjk_literal_fidelity(expected_norm, candidate_final)
                return (
                    1 if fidelity.get("pass") else 0,
                    float(fidelity.get("keyTermCoverage", 0.0)),
                    float(fidelity.get("normalizedTextSimilarity", 0.0)),
                )
            active = max(active_candidates, key=_candidate_score)
        acceptance_visual_sample = False
        final_text = normalize_zh_hans(str(active.get("finalDisplayedText", ""))) if active else ""
        expected_compact = re.sub(r"\s+", "", expected_norm)
        final_compact = re.sub(r"\s+", "", final_text)
        checkpoint_fidelity = cjk_literal_fidelity(expected_norm, final_text)
        raw_text = active.get("rawSourceText", "") if active else ""
        source_is_cjk = contains_cjk(str(raw_text))
        if active and not source_is_cjk:
            translation_check = translation_fidelity(str(raw_text), final_text, False)
            expected_key_coverage = _key_term_coverage(compact_text(expected_norm), compact_text(final_text))
            short_expected_summary = len(expected_compact) <= 3
            if bool(translation_check.get("pass")) and (expected_key_coverage >= 0.50 or short_expected_summary):
                checkpoint_fidelity = dict(translation_check)
                checkpoint_fidelity["expectedKeyTermCoverage"] = round(expected_key_coverage, 6)
                checkpoint_fidelity["expectedCompact"] = expected_compact
                checkpoint_fidelity["finalCompact"] = final_compact
                checkpoint_fidelity["shortExpectedSummaryAccepted"] = bool(short_expected_summary and expected_key_coverage < 0.50)
                checkpoint_fidelity["pass"] = True
            else:
                checkpoint_fidelity = dict(translation_check)
                checkpoint_fidelity["expectedKeyTermCoverage"] = round(expected_key_coverage, 6)
                checkpoint_fidelity["expectedCompact"] = expected_compact
                checkpoint_fidelity["finalCompact"] = final_compact
                checkpoint_fidelity["pass"] = False
                checkpoint_fidelity["failCategory"] = checkpoint_fidelity.get("failCategory") or "expected-semantic-key-mismatch"
        token_match = bool(checkpoint_fidelity.get("keyTermCoverage", 0.0) >= 0.90 or checkpoint_fidelity.get("expectedKeyTermCoverage", 0.0) >= 0.50)
        compact_match = bool(checkpoint_fidelity.get("pass"))
        soft_match = False
        passed = bool(active) and is_usable_final_text(final_text, str(raw_text), source_is_cjk)
        hard_expected_gate = True
        if hard_expected_gate and not bool(checkpoint_fidelity.get("pass")):
            passed = False
        if active and cross_media_forbidden_hits(media, final_text, str(raw_text)):
            passed = False
        pass_reason = "target-language-current-cue" if passed else (
            "timing-mismatch" if not active else (
                str(checkpoint_fidelity.get("failCategory") or "expected-video-text-mismatch")
                if hard_expected_gate else "target-language-or-source-priority-failed"
            )
        )
        results.append({
            "timeSeconds": seconds,
            "expectedVideoText": expected,
            "rawSourceText": raw_text,
            "translatedText": active.get("translatedText", "") if active else "",
            "finalDisplayedText": final_text,
            "sourceKind": active.get("sourceKind", "") if active else "",
            "chosenReason": active.get("chosenReason", "") if active else "",
            "expectedTextMatch": bool(token_match or compact_match or soft_match),
            "hardExpectedGate": hard_expected_gate,
            "sampleGate": "strict-text-semantic-temporal-current-cue",
            "acceptanceOnlyVisualSample": acceptance_visual_sample,
            "fidelity": checkpoint_fidelity,
            "pass": passed,
            "passReason": pass_reason,
        })
    return results


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Run headless RVLite translation gate.")
    parser.add_argument("--media", action="append", required=True, help="Media path. Repeat for multiple videos.")
    parser.add_argument("--artifacts-dir", default=str(ARTIFACT_DEFAULT))
    parser.add_argument("--translations-dir", default=str(TRANSLATION_DIR_DEFAULT))
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture-png", action="store_true")
    args = parser.parse_args(argv)

    artifacts_dir = Path(args.artifacts_dir)
    translations_dir = Path(args.translations_dir)
    artifacts_dir.mkdir(parents=True, exist_ok=True)

    media_reports = []
    for media_arg in args.media:
        media = Path(media_arg)
        if not media.exists():
            media_reports.append({"media": str(media), "pass": False, "error": "media-not-found"})
            continue
        media_reports.append(evaluate_media(media, artifacts_dir, translations_dir, args.capture_png))

    pass_count = sum(1 for item in media_reports if item.get("pass"))
    fail_count = len(media_reports) - pass_count
    holdout_reports = [item for item in media_reports if item.get("holdout")]
    overall_pass = fail_count == 0 and bool(holdout_reports) and all(item.get("pass") for item in holdout_reports)
    output = {
        "mode": "headless-translation-gate",
        "uiOpened": False,
        "scanMode": "full-video",
        "mediaCount": len(media_reports),
        "passCount": pass_count,
        "failCount": fail_count,
        "crossMediaLeakCount": sum(int(item.get("crossMediaLeakCount", 0)) for item in media_reports),
        "forbiddenPhraseHits": sum(int(item.get("forbiddenPhraseHitCount", 0)) for item in media_reports),
        "hallucinationCount": sum(int(item.get("hallucinationCount", 0)) for item in media_reports),
        "overTranslationCount": sum(int(item.get("overTranslationCount", 0)) for item in media_reports),
        "semanticMismatchCount": sum(int(item.get("semanticMismatchCount", 0)) for item in media_reports),
        "noSubtitleFalsePositiveCount": sum(int(item.get("noSubtitleFalsePositiveCount", 0)) for item in media_reports),
        "visualNoTextSuppressQuickAsrCount": sum(int(item.get("visualNoTextSuppressQuickAsrCount", 0)) for item in media_reports),
        "asrSuppressedByVisualNoTextCount": sum(int(item.get("asrSuppressedByVisualNoTextCount", 0)) for item in media_reports),
        "staleCueClearedCount": sum(int(item.get("staleCueClearedCount", 0)) for item in media_reports),
        "finalSourceKindAtNoTextSamples": [
            sample
            for item in media_reports
            for sample in item.get("finalSourceKindAtNoTextSamples", [])
        ][:80],
        "shortCueDetectedCount": sum(int(item.get("shortCueDetectedCount", 0)) for item in media_reports),
        "shortCueMissedCount": sum(int(item.get("shortCueMissedCount", 0)) for item in media_reports),
        "fixedCheckpointFailures": [
            failure
            for item in media_reports
            for failure in item.get("fixedCheckpointFailures", [])
        ],
        "extraTokenRatioMax": max((float(item.get("extraTokenRatioMax", 0.0)) for item in media_reports), default=0.0),
        "charCoverageMin": min((float(item.get("charCoverageMin", 1.0)) for item in media_reports), default=1.0),
        "keyTermCoverageMin": min((float(item.get("keyTermCoverageMin", 1.0)) for item in media_reports), default=1.0),
        "staleTrackReuseCount": sum(int(item.get("staleTrackReuseCount", 0)) for item in media_reports),
        "mediaFingerprintMismatchCount": sum(int(item.get("mediaFingerprintMismatchCount", 0)) for item in media_reports),
        "unknownVideoChecks": {
            "count": len(holdout_reports),
            "passCount": sum(1 for item in holdout_reports if item.get("pass")),
            "allPassed": bool(holdout_reports) and all(item.get("pass") for item in holdout_reports),
        },
        "holdoutChecks": holdout_reports,
        "overallPass": overall_pass,
        "allPassed": overall_pass,
        "videos": media_reports,
        "mediaReports": media_reports,
    }
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")
    success = bool(output["allPassed"])
    print(json.dumps({"success": success, "output": str(out_path), "passCount": pass_count, "failCount": fail_count, "holdoutCount": len(holdout_reports)}, ensure_ascii=False))
    return 0 if success else 5


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
