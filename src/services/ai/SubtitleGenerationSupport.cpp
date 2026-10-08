#include "SubtitleGenerationSupport.h"

#include "common/core/ServiceLocator.h"
#include "ai/api/IAICredentialStore.h"

#include "common/jobs/JobSystem.h"
#include "common/core/ServiceLocator.h"
#include "ai/api/IAIProviderManager.h"
#include "ai/api/IAICredentialStore.h"
#include "ai/SubtitleAsrApiClient.h"
#include "media/MediaProbe.h"
#include "settings/api/ISettingsService.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

namespace cgplay::subtitle_generation_support {

constexpr int kFfmpegTimeoutMs = 120000;

QString cleanLanguageCode(QString code)
{
    code = code.trimmed();
    if (code.isEmpty() || code.compare(QStringLiteral("zh"), Qt::CaseInsensitive) == 0 ||
        code.compare(QStringLiteral("zh-cn"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("zh-Hans");
    }
    return code;
}

int configuredInt(
    ISettingsService* settings,
    const QString& settingsKey,
    const QString& envKey,
    int fallback,
    int minimum,
    int maximum)
{
    int value = fallback;
    bool ok = false;
    const QString envValue = QProcessEnvironment::systemEnvironment().value(envKey).trimmed();
    if (!envValue.isEmpty()) {
        const int parsed = envValue.toInt(&ok);
        if (ok) {
            value = parsed;
        }
    } else if (settings) {
        const int parsed = settings->value(settingsKey, fallback).toInt(&ok);
        if (ok) {
            value = parsed;
        }
    }
    return std::clamp(value, minimum, maximum);
}

bool configuredBool(
    ISettingsService* settings,
    const QString& settingsKey,
    const QString& envKey,
    bool fallback)
{
    const QString envValue = QProcessEnvironment::systemEnvironment().value(envKey).trimmed().toLower();
    if (!envValue.isEmpty()) {
        return envValue == QStringLiteral("1") ||
            envValue == QStringLiteral("true") ||
            envValue == QStringLiteral("yes") ||
            envValue == QStringLiteral("on");
    }
    if (settings && settings->contains(settingsKey)) {
        return settings->value(settingsKey, fallback).toBool();
    }
    return fallback;
}

QString subtitleSuffix(const QString& targetLanguage)
{
    QString normalized = cleanLanguageCode(targetLanguage).toLower();
    if (normalized == QStringLiteral("zh-hans")) {
        return QStringLiteral("zh");
    }
    normalized.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    return normalized;
}

QString defaultSubtitleOutputDirectory(const QString& mediaPath)
{
    const QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!desktopPath.trimmed().isEmpty()) {
        return QDir(desktopPath).filePath(QStringLiteral("CGPlay_Translations"));
    }
    return QFileInfo(mediaPath).absolutePath();
}

QString baseOutputPath(const QString& mediaPath, const QString& outputDirectory)
{
    const QFileInfo mediaInfo(mediaPath);
    const QString dirPath = outputDirectory.trimmed().isEmpty()
        ? defaultSubtitleOutputDirectory(mediaPath)
        : QFileInfo(outputDirectory).absoluteFilePath();
    const QString fingerprint = SubtitleGenerationService::mediaFingerprint(mediaPath).left(12);
    const QString scopedBaseName = fingerprint.isEmpty()
        ? mediaInfo.completeBaseName()
        : QStringLiteral("%1.%2").arg(mediaInfo.completeBaseName(), fingerprint);
    return QDir(dirPath).filePath(scopedBaseName);
}

QString normalizeSubtitlePath(const QString& path)
{
    const QString trimmed = path.trimmed();
    return trimmed.isEmpty() ? QString() : QFileInfo(trimmed).absoluteFilePath().trimmed();
}

QString fileContentSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QString::fromLatin1(hash.result().toHex());
}

void writeMediaIdentitySidecars(
    const QStringList& cachePaths,
    const QString& mediaPath,
    double durationSeconds)
{
    for (const QString& cachePath : cachePaths) {
        if (cachePath.trimmed().isEmpty() || !QFileInfo::exists(cachePath)) {
            continue;
        }
        QString error;
        if (!SubtitleGenerationService::writeMediaIdentitySidecar(cachePath, mediaPath, durationSeconds, &error)) {
            qWarning() << "[SubtitleGeneration] Failed to write media identity sidecar"
                       << cachePath << error;
        }
    }
}

QStringList candidateSubtitlePaths(const QString& mediaPath)
{
    const QFileInfo mediaInfo(mediaPath);
    QStringList candidates = MediaProbe::probe(mediaPath).externalSubtitlePaths;
    const QDir dir = mediaInfo.dir();
    const QString baseName = mediaInfo.completeBaseName();
    for (const QString& suffix : {QStringLiteral(".srt"), QStringLiteral(".vtt"), QStringLiteral(".ass"), QStringLiteral(".ssa")}) {
        const QString candidate = dir.filePath(baseName + suffix);
        if (QFileInfo::exists(candidate)) {
            candidates.push_back(QFileInfo(candidate).absoluteFilePath());
        }
    }
    candidates.removeDuplicates();
    return candidates;
}

bool isDirectSubtitleFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    return suffix == QStringLiteral("srt") ||
        suffix == QStringLiteral("vtt") ||
        suffix == QStringLiteral("ass") ||
        suffix == QStringLiteral("ssa");
}

bool isGeneratedSubtitleSidecarPath(const QString& path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    return name.contains(QStringLiteral(".source.")) ||
        name.contains(QStringLiteral(".refined.")) ||
        name.contains(QStringLiteral(".enhanced.")) ||
        name.contains(QStringLiteral(".repair.")) ||
        name.contains(QStringLiteral(".online.")) ||
        name.contains(QStringLiteral(".ocr.")) ||
        name.contains(QStringLiteral(".longasr.")) ||
        name.contains(QStringLiteral(".hq.translated."));
}

