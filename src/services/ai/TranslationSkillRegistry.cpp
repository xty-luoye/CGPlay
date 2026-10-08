#include "TranslationSkillRegistry.h"

#include <QJsonArray>

namespace cgplay {

QJsonObject TranslationSkillDescriptor::toJson() const
{
    QJsonArray boundaryArray;
    for (const auto& boundary : safetyBoundaries) {
        boundaryArray.append(boundary);
    }
    QJsonArray dependencyArray;
    for (const auto& dependency : dependencyIds) {
        dependencyArray.append(dependency);
    }
    QJsonArray resourceArray;
    for (const auto& resource : resourceTags) {
        resourceArray.append(resource);
    }
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("displayName"), displayName },
        { QStringLiteral("apiUsage"), apiUsage },
        { QStringLiteral("workerThreadUsage"), workerThreadUsage },
        { QStringLiteral("sidecarCacheOwnership"), sidecarCacheOwnership },
        { QStringLiteral("uiImpact"), uiImpact },
        { QStringLiteral("autoRun"), autoRun },
        { QStringLiteral("canAffectQuickPath"), canAffectQuickPath },
        { QStringLiteral("defaultState"), defaultState },
        { QStringLiteral("schedulerLane"), schedulerLane },
        { QStringLiteral("resourceClass"), resourceClass },
        { QStringLiteral("dependencyIds"), dependencyArray },
        { QStringLiteral("resourceTags"), resourceArray },
        { QStringLiteral("safetyBoundaries"), boundaryArray }
    };
}

