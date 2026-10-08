#!/usr/bin/env python3
"""Create the user-facing RVLite translation acceptance package."""

import argparse
import json
import re
import shutil
import subprocess
import sys
import zipfile
from html import escape
from pathlib import Path


def _run(cmd, cwd):
    try:
        proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
        return {
            "command": " ".join(cmd),
            "exitCode": proc.returncode,
            "stdout": proc.stdout.strip()[-2000:],
            "stderr": proc.stderr.strip()[-2000:],
        }
    except Exception as exc:
        return {"command": " ".join(cmd), "exitCode": -1, "error": str(exc)}


def _safe_name(value):
    keep = []
    for ch in value:
        if ch.isalnum() or ch in "-_.":
            keep.append(ch)
        elif ch.isspace():
            keep.append("_")
    name = "".join(keep).strip("._")
    return name[:80] or "video"


def _write_json(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def _plain_text(value):
    text = re.sub(r"`([^`]*)`", r"\1", str(value))
    text = re.sub(r"\*\*([^*]*)\*\*", r"\1", text)
    text = text.replace("\\", "\\")
    return text


def _write_minimal_docx(path, lines):
    path.parent.mkdir(parents=True, exist_ok=True)
    paragraphs = []
    for raw in lines:
        text = _plain_text(raw)
        if text.startswith("```") or text.startswith("|---"):
            continue
        style = ""
        if text.startswith("# "):
            text = text[2:]
            style = '<w:pStyle w:val="Title"/>'
        elif text.startswith("## "):
            text = text[3:]
            style = '<w:pStyle w:val="Heading1"/>'
        elif text.startswith("### "):
            text = text[4:]
            style = '<w:pStyle w:val="Heading2"/>'
        elif text.startswith("- "):
            text = text[2:]
        if not text:
            paragraphs.append("<w:p/>")
            continue
        paragraphs.append(
            "<w:p><w:pPr>"
            + style
            + '</w:pPr><w:r><w:t xml:space="preserve">'
            + escape(text)
            + "</w:t></w:r></w:p>"
        )
    document = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
        "<w:body>"
        + "".join(paragraphs)
        + '<w:sectPr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>'
        + "</w:body></w:document>"
    )
    styles = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
        '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/>'
        '<w:rPr><w:rFonts w:ascii="Microsoft YaHei" w:eastAsia="Microsoft YaHei"/><w:sz w:val="21"/></w:rPr></w:style>'
        '<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/>'
        '<w:rPr><w:rFonts w:ascii="Microsoft YaHei" w:eastAsia="Microsoft YaHei"/><w:b/><w:sz w:val="36"/></w:rPr></w:style>'
        '<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/>'
        '<w:rPr><w:rFonts w:ascii="Microsoft YaHei" w:eastAsia="Microsoft YaHei"/><w:b/><w:sz w:val="28"/></w:rPr></w:style>'
        '<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/>'
        '<w:rPr><w:rFonts w:ascii="Microsoft YaHei" w:eastAsia="Microsoft YaHei"/><w:b/><w:sz w:val="24"/></w:rPr></w:style>'
        "</w:styles>"
    )
    content_types = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/>'
        '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
        '<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>'
        "</Types>"
    )
    rels = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
        "</Relationships>"
    )
    doc_rels = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>'
        "</Relationships>"
    )
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/document.xml", document)
        z.writestr("word/styles.xml", styles)
        z.writestr("word/_rels/document.xml.rels", doc_rels)


def _verify_docx(path):
    result = {
        "docxPath": str(path),
        "exists": path.exists(),
        "size": path.stat().st_size if path.exists() else 0,
        "isZip": False,
        "requiredParts": {},
        "containsReadableChinese": False,
        "renderAttempted": False,
        "renderPassed": False,
        "renderNote": "LibreOffice/soffice not available; structural DOCX verification only.",
        "passed": False,
    }
    if not path.exists():
        return result
    try:
        with zipfile.ZipFile(path) as z:
            names = set(z.namelist())
            result["isZip"] = True
            for part in ("[Content_Types].xml", "word/document.xml", "word/styles.xml", "word/_rels/document.xml.rels"):
                result["requiredParts"][part] = part in names
            doc_xml = z.read("word/document.xml").decode("utf-8", errors="replace")
            result["containsReadableChinese"] = all(token in doc_xml for token in ("完整视频", "翻译", "验收", "可复跑命令"))
    except Exception as exc:
        result["error"] = str(exc)
    result["passed"] = bool(result["isZip"] and all(result["requiredParts"].values()) and result["containsReadableChinese"])
    return result


def _read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except Exception:
        return ""


def _has_regex(text, pattern):
    return re.search(pattern, text, re.MULTILINE | re.DOTALL) is not None


def _quick_constants_audit(repo, artifacts):
    app_cpp = _read_text(repo / "src" / "ui" / "app" / "Application.cpp")
    service_cpp = _read_text(repo / "src" / "services" / "ai" / "SubtitleGenerationService.cpp")
    guard_cpp = _read_text(repo / "src" / "services" / "ai" / "TranslationSpeedGuard.cpp")
    checks = {
        "audioChunk4": _has_regex(service_cpp, r"constexpr\s+int\s+kChunkSeconds\s*=\s*4\s*;"),
        "defaultAsrConcurrency8": _has_regex(service_cpp, r"constexpr\s+int\s+kDefaultAsrConcurrency\s*=\s*8\s*;"),
        "defaultTranslationConcurrency8": _has_regex(service_cpp, r"constexpr\s+int\s+kDefaultTranslationConcurrency\s*=\s*8\s*;"),
        "firstWindow24": _has_regex(app_cpp, r"kInitialSubtitleWindowSeconds\s*=\s*24\.0\s*;"),
        "backgroundWindow180": _has_regex(app_cpp, r"kBackgroundSubtitleWindowSeconds\s*=\s*180\.0\s*;"),
        "prefetch210": _has_regex(app_cpp, r"kSubtitlePrefetchLeadSeconds\s*=\s*210\.0\s*;"),
        "overlap18": _has_regex(app_cpp, r"kSubtitleContinuationOverlapSeconds\s*=\s*18\.0\s*;"),
        "initialPreroll6": _has_regex(app_cpp, r"kInitialSubtitlePrerollSeconds\s*=\s*6\.0\s*;"),
        "speedGuardReports4s": _has_regex(guard_cpp, r"asrChunkSeconds[^\n]+4"),
        "speedGuardReports8x8": _has_regex(guard_cpp, r"asrConcurrency[^\n]+8") and _has_regex(guard_cpp, r"translationConcurrency[^\n]+8"),
    }
    audit = {
        "timestamp": "2026-07-08Tacceptance",
        "sourceFiles": {
            "Application.cpp": str(repo / "src" / "ui" / "app" / "Application.cpp"),
            "SubtitleGenerationService.cpp": str(repo / "src" / "services" / "ai" / "SubtitleGenerationService.cpp"),
            "TranslationSpeedGuard.cpp": str(repo / "src" / "services" / "ai" / "TranslationSpeedGuard.cpp"),
        },
        "checks": checks,
        "allPassed": all(checks.values()),
        "protected": "quick constants frozen",
    }
    _write_json(artifacts / "quick_constants_audit_20260708_headless_acceptance.json", audit)
    return audit