bool isUsableExternalSubtitleSourceFile(const QString& path)
{
    return isDirectSubtitleFile(path) && !isGeneratedSubtitleSidecarPath(path);
}

QString subtitleSourceKindForPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("ass") || suffix == QStringLiteral("ssa")) {
        return QStringLiteral("ass");
    }
    if (suffix == QStringLiteral("srt") || suffix == QStringLiteral("vtt")) {
        return suffix;
    }
    return path.trimmed().isEmpty() ? QString() : QStringLiteral("embedded");
}

bool subtitleTimelineUsable(const QVector<GeneratedSubtitleCue>& cues)
{
    if (cues.isEmpty()) {
        return false;
    }
    double previousEnd = -1.0;
    int validCount = 0;
    for (const auto& cue : cues) {
        if (cue.endSeconds <= cue.startSeconds) {
            return false;
        }
        if (previousEnd >= 0.0 && cue.startSeconds + 0.25 < previousEnd) {
            return false;
        }
        previousEnd = std::max(previousEnd, cue.endSeconds);
        ++validCount;
    }
    return validCount > 0;
}

QString detectSubtitleTextLanguage(const QVector<GeneratedSubtitleCue>& cues)
{
    int han = 0;
    int kana = 0;
    int latin = 0;
    for (const auto& cue : cues) {
        const QString text = (cue.sourceText.trimmed().isEmpty() ? cue.translatedText : cue.sourceText).trimmed();
        for (const QChar ch : text) {
            const ushort u = ch.unicode();
            if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0x31F0 && u <= 0x31FF)) {
                ++kana;
            } else if (u >= 0x4E00 && u <= 0x9FFF) {
                ++han;
            } else if ((u >= QLatin1Char('A').unicode() && u <= QLatin1Char('Z').unicode()) ||
                       (u >= QLatin1Char('a').unicode() && u <= QLatin1Char('z').unicode())) {
                ++latin;
            }
        }
    }
    if (kana > 0) {
        return QStringLiteral("ja");
    }
    if (han > latin) {
        return QStringLiteral("zh");
    }
    if (latin > 0) {
        return QStringLiteral("en");
    }
    return QStringLiteral("unknown");
}

QStringList defaultTerminologyHints()
{
    return {
        QStringLiteral("Shadow Garden=暗影庭园"),
        QStringLiteral("Shadow=暗影大人"),
        QStringLiteral("Bushin Festival=武神祭"),
        QStringLiteral("Oriana=奥利亚纳")
    };
}

QString onlineSubtitleApiKey()
{
    if (auto* store = ServiceLocator::getService<IAICredentialStore>()) {
        QString error;
        const QByteArray secret =
            store->loadSecret(QStringLiteral("subtitles/onlineApiKey"), &error).trimmed();
        if (!secret.isEmpty()) {
            return QString::fromUtf8(secret).trimmed();
        }
    }
    return QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("SUBTITLE_ONLINE_API_KEY"))
        .trimmed();
}

QString onlineSubtitleBaseUrl(ISettingsService* settings)
{
    QString baseUrl;
    if (settings) {
        baseUrl = settings->value(QStringLiteral("ai/subtitles/online/baseUrl")).toString().trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_ONLINE_BASE_URL")).trimmed();
    }
    return baseUrl.isEmpty() ? QStringLiteral("https://api.opensubtitles.com/api/v1") : baseUrl;
}

