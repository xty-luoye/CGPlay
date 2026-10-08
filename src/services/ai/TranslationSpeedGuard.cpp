#include "TranslationSpeedGuard.h"

#include <QJsonArray>

namespace cgplay {

QJsonObject TranslationSpeedGuard::frozenQuickPathConstants() const
{
    return QJsonObject{
        { QStringLiteral("asrChunkSeconds"), 4 },
        { QStringLiteral("asrConcurrency"), 8 },
        { QStringLiteral("translationConcurrency"), 8 },
        { QStringLiteral("firstWindowSeconds"), 24 },
        { QStringLiteral("backgroundWindowSeconds"), 180 },
        { QStringLiteral("prefetchLeadSeconds"), 210 },
        { QStringLiteral("continuationOverlapSeconds"), 18 },
        { QStringLiteral("initialPrerollSeconds"), 6 }
    };
}

QJsonObject TranslationSpeedGuard::evaluate(const TranslationSkillRegistry& registry) const
{
    QJsonArray riskyAutoRunSkills;
    QJsonArray guardedSkills;
    QJsonArray enabledSkills;
    QJsonArray disabledSkills;
    QJsonArray deferredSkills;
    QJsonArray decisions;
    for (const auto& skill : registry.skills()) {
        const bool autoRuns =
            skill.autoRun.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0 ||
            skill.autoRun.startsWith(QStringLiteral("auto "), Qt::CaseInsensitive) ||
            skill.autoRun.startsWith(QStringLiteral("auto when"), Qt::CaseInsensitive) ||
            skill.autoRun.startsWith(QStringLiteral("auto while"), Qt::CaseInsensitive);
        if (skill.canAffectQuickPath && autoRuns && !_quickPathAutoRunAllowed(skill.id)) {
            riskyAutoRunSkills.append(skill.id);
        }
        if (skill.canAffectQuickPath && !autoRuns) {
            guardedSkills.append(skill.id);
        }
        if (skill.defaultState == QStringLiteral("enabled")) {
            enabledSkills.append(skill.id);
        } else if (skill.defaultState == QStringLiteral("deferred")) {
            deferredSkills.append(skill.id);
        } else {
            disabledSkills.append(skill.id);
        }
        decisions.append(decisionForSkill(skill));
    }
    return QJsonObject{
        { QStringLiteral("ok"), riskyAutoRunSkills.isEmpty() },
        { QStringLiteral("frozenQuickPathConstants"), frozenQuickPathConstants() },
        { QStringLiteral("riskyAutoRunSkills"), riskyAutoRunSkills },
        { QStringLiteral("guardedAccuracySkills"), guardedSkills },
        { QStringLiteral("enabledSkills"), enabledSkills },
        { QStringLiteral("disabledSkills"), disabledSkills },
        { QStringLiteral("deferredSkills"), deferredSkills },
        { QStringLiteral("skillDecisions"), decisions },
        { QStringLiteral("quickZhIsDisplayBaseline"), true },
        { QStringLiteral("noPlaybackPauseAllowed"), true },
        { QStringLiteral("cueLevelEnhancementOnly"), true },
        { QStringLiteral("sourceCoverageSeparateFromDisplayableCoverage"), true }
    };
}

QJsonObject TranslationSpeedGuard::decisionForSkill(const TranslationSkillDescriptor& skill) const
{
    QString decision = skill.defaultState;
    QString reason = QStringLiteral("declared default state");
    if (skill.canAffectQuickPath && !_quickPathAutoRunAllowed(skill.id)) {
        if (skill.defaultState == QStringLiteral("enabled")) {
            decision = QStringLiteral("deferred");
        }
        reason = QStringLiteral("can affect quick path; must stay manual/background/default-disabled unless isolated");
    } else if (_quickPathAutoRunAllowed(skill.id)) {
        decision = QStringLiteral("enabled");
        reason = QStringLiteral("existing quick-path behavior boundary");
    }
    return QJsonObject{
        { QStringLiteral("skillId"), skill.id },
        { QStringLiteral("decision"), decision },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("canAffectQuickPath"), skill.canAffectQuickPath },
        { QStringLiteral("schedulerLane"), skill.schedulerLane },
        { QStringLiteral("resourceClass"), skill.resourceClass }
    };
}

bool TranslationSpeedGuard::_quickPathAutoRunAllowed(const QString& skillId) const
{
    return skillId == QStringLiteral("local-subtitle") ||
        skillId == QStringLiteral("quick-asr") ||
        skillId == QStringLiteral("quick-translate") ||
        skillId == QStringLiteral("continuation") ||
        skillId == QStringLiteral("no-dialogue-guard") ||
        skillId == QStringLiteral("cache-guard");
}

} // namespace cgplay
