#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace cgplay {

struct TranslationSkillDescriptor
{
    QString id;
    QString displayName;
    QString apiUsage;
    QString workerThreadUsage;
    QString sidecarCacheOwnership;
    QString uiImpact;
    QString autoRun;
    bool canAffectQuickPath = false;
    QString defaultState;
    QString schedulerLane;
    QString resourceClass;
    QStringList dependencyIds;
    QStringList resourceTags;
    QStringList safetyBoundaries;

    QJsonObject toJson() const;
};

class TranslationSkillRegistry final
{
public:
    TranslationSkillRegistry();

    const QVector<TranslationSkillDescriptor>& skills() const;
    const TranslationSkillDescriptor* find(const QString& id) const;
    QStringList skillIds() const;
    QJsonObject toJson() const;

private:
    QVector<TranslationSkillDescriptor> _skills;
};

} // namespace cgplay