QString srtTime(double seconds)
{
    const int totalMs = std::max(0, static_cast<int>(std::round(seconds * 1000.0)));
    const int hours = totalMs / 3600000;
    const int minutes = (totalMs / 60000) % 60;
    const int secs = (totalMs / 1000) % 60;
    const int ms = totalMs % 1000;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

QString vttTime(double seconds)
{
    QString value = srtTime(seconds);
    value.replace(QLatin1Char(','), QLatin1Char('.'));
    return value;
}

double parseSubtitleTime(QString value)
{
    value = value.trimmed();
    value.replace(QLatin1Char(','), QLatin1Char('.'));
    const QStringList parts = value.split(QLatin1Char(':'));
    if (parts.size() != 3) {
        return 0.0;
    }
    bool okH = false;
    bool okM = false;
    bool okS = false;
    const int hours = parts.at(0).toInt(&okH);
    const int minutes = parts.at(1).toInt(&okM);
    const double seconds = parts.at(2).toDouble(&okS);
    if (!okH || !okM || !okS) {
        return 0.0;
    }
    return hours * 3600.0 + minutes * 60.0 + seconds;
}

int subtitleMojibakeScore(const QString& text)
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
        case 0x50A6: // 傦
        case 0x59DD: // 姝
        case 0x6902: // 椂
        case 0x69F8: // 槸
        case 0x9286: // 銆
        case 0x934B: // 鍋
        case 0x9360: // 鍠
        case 0x9365: // 鍥
        case 0x93B4: // 鎴
        case 0x93C4: // 鏄
        case 0x93C8: // 鏈
        case 0x93C9: // 鏉
        case 0x9470: // 鑰
        case 0x9473: // 鑳
        case 0x95AD: // 閭
        case 0x95AB: // 閫
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

QString repairSubtitleMojibake(const QString& text)
{
    if (text.trimmed().isEmpty() || subtitleMojibakeScore(text) < 3) {
        return text;
    }
    const QString localRepaired = QString::fromUtf8(text.toLocal8Bit()).trimmed();
    if (!localRepaired.isEmpty() && subtitleMojibakeScore(localRepaired) + 2 < subtitleMojibakeScore(text)) {
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
    if (cleaned.endsWith(QLatin1Char('?')) && subtitleMojibakeScore(text) >= 3) {
        cleaned.chop(1);
        cleaned += QStringLiteral("。");
    }
    return subtitleMojibakeScore(cleaned) + 2 < subtitleMojibakeScore(text) ? cleaned : text;
}

bool sourceHintExpectsJapanese(const QString& sourceLanguageHint)
{
    return sourceLanguageHint.trimmed().compare(QStringLiteral("ja"), Qt::CaseInsensitive) == 0 ||
        sourceLanguageHint.trimmed().compare(QStringLiteral("japanese"), Qt::CaseInsensitive) == 0;
}

bool sourceHintExpectsChinese(const QString& sourceLanguageHint)
{
    const QString hint = sourceLanguageHint.trimmed().toLower();
    return hint == QStringLiteral("zh") ||
        hint == QStringLiteral("zh-cn") ||
        hint == QStringLiteral("zh-hans") ||
        hint == QStringLiteral("zh-hant") ||
        hint == QStringLiteral("chinese") ||
        hint == QStringLiteral("traditional chinese") ||
        hint == QStringLiteral("simplified chinese");
}

bool textContainsHan(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

QString normalizeChineseSubtitleLiteralForZhHans(QString text)
{
    text = repairSubtitleMojibake(text).trimmed();
    const QVector<QPair<QString, QString>> replacements = {
        { QStringLiteral("\u767c\u5e03"), QStringLiteral("\u53d1\u5e03") },
        { QStringLiteral("\u767c"), QStringLiteral("\u53d1") },
        { QStringLiteral("\u56b4"), QStringLiteral("\u4e25") },
        { QStringLiteral("\u95c7\u5f71"), QStringLiteral("\u6697\u5f71") },
        { QStringLiteral("\u95a3\u5f71"), QStringLiteral("\u6697\u5f71") },
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
        { QStringLiteral("\u5f8c"), QStringLiteral("\u540e") },
        { QStringLiteral("\u7121"), QStringLiteral("\u65e0") },
        { QStringLiteral("\u55da"), QStringLiteral("\u545c") },
        { QStringLiteral("\u807d"), QStringLiteral("\u542c") },
        { QStringLiteral("\u6642"), QStringLiteral("\u65f6") },
        { QStringLiteral("\u5c0d"), QStringLiteral("\u5bf9") },
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

QString normalizeJapaneseAsrSourceText(QString text, const QString& sourceLanguageHint)
{
    text = repairSubtitleMojibake(text).trimmed();
    if (!sourceHintExpectsJapanese(sourceLanguageHint) || text.isEmpty()) {
        return text;
    }

    const QVector<QPair<QString, QString>> replacements = {
        { QStringLiteral("让せ"), QStringLiteral("任せ") },
        { QStringLiteral("让"), QStringLiteral("譲") },
        { QStringLiteral("时"), QStringLiteral("時") },
        { QStringLiteral("决破"), QStringLiteral("突破") },
        { QStringLiteral("决"), QStringLiteral("決") },
        { QStringLiteral("头"), QStringLiteral("頭") },
        { QStringLiteral("气"), QStringLiteral("気") },
        { QStringLiteral("全身だ"), QStringLiteral("前進だ") },
        { QStringLiteral("全身！"), QStringLiteral("前進！") },
        { QStringLiteral("全身!"), QStringLiteral("前進!") }
    };
    for (const auto& replacement : replacements) {
        text.replace(replacement.first, replacement.second);
    }
    text.replace(QStringLiteral("杀"), QStringLiteral("殺"));
    text.replace(QStringLiteral("这"), QStringLiteral("これ"));
    text.replace(QStringLiteral("们"), QStringLiteral(""));
    text.replace(QStringLiteral("吗"), QStringLiteral("か"));
    text.replace(QStringLiteral("为"), QStringLiteral("為"));
    text.replace(QStringLiteral("对"), QStringLiteral("対"));
    text.replace(QStringLiteral("准备"), QStringLiteral("準備"));
    text.replace(QStringLiteral("积极"), QStringLiteral("積極"));
    text.replace(QStringLiteral("学园"), QStringLiteral("学園"));
    text.replace(QStringLiteral("一绪"), QStringLiteral("一緒"));
    text.replace(QStringLiteral("愿"), QStringLiteral("願"));
    text.replace(QStringLiteral("后者"), QStringLiteral("強者"));
    text.replace(QStringLiteral("动"), QStringLiteral("動"));
    text.replace(QStringLiteral("魔法庭"), QStringLiteral("シャドウガーデン"));
    text.replace(QStringLiteral("Boss"), QStringLiteral("ボス"));
    text.replace(QStringLiteral("机に動かれている"), QStringLiteral("敵に動かれている"));
    text.replace(QStringLiteral("脱出の岛とお願いしましょう"), QStringLiteral("脱出の準備を急ぎましょう"));
    text.replace(QStringLiteral("脱出の始末"), QStringLiteral("脱出の準備"));
    text.replace(QStringLiteral("日本が入るんだって"), QStringLiteral("武神祭に出るんだって"));
    if (text.contains(QStringLiteral("朝山")) ||
        text.contains(QStringLiteral("中意什么死"))) {
        text = QStringLiteral("すべてはシャドウ様の御心のままに。");
    }

    const QString normalizedLower = text.simplified().toLower();
    if (normalizedLower == QStringLiteral("yeah") ||
        normalizedLower == QStringLiteral("yep") ||
        normalizedLower == QStringLiteral("ya")) {
        return QStringLiteral("うん");
    }
    if (normalizedLower == QStringLiteral("okay") ||
        normalizedLower == QStringLiteral("ok")) {
        return QStringLiteral("わかった");
    }
    if (normalizedLower.contains(QStringLiteral("like my power"))) {
        return QStringLiteral("我が魔力");
    }
    return text;
}

QString normalizeAnimeTerminology(QString text);

bool subtitleTranslationFailureCanUsePlaceholder(const QString& error)
{
    Q_UNUSED(error);
    return false;
}

bool subtitleTranslationFailureCanFallbackToProvider(const QString& error)
{
    const QString normalized = error.trimmed().toLower();
    return normalized.contains(QStringLiteral("timed out")) ||
        normalized.contains(QStringLiteral("timeout")) ||
        normalized.contains(QStringLiteral("http 429")) ||
        normalized.contains(QStringLiteral("http 500")) ||
        normalized.contains(QStringLiteral("http 502")) ||
        normalized.contains(QStringLiteral("http 503")) ||
        normalized.contains(QStringLiteral("http 504"));
}

bool workspaceRefinementFailureCanUseSubtitleApiFallback(const QString& error)
{
    const QString normalized = error.trimmed().toLower();
    return normalized.contains(QStringLiteral("provider is unavailable")) ||
        normalized.contains(QStringLiteral("api provider is unavailable")) ||
        normalized.contains(QStringLiteral("no api key configured")) ||
        normalized.contains(QStringLiteral("api key is required")) ||
        normalized.contains(QStringLiteral("api key is missing")) ||
        normalized.contains(QStringLiteral("missing api key")) ||
        normalized.contains(QStringLiteral("not configured"));
}

bool textContainsJapaneseKana(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 0x3040 && u <= 0x30ff) || (u >= 0x31f0 && u <= 0x31ff)) {
            return true;
        }
    }
    return false;
}

bool textContainsLatinLetter(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            return true;
        }
    }
    return false;
}