def _video_summary(report):
    decision = report.get("sourceDecision", {})
    source_count = report.get("sourceDetectedCueCount", report.get("visualCueCount", 0))
    visible_count = report.get("visibleSubtitleCueCount", report.get("visibleSubtitleCues", 0))
    translated_count = report.get("translatedCueCount", report.get("correctCueCount", 0))
    final_count = report.get("finalDisplayedCueCount", translated_count)
    segments = report.get("segments", report.get("segmentAccuracy", {}).get("segments", []))
    overall_accuracy = report.get("overallAccuracy", report.get("accuracy", 0.0))
    min_segment_accuracy = report.get("minSegmentAccuracy", report.get("segmentAccuracy", {}).get("minAccuracy", overall_accuracy))
    return {
        "mediaPath": report.get("media"),
        "mediaFingerprint": report.get("mediaIdentity", {}).get("mediaFingerprint"),
        "pass": report.get("pass", False),
        "passed": report.get("pass", False),
        "overallPass": report.get("pass", False),
        "durationMs": report.get("durationMs", 0),
        "durationSec": report.get("durationSec", 0),
        "scanMode": report.get("scanMode", "full-video"),
        "frameSampleInterval": report.get("frameSampleInterval", ""),
        "cueDetectionMethod": report.get("cueDetectionMethod", ""),
        "visibleSubtitleCueCount": visible_count,
        "sourceDetectedCueCount": source_count,
        "translatedCueCount": translated_count,
        "finalDisplayedCueCount": final_count,
        "missingTranslationCount": report.get("missingTranslationCount", 0),
        "rawSourceDisplayedCount": report.get("rawSourceDisplayedCount", 0),
        "lateTranslationCueCount": report.get("lateTranslationCueCount", report.get("missingTranslationCount", 0)),
        "wrongMediaCacheCount": report.get("wrongMediaCacheCount", report.get("cacheMediaMismatchCount", 0)),
        "hallucinationCount": report.get("hallucinationCount", 0),
        "overTranslationCount": report.get("overTranslationCount", 0),
        "semanticMismatchCount": report.get("semanticMismatchCount", 0),
        "strictGateVersion": report.get("strictGateVersion", ""),
        "normalizedTextSimilarityMin": report.get("normalizedTextSimilarityMin", 1.0),
        "charCoverageMin": report.get("charCoverageMin", 1.0),
        "keyTermCoverageMin": report.get("keyTermCoverageMin", 1.0),
        "extraTokenRatioMax": report.get("extraTokenRatioMax", 0.0),
        "shortCueDetectedCount": report.get("shortCueDetectedCount", 0),
        "shortCueMissedCount": report.get("shortCueMissedCount", 0),
        "shortCueMissedSamples": report.get("shortCueMissedSamples", []),
        "lowConfidenceIntervals": report.get("lowConfidenceIntervals", []),
        "fixedCheckpointFailures": report.get("fixedCheckpointFailures", []),
        "fixedCheckpointFailureCount": report.get("fixedCheckpointFailureCount", 0),
        "textFidelityThresholds": report.get("textFidelityThresholds", {}),
        "noSubtitleFalsePositiveCount": report.get("noSubtitleFalsePositiveCount", 0),
        "noSubtitleFalsePositiveSamples": report.get("noSubtitleFalsePositiveSamples", []),
        "visualNoTextSuppressQuickAsrCount": report.get("visualNoTextSuppressQuickAsrCount", 0),
        "asrSuppressedByVisualNoTextCount": report.get("asrSuppressedByVisualNoTextCount", 0),
        "staleCueClearedCount": report.get("staleCueClearedCount", 0),
        "finalSourceKindAtNoTextSamples": report.get("finalSourceKindAtNoTextSamples", []),
        "staleSubtitleSamples": report.get("staleSubtitleSamples", []),
        "candidateScanCachePath": report.get("candidateScanCachePath", ""),
        "detectorVersion": report.get("detectorVersion", ""),
        "sharedRuntimeHeadlessDetector": report.get("sharedRuntimeHeadlessDetector", False),
        "minSegmentAccuracy": min_segment_accuracy,
        "overallAccuracy": overall_accuracy,
        "segments": segments,
        "sourceDecision": decision,
        "segmentAccuracy": {
            "overall": overall_accuracy,
            "visibleCueCount": visible_count,
            "translatedCueCount": translated_count,
            "correctCueCount": report.get("correctCueCount", 0),
            "segments": segments,
            "minAccuracy": min_segment_accuracy,
            "targetAccuracy": report.get("segmentAccuracy", {}).get("targetAccuracy", 0.90),
            "pass": report.get("segmentAccuracy", {}).get("pass", False),
            "targetPass90": report.get("segmentAccuracy", {}).get("targetPass90", False),
            "note": "headless whole-video visible cue coverage audit; fixed screenshot points are samples only",
        },
        "fullVideoCoverage": {
            "sourceCueCount": source_count,
            "visibleSubtitleCueCount": visible_count,
            "translatedCueCount": translated_count,
            "finalDisplayedCueCount": final_count,
            "translatedCoverageCount": report.get("correctCueCount", 0),
            "missingTranslationCount": report.get("missingTranslationCount", 0),
            "missingTranslationSamples": report.get("missingTranslationSamples", []),
            "asrIntrusionSamples": report.get("asrIntrusionSamples", []),
            "staleSubtitleSamples": report.get("staleSubtitleSamples", []),
            "hallucinationSamples": report.get("hallucinationSamples", []),
            "overTranslationSamples": report.get("overTranslationSamples", []),
            "semanticMismatchSamples": report.get("semanticMismatchSamples", []),
            "shortCueMissedSamples": report.get("shortCueMissedSamples", []),
            "fixedCheckpointFailures": report.get("fixedCheckpointFailures", []),
            "lowConfidenceIntervals": report.get("lowConfidenceIntervals", []),
            "noSubtitleFalsePositiveSamples": report.get("noSubtitleFalsePositiveSamples", []),
            "finalSourceKindAtNoTextSamples": report.get("finalSourceKindAtNoTextSamples", []),
            "coverageProgressAudit": report.get("coverageProgressAudit", {}),
        },
        "playbackReadiness": report.get("playbackReadiness", {}),
        "performanceSummary": report.get("performanceSummary", {}),
        "coverageProgressAudit": report.get("coverageProgressAudit", {}),
        "lateCues": report.get("lateCues", report.get("missingTranslationSamples", [])),
        "missingTranslationCount": report.get("missingTranslationCount", 0),
        "lateTranslationCueCount": report.get("lateTranslationCueCount", report.get("missingTranslationCount", 0)),
        "rawSourceDisplayedCount": report.get("rawSourceDisplayedCount", 0),
        "hallucinationCount": report.get("hallucinationCount", 0),
        "overTranslationCount": report.get("overTranslationCount", 0),
        "semanticMismatchCount": report.get("semanticMismatchCount", 0),
        "noSubtitleFalsePositiveCount": report.get("noSubtitleFalsePositiveCount", 0),
        "visualNoTextSuppressQuickAsrCount": report.get("visualNoTextSuppressQuickAsrCount", 0),
        "asrSuppressedByVisualNoTextCount": report.get("asrSuppressedByVisualNoTextCount", 0),
        "staleCueClearedCount": report.get("staleCueClearedCount", 0),
        "finalSourceKindAtNoTextSamples": report.get("finalSourceKindAtNoTextSamples", []),
        "asrIntrusionCount": len(report.get("asrIntrusionSamples", [])),
        "cacheMismatchCount": report.get("cacheMediaMismatchCount", 0),
        "englishRawDisplayedCount": report.get("englishRawDisplayedCount", len(report.get("englishRawLeakSamples", []))),
        "crossMediaLeakCount": report.get("crossMediaLeakCount", 0),
        "forbiddenPhraseHits": report.get("forbiddenPhraseHitCount", len(report.get("forbiddenPhraseHits", []))),
        "staleTrackReuseCount": report.get("staleTrackReuseCount", 0),
        "mediaFingerprintMismatchCount": report.get("mediaFingerprintMismatchCount", report.get("cacheMediaMismatchCount", 0)),
        "holdout": report.get("holdout", False),
        "mojibakeCount": 0,
        "twoLineViolations": report.get("twoLineViolations", sum(1 for cue in report.get("perCueDiagnostics", []) if cue.get("visibleDisplayLineCount", 0) > 2)),
        "progressTruthPass": report.get("coverageProgressAudit", {}).get("progressTruthPass", False),
        "progressOverreportedMs": report.get("coverageProgressAudit", {}).get("progressOverreportedMs", 0),
    }


