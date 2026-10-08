#include "ApplicationInternal.h"

namespace cgplay {

QString repairGeneratedSubtitleMojibake(const QString& text);

QString generatedSubtitleOcrProgressPath(QString ocrSourcePath)
{
    if (ocrSourcePath.endsWith(QStringLiteral(".ocr.source.vtt"), Qt::CaseInsensitive)) {
        ocrSourcePath.chop(QStringLiteral(".ocr.source.vtt").size());
        return ocrSourcePath + QStringLiteral(".ocr.intake.json");
    }
    return ocrSourcePath + QStringLiteral(".intake.json");
}

QJsonObject readGeneratedSubtitleProgress(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QString generatedSubtitleVisibleDisplayText(QString text, const QFontMetrics& metrics, int maxWidth)
{
    text = repairGeneratedSubtitleMojibake(text).trimmed();
    if (text.isEmpty()) {
        return QString();
    }

    QStringList lines;
    for (QString line : text.replace(QLatin1Char('\r'), QLatin1Char('\n')).split(QLatin1Char('\n'))) {
        line = line.simplified();
        if (line.isEmpty()) {
            continue;
        }
        if (lines.isEmpty() || lines.constLast() != line) {
            lines.append(line);
        }
    }
    if (lines.isEmpty()) {
        return QString();
    }

    auto splitLongLine = [&](const QString& line) {
        if (metrics.horizontalAdvance(line) <= maxWidth || line.size() < 8) {
            return QStringList{ line };
        }
        const int mid = line.size() / 2;
        int split = -1;
        int bestDistance = line.size();
        const QString breakChars = QString::fromUtf8(u8" ，。！？；、,.!?; ");
        for (int i = 1; i < line.size() - 1; ++i) {
            if (!breakChars.contains(line.at(i))) {
                continue;
            }
            const int distance = std::abs(i - mid);
            if (distance < bestDistance) {
                split = i + 1;
                bestDistance = distance;
            }
        }
        if (split < 0) {
            split = mid;
        }
        return QStringList{ line.left(split).trimmed(), line.mid(split).trimmed() };
    };

    if (lines.size() == 1) {
        lines = splitLongLine(lines.first());
    }
    while (lines.size() > 2) {
        const int midpoint = (lines.size() + 1) / 2;
        QStringList merged;
        merged.append(lines.mid(0, midpoint).join(QLatin1Char(' ')).simplified());
        merged.append(lines.mid(midpoint).join(QLatin1Char(' ')).simplified());
        lines = merged;
    }

    for (QString& line : lines) {
        line = metrics.elidedText(line.simplified(), Qt::ElideRight, std::max(32, maxWidth));
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

QString recoverySessionPromptTitle()
{
    return QStringLiteral("\u6062\u590d\u4f1a\u8bdd");
}

QString recoverySessionPromptText()
{
    return QStringLiteral("\u68c0\u6d4b\u5230\u672a\u4fdd\u5b58\u7684\u4f1a\u8bdd\uff0c\u662f\u5426\u6062\u590d\uff1f");
}

QMessageBox* createYesNoQuestionBox(QWidget* parent, const QString& title, const QString& text)
{
    auto* box = new QMessageBox(QMessageBox::Question, title, text,
                                QMessageBox::Yes | QMessageBox::No, parent);
    box->setDefaultButton(QMessageBox::Yes);
    if (auto* yesButton = box->button(QMessageBox::Yes)) {
        yesButton->setText(QStringLiteral("\u662f"));
    }
    if (auto* noButton = box->button(QMessageBox::No)) {
        noButton->setText(QStringLiteral("\u5426"));
    }
    return box;
}

QMessageBox* createRecoverySessionPromptBox(QWidget* parent)
{
    auto* box = createYesNoQuestionBox(parent, recoverySessionPromptTitle(), recoverySessionPromptText());
    box->setObjectName(QStringLiteral("RecoverySessionPrompt"));
    return box;
}

QString localReferenceSubtitlePathForMedia(const QString& mediaPath)
{
    const QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists()) {
        return {};
    }
    const QDir dir = mediaInfo.dir();
    const QString baseName = mediaInfo.completeBaseName();
    for (const QString& suffix : { QStringLiteral(".srt"), QStringLiteral(".vtt"), QStringLiteral(".ass"), QStringLiteral(".ssa") }) {
        const QString candidate = dir.filePath(baseName + suffix);
        const QString lowerName = QFileInfo(candidate).fileName().toLower();
        if (QFileInfo::exists(candidate) &&
            !lowerName.contains(QStringLiteral(".source.")) &&
            !lowerName.contains(QStringLiteral(".refined.")) &&
            !lowerName.contains(QStringLiteral(".enhanced.")) &&
            !lowerName.contains(QStringLiteral(".repair.")) &&
            !lowerName.contains(QStringLiteral(".online.")) &&
            !lowerName.contains(QStringLiteral(".ocr.")) &&
            !lowerName.contains(QStringLiteral(".longasr.")) &&
            !lowerName.contains(QStringLiteral(".hq.translated."))) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

bool settingOrEnvBool(const std::shared_ptr<ISettingsService>& settings, const QString& key, const QString& envName)
{
    const QString envValue = QProcessEnvironment::systemEnvironment().value(envName).trimmed().toLower();
    if (envValue == QStringLiteral("1") ||
        envValue == QStringLiteral("true") ||
        envValue == QStringLiteral("yes") ||
        envValue == QStringLiteral("on")) {
        return true;
    }
    return settings && settings->value(key, false).toBool();
}

void markAppearanceCustomized(const std::shared_ptr<ISettingsService>& settings, QComboBox* modeCombo)
{
    if (!settings) {
        return;
    }
    if (modeCombo && modeCombo->currentData().toString().compare(QStringLiteral("custom"), Qt::CaseInsensitive) != 0) {
        const QSignalBlocker blocker(modeCombo);
        const int customIndex = modeCombo->findData(QStringLiteral("custom"));
        if (customIndex >= 0) {
            modeCombo->setCurrentIndex(customIndex);
        }
    }
    settings->setValue(QStringLiteral("appearance/mode"), QStringLiteral("custom"));
    settings->sync();
}

QString firstNonEmptySetting(
    const std::shared_ptr<ISettingsService>& settings,
    std::initializer_list<const char*> keys)
{
    if (!settings) {
        return {};
    }
    for (const char* key : keys) {
        const QString value = settings->value(QString::fromLatin1(key)).toString().trimmed();
        if (!value.isEmpty() && value.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            return value;
        }
    }
    return {};
}

QString firstNonEmptyEnv(std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const QString value = QString::fromUtf8(qgetenv(key)).trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

QString firstNonEmptySecret(std::initializer_list<const char*> ids)
{
    auto* store = ServiceLocator::getService<IAICredentialStore>();
    if (!store) {
        return {};
    }
    QString error;
    for (const char* id : ids) {
        const QByteArray secret = store->loadSecret(QString::fromLatin1(id), &error).trimmed();
        if (!secret.isEmpty()) {
            return QString::fromUtf8(secret).trimmed();
        }
    }
    return {};
}

bool hasDedicatedSubtitleRefinementWorkspace(const std::shared_ptr<ISettingsService>& settings)
{
    if (!settings) {
        return false;
    }
    QString providerId = settings->value(QStringLiteral("ai/workspace/providerId"))
        .toString()
        .trimmed();
    if (providerId.isEmpty() || providerId.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        providerId = settings->value(QStringLiteral("ai/connection/providerId")).toString().trimmed();
    }
    if (providerId.isEmpty() || providerId.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        const QString connectionBaseUrl =
            settings->value(QStringLiteral("ai/connection/baseUrl")).toString().trimmed();
        if (!connectionBaseUrl.isEmpty()) {
            providerId = QStringLiteral("openai");
        }
    }

    QString model = settings->value(QStringLiteral("ai/workspace/model"))
        .toString()
        .trimmed();
    if (model.isEmpty() || model.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        model = settings->value(QStringLiteral("ai/connection/model")).toString().trimmed();
    }
    if (model.isEmpty() || model.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        model = settings->value(QStringLiteral("ai/connection/recommendedModel")).toString().trimmed();
    }
    return !providerId.isEmpty() &&
        providerId.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0 &&
        !model.isEmpty() &&
        model.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0;
}

double generatedSubtitleMaxEndSeconds(const QVector<GeneratedSubtitleCue>& cues)
{
    double maxEnd = 0.0;
    for (const auto& cue : cues) {
        maxEnd = std::max(maxEnd, cue.endSeconds);
    }
    return maxEnd;
}

bool generatedSubtitleCueHasDisplayableTranslation(const GeneratedSubtitleCue& cue)
{
    auto compactVisibleText = [](QString text) {
        text = text.trimmed().toLower();
        QString compact;
        compact.reserve(text.size());
        for (const QChar ch : text) {
            if (ch.isSpace() || ch.isPunct()) {
                continue;
            }
            compact.append(ch);
        }
        return compact;
    };
    auto isFillerText = [&compactVisibleText](const QString& text) {
        const QString compact = compactVisibleText(text);
        if (compact.isEmpty()) {
            return true;
        }
        static const QSet<QString> exactFillers = {
            QStringLiteral("啊"),
            QStringLiteral("啊啊"),
            QStringLiteral("嗯"),
            QStringLiteral("嗯嗯"),
            QStringLiteral("呃"),
            QStringLiteral("呃呃"),
            QStringLiteral("哦"),
            QStringLiteral("哦哦"),
            QStringLiteral("喔"),
            QStringLiteral("喔喔"),
            QStringLiteral("哎"),
            QStringLiteral("唉"),
            QStringLiteral("诶"),
            QStringLiteral("欸"),
            QStringLiteral("呀"),
            QStringLiteral("\u597d"),
            QStringLiteral("\u597d\u7684"),
            QStringLiteral("\u597d\u5427"),
            QStringLiteral("\u884c"),
            QStringLiteral("ah"),
            QStringLiteral("uh"),
            QStringLiteral("um"),
            QStringLiteral("oh"),
            QStringLiteral("hm"),
            QStringLiteral("hmm"),
            QStringLiteral("mm"),
            QStringLiteral("eh")
        };
        if (exactFillers.contains(compact)) {
            return true;
        }
        if (compact.size() <= 3) {
            bool allFillerChars = true;
            static const QString fillerChars = QStringLiteral("啊嗯呃哦喔哎唉诶欸呀好");
            for (const QChar ch : compact) {
                if (!fillerChars.contains(ch)) {
                    allFillerChars = false;
                    break;
                }
            }
            if (allFillerChars) {
                return true;
            }
        }
        return false;
    };
    auto isShortAffirmation = [&compactVisibleText](const QString& text) {
        const QString compact = compactVisibleText(text);
        if (compact.isEmpty()) {
            return false;
        }
        static const QSet<QString> affirmations = {
            QStringLiteral("\u597d"),
            QStringLiteral("\u597d\u7684"),
            QStringLiteral("\u597d\u5427"),
            QStringLiteral("\u597d\u597d"),
            QStringLiteral("\u597d\u597d\u597d"),
            QStringLiteral("\u884c"),
            QStringLiteral("\u53ef\u4ee5"),
            QStringLiteral("ok"),
            QStringLiteral("okay"),
            QStringLiteral("yes"),
            QStringLiteral("yeah"),
            QStringLiteral("yep"),
            QStringLiteral("sure"),
            QStringLiteral("fine"),
            QStringLiteral("alright"),
            QStringLiteral("allright")
        };
        return affirmations.contains(compact);
    };
    auto sourceSupportsShortAffirmation = [&compactVisibleText](const QString& text) {
        const QString compact = compactVisibleText(text);
        if (compact.isEmpty()) {
            return false;
        }
        static const QSet<QString> sourceAffirmations = {
            QStringLiteral("\u597d"),
            QStringLiteral("\u597d\u7684"),
            QStringLiteral("\u597d\u5427"),
            QStringLiteral("\u597d\u597d"),
            QStringLiteral("\u597d\u597d\u597d"),
            QStringLiteral("\u884c"),
            QStringLiteral("\u53ef\u4ee5"),
            QStringLiteral("ok"),
            QStringLiteral("okay"),
            QStringLiteral("yes"),
            QStringLiteral("yeah"),
            QStringLiteral("yep"),
            QStringLiteral("sure"),
            QStringLiteral("fine"),
            QStringLiteral("alright"),
            QStringLiteral("allright")
        };
        return sourceAffirmations.contains(compact);
    };

    const QString translated = cue.translatedText.trimmed();
    if (translated.isEmpty()) {
        return false;
    }
    if (!isFillerText(translated)) {
        return true;
    }
    const QString source = cue.sourceText.trimmed();
    if (isShortAffirmation(translated) && !sourceSupportsShortAffirmation(source)) {
        return false;
    }
    return !source.isEmpty() && !isFillerText(source) &&
        compactVisibleText(source) != compactVisibleText(translated);
}

double generatedSubtitleTranslatedMaxEndSeconds(const QVector<GeneratedSubtitleCue>& cues)
{
    double maxEnd = 0.0;
    for (const auto& cue : cues) {
        if (generatedSubtitleCueHasDisplayableTranslation(cue)) {
            maxEnd = std::max(maxEnd, cue.endSeconds);
        }
    }
    return maxEnd;
}

int generatedSubtitleMojibakeScore(const QString& text)
{
    int score = text.count(QChar(0xFFFD)) * 5;
    QChar previous;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0xE000 && u <= 0xF8FF) {
            score += 6;
        } else if ((u >= 0xFF10 && u <= 0xFF19) ||
                   (u >= 0xFF21 && u <= 0xFF3A) ||
                   (u >= 0xFF41 && u <= 0xFF5A)) {
            score += 4;
        } else if (u == 0x20AC) {
            score += 4;
        } else if (u == 0x003F) {
            const ushort p = previous.unicode();
            if ((p >= 0x4E00 && p <= 0x9FFF) || (p >= 0xE000 && p <= 0xF8FF)) {
                score += 2;
            }
        }
        switch (u) {
        case 0x50A6:
        case 0x59DD:
        case 0x6902:
        case 0x69F8:
        case 0x9286:
        case 0x934B:
        case 0x9360:
        case 0x9365:
        case 0x93B4:
        case 0x93C4:
        case 0x93C8:
        case 0x93C9:
        case 0x9470:
        case 0x9473:
        case 0x95AD:
        case 0x95AB:
            score += 3;
            break;
        default:
            break;
        }
        previous = ch;
    }
    static const QStringList markers = {
        QStringLiteral("銇"),
        QStringLiteral("銈"),
        QStringLiteral("偄"),
        QStringLiteral("儯"),
        QStringLiteral("銆"),
        QStringLiteral("€"),
        QStringLiteral("灏"),
        QStringLiteral("辫"),
        QStringLiteral("繖"),
        QStringLiteral("枫"),
        QStringLiteral("鍑"),
        QStringLiteral("鐜"),
        QStringLiteral("涓"),
        QStringLiteral("仮"),
        QStringLiteral("浠"),
        QStringLiteral("璧"),
        QStringLiteral("瀛"),
        QStringLiteral("鐢"),
        QStringLiteral("垚"),
        QStringLiteral("娴"),
        QStringLiteral("嬭"),
        QStringLiteral("瘯"),
        QStringLiteral("浣犳"),
        QStringLiteral("槸璇"),
        QStringLiteral("缃楀"),
        QStringLiteral("鍚楋"),
        QStringLiteral("鐨勮"),
        QStringLiteral("鍦哄"),
        QStringLiteral("鍙"),
        QStringLiteral("绋嬬"),
        QStringLiteral("搴嗙"),
        QStringLiteral("€?")
    };
    for (const QString& marker : markers) {
        score += text.count(marker) * 3;
    }
    return score;
}

QString repairGeneratedSubtitleMojibake(const QString& text)
{
    if (text.trimmed().isEmpty() || generatedSubtitleMojibakeScore(text) < 3) {
        return text;
    }
    const QString localRepaired = QString::fromUtf8(text.toLocal8Bit()).trimmed();
    if (!localRepaired.isEmpty() && generatedSubtitleMojibakeScore(localRepaired) + 2 < generatedSubtitleMojibakeScore(text)) {
        return localRepaired;
    }
    const auto encoding = QStringConverter::encodingForName("GB18030");
    if (!encoding.has_value()) {
        return text;
    }
    QStringEncoder encoder(*encoding);
    const QString repaired = QString::fromUtf8(encoder(text)).trimmed();
    if (repaired.isEmpty()) {
        return text;
    }
    QString cleaned = repaired;
    cleaned.replace(QStringLiteral("\uFFFD?"), QStringLiteral("。"));
    cleaned.replace(QStringLiteral("\uFFFD"), QString());
    if (cleaned.endsWith(QLatin1Char('?')) && generatedSubtitleMojibakeScore(text) >= 3) {
        cleaned.chop(1);
        cleaned += QStringLiteral("。");
    }
    return generatedSubtitleMojibakeScore(cleaned) + 2 < generatedSubtitleMojibakeScore(text) ? cleaned : text;
}

void repairGeneratedSubtitleCueText(GeneratedSubtitleCue* cue)
{
    if (!cue) {
        return;
    }
    cue->sourceText = repairGeneratedSubtitleMojibake(cue->sourceText);
    cue->translatedText = repairGeneratedSubtitleMojibake(cue->translatedText);
}

bool generatedSubtitleContainsHan(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

QString generatedSubtitleNormalizeChineseLiteralForZhHans(QString text)
{
    text = repairGeneratedSubtitleMojibake(text).trimmed();
    const QVector<QPair<QString, QString>> replacements = {
        { QStringLiteral("\u767c\u5e03"), QStringLiteral("\u53d1\u5e03") },
        { QStringLiteral("\u767c"), QStringLiteral("\u53d1") },
        { QStringLiteral("\u56b4"), QStringLiteral("\u4e25") },
        { QStringLiteral("\u95c7\u5f71"), QStringLiteral("\u6697\u5f71") },
        { QStringLiteral("\u95a3\u5f71"), QStringLiteral("\u6697\u5f71") },
        { QStringLiteral("\u95d4\u5f71"), QStringLiteral("\u6697\u5f71") },
        { QStringLiteral("\u5ead\u5712"), QStringLiteral("\u5ead\u56ed") },
        { QStringLiteral("\u6211\u5011"), QStringLiteral("\u6211\u4eec") },
        { QStringLiteral("\u4ed6\u5011"), QStringLiteral("\u4ed6\u4eec") },
        { QStringLiteral("\u4f60\u5011"), QStringLiteral("\u4f60\u4eec") },
        { QStringLiteral("\u5011"), QStringLiteral("\u4eec") },
        { QStringLiteral("\u9019"), QStringLiteral("\u8fd9") },
        { QStringLiteral("\u4f86"), QStringLiteral("\u6765") },
        { QStringLiteral("\u70ba"), QStringLiteral("\u4e3a") },
        { QStringLiteral("\u8aaa"), QStringLiteral("\u8bf4") },
        { QStringLiteral("\u5225"), QStringLiteral("\u522b") },
        { QStringLiteral("\u59a8\u7919"), QStringLiteral("\u59a8\u788d") },
        { QStringLiteral("\u55da"), QStringLiteral("\u545c") },
        { QStringLiteral("\u5f8c"), QStringLiteral("\u540e") },
        { QStringLiteral("\u807d"), QStringLiteral("\u542c") },
        { QStringLiteral("\u9084"), QStringLiteral("\u8fd8") },
        { QStringLiteral("\u6703"), QStringLiteral("\u4f1a") },
        { QStringLiteral("\u9ebc"), QStringLiteral("\u4e48") }
    };
    for (const auto& replacement : replacements) {
        text.replace(replacement.first, replacement.second);
    }
    return text.replace(QStringLiteral("\r\n"), QStringLiteral(" "))
        .replace(QLatin1Char('\n'), QLatin1Char(' '))
        .replace(QLatin1Char('\r'), QLatin1Char(' '))
        .simplified();
}

int generatedSubtitleCueQualityScore(const GeneratedSubtitleCue& cue)
{
    const QString source = repairGeneratedSubtitleMojibake(cue.sourceText).trimmed();
    const QString translated = repairGeneratedSubtitleMojibake(cue.translatedText).trimmed();
    int score = 0;
    if (!source.isEmpty()) {
        score += 20 + std::min(80, static_cast<int>(source.size()));
    }
    if (!translated.isEmpty()) {
        score += 30 + std::min(100, static_cast<int>(translated.size()));
    }
    score -= generatedSubtitleMojibakeScore(source) * 4;
    score -= generatedSubtitleMojibakeScore(translated) * 4;
    return score;
}

bool isSubtitleGenerationSmokeTrack(const QVector<GeneratedSubtitleCue>& cues)
{
    if (cues.size() > 2) {
        return false;
    }
    for (const auto& cue : cues) {
        const QString text = (cue.sourceText + QLatin1Char('\n') + cue.translatedText).toLower();
        if (text.contains(QStringLiteral("cgplay subtitle generation smoke test")) ||
            text.contains(QStringLiteral("smoke test")) ||
            text.contains(QStringLiteral("cgplay"))) {
            return true;
        }
    }
    return false;
}

bool generatedSubtitleTrackUsable(
    const QVector<GeneratedSubtitleCue>& cues,
    double currentSeconds,
    double mediaDurationSeconds,
    bool allowSmokeTrack,
    QString* reason)
{
    if (cues.isEmpty()) {
        if (reason) {
            *reason = QStringLiteral("empty");
        }
        return false;
    }

    if (!allowSmokeTrack && isSubtitleGenerationSmokeTrack(cues)) {
        if (reason) {
            *reason = QStringLiteral("smoke-test-track");
        }
        return false;
    }

    const double maxEnd = generatedSubtitleTranslatedMaxEndSeconds(cues);
    if (maxEnd <= 0.0) {
        if (reason) {
            *reason = QStringLiteral("no-displayable-translation");
        }
        return false;
    }
    double minStart = std::numeric_limits<double>::max();
    for (const auto& cue : cues) {
        if (generatedSubtitleCueHasDisplayableTranslation(cue)) {
            minStart = std::min(minStart, cue.startSeconds);
        }
    }
    for (const auto& cue : cues) {
        if (generatedSubtitleCueHasDisplayableTranslation(cue) &&
            currentSeconds + 0.02 >= cue.startSeconds &&
            currentSeconds <= cue.endSeconds + 0.02) {
            return true;
        }
    }
    if (currentSeconds + 3.0 < minStart) {
        if (reason) {
            *reason = QStringLiteral("starts-after-current-time");
        }
        return false;
    }
    if (currentSeconds + 3.0 >= minStart && currentSeconds <= maxEnd + 3.0) {
        return true;
    }
    if (mediaDurationSeconds >= 60.0) {
        if (cues.size() < 3) {
            if (reason) {
                *reason = QStringLiteral("too-few-cues-for-media");
            }
            return false;
        }
        const double minimumUsefulEnd = std::min(mediaDurationSeconds * 0.20, 300.0);
        if (maxEnd < minimumUsefulEnd && maxEnd + 2.0 < mediaDurationSeconds) {
            if (reason) {
                *reason = QStringLiteral("too-short-for-media");
            }
            return false;
        }
        if (currentSeconds > maxEnd + 3.0 && maxEnd < mediaDurationSeconds * 0.80) {
            if (reason) {
                *reason = QStringLiteral("does-not-reach-current-time");
            }
            return false;
        }
    } else if (currentSeconds > maxEnd + 2.0) {
        if (reason) {
            *reason = QStringLiteral("does-not-reach-current-time");
        }
        return false;
    }

    return true;
}

QString generatedSubtitleTextForLeakScan(const QVector<GeneratedSubtitleCue>& cues)
{
    QString text;
    int scanned = 0;
    for (const auto& cue : cues) {
        text += QLatin1Char('\n');
        text += cue.sourceText;
        text += QLatin1Char('\n');
        text += cue.translatedText;
        if (++scanned >= 240 || text.size() > 120000) {
            break;
        }
    }
    return repairGeneratedSubtitleMojibake(text).toLower();
}

QString companionSubtitleSourceTextForMedia(const QString& mediaPath)
{
    QStringList sourcePaths;
    const QString sourceSrtPath = SubtitleGenerationService::defaultSourceSrtPath(mediaPath);
    const QString sourceVttPath = QFileInfo(sourceSrtPath).absolutePath() + QLatin1Char('/') +
        QFileInfo(sourceSrtPath).completeBaseName() + QStringLiteral(".vtt");
    sourcePaths << sourceVttPath << sourceSrtPath;

    const QString ocrSourcePath = SubtitleGenerationService::defaultOcrSubtitleCachePath(mediaPath);
    sourcePaths << ocrSourcePath;
    if (ocrSourcePath.endsWith(QStringLiteral(".ocr.source.vtt"))) {
        QString workbenchSourcePath = ocrSourcePath;
        workbenchSourcePath.chop(QStringLiteral(".ocr.source.vtt").size());
        workbenchSourcePath += QStringLiteral(".workbench.vision.source.vtt");
        sourcePaths << workbenchSourcePath;
    }
    sourcePaths << SubtitleGenerationService::defaultOnlineSubtitleCachePath(mediaPath);
    sourcePaths.removeDuplicates();

    QString text;
    for (const QString& path : sourcePaths) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        text += generatedSubtitleTextForLeakScan(SubtitleGenerationService::readSubtitleFile(path));
        if (text.size() > 180000) {
            break;
        }
    }
    return text.toLower();
}

bool sourceContextAllowsGeneratedSubtitleSignature(const QString& sourceText)
{
    if (sourceText.isEmpty()) {
        return true;
    }
    static const QStringList allowedSourceNeedles{
        QString::fromUtf8(u8"闇影庭園"),
        QString::fromUtf8(u8"暗影庭园"),
        QString::fromUtf8(u8"暗影庭園"),
        QString::fromUtf8(u8"shadow garden"),
        QString::fromUtf8(u8"shadow-garden"),
        QString::fromUtf8(u8"numbered"),
        QString::fromUtf8(u8"named numbers"),
        QString::fromUtf8(u8"hunting begins"),
        QString::fromUtf8(u8"hunt begins")
    };
    for (const QString& needle : allowedSourceNeedles) {
        if (sourceText.contains(needle.toLower())) {
            return true;
        }
    }
    return false;
}

bool generatedSubtitleTextLooksCrossMediaLeaked(
    const QString& text,
    const QString& companionSourceText,
    QString* reason)
{
    const QString compact = repairGeneratedSubtitleMojibake(text).toLower().simplified();
    if (compact.isEmpty()) {
        return false;
    }
    static const QStringList foreignSignatureNeedles{
        QString::fromUtf8(u8"暗影庭园"),
        QString::fromUtf8(u8"闇影庭園"),
        QString::fromUtf8(u8"影之强者"),
        QString::fromUtf8(u8"影野"),
        QString::fromUtf8(u8"吾等乃暗影"),
        QString::fromUtf8(u8"编号者"),
        QString::fromUtf8(u8"編號者"),
        QString::fromUtf8(u8"狩猎开始"),
        QString::fromUtf8(u8"狩獵開始")
    };
    for (const QString& needle : foreignSignatureNeedles) {
        if (compact.contains(needle.toLower()) &&
            !sourceContextAllowsGeneratedSubtitleSignature(companionSourceText)) {
            if (reason) {
                *reason = QStringLiteral("foreign-signature:%1").arg(needle);
            }
            return true;
        }
    }
    return false;
}

int scrubGeneratedSubtitleCrossMediaLeaks(
    QVector<GeneratedSubtitleCue>* cues,
    const QString& companionSourceText,
    const QString& role,
    QString* firstReason)
{
    if (!cues || cues->isEmpty()) {
        return 0;
    }
    int removed = 0;
    for (auto& cue : *cues) {
        QString reason;
        if (!generatedSubtitleTextLooksCrossMediaLeaked(cue.translatedText, companionSourceText, &reason)) {
            continue;
        }
        if (firstReason && firstReason->isEmpty()) {
            *firstReason = QStringLiteral("%1:%2").arg(role, reason);
        }
        cue.translatedText.clear();
        ++removed;
    }
    return removed;
}

bool generatedSubtitleTextLooksLikeRawEnglishFinal(const QString& text)
{
    const QString value = repairGeneratedSubtitleMojibake(text).simplified();
    if (value.isEmpty()) {
        return false;
    }
    int latinWords = 0;
    int cjk = 0;
    bool inLatinWord = false;
    for (const QChar ch : value) {
        if ((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
            (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))) {
            if (!inLatinWord) {
                ++latinWords;
                inLatinWord = true;
            }
            continue;
        }
        inLatinWord = false;
        if (ch.unicode() >= 0x4e00 && ch.unicode() <= 0x9fff) {
            ++cjk;
        }
    }
    return latinWords >= 3 && cjk == 0;
}

bool generatedSubtitleHasCueAtSeconds(const QVector<GeneratedSubtitleCue>& cues, double seconds)
{
    for (const auto& cue : cues) {
        if (generatedSubtitleCueHasDisplayableTranslation(cue) &&
            seconds + 0.02 >= cue.startSeconds &&
            seconds <= cue.endSeconds + 0.02) {
            return true;
        }
    }
    return false;
}

bool generatedSubtitleHasCueNearSeconds(const QVector<GeneratedSubtitleCue>& cues, double seconds, double toleranceSeconds)
{
    for (const auto& cue : cues) {
        if (generatedSubtitleCueHasDisplayableTranslation(cue) &&
            seconds + toleranceSeconds >= cue.startSeconds &&
            seconds <= cue.endSeconds + toleranceSeconds) {
            return true;
        }
    }
    return false;
}

bool generatedSubtitleCueTimingsAlign(
    const GeneratedSubtitleCue& quick,
    const GeneratedSubtitleCue& refined)
{
    const double quickDuration = std::max(0.05, quick.endSeconds - quick.startSeconds);
    const double refinedDuration = std::max(0.05, refined.endSeconds - refined.startSeconds);
    const double overlapStart = std::max(quick.startSeconds, refined.startSeconds);
    const double overlapEnd = std::min(quick.endSeconds, refined.endSeconds);
    const double overlap = std::max(0.0, overlapEnd - overlapStart);
    const bool closeBoundary =
        std::abs(quick.startSeconds - refined.startSeconds) <= 0.35 &&
        std::abs(quick.endSeconds - refined.endSeconds) <= 0.50;
    const bool mostlyOverlaps =
        overlap / quickDuration >= 0.70 &&
        overlap / refinedDuration >= 0.70;
    return closeBoundary || mostlyOverlaps;
}

int findGeneratedSubtitleRefinedMatch(
    const QVector<GeneratedSubtitleCue>& refinedCues,
    const GeneratedSubtitleCue& quickCue,
    QSet<int>* usedRefinedIndexes)
{
    if (quickCue.index > 0 && quickCue.index <= refinedCues.size()) {
        const int candidateIndex = quickCue.index - 1;
        if ((!usedRefinedIndexes || !usedRefinedIndexes->contains(candidateIndex)) &&
            generatedSubtitleCueTimingsAlign(quickCue, refinedCues.at(candidateIndex))) {
            return candidateIndex;
        }
    }
    int bestIndex = -1;
    double bestDelta = std::numeric_limits<double>::max();
    for (int i = 0; i < refinedCues.size(); ++i) {
        if (usedRefinedIndexes && usedRefinedIndexes->contains(i)) {
            continue;
        }
        const auto& refined = refinedCues.at(i);
        if (!generatedSubtitleCueTimingsAlign(quickCue, refined)) {
            continue;
        }
        const double delta =
            std::abs(quickCue.startSeconds - refined.startSeconds) +
            std::abs(quickCue.endSeconds - refined.endSeconds);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = i;
        }
    }
    return bestIndex;
}

int mergeGeneratedSubtitleRefinedEnhancements(
    QVector<GeneratedSubtitleCue>* quickCues,
    const QVector<GeneratedSubtitleCue>& refinedCues,
    int* matchedCueCount)
{
    if (matchedCueCount) {
        *matchedCueCount = 0;
    }
    if (!quickCues || quickCues->isEmpty() || refinedCues.isEmpty()) {
        return 0;
    }
    QSet<int> usedRefinedIndexes;
    int changedCount = 0;
    for (auto& quickCue : *quickCues) {
        repairGeneratedSubtitleCueText(&quickCue);
        if (!generatedSubtitleCueHasDisplayableTranslation(quickCue)) {
            continue;
        }
        const int refinedIndex = findGeneratedSubtitleRefinedMatch(refinedCues, quickCue, &usedRefinedIndexes);
        if (refinedIndex < 0) {
            continue;
        }
        GeneratedSubtitleCue refinedCue = refinedCues.at(refinedIndex);
        repairGeneratedSubtitleCueText(&refinedCue);
        const QString refinedText = refinedCue.translatedText.trimmed();
        if (refinedText.isEmpty() || refinedCue.endSeconds <= refinedCue.startSeconds) {
            continue;
        }
        usedRefinedIndexes.insert(refinedIndex);
        if (matchedCueCount) {
            *matchedCueCount += 1;
        }
        if (refinedText != quickCue.translatedText.trimmed()) {
            quickCue.translatedText = refinedText;
            ++changedCount;
        }
    }
    for (int i = 0; i < quickCues->size(); ++i) {
        (*quickCues)[i].index = i + 1;
    }
    return changedCount;
}

double generatedSubtitleCurrentSeconds(IPlaybackService* playbackCtrl)
{
    if (!playbackCtrl) {
        return 0.0;
    }
    const double fps = playbackCtrl->fps() > 0.0 ? playbackCtrl->fps() : 24.0;
    return std::max(0, playbackCtrl->currentFrame()) / fps;
}

void mergeGeneratedSubtitleCues(
    QVector<GeneratedSubtitleCue>* target,
    const QVector<GeneratedSubtitleCue>& incoming,
    bool preserveExistingDisplayText)
{
    if (!target || incoming.isEmpty()) {
        return;
    }
    for (auto cue : incoming) {
        repairGeneratedSubtitleCueText(&cue);
        if (cue.endSeconds <= cue.startSeconds) {
            continue;
        }
        int duplicateIndex = -1;
        bool sameTimingExistingDisplay = false;
        for (int i = 0; i < target->size(); ++i) {
            const auto& existing = target->at(i);
            const bool sameTiming =
                std::abs(existing.startSeconds - cue.startSeconds) < 0.25 &&
                std::abs(existing.endSeconds - cue.endSeconds) < 0.25;
            const bool sameText =
                existing.sourceText.trimmed() == cue.sourceText.trimmed() &&
                existing.translatedText.trimmed() == cue.translatedText.trimmed();
            if (sameTiming && sameText) {
                duplicateIndex = i;
                break;
            }
            if (preserveExistingDisplayText &&
                sameTiming &&
                generatedSubtitleCueHasDisplayableTranslation(existing)) {
                duplicateIndex = i;
                sameTimingExistingDisplay = true;
                break;
            }
        }
        if (duplicateIndex >= 0) {
            if (sameTimingExistingDisplay) {
                continue;
            }
            if (generatedSubtitleCueQualityScore(cue) > generatedSubtitleCueQualityScore(target->at(duplicateIndex))) {
                (*target)[duplicateIndex] = cue;
            }
        } else {
            target->push_back(cue);
        }
    }
    std::sort(target->begin(), target->end(), [](const GeneratedSubtitleCue& a, const GeneratedSubtitleCue& b) {
        if (a.startSeconds == b.startSeconds) {
            return a.endSeconds < b.endSeconds;
        }
        return a.startSeconds < b.startSeconds;
    });
    for (int i = 0; i < target->size(); ++i) {
        (*target)[i].index = i + 1;
    }
    QVector<GeneratedSubtitleCue> deduped;
    deduped.reserve(target->size());
    for (const auto& cue : *target) {
        if (!deduped.isEmpty()) {
            GeneratedSubtitleCue& previous = deduped.last();
            const bool sameTiming =
                std::abs(previous.startSeconds - cue.startSeconds) < 0.25 &&
                std::abs(previous.endSeconds - cue.endSeconds) < 0.25;
            if (sameTiming) {
                if (preserveExistingDisplayText &&
                    generatedSubtitleCueHasDisplayableTranslation(previous)) {
                    continue;
                }
                if (generatedSubtitleCueQualityScore(cue) > generatedSubtitleCueQualityScore(previous)) {
                    previous = cue;
                }
                continue;
            }
        }
        deduped.push_back(cue);
    }
    *target = deduped;
    for (int i = 0; i < target->size(); ++i) {
        (*target)[i].index = i + 1;
    }
}

bool subtitleGenerationErrorLooksRateLimited(const QString& errorMessage)
{
    const QString normalized = errorMessage.trimmed().toLower();
    return normalized.contains(QStringLiteral("429")) ||
        normalized.contains(QStringLiteral("quota/rate limit")) ||
        normalized.contains(QStringLiteral("resource_exhausted")) ||
        normalized.contains(QStringLiteral("quota exceeded")) ||
        normalized.contains(QStringLiteral("rate limit"));
}

QString generatedSubtitleSrtTime(double seconds)
{
    const qint64 totalMs = std::max<qint64>(0, std::llround(seconds * 1000.0));
    const qint64 ms = totalMs % 1000;
    const qint64 totalSeconds = totalMs / 1000;
    const qint64 s = totalSeconds % 60;
    const qint64 totalMinutes = totalSeconds / 60;
    const qint64 m = totalMinutes % 60;
    const qint64 h = totalMinutes / 60;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

QString generatedSubtitleVttTime(double seconds)
{
    QString value = generatedSubtitleSrtTime(seconds);
    value.replace(QLatin1Char(','), QLatin1Char('.'));
    return value;
}

bool writeGeneratedSubtitleTrack(
    const QString& path,
    const QVector<GeneratedSubtitleCue>& cues,
    bool translated,
    bool webVtt)
{
    QFile file(path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    if (webVtt) {
        stream << "WEBVTT\n\n";
    }
    int index = 1;
    for (const auto& cue : cues) {
        const QString rawText = translated
            ? cue.translatedText.trimmed()
            : cue.sourceText.trimmed();
        const QString text = repairGeneratedSubtitleMojibake(rawText);
        if (text.isEmpty()) {
            continue;
        }
        if (!webVtt) {
            stream << index++ << "\n";
            stream << generatedSubtitleSrtTime(cue.startSeconds) << " --> "
                   << generatedSubtitleSrtTime(cue.endSeconds) << "\n";
        } else {
            stream << generatedSubtitleVttTime(cue.startSeconds) << " --> "
                   << generatedSubtitleVttTime(cue.endSeconds) << "\n";
        }
        stream << text << "\n\n";
    }
    return true;
}

bool writeGeneratedTranslatedSubtitleSidecars(
    const QString& srtPath,
    const QString& vttPath,
    const QVector<GeneratedSubtitleCue>& cues,
    const QString& reason)
{
    if (cues.isEmpty()) {
        return false;
    }
    if (generatedSubtitleTranslatedMaxEndSeconds(cues) <= 0.0) {
        qWarning() << "[SubtitleGeneration] Refusing to persist translated subtitle sidecars without displayable Chinese"
                   << reason
                   << "sourceCoverageEnd=" << generatedSubtitleMaxEndSeconds(cues);
        return false;
    }
    const bool srtOk = writeGeneratedSubtitleTrack(srtPath, cues, true, false);
    const bool vttOk = writeGeneratedSubtitleTrack(vttPath, cues, true, true);
    if (!srtOk || !vttOk) {
        qWarning() << "[SubtitleGeneration] Failed to persist translated subtitle sidecars"
                   << reason
                   << "srtOk=" << srtOk
                   << "vttOk=" << vttOk
                   << "cueCount=" << cues.size();
        return false;
    }
    qInfo() << "[SubtitleGeneration] Persisted translated subtitle sidecars"
            << reason
            << "cueCount=" << cues.size()
            << "translatedCoverageEnd=" << generatedSubtitleTranslatedMaxEndSeconds(cues)
            << "sourceCoverageEnd=" << generatedSubtitleMaxEndSeconds(cues);
    return true;
}

void writeGeneratedSubtitleMediaIdentitySidecars(
    const QStringList& paths,
    const QString& mediaPath,
    double durationSeconds,
    const QString& reason)
{
    for (const QString& path : paths) {
        if (path.trimmed().isEmpty() || !QFileInfo::exists(path)) {
            continue;
        }
        QString error;
        if (!SubtitleGenerationService::writeMediaIdentitySidecar(path, mediaPath, durationSeconds, &error)) {
            qWarning() << "[SubtitleGeneration] Failed to write generated subtitle media identity sidecar"
                       << reason << path << error;
        }
    }
}

bool generatedSubtitleCacheMatchesCurrentMedia(
    const QString& path,
    const QString& mediaPath,
    double durationSeconds,
    QString* mismatchReason,
    QJsonObject* recordedIdentity)
{
    if (!QFileInfo::exists(path)) {
        if (mismatchReason) {
            *mismatchReason = QStringLiteral("cache-missing");
        }
        return false;
    }
    return SubtitleGenerationService::cacheMatchesMediaIdentity(
        path,
        mediaPath,
        durationSeconds,
        mismatchReason,
        recordedIdentity);
}

QString formatCodecText(const MediaInfo& media)
{
    return media.codecDisplayText();
}

TranslationEnhancementScheduleRequest MainWindow::_buildTranslationEnhancementRequest(
    bool manualRequested,
    bool executeWorkers,
    bool allowWhilePlayback) const
{
    const QString modeId = TranslationPlaybackStrategy::normalizeModeId(_p->translationPlaybackMode);
    const double currentSeconds = generatedSubtitleCurrentSeconds(_p->playbackCtrl.get());
    const double quickCoverageEndSeconds =
        generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
    const bool playbackAlreadyRunning =
        _p->playbackCtrl && _p->playbackCtrl->playbackState() != 0;
    const QString quickTranslatedVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultTranslatedVttPath(_p->currentPath, QStringLiteral("zh-Hans"));

    TranslationEnhancementScheduleRequest schedulerRequest;
    schedulerRequest.playbackMode = modeId;
    schedulerRequest.mediaPath = _p->currentPath;
    schedulerRequest.quickTranslatedVttPath = quickTranslatedVttPath;
    schedulerRequest.refinedTranslatedVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultRefinedTranslatedVttPath(_p->currentPath, QStringLiteral("zh-Hans"));
    schedulerRequest.onlineSubtitleCachePath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultOnlineSubtitleCachePath(_p->currentPath);
    schedulerRequest.onlineTranslatedCachePath = schedulerRequest.onlineSubtitleCachePath;
    if (schedulerRequest.onlineTranslatedCachePath.endsWith(QStringLiteral(".online.source.vtt"))) {
        schedulerRequest.onlineTranslatedCachePath.chop(QStringLiteral(".online.source.vtt").size());
        schedulerRequest.onlineTranslatedCachePath += QStringLiteral(".online.translated.zh.vtt");
    } else if (!schedulerRequest.onlineTranslatedCachePath.trimmed().isEmpty()) {
        schedulerRequest.onlineTranslatedCachePath += QStringLiteral(".translated.zh.vtt");
    }
    schedulerRequest.ocrSubtitleCachePath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultOcrSubtitleCachePath(_p->currentPath);
    schedulerRequest.ocrTranslatedCachePath = schedulerRequest.ocrSubtitleCachePath;
    if (schedulerRequest.ocrTranslatedCachePath.endsWith(QStringLiteral(".ocr.source.vtt"))) {
        schedulerRequest.ocrTranslatedCachePath.chop(QStringLiteral(".ocr.source.vtt").size());
        schedulerRequest.ocrTranslatedCachePath += QStringLiteral(".ocr.translated.zh.vtt");
    } else if (!schedulerRequest.ocrTranslatedCachePath.trimmed().isEmpty()) {
        schedulerRequest.ocrTranslatedCachePath += QStringLiteral(".translated.zh.vtt");
    }
    schedulerRequest.longAsrSourceCachePath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : schedulerRequest.ocrSubtitleCachePath;
    if (schedulerRequest.longAsrSourceCachePath.endsWith(QStringLiteral(".ocr.source.vtt"))) {
        schedulerRequest.longAsrSourceCachePath.chop(QStringLiteral(".ocr.source.vtt").size());
        schedulerRequest.longAsrSourceCachePath += QStringLiteral(".longasr.source.vtt");
    }
    schedulerRequest.longAsrTranslatedCachePath = schedulerRequest.longAsrSourceCachePath;
    if (schedulerRequest.longAsrTranslatedCachePath.endsWith(QStringLiteral(".longasr.source.vtt"))) {
        schedulerRequest.longAsrTranslatedCachePath.chop(QStringLiteral(".longasr.source.vtt").size());
        schedulerRequest.longAsrTranslatedCachePath += QStringLiteral(".hq.translated.zh.vtt");
    }
    schedulerRequest.longAsrJsonPath = schedulerRequest.longAsrSourceCachePath;
    if (schedulerRequest.longAsrJsonPath.endsWith(QStringLiteral(".longasr.source.vtt"))) {
        schedulerRequest.longAsrJsonPath.chop(QStringLiteral(".longasr.source.vtt").size());
        schedulerRequest.longAsrJsonPath += QStringLiteral(".longasr.report.json");
    }
    schedulerRequest.enhancedTranslatedVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultEnhancedTranslatedVttPath(_p->currentPath, QStringLiteral("zh-Hans"));
    schedulerRequest.repairTranslatedVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultLowConfidenceRepairVttPath(_p->currentPath, QStringLiteral("zh-Hans"));
    schedulerRequest.manualRequested = manualRequested;
    schedulerRequest.executeWorkers = executeWorkers;
    schedulerRequest.allowManualWhilePlayback = allowWhilePlayback;
    schedulerRequest.playbackAlreadyRunning = playbackAlreadyRunning;
    schedulerRequest.quickGenerationBusy = _p->subtitleGenerationBusy;
    schedulerRequest.continuationScheduled = _p->subtitleContinuationScheduled;
    schedulerRequest.highQualityWaitActive = _p->translationPrePlaybackWaitActive;
    schedulerRequest.currentSeconds = currentSeconds;
    schedulerRequest.quickCoverageEndSeconds = quickCoverageEndSeconds;
    schedulerRequest.highQualityTargetCoverageSeconds =
        _p->translationPrePlaybackWaitTargetSeconds > 0.0
        ? _p->translationPrePlaybackWaitTargetSeconds
        : kHighQualityPrePlaybackTargetSeconds;
    schedulerRequest.quickCueCount = _p->generatedSubtitleCues.size();
    schedulerRequest.lowConfidenceQuickCueCount =
        std::count_if(_p->generatedSubtitleCues.cbegin(), _p->generatedSubtitleCues.cend(), [](const GeneratedSubtitleCue& cue) {
            const QString reason = cue.qualityReason.toLower();
            return cue.lowConfidence ||
                reason.contains(QStringLiteral("english")) ||
                reason.contains(QStringLiteral("latin")) ||
                reason.contains(QStringLiteral("filler")) ||
                reason.contains(QStringLiteral("drift")) ||
                reason.contains(QStringLiteral("source-mismatch"));
        });
    schedulerRequest.lowConfidenceQuickCueRatio =
        _p->generatedSubtitleCues.isEmpty()
        ? 0.0
        : static_cast<double>(schedulerRequest.lowConfidenceQuickCueCount) /
              static_cast<double>(_p->generatedSubtitleCues.size());
    schedulerRequest.onlineMockMode =
        qApp && qApp->property("cgplay.subtitleGenerationMockOnlineWorker").toBool();
    schedulerRequest.ocrMockMode =
        qApp && qApp->property("cgplay.subtitleGenerationMockOcrWorker").toBool();
    schedulerRequest.lowConfidenceRepairMockMode =
        qApp && qApp->property("cgplay.subtitleGenerationMockRepairWorker").toBool();
    auto envBool = [](const QString& name) {
        const QString value =
            QProcessEnvironment::systemEnvironment().value(name).trimmed().toLower();
        return value == QStringLiteral("1") ||
            value == QStringLiteral("true") ||
            value == QStringLiteral("yes") ||
            value == QStringLiteral("on");
    };
    const bool highQualityManualRequest =
        modeId == TranslationPlaybackStrategy::highQualityModeId() && manualRequested;
    QString smartWorkbenchApiKey;
    QString smartWorkbenchBaseUrl;
    QString smartWorkbenchModel;
    if (_p->userSettings) {
        smartWorkbenchBaseUrl = firstNonEmptySetting(_p->userSettings, {
            "ai/connection/baseUrl",
            "ai/providers/openai/baseUrl"
        });
        if (smartWorkbenchBaseUrl.trimmed().isEmpty()) {
            smartWorkbenchBaseUrl = firstNonEmptyEnv({
                "AI_BASE_URL",
                "OPENAI_BASE_URL"
            });
        }
        smartWorkbenchModel = firstNonEmptySetting(_p->userSettings, {
            "ai/workspace/model",
            "ai/connection/model",
            "ai/connection/recommendedModel",
            "ai/providers/openai/model"
        });
        if (smartWorkbenchModel.trimmed().isEmpty()) {
            smartWorkbenchModel = firstNonEmptyEnv({
                "AI_MODEL",
                "OPENAI_MODEL"
            });
        }
        smartWorkbenchApiKey = firstNonEmptySecret({
            kGenericCredentialId,
            kOpenAICredentialId
        });
        if (smartWorkbenchApiKey.trimmed().isEmpty()) {
            smartWorkbenchApiKey = firstNonEmptyEnv({
                "AI_API_KEY",
                "OPENAI_API_KEY"
            });
        }
        schedulerRequest.onlineConfigured =
            _p->userSettings->value(QStringLiteral("ai/subtitles/online/enabled"), false).toBool() ||
            envBool(QStringLiteral("SUBTITLE_ONLINE_ENABLED")) ||
            schedulerRequest.onlineMockMode;
        schedulerRequest.onlineApiKey =
            firstNonEmptySecret({ kOnlineSubtitleCredentialId });
        schedulerRequest.onlineProvider =
            _p->userSettings->value(QStringLiteral("ai/subtitles/online/provider"),
                                    QStringLiteral("opensubtitles-compatible")).toString().trimmed();
        schedulerRequest.onlineBaseUrl =
            _p->userSettings->value(QStringLiteral("ai/subtitles/online/baseUrl")).toString().trimmed();
        schedulerRequest.onlineLanguage =
            _p->userSettings->value(QStringLiteral("ai/subtitles/online/language"), QStringLiteral("zh,ja,en"))
                .toString()
                .trimmed();
    schedulerRequest.onlineHelperScriptPath =
            _p->userSettings->value(QStringLiteral("ai/subtitles/online/helperScript")).toString().trimmed();
        schedulerRequest.translationApiKey = firstNonEmptySecret({
            kSubtitleTranslationCredentialId,
            kQwenApiKeyId,
            kMimoCredentialId,
            kGenericCredentialId,
            kOpenAICredentialId
        });
        schedulerRequest.translationBaseUrl =
            _p->userSettings->value(QStringLiteral("ai/subtitles/translation/baseUrl")).toString().trimmed();
        schedulerRequest.translationModel =
            _p->userSettings->value(QStringLiteral("ai/subtitles/translationModel")).toString().trimmed();
        if (highQualityManualRequest &&
            !smartWorkbenchBaseUrl.isEmpty() &&
            !smartWorkbenchModel.isEmpty() &&
            !smartWorkbenchApiKey.isEmpty()) {
            schedulerRequest.translationBaseUrl = smartWorkbenchBaseUrl;
            schedulerRequest.translationModel = smartWorkbenchModel;
            schedulerRequest.translationApiKey = smartWorkbenchApiKey;
            schedulerRequest.workbenchConfigSource = QStringLiteral("smart-recognition");
        }
        if (schedulerRequest.translationBaseUrl.trimmed().isEmpty()) {
            schedulerRequest.translationBaseUrl = firstNonEmptySetting(_p->userSettings, {
                "ai/subtitles/qwen/baseUrl",
                "ai/subtitles/mimo/baseUrl",
                "ai/connection/baseUrl",
                "ai/providers/openai/baseUrl"
            });
        }
        if (schedulerRequest.translationModel.trimmed().isEmpty()) {
            schedulerRequest.translationModel = firstNonEmptySetting(_p->userSettings, {
                "ai/workspace/model",
                "ai/connection/model",
                "ai/connection/recommendedModel",
                "ai/providers/openai/model"
            });
        }
        if (schedulerRequest.translationApiKey.trimmed().isEmpty()) {
            schedulerRequest.translationApiKey = firstNonEmptySecret({
                kGenericCredentialId,
                kOpenAICredentialId,
                kQwenApiKeyId
            });
        }
        schedulerRequest.ocrConfigured =
            _p->userSettings->value(QStringLiteral("ai/subtitles/ocr/enabled"), false).toBool() ||
            envBool(QStringLiteral("SUBTITLE_OCR_ENABLED")) ||
            schedulerRequest.ocrMockMode;
        schedulerRequest.ocrHelperScriptPath =
            _p->userSettings->value(QStringLiteral("ai/subtitles/ocr/helperScript")).toString().trimmed();
        schedulerRequest.longAsrConfigured =
            _p->userSettings->value(QStringLiteral("ai/subtitles/longAsr/enabled"), false).toBool() ||
            envBool(QStringLiteral("SUBTITLE_LONG_ASR_ENABLED"));
        schedulerRequest.longAsrHelperScriptPath =
            _p->userSettings->value(QStringLiteral("ai/subtitles/longAsr/helperScript")).toString().trimmed();
        schedulerRequest.fusionConfigured =
            _p->userSettings->value(QStringLiteral("ai/subtitles/fusion/enabled"), true).toBool();
        schedulerRequest.lowConfidenceRepairConfigured =
            _p->userSettings->value(QStringLiteral("ai/subtitles/repair/enabled"), false).toBool() ||
            envBool(QStringLiteral("SUBTITLE_REPAIR_ENABLED")) ||
            schedulerRequest.lowConfidenceRepairMockMode;
    }
    if (schedulerRequest.workbenchConfigSource.trimmed().isEmpty()) {
        schedulerRequest.workbenchConfigSource =
            highQualityManualRequest
                ? QStringLiteral("legacy-subtitle-translation-fallback")
                : QStringLiteral("subtitle-translation");
    }
    if (schedulerRequest.onlineMockMode) {
        schedulerRequest.onlineConfigured = true;
    } else if (!schedulerRequest.onlineConfigured) {
        schedulerRequest.onlineConfigured = envBool(QStringLiteral("SUBTITLE_ONLINE_ENABLED"));
    }
    if (schedulerRequest.ocrMockMode) {
        schedulerRequest.ocrConfigured = true;
    } else if (!schedulerRequest.ocrConfigured) {
        schedulerRequest.ocrConfigured = envBool(QStringLiteral("SUBTITLE_OCR_ENABLED"));
    }
    if (!schedulerRequest.longAsrConfigured) {
        schedulerRequest.longAsrConfigured = envBool(QStringLiteral("SUBTITLE_LONG_ASR_ENABLED"));
    }
    if (schedulerRequest.lowConfidenceRepairMockMode) {
        schedulerRequest.lowConfidenceRepairConfigured = true;
    } else if (!schedulerRequest.lowConfidenceRepairConfigured) {
        schedulerRequest.lowConfidenceRepairConfigured = envBool(QStringLiteral("SUBTITLE_REPAIR_ENABLED"));
    }
    schedulerRequest.localReferenceSubtitlePath = localReferenceSubtitlePathForMedia(_p->currentPath);
    schedulerRequest.localReferenceProviderAvailable =
        !schedulerRequest.localReferenceSubtitlePath.trimmed().isEmpty();
    const QFileInfo enhancedInfo(schedulerRequest.enhancedTranslatedVttPath);
    const QFileInfo quickInfo(schedulerRequest.quickTranslatedVttPath);
    const QVector<GeneratedSubtitleCue> enhancedExistingCues =
        SubtitleGenerationService::readSubtitleFile(schedulerRequest.enhancedTranslatedVttPath);
    const QVector<GeneratedSubtitleCue> ocrTranslatedExistingCues =
        SubtitleGenerationService::readSubtitleFile(schedulerRequest.ocrTranslatedCachePath);
    const auto hasVisualCueNearCurrent = [currentSeconds](const QVector<GeneratedSubtitleCue>& cues) {
        for (const GeneratedSubtitleCue& cue : cues) {
            if (!cue.translatedText.trimmed().isEmpty() &&
                cue.startSeconds <= currentSeconds + 5.0 &&
                cue.endSeconds + 5.0 >= currentSeconds) {
                return true;
            }
        }
        return false;
    };
    const double enhancedExistingCoverageEndSeconds =
        generatedSubtitleTranslatedMaxEndSeconds(enhancedExistingCues);
    bool enhancedCacheBelowTargetCoverage =
        enhancedExistingCoverageEndSeconds + 0.5 <
            currentSeconds + schedulerRequest.highQualityTargetCoverageSeconds;
    const bool enhancedCacheMissingEmptyOrStale =
        schedulerRequest.enhancedTranslatedVttPath.trimmed().isEmpty() ||
        !enhancedInfo.exists() ||
        enhancedExistingCues.isEmpty() ||
        (quickInfo.exists() && enhancedInfo.exists() &&
         enhancedInfo.lastModified().msecsTo(quickInfo.lastModified()) > 2000);
    bool enhancedCacheNeedsHighQualityRefresh =
        enhancedCacheMissingEmptyOrStale || enhancedCacheBelowTargetCoverage;
    const bool shouldTryVisualSubtitleRecognitionForHighQuality =
        highQualityManualRequest &&
        schedulerRequest.localReferenceSubtitlePath.trimmed().isEmpty();
    if (shouldTryVisualSubtitleRecognitionForHighQuality && _p->playbackCtrl) {
        const double fps = _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
        const double mediaDurationSeconds = _p->playbackCtrl->totalFrames() > 0
            ? static_cast<double>(_p->playbackCtrl->totalFrames()) / fps
            : 0.0;
        const double remainingSeconds = mediaDurationSeconds > currentSeconds
            ? mediaDurationSeconds - currentSeconds
            : 0.0;
        if (remainingSeconds > 0.0) {
            schedulerRequest.highQualityTargetCoverageSeconds = std::min(
                schedulerRequest.highQualityTargetCoverageSeconds,
                remainingSeconds);
        }
    }
    const bool currentVisualSubtitleCoverageMissing =
        shouldTryVisualSubtitleRecognitionForHighQuality &&
        !hasVisualCueNearCurrent(ocrTranslatedExistingCues);
    if (modeId == TranslationPlaybackStrategy::highQualityModeId() &&
        manualRequested &&
        !_p->currentPath.trimmed().isEmpty() &&
        shouldTryVisualSubtitleRecognitionForHighQuality &&
        (enhancedCacheNeedsHighQualityRefresh || currentVisualSubtitleCoverageMissing) &&
        !schedulerRequest.ocrConfigured &&
        !schedulerRequest.ocrMockMode) {
        schedulerRequest.ocrConfigured = true;
        schedulerRequest.ocrAutoEnabledForHighQualityCurrentMedia = true;
        schedulerRequest.ocrAutoEnableReason = currentVisualSubtitleCoverageMissing
            ? QStringLiteral("current-frame-visual-subtitle-coverage-missing")
            : QStringLiteral("generic-high-quality-source-decision-visual-before-asr");
    }
    if (schedulerRequest.onlineApiKey.trimmed().isEmpty()) {
        schedulerRequest.onlineApiKey =
            QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_ONLINE_API_KEY")).trimmed();
    }
    if (schedulerRequest.onlineApiKey.trimmed().isEmpty()) {
        schedulerRequest.onlineApiKey = firstNonEmptySecret({ kOnlineSubtitleCredentialId });
    }
    if (schedulerRequest.onlineBaseUrl.trimmed().isEmpty()) {
        schedulerRequest.onlineBaseUrl =
            QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_ONLINE_BASE_URL")).trimmed();
    }
    const auto comparableEndpoint = [](QString value) {
        value = value.trimmed().toLower();
        while (value.endsWith(QLatin1Char('/'))) {
            value.chop(1);
        }
        return value;
    };
    const QString onlineEndpoint = comparableEndpoint(schedulerRequest.onlineBaseUrl);
    const QString translationEndpoint = comparableEndpoint(schedulerRequest.translationBaseUrl);
    if (schedulerRequest.onlineConfigured &&
        !schedulerRequest.onlineMockMode &&
        schedulerRequest.onlineProvider.compare(QStringLiteral("opensubtitles-compatible"), Qt::CaseInsensitive) == 0 &&
        !onlineEndpoint.isEmpty() &&
        !translationEndpoint.isEmpty() &&
        (onlineEndpoint == translationEndpoint ||
         onlineEndpoint.startsWith(translationEndpoint + QStringLiteral("/v1")) ||
         translationEndpoint.startsWith(onlineEndpoint + QStringLiteral("/v1")))) {
        schedulerRequest.onlineConfigured = false;
        schedulerRequest.onlineBaseUrl.clear();
    }
    if (schedulerRequest.translationApiKey.trimmed().isEmpty()) {
        schedulerRequest.translationApiKey =
            QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_TRANSLATION_API_KEY")).trimmed();
    }
    if (schedulerRequest.translationApiKey.trimmed().isEmpty()) {
            schedulerRequest.translationApiKey = firstNonEmptySecret({
                kSubtitleTranslationCredentialId,
                "qwen/apiKey",
                kMimoCredentialId,
            kGenericCredentialId,
            kOpenAICredentialId
        });
    }
    if (schedulerRequest.translationApiKey.trimmed().isEmpty()) {
        schedulerRequest.translationApiKey = firstNonEmptyEnv({
            "QWEN_API_KEY",
            "DASHSCOPE_API_KEY",
            "MIMO_API_KEY",
            "XIAOMI_MIMO_API_KEY",
            "AI_API_KEY",
            "OPENAI_API_KEY"
        });
    }
    if (schedulerRequest.translationBaseUrl.trimmed().isEmpty()) {
        schedulerRequest.translationBaseUrl =
            QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_TRANSLATION_BASE_URL")).trimmed();
    }
    if (schedulerRequest.translationBaseUrl.trimmed().isEmpty()) {
        schedulerRequest.translationBaseUrl = firstNonEmptyEnv({
            "QWEN_BASE_URL",
            "DASHSCOPE_BASE_URL",
            "MIMO_BASE_URL",
            "XIAOMI_MIMO_BASE_URL",
            "AI_BASE_URL",
            "OPENAI_BASE_URL"
        });
    }
    if (schedulerRequest.translationModel.trimmed().isEmpty()) {
        schedulerRequest.translationModel =
            QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_TRANSLATION_MODEL")).trimmed();
    }
    if (schedulerRequest.translationModel.trimmed().isEmpty()) {
        schedulerRequest.translationModel = firstNonEmptyEnv({
            "QWEN_MODEL",
            "DASHSCOPE_MODEL",
            "MIMO_MODEL",
            "XIAOMI_MIMO_MODEL",
            "AI_MODEL",
            "OPENAI_MODEL"
        });
    }
    if (schedulerRequest.translationBaseUrl.trimmed().isEmpty() &&
        schedulerRequest.translationApiKey.trimmed().startsWith(QStringLiteral("sk-"), Qt::CaseInsensitive)) {
        schedulerRequest.translationBaseUrl = QStringLiteral("https://api.openai.com");
    }
    if (schedulerRequest.translationModel.trimmed().isEmpty() &&
        !schedulerRequest.translationApiKey.trimmed().isEmpty() &&
        !schedulerRequest.translationBaseUrl.trimmed().isEmpty()) {
        const QString baseLower = schedulerRequest.translationBaseUrl.trimmed().toLower();
        schedulerRequest.translationModel =
            (baseLower.contains(QStringLiteral("dashscope")) ||
             baseLower.contains(QStringLiteral("aliyuncs.com")) ||
             baseLower.contains(QStringLiteral("qwen")))
            ? QStringLiteral("qwen-plus")
            : QStringLiteral("gpt-4o-mini");
    }
    const QString translationBaseLower = schedulerRequest.translationBaseUrl.trimmed().toLower();
    const bool mimoTextCredentialAvailable =
        !firstNonEmptySecret({ kMimoCredentialId }).trimmed().isEmpty() ||
        !firstNonEmptyEnv({ "MIMO_API_KEY", "XIAOMI_MIMO_API_KEY" }).trimmed().isEmpty();
    if (translationBaseLower.contains(QStringLiteral("dashscope")) ||
        translationBaseLower.contains(QStringLiteral("aliyuncs.com")) ||
        translationBaseLower.contains(QStringLiteral("qwen"))) {
        schedulerRequest.translationProviderKind = QStringLiteral("qwen-openai-compatible-text-provider");
    } else if (translationBaseLower.contains(QStringLiteral("xiaomimimo")) ||
        translationBaseLower.contains(QStringLiteral("mimo"))) {
        schedulerRequest.translationProviderKind = QStringLiteral("mimo-direct-text-provider");
    } else if (!schedulerRequest.translationApiKey.trimmed().isEmpty() &&
               !schedulerRequest.translationBaseUrl.trimmed().isEmpty()) {
        schedulerRequest.translationProviderKind = QStringLiteral("openai-compatible-text-provider");
    } else if (mimoTextCredentialAvailable) {
        schedulerRequest.translationProviderKind = QStringLiteral("mimo-credential-present-text-base-missing");
    } else if (schedulerRequest.workbenchProviderAvailable) {
        schedulerRequest.translationProviderKind = QStringLiteral("workbench-text-provider");
    } else {
        schedulerRequest.translationProviderKind = QStringLiteral("translation-provider-unavailable");
    }
    auto* providerManager = ServiceLocator::getService<IAIProviderManager>();
    const QVector<AIProviderInfo> providerInfos =
        providerManager ? providerManager->providers() : QVector<AIProviderInfo>{};
    for (const AIProviderInfo& provider : providerInfos) {
        if (!provider.available) {
            continue;
        }
        schedulerRequest.workbenchProviderSupportsText =
            schedulerRequest.workbenchProviderSupportsText || provider.capabilities.supportsText;
        schedulerRequest.workbenchProviderSupportsImages =
            schedulerRequest.workbenchProviderSupportsImages || provider.capabilities.supportsImages;
        schedulerRequest.workbenchProviderAvailable =
            schedulerRequest.workbenchProviderAvailable ||
            provider.capabilities.supportsText ||
            provider.capabilities.supportsImages;
        schedulerRequest.translationProviderAvailable =
            schedulerRequest.translationProviderAvailable || provider.capabilities.supportsText;
        schedulerRequest.asrProviderAvailable =
            schedulerRequest.asrProviderAvailable || provider.capabilities.supportsAudioTranscription;
    }
    schedulerRequest.translationProviderAvailable =
        schedulerRequest.translationProviderAvailable ||
        (!schedulerRequest.translationApiKey.trimmed().isEmpty() &&
         !schedulerRequest.translationBaseUrl.trimmed().isEmpty());
    if (schedulerRequest.translationProviderKind == QStringLiteral("translation-provider-unavailable") &&
        schedulerRequest.workbenchProviderAvailable) {
        schedulerRequest.translationProviderKind = QStringLiteral("workbench-text-provider");
    }
    schedulerRequest.asrProviderAvailable =
        schedulerRequest.asrProviderAvailable ||
        (_p->userSettings &&
         !_p->userSettings->value(QStringLiteral("ai/subtitles/asr/baseUrl")).toString().trimmed().isEmpty()) ||
        !QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_ASR_BASE_URL")).trimmed().isEmpty();
    const bool workbenchApiConfigured =
        !schedulerRequest.translationApiKey.trimmed().isEmpty() &&
        !schedulerRequest.translationBaseUrl.trimmed().isEmpty() &&
        !schedulerRequest.translationModel.trimmed().isEmpty();
    const bool configuredWorkbenchTextAvailable = workbenchApiConfigured;
    const bool configuredWorkbenchVisionAvailable = workbenchApiConfigured;
    const bool workbenchOnlineSearchRequested =
        highQualityManualRequest ||
        settingOrEnvBool(_p->userSettings,
                         QStringLiteral("ai/subtitles/online/workbenchSearchEnabled"),
                         QStringLiteral("SUBTITLE_WORKBENCH_ONLINE_SEARCH_ENABLED"));
    schedulerRequest.workbenchCanSearchOnlineSubtitles =
        workbenchOnlineSearchRequested &&
        configuredWorkbenchTextAvailable;
    schedulerRequest.workbenchCanReadImageText =
        highQualityManualRequest &&
        configuredWorkbenchVisionAvailable;
    schedulerRequest.workbenchCanTranslate =
        schedulerRequest.workbenchProviderAvailable || schedulerRequest.translationProviderAvailable;
    schedulerRequest.workbenchNoOnlineSearchCapability =
        schedulerRequest.workbenchProviderAvailable &&
        !schedulerRequest.workbenchCanSearchOnlineSubtitles;
    if (schedulerRequest.workbenchCanSearchOnlineSubtitles) {
        schedulerRequest.onlineConfigured = true;
        schedulerRequest.onlineProvider = QStringLiteral("workbench-api");
        schedulerRequest.onlineBaseUrl = schedulerRequest.translationBaseUrl;
        schedulerRequest.onlineApiKey = schedulerRequest.translationApiKey;
        schedulerRequest.workbenchNoOnlineSearchCapability = false;
    }
    if (schedulerRequest.onlineLanguage.trimmed().isEmpty()) {
        schedulerRequest.onlineLanguage = QStringLiteral("zh,ja,en");
    }
    return schedulerRequest;
}

void MainWindow::_scheduleHighQualityVisualContinuation()
{
    if (_p->translationPlaybackMode != TranslationPlaybackStrategy::highQualityModeId() ||
        !_p->playbackBar ||
        !_p->playbackBar->translationVisible() ||
        !_p->playbackCtrl ||
        _p->currentPath.trimmed().isEmpty() ||
        _p->translationPrePlaybackWaitActive ||
        _p->highQualityEnhancementBusy) {
        return;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - _p->highQualityVisualPrefetchLastCheckMs <
        kHighQualityVisualPrefetchCheckIntervalMs) {
        return;
    }
    _p->highQualityVisualPrefetchLastCheckMs = nowMs;

    const TranslationEnhancementScheduleRequest request =
        _buildTranslationEnhancementRequest(true, false, true);
    const QJsonObject scan = readGeneratedSubtitleProgress(
        request.ocrSubtitleCachePath + QStringLiteral(".candidate_scan.json"));
    if (scan.isEmpty() ||
        scan.value(QStringLiteral("mediaFingerprint")).toString() !=
            SubtitleGenerationService::mediaFingerprint(_p->currentPath)) {
        return;
    }
    const double scanStartSeconds = scan.value(QStringLiteral("scanStartSeconds")).toDouble(-1.0);
    const double scanEndSeconds = scan.value(QStringLiteral("scanEndSeconds")).toDouble(-1.0);
    if (scanStartSeconds < 0.0 || scanEndSeconds <= scanStartSeconds) {
        return;
    }
    const double fps = _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
    const double mediaDurationSeconds = _p->playbackCtrl->totalFrames() > 0
        ? _p->playbackCtrl->totalFrames() / fps
        : 0.0;
    if (mediaDurationSeconds > 0.0 && scanEndSeconds >= mediaDurationSeconds - 1.0) {
        return;
    }
    const double nextStartSeconds = std::max(
        0.0,
        scanEndSeconds - kSubtitleContinuationOverlapSeconds);
    _startCurrentMediaHighQualityEnhancement(
        QStringLiteral("continuous-visual-prefetch"),
        nextStartSeconds);
}

void MainWindow::_startCurrentMediaHighQualityEnhancement(
    const QString& reason,
    double preferredStartSeconds)
{
    if (_p->translationPlaybackMode != TranslationPlaybackStrategy::highQualityModeId() ||
        _p->currentPath.trimmed().isEmpty() ||
        (_p->playbackCtrl &&
         _p->playbackCtrl->playbackState() != 0 &&
         _p->translationPrePlaybackWaitDegradedToQuick) ||
        _p->highQualityEnhancementBusy) {
        return;
    }
    if (_p->translationPrePlaybackWaitActive &&
        _p->translationPrePlaybackWaitEnhancementStarted) {
        return;
    }
    const QString mediaPath = QFileInfo(_p->currentPath).absoluteFilePath();
    const quint64 mediaGenerationId = _p->mediaGenerationId;
    const QString mediaFingerprint = SubtitleGenerationService::mediaFingerprint(_p->currentPath);
    const auto applyPreferredWindow = [this, preferredStartSeconds, reason](
        TranslationEnhancementScheduleRequest request) {
        const bool playbackBackgroundRequest =
            !_p->translationPrePlaybackWaitActive &&
            _p->playbackCtrl &&
            _p->playbackCtrl->playbackState() != 0;
        const bool continuousBackgroundRequest =
            reason == QStringLiteral("continuous-visual-prefetch") ||
            playbackBackgroundRequest;
        double effectiveStartSeconds = preferredStartSeconds;
        if (effectiveStartSeconds < 0.0 && continuousBackgroundRequest) {
            const QJsonObject scan = readGeneratedSubtitleProgress(
                request.ocrSubtitleCachePath + QStringLiteral(".candidate_scan.json"));
            const double scanStartSeconds =
                scan.value(QStringLiteral("scanStartSeconds")).toDouble(-1.0);
            const double scanEndSeconds =
                scan.value(QStringLiteral("scanEndSeconds")).toDouble(-1.0);
            if (scanStartSeconds >= 0.0 && scanEndSeconds > scanStartSeconds) {
                effectiveStartSeconds = std::max(
                    0.0,
                    scanEndSeconds - kSubtitleContinuationOverlapSeconds);
            }
        }
        const double targetWindowSeconds =
            continuousBackgroundRequest
            ? kBackgroundSubtitleWindowSeconds
            : kHighQualityPrePlaybackTargetSeconds;
        if (effectiveStartSeconds < 0.0) {
            request.highQualityTargetCoverageSeconds = std::min(
                request.highQualityTargetCoverageSeconds,
                targetWindowSeconds);
            return request;
        }
        request.currentSeconds = std::max(0.0, effectiveStartSeconds);
        if (_p->playbackCtrl &&
            _p->playbackCtrl->fps() > 0.0 &&
            _p->playbackCtrl->totalFrames() > 0) {
            const double totalSeconds =
                _p->playbackCtrl->totalFrames() / _p->playbackCtrl->fps();
            request.highQualityTargetCoverageSeconds = std::max(
                1.0,
                std::min(targetWindowSeconds,
                         totalSeconds - request.currentSeconds));
        }
        return request;
    };
    TranslationEnhancementScheduler diagnosticScheduler;
    const TranslationEnhancementScheduleRequest diagnosticRequest = applyPreferredWindow(
        _buildTranslationEnhancementRequest(true, false, true));
    const QJsonObject diagnostics =
        diagnosticScheduler.evaluate(diagnosticRequest);
    const QJsonObject currentMedia =
        diagnostics.value(QStringLiteral("currentMediaHighQuality")).toObject();
    const QString needReason =
        currentMedia.value(QStringLiteral("needReason")).toString(QStringLiteral("enhanced-missing"));
    const bool continuousBackgroundRequest =
        reason == QStringLiteral("continuous-visual-prefetch") ||
        (!_p->translationPrePlaybackWaitActive &&
         _p->playbackCtrl &&
         _p->playbackCtrl->playbackState() != 0);
    const QJsonObject visualScan = readGeneratedSubtitleProgress(
        diagnosticRequest.ocrSubtitleCachePath + QStringLiteral(".candidate_scan.json"));
    const double visualScanEndSeconds =
        visualScan.value(QStringLiteral("scanEndSeconds")).toDouble(-1.0);
    const double visualTargetEndSeconds =
        diagnosticRequest.currentSeconds +
        diagnosticRequest.highQualityTargetCoverageSeconds;
    const bool visualScanCoversTarget =
        visualScanEndSeconds >= 0.0 &&
        visualScanEndSeconds + 0.5 >= visualTargetEndSeconds;
    _p->highQualityEnhancementLastDiagnostics = diagnostics;
    _p->highQualityEnhancementMediaPath = mediaPath;
    _p->highQualityEnhancementLastReason = needReason;
    if (currentMedia.value(QStringLiteral("enhancedReadyForCurrentMedia")).toBool() &&
        (!continuousBackgroundRequest || visualScanCoversTarget)) {
        _loadGeneratedSubtitleTrack();
        return;
    }
    if (needReason == QStringLiteral("no-readable-source")) {
        _publishTranslationPlaybackStrategyDiagnostics(true);
        return;
    }

    TranslationEnhancementScheduleRequest request = applyPreferredWindow(
        _buildTranslationEnhancementRequest(true, true, true));
    request.cancelRequested = std::make_shared<std::atomic_bool>(false);
    _p->highQualityEnhancementCancelRequested = request.cancelRequested;
    _p->highQualityEnhancementBusy = true;
    if (_p->translationPrePlaybackWaitActive) {
        _p->translationPrePlaybackWaitEnhancementStarted = true;
    }
    _p->highQualityEnhancementLastReason = reason.trimmed().isEmpty() ? needReason : reason;
    if (statusBar()) {
        statusBar()->showMessage(QString::fromUtf8(u8"高质量增强正在后台处理当前媒体；播放继续，失败会回退 quick。"), 4000);
    }
    auto future = QtConcurrent::run([request]() {
        TranslationEnhancementScheduler scheduler;
        return scheduler.evaluate(request);
    });
    auto* watcher = new QFutureWatcher<QJsonObject>(this);
    _p->highQualityEnhancementWatcher = watcher;
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this,
                                                                    watcherPtr = QPointer<QFutureWatcher<QJsonObject>>(watcher),
                                                                    mediaPath,
                                                                    mediaFingerprint,
                                                                    mediaGenerationId]() {
        if (!watcherPtr) {
            return;
        }
        const QJsonObject result = watcherPtr->result();
        watcherPtr->deleteLater();
        const bool watcherIsCurrent =
            _p->highQualityEnhancementWatcher.data() == watcherPtr.data();
        if (watcherIsCurrent) {
            _p->highQualityEnhancementBusy = false;
            _p->highQualityEnhancementWatcher.clear();
            _p->highQualityEnhancementCancelRequested.reset();
        }
        const QString currentMediaPath = QFileInfo(_p->currentPath).absoluteFilePath();
        const QString currentMediaFingerprint =
            _p->currentPath.trimmed().isEmpty()
                ? QString()
                : SubtitleGenerationService::mediaFingerprint(_p->currentPath);
        const bool staleMediaResult =
            mediaGenerationId != _p->mediaGenerationId ||
            currentMediaPath != mediaPath ||
            currentMediaFingerprint != mediaFingerprint;
        if (staleMediaResult) {
            qWarning() << "[TranslationEnhancement] Discarding stale high-quality result"
                       << "requestedPath=" << mediaPath
                       << "currentPath=" << currentMediaPath
                       << "requestedGeneration=" << mediaGenerationId
                       << "currentGeneration=" << _p->mediaGenerationId
                       << "requestedFingerprint=" << mediaFingerprint
                       << "currentFingerprint=" << currentMediaFingerprint;
            if (qApp) {
                QJsonObject staleReport{
                    { QStringLiteral("staleMediaGenerationDiscarded"), true },
                    { QStringLiteral("requestedMediaPath"), mediaPath },
                    { QStringLiteral("currentMediaPath"), currentMediaPath },
                    { QStringLiteral("requestedMediaFingerprint"), mediaFingerprint },
                    { QStringLiteral("currentMediaFingerprint"), currentMediaFingerprint }
                };
                qApp->setProperty(
                    "cgplay.highQualityEnhancementLastStaleResult",
                    QString::fromUtf8(QJsonDocument(staleReport).toJson(QJsonDocument::Compact)));
            }
            return;
        }
        _p->highQualityEnhancementLastDiagnostics = result;
        _p->highQualityEnhancementMediaPath = mediaPath;
        if (qApp) {
            qApp->setProperty(
                "cgplay.currentMediaHighQualityEnhancement",
                QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        }
        if (QFileInfo(_p->currentPath).absoluteFilePath() == mediaPath) {
            _loadGeneratedSubtitleTrack();
            _publishTranslationPlaybackStrategyDiagnostics(true);
            const QString fallback =
                result.value(QStringLiteral("fallbackReason")).toString(
                    result.value(QStringLiteral("currentMediaHighQuality")).toObject()
                        .value(QStringLiteral("fallbackReason")).toString());
            if (statusBar()) {
                statusBar()->showMessage(
                    result.value(QStringLiteral("enhancementSucceeded")).toBool()
                        ? QString::fromUtf8(u8"高质量增强已加载；缺失 cue 继续回退 quick。")
                        : QString::fromUtf8(u8"高质量增强未完成，已回退 quick：%1").arg(fallback),
                    5000);
            }
            TranslationEnhancementScheduler diagnosticScheduler;
            const QJsonObject diagnostics =
                diagnosticScheduler.evaluate(_buildTranslationEnhancementRequest(true, false, true));
            const QJsonObject currentMedia =
                diagnostics.value(QStringLiteral("currentMediaHighQuality")).toObject();
            const double quickCoverageEnd =
                currentMedia.value(QStringLiteral("quick")).toObject()
                    .value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble();
            const double enhancedCoverageEnd =
                currentMedia.value(QStringLiteral("enhanced")).toObject()
                    .value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble();
            const QString refreshProperty = QStringLiteral("cgplay.hqRefreshAfterQuickGrowthMedia");
            const bool visualWorkerAttempted =
                result.value(QStringLiteral("manualOcrWorker")).toObject()
                    .value(QStringLiteral("visionApiAttempted")).toBool(false) ||
                result.value(QStringLiteral("currentMediaHighQuality")).toObject()
                    .value(QStringLiteral("visualTrackGenerated")).toBool(false);
            const bool refreshAlreadyTried =
                qApp && qApp->property(refreshProperty.toUtf8().constData()).toString() == mediaPath;
            if (quickCoverageEnd > enhancedCoverageEnd + 0.5 &&
                !visualWorkerAttempted &&
                !refreshAlreadyTried &&
                qApp) {
                qApp->setProperty(refreshProperty.toUtf8().constData(), mediaPath);
                QTimer::singleShot(0, this, [this]() {
                    _startCurrentMediaHighQualityEnhancement(QStringLiteral("high-quality-refresh-after-quick-baseline-growth"));
                });
            }
            _scheduleHighQualityVisualContinuation();
        }
    });
    watcher->setFuture(future);
    _publishTranslationPlaybackStrategyDiagnostics(true);
}

QJsonObject MainWindow::_translationPlaybackStrategyDiagnostics(bool explicitlyRequested) const
{
    TranslationPlaybackStrategy strategy;
    TranslationEnhancementScheduler enhancementScheduler;
    const QString modeId = TranslationPlaybackStrategy::normalizeModeId(_p->translationPlaybackMode);
    const QString enhancedRejectedReason =
        (!_p->generatedSubtitleEnhancedLoaded &&
         modeId != TranslationPlaybackStrategy::highQualityModeId() &&
         _p->generatedSubtitleEnhancedRejectedReason.trimmed().isEmpty())
        ? QStringLiteral("disabled-in-quick-playback")
        : _p->generatedSubtitleEnhancedRejectedReason;
    const double currentSeconds = generatedSubtitleCurrentSeconds(_p->playbackCtrl.get());
    const double quickCoverageEndSeconds =
        generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
    const bool hasEnhancement =
        _p->generatedSubtitleRefinedMerged || _p->generatedSubtitleEnhancedMerged;
    const double enhancedCoverageEndSeconds = hasEnhancement ? quickCoverageEndSeconds : 0.0;
    const bool playbackAlreadyRunning =
        _p->playbackCtrl && _p->playbackCtrl->playbackState() != 0;
    QJsonObject decision = strategy.decideForMode(
        modeId,
        currentSeconds,
        quickCoverageEndSeconds,
        enhancedCoverageEndSeconds,
        playbackAlreadyRunning,
        explicitlyRequested);
    const QJsonObject enhancementSchedulerDiagnostics =
        enhancementScheduler.evaluate(_buildTranslationEnhancementRequest(explicitlyRequested, false, true));
    const bool lastHqResultMatchesCurrent =
        !_p->highQualityEnhancementLastDiagnostics.isEmpty() &&
        QFileInfo(_p->highQualityEnhancementMediaPath).absoluteFilePath() ==
            QFileInfo(_p->currentPath).absoluteFilePath();
    const QJsonObject evaluatedCurrentMediaHighQuality =
        enhancementSchedulerDiagnostics.value(QStringLiteral("currentMediaHighQuality")).toObject();
    const QJsonObject lastCurrentMediaHighQuality =
        lastHqResultMatchesCurrent
        ? _p->highQualityEnhancementLastDiagnostics.value(QStringLiteral("currentMediaHighQuality")).toObject()
        : QJsonObject{};
    const QJsonObject currentMediaHighQuality =
        !evaluatedCurrentMediaHighQuality.isEmpty()
        ? evaluatedCurrentMediaHighQuality
        : lastCurrentMediaHighQuality;
    const QString lastActiveSource =
        lastHqResultMatchesCurrent
        ? _p->highQualityEnhancementLastDiagnostics.value(QStringLiteral("activeSource")).toString()
        : QString();
    const QString displayActiveSource =
        _p->generatedSubtitleEnhancedLoaded && !lastActiveSource.trimmed().isEmpty()
        ? lastActiveSource
        : enhancementSchedulerDiagnostics.value(QStringLiteral("activeSource")).toString();
    const QString displayFallbackReason =
        _p->generatedSubtitleEnhancedLoaded
        ? _p->generatedSubtitleEnhancedRejectedReason
        : (lastHqResultMatchesCurrent
              ? _p->highQualityEnhancementLastDiagnostics.value(QStringLiteral("fallbackReason")).toString(
                    currentMediaHighQuality.value(QStringLiteral("fallbackReason")).toString())
              : enhancementSchedulerDiagnostics.value(QStringLiteral("fallbackReason")).toString());
    return QJsonObject{
        { QStringLiteral("mode"), modeId },
        { QStringLiteral("modeLabel"), TranslationPlaybackStrategy::modeLabel(modeId) },
        { QStringLiteral("policy"), strategy.policyForMode(modeId) },
        { QStringLiteral("decision"), decision },
        { QStringLiteral("enhancementScheduler"), enhancementSchedulerDiagnostics },
        { QStringLiteral("currentMediaHighQuality"), currentMediaHighQuality },
        { QStringLiteral("currentMediaHighQualityEnhancementBusy"), _p->highQualityEnhancementBusy },
        { QStringLiteral("currentMediaHighQualityLastResult"),
          lastHqResultMatchesCurrent ? _p->highQualityEnhancementLastDiagnostics : QJsonObject{} },
        { QStringLiteral("activeMode"), enhancementSchedulerDiagnostics.value(QStringLiteral("activeMode")).toString(modeId) },
        { QStringLiteral("activeSource"), displayActiveSource },
        { QStringLiteral("enhancementAttempted"),
          lastHqResultMatchesCurrent
              ? _p->highQualityEnhancementLastDiagnostics.value(QStringLiteral("enhancementAttempted")).toBool()
              : enhancementSchedulerDiagnostics.value(QStringLiteral("enhancementAttempted")).toBool() },
        { QStringLiteral("enhancementSucceeded"),
          _p->generatedSubtitleEnhancedLoaded ||
              (lastHqResultMatchesCurrent &&
               _p->highQualityEnhancementLastDiagnostics.value(QStringLiteral("enhancementSucceeded")).toBool()) },
        { QStringLiteral("fallbackReason"), displayFallbackReason },
        { QStringLiteral("changedCueCount"),
          lastHqResultMatchesCurrent
              ? _p->highQualityEnhancementLastDiagnostics.value(QStringLiteral("changedCueCount")).toInt()
              : enhancementSchedulerDiagnostics.value(QStringLiteral("changedCueCount")).toInt() },
        { QStringLiteral("providerConfigState"), enhancementSchedulerDiagnostics.value(QStringLiteral("providerConfigState")).toObject() },
        { QStringLiteral("waited"), _p->translationPlaybackModeWaited },
        { QStringLiteral("waitMs"), static_cast<double>(_p->translationPlaybackModeWaitMs) },
        { QStringLiteral("prePlaybackWaitActive"), _p->translationPrePlaybackWaitActive },
        { QStringLiteral("prePlaybackWaitResult"), _p->translationPrePlaybackWaitResult },
        { QStringLiteral("prePlaybackWaitCanceled"), _p->translationPrePlaybackWaitCanceled },
        { QStringLiteral("prePlaybackWaitTimedOut"), _p->translationPrePlaybackWaitTimedOut },
        { QStringLiteral("prePlaybackWaitDegradedToQuick"), _p->translationPrePlaybackWaitDegradedToQuick },
        { QStringLiteral("prePlaybackWaitCancelable"), true },
        { QStringLiteral("prePlaybackWaitTargetSeconds"), _p->translationPrePlaybackWaitTargetSeconds },
        { QStringLiteral("prePlaybackWaitCoverageAheadSeconds"),
          _p->translationPrePlaybackWaitCoverageAheadSeconds },
        { QStringLiteral("prePlaybackWaitTimeoutMs"),
          static_cast<double>(_p->translationPrePlaybackWaitTimeoutMs) },
        { QStringLiteral("prePlaybackWaitPlaybackAlreadyRunning"),
          _p->translationPrePlaybackWaitPlaybackAlreadyRunning },
        { QStringLiteral("hqCoverageSeconds"),
          currentMediaHighQuality.value(QStringLiteral("hqCoverageSeconds")).toDouble() },
        { QStringLiteral("targetCoverageSeconds"),
          _p->translationPrePlaybackWaitTargetSeconds },
        { QStringLiteral("activeHqSource"),
          currentMediaHighQuality.value(QStringLiteral("activeHqSource")).toString(displayActiveSource) },
        { QStringLiteral("hqTranslatedCueCount"),
          currentMediaHighQuality.value(QStringLiteral("hqTranslatedCueCount")).toInt() },
        { QStringLiteral("lowConfidenceCueCount"),
          currentMediaHighQuality.value(QStringLiteral("lowConfidenceCueCount")).toInt() },
        { QStringLiteral("repairAttemptCount"),
          currentMediaHighQuality.value(QStringLiteral("repairAttemptCount")).toInt() },
        { QStringLiteral("qualityScore"),
          currentMediaHighQuality.value(QStringLiteral("qualityScore")).toVariant().toString() },
        { QStringLiteral("quickFallbackCueCount"), _p->generatedSubtitleQuickFallbackCueCount },
        { QStringLiteral("refinedMatchedCueCount"), _p->generatedSubtitleRefinedMatchedCueCount },
        { QStringLiteral("enhancedMatchedCueCount"), _p->generatedSubtitleEnhancedMatchedCueCount },
        { QStringLiteral("enhancedLoaded"), _p->generatedSubtitleEnhancedLoaded },
        { QStringLiteral("enhancedCueMatches"), _p->generatedSubtitleEnhancedMatchedCueCount },
        { QStringLiteral("enhancedRejectedReason"), enhancedRejectedReason },
        { QStringLiteral("refinedMerged"), _p->generatedSubtitleRefinedMerged },
        { QStringLiteral("enhancedMerged"), _p->generatedSubtitleEnhancedMerged },
        { QStringLiteral("refinedTookOverTrack"), false },
        { QStringLiteral("enhancedTookOverTrack"), false },
        { QStringLiteral("wholeTrackReplacement"), false },
        { QStringLiteral("currentCueFallbackSafe"), _p->generatedSubtitleCurrentCueFallbackSafe },
        { QStringLiteral("quickBaselineIsSourceOfTruth"), true },
        { QStringLiteral("playbackPausedForMode"), false },
        { QStringLiteral("quickPathBehaviorChanged"), false }
    };
}

QJsonObject MainWindow::_translationGuardReportDiagnostics(
    const QJsonObject& serviceDiagnostics,
    const QJsonObject& playbackSampling) const
{
    QJsonObject resolvedServiceDiagnostics = serviceDiagnostics;
    if (resolvedServiceDiagnostics.isEmpty() && qApp) {
        const QJsonDocument doc = QJsonDocument::fromJson(
            qApp->property("cgplay.subtitleGenerationLastResult").toString().toUtf8());
        if (doc.isObject()) {
            resolvedServiceDiagnostics = doc.object();
        }
    }

    int translatedCueCount = 0;
    int sourceOnlyCueCount = 0;
    bool hasExactTranslatedCue = false;
    const double currentSeconds = generatedSubtitleCurrentSeconds(_p->playbackCtrl.get());
    for (const auto& cue : _p->generatedSubtitleCues) {
        const bool hasSource = !cue.sourceText.trimmed().isEmpty();
        const bool hasTranslation = generatedSubtitleCueHasDisplayableTranslation(cue);
        if (hasTranslation) {
            translatedCueCount += 1;
        } else if (hasSource) {
            sourceOnlyCueCount += 1;
        }
        if (hasTranslation &&
            currentSeconds + 0.02 >= cue.startSeconds &&
            currentSeconds <= cue.endSeconds + 0.02) {
            hasExactTranslatedCue = true;
        }
    }

    auto* overlayLabel = findChild<QLabel*>(QStringLiteral("GeneratedSubtitleOverlayLabel"));
    TranslationGuardSnapshot snapshot;
    snapshot.mediaPath = _p->currentPath;
    snapshot.generatedSubtitlePath = _p->generatedSubtitlePath;
    snapshot.translatedVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultTranslatedVttPath(_p->currentPath, QStringLiteral("zh-Hans"));
    snapshot.refinedVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultRefinedTranslatedVttPath(_p->currentPath, QStringLiteral("zh-Hans"));
    snapshot.enhancedVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultEnhancedTranslatedVttPath(_p->currentPath, QStringLiteral("zh-Hans"));
    snapshot.repairVttPath = _p->currentPath.trimmed().isEmpty()
        ? QString()
        : SubtitleGenerationService::defaultLowConfidenceRepairVttPath(_p->currentPath, QStringLiteral("zh-Hans"));
    snapshot.playbackMode = _p->translationPlaybackMode;
    snapshot.currentSeconds = currentSeconds;
    snapshot.sourceCoverageEndSeconds = generatedSubtitleMaxEndSeconds(_p->generatedSubtitleCues);
    snapshot.translatedCoverageEndSeconds = generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
    snapshot.displayCoverageEndSeconds = _p->generatedSubtitleCoverageEndSeconds;
    snapshot.processedEndSeconds = _p->generatedSubtitleProcessedEndSeconds;
    snapshot.sourceCueCount = _p->generatedSubtitleCues.size();
    snapshot.translatedCueCount = translatedCueCount;
    snapshot.sourceOnlyCueCount = sourceOnlyCueCount;
    snapshot.quickFallbackCueCount = _p->generatedSubtitleQuickFallbackCueCount;
    snapshot.refinedMatchedCueCount = _p->generatedSubtitleRefinedMatchedCueCount;
    snapshot.enhancedMatchedCueCount = _p->generatedSubtitleEnhancedMatchedCueCount;
    snapshot.enhancedLoaded = _p->generatedSubtitleEnhancedLoaded;
    snapshot.enhancedRejectedReason =
        (!_p->generatedSubtitleEnhancedLoaded &&
         _p->translationPlaybackMode != TranslationPlaybackStrategy::highQualityModeId() &&
         _p->generatedSubtitleEnhancedRejectedReason.trimmed().isEmpty())
        ? QStringLiteral("disabled-in-quick-playback")
        : _p->generatedSubtitleEnhancedRejectedReason;
    snapshot.currentCueFallbackSafe = _p->generatedSubtitleCurrentCueFallbackSafe;
    snapshot.generatedSubtitleBusy = _p->subtitleGenerationBusy;
    snapshot.continuationScheduled = _p->subtitleContinuationScheduled;
    snapshot.continuationQueuedWhileBusy = _p->subtitleContinuationQueuedWhileBusy;
    snapshot.hasExactTranslatedCue = hasExactTranslatedCue;
    snapshot.overlayVisible = overlayLabel && overlayLabel->isVisible();
    snapshot.overlayText = overlayLabel ? overlayLabel->text().trimmed() : QString();
    snapshot.refinedMerged = _p->generatedSubtitleRefinedMerged;
    snapshot.enhancedMerged = _p->generatedSubtitleEnhancedMerged;
    snapshot.refinedWholeTrackTakeover = false;
    snapshot.enhancedWholeTrackTakeover = false;
    snapshot.serviceDiagnostics = resolvedServiceDiagnostics;
    snapshot.playbackStrategyDiagnostics =
        _translationPlaybackStrategyDiagnostics(
            _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
    snapshot.playbackSampling = playbackSampling;

    TranslationCoverageAuditSkill coverageAuditSkill;
    TranslationCacheGuardSkill cacheGuardSkill;
    TranslationNoDialogueGuardSkill noDialogueGuardSkill;
    TranslationSourcePriorityGuardSkill sourcePriorityGuardSkill;
    TranslationReportSkill reportSkill;
    const QJsonObject coverageAudit = coverageAuditSkill.evaluate(snapshot);
    const QJsonObject cacheGuard = cacheGuardSkill.evaluate(snapshot);
    const QJsonObject noDialogueGuard = noDialogueGuardSkill.evaluate(snapshot);
    const QJsonObject sourcePriorityAudit = sourcePriorityGuardSkill.evaluate(snapshot);
    QJsonObject report = reportSkill.aggregate(
        snapshot,
        coverageAudit,
        cacheGuard,
        noDialogueGuard,
        sourcePriorityAudit);
    report.insert(QStringLiteral("consumers"), QJsonObject{
        { QStringLiteral("CoverageAuditSkill"), coverageAudit },
        { QStringLiteral("CacheGuardSkill"), cacheGuard },
        { QStringLiteral("NoDialogueGuardSkill"), noDialogueGuard },
        { QStringLiteral("SourcePriorityGuardSkill"), sourcePriorityAudit },
        { QStringLiteral("ReportSkill"), QJsonObject{
              { QStringLiteral("mode"), QStringLiteral("diagnostic-guard-only") },
              { QStringLiteral("canTriggerAsrOrTranslation"), false },
              { QStringLiteral("canChangeScheduling"), false },
              { QStringLiteral("canWriteSubtitleSidecars"), false },
              { QStringLiteral("canChangeUiDisplayDecision"), false }
          } }
    });
    if (qApp) {
        qApp->setProperty(
            "cgplay.translationGuardReport",
            QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Compact)));
    }
    return report;
}

void MainWindow::_publishTranslationPlaybackStrategyDiagnostics(bool explicitlyRequested) const
{
    if (!qApp) {
        return;
    }
    const QJsonObject diagnostics = _translationPlaybackStrategyDiagnostics(explicitlyRequested);
    qApp->setProperty("cgplay.translationPlaybackMode", diagnostics.value(QStringLiteral("mode")).toString());
    qApp->setProperty(
        "cgplay.translationPlaybackStrategy",
        QString::fromUtf8(QJsonDocument(diagnostics).toJson(QJsonDocument::Compact)));
}

void MainWindow::_onPlaybackTranslationToggled(bool visible)
{
    if (_p->playbackBar) {
        _p->translationPlaybackMode =
            TranslationPlaybackStrategy::normalizeModeId(_p->playbackBar->translationMode());
    }
    _publishTranslationPlaybackStrategyDiagnostics(
        visible &&
        _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
    if (!visible) {
        _finishHighQualityPrePlaybackWait(
            QStringLiteral("toggle-off-fallback-quick"),
            false,
            true,
            true);
        _p->subtitleContinuationScheduled = false;
        _p->subtitleContinuationStartSeconds = -1.0;
        _p->subtitleContinuationQueuedWhileBusy = false;
        _p->subtitleContinuationQueuedStartSeconds = -1.0;
        _setTranslationStripVisible(false, false);
        if (_p->generatedSubtitleLabel) {
            _p->generatedSubtitleLabel->hide();
        }
        _p->generatedSubtitleLastText.clear();
        _p->generatedSubtitleLastCueStartSeconds = -1.0;
        _p->generatedSubtitleLastCueEndSeconds = -1.0;
        _p->generatedSubtitleLastShownAtSeconds = -1.0;
        if (_p->generatedSubtitleRefreshTimer) {
            _p->generatedSubtitleRefreshTimer->stop();
        }
        return;
    }

    if (_p->currentPath.trimmed().isEmpty()) {
        if (statusBar()) {
            statusBar()->showMessage(QStringLiteral("请先打开一个视频文件。"), 3000);
        }
        if (_p->playbackBar) {
            _p->playbackBar->setTranslationVisible(false);
        }
        _setTranslationStripVisible(false, false);
        return;
    }

    _setTranslationStripVisible(false, false);
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(true);
    }
    if (_p->windowSettings) {
        _p->windowSettings->setValue(QStringLiteral("ai/subtitles/playbackMode"), _p->translationPlaybackMode);
    }
    if (_p->generatedSubtitleCues.isEmpty()) {
        _loadGeneratedSubtitleTrack();
    }
    if (!_p->generatedSubtitleCues.isEmpty()) {
        if (_p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId()) {
            _startCurrentMediaHighQualityEnhancement(QStringLiteral("toggle-visible-current-media"));
        }
        const double fps = _p->playbackCtrl && _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
        const double currentSeconds = _p->playbackCtrl
            ? std::max(0, _p->playbackCtrl->currentFrame()) / fps
            : 0.0;
        QString unusableReason;
        if (!generatedSubtitleTrackUsable(
                _p->generatedSubtitleCues,
                currentSeconds,
                _p->playbackCtrl && _p->playbackCtrl->totalFrames() > 0
                    ? _p->playbackCtrl->totalFrames() / fps
                    : 0.0,
                false,
                &unusableReason)) {
            qInfo() << "[SubtitleGeneration] Cached subtitles do not cover current playback time; regenerating current window"
                    << "current=" << currentSeconds
                    << "reason=" << unusableReason
                    << "cueCount=" << _p->generatedSubtitleCues.size()
                    << "translatedCoverageEnd="
                    << generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues)
                    << "sourceCoverageEnd=" << generatedSubtitleMaxEndSeconds(_p->generatedSubtitleCues);
            _p->generatedSubtitleCues.clear();
            _p->generatedSubtitlePath.clear();
            _p->generatedSubtitleMediaFingerprint.clear();
            _p->generatedSubtitleCoverageEndSeconds = 0.0;
            _p->generatedSubtitleProcessedEndSeconds = 0.0;
            _p->generatedSubtitleLastText.clear();
            _p->generatedSubtitleLastCueStartSeconds = -1.0;
            _p->generatedSubtitleLastCueEndSeconds = -1.0;
            _p->generatedSubtitleLastShownAtSeconds = -1.0;
            _generateSubtitlesForCurrentMedia(currentSeconds, false);
            return;
        }
        _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
        return;
    }
    _generateSubtitlesForCurrentMedia();
}

void MainWindow::_generateSubtitlesForCurrentMedia(double startSeconds, bool background)
{
    if (_p->currentPath.trimmed().isEmpty()) {
        if (!background) {
            QMessageBox::warning(this, QStringLiteral("生成字幕"), QStringLiteral("请先打开一个视频文件。"));
        }
        return;
    }
    if (_p->subtitleGenerationBusy) {
        return;
    }
    _p->subtitleContinuationScheduled = false;
    _p->subtitleContinuationStartSeconds = -1.0;
    _p->subtitleContinuationQueuedWhileBusy = false;
    _p->subtitleContinuationQueuedStartSeconds = -1.0;

    auto* providerManager = ServiceLocator::getService<IAIProviderManager>();
    if (!providerManager) {
        if (!background) {
            QMessageBox::warning(this, QStringLiteral("生成字幕"), QStringLiteral("AI Provider 不可用。"));
        }
        return;
    }

    _p->subtitleGenerationBusy = true;
    _p->subtitleGenerationLastError.clear();
    if (!background) {
        _p->translationPlaybackModeWaited = false;
        _p->translationPlaybackModeWaitMs = 0;
        _p->translationPrePlaybackWaitResult.clear();
        _p->translationPrePlaybackWaitTimedOut = false;
        _p->translationPrePlaybackWaitCanceled = false;
        _p->translationPrePlaybackWaitDegradedToQuick = false;
        _p->translationPrePlaybackWaitPlaybackAlreadyRunning = false;
        _p->generatedSubtitleQuickFallbackCueCount = 0;
        _p->generatedSubtitleRefinedMatchedCueCount = 0;
        _p->generatedSubtitleEnhancedMatchedCueCount = 0;
        _p->generatedSubtitleRefinedMerged = false;
        _p->generatedSubtitleEnhancedMerged = false;
        if (_p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId()) {
            _startHighQualityPrePlaybackWait();
        }
    }
    _setupGeneratedSubtitleOverlay();
    if (!_p->generatedSubtitleRefreshTimer) {
        _p->generatedSubtitleRefreshTimer = new QTimer(this);
        _p->generatedSubtitleRefreshTimer->setInterval(1200);
        connect(_p->generatedSubtitleRefreshTimer, &QTimer::timeout, this, [this]() {
            if (!_p->playbackBar || !_p->playbackBar->translationVisible() || _p->currentPath.trimmed().isEmpty()) {
                return;
            }
            const QString partialPath = !_p->subtitleGenerationActiveTranslatedVttPath.trimmed().isEmpty()
                ? _p->subtitleGenerationActiveTranslatedVttPath
                : SubtitleGenerationService::defaultTranslatedVttPath(
                    _p->currentPath,
                    QStringLiteral("zh-Hans"));
            QVector<GeneratedSubtitleCue> partialCues = SubtitleGenerationService::readSubtitleFile(partialPath);
            if (partialCues.isEmpty()) {
                return;
            }
            QString partialMismatchReason;
            if (!generatedSubtitleCacheMatchesCurrentMedia(
                    partialPath,
                    _p->currentPath,
                    0.0,
                    &partialMismatchReason)) {
                qWarning() << "[SubtitleGeneration] Ignoring partial subtitle cache from non-current media"
                           << QFileInfo(partialPath).absoluteFilePath()
                           << partialMismatchReason;
                return;
            }
            mergeGeneratedSubtitleCues(&_p->generatedSubtitleCues, partialCues);
            if (_p->generatedSubtitleCues.isEmpty()) {
                return;
            }
            _p->generatedSubtitlePath = QFileInfo(partialPath).absoluteFilePath();
            _p->generatedSubtitleMediaFingerprint =
                SubtitleGenerationService::mediaFingerprint(_p->currentPath);
            _p->generatedSubtitleCoverageEndSeconds =
                generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
            _p->generatedSubtitleProcessedEndSeconds = std::max(
                _p->generatedSubtitleProcessedEndSeconds,
                _p->generatedSubtitleCoverageEndSeconds);
            const QString finalTranslatedSrtPath = SubtitleGenerationService::defaultTranslatedSrtPath(
                _p->currentPath,
                QStringLiteral("zh-Hans"));
            const QString finalTranslatedVttPath = SubtitleGenerationService::defaultTranslatedVttPath(
                _p->currentPath,
                QStringLiteral("zh-Hans"));
            writeGeneratedTranslatedSubtitleSidecars(
                finalTranslatedSrtPath,
                finalTranslatedVttPath,
                _p->generatedSubtitleCues,
                QStringLiteral("partial-refresh"));
            writeGeneratedSubtitleMediaIdentitySidecars(
                { finalTranslatedSrtPath, finalTranslatedVttPath },
                _p->currentPath,
                0.0,
                QStringLiteral("partial-refresh"));
            _updateHighQualityPrePlaybackWait(QStringLiteral("partial-refresh"));
            _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
        });
    }
    _p->generatedSubtitleRefreshTimer->start();
    if (_p->generatedSubtitleLabel && (!background || _p->generatedSubtitleCues.isEmpty())) {
        _p->generatedSubtitleLabel->hide();
        _p->generatedSubtitleLabel->clear();
    }

    QProgressDialog* progressDlg = nullptr;
    if (!background) {
        if (statusBar()) {
            statusBar()->showMessage(QStringLiteral("正在生成字幕缓存，播放会继续，字幕会边生成边显示..."), 5000);
        }
    } else if (statusBar()) {
        statusBar()->showMessage(QStringLiteral("正在后台补后续字幕..."), 2500);
    }

    SubtitleGenerationRequest request;
    request.mediaPath = _p->currentPath;
    request.targetLanguage = QStringLiteral("zh-Hans");
    request.sourceLanguageHint = QStringLiteral("ja");
    if (_p->userSettings) {
        request.sourceLanguageHint = _p->userSettings
            ->value(QStringLiteral("ai/subtitles/sourceLanguage"), request.sourceLanguageHint)
            .toString()
            .trimmed();
        if (request.sourceLanguageHint.isEmpty() ||
            request.sourceLanguageHint.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
            request.sourceLanguageHint = QStringLiteral("ja");
        }
    }
    request.useApiTranscription = true;
    request.forceRegenerate = true;
    request.mockWithoutApi = qApp && qApp->property("cgplay.subtitleGenerationMock").toBool();
    const QString finalSourceSrtPath = SubtitleGenerationService::defaultSourceSrtPath(request.mediaPath);
    const QString finalSourceVttPath = QFileInfo(finalSourceSrtPath).absolutePath() + QLatin1Char('/') +
        QFileInfo(finalSourceSrtPath).completeBaseName() + QStringLiteral(".vtt");
    const QString finalTranslatedSrtPath =
        SubtitleGenerationService::defaultTranslatedSrtPath(request.mediaPath, request.targetLanguage);
    const QString finalTranslatedVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(request.mediaPath, request.targetLanguage);
    if (background) {
        request.outputDirectory = QDir(QDir::tempPath()).filePath(
            QStringLiteral("cgplay_subtitle_bg_%1_%2")
                .arg(QCoreApplication::applicationPid())
                .arg(QDateTime::currentMSecsSinceEpoch()));
    }
    _p->subtitleGenerationActiveTranslatedVttPath = SubtitleGenerationService::defaultTranslatedVttPath(
        request.mediaPath,
        request.targetLanguage,
        request.outputDirectory);
    request.cancelRequested = std::make_shared<std::atomic_bool>(false);
    _p->subtitleGenerationCancelRequested = request.cancelRequested;
    if (_p->playbackCtrl) {
        const double fps = _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
        const double currentSeconds = std::max(0, _p->playbackCtrl->currentFrame()) / fps;
        const double prerollSeconds = background
            ? kSubtitleContinuationOverlapSeconds
            : kInitialSubtitlePrerollSeconds;
        request.startSeconds = startSeconds >= 0.0
            ? std::max(0.0, startSeconds)
            : std::max(0.0, currentSeconds - prerollSeconds);
        request.maxDurationSeconds = background
            ? kBackgroundSubtitleWindowSeconds
            : kInitialSubtitleWindowSeconds;
        if (background) {
            request.priorityStartSeconds = std::max(
                request.startSeconds,
                generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues));
        }
    }
    if (request.mockWithoutApi && _p->playbackCtrl) {
        const double fps = _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
        const double currentSeconds = std::max(0, _p->playbackCtrl->currentFrame()) / fps;
        request.mockCueStartSeconds = startSeconds >= 0.0
            ? (background ? std::max(currentSeconds, request.startSeconds) : request.startSeconds)
            : currentSeconds;
    }

    auto future = QtConcurrent::run([providerManager, settings = _p->userSettings, request]() {
        SubtitleGenerationService service(providerManager, settings.get());
        return service.generate(request);
    });
    auto* watcher = new QFutureWatcher<SubtitleGenerationResult>(this);
    _p->subtitleGenerationWatcher = watcher;
    _p->subtitleGenerationProgress = progressDlg;
    const quint64 requestedMediaGenerationId = _p->mediaGenerationId;
    const QString requestedMediaPath = QFileInfo(request.mediaPath).absoluteFilePath();
    const QString requestedMediaFingerprint =
        SubtitleGenerationService::mediaFingerprint(request.mediaPath);
    if (progressDlg) {
        connect(progressDlg, &QProgressDialog::canceled, watcher, [cancelRequested = request.cancelRequested,
                                                                   progressPtr = QPointer<QProgressDialog>(progressDlg)]() {
            if (cancelRequested) {
                cancelRequested->store(true);
            }
            if (progressPtr) {
                progressPtr->setLabelText(QStringLiteral("正在取消字幕生成，等待当前音频请求结束..."));
                progressPtr->setCancelButton(nullptr);
            }
        });
    }
    const double requestedStartSeconds = request.startSeconds;
    const double requestedDurationSeconds = request.maxDurationSeconds;
    const bool requestedMockWithoutApi = request.mockWithoutApi;
    connect(watcher, &QFutureWatcher<SubtitleGenerationResult>::finished, this, [this,
                                                                                 watcherPtr = QPointer<QFutureWatcher<SubtitleGenerationResult>>(watcher),
                                                                                   progressPtr = QPointer<QProgressDialog>(progressDlg),
                                                                                   background,
                                                                                   requestedMediaGenerationId,
                                                                                   requestedMediaPath,
                                                                                   requestedMediaFingerprint,
                                                                                   requestedStartSeconds,
                                                                                   requestedDurationSeconds,
                                                                                   requestedMockWithoutApi,
                                                                                   finalSourceSrtPath,
                                                                                   finalSourceVttPath,
                                                                                   finalTranslatedSrtPath,
                                                                                   finalTranslatedVttPath]() {
        if (!watcherPtr) {
            return;
        }
        const SubtitleGenerationResult result = watcherPtr->result();
        const QString currentMediaPath = QFileInfo(_p->currentPath).absoluteFilePath();
        const QString currentMediaFingerprint =
            _p->currentPath.trimmed().isEmpty()
                ? QString()
                : SubtitleGenerationService::mediaFingerprint(_p->currentPath);
        const bool staleMediaResult =
            requestedMediaGenerationId != _p->mediaGenerationId ||
            requestedMediaPath != currentMediaPath ||
            requestedMediaFingerprint != currentMediaFingerprint;
        const bool watcherIsCurrent =
            _p->subtitleGenerationWatcher.data() == watcherPtr.data();
        if (watcherIsCurrent) {
            _p->subtitleGenerationBusy = false;
            _p->subtitleGenerationCancelRequested.reset();
            _p->subtitleGenerationWatcher.clear();
            _p->subtitleGenerationProgress.clear();
            if (_p->generatedSubtitleRefreshTimer) {
                _p->generatedSubtitleRefreshTimer->stop();
            }
            _p->subtitleGenerationActiveTranslatedVttPath.clear();
        }
        watcherPtr->deleteLater();
        if (progressPtr) {
            progressPtr->close();
            progressPtr->deleteLater();
        }
        if (staleMediaResult) {
            qWarning() << "[SubtitleGeneration] Discarding stale subtitle generation result"
                       << "requestedPath=" << requestedMediaPath
                       << "currentPath=" << currentMediaPath
                       << "requestedGeneration=" << requestedMediaGenerationId
                       << "currentGeneration=" << _p->mediaGenerationId
                       << "requestedFingerprint=" << requestedMediaFingerprint
                       << "currentFingerprint=" << currentMediaFingerprint;
            if (qApp) {
                QJsonObject staleReport{
                    { QStringLiteral("staleMediaGenerationDiscarded"), true },
                    { QStringLiteral("requestedMediaPath"), requestedMediaPath },
                    { QStringLiteral("currentMediaPath"), currentMediaPath },
                    { QStringLiteral("requestedMediaFingerprint"), requestedMediaFingerprint },
                    { QStringLiteral("currentMediaFingerprint"), currentMediaFingerprint }
                };
                qApp->setProperty(
                    "cgplay.subtitleGenerationLastStaleResult",
                    QString::fromUtf8(QJsonDocument(staleReport).toJson(QJsonDocument::Compact)));
            }
            return;
        }
        if (qApp) {
            qApp->setProperty(
                "cgplay.subtitleGenerationLastResult",
                QString::fromUtf8(QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact)));
        }

        if (!result.success) {
            const bool rateLimited = subtitleGenerationErrorLooksRateLimited(result.errorMessage);
            bool hasExistingSubtitles = !_p->generatedSubtitleCues.isEmpty();
            if (background && !result.translatedVttPath.trimmed().isEmpty()) {
                const QVector<GeneratedSubtitleCue> partialCues =
                    SubtitleGenerationService::readSubtitleFile(result.translatedVttPath);
                if (!partialCues.isEmpty()) {
                    mergeGeneratedSubtitleCues(&_p->generatedSubtitleCues, partialCues);
                    hasExistingSubtitles = !_p->generatedSubtitleCues.isEmpty();
                    if (hasExistingSubtitles) {
                        writeGeneratedTranslatedSubtitleSidecars(
                            finalTranslatedSrtPath,
                            finalTranslatedVttPath,
                            _p->generatedSubtitleCues,
                            QStringLiteral("background-failure-partial"));
                        writeGeneratedSubtitleMediaIdentitySidecars(
                            { finalTranslatedSrtPath, finalTranslatedVttPath },
                            _p->currentPath,
                            0.0,
                            QStringLiteral("background-failure-partial"));
                        _p->generatedSubtitlePath = QFileInfo(finalTranslatedVttPath).absoluteFilePath();
                        _p->generatedSubtitleMediaFingerprint =
                            SubtitleGenerationService::mediaFingerprint(_p->currentPath);
                        _p->generatedSubtitleCoverageEndSeconds =
                            generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
                        _p->generatedSubtitleProcessedEndSeconds = std::max(
                            _p->generatedSubtitleProcessedEndSeconds,
                            _p->generatedSubtitleCoverageEndSeconds);
                        if (_p->playbackBar) {
                            _p->playbackBar->setTranslationVisible(true);
                        }
                        _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
                        qInfo() << "[SubtitleGeneration] Merged partial subtitles from failed background continuation"
                                << "partialCueCount=" << partialCues.size()
                                << "translatedCoverageEnd=" << _p->generatedSubtitleCoverageEndSeconds
                                << "sourceCoverageEnd=" << generatedSubtitleMaxEndSeconds(_p->generatedSubtitleCues)
                                << "error=" << result.errorMessage;
                    }
                }
            }
            if (!background || !hasExistingSubtitles) {
                _p->subtitleGenerationLastError = result.errorMessage.trimmed();
            }
            if (!hasExistingSubtitles) {
                if (_p->playbackBar) {
                    _p->playbackBar->setTranslationVisible(background);
                }
                if (_p->generatedSubtitleLabel) {
                    _p->generatedSubtitleLabel->hide();
                }
            } else {
                if (_p->playbackBar) {
                    _p->playbackBar->setTranslationVisible(true);
                }
                _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
            }
            if (result.errorMessage.contains(QStringLiteral("canceled"), Qt::CaseInsensitive) ||
                result.errorMessage.contains(QStringLiteral("取消"))) {
                if (statusBar()) {
                    statusBar()->showMessage(QStringLiteral("字幕生成已取消。"), 3000);
                }
                return;
            }
            if (background || (rateLimited && hasExistingSubtitles)) {
                qWarning() << "[SubtitleGeneration] Background subtitle continuation failed:"
                           << result.errorMessage;
                const bool canRetryBackground =
                    background &&
                    hasExistingSubtitles &&
                    _p->playbackBar &&
                    _p->playbackBar->translationVisible() &&
                    _p->subtitleContinuationRetryCount < 4;
                if (statusBar()) {
                    statusBar()->showMessage(
                        rateLimited
                            ? QStringLiteral("Gemini 免费额度/频率限制，已保留当前字幕，稍后会继续补。")
                            : (canRetryBackground
                                ? QStringLiteral("后续字幕生成失败，正在自动重试：%1").arg(result.errorMessage)
                                : QStringLiteral("后续字幕后台生成失败：%1").arg(result.errorMessage)),
                        5000);
                }
                if ((rateLimited || canRetryBackground) && _p->playbackBar && _p->playbackBar->translationVisible()) {
                    const int retryDelayMs = rateLimited
                        ? 25000
                        : std::min(45000, 7000 + _p->subtitleContinuationRetryCount * 9000);
                    _p->subtitleContinuationRetryCount += 1;
                    QTimer::singleShot(retryDelayMs, this, [this, requestedStartSeconds]() {
                        if (_p->playbackBar &&
                            _p->playbackBar->translationVisible() &&
                            !_p->subtitleGenerationBusy) {
                            _generateSubtitlesForCurrentMedia(requestedStartSeconds, true);
                        }
                    });
                }
                return;
            }
            QMessageBox::warning(
                this,
                QStringLiteral("生成字幕失败"),
                rateLimited
                    ? QStringLiteral("Gemini 免费额度/频率限制，请等 20-30 秒后再继续，或换更高额度的语音识别 API。")
                    : (result.errorMessage.isEmpty() ? QStringLiteral("没有可用的错误信息。") : result.errorMessage));
            return;
        }

        _p->subtitleGenerationLastError.clear();
        _p->subtitleContinuationRetryCount = 0;
        _setTranslationStripVisible(false, false);
        if (_p->playbackBar) {
            _p->playbackBar->setTranslationVisible(true);
        }
        _setupGeneratedSubtitleOverlay();
        if (background && qApp && qApp->property("cgplay.subtitleGenerationSmokeMode").toBool()) {
            qApp->setProperty("cgplay.subtitleGenerationSmokeContinuationDone", true);
        }
        if (background) {
            mergeGeneratedSubtitleCues(&_p->generatedSubtitleCues, result.cues);
        } else {
            _p->generatedSubtitleCues = result.cues;
            for (auto& cue : _p->generatedSubtitleCues) {
                repairGeneratedSubtitleCueText(&cue);
            }
        }
        if (!_p->generatedSubtitleCues.isEmpty()) {
            writeGeneratedSubtitleTrack(finalSourceSrtPath, _p->generatedSubtitleCues, false, false);
            writeGeneratedSubtitleTrack(finalSourceVttPath, _p->generatedSubtitleCues, false, true);
            writeGeneratedSubtitleTrack(finalTranslatedSrtPath, _p->generatedSubtitleCues, true, false);
            writeGeneratedSubtitleTrack(finalTranslatedVttPath, _p->generatedSubtitleCues, true, true);
            writeGeneratedSubtitleMediaIdentitySidecars(
                { finalSourceSrtPath, finalSourceVttPath, finalTranslatedSrtPath, finalTranslatedVttPath },
                _p->currentPath,
                0.0,
                background ? QStringLiteral("background-finished") : QStringLiteral("initial-finished"));
            if (requestedMockWithoutApi) {
                _scheduleGeneratedSubtitleRefinement(
                    _p->currentPath,
                    _p->generatedSubtitleCues,
                    true);
            }
        }
        _p->generatedSubtitleCoverageEndSeconds =
            generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
        _updateHighQualityPrePlaybackWait(background
            ? QStringLiteral("background-finished")
            : QStringLiteral("initial-finished"));
        if (!_p->generatedSubtitleRefinedMerged && !_p->generatedSubtitleEnhancedMerged) {
            _p->generatedSubtitleQuickFallbackCueCount = _p->generatedSubtitleCues.size();
        }
        _p->generatedSubtitleProcessedEndSeconds = std::max(
            _p->generatedSubtitleProcessedEndSeconds,
            result.sourceCoverageEndSeconds > 0.0
                ? result.sourceCoverageEndSeconds
                : (result.processedEndSeconds > 0.0
                    ? result.processedEndSeconds
                    : requestedStartSeconds + requestedDurationSeconds));
        _p->generatedSubtitlePath = QFileInfo(finalTranslatedVttPath).absoluteFilePath();
        _p->generatedSubtitleMediaFingerprint =
            SubtitleGenerationService::mediaFingerprint(_p->currentPath);
        if (_p->generatedSubtitleCues.isEmpty()) {
            _loadGeneratedSubtitleTrack();
        } else {
            _publishTranslationPlaybackStrategyDiagnostics(
                _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
            _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
        }
        if (_p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId() &&
            _p->playbackBar &&
            _p->playbackBar->translationVisible()) {
            _startCurrentMediaHighQualityEnhancement(QStringLiteral("quick-generation-finished-current-media"));
        }
        if (statusBar()) {
            statusBar()->showMessage(
                QStringLiteral("字幕已生成：%1 条").arg(_p->generatedSubtitleCues.size()),
                4000);
        }
        if (_p->playbackBar && _p->playbackBar->translationVisible()) {
            const bool smokeStopsAfterOneContinuation = false;
            if (!smokeStopsAfterOneContinuation) {
                const bool hasQueuedContinuation =
                    _p->subtitleContinuationQueuedWhileBusy &&
                    _p->subtitleContinuationQueuedStartSeconds >= 0.0;
                const double nextStartSeconds = hasQueuedContinuation
                    ? _p->subtitleContinuationQueuedStartSeconds
                    : std::max(0.0, _p->generatedSubtitleCoverageEndSeconds - kSubtitleContinuationOverlapSeconds);
                _p->subtitleContinuationQueuedWhileBusy = false;
                _p->subtitleContinuationQueuedStartSeconds = -1.0;
                const double currentSeconds = generatedSubtitleCurrentSeconds(_p->playbackCtrl.get());
                const bool dedicatedWorkspaceRefinement =
                    hasDedicatedSubtitleRefinementWorkspace(_p->userSettings);
                const bool canRunDedicatedRefinementInBackground =
                    dedicatedWorkspaceRefinement &&
                    !_p->subtitleRefinementBusy &&
                    _p->generatedSubtitleCoverageEndSeconds >
                        _p->subtitleRefinementLastCoverageEndSeconds + 45.0;
                const bool canGiveRefinementTimeSlice =
                    !dedicatedWorkspaceRefinement &&
                    !_p->subtitleRefinementBusy &&
                    !_p->subtitleGenerationBusy &&
                    _p->generatedSubtitleCoverageEndSeconds - currentSeconds >= kSubtitlePrefetchLeadSeconds &&
                    _p->generatedSubtitleCoverageEndSeconds >
                        _p->subtitleRefinementLastCoverageEndSeconds + 45.0;
                if (canRunDedicatedRefinementInBackground) {
                    _scheduleGeneratedSubtitleContinuation(
                        nextStartSeconds,
                        0);
                    _scheduleGeneratedSubtitleRefinement(
                        _p->currentPath,
                        _p->generatedSubtitleCues,
                        false);
                } else if (canGiveRefinementTimeSlice) {
                    _p->subtitleContinuationAfterRefinementPending = true;
                    _p->subtitleContinuationAfterRefinementStartSeconds = nextStartSeconds;
                    _scheduleGeneratedSubtitleRefinement(
                        _p->currentPath,
                        _p->generatedSubtitleCues,
                        false);
                    if (!_p->subtitleRefinementBusy) {
                        _p->subtitleContinuationAfterRefinementPending = false;
                        _p->subtitleContinuationAfterRefinementStartSeconds = -1.0;
                        _scheduleGeneratedSubtitleContinuation(
                            nextStartSeconds,
                            0);
                    }
                } else {
                    _scheduleGeneratedSubtitleContinuation(
                        nextStartSeconds,
                        0);
                }
            }
        } else if (!_p->generatedSubtitleCues.isEmpty()) {
            _scheduleGeneratedSubtitleRefinement(
                _p->currentPath,
                _p->generatedSubtitleCues,
                requestedMockWithoutApi);
        }
    });
    watcher->setFuture(future);
}

} // namespace cgplay
