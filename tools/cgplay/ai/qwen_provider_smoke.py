#!/usr/bin/env python3
"""Static smoke checks for RVLite Qwen provider wiring.

This intentionally avoids real network/API calls so missing Qwen keys cannot
break local playback or CI-style verification.
"""

from __future__ import annotations

import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[3]


def read_text(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8", errors="replace")


def check(name: str, condition: bool, message: str) -> dict:
    return {"name": name, "passed": bool(condition), "message": message}


def main() -> int:
    subtitle_service = read_text("src/services/ai/SubtitleGenerationService.cpp")
    application = read_text("src/ui/app/Application.cpp")
    asr_header = read_text("src/services/ai/SubtitleAsrApiClient.h")
    asr_client = read_text("src/services/ai/SubtitleAsrApiClient.cpp")
    workspace = read_text("src/ui/app/AIAgentWorkspace.cpp")
    detector = read_text("src/services/ai/discovery/AIProviderDetector.cpp")
    provider = read_text("src/services/ai/OpenAIResponsesProvider.cpp")
    long_worker = read_text("tools/cgplay/ai/long_window_asr_worker.py")

    checks = [
        check(
            "Qwen translation provider selection is persisted",
            'ai/subtitles/translation/provider' in subtitle_service
            and 'providerKind.compare(QStringLiteral("qwen")' in subtitle_service
            and 'Qwen / DashScope' in workspace,
            "subtitle translation provider=qwen has UI and runtime branches",
        ),
        check(
            "Qwen default endpoint and model are wired",
            "https://dashscope.aliyuncs.com/compatible-mode/v1" in subtitle_service
            and "qwen-plus" in subtitle_service
            and "qwen-plus" in workspace,
            "default compatible endpoint/model are present",
        ),
        check(
            "Qwen API key is independent and masked by design",
            "qwen/apiKey" in subtitle_service
            and "qwen/apiKey" in detector
            and "qwen/apiKey" in provider
            and "apiKey.toUtf8()" in subtitle_service
            and "qInfo()" in subtitle_service,
            "Qwen key uses credential store/env and logs provider/model/endpoint only",
        ),
        check(
            "No-key error is explicit",
            "Subtitle translation API key is missing for provider=qwen" in subtitle_service,
            "missing Qwen key returns a clear translation error",
        ),
        check(
            "Qwen ASR entry calls real DashScope compatible route",
            "QwenDashScopeAsr" in asr_header
            and "qwen-asr-provider-not-wired" not in asr_client
            and "normalizeQwenChatEndpoint" in asr_client
            and "provider=qwen" in asr_client
            and "qwen3-asr-flash" in workspace,
            "Qwen3-ASR is selectable and uses the official OpenAI-compatible ASR request path",
        ),
        check(
            "Qwen ASR default model replaces old MiMo/legacy value",
            "kQwenDefaultAsrModel = \"qwen3-asr-flash\"" in workspace
            and "modelLooksUnsuitableForQwenAsr" in workspace
            and "qwen3-asr-flash" in subtitle_service
            and "qwen3-asr" in workspace,
            "selecting provider=qwen defaults stale qwen3-asr/mimo values to qwen3-asr-flash",
        ),
        check(
            "Qwen ASR no-key error is provider-specific",
            "ASR API key is missing provider=qwen" in asr_client,
            "missing Qwen/DashScope key is reported without falling through to a stub",
        ),
        check(
            "Qwen ASR transient network failures are retried with a useful endpoint hint",
            "isRetryableAttemptError" in asr_client
            and "QThread::msleep" in asr_client
            and "Model Studio Workspace endpoint" in asr_client
            and "connection closed" in asr_client.lower(),
            "HTTP 0 / connection-closed Qwen ASR failures get short retry and actionable endpoint diagnostics",
        ),
        check(
            "Qwen ASR UI smoke no longer treats transient failures as pass",
            "transientNetworkError" in workspace
            and "apiReachableButRejected" in workspace
            and "!report.value(QStringLiteral(\"transientNetworkError\")).toBool()" in read_text("src/ui/app/MainWindowAutomation.cpp"),
            "provider smoke distinguishes wired/auth-rejected from connection-closed failure",
        ),
        check(
            "Existing MiMo/OpenAI fallbacks remain",
            "mimo/apiKey" in subtitle_service
            and "XIAOMI_MIMO_API_KEY" in subtitle_service
            and "openai/apiKey" in subtitle_service
            and "OPENAI_API_KEY" in provider,
            "MiMo and OpenAI credential paths remain present",
        ),
        check(
            "Manual HQ worker can receive Qwen env",
            "QWEN_API_KEY" in long_worker
            and "DASHSCOPE_API_KEY" in long_worker
            and "qwen-plus" in long_worker,
            "long-window ASR translation fallback recognizes Qwen env",
        ),
        check(
            "Quick subtitle constants remain frozen",
            "constexpr int kChunkSeconds = 4" in subtitle_service
            and "constexpr int kDefaultAsrConcurrency = 8" in subtitle_service
            and "constexpr int kDefaultTranslationConcurrency = 8" in subtitle_service
            and "kInitialSubtitleWindowSeconds = 24.0" in application
            and "kBackgroundSubtitleWindowSeconds = 180.0" in application
            and "kSubtitlePrefetchLeadSeconds = 210.0" in application
            and "kSubtitleContinuationOverlapSeconds = 18.0" in application
            and "kInitialSubtitlePrerollSeconds = 6.0" in application,
            "Qwen ASR wiring did not change quick chunk/concurrency/window constants",
        ),
    ]

    failed = [item for item in checks if not item["passed"]]
    report = {
        "success": not failed,
        "passed": len(checks) - len(failed),
        "failed": len(failed),
        "checks": checks,
    }
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