TranslationSkillRegistry::TranslationSkillRegistry()
    : _skills{
          {
              QStringLiteral("local-subtitle"),
              QStringLiteral("LocalSubtitleSkill"),
              QStringLiteral("none"),
              QStringLiteral("runs inside the existing subtitle generation worker"),
              QStringLiteral("reads same-name/external/embedded subtitle sources and writes quick .source/.zh sidecars only through SubtitleGenerationService"),
              QStringLiteral("service diagnostics only"),
              QStringLiteral("auto when a usable local/embedded subtitle source exists"),
              true,
              QStringLiteral("enabled"),
              QStringLiteral("quick-source"),
              QStringLiteral("local-file"),
              {},
              { QStringLiteral("filesystem"), QStringLiteral("quick-sidecar") },
              { QStringLiteral("priority above audio ASR"),
                QStringLiteral("must fall back to quick ASR if unusable"),
                QStringLiteral("must not advance displayable coverage without translated text") }
          },
          {
              QStringLiteral("quick-asr"),
              QStringLiteral("QuickAsrSkill"),
              QStringLiteral("subtitle ASR provider only"),
              QStringLiteral("existing 8-way ASR worker batching"),
              QStringLiteral("writes quick .source sidecars and temp audio chunks"),
              QStringLiteral("no direct UI impact; display waits for quick translated cues"),
              QStringLiteral("auto when no usable text subtitle source exists"),
              true,
              QStringLiteral("enabled"),
              QStringLiteral("quick-source"),
              QStringLiteral("quick-api"),
              { QStringLiteral("local-subtitle") },
              { QStringLiteral("subtitle-asr-api"), QStringLiteral("worker-pool"), QStringLiteral("quick-sidecar") },
              { QStringLiteral("4s chunks are frozen"),
                QStringLiteral("8-way ASR concurrency is frozen"),
                QStringLiteral("must never pause playback") }
          },
          {
              QStringLiteral("quick-translate"),
              QStringLiteral("QuickTranslateSkill"),
              QStringLiteral("subtitle translation provider only"),
              QStringLiteral("existing 8-way translation worker batching"),
              QStringLiteral("writes quick .zh.srt/.zh.vtt baseline sidecars"),
              QStringLiteral("quick .zh becomes visible baseline"),
              QStringLiteral("auto for source cues that need Chinese display text"),
              true,
              QStringLiteral("enabled"),
              QStringLiteral("quick-translation"),
              QStringLiteral("quick-api"),
              { QStringLiteral("local-subtitle"), QStringLiteral("quick-asr") },
              { QStringLiteral("subtitle-translation-api"), QStringLiteral("worker-pool"), QStringLiteral("quick-sidecar") },
              { QStringLiteral("quick .zh is source of truth"),
                QStringLiteral("must not write source text as translated fallback"),
                QStringLiteral("must keep translated/displayable coverage separate from source coverage") }
          },
          {
              QStringLiteral("continuation"),
              QStringLiteral("ContinuationSkill"),
              QStringLiteral("same quick ASR/translation providers as quick path"),
              QStringLiteral("existing background generation worker; queued when busy"),
              QStringLiteral("background temp output directory merged back into quick .zh after success/partial recovery"),
              QStringLiteral("status bar only for background state"),
              QStringLiteral("auto while playback translation is enabled"),
              true,
              QStringLiteral("enabled"),
              QStringLiteral("quick-continuation"),
              QStringLiteral("quick-api"),
              { QStringLiteral("quick-asr"), QStringLiteral("quick-translate"), QStringLiteral("cache-guard") },
              { QStringLiteral("subtitle-asr-api"), QStringLiteral("subtitle-translation-api"), QStringLiteral("background-temp-sidecar") },
              { QStringLiteral("180s background window, 210s prefetch, and 18s overlap are frozen"),
                QStringLiteral("must not delete active quick sidecars while UI reads them"),
                QStringLiteral("must never pause playback") }
          },
          {
              QStringLiteral("refine"),
              QStringLiteral("RefineSkill"),
              QStringLiteral("Workbench text API first; subtitle translation API only when no workbench/connection config exists"),
              QStringLiteral("separate refinement worker after quick subtitles exist"),
              QStringLiteral("writes .refined.zh sidecars; never overwrites quick .zh on failure"),
              QStringLiteral("cue-level text enhancement only"),
              QStringLiteral("default-disabled for real API; mock-only auto in tests or explicit user setting"),
              true,
              QStringLiteral("deferred"),
              QStringLiteral("enhancement-background"),
              QStringLiteral("background-api"),
              { QStringLiteral("quick-translate"), QStringLiteral("coverage-audit"), QStringLiteral("cache-guard") },
              { QStringLiteral("workbench-api"), QStringLiteral("refined-sidecar"), QStringLiteral("background-worker") },
              { QStringLiteral("must delay while quick generation/continuation is busy or cache is thin"),
                QStringLiteral("must merge per cue with quick fallback"),
                QStringLiteral("must not whole-track replace quick") }
          },
          {
              QStringLiteral("online-subtitle"),
              QStringLiteral("OnlineSubtitleSkill"),
              QStringLiteral("workbench API search capability, local reference, or legacy explicit subtitle REST/search API only; no scraping"),
              QStringLiteral("manual/background tool only"),
              QStringLiteral("independent .online.search.json/.online.source cache"),
              QStringLiteral("diagnostics only in quick generation"),
              QStringLiteral("manual/background/default-disabled"),
              true,
              QStringLiteral("disabled"),
              QStringLiteral("enhancement-manual"),
              QStringLiteral("external-api"),
              { QStringLiteral("coverage-audit"), QStringLiteral("cache-guard") },
              { QStringLiteral("workbench-api"), QStringLiteral("legacy-online-rest-api"), QStringLiteral("independent-cache"), QStringLiteral("manual-trigger") },
              { QStringLiteral("must not run in quick path"),
                QStringLiteral("must align and validate before cue-level use"),
                QStringLiteral("must fall back to quick") }
          },
          {
              QStringLiteral("online-reference-search"),
              QStringLiteral("OnlineReferenceSearchSkill"),
              QStringLiteral("workbench API is the high-quality online/reference subtitle search provider; call it with minimal metadata and report real API errors"),
              QStringLiteral("manual/background worker; network disabled in quick"),
              QStringLiteral("writes independent .online.search.json and optional .online.source/.online.reference cache only"),
              QStringLiteral("diagnostics/status only; display only through later cue-level fusion"),
              QStringLiteral("manual/background/default-disabled; auto-run=false in quick"),
              true,
              QStringLiteral("disabled"),
              QStringLiteral("enhancement-manual"),
              QStringLiteral("external-network"),
              { QStringLiteral("coverage-audit"), QStringLiteral("cache-guard") },
              { QStringLiteral("workbench-search-capability"), QStringLiteral("legacy-online-search-api"), QStringLiteral("independent-cache"), QStringLiteral("manual-trigger") },
              { QStringLiteral("must send only filename/title/duration/language metadata"),
                QStringLiteral("must not upload video/audio payloads"),
                QStringLiteral("must reject mismatched results before fusion/display"),
                QStringLiteral("quick timeline remains display baseline") }
          },
          {
              QStringLiteral("ocr-subtitle"),
              QStringLiteral("OcrSubtitleSkill"),
              QStringLiteral("workbench vision/image-text capability when available, otherwise local/background OCR compatibility path"),
              QStringLiteral("manual/background tool only"),
              QStringLiteral("independent OCR source cache"),
              QStringLiteral("diagnostics only in quick generation"),
              QStringLiteral("manual/background/default-disabled"),
              true,
              QStringLiteral("disabled"),
              QStringLiteral("enhancement-manual"),
              QStringLiteral("local-tool"),
              { QStringLiteral("coverage-audit"), QStringLiteral("cache-guard") },
              { QStringLiteral("ocr-tool"), QStringLiteral("independent-cache"), QStringLiteral("manual-trigger") },
              { QStringLiteral("must crop/sparsely sample; no full-frame quick OCR"),
                QStringLiteral("must not run in quick path"),
                QStringLiteral("must fall back to quick") }
          },
          {
              QStringLiteral("fusion"),
              QStringLiteral("FusionSkill"),
              QStringLiteral("none unless a future manual provider is explicitly configured"),
              QStringLiteral("manual/background file-level fusion only"),
              QStringLiteral("writes .enhanced.zh cache; quick .zh remains baseline"),
              QStringLiteral("cue-level enhancement only"),
              QStringLiteral("manual/background/default-disabled; existing cache may be read as cue-level enhancement"),
              true,
              QStringLiteral("disabled"),
              QStringLiteral("enhancement-manual"),
              QStringLiteral("local-cache"),
              { QStringLiteral("quick-translate"), QStringLiteral("coverage-audit"), QStringLiteral("cache-guard") },
              { QStringLiteral("enhanced-sidecar"), QStringLiteral("cue-match"), QStringLiteral("manual-trigger") },
              { QStringLiteral("must preserve quick cue timing/count"),
                QStringLiteral("must not whole-track replace quick"),
                QStringLiteral("must fall back per cue to quick") }
          },
          {
              QStringLiteral("low-confidence-repair"),
              QStringLiteral("LowConfidenceRepairSkill"),
              QStringLiteral("none in quick path; future background text API only if explicitly enabled"),
              QStringLiteral("manual/background only"),
              QStringLiteral("writes independent .repair.zh cache"),
              QStringLiteral("cue-level enhancement only"),
              QStringLiteral("manual/background/default-disabled"),
              true,
              QStringLiteral("disabled"),
              QStringLiteral("enhancement-manual"),
              QStringLiteral("background-api"),
              { QStringLiteral("quick-translate"), QStringLiteral("coverage-audit"), QStringLiteral("cache-guard") },
              { QStringLiteral("repair-sidecar"), QStringLiteral("low-confidence-cues"), QStringLiteral("manual-trigger") },
              { QStringLiteral("must not retry quick ASR"),
                QStringLiteral("must not consume quick ASR/translation capacity"),
                QStringLiteral("must fall back per cue to quick") }
          },
          {
              QStringLiteral("terminology"),
              QStringLiteral("TerminologySkill"),
              QStringLiteral("none"),
              QStringLiteral("local-only normalization"),
              QStringLiteral("reads resources/subtitles terminology data when present"),
              QStringLiteral("none"),
              QStringLiteral("local-only in existing write/refine boundaries"),
              false,
              QStringLiteral("enabled"),
              QStringLiteral("local-normalization"),
              QStringLiteral("local-data"),
              {},
              { QStringLiteral("terminology-json"), QStringLiteral("local-only") },
              { QStringLiteral("must not add API calls"),
                QStringLiteral("must not delay quick display") }
          },
          {
              QStringLiteral("coverage-audit"),
              QStringLiteral("CoverageAuditSkill"),
              QStringLiteral("none"),
              QStringLiteral("local-only audit"),
              QStringLiteral("no sidecar ownership"),
              QStringLiteral("diagnostics only"),
              QStringLiteral("auto"),
              false,
              QStringLiteral("enabled"),
              QStringLiteral("audit"),
              QStringLiteral("local-diagnostics"),
              {},
              { QStringLiteral("json-report"), QStringLiteral("coverage-policy") },
              { QStringLiteral("source coverage and translated/displayable coverage must stay separate"),
                QStringLiteral("source-only cues are not playable coverage") }
          },
          {
              QStringLiteral("no-dialogue-guard"),
              QStringLiteral("NoDialogueGuardSkill"),
              QStringLiteral("none"),
              QStringLiteral("UI thread display guard only"),
              QStringLiteral("no sidecar ownership"),
              QStringLiteral("hides overlay during no-dialogue/no-exact-cue scenes"),
              QStringLiteral("auto"),
              true,
              QStringLiteral("enabled"),
              QStringLiteral("ui-guard"),
              QStringLiteral("ui-state"),
              { QStringLiteral("coverage-audit") },
              { QStringLiteral("overlay-label"), QStringLiteral("status-bar") },
              { QStringLiteral("must not show progress text over no-dialogue scenes"),
                QStringLiteral("must not hide exact active translated cues") }
          },
          {
              QStringLiteral("cache-guard"),
              QStringLiteral("CacheGuardSkill"),
              QStringLiteral("none"),
              QStringLiteral("local-only cache validation"),
              QStringLiteral("protects quick .zh baseline and rejects stale/holey enhancement caches"),
              QStringLiteral("diagnostics only"),
              QStringLiteral("auto"),
              true,
              QStringLiteral("enabled"),
              QStringLiteral("cache-guard"),
              QStringLiteral("local-cache"),
              { QStringLiteral("coverage-audit") },
              { QStringLiteral("quick-sidecar"), QStringLiteral("refined-sidecar"), QStringLiteral("enhanced-sidecar") },
              { QStringLiteral("quick .zh loads first"),
                QStringLiteral("old/short/holey enhancement caches cannot replace quick") }
          },
          {
              QStringLiteral("report"),
              QStringLiteral("ReportSkill"),
              QStringLiteral("none"),
              QStringLiteral("local-only JSON diagnostics"),
              QStringLiteral("no sidecar ownership"),
              QStringLiteral("smoke JSON/status diagnostics"),
              QStringLiteral("auto"),
              false,
              QStringLiteral("enabled"),
              QStringLiteral("reporting"),
              QStringLiteral("local-diagnostics"),
              { QStringLiteral("coverage-audit"), QStringLiteral("cache-guard"), QStringLiteral("no-dialogue-guard") },
              { QStringLiteral("smoke-json"), QStringLiteral("screenshot-evidence") },
              { QStringLiteral("must avoid full cue text in noisy diagnostics"),
                QStringLiteral("must preserve JSON+screenshot smoke evidence") }
          }
      }
{
}

const QVector<TranslationSkillDescriptor>& TranslationSkillRegistry::skills() const
{
    return _skills;
}

const TranslationSkillDescriptor* TranslationSkillRegistry::find(const QString& id) const
{
    for (const auto& skill : _skills) {
        if (skill.id == id) {
            return &skill;
        }
    }
    return nullptr;
}

QStringList TranslationSkillRegistry::skillIds() const
{
    QStringList ids;
    for (const auto& skill : _skills) {
        ids.append(skill.id);
    }
    return ids;
}

QJsonObject TranslationSkillRegistry::toJson() const
{
    QJsonArray skillArray;
    for (const auto& skill : _skills) {
        skillArray.append(skill.toJson());
    }
    return QJsonObject{
        { QStringLiteral("skillCount"), _skills.size() },
        { QStringLiteral("skills"), skillArray }
    };
}

} // namespace cgplay
