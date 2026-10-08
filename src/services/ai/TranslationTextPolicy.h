#pragma once

#include <QString>

namespace cgplay {
namespace translation_text {

bool containsHan(const QString& text);
bool containsKanaOrReplacement(const QString& text);
bool containsLatinSentence(const QString& text);
bool isPlaceholderTranslationText(const QString& text);
bool isLikelyVisualWatermarkText(const QString& text);
bool isFinalDisplayableText(const QString& text);

} // namespace translation_text
} // namespace cgplay
