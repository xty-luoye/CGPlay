#pragma once

#include "SubtitleGenerationService.h"

#include <QJsonObject>

namespace cgplay {

class TranslationBaselineController final
{
public:
    QJsonObject describeQuickBaseline(const SubtitleGenerationResult& result) const;
    QJsonObject describeRefinementBoundary(const SubtitleRefinementResult& result) const;
};

} // namespace cgplay