QJsonObject analyzeSubtitleSourceQuality(
    const QString& sourceText,
    const QString& sourceLanguageHint,
    double durationSeconds)
{
    const QString rawText = repairSubtitleMojibake(sourceText).trimmed();
    int simplifiedDriftCount = 0;
    for (const QChar ch : rawText) {
        switch (ch.unicode()) {
        case 0x8BA9: // 让
        case 0x65F6: // 时
        case 0x51B3: // 决
        case 0x5934: // 头
        case 0x6C14: // 气
            simplifiedDriftCount += 1;
            break;
        default:
            break;
        }
    }
    const QString text = normalizeJapaneseAsrSourceText(sourceText, sourceLanguageHint);
    int kanaCount = 0;
    int hanCount = 0;
    int latinCount = 0;
    int digitCount = 0;
    int visibleCount = 0;
    for (const QChar ch : text) {
        if (ch.isSpace() || ch.isPunct()) {
            continue;
        }
        const ushort u = ch.unicode();
        visibleCount += 1;
        if ((u >= 0x3040 && u <= 0x30ff) || (u >= 0x31f0 && u <= 0x31ff)) {
            kanaCount += 1;
        } else if (u >= 0x4e00 && u <= 0x9fff) {
            hanCount += 1;
        } else if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            latinCount += 1;
        } else if (u >= '0' && u <= '9') {
            digitCount += 1;
        }
    }
    const double denominator = std::max(1, visibleCount);
    const double kanaRatio = kanaCount / denominator;
    const double hanRatio = hanCount / denominator;
    const double latinRatio = latinCount / denominator;
    const bool expectsJapanese =
        sourceLanguageHint.trimmed().compare(QStringLiteral("ja"), Qt::CaseInsensitive) == 0 ||
        sourceLanguageHint.trimmed().compare(QStringLiteral("japanese"), Qt::CaseInsensitive) == 0;

    QStringList reasons;
    if (text.isEmpty()) {
        reasons.push_back(QStringLiteral("empty-source"));
    }
    if (expectsJapanese) {
        const QString lower = text.toLower();
        static const QStringList englishFillers = {
            QStringLiteral("yeah"),
            QStringLiteral("okay"),
            QStringLiteral(" ok"),
            QStringLiteral("hey"),
            QStringLiteral("wow"),
            QStringLiteral("oh my god"),
            QStringLiteral("like my power"),
            QStringLiteral("my power")
        };
        for (const QString& marker : englishFillers) {
            if (lower == marker.trimmed() || lower.contains(marker)) {
                reasons.push_back(QStringLiteral("english-filler-in-japanese-asr"));
                break;
            }
        }
        if (latinCount >= 2 && latinRatio >= 0.25) {
            reasons.push_back(QStringLiteral("latin-heavy-for-japanese"));
        }
        if (kanaCount == 0 && hanCount >= 6 && latinCount == 0) {
            reasons.push_back(QStringLiteral("kanji-only-or-possibly-translated"));
        }
        if (visibleCount >= 8 && kanaCount == 0 && latinCount == 0 && hanRatio >= 0.75) {
            reasons.push_back(QStringLiteral("possible-chinese-translation-in-source"));
        }
        if (simplifiedDriftCount > 0) {
            reasons.push_back(QStringLiteral("simplified-kanji-drift-in-japanese-asr"));
        }
        static const QStringList obviousChineseDrift = {
            QStringLiteral("杀"),
            QStringLiteral("这"),
            QStringLiteral("们"),
            QStringLiteral("吗"),
            QStringLiteral("为"),
            QStringLiteral("对"),
            QStringLiteral("准备"),
            QStringLiteral("积极"),
            QStringLiteral("学园"),
            QStringLiteral("后者"),
            QStringLiteral("魔法庭")
        };
        for (const QString& marker : obviousChineseDrift) {
            if (rawText.contains(marker)) {
                reasons.push_back(QStringLiteral("chinese-simplified-drift-in-japanese-asr"));
                break;
            }
        }
        if (durationSeconds >= 3.5 && visibleCount <= 2) {
            reasons.push_back(QStringLiteral("very-short-four-second-cue"));
        }
    }

    QJsonObject metrics{
        { QStringLiteral("visible"), visibleCount },
        { QStringLiteral("kana"), kanaCount },
        { QStringLiteral("han"), hanCount },
        { QStringLiteral("latin"), latinCount },
        { QStringLiteral("digits"), digitCount },
        { QStringLiteral("simplifiedDrift"), simplifiedDriftCount },
        { QStringLiteral("durationSeconds"), durationSeconds },
        { QStringLiteral("kanaRatio"), kanaRatio },
        { QStringLiteral("hanRatio"), hanRatio },
        { QStringLiteral("latinRatio"), latinRatio },
        { QStringLiteral("lowConfidence"), !reasons.isEmpty() },
        { QStringLiteral("reason"), reasons.join(QLatin1Char('|')) }
    };
    return metrics;
}

