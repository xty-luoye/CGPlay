#!/usr/bin/env python3
import argparse
import difflib
import json
import re
from pathlib import Path


def _parse_time(value):
    value = value.strip().replace(",", ".")
    hh, mm, ss = value.split(":")
    return int(hh) * 3600 + int(mm) * 60 + float(ss)


def _format_time(seconds):
    ms = max(0, int(round(float(seconds) * 1000)))
    h = ms // 3600000
    m = (ms // 60000) % 60
    s = (ms // 1000) % 60
    r = ms % 1000
    return f"{h:02d}:{m:02d}:{s:02d}.{r:03d}"


def _read_subtitle(path):
    if not path or not Path(path).exists():
        return []
    text = Path(path).read_text(encoding="utf-8-sig", errors="replace").replace("\r\n", "\n")
    pattern = re.compile(
        r"(\d{2}:\d{2}:\d{2}[,.]\d{3})\s*-->\s*(\d{2}:\d{2}:\d{2}[,.]\d{3}).*?\n(.+?)(?=\n\s*\n|\Z)",
        re.S,
    )
    cues = []
    for match in pattern.finditer(text):
        body = " ".join(
            line.strip()
            for line in match.group(3).splitlines()
            if line.strip() and not line.strip().isdigit() and line.strip() != "WEBVTT"
        )
        if body:
            cues.append({
                "start": _parse_time(match.group(1)),
                "end": _parse_time(match.group(2)),
                "text": body,
            })
    return cues


def _clean_subtitle_text(text):
    value = str(text or "")
    value = re.sub(r"<[^>]+>", "", value)
    value = re.sub(r"\{[^}]*\}", "", value)
    value = re.sub(r"\s+", " ", value).strip()
    return value


def _source_kind_from_path(path, fallback="unknown"):
    value = str(path or "").lower()
    if not value:
        return fallback
    if ".ocr." in value:
        return "workbench-vision/OCR/visual-subtitle"
    if ".online." in value:
        return "online-subtitle"
    if ".longasr." in value or ".hq.translated." in value:
        return "long-asr-corrected"
    if ".enhanced." in value:
        return "enhanced"
    if ".refined." in value:
        return "refined"
    if ".source." in value:
        return fallback if fallback != "unknown" else "quick-source"
    if ".zh." in value:
        return "quick-baseline"
    return fallback


def _source_priority_rank(kind):
    value = str(kind or "").lower()
    if "ocr" in value or "hard-sub" in value or "vision" in value or "visual-subtitle" in value:
        return "2-hard-sub-ocr"
    if value in ("local", "embedded", "external", "srt", "vtt", "ass", "ssa", "subtitle-source") or "subtitle" in value and "online" not in value:
        return "1-local-embedded-external-subtitle"
    if "online" in value:
        return "1b-online-high-confidence-local-like-hq-candidate"
    if "asr" in value or "quick" in value:
        return "3-audio-asr-fallback"
    return "unknown"


def _timing_alignment(reference, candidate):
    if not reference or not candidate:
        return 0.0
    overlap = max(0.0, min(reference["end"], candidate["end"]) - max(reference["start"], candidate["start"]))
    union = max(reference["end"], candidate["end"]) - min(reference["start"], candidate["start"])
    if union <= 0:
        return 0.0
    return max(0.0, min(1.0, overlap / union))


def _cleanup_reason(raw, cleaned):
    if not raw:
        return "empty"
    if raw == cleaned:
        return "unchanged"
    return "normalized-tags-or-whitespace"


def _read_optional_json(path):
    if not path or not Path(path).exists():
        return {}
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig", errors="replace"))
    except Exception as exc:
        return {"readError": str(exc)}


def _first_nonempty(*values):
    for value in values:
        if str(value or "").strip():
            return value
    return ""


def _load_inline_reference(items):
    cues = []
    for item in items or []:
        parts = item.split("|", 2)
        if len(parts) != 3:
            continue
        cues.append({"start": float(parts[0]), "end": float(parts[1]), "text": parts[2].strip()})
    return cues


def _norm(text):
    value = str(text or "").strip().lower()
    value = re.sub(r"\s+", " ", value)
    return value


def _compact_zh(text):
    return re.sub(r"[\s，。！？、,.!?;；:：\"'“”‘’（）()<>《》【】\[\]-]+", "", str(text or ""))


def _similarity(a, b):
    a = _norm(a)
    b = _norm(b)
    if not a and not b:
        return 1.0
    if not a or not b:
        return 0.0
    score = difflib.SequenceMatcher(None, a, b).ratio()
    a_compact = _compact_zh(a)
    b_compact = _compact_zh(b)
    if a_compact and b_compact:
        a_chars = set(a_compact)
        b_chars = set(b_compact)
        dice = (2.0 * len(a_chars & b_chars)) / max(1, len(a_chars) + len(b_chars))
        score = max(score, dice)
        both_unsafe = _contains_any(a, ("不安全", "危险", "not safe", "unsafe")) and _contains_any(b, ("不安全", "危险", "not safe", "unsafe"))
        both_go = _contains_any(a, ("走", "离开", "go", "leave")) and _contains_any(b, ("走", "离开", "go", "leave"))
        if both_unsafe and both_go:
            score = max(score, 0.82)
    return score


def _contains_any(text, markers):
    value = _norm(text)
    compact = _compact_zh(text).lower()
    for marker in markers:
        marker_value = str(marker).lower()
        if marker_value in value or _compact_zh(marker_value).lower() in compact:
            return True
    return False


UNSAFE_MARKERS = (
    "not safe", "isn't safe", "is not safe", "cannot be safe", "can't be safe",
    "unsafe", "not secure", "dangerous", "in danger",
    "不安全", "有危险", "有危險", "危险", "危險",
    "涓嶅畨鍏", "鏈夊嵄闄", "鍗遍毆", "鍗遍櫓",
)
SAFE_MARKERS = (
    "safe", "no danger", "not dangerous",
    "没有危险", "沒有危險", "无危险", "無危險", "没危险", "沒危險",
    "不危险", "不危險", "很安全", "这里安全", "這裡安全",
    "娌℃湁鍗遍櫓", "娌掓湁鍗遍毆", "鏃犲嵄闄", "鐒″嵄闅",
    "娌″嵄闄", "娌掑嵄闅", "涓嶅嵄闄", "寰堝畨鍏", "杩欓噷瀹夊叏",
)
NEGATION_MARKERS = (
    "not", "no ", "never", "can't", "cannot", "don't", "doesn't", "didn't",
    "不", "没有", "沒有", "没", "沒", "别", "不要",
    "涓嶈兘", "涓嶅彲浠", "涓嶈", "娌℃湁", "娌掓湁",
)
REVERSAL_PAIRS = (
    ("not safe", SAFE_MARKERS),
    ("unsafe", SAFE_MARKERS),
    ("不安全", SAFE_MARKERS),
    ("危险", ("没有危险", "沒有危險", "无危险", "無危險", "不危险")),
    ("can't", ("可以", "能", "鍙", "鑳")),
    ("cannot", ("可以", "能", "鍙", "鑳")),
    ("don't", ("要", "应该", "應該", "瑕", "搴旇")),
    ("never", ("总是", "一定", "鎬绘槸", "涓€瀹")),
    ("before", ("之后", "以後", "以后", "涔嬪悗", "浠ュ悗")),
    ("after", ("之前", "以前", "涔嬪墠", "浠ュ墠")),
    ("kill", ("救", "拯救", "鏁", "鎷")),
    ("save", ("杀", "殺", "死", "鏉", "娈")),
)


def _polarity_report(source_text, reference_source_text, reference_text, candidate_text):
    basis = f"{source_text} {reference_source_text} {reference_text}"
    candidate = candidate_text or ""
    source_unsafe = _contains_any(basis, UNSAFE_MARKERS)
    candidate_explicit_unsafe = _contains_any(candidate, ("不安全", "这里不安全", "這裡不安全")) or (
        _contains_any(candidate, ("有危险", "有危險", "危险", "危險")) and
        not _contains_any(candidate, ("没有危险", "沒有危險", "无危险", "無危險", "没危险", "沒危險", "不危险", "不危險"))
    )
    candidate_safe = _contains_any(candidate, SAFE_MARKERS) and not candidate_explicit_unsafe
    categories = []
    if source_unsafe and candidate_safe:
        categories.append("safety-negation-reversal")
    for marker, wrong_markers in REVERSAL_PAIRS:
        if marker.lower() in _norm(basis) and _contains_any(candidate, wrong_markers):
            label = f"polarity-reversal:{marker}"
            if label not in categories:
                categories.append(label)
    return {
        "sourceHasNegation": _contains_any(basis, NEGATION_MARKERS),
        "sourceUnsafe": source_unsafe,
        "candidateSafe": candidate_safe,
        "candidateExplicitUnsafe": candidate_explicit_unsafe,
        "polarityReversal": bool(categories),
        "categories": categories,
    }


def _find_at(cues, start, end, tolerance=8.0):
    best = None
    best_score = -999999.0
    for cue in cues:
        overlap = max(0.0, min(end, cue["end"]) - max(start, cue["start"]))
        delta = abs(cue["start"] - start) + abs(cue["end"] - end)
        center_delta = abs(((cue["start"] + cue["end"]) / 2.0) - ((start + end) / 2.0))
        score = overlap * 10.0 - delta - center_delta * 0.25
        if score > best_score:
            best = cue
            best_score = score
    return best if best is not None and best_score > -tolerance else None


def _nearest_or_at(cues, ref):
    if not cues or not ref:
        return None
    # Do not pull in a far-away "nearest" cue for layer diagnostics. A wrong
    # distant OCR/reference cue makes the per-cue evidence look like a source
    # selection bug when the real issue is missing coverage for this time.
    return _find_at(cues, ref["start"], ref["end"])


def _diagnostic_references(reference, source, quick, enhanced, final, ocr_source, ocr_translated):
    if reference:
        return reference
    for cues in (final, enhanced, quick, ocr_translated, ocr_source, source):
        if cues:
            return cues
    return []


def _build_cue_diagnostics(args, reference, reference_source, source, quick, enhanced, final, ocr_source, ocr_translated):
    rows = []
    ocr_report = _read_optional_json(args.ocr_report)
    refs = _diagnostic_references(reference, source, quick, enhanced, final, ocr_source, ocr_translated)
    quick_kind = args.quick_kind or _source_kind_from_path(args.quick, "quick-baseline")
    enhanced_kind = args.enhanced_kind or _source_kind_from_path(args.enhanced, "enhanced")
    final_kind = args.final_kind or _source_kind_from_path(args.final, "final-display")
    source_kind = args.source_kind or _source_kind_from_path(args.source, "quick-source")
    for index, ref in enumerate(refs, 1):
        ref_source_cue = _find_at(reference_source, ref["start"], ref["end"]) if reference_source else None
        source_cue = _nearest_or_at(source, ref)
        quick_cue = _nearest_or_at(quick, ref)
        enhanced_cue = _nearest_or_at(enhanced, ref)
        final_cue = _nearest_or_at(final, ref) if final else (enhanced_cue or quick_cue)
        ocr_source_cue = _nearest_or_at(ocr_source, ref)
        ocr_translated_cue = _nearest_or_at(ocr_translated, ref)
        raw_source = _first_nonempty(source_cue and source_cue.get("text"), ref_source_cue and ref_source_cue.get("text"))
        cleaned_source = _clean_subtitle_text(raw_source)
        raw_ocr = ocr_source_cue.get("text", "") if ocr_source_cue else ""
        cleaned_ocr = _clean_subtitle_text(raw_ocr)
        reference_text = ref.get("text", "") if reference else ""
        final_text = final_cue.get("text", "") if final_cue else ""
        has_nearby_ocr = bool(raw_ocr or (ocr_translated_cue and ocr_translated_cue.get("text", "")))
        enhanced_text = enhanced_cue.get("text", "") if enhanced_cue else ""
        quick_text = quick_cue.get("text", "") if quick_cue else ""
        visual_text_detected = bool(cleaned_ocr or raw_ocr or (ocr_translated_cue and ocr_translated_cue.get("text", "")))
        visual_cue_at_time_text = _first_nonempty(
            ocr_translated_cue and ocr_translated_cue.get("text"),
            raw_ocr,
            cleaned_ocr,
        )
        visual_cue_at_time = bool(visual_cue_at_time_text)
        quick_cue_at_time = bool(quick_text)
        if final_cue and final:
            chosen_kind = final_kind
            fallback_reason = "final-sidecar"
        elif enhanced_cue:
            enhanced_claims_ocr = any(token in str(enhanced_kind).lower() for token in ("ocr", "hard-sub", "vision", "visual-subtitle"))
            if enhanced_claims_ocr and not has_nearby_ocr:
                chosen_kind = quick_kind if quick_cue else "fusion"
                fallback_reason = "fallback-quick-no-nearby-ocr-cue" if enhanced_text == quick_text else "enhanced-without-nearby-ocr-cue"
            else:
                chosen_kind = enhanced_kind
                fallback_reason = "accepted-enhanced"
        else:
            chosen_kind = quick_kind if quick_cue else "missing"
            fallback_reason = "fallback-quick" if quick_cue else "missing-final-cue"
        translated_text = _first_nonempty(
            final_text,
            enhanced_text,
            quick_text,
            ocr_translated_cue and ocr_translated_cue.get("text"),
        )
        final_displayed_text = _first_nonempty(final_text, enhanced_text, quick_text, ocr_translated_cue and ocr_translated_cue.get("text"))
        if visual_text_detected and not visual_cue_at_time:
            fallback_reason = "visual-no-text-suppress-quick-asr"
            final_displayed_text = ""
        align_basis = ref if reference else (quick_cue or source_cue or ocr_source_cue)
        visual_source_cue_at_time = bool(raw_ocr or cleaned_ocr or (ocr_source_cue and ocr_source_cue.get("text", "")))
        if visual_source_cue_at_time and not visual_cue_at_time:
            fallback_reason = "visual-no-text-suppress-quick-asr"
        if visual_source_cue_at_time and not final_displayed_text:
            final_displayed_text = _first_nonempty(ocr_translated_cue and ocr_translated_cue.get("text"), quick_text, enhanced_text, final_text)
        rows.append({
            "index": index,
            "time": {
                "startSeconds": ref["start"],
                "endSeconds": ref["end"],
                "start": _format_time(ref["start"]),
                "end": _format_time(ref["end"]),
            },
            "track": {
                "id": args.track_id,
                "lang": args.track_lang,
                "kind": args.track_kind,
            },
            "sourceKind": source_kind,
            "sourcePriorityRank": _source_priority_rank(chosen_kind or source_kind),
            "chosenSourceKind": chosen_kind,
            "rawSourceText": raw_source,
            "cleanedSourceText": cleaned_source,
            "sentToProviderText": cleaned_source,
            "providerResult": translated_text,
            "visualTextDetected": visual_text_detected,
            "visualSourceCueAtTime": visual_source_cue_at_time,
            "visualCueAtTime": visual_cue_at_time,
            "quickCueAtTime": quick_cue_at_time,
            "quickDisplayedText": quick_text,
            "visualSourceDisplayedText": raw_ocr or cleaned_ocr,
            "visualDisplayedText": visual_cue_at_time_text,
            "quickText": quick_text,
            "enhancedText": enhanced_text,
            "translatedText": translated_text,
            "finalDisplayedText": final_displayed_text,
            "referenceText": reference_text,
            "referenceTextOcrOnscreenChinese": cleaned_ocr,
            "referenceSourceText": ref_source_cue.get("text", "") if ref_source_cue else "",
            "chosenReason": fallback_reason,
            "rejectReason": "" if fallback_reason.startswith(("accepted", "candidate-used")) else fallback_reason,
            "fallbackReason": fallback_reason,
            "provider": args.provider,
            "model": args.model,
            "batchId": 1 + ((index - 1) // max(1, args.batch_size)),
            "alignmentScore": round(_timing_alignment(align_basis, final_cue or quick_cue or enhanced_cue or source_cue or ocr_source_cue), 4),
            "sourceLayer": {
                "track": args.track_id,
                "lang": args.track_lang,
                "rawSourceText": raw_source,
                "cleanedSourceText": cleaned_source,
                "cleanupReason": _cleanup_reason(raw_source, cleaned_source),
                "alignmentScore": round(_timing_alignment(align_basis, source_cue), 4),
            },
            "translationLayer": {
                "provider": args.provider,
                "model": args.model,
                "batchId": 1 + ((index - 1) // max(1, args.batch_size)),
                "sentToProviderText": cleaned_source,
                "providerResult": translated_text,
                "translatedText": translated_text,
                "finalDisplayedText": final_displayed_text,
                "chosenReason": fallback_reason,
                "fallbackReason": fallback_reason,
            },
            "ocrLayer": {
                "sourceKind": "workbench-vision/OCR/visual-subtitle" if (ocr_source_cue or ocr_translated_cue or args.ocr_report) else "",
                "rawText": raw_ocr,
                "cleanedText": cleaned_ocr,
                "confidence": ocr_report.get("meanConfidence", ocr_report.get("confidence")),
                "cleanupReason": _cleanup_reason(raw_ocr, cleaned_ocr),
                "cueSplitMergeReason": ocr_report.get("cueSplitMergeReason", "as-read-from-ocr-cache" if raw_ocr else ""),
                "subtitleRegion": args.subtitle_region,
                "translatedText": ocr_translated_cue.get("text", "") if ocr_translated_cue else "",
                "finalDisplayedText": final_displayed_text,
                "referenceText": reference_text,
                "chosenReason": "candidate-used" if any(token in str(chosen_kind).lower() for token in ("ocr", "hard-sub", "vision", "visual-subtitle")) and raw_ocr else ("available-not-chosen" if raw_ocr else "no-nearby-ocr-cue"),
                "fallbackReason": fallback_reason,
            },
        })
    return rows


def _filter_window(cues, target, window):
    if target < 0 or window <= 0:
        return cues
    lo = target - window
    hi = target + window
    return [cue for cue in cues if cue["end"] >= lo and cue["start"] <= hi]


def _person_name_report(rows):
    aliases = {
        "Elmo": ("Elmo", "艾尔莫", "埃尔莫", "伊莫", "浼婅帿", "鑹捐帿"),
        "Emo": ("Emo", "伊莫", "艾莫", "浼婅帿", "鑹捐帿"),
    }
    diagnostics = {}
    for canonical, names in aliases.items():
        ref_hits = 0
        final_hits = 0
        inconsistent = 0
        for row in rows:
            reference = f"{row.get('referenceSourceText', '')} {row.get('referenceText', '')}"
            final = row.get("finalText", "")
            if any(name in reference for name in names):
                ref_hits += 1
                if any(name in final for name in names):
                    final_hits += 1
                else:
                    inconsistent += 1
        if ref_hits:
            diagnostics[canonical] = {
                "referenceMentions": ref_hits,
                "finalMentions": final_hits,
                "inconsistentCueCount": inconsistent,
            }
    return diagnostics


def _terminology_report(rows):
    diagnostics = {
        "personNameConsistency": _person_name_report(rows),
        "placeholderCueCount": 0,
        "mojibakeLikeCueCount": 0,
    }
    mojibake_markers = ("鍟", "鍡", "浼", "鎴", "涓", "娌", "鈥", "�")
    for row in rows:
        final = row.get("finalText", "")
        if "<chinese>" in final.lower() or "<中文>" in final:
            diagnostics["placeholderCueCount"] += 1
        if any(marker in final for marker in mojibake_markers):
            diagnostics["mojibakeLikeCueCount"] += 1
    return diagnostics


def _decision(quick_cue, enhanced_cue, quick_polarity, enhanced_polarity):
    if enhanced_cue and enhanced_polarity["polarityReversal"] and quick_cue:
        return "fallback-quick-polarity-conflict"
    if enhanced_cue:
        return "accepted-enhanced"
    if quick_cue:
        return "fallback-quick"
    return "missing"


def evaluate(args):
    reference_translation = _read_subtitle(args.reference) + _read_subtitle(args.reference_translation)
    reference_translation += _load_inline_reference(args.reference_cue)
    reference_source = _read_subtitle(args.reference_source)
    reference = _filter_window(reference_translation, args.focus_time, args.window_seconds)
    source = _read_subtitle(args.source)
    quick = _read_subtitle(args.quick)
    enhanced = _read_subtitle(args.enhanced)
    final = _read_subtitle(args.final)
    ocr_source = _read_subtitle(args.ocr_source)
    ocr_translated = _read_subtitle(args.ocr_translated)
    rows = []
    source_kind_counts = {}
    polarity_errors = 0
    empty_errors = 0
    missing_errors = 0
    matched = 0
    measurable = 0
    visible = 0
    for index, ref in enumerate(reference, 1):
        ref_source_cue = _find_at(reference_source, ref["start"], ref["end"]) if reference_source else None
        source_cue = _find_at(source, ref["start"], ref["end"])
        quick_cue = _find_at(quick, ref["start"], ref["end"])
        enhanced_cue = _find_at(enhanced, ref["start"], ref["end"]) if enhanced else None
        source_text = source_cue["text"] if source_cue else ""
        ref_source_text = ref_source_cue["text"] if ref_source_cue else ""
        quick_text = quick_cue["text"] if quick_cue else ""
        enhanced_text = enhanced_cue["text"] if enhanced_cue else ""
        quick_polarity = _polarity_report(source_text, ref_source_text, ref["text"], quick_text)
        enhanced_polarity = _polarity_report(source_text, ref_source_text, ref["text"], enhanced_text)
        decision = _decision(quick_cue, enhanced_cue, quick_polarity, enhanced_polarity)
        final_cue = quick_cue if decision == "fallback-quick-polarity-conflict" else (enhanced_cue or quick_cue)
        final_text = final_cue["text"] if final_cue else ""
        polarity = _polarity_report(source_text, ref_source_text, ref["text"], final_text)
        chosen_kind = (
            "quick-baseline"
            if decision == "fallback-quick-polarity-conflict"
            else (args.enhanced_kind if enhanced_cue else (args.quick_kind if quick_cue else "missing"))
        )
        source_kind_counts[chosen_kind] = source_kind_counts.get(chosen_kind, 0) + 1
        score = _similarity(ref["text"], final_text)
        errors = []
        if not final_text:
            empty_errors += 1
            errors.append("empty-or-missing-final")
        if final_cue is None:
            missing_errors += 1
            errors.append("missing-final-cue")
        if polarity["polarityReversal"]:
            polarity_errors += 1
            errors.extend(polarity["categories"])
        if score < args.threshold and not errors:
            errors.append("low-text-similarity")
        passed = bool(final_text) and score >= args.threshold and not polarity["polarityReversal"]
        if final_text:
            measurable += 1
            visible += 1
        if passed:
            matched += 1
        rows.append({
            "index": index,
            "time": {
                "startSeconds": ref["start"],
                "endSeconds": ref["end"],
                "start": _format_time(ref["start"]),
                "end": _format_time(ref["end"]),
            },
            "chosenSourceKind": chosen_kind,
            "sourceKind": args.source_kind or _source_kind_from_path(args.source, "quick-source"),
            "sourcePriorityRank": args.source_priority_rank or _source_priority_rank(chosen_kind),
            "track": {
                "id": args.track_id,
                "lang": args.track_lang,
                "kind": args.track_kind,
            },
            "rawSourceText": source_text,
            "cleanedSourceText": _clean_subtitle_text(source_text),
            "sentToProviderText": _clean_subtitle_text(source_text),
            "provider": args.provider,
            "model": args.model,
            "providerResult": enhanced_text or quick_text,
            "sourceText": source_text,
            "referenceSourceText": ref_source_text,
            "quickText": quick_text,
            "enhancedText": enhanced_text,
            "finalText": final_text,
            "finalDisplayedText": final_text,
            "referenceText": ref["text"],
            "referenceTextOcrOnscreenChinese": "",
            "chosenReason": decision,
            "rejectReason": "" if decision.startswith("accepted") else decision,
            "fallbackReason": decision,
            "alignmentScore": round(_timing_alignment(ref, final_cue or quick_cue or enhanced_cue or source_cue), 4),
            "similarity": round(score, 4),
            "score": round(score, 4),
            "match": passed,
            "errorCategories": errors,
            "polarityCheck": polarity,
            "quickPolarityCheck": quick_polarity,
            "enhancedPolarityCheck": enhanced_polarity if enhanced_cue else {},
            "repairDecision": decision,
            "fallbackDecision": decision,
            "quickFallback": decision.startswith("fallback-quick"),
        })
    accuracy = matched / max(1, len(reference))
    report = {
        "version": "accuracy-v2",
        "media": args.media,
        "paths": {
            "referenceSource": args.reference_source,
            "referenceTranslation": args.reference_translation or args.reference,
            "source": args.source,
            "quick": args.quick,
            "enhanced": args.enhanced,
            "final": args.final,
            "ocrSource": args.ocr_source,
            "ocrTranslated": args.ocr_translated,
        },
        "ocrReferenceUsed": args.ocr_reference_used,
        "referenceUsed": bool(reference_translation),
        "sourceReferenceUsed": bool(reference_source),
        "targetTimeSeconds": args.focus_time if args.focus_time >= 0 else None,
        "windowSeconds": args.window_seconds if args.window_seconds > 0 else None,
        "totalCues": len(reference),
        "totalCueCount": len(reference),
        "measurableCues": measurable,
        "assessableCueCount": measurable,
        "matchedCueCount": matched,
        "visibleSubtitleCues": visible,
        "correctOrSemanticallyEquivalentCues": matched,
        "correctOrSemanticallyEquivalentCueRatio": round(matched / max(1, visible), 4),
        "roughAccuracy": round(accuracy, 4),
        "polarityErrorCount": polarity_errors,
        "emptyOrMissingTranslationCount": empty_errors,
        "missingCueCount": missing_errors,
        "sourceKindDistribution": source_kind_counts,
        "terminology": _terminology_report(rows),
        "terminologyUsed": _terminology_report(rows),
        "rows": rows,
        "cueDiagnostics": _build_cue_diagnostics(
            args,
            reference,
            reference_source,
            source,
            quick,
            enhanced,
            final,
            ocr_source,
            ocr_translated,
        ),
    }
    if args.focus_time >= 0:
        focus = [row for row in rows if row["time"]["startSeconds"] - 1.0 <= args.focus_time <= row["time"]["endSeconds"] + 1.0]
        if not focus and rows:
            focus = sorted(rows, key=lambda row: abs(((row["time"]["startSeconds"] + row["time"]["endSeconds"]) / 2.0) - args.focus_time))[:1]
        report["focusTimeSeconds"] = args.focus_time
        report["focusRows"] = focus
        report["focusPassed"] = bool(focus) and all(row["match"] and not row["polarityCheck"]["polarityReversal"] for row in focus)
    return report


def main():
    parser = argparse.ArgumentParser(description="CGPlay reference-based subtitle accuracy evaluator v2.")
    parser.add_argument("--media", default="")
    parser.add_argument("--reference", default="", help="Backward-compatible translation reference path.")
    parser.add_argument("--reference-source", default="")
    parser.add_argument("--reference-translation", default="")
    parser.add_argument("--reference-cue", action="append", default=[], help="Inline translation cue: start|end|text")
    parser.add_argument("--source", default="")
    parser.add_argument("--quick", default="")
    parser.add_argument("--enhanced", default="")
    parser.add_argument("--quick-kind", default="quick-baseline")
    parser.add_argument("--enhanced-kind", default="enhanced")
    parser.add_argument("--final", default="")
    parser.add_argument("--final-kind", default="final-display")
    parser.add_argument("--ocr-source", default="")
    parser.add_argument("--ocr-translated", default="")
    parser.add_argument("--ocr-report", default="")
    parser.add_argument("--source-kind", default="")
    parser.add_argument("--source-priority-rank", default="")
    parser.add_argument("--track-id", default="")
    parser.add_argument("--track-lang", default="")
    parser.add_argument("--track-kind", default="")
    parser.add_argument("--provider", default="")
    parser.add_argument("--model", default="")
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--subtitle-region", default="")
    parser.add_argument("--ocr-reference-used", action="store_true")
    parser.add_argument("--focus-time", type=float, default=-1.0)
    parser.add_argument("--window-seconds", type=float, default=0.0)
    parser.add_argument("--threshold", type=float, default=0.55)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    report = evaluate(args)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({
        "output": str(output),
        "roughAccuracy": report["roughAccuracy"],
        "polarityErrorCount": report["polarityErrorCount"],
        "focusPassed": report.get("focusPassed"),
        "totalCues": report["totalCues"],
    }, ensure_ascii=False))
    return 0 if report.get("focusPassed", True) and report["polarityErrorCount"] == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
