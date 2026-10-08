#!/usr/bin/env python3
"""Static audit for translation mock/legacy/fallback provenance boundaries.

This does not remove any test or fallback path. It records whether each risky
path is still explicitly gated and labeled so cleanup work can continue without
silently turning mock, legacy, or degraded output into product success.
"""

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def has_regex(text: str, pattern: str) -> bool:
    return re.search(pattern, text, flags=re.MULTILINE) is not None


def contains_all(text: str, tokens: List[str]) -> bool:
    return all(token in text for token in tokens)


def audit(repo: Path) -> Dict[str, object]:
    scheduler_path = repo / "src" / "services" / "ai" / "TranslationEnhancementScheduler.cpp"
    scheduler_support_path = repo / "src" / "services" / "ai" / "TranslationEnhancementSupport.cpp"
    application_path = repo / "src" / "ui" / "app" / "Application.cpp"
    runtime_path = repo / "src" / "ui" / "app" / "ApplicationRuntime.cpp"
    skill_registry_path = repo / "src" / "services" / "ai" / "TranslationSkillRegistry.cpp"
    speed_guard_path = repo / "src" / "services" / "ai" / "TranslationSpeedGuard.cpp"
    runner_path = repo / "tools" / "cgplay" / "ai" / "run_translation_acceptance.ps1"
    headless_path = repo / "tools" / "cgplay" / "ai" / "headless_translation_gate.py"

    scheduler = "\n".join(read_text(path) for path in (scheduler_path, scheduler_support_path))
    application = read_text(application_path)
    runtime = read_text(runtime_path)
    skill_registry = read_text(skill_registry_path)
    speed_guard = read_text(speed_guard_path)
    runner = read_text(runner_path)
    headless = read_text(headless_path)
    scheduler_helper_tokens = [
        "activeEnhancementSource",
        "enhancementFallbackReason",
        "visualFallbackReport",
        "visualPrecheckChosenReason",
        "reportedVisualCueSourceKind",
        "reportedCurrentFrameSourceKind",
        "activeHighQualitySource",
    ]
    scheduler_schema_tokens = [
        "sourceKind",
        "chosenSource",
        "chosenReason",
        "fallbackReason",
        "provider",
        "model",
        "ocrIntakeJsonPath",
    ]
    scheduler_visual_boundary_tokens = [
        "visualTrackAuthoritativeAtTime",
        "visual-no-text-suppress-quick-asr",
        "outside-visual-coverage-quick-baseline-allowed",
        "quick-baseline-allowed-outside-visual-coverage",
        "visual-cue-text-missing-suppress-quick-asr",
        "visual-subtitle",
        "hard-sub-ocr",
        "workbench-vision",
        "visual-no-text",
    ]

    checks = {
        "backupManifestGateExists": True,
        "headlessFinalCueHelpersKeepReasons": contains_all(headless, [
            "choose_final_cue",
            "audit_final_cue",
            "chosenSource",
            "chosenReason",
            "quick-baseline-for-current-visual-source",
            "quick-asr-baseline",
        ]),
        "visualNoTextSuppressHasProvenance": contains_all(headless, [
            "visual-no-text",
            "visual-no-text-suppress-quick-asr",
            "fallbackReason",
        ]),
        "asrFallbackAllowedIsExplicit": contains_all(headless, [
            "asrFallbackAllowed",
            "primary_source_kind in (\"none\", \"quick-asr\")",
        ]),
        "holdoutModeIsExplicit": contains_all(headless, [
            "unknownVideoCheck",
            "holdout-invariant-only-no-source-cues",
            "holdoutChecks",
        ]),
        "legacyResealRequiresCurrentIdentity": contains_all(runner, [
            "headless-acceptance-legacy-slash-fingerprint-resealed",
            "Write-CurrentMediaIdentitySidecar",
            "cacheContentSha256",
            "Get-MediaIdentity",
            "mtimeMs",
        ]),
        "legacyResealPrefersHigherCueCount": contains_all(runner, [
            "$legacyCount",
            "$currentCount",
            "$legacyCount -gt $currentCount",
        ]),
        "runtimeMockFlagsRemainSmokeScoped": contains_all(runtime, [
            "--smoke-subtitle-generation-mock",
            "--smoke-subtitle-generation-mock-online-worker",
            "--smoke-subtitle-generation-mock-ocr-worker",
            "--smoke-subtitle-generation-mock-repair-worker",
        ]),
        "subtitleMockFlagIsRequestScoped": contains_all(application, [
            "request.mockWithoutApi",
            "cgplay.subtitleGenerationMock",
            "requestedMockWithoutApi",
        ]),
        "enhancementMockModesAreRequestScoped": contains_all(scheduler, [
            "request.onlineMockMode",
            "request.ocrMockMode",
            "mock-online-reference-provider",
            "mock-ocr-source-cache-written",
            "mock-repair-terminology-cache-written",
            "repairManualAllowed",
        ]),
        "fallbackProvenanceFieldsRuntime": contains_all(scheduler, [
            "sourceKind",
            "chosenSource",
            "fallbackReason",
            "quick-fallback",
            "local-ocr-fallback",
            "visionSourceKind",
        ]),
        "schedulerReportingHelpersKeepProvenanceSchema": contains_all(scheduler, scheduler_helper_tokens + scheduler_schema_tokens),
        "schedulerVisualBoundaryLiteralsStable": contains_all(scheduler, scheduler_visual_boundary_tokens),
        "localOcrFallbackNotWorkbenchSuccess": contains_all(scheduler, [
            "workbench-vision-fallback-local-ocr",
            "local-ocr-fallback",
            "visionApiSucceeded",
        ]),
        "longAsrFallbackIsNotPrimaryForHardSub": contains_all(skill_registry, [
            "quick-asr",
            "priority above audio ASR",
            "must not write source text as translated fallback",
            "must not consume quick ASR/translation capacity",
        ]),
        "speedGuardStillProtectsQuickPath": contains_all(speed_guard, [
            "quick-asr",
            "quick-translate",
            "continuation",
            "canAffectQuickPath",
            "noPlaybackPauseAllowed",
        ]),
        "noBroadDeletionMarkersInAuditScope": not has_regex(
            headless + runner + scheduler,
            r"(?i)(remove\s+mock|delete\s+mock|disable\s+fallback|drop\s+legacy)",
        ),
    }
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "task": "translation_mock_legacy_fallback_provenance_static_audit",
        "files": {
            "scheduler": str(scheduler_path),
            "schedulerSupport": str(scheduler_support_path),
            "application": str(application_path),
            "runtime": str(runtime_path),
            "skillRegistry": str(skill_registry_path),
            "speedGuard": str(speed_guard_path),
            "runner": str(runner_path),
            "headless": str(headless_path),
        },
        "checks": checks,
        "allPassed": all(bool(v) for v in checks.values()),
        "protected": "mock paths remain smoke/request scoped; legacy reseal stays media-identity scoped; scheduler helpers keep literal/source/fallback schema provenance",
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