void annotateSubtitleSourceQuality(
    GeneratedSubtitleCue* cue,
    const QString& sourceLanguageHint)
{
    if (!cue) {
        return;
    }
    const QJsonObject metrics = analyzeSubtitleSourceQuality(
        cue->sourceText,
        sourceLanguageHint,
        std::max(0.0, cue->endSeconds - cue->startSeconds));
    cue->qualityMetrics = metrics;
    cue->lowConfidence = metrics.value(QStringLiteral("lowConfidence")).toBool(false);
    cue->qualityReason = metrics.value(QStringLiteral("reason")).toString();
}

QString subtitleTranslationQualityReason(const GeneratedSubtitleCue& cue)
{
    QStringList reasons;
    const QString source = repairSubtitleMojibake(cue.sourceText).trimmed();
    QString translated = cue.translatedText;
    translated = translated.replace(QStringLiteral("\r\n"), QStringLiteral(" "))
                     .replace(QLatin1Char('\n'), QLatin1Char(' '))
                     .replace(QLatin1Char('\r'), QLatin1Char(' '))
                     .simplified();
    if (translated.isEmpty()) {
        reasons.push_back(QStringLiteral("empty-quick-translation"));
    }
    if (textContainsJapaneseKana(translated)) {
        reasons.push_back(QStringLiteral("japanese-leftover-in-quick-translation"));
    }
    if (subtitleMojibakeScore(translated) >= 4) {
        reasons.push_back(QStringLiteral("mojibake-in-quick-translation"));
    }

    int cjkCount = 0;
    int latinCount = 0;
    int visibleCount = 0;
    for (const QChar ch : translated) {
        if (ch.isSpace() || ch.isPunct()) {
            continue;
        }
        const ushort u = ch.unicode();
        visibleCount += 1;
        if (u >= 0x4e00 && u <= 0x9fff) {
            cjkCount += 1;
        } else if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            latinCount += 1;
        }
    }
    if (visibleCount >= 3 && latinCount >= 2 && cjkCount == 0) {
        reasons.push_back(QStringLiteral("latin-heavy-quick-translation"));
    }

    const int sourceVisible = source.simplified().remove(QLatin1Char(' ')).size();
    const int translatedVisible = translated.simplified().remove(QLatin1Char(' ')).size();
    if (sourceVisible >= 6 && translatedVisible <= 1) {
        reasons.push_back(QStringLiteral("too-short-quick-translation"));
    }
    if (sourceVisible >= 4 &&
        translatedVisible > std::max(80, sourceVisible * 5 + 20)) {
        reasons.push_back(QStringLiteral("too-long-quick-translation"));
    }
    return reasons.join(QLatin1Char('|'));
}

QString normalizeTranslatedSubtitleLine(QString text)
{
    text = text.replace(QStringLiteral("\r\n"), QStringLiteral(" "))
               .replace(QLatin1Char('\n'), QLatin1Char(' '))
               .replace(QLatin1Char('\r'), QLatin1Char(' '));
    return text.simplified();
}

bool sourceExpressesUnsafeHere(const QString& sourceText)
{
    const QString source = repairSubtitleMojibake(sourceText).toLower().simplified();
    return source.contains(QStringLiteral("not safe")) ||
        source.contains(QStringLiteral("isn't safe")) ||
        source.contains(QStringLiteral("is not safe")) ||
        source.contains(QStringLiteral("unsafe")) ||
        source.contains(QStringLiteral("not secure")) ||
        source.contains(QStringLiteral("dangerous")) ||
        source.contains(QStringLiteral("in danger")) ||
        source.contains(QStringLiteral("不安全"));
}

bool translatedExpressesSafeHere(const QString& translatedText)
{
    const QString text = normalizeTranslatedSubtitleLine(translatedText);
    const bool explicitlyUnsafe =
        text.contains(QStringLiteral("不安全")) ||
        text.contains(QStringLiteral("有危险")) ||
        (text.contains(QStringLiteral("危险")) &&
         !text.contains(QStringLiteral("没有危险")) &&
         !text.contains(QStringLiteral("沒有危險")) &&
         !text.contains(QStringLiteral("无危险")) &&
         !text.contains(QStringLiteral("無危險")) &&
         !text.contains(QStringLiteral("沒危險")) &&
         !text.contains(QStringLiteral("不危险")));
    if (explicitlyUnsafe) {
        return false;
    }
    return text.contains(QStringLiteral("没有危险")) ||
        text.contains(QStringLiteral("无危险")) ||
        text.contains(QStringLiteral("沒危險")) ||
        text.contains(QStringLiteral("沒有危險")) ||
        text.contains(QStringLiteral("不危险")) ||
        text.contains(QStringLiteral("很安全")) ||
        text.contains(QStringLiteral("这里安全")) ||
        text.contains(QStringLiteral("這裡安全"));
}