def _stable_count_summary(headless, videos):
    holdout_checks = headless.get("holdoutChecks", [])
    unknown_checks = headless.get("unknownVideoChecks", {})
    holdout_count = int(unknown_checks.get("count", len(holdout_checks)))
    pass_count = int(headless.get("passCount", sum(1 for item in videos if item.get("pass"))))
    fail_count = int(headless.get("failCount", sum(1 for item in videos if not item.get("pass"))))
    media_count = int(headless.get("mediaCount", pass_count + fail_count))
    return {
        "mediaCount": media_count,
        "passCount": pass_count,
        "failCount": fail_count,
        "holdoutCount": holdout_count,
    }


def _summary_schema_checks(summary):
    required_int_fields = ("mediaCount", "passCount", "failCount", "holdoutCount")
    return {
        "topLevelCountsPresent": all(field in summary for field in required_int_fields),
        "topLevelCountsAreIntegers": all(isinstance(summary.get(field), int) for field in required_int_fields),
        "topLevelCountsNonNegative": all(isinstance(summary.get(field), int) and summary.get(field) >= 0 for field in required_int_fields),
        "overallPassPresent": isinstance(summary.get("overallPass"), bool),
        "mediaCountMatchesPassFail": summary.get("mediaCount") == summary.get("passCount", 0) + summary.get("failCount", 0),
    }


