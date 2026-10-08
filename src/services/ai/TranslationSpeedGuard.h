#pragma once

#include "TranslationSkillRegistry.h"

#include <QJsonObject>
#include <QStringList>

namespace cgplay {

class TranslationSpeedGuard final
{
public:
    QJsonObject evaluate(const TranslationSkillRegistry& registry) const;
    QJsonObject decisionForSkill(const TranslationSkillDescriptor& skill) const;
    QJsonObject frozenQuickPathConstants() const;

private:
    bool _quickPathAutoRunAllowed(const QString& skillId) const;
};

} // namespace cgplay