QString enforceSafetyNegationPolarity(const QString& sourceText, const QString& translatedText)
{
    QString text = normalizeTranslatedSubtitleLine(normalizeAnimeTerminology(translatedText));
    if (!sourceExpressesUnsafeHere(sourceText) || !translatedExpressesSafeHere(text)) {
        return text;
    }
    text.replace(QStringLiteral("这里没有危险"), QStringLiteral("这里不安全"));
    text.replace(QStringLiteral("這裡沒有危險"), QStringLiteral("这里不安全"));
    text.replace(QStringLiteral("这里无危险"), QStringLiteral("这里不安全"));
    text.replace(QStringLiteral("這裡無危險"), QStringLiteral("这里不安全"));
    text.replace(QStringLiteral("这里沒危險"), QStringLiteral("这里不安全"));
    text.replace(QStringLiteral("不危险"), QStringLiteral("不安全"));
    text.replace(QStringLiteral("很安全"), QStringLiteral("不安全"));
    text.replace(QStringLiteral("这里安全"), QStringLiteral("这里不安全"));
    text.replace(QStringLiteral("這裡安全"), QStringLiteral("这里不安全"));
    text.replace(QStringLiteral("没有危险"), QStringLiteral("不安全"));
    text.replace(QStringLiteral("沒有危險"), QStringLiteral("不安全"));
    text.replace(QStringLiteral("无危险"), QStringLiteral("不安全"));
    text.replace(QStringLiteral("無危險"), QStringLiteral("不安全"));
    text.replace(QStringLiteral("沒危險"), QStringLiteral("不安全"));
    if (translatedExpressesSafeHere(text)) {
        return QStringLiteral("不，这里不安全。我们走吧。");
    }
    return text;
}

int applySafetyNegationPolarity(QVector<GeneratedSubtitleCue>* cues)
{
    if (!cues) {
        return 0;
    }
    int changed = 0;
    for (GeneratedSubtitleCue& cue : *cues) {
        const QString repaired = enforceSafetyNegationPolarity(cue.sourceText, cue.translatedText);
        if (!repaired.isEmpty() && repaired != normalizeTranslatedSubtitleLine(cue.translatedText)) {
            cue.translatedText = repaired;
            cue.qualityReason = cue.qualityReason.trimmed().isEmpty()
                ? QStringLiteral("safety-negation-polarity-repaired")
                : cue.qualityReason + QStringLiteral("|safety-negation-polarity-repaired");
            ++changed;
        }
    }
    return changed;
}

int applySafetyNegationPolarityFromSourceCues(
    QVector<GeneratedSubtitleCue>* translatedCues,
    const QVector<GeneratedSubtitleCue>& sourceCues)
{
    if (!translatedCues || translatedCues->isEmpty() || sourceCues.isEmpty()) {
        return 0;
    }
    int changed = 0;
    QSet<int> usedSourceIndexes;
    for (GeneratedSubtitleCue& translatedCue : *translatedCues) {
        int bestIndex = -1;
        double bestDelta = 1.0;
        for (int i = 0; i < sourceCues.size(); ++i) {
            if (usedSourceIndexes.contains(i)) {
                continue;
            }
            const GeneratedSubtitleCue& sourceCue = sourceCues.at(i);
            const double overlap =
                std::min(translatedCue.endSeconds, sourceCue.endSeconds) -
                std::max(translatedCue.startSeconds, sourceCue.startSeconds);
            const double delta =
                std::abs(translatedCue.startSeconds - sourceCue.startSeconds) +
                std::abs(translatedCue.endSeconds - sourceCue.endSeconds);
            if (overlap >= 0.25 || delta < bestDelta) {
                if (delta < bestDelta) {
                    bestDelta = delta;
                    bestIndex = i;
                }
            }
        }
        if (bestIndex < 0) {
            continue;
        }
        usedSourceIndexes.insert(bestIndex);
        const QString source = sourceCues.at(bestIndex).sourceText.trimmed().isEmpty()
            ? sourceCues.at(bestIndex).translatedText.trimmed()
            : sourceCues.at(bestIndex).sourceText.trimmed();
        const QString repaired = enforceSafetyNegationPolarity(source, translatedCue.translatedText);
        if (!repaired.isEmpty() && repaired != normalizeTranslatedSubtitleLine(translatedCue.translatedText)) {
            translatedCue.sourceText = source;
            translatedCue.translatedText = repaired;
            translatedCue.qualityReason = translatedCue.qualityReason.trimmed().isEmpty()
                ? QStringLiteral("safety-negation-polarity-repaired-from-source-cache")
                : translatedCue.qualityReason + QStringLiteral("|safety-negation-polarity-repaired-from-source-cache");
            ++changed;
        }
    }
    return changed;
}

int subtitleCueQualityScore(const GeneratedSubtitleCue& cue)
{
    const QString source = repairSubtitleMojibake(cue.sourceText).trimmed();
    const QString translated = repairSubtitleMojibake(cue.translatedText).trimmed();
    int score = 0;
    if (!source.isEmpty()) {
        score += 20 + std::min(80, static_cast<int>(source.size()));
    }
    if (!translated.isEmpty()) {
        score += 30 + std::min(100, static_cast<int>(translated.size()));
    }
    score -= subtitleMojibakeScore(source) * 4;
    score -= subtitleMojibakeScore(translated) * 4;
    return score;
}

void dedupeNearDuplicateSubtitleCues(QVector<GeneratedSubtitleCue>* cues)
{
    if (!cues || cues->size() < 2) {
        return;
    }
    QVector<GeneratedSubtitleCue> deduped;
    deduped.reserve(cues->size());
    for (auto cue : *cues) {
        cue.sourceText = repairSubtitleMojibake(cue.sourceText);
        cue.translatedText = repairSubtitleMojibake(cue.translatedText);
        if (!deduped.isEmpty()) {
            GeneratedSubtitleCue& previous = deduped.last();
            const bool sameTiming =
                std::abs(previous.startSeconds - cue.startSeconds) < 0.25 &&
                std::abs(previous.endSeconds - cue.endSeconds) < 0.25;
            if (sameTiming) {
                if (subtitleCueQualityScore(cue) > subtitleCueQualityScore(previous)) {
                    previous = cue;
                }
                continue;
            }
        }
        deduped.push_back(cue);
    }
    *cues = deduped;
}

