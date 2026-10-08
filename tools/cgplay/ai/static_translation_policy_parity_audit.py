#!/usr/bin/env python3
"""Static parity audit for runtime and headless final-display text policy.

The audit intentionally stays source-based so it can run without opening the app.
It checks that the C++ runtime helper and Python headless verifier keep the same
critical rejection classes: target CJK required, kana/replacement rejected, raw
Latin sentences rejected, placeholders rejected, and watermark-like text rejected.
"""

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def compact(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.lower())


def has_all_tokens(text: str, tokens: List[str]) -> bool:
    compact_text = compact(text)
    compact_tokens = [compact(token) for token in tokens]
    return all(compact_tokens) and all(token in compact_text for token in compact_tokens)


def has_all_literals(text: str, tokens: List[str]) -> bool:
    return all(token in text for token in tokens)


def has_regex(text: str, pattern: str) -> bool:
    return re.search(pattern, text, flags=re.MULTILINE) is not None


def has_tokens_in_order(text: str, tokens: List[str]) -> bool:
    compact_text = compact(text)
    compact_tokens = [compact(token) for token in tokens]
    if not all(compact_tokens):
        return False
    cursor = 0
    for token in compact_tokens:
        found = compact_text.find(token, cursor)
        if found < 0:
            return False
        cursor = found + len(token)
    return True