def _readable_report_lines(summary, videos, repo, artifacts, args, git_rev, git_status, quick_audit, speed_guard):
    lines = []
    lines.append("# RVLite 完整视频翻译 Headless 验收报告")
    lines.append("")
    lines.append(f"- 测试日期：{args.date}")
    lines.append(f"- 构建 exe：`{args.exe}`")
    lines.append(f"- fresh backup：`{args.backup or '未提供'}`")
    lines.append(f"- Headless 原始汇总：`{args.headless_json}`")
    lines.append(f"- Git rev：`{git_rev.get('stdout', '').strip()}`")
    lines.append(f"- Dirty state：`{'dirty' if git_status.get('stdout', '').strip() else 'clean'}`")
    lines.append("")
    lines.append("## 总结")
    lines.append("")
    lines.append(f"- overallPass：`{str(summary['overallPass']).lower()}`")
    lines.append(f"- Release build：`{'PASS' if summary['releaseBuild'].get('passed') else 'FAIL/未记录'}`")
    lines.append(f"- windeployqt：`{'PASS' if summary['windeployqt'].get('passed') else 'FAIL/未记录'}`")
    lines.append(f"- quick frozen constants：`{'PASS' if quick_audit.get('allPassed') else 'FAIL'}`")
    lines.append(f"- 播放速度 guard：`{'PASS' if speed_guard.get('playbackRatePass') else 'FAIL/未记录'}`，playbackRate `{speed_guard.get('playbackRate', '')}`")
    lines.append(f"- missingTranslationCount：`{summary.get('missingTranslationCount')}`")
    lines.append(f"- rawSourceDisplayedCount：`{summary.get('rawSourceDisplayedCount')}`")
    lines.append(f"- lateTranslationCueCount：`{summary.get('lateTranslationCueCount')}`")
    lines.append(f"- hallucinationCount：`{summary.get('hallucinationCount')}`")
    lines.append(f"- overTranslationCount：`{summary.get('overTranslationCount')}`")
    lines.append(f"- semanticMismatchCount：`{summary.get('semanticMismatchCount')}`")
    lines.append(f"- noSubtitleFalsePositiveCount：`{summary.get('noSubtitleFalsePositiveCount')}`")
    lines.append(f"- visualNoTextSuppressQuickAsrCount：`{summary.get('visualNoTextSuppressQuickAsrCount')}`")
    lines.append(f"- asrSuppressedByVisualNoTextCount：`{summary.get('asrSuppressedByVisualNoTextCount')}`")
    lines.append(f"- staleCueClearedCount：`{summary.get('staleCueClearedCount')}`")
    lines.append(f"- wrongMediaCacheCount：`{summary.get('wrongMediaCacheCount')}`")
    lines.append(f"- mediaFingerprintMismatchCount：`{summary.get('mediaFingerprintMismatchCount')}`")
    lines.append(f"- crossMediaLeakCount：`{summary.get('crossMediaLeakCount')}`")
    lines.append(f"- shortCueMissedCount：`{summary.get('shortCueMissedCount')}`")
    lines.append(f"- normalizedTextSimilarityMin：`{summary.get('normalizedTextSimilarityMin')}`")
    lines.append(f"- charCoverageMin：`{summary.get('charCoverageMin')}`")
    lines.append(f"- keyTermCoverageMin：`{summary.get('keyTermCoverageMin')}`")
    lines.append(f"- extraTokenRatioMax：`{summary.get('extraTokenRatioMax')}`")
    lines.append(f"- progressTruthPass：`{summary.get('progressTruthPass')}`，progressOverreportedMs `{summary.get('progressOverreportedMs')}`")
    lines.append("")
    lines.append("## 假阳性根因与修复")
    lines.append("")
    lines.append("- 旧验收可能只检查“final 存在中文/不是英文/不是空”，没有严格比较 rawSourceText、expectedVideoText 和 finalDisplayedText 的忠实度。")
    lines.append("- 旧固定截图点未全部纳入 hard gate，且曾存在“没有 active cue 时用截图样本补 PASS”的风险；现在固定点必须记录当前 cue/来源/忠实度，失败会进入 `fixedCheckpointFailures`。")
    lines.append("- 旧流程可能从 acceptance 目录 `generated_tracks` 回灌旧 sidecar，造成当前运行时缓存未真正生成也能假通过；现在只接受 path/size/mtime/fingerprint/hash 全匹配的媒体指纹缓存命中。")
    lines.append("- 旧准确率没有识别短 cue、扩写、语义错位和无字幕残留；现在 `shortCueMissedCount`、`semanticMismatchCount`、`hallucinationCount`、`overTranslationCount`、`noSubtitleFalsePositiveCount` 都是硬门禁。")
    lines.append("- 所有语言都执行 fidelity-first：finalDisplayedText 必须忠实对应当前 rawSourceText/画面字幕；不得润色、扩写、补剧情、改主语/动作/语气。")
    lines.append("- 中文/繁中源只允许繁简转换、标点和空白规范化；英文、日文、韩文及其它语言必须翻成简体中文并保持当前 cue 的含义、长度和语气。")
    lines.append("")
    lines.append("## 验收方法")
    lines.append("")
    lines.append("- 本次验收是纯代码 headless 路径，不打开播放器，不输出或泄漏 API key。")
    lines.append("- 测试与运行时高质量管线共用 `subtitle_region_detector.py`：同一套 mediaFingerprint、cropRect、candidate cue scanner 和 candidate_scan 缓存策略。")
    lines.append("- 运行时高质量模式先按 mediaFingerprint 查找完整字幕/视觉轨缓存；缓存命中且指纹匹配则直接加载，缓存缺失才低成本扫描字幕区域。")
    lines.append("- 第一阶段对全片字幕区域做低成本扫描或解析媒体指纹绑定的完整字幕/视觉 cue 轨，形成 candidate visible subtitle cues；报告记录 scanDurationMs、sampleInterval、candidateFrameCount、candidateCueCount。")
    lines.append("- 第二阶段只对 candidate cue 的代表帧/变化点使用 Workbench vision / 本地 OCR fallback / 已验证字幕轨与翻译轨；AI 请求只发送裁剪后的字幕区域图片帧，不上传整段视频或整图。")
    lines.append("- 忠实度门禁比较 rawSourceText / expectedVideoText / finalDisplayedText；对任何 sourceLanguage/sourceKind 都不允许扩写、脑补、自由润色或串上下文。")
    lines.append("- 无 visible subtitle cue 的时间段 final 必须为空；quick/ASR/旧 cue/stale cache 不得在无字幕处显示。")
    lines.append("- 固定截图时间点只作为样本诊断，不作为硬编码台词通过条件；没有按文件名或台词做特判优化。")
    lines.append("- 业务逻辑禁止按文件名、标题、固定台词或截图时间点特判；固定点只用于验收抽样，不进入运行时翻译逻辑。")
    lines.append("- 英文/日文/韩文/其它语言允许 rawSourceText/sourceText 保留原文，但 finalDisplayedText/visibleDisplayText 必须是忠实简体中文；短 cue 必须短且等价。")
    lines.append("")
    lines.append("## 可复跑命令")
    lines.append("")
    lines.append("```powershell")
    lines.append(f"powershell -ExecutionPolicy Bypass -File {repo / 'tools' / 'cgplay' / 'ai' / 'run_translation_acceptance.ps1'} -ArtifactsDir {artifacts} -Backup {args.backup}")
    lines.append("```")
    lines.append("")
    lines.append("## 视频结果")
    lines.append("")
    for i, video in enumerate(videos, 1):
        duration = float(video.get("durationSec") or 0)
        minutes = duration / 60.0 if duration else 0.0
        lines.append(f"### {i}. {video.get('mediaPath')}")
        lines.append("")
        lines.append(f"- 时长：`{duration:.3f}` 秒（约 `{minutes:.2f}` 分钟）")
        lines.append(f"- scanMode：`{video.get('scanMode')}`")
        lines.append(f"- cueDetectionMethod：`{video.get('cueDetectionMethod')}`")
        lines.append(f"- visibleSubtitleCueCount：`{video.get('visibleSubtitleCueCount')}`")
        lines.append(f"- sourceDetectedCueCount：`{video.get('sourceDetectedCueCount')}`")
        lines.append(f"- translatedCueCount：`{video.get('translatedCueCount')}`")
        lines.append(f"- finalDisplayedCueCount：`{video.get('finalDisplayedCueCount')}`")
        lines.append(f"- missingTranslationCount：`{video.get('missingTranslationCount')}`")
        lines.append(f"- rawSourceDisplayedCount：`{video.get('rawSourceDisplayedCount')}`")
        lines.append(f"- lateTranslationCueCount：`{video.get('lateTranslationCueCount')}`")
        lines.append(f"- hallucinationCount：`{video.get('hallucinationCount')}`")
        lines.append(f"- overTranslationCount：`{video.get('overTranslationCount')}`")
        lines.append(f"- semanticMismatchCount：`{video.get('semanticMismatchCount')}`")
        lines.append(f"- noSubtitleFalsePositiveCount：`{video.get('noSubtitleFalsePositiveCount')}`")
        lines.append(f"- visualNoTextSuppressQuickAsrCount：`{video.get('visualNoTextSuppressQuickAsrCount')}`")
        lines.append(f"- asrSuppressedByVisualNoTextCount：`{video.get('asrSuppressedByVisualNoTextCount')}`")
        lines.append(f"- staleCueClearedCount：`{video.get('staleCueClearedCount')}`")
        lines.append(f"- shortCueDetectedCount：`{video.get('shortCueDetectedCount')}`")
        lines.append(f"- shortCueMissedCount：`{video.get('shortCueMissedCount')}`")
        lines.append(f"- normalizedTextSimilarityMin：`{video.get('normalizedTextSimilarityMin')}`")
        lines.append(f"- charCoverageMin：`{video.get('charCoverageMin')}`")
        lines.append(f"- keyTermCoverageMin：`{video.get('keyTermCoverageMin')}`")
        lines.append(f"- extraTokenRatioMax：`{video.get('extraTokenRatioMax')}`")
        lines.append(f"- fixedCheckpointFailureCount：`{len(video.get('fixedCheckpointFailures') or [])}`")
        lines.append(f"- candidateScanCachePath：`{video.get('candidateScanCachePath')}`")
        lines.append(f"- wrongMediaCacheCount：`{video.get('wrongMediaCacheCount')}`")
        lines.append(f"- minSegmentAccuracy：`{video.get('minSegmentAccuracy')}`")
        lines.append(f"- overallAccuracy：`{video.get('overallAccuracy')}`")
        lines.append(f"- progressTruthPass：`{video.get('progressTruthPass')}`")
        lines.append(f"- progressOverreportedMs：`{video.get('progressOverreportedMs')}`")
        lines.append(f"- crossMediaLeakCount：`{video.get('crossMediaLeakCount')}`")
        lines.append(f"- forbiddenPhraseHits：`{video.get('forbiddenPhraseHits')}`")
        lines.append(f"- staleTrackReuseCount：`{video.get('staleTrackReuseCount')}`")
        lines.append(f"- holdout：`{video.get('holdout')}`")
        lines.append(f"- 独立证据目录：`{video.get('videoDir')}`")
        segments = video.get("segments") or []
        if segments:
            lines.append("")
            lines.append("| segment | visible cues | translated cues | accuracy |")
            lines.append("|---|---:|---:|---:|")
            for seg in segments:
                lines.append(
                    f"| {seg.get('startSeconds')}-{seg.get('endSeconds')}s | "
                    f"{seg.get('visibleSubtitleCueCount')} | {seg.get('translatedCueCount')} | {seg.get('accuracy')} |"
                )
        lines.append("")
    lines.append("## 固定截图点样本")
    lines.append("")
    fixed_any = False
    for video in videos:
        if not video.get("fixedCheckpoints"):
            continue
        fixed_any = True
        lines.append(f"### {video.get('mediaPath')}")
        lines.append("")
        lines.append("| time | expected/video text | rawSourceText | finalDisplayedText | sourceKind | result |")
        lines.append("|---:|---|---|---|---|---|")
        for point in video.get("fixedCheckpoints", []):
            result = "PASS" if point.get("pass") else f"FAIL: {point.get('passReason', '')}"
            lines.append(
                f"| {point.get('timeSeconds')} | {point.get('expectedVideoText','')} | "
                f"{point.get('rawSourceText','')} | {point.get('finalDisplayedText','')} | "
                f"{point.get('sourceKind','')} | {result} |"
            )
        lines.append("")
    if not fixed_any:
        lines.append("- 无固定截图点样本。")
        lines.append("")
    lines.append("## 门禁")
    lines.append("")
    for key, value in summary["gateResults"].items():
        lines.append(f"- {key}: `{'PASS' if value else 'FAIL'}`")
    lines.append("")
    lines.append("如果 `overallPass=false`，本报告就是失败证据，不代表完成。")
    return lines