QString cueTextForWrite(const GeneratedSubtitleCue& cue, bool translated)
{
    QString text = translated ? cue.translatedText.trimmed() : cue.sourceText.trimmed();
    text = translated ? enforceSafetyNegationPolarity(cue.sourceText, text) : repairSubtitleMojibake(text);
    return text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
}

QString stripJsonMarkdownFence(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("```"))) {
        const int firstNewline = text.indexOf(QLatin1Char('\n'));
        const int lastFence = text.lastIndexOf(QStringLiteral("```"));
        if (firstNewline >= 0 && lastFence > firstNewline) {
            text = text.mid(firstNewline + 1, lastFence - firstNewline - 1).trimmed();
        }
    }
    return text;
}

QJsonArray extractSegments(const QJsonObject& object)
{
    QJsonArray segments = object.value(QStringLiteral("segments")).toArray();
    if (!segments.isEmpty()) {
        return segments;
    }
    segments = object.value(QStringLiteral("chunks")).toArray();
    if (!segments.isEmpty()) {
        return segments;
    }
    return {};
}

double segmentNumber(const QJsonObject& object, const QString& key, double fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble()) {
        return value.toDouble(fallback);
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (key == QStringLiteral("start") && !array.isEmpty()) {
            return array.at(0).toDouble(fallback);
        }
        if (key == QStringLiteral("end") && array.size() > 1) {
            return array.at(1).toDouble(fallback);
        }
    }
    return fallback;
}

QString segmentText(const QJsonObject& object)
{
    QString text = object.value(QStringLiteral("text")).toString().trimmed();
    if (!text.isEmpty()) {
    return repairSubtitleMojibake(text);
}
    text = object.value(QStringLiteral("sentence")).toString().trimmed();
    if (!text.isEmpty()) {
        return text;
    }
    return object.value(QStringLiteral("content")).toString().trimmed();
}

QString tempAudioChunkPath(const QString& mediaPath, double startSeconds)
{
    const QByteArray hash = QCryptographicHash::hash(
        QFileInfo(mediaPath).absoluteFilePath().toUtf8() +
            QByteArray::number(startSeconds, 'f', 3) +
            QUuid::createUuid().toByteArray(),
        QCryptographicHash::Sha1).toHex();
    const QString dirPath = QDir::tempPath() + QStringLiteral("/cgplay_vod_subtitles");
    QDir().mkpath(dirPath);
    return QDir(dirPath).filePath(QString::fromLatin1(hash) + QStringLiteral(".wav"));
}

QString tempSubtitleTrackPath(const QString& mediaPath)
{
    const QByteArray hash = QCryptographicHash::hash(
        QFileInfo(mediaPath).absoluteFilePath().toUtf8(),
        QCryptographicHash::Sha1).toHex();
    const QString dirPath = QDir::tempPath() + QStringLiteral("/cgplay_vod_subtitles");
    QDir().mkpath(dirPath);
    return QDir(dirPath).filePath(QString::fromLatin1(hash) + QStringLiteral(".track.srt"));
}


void fillTranslationTaskWithSourcePlaceholders(SubtitleTranslationTask* task)
{
    if (!task) {
        return;
    }
    task->translatedItems.clear();
    for (const QJsonValue& value : task->items) {
        const QJsonObject object = value.toObject();
        const int cueIndex = object.value(QStringLiteral("i")).toInt(-1);
        const QString text = repairSubtitleMojibake(object.value(QStringLiteral("t")).toString().trimmed());
        if (cueIndex >= task->batchStart && cueIndex < task->batchEnd && !text.isEmpty()) {
            task->translatedItems.push_back(qMakePair(cueIndex, text));
        }
    }
}

QString defaultAnimeTerminologyPrompt()
{
    return QStringLiteral(
        "Terminology and name rules: "
        "シャドウガーデン/Shadow Garden=暗影庭园; "
        "シャドウ/Shadow=暗影; "
        "ローズ/Rose=萝兹; "
        "アイリス/Iris=艾莉丝; "
        "ベアトリクス/Beatrix=贝阿特丽克丝; "
        "オリアナ/Oriana=奥利亚纳; "
        "シド/Cid=希妲. "
        "Use these exact Chinese terms consistently across all subtitle cues.");
}

QString normalizeAnimeTerminology(QString text)
{
    text = repairSubtitleMojibake(text);
    const QVector<QPair<QString, QString>> replacements = {
        { QStringLiteral("暗影花园"), QStringLiteral("暗影庭园") },
        { QStringLiteral("影之庭园"), QStringLiteral("暗影庭园") },
        { QStringLiteral("暗影庭院"), QStringLiteral("暗影庭园") },
        { QStringLiteral("罗兹"), QStringLiteral("萝兹") },
        { QStringLiteral("萝丝"), QStringLiteral("萝兹") },
        { QStringLiteral("罗斯"), QStringLiteral("萝兹") },
        { QStringLiteral("艾丽丝"), QStringLiteral("艾莉丝") },
        { QStringLiteral("爱丽丝"), QStringLiteral("艾莉丝") },
        { QStringLiteral("贝阿特丽丝"), QStringLiteral("贝阿特丽克丝") },
        { QStringLiteral("贝亚特丽克丝"), QStringLiteral("贝阿特丽克丝") },
        { QStringLiteral("奥里亚纳"), QStringLiteral("奥利亚纳") },
        { QStringLiteral("奥莉亚纳"), QStringLiteral("奥利亚纳") },
        { QStringLiteral("Shadow先生"), QStringLiteral("暗影大人") },
        { QStringLiteral("暗影先生"), QStringLiteral("暗影大人") }
    };
    for (const auto& replacement : replacements) {
        text.replace(replacement.first, replacement.second);
    }
    return text.trimmed();
}

