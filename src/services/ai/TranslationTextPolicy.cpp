#include "TranslationTextPolicy.h"

#include <QChar>

namespace cgplay {
namespace translation_text {

bool containsHan(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

bool containsKanaOrReplacement(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 0x3040 && u <= 0x30FF) || u == 0xFFFD) {
            return true;
        }
    }
    return false;
}

bool containsLatinSentence(const QString& text)
{
    int latinWords = 0;
    bool inWord = false;
    int wordLen = 0;
    for (const QChar ch : text) {
        if ((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
            (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))) {
            inWord = true;
            ++wordLen;
            continue;
        }
        if (inWord && wordLen >= 2) {
            ++latinWords;
        }
        inWord = false;
        wordLen = 0;
    }
    if (inWord && wordLen >= 2) {
        ++latinWords;
    }
    return latinWords >= 2;
}

bool isPlaceholderTranslationText(const QString& text)
{
    const QString compact = text.simplified().remove(QLatin1Char(' ')).toLower();
    return compact.contains(QStringLiteral("中文翻译待精修")) ||
           compact.contains(QStringLiteral("待精修")) ||
           compact.contains(QStringLiteral("待翻译")) ||
           compact.contains(QStringLiteral("translationpending")) ||
           compact.contains(QStringLiteral("placeholder"));
}

bool isLikelyVisualWatermarkText(const QString& text)
{
    const QString compact = text.simplified().remove(QLatin1Char(' ')).toLower();
    return compact.contains(QStringLiteral("levelinganimation")) ||
           compact.contains(QStringLiteral("animationpartners")) ||
           compact.contains(QStringLiteral("sololeveling")) ||
           compact.contains(QStringLiteral("©")) ||
           compact.contains(QStringLiteral("动画"));
}

bool isFinalDisplayableText(const QString& text)
{
    const QString value = text.trimmed();
    return !value.isEmpty() &&
           containsHan(value) &&
           !containsKanaOrReplacement(value) &&
           !containsLatinSentence(value) &&
           !isPlaceholderTranslationText(value) &&
           !isLikelyVisualWatermarkText(value);
}

} // namespace translation_text
} // namespace cgplay