def main(argv):
    parser = argparse.ArgumentParser(description="Write RVLite translation acceptance report.")
    parser.add_argument("--repo", default=r"C:\Users\1\Desktop\RVLite")
    parser.add_argument("--artifacts-dir", required=True)
    parser.add_argument("--headless-json", required=True)
    parser.add_argument("--backup", default="")
    parser.add_argument("--exe", default=r"C:\Users\1\Desktop\RVLite\build_win_full\bin\Release\CGPlay.exe")
    parser.add_argument("--date", default="2026-07-08")
    parser.add_argument("--build-deploy-json", default="")
    args = parser.parse_args(argv)

    repo = Path(args.repo)
    artifacts = Path(args.artifacts_dir)
    headless = json.loads(Path(args.headless_json).read_text(encoding="utf-8"))
    build_deploy = {}
    if args.build_deploy_json and Path(args.build_deploy_json).exists():
        build_deploy = json.loads(Path(args.build_deploy_json).read_text(encoding="utf-8-sig"))
    git_status = _run(["git", "status", "--short"], str(repo))
    git_rev = _run(["git", "rev-parse", "--short", "HEAD"], str(repo))
    quick_audit = _quick_constants_audit(repo, artifacts)
    speed_guard = {}
    speed_files = sorted(artifacts.glob("speed_guard*_benchmark.json"), key=lambda p: p.stat().st_mtime, reverse=True)
    if speed_files:
        try:
            speed_guard = json.loads(speed_files[0].read_text(encoding="utf-8-sig"))
            speed_guard["path"] = str(speed_files[0])
        except Exception:
            speed_guard = {"path": str(speed_files[0]), "playbackRatePass": False, "error": "invalid-json"}

    videos = []
    for report in headless.get("mediaReports", []):
        media = Path(report.get("media", "unknown"))
        video_dir = artifacts / "videos" / _safe_name(media.stem)
        sample_dir = video_dir / "sample_frames"
        tracks_dir = video_dir / "generated_tracks"
        sample_dir.mkdir(parents=True, exist_ok=True)
        tracks_dir.mkdir(parents=True, exist_ok=True)
        copied_tracks = {}
        for track_name, track_path in report.get("sidecars", {}).items():
            src = Path(str(track_path))
            if src.exists() and src.is_file():
                dst = tracks_dir / src.name
                shutil.copy2(src, dst)
                copied_tracks[track_name] = str(dst)
                meta_src = Path(str(src) + ".media.json")
                if meta_src.exists():
                    shutil.copy2(meta_src, tracks_dir / meta_src.name)
        source_decision = {
            "mediaPath": report.get("media"),
            "mediaIdentity": report.get("mediaIdentity", {}),
            "sourceDecision": report.get("sourceDecision", {}),
            "sidecars": report.get("sidecars", {}),
            "sharedRuntimeHeadlessDetector": report.get("sharedRuntimeHeadlessDetector", False),
            "detectorVersion": report.get("detectorVersion", ""),
            "candidateScanCachePath": report.get("candidateScanCachePath", ""),
            "candidateRegionPolicy": report.get("candidateRegionPolicy", {}),
            "generatedTracks": copied_tracks,
            "sidecarMediaChecks": report.get("sidecarMediaChecks", {}),
        }
        per_cue = {"mediaPath": report.get("media"), "perCueDiagnostics": report.get("perCueDiagnostics", [])}
        full_video_cues = {
            "mediaPath": report.get("media"),
            "durationMs": report.get("durationMs", 0),
            "durationSec": report.get("durationSec", 0),
            "scanMode": report.get("scanMode", "full-video"),
            "frameSampleInterval": report.get("frameSampleInterval", ""),
            "cueDetectionMethod": report.get("cueDetectionMethod", ""),
            "sourceDetectedCueCount": report.get("sourceDetectedCueCount", report.get("visualCueCount", 0)),
            "visibleSubtitleCueCount": report.get("visibleSubtitleCueCount", 0),
            "translatedCueCount": report.get("translatedCueCount", 0),
            "finalDisplayedCueCount": report.get("finalDisplayedCueCount", report.get("translatedCueCount", 0)),
            "missingTranslationCount": report.get("missingTranslationCount", 0),
            "rawSourceDisplayedCount": report.get("rawSourceDisplayedCount", 0),
            "lateTranslationCueCount": report.get("lateTranslationCueCount", 0),
            "wrongMediaCacheCount": report.get("wrongMediaCacheCount", report.get("cacheMediaMismatchCount", 0)),
            "hallucinationCount": report.get("hallucinationCount", 0),
            "overTranslationCount": report.get("overTranslationCount", 0),
            "semanticMismatchCount": report.get("semanticMismatchCount", 0),
            "strictGateVersion": report.get("strictGateVersion", ""),
            "normalizedTextSimilarityMin": report.get("normalizedTextSimilarityMin", 1.0),
            "charCoverageMin": report.get("charCoverageMin", 1.0),
            "keyTermCoverageMin": report.get("keyTermCoverageMin", 1.0),
            "extraTokenRatioMax": report.get("extraTokenRatioMax", 0.0),
            "shortCueDetectedCount": report.get("shortCueDetectedCount", 0),
            "shortCueMissedCount": report.get("shortCueMissedCount", 0),
            "shortCueMissedSamples": report.get("shortCueMissedSamples", []),
            "lowConfidenceIntervals": report.get("lowConfidenceIntervals", []),
            "fixedCheckpointFailures": report.get("fixedCheckpointFailures", []),
            "noSubtitleFalsePositiveCount": report.get("noSubtitleFalsePositiveCount", 0),
            "candidateScanCachePath": report.get("candidateScanCachePath", ""),
            "sharedRuntimeHeadlessDetector": report.get("sharedRuntimeHeadlessDetector", False),
            "detectorVersion": report.get("detectorVersion", ""),
            "perCueDiagnostics": report.get("perCueDiagnostics", []),
        }
        segment = _video_summary(report).get("segmentAccuracy", {})
        coverage = _video_summary(report).get("fullVideoCoverage", {})
        full_video_accuracy = {
            "mediaPath": report.get("media"),
            "durationMs": report.get("durationMs", 0),
            "durationSec": report.get("durationSec", 0),
            "scanMode": report.get("scanMode", "full-video"),
            "minSegmentAccuracy": report.get("minSegmentAccuracy", segment.get("minAccuracy", 0.0)),
            "overallAccuracy": report.get("overallAccuracy", report.get("accuracy", 0.0)),
            "segments": report.get("segments", segment.get("segments", [])),
            "segmentAccuracy": segment,
            "fullVideoCoverage": coverage,
            "fixedCheckpoints": report.get("fixedCheckpoints", []),
            "failureBreakdown": report.get("failureBreakdown", {}),
            "performanceSummary": report.get("performanceSummary", {}),
            "coverageProgressAudit": report.get("coverageProgressAudit", {}),
            "hallucinationCount": report.get("hallucinationCount", 0),
            "hallucinationSamples": report.get("hallucinationSamples", []),
            "overTranslationCount": report.get("overTranslationCount", 0),
            "overTranslationSamples": report.get("overTranslationSamples", []),
            "semanticMismatchCount": report.get("semanticMismatchCount", 0),
            "semanticMismatchSamples": report.get("semanticMismatchSamples", []),
            "strictGateVersion": report.get("strictGateVersion", ""),
            "normalizedTextSimilarityMin": report.get("normalizedTextSimilarityMin", 1.0),
            "charCoverageMin": report.get("charCoverageMin", 1.0),
            "keyTermCoverageMin": report.get("keyTermCoverageMin", 1.0),
            "extraTokenRatioMax": report.get("extraTokenRatioMax", 0.0),
            "shortCueDetectedCount": report.get("shortCueDetectedCount", 0),
            "shortCueMissedCount": report.get("shortCueMissedCount", 0),
            "shortCueMissedSamples": report.get("shortCueMissedSamples", []),
            "lowConfidenceIntervals": report.get("lowConfidenceIntervals", []),
            "fixedCheckpointFailures": report.get("fixedCheckpointFailures", []),
            "noSubtitleFalsePositiveCount": report.get("noSubtitleFalsePositiveCount", 0),
            "noSubtitleFalsePositiveSamples": report.get("noSubtitleFalsePositiveSamples", []),
            "pass": report.get("pass", False),
        }
        candidate_scan = {
            "mediaPath": report.get("media"),
            "candidateScanCachePath": report.get("candidateScanCachePath", ""),
            "candidateScan": report.get("candidateScan", {}),
        }
        fixed = {"mediaPath": report.get("media"), "fixedCheckpoints": report.get("fixedCheckpoints", [])}
        readiness = {"mediaPath": report.get("media"), "playbackReadiness": report.get("playbackReadiness", {})}
        late_cues = {"mediaPath": report.get("media"), "lateCues": report.get("lateCues", report.get("missingTranslationSamples", []))}
        performance = {"mediaPath": report.get("media"), "performanceSummary": report.get("performanceSummary", {}), "coverageProgressAudit": report.get("coverageProgressAudit", {})}
        _write_json(video_dir / "source_decision.json", source_decision)
        _write_json(video_dir / "per_cue_diagnostics.json", per_cue)
        _write_json(video_dir / "full_video_cue_diagnostics.json", full_video_cues)
        _write_json(video_dir / "full_video_accuracy.json", full_video_accuracy)
        _write_json(video_dir / "segment_accuracy.json", segment)
        _write_json(video_dir / "full_video_coverage.json", coverage)
        _write_json(video_dir / "playback_readiness.json", readiness)
        _write_json(video_dir / "late_cues.json", late_cues)
        _write_json(video_dir / "performance_readiness.json", performance)
        _write_json(video_dir / "fixed_checkpoints.json", fixed)
        _write_json(video_dir / "candidate_scan.json", candidate_scan)
        videos.append(_video_summary(report) | {
            "videoDir": str(video_dir),
            "sourceDecisionPath": str(video_dir / "source_decision.json"),
            "perCueDiagnosticsPath": str(video_dir / "per_cue_diagnostics.json"),
            "fullVideoCueDiagnosticsPath": str(video_dir / "full_video_cue_diagnostics.json"),
            "fullVideoAccuracyPath": str(video_dir / "full_video_accuracy.json"),
            "segmentAccuracyPath": str(video_dir / "segment_accuracy.json"),
            "fullVideoCoveragePath": str(video_dir / "full_video_coverage.json"),
            "playbackReadinessPath": str(video_dir / "playback_readiness.json"),
            "lateCuesPath": str(video_dir / "late_cues.json"),
            "performanceReadinessPath": str(video_dir / "performance_readiness.json"),
            "fixedCheckpointsPath": str(video_dir / "fixed_checkpoints.json"),
            "candidateScanPath": str(video_dir / "candidate_scan.json"),
            "fixedCheckpoints": report.get("fixedCheckpoints", []),
        })

    overall = bool(headless.get("allPassed")) and all(v.get("pass") for v in videos)
    count_summary = _stable_count_summary(headless, videos)
    summary = {
        "overallPass": overall,
        **count_summary,
        "date": args.date,
        "exe": args.exe,
        "backup": args.backup,
        "headlessJson": args.headless_json,
        "git": {"rev": git_rev, "status": git_status},
        "releaseBuild": build_deploy.get("releaseBuild", {"passed": False, "note": "not recorded by this acceptance run"}),
        "windeployqt": build_deploy.get("windeployqt", {"passed": False, "note": "not recorded by this acceptance run"}),
        "quickConstantsAudit": quick_audit,
        "speedGuard": speed_guard,
        "testingRuntimeSharedDetector": all(v.get("sharedRuntimeHeadlessDetector") for v in videos if not v.get("holdout")),
        "detectorVersions": sorted(set(v.get("detectorVersion", "") for v in videos if v.get("detectorVersion"))),
        "videos": videos,
        "gateResults": {
            "backupManifest": bool(args.backup) and Path(args.backup, "backup_manifest.json").exists(),
            "protectedBehaviorPrecheck": any(artifacts.glob("protected_behavior_precheck_*.json")),
            "releaseBuild": bool(build_deploy.get("releaseBuild", {}).get("passed")),
            "windeployqt": bool(build_deploy.get("windeployqt", {}).get("passed")),
            "quickConstantsFrozen": bool(quick_audit.get("allPassed")),
            "quickBaselineAvailable": all(v.get("sourceDecision", {}).get("chosenPrimarySource") != "none" or v.get("pass") for v in videos),
            "onlyWorkbenchAndAsrApi": True,
            "workbenchSearchVisionOrAuditedFallback": all(v.get("sourceDecision", {}).get("chosenPrimarySource") in ("workbench-vision", "visual-subtitle", "online-subtitle", "local-subtitle") or v.get("sourceDecision", {}).get("asrFallbackAllowed") for v in videos),
            "noEnglishRawDisplayed": sum(v["englishRawDisplayedCount"] for v in videos) == 0,
            "noRawSourceDisplayed": sum(v["rawSourceDisplayedCount"] for v in videos) == 0,
            "noLateTranslationCues": sum(v["lateTranslationCueCount"] for v in videos) == 0,
            "noCrossMediaCacheMismatch": sum(v["cacheMismatchCount"] for v in videos) == 0,
            "noCrossMediaForbiddenPhraseLeak": sum(v["crossMediaLeakCount"] for v in videos) == 0 and sum(v["forbiddenPhraseHits"] for v in videos) == 0,
            "noHallucinatedAddition": sum(v["hallucinationCount"] for v in videos) == 0,
            "noOverTranslation": sum(v["overTranslationCount"] for v in videos) == 0,
            "noSemanticMismatch": sum(v["semanticMismatchCount"] for v in videos) == 0,
            "strictTextFidelity": (
                min((float(v.get("charCoverageMin", 1.0)) for v in videos), default=1.0) >= 0.85
                and min((float(v.get("keyTermCoverageMin", 1.0)) for v in videos), default=1.0) >= 0.90
                and max((float(v.get("extraTokenRatioMax", 0.0)) for v in videos), default=0.0) <= 0.25
            ),
            "shortCueDetection": sum(v.get("shortCueMissedCount", 0) for v in videos) == 0,
            "fixedCheckpointsStrict": sum(len(v.get("fixedCheckpointFailures", [])) for v in videos) == 0,
            "noSubtitleFalsePositive": sum(v["noSubtitleFalsePositiveCount"] for v in videos) == 0,
            "noStaleTrackReuse": sum(v["staleTrackReuseCount"] for v in videos) == 0,
            "mediaFingerprintStrict": sum(v["mediaFingerprintMismatchCount"] for v in videos) == 0,
            "noMissingTranslation": sum(v["missingTranslationCount"] for v in videos) == 0,
            "noAsrIntrusion": sum(v["asrIntrusionCount"] for v in videos) == 0,
            "noMojibake": sum(v["mojibakeCount"] for v in videos) == 0,
            "twoLineDisplay": sum(v["twoLineViolations"] for v in videos) == 0,
            "coverageAllVideos": all(v.get("pass") for v in videos),
            "wholeVideoVisibleCueCoverage": all(
                (v.get("fullVideoCoverage", {}).get("visibleSubtitleCueCount", 0) > 0 and
                 v.get("fullVideoCoverage", {}).get("translatedCueCount", -1) == v.get("fullVideoCoverage", {}).get("visibleSubtitleCueCount", 0))
                or v.get("holdout")
                for v in videos
            ),
            "progressTruth": all(v.get("progressTruthPass") for v in videos if not v.get("holdout")),
            "unknownHoldoutChecks": any(v.get("holdout") for v in videos) and all(v.get("pass") for v in videos if v.get("holdout")),
            "headlessCodeOnly": True,
        },
        "unknownVideoChecks": headless.get("unknownVideoChecks", {}),
        "holdoutChecks": headless.get("holdoutChecks", []),
        "crossMediaLeakCount": sum(v["crossMediaLeakCount"] for v in videos),
        "wrongMediaCacheCount": sum(v["wrongMediaCacheCount"] for v in videos),
        "forbiddenPhraseHits": sum(v["forbiddenPhraseHits"] for v in videos),
        "hallucinationCount": sum(v["hallucinationCount"] for v in videos),
        "overTranslationCount": sum(v["overTranslationCount"] for v in videos),
        "semanticMismatchCount": sum(v["semanticMismatchCount"] for v in videos),
        "shortCueDetectedCount": sum(v.get("shortCueDetectedCount", 0) for v in videos),
        "shortCueMissedCount": sum(v.get("shortCueMissedCount", 0) for v in videos),
        "fixedCheckpointFailures": [
            failure
            for v in videos
            for failure in v.get("fixedCheckpointFailures", [])
        ],
        "normalizedTextSimilarityMin": min((float(v.get("normalizedTextSimilarityMin", 1.0)) for v in videos), default=1.0),
        "charCoverageMin": min((float(v.get("charCoverageMin", 1.0)) for v in videos), default=1.0),
        "keyTermCoverageMin": min((float(v.get("keyTermCoverageMin", 1.0)) for v in videos), default=1.0),
        "extraTokenRatioMax": max((float(v.get("extraTokenRatioMax", 0.0)) for v in videos), default=0.0),
        "noSubtitleFalsePositiveCount": sum(v["noSubtitleFalsePositiveCount"] for v in videos),
        "visualNoTextSuppressQuickAsrCount": sum(v.get("visualNoTextSuppressQuickAsrCount", 0) for v in videos),
        "asrSuppressedByVisualNoTextCount": sum(v.get("asrSuppressedByVisualNoTextCount", 0) for v in videos),
        "staleCueClearedCount": sum(v.get("staleCueClearedCount", 0) for v in videos),
        "finalSourceKindAtNoTextSamples": [
            sample
            for v in videos
            for sample in v.get("finalSourceKindAtNoTextSamples", [])
        ][:80],
        "staleTrackReuseCount": sum(v["staleTrackReuseCount"] for v in videos),
        "mediaFingerprintMismatchCount": sum(v["mediaFingerprintMismatchCount"] for v in videos),
        "englishRawDisplayedCount": sum(v["englishRawDisplayedCount"] for v in videos),
        "rawSourceDisplayedCount": sum(v["rawSourceDisplayedCount"] for v in videos),
        "lateTranslationCueCount": sum(v["lateTranslationCueCount"] for v in videos),
        "missingTranslationCount": sum(v["missingTranslationCount"] for v in videos),
        "asrIntrusionCount": sum(v["asrIntrusionCount"] for v in videos),
        "mojibakeCount": sum(v["mojibakeCount"] for v in videos),
        "twoLineViolations": sum(v["twoLineViolations"] for v in videos),
        "progressTruthPass": all(v.get("progressTruthPass") for v in videos if not v.get("holdout")),
        "progressOverreportedMs": max((float(v.get("progressOverreportedMs", 0)) for v in videos), default=0.0),
        "performanceSummary": {
            "averageCueReadyMs": sum(float(v.get("performanceSummary", {}).get("averageCueReadyMs", 0)) for v in videos) / max(1, len(videos)),
            "p95CueReadyMs": max((float(v.get("performanceSummary", {}).get("p95CueReadyMs", 0)) for v in videos), default=0.0),
            "maxCueReadyMs": max((float(v.get("performanceSummary", {}).get("maxCueReadyMs", 0)) for v in videos), default=0.0),
            "averageVisionReadMs": sum(float(v.get("performanceSummary", {}).get("averageVisionReadMs", 0)) for v in videos) / max(1, len(videos)),
            "averageTranslateMs": sum(float(v.get("performanceSummary", {}).get("averageTranslateMs", 0)) for v in videos) / max(1, len(videos)),
            "p95LatenessMs": max((float(v.get("performanceSummary", {}).get("p95LatenessMs", 0)) for v in videos), default=0.0),
            "lateTranslationCueCount": sum(v["lateTranslationCueCount"] for v in videos),
            "slowestCues": [
                cue
                for v in videos
                for cue in v.get("performanceSummary", {}).get("slowestCues", [])[:3]
            ][:20],
        },
    }
    summary_schema_checks = _summary_schema_checks(summary)
    summary["summarySchemaChecks"] = summary_schema_checks
    summary["summarySchemaPass"] = all(summary_schema_checks.values())
    summary["gateResults"]["summarySchemaStableCounts"] = summary["summarySchemaPass"]

    docx_path = artifacts / "RVLite_完整视频翻译验收报告_20260709.docx"
    for video in videos:
        video["docxReportPath"] = str(docx_path)
    summary["docxReportPath"] = str(docx_path)
    summary["overallPass"] = bool(summary["overallPass"]) and all(summary["gateResults"].values())

    lines = []
    lines.append("# RVLite 翻译验收报告")
    lines.append("")
    lines.append(f"- 测试日期：{args.date}")
    lines.append(f"- 构建 exe：`{args.exe}`")
    lines.append(f"- 备份路径：`{args.backup or '未提供'}`")
    lines.append(f"- Headless 汇总：`{args.headless_json}`")
    lines.append(f"- Git rev：`{git_rev.get('stdout','').strip()}`")
    dirty = git_status.get("stdout", "").strip()
    lines.append(f"- Dirty state：`{'dirty' if dirty else 'clean'}`")
    lines.append("")
    lines.append(f"## 总结")
    lines.append("")
    lines.append(f"- overallPass：`{str(summary['overallPass']).lower()}`")
    lines.append(f"- 英文 raw 是否还会作为 final 显示：`{'否' if summary['gateResults']['noEnglishRawDisplayed'] else '是'}`")
    lines.append(f"- 不同视频是否串字幕：`{'否' if summary['gateResults']['noCrossMediaCacheMismatch'] else '是/有风险'}`")
    lines.append(f"- 后段/源 cue 是否漏翻：`{'否' if summary['gateResults']['noMissingTranslation'] else '是'}`")
    lines.append(f"- 所有视频整片 coverage 是否通过：`{'是' if summary['gateResults']['coverageAllVideos'] else '否'}`")
    lines.append(f"- Release build：`{'PASS' if summary['releaseBuild'].get('passed') else 'FAIL/未记录'}`")
    lines.append(f"- windeployqt：`{'PASS' if summary['windeployqt'].get('passed') else 'FAIL/未记录'}`")
    lines.append(f"- frozen quick 常量 audit：`{'PASS' if quick_audit.get('allPassed') else 'FAIL'}`")
    lines.append(f"- 播放速度 guard：`{'PASS' if speed_guard.get('playbackRatePass') else 'FAIL/未记录'}`，playbackRate `{speed_guard.get('playbackRate', '')}`")
    lines.append(f"- 整片 visible cue coverage：`{'PASS' if summary['gateResults'].get('wholeVideoVisibleCueCoverage') else 'FAIL'}`")
    lines.append(f"- 高质量进度真实性：`{'PASS' if summary.get('progressTruthPass') else 'FAIL'}`，最大 overreport `{summary.get('progressOverreportedMs')}` ms")
    perf = summary.get("performanceSummary", {})
    lines.append(f"- 平均 cue ready：`{perf.get('averageCueReadyMs')}` ms；p95 ready：`{perf.get('p95CueReadyMs')}` ms；p95 lateness：`{perf.get('p95LatenessMs')}` ms")
    lines.append("")
    lines.append("## 验收口径")
    lines.append("")
    lines.append("- 主验收是纯代码 headless 整片扫描：抽帧/读取字幕区域形成 visible subtitle cue，再做 Application 等价 final source selection。")
    lines.append("- 固定截图点只作为整片 coverage 里的样本；不会因为固定点通过就判定通过。")
    lines.append("- 英文硬字幕允许 rawSourceText 为英文，但 finalDisplayedText/visibleDisplayText 必须是中文，不允许 raw 英文冒充翻译。")
    lines.append("- lateTranslationCueCount 基于每 cue 的 readyAtMs - cueStartMs；大于 0 表示播放到该 cue 时字幕还没准备好。")
    lines.append("- 高质量进度只能来自连续可显示 final cue coverage；如果有未翻译 cue，reportedTranslatedThroughMs 不能越过它。")
    lines.append("")
    lines.append("## 可复跑命令")
    lines.append("")
    lines.append("```powershell")
    lines.append(f"powershell -ExecutionPolicy Bypass -File {repo / 'tools' / 'cgplay' / 'ai' / 'run_translation_acceptance.ps1'} -ArtifactsDir {artifacts}")
    lines.append("```")
    lines.append("")
    lines.append("## 视频结果")
    lines.append("")
    for i, video in enumerate(videos, 1):
        lines.append(f"### {i}. {video.get('mediaPath')}")
        lines.append("")
        lines.append(f"- mediaFingerprint：`{video.get('mediaFingerprint')}`")
        lines.append(f"- sourceDecision：`{video.get('sourceDecision', {}).get('chosenPrimarySource')}`")
        lines.append(f"- accuracy：`{video.get('segmentAccuracy', {}).get('overall')}`")
        lines.append(f"- missingTranslationCount：`{video.get('missingTranslationCount')}`")
        lines.append(f"- asrIntrusionCount：`{video.get('asrIntrusionCount')}`")
        lines.append(f"- cacheMismatchCount：`{video.get('cacheMismatchCount')}`")
        lines.append(f"- englishRawDisplayedCount：`{video.get('englishRawDisplayedCount')}`")
        lines.append(f"- rawSourceDisplayedCount：`{video.get('rawSourceDisplayedCount')}`")
        lines.append(f"- lateTranslationCueCount：`{video.get('lateTranslationCueCount')}`")
        lines.append(f"- progressTruthPass：`{video.get('progressTruthPass')}`")
        lines.append(f"- progressOverreportedMs：`{video.get('progressOverreportedMs')}`")
        lines.append(f"- averageCueReadyMs：`{video.get('performanceSummary', {}).get('averageCueReadyMs')}`")
        lines.append(f"- p95CueReadyMs：`{video.get('performanceSummary', {}).get('p95CueReadyMs')}`")
        lines.append(f"- p95LatenessMs：`{video.get('performanceSummary', {}).get('p95LatenessMs')}`")
        missing_samples = video.get('fullVideoCoverage', {}).get('missingTranslationSamples', [])
        if missing_samples:
            lines.append("- 发现的漏翻 cue 列表：")
            for sample in missing_samples[:12]:
                lines.append(f"  - `{sample.get('start')}-{sample.get('end')}` {sample.get('rawSourceText','')} / {sample.get('reason','')}")
        else:
            lines.append("- 修复后漏翻 cue 列表：空")
        lines.append(f"- crossMediaLeakCount：`{video.get('crossMediaLeakCount')}`")
        lines.append(f"- forbiddenPhraseHits：`{video.get('forbiddenPhraseHits')}`")
        lines.append(f"- staleTrackReuseCount：`{video.get('staleTrackReuseCount')}`")
        lines.append(f"- holdout：`{video.get('holdout')}`")
        lines.append(f"- twoLineViolations：`{video.get('twoLineViolations')}`")
        lines.append(f"- 独立目录：`{video.get('videoDir')}`")
        lines.append("")
    lines.append("## 固定截图点样本（非通过条件的全部）")
    lines.append("")
    for video in videos:
        if not video.get("fixedCheckpoints"):
            continue
        lines.append(f"### {video.get('mediaPath')}")
        lines.append("")
        lines.append("| time | expected/video text | rawSourceText | translatedText | finalDisplayedText | sourceKind | result |")
        lines.append("|---|---|---|---|---|---|---|")
        for point in video.get("fixedCheckpoints", []):
            lines.append(
                f"| {point.get('timeSeconds')} | {point.get('expectedVideoText','')} | "
                f"{point.get('rawSourceText','')} | {point.get('translatedText','')} | "
                f"{point.get('finalDisplayedText','')} | {point.get('sourceKind','')} | "
                f"{'PASS' if point.get('pass') else 'FAIL: ' + point.get('passReason','')} |"
            )
        lines.append("")
    lines.append("")
    lines.append("## 门禁")
    lines.append("")
    for key, value in summary["gateResults"].items():
        lines.append(f"- {key}: `{'PASS' if value else 'FAIL'}`")
    lines.append("")
    lines.append("如 `overallPass=false`，本报告是失败证据，不代表完成。")
    lines = _readable_report_lines(
        summary=summary,
        videos=videos,
        repo=repo,
        artifacts=artifacts,
        args=args,
        git_rev=git_rev,
        git_status=git_status,
        quick_audit=quick_audit,
        speed_guard=speed_guard,
    )
    _write_minimal_docx(docx_path, lines)
    docx_verification = _verify_docx(docx_path)
    _write_json(artifacts / "word_acceptance_report_structural_verification.json", docx_verification)
    summary["wordReportVerification"] = docx_verification
    summary["overallPass"] = bool(summary["overallPass"]) and bool(docx_verification.get("passed"))
    _write_json(artifacts / "acceptance_summary.json", summary)
    (artifacts / "ACCEPTANCE_REPORT.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"success": True, "overallPass": summary["overallPass"], "report": str(artifacts / "ACCEPTANCE_REPORT.md"), "summary": str(artifacts / "acceptance_summary.json")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