def audit(repo: Path) -> Dict[str, object]:
    cpp_path = repo / "src" / "services" / "ai" / "TranslationTextPolicy.cpp"
    headless_path = repo / "tools" / "cgplay" / "ai" / "headless_translation_gate.py"
    scheduler_path = repo / "src" / "services" / "ai" / "TranslationEnhancementScheduler.cpp"
    scheduler_support_path = repo / "src" / "services" / "ai" / "TranslationEnhancementSupport.cpp"
    scheduler_support_header_path = repo / "src" / "services" / "ai" / "TranslationEnhancementSupport.h"
    subtitle_service_path = repo / "src" / "services" / "ai" / "SubtitleGenerationService.cpp"
    application_path = repo / "src" / "ui" / "app" / "Application.cpp"
    acceptance_report_path = repo / "tools" / "cgplay" / "ai" / "write_acceptance_report.py"
    visual_translate_path = repo / "tools" / "cgplay" / "ai" / "translate_visual_source_cache.py"

    cpp = read_text(cpp_path) if cpp_path.exists() else ""
    headless = read_text(headless_path) if headless_path.exists() else ""
    scheduler = "\n".join(
        read_text(path)
        for path in (scheduler_support_header_path, scheduler_support_path, scheduler_path)
        if path.exists()
    )
    subtitle_service = read_text(subtitle_service_path) if subtitle_service_path.exists() else ""
    application = read_text(application_path) if application_path.exists() else ""
    acceptance_report = read_text(acceptance_report_path) if acceptance_report_path.exists() else ""
    visual_translate = read_text(visual_translate_path) if visual_translate_path.exists() else ""

    common_placeholder_tokens = ["translation pending", "placeholder"]
    common_watermark_tokens = ["leveling animation", "animation partners"]
    python_reason_tokens = [
        "translated-visual-cue",
        "literal-cjk-visual-cue",
        "translated-enhanced-visual-cue",
        "quick-baseline-for-current-visual-source",
        "quick-asr-baseline",
        "quick-baseline-for-text-subtitle",
    ]
    cpp_policy_functions = [
        "containsHan",
        "containsKanaOrReplacement",
        "containsLatinSentence",
        "isPlaceholderTranslationText",
        "isLikelyVisualWatermarkText",
        "isFinalDisplayableText",
    ]
    python_policy_functions = [
        "contains_cjk",
        "contains_kana_or_replacement",
        "contains_latin_sentence",
        "is_placeholder_translation",
        "is_watermark_translation",
        "is_usable_final_text",
        "choose_final_cue",
        "audit_final_cue",
    ]
    scheduler_reporting_helpers = [
        "activeEnhancementSource",
        "enhancementFallbackReason",
        "visualFallbackReport",
        "visualPrecheckChosenReason",
        "reportedVisualCueSourceKind",
        "reportedCurrentFrameSourceKind",
        "activeHighQualitySource",
    ]
    scheduler_visual_reason_tokens = [
        "visual-no-text-suppress-quick-asr",
        "outside-visual-coverage-quick-baseline-allowed",
        "visual-no-current-cue-quick-baseline-allowed",
        "local-ocr-no-displayable-text",
        "workbench-vision-not-configured",
        "visual-cue-text-missing-suppress-quick-asr",
        "visual-subtitle-text-detected",
    ]
    scheduler_source_tokens = [
        "repair-cache",
        "long-asr-corrected",
        "hard-sub-ocr",
        "online-subtitle",
        "quick-fallback",
        "enhanced",
        "visual-subtitle",
        "workbench-vision",
        "visual-no-text",
        "quick-baseline-allowed-outside-visual-coverage",
    ]
    scheduler_visual_precheck_schema_tokens = [
        "visualPrecheck",
        "chosenReason",
        "fallbackReason",
        "finalDisplayedText",
        "visibleDisplayText",
        "sourceKind",
        "chosenSource",
        "visualNoTextSuppressQuickAsr",
        "asrSuppressedByVisualNoText",
        "staleCueCleared",
    ]
    scheduler_final_display_schema_tokens = [
        "finalDisplayedText",
        "visibleDisplayText",
        "activeHqSource",
        "hqTargetCoverageReady",
        "fallbackReason",
        "needReason",
        "visualPrecheck",
    ]
    scheduler_helper_definition_patterns = [
        r"QString\s+activeEnhancementSource\s*\(",
        r"QString\s+enhancementFallbackReason\s*\(",
        r"struct\s+VisualFallbackReport",
        r"VisualFallbackReport\s+visualFallbackReport\s*\(",
        r"QString\s+visualPrecheckChosenReason\s*\(",
        r"QString\s+reportedVisualCueSourceKind\s*\(",
        r"QString\s+reportedCurrentFrameSourceKind\s*\(",
        r"QString\s+activeHighQualitySource\s*\(",
    ]
    scheduler_hq_source_priority_tokens = [
        "activeHighQualitySource",
        "manualFusionPrimarySource",
        "enhanced",
        "online-subtitle",
        "hard-sub-ocr",
        "long-asr-corrected",
        "quick-fallback",
    ]
    scheduler_visual_precheck_helper_tokens = [
        "visualPrecheck",
        "chosenReason",
        "visualPrecheckChosenReason(chosenReason, visualFallbackReason)",
        "fallbackReason",
        "visualFallbackReason",
    ]
    scheduler_source_kind_priority_tokens = [
        "reportedCurrentFrameSourceKind",
        "visualCueAtTime",
        "currentFrameUsesWorkbenchVision",
        "workbench-vision",
        "workerSourceKind.isEmpty()",
        "visualCueSourceKind",
        "visualTrackAuthoritativeAtTime",
        "visual-no-text",
        "quick-baseline-allowed-outside-visual-coverage",
    ]
    acceptance_summary_schema_tokens = [
        "_stable_count_summary",
        "_summary_schema_checks",
        "mediaCount",
        "passCount",
        "failCount",
        "holdoutCount",
        "summarySchemaChecks",
        "summarySchemaPass",
        "summarySchemaStableCounts",
    ]
    acceptance_summary_schema_order_tokens = [
        "_stable_count_summary",
        "mediaCount",
        "passCount",
        "failCount",
        "holdoutCount",
        "_summary_schema_checks",
        "summarySchemaChecks",
        "summarySchemaPass",
        "summarySchemaStableCounts",
    ]
    python_normalization_tokens = [
        "_TRAD_TO_SIMP",
        "def normalize_zh_hans",
        "\\u767c\\u5e03",
        "\\u53d1\\u5e03",
        "\\u9084",
        "\\u8fd8",
        "normalize_text(value).translate(_TRAD_TO_SIMP)",
    ]
    python_normalization_consumer_tokens = [
        "normalize_zh_hans(final_text)",
        "normalize_zh_hans(raw)",
        "normalize_zh_hans(chosen.text if chosen else \"\")",
        "normalize_zh_hans(str(cue.get(\"finalDisplayedText\", \"\")))",
    ]
    visual_translate_normalization_tokens = [
        "from headless_translation_gate import read_vtt, _seconds_to_ts, contains_cjk, normalize_zh_hans",
        "return normalize_zh_hans(ruled)",
        "value = normalize_zh_hans(obj.get(\"translatedText\", \"\"))",
        "return [normalize_zh_hans(str(v)) for v in values]",
    ]

    checks = {
        "cppPolicyExists": cpp_path.exists(),
        "headlessGateExists": headless_path.exists(),
        "schedulerExists": scheduler_path.exists(),
        "acceptanceReportExists": acceptance_report_path.exists(),
        "visualTranslateExists": visual_translate_path.exists(),
        "auditTokenCompactionRejectsEmptyNeedles": not has_all_tokens(headless, ["暗影"]),
        "auditOrderTokenCompactionRejectsEmptyNeedles": not has_tokens_in_order(headless, ["暗影"]),
        "cppPolicyFunctionsPresent": all(fn in cpp for fn in cpp_policy_functions),
        "pythonPolicyFunctionsPresent": all(fn in headless for fn in python_policy_functions),
        "cppRequiresHan": "containsHan(value)" in cpp and "!value.isEmpty()" in cpp,
        "pythonRequiresCjk": "not contains_cjk(final_full)" in headless,
        "cppRejectsKanaReplacement": "!containsKanaOrReplacement(value)" in cpp,
        "pythonRejectsKanaReplacement": "contains_kana_or_replacement(final_full)" in headless,
        "cppRejectsLatinSentence": "!containsLatinSentence(value)" in cpp,
        "pythonRejectsLatinSentence": "contains_latin_sentence(final_full)" in headless,
        "cppRejectsPlaceholder": "!isPlaceholderTranslationText(value)" in cpp,
        "pythonRejectsPlaceholder": "is_placeholder_translation(final_full)" in headless,
        "cppRejectsWatermark": "!isLikelyVisualWatermarkText(value)" in cpp,
        "pythonRejectsWatermark": "is_watermark_translation(final_full)" in headless,
        "placeholderTokenParity": has_all_tokens(cpp, common_placeholder_tokens) and has_all_tokens(headless, common_placeholder_tokens),
        "watermarkTokenParity": has_all_tokens(cpp, common_watermark_tokens) and has_all_tokens(headless, common_watermark_tokens),
        "pythonFinalSelectionHelperExtracted": "def choose_final_cue(" in headless and "def audit_final_cue(" in headless,
        "pythonDecisionReasonsStable": has_all_tokens(headless, python_reason_tokens),
        "schedulerStillDelegatesToPolicy": "translation_text::isFinalDisplayableText" in scheduler,
        "schedulerReportingHelpersPresent": all(f"{name}(" in scheduler for name in scheduler_reporting_helpers),
        "schedulerReportingHelperDefinitionsStable": all(has_regex(scheduler, pattern) for pattern in scheduler_helper_definition_patterns),
        "schedulerVisualReasonLiteralsStable": has_all_tokens(scheduler, scheduler_visual_reason_tokens),
        "schedulerSourceLiteralsStable": has_all_tokens(scheduler, scheduler_source_tokens),
        "schedulerHighQualitySourcePriorityStable": has_tokens_in_order(scheduler, scheduler_hq_source_priority_tokens),
        "schedulerVisualPrecheckSchemaStable": has_all_tokens(scheduler, scheduler_visual_precheck_schema_tokens),
        "schedulerVisualPrecheckUsesChosenReasonHelper": has_tokens_in_order(scheduler, scheduler_visual_precheck_helper_tokens),
        "schedulerSourceKindPriorityStable": has_tokens_in_order(scheduler, scheduler_source_kind_priority_tokens),
        "schedulerFinalDisplaySchemaStable": has_all_tokens(scheduler, scheduler_final_display_schema_tokens),
        "acceptanceSummarySchemaCountsStable": has_all_tokens(acceptance_report, acceptance_summary_schema_tokens),
        "acceptanceSummarySchemaOrderStable": has_tokens_in_order(acceptance_report, acceptance_summary_schema_order_tokens),
        "pythonCjkNormalizationTableStable": has_all_literals(headless, python_normalization_tokens),
        "pythonFinalSelectionUsesCjkNormalization": has_all_literals(headless, python_normalization_consumer_tokens),
        "visualSourceTranslatorUsesSharedNormalization": has_all_literals(visual_translate, visual_translate_normalization_tokens),
        "quickChunk4": has_regex(subtitle_service, r"constexpr\s+int\s+kChunkSeconds\s*=\s*4\s*;"),
        "defaultAsrConcurrency8": has_regex(subtitle_service, r"constexpr\s+int\s+kDefaultAsrConcurrency\s*=\s*8\s*;"),
        "defaultTranslationConcurrency8": has_regex(subtitle_service, r"constexpr\s+int\s+kDefaultTranslationConcurrency\s*=\s*8\s*;"),
        "firstWindow24": has_regex(application, r"kInitialSubtitleWindowSeconds\s*=\s*24\.0\s*;"),
        "backgroundWindow180": has_regex(application, r"kBackgroundSubtitleWindowSeconds\s*=\s*180\.0\s*;"),
        "prefetch210": has_regex(application, r"kSubtitlePrefetchLeadSeconds\s*=\s*210\.0\s*;"),
        "overlap18": has_regex(application, r"kSubtitleContinuationOverlapSeconds\s*=\s*18\.0\s*;"),
        "initialPreroll6": has_regex(application, r"kInitialSubtitlePrerollSeconds\s*=\s*6\.0\s*;"),
    }
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "task": "runtime_headless_final_display_policy_parity",
        "files": {
            "cppPolicy": str(cpp_path),
            "headlessGate": str(headless_path),
            "scheduler": str(scheduler_path),
            "schedulerSupport": str(scheduler_support_path),
            "schedulerSupportHeader": str(scheduler_support_header_path),
            "subtitleService": str(subtitle_service_path),
            "application": str(application_path),
            "acceptanceReport": str(acceptance_report_path),
            "visualSourceTranslator": str(visual_translate_path),
        },
        "checks": checks,
        "allPassed": all(bool(v) for v in checks.values()),
        "protected": "final display text policy parity, scheduler helper literals/schema, headless final cue helper extraction, CJK normalization consumers, acceptance summary schema counts, frozen quick constants",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(r"C:\Users\1\Desktop\RVLite"))
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()
    result = audit(args.repo)
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if result["allPassed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