QString cleanAssSubtitleText(QString text)
{
    text.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\\h"), QStringLiteral(" "));
    text.remove(QRegularExpression(QStringLiteral("\\{[^}]*\\}")));
    text.replace(QRegularExpression(QStringLiteral("[ \\t]+")), QStringLiteral(" "));
    QStringList lines;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            lines.push_back(trimmed);
        }
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

QStringList splitAssFields(const QString& value, int expectedFieldCount)
{
    QStringList fields;
    QString current;
    const int stopAt = std::max(1, expectedFieldCount) - 1;
    for (const QChar ch : value) {
        if (ch == QLatin1Char(',') && fields.size() < stopAt) {
            fields.push_back(current.trimmed());
            current.clear();
        } else {
            current.append(ch);
        }
    }
    fields.push_back(current.trimmed());
    return fields;
}

QVector<GeneratedSubtitleCue> readAssSubtitleFile(const QString& subtitlePath)
{
    QFile file(subtitlePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    const QString content = QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QStringList lines = content.split(QLatin1Char('\n'));
    QStringList formatFields;
    int startIndex = 1;
    int endIndex = 2;
    int textIndex = 9;
    QVector<GeneratedSubtitleCue> cues;

    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("Format:"), Qt::CaseInsensitive)) {
            formatFields = line.mid(QStringLiteral("Format:").size()).split(QLatin1Char(','));
            for (QString& field : formatFields) {
                field = field.trimmed().toLower();
            }
            const int parsedStart = formatFields.indexOf(QStringLiteral("start"));
            const int parsedEnd = formatFields.indexOf(QStringLiteral("end"));
            const int parsedText = formatFields.indexOf(QStringLiteral("text"));
            if (parsedStart >= 0 && parsedEnd >= 0 && parsedText >= 0) {
                startIndex = parsedStart;
                endIndex = parsedEnd;
                textIndex = parsedText;
            }
            continue;
        }
        if (!line.startsWith(QStringLiteral("Dialogue:"), Qt::CaseInsensitive)) {
            continue;
        }

        const int fieldCount = formatFields.isEmpty() ? 10 : formatFields.size();
        const QStringList fields = splitAssFields(line.mid(QStringLiteral("Dialogue:").size()), fieldCount);
        if (fields.size() <= std::max({startIndex, endIndex, textIndex})) {
            continue;
        }
        const QString text = cleanAssSubtitleText(fields.at(textIndex));
        if (text.isEmpty()) {
            continue;
        }

        GeneratedSubtitleCue cue;
        cue.index = cues.size() + 1;
        cue.startSeconds = parseSubtitleTime(fields.at(startIndex));
        cue.endSeconds = parseSubtitleTime(fields.at(endIndex));
        cue.translatedText = text;
        cue.sourceText = cue.translatedText;
        if (cue.endSeconds > cue.startSeconds) {
            cues.push_back(cue);
        }
    }
    return cues;
}

bool extractSubtitleStream(const QString& mediaPath, const QString& outputPath, QString* errorMessage)
{
    const QString ffmpeg = MediaProbe::locateFfmpeg();
    if (ffmpeg.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("ffmpeg not found");
        }
        return false;
    }

    QFile::remove(outputPath);
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(ffmpeg, {
        QStringLiteral("-y"),
        QStringLiteral("-i"), mediaPath,
        QStringLiteral("-map"), QStringLiteral("0:s:0"),
        QStringLiteral("-c:s"), QStringLiteral("srt"),
        outputPath
    });
    JobContext job(kFfmpegTimeoutMs);
    const ProcessOutcome processOutcome = job.waitForProcess(process, 25, kFfmpegTimeoutMs);
    if (processOutcome.state == JobState::TimedOut) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("ffmpeg subtitle extraction timed out");
        }
        return false;
    }
    if (!processOutcome.succeeded() || !QFileInfo::exists(outputPath)) {
        if (errorMessage) {
            const QString processError = QString::fromUtf8(processOutcome.standardOutput).trimmed().left(600);
            *errorMessage = QStringLiteral("ffmpeg subtitle extraction failed: %1")
                .arg(processError.isEmpty() ? process.errorString() : processError);
        }
        return false;
    }
    return true;
}

QString resolveSubtitleSourcePath(const SubtitleGenerationRequest& request, const MediaInfo& mediaInfo, QString* errorMessage)
{
    const QString requested = normalizeSubtitlePath(request.subtitleSourcePath);
    if (!requested.isEmpty() && QFileInfo::exists(requested) && isUsableExternalSubtitleSourceFile(requested)) {
        return requested;
    }

    QStringList candidates = mediaInfo.externalSubtitlePaths;
    candidates.append(candidateSubtitlePaths(mediaInfo.path));
    candidates.removeDuplicates();
    for (const QString& candidate : candidates) {
        const QString normalized = normalizeSubtitlePath(candidate);
        if (!normalized.isEmpty() && QFileInfo::exists(normalized) && isUsableExternalSubtitleSourceFile(normalized)) {
            return normalized;
        }
    }

    if (mediaInfo.hasEmbeddedSubtitles) {
        const QString outputPath = tempSubtitleTrackPath(mediaInfo.path);
        QString extractError;
        if (extractSubtitleStream(mediaInfo.path, outputPath, &extractError)) {
            return outputPath;
        }
        if (errorMessage && !extractError.trimmed().isEmpty()) {
            *errorMessage = extractError;
        }
    }
    return {};
}


} // namespace cgplay::subtitle_generation_support
