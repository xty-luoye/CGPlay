#include "services/ai/TranslationEnhancementScheduler.h"
#include "services/ai/TranslationTextPolicy.h"
#include "services/ai/AIProviderManager.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <algorithm>

using namespace cgplay;

class CancelProbeProvider final : public IAIProvider {
public:
    std::shared_ptr<std::atomic_bool> token;
    std::atomic_int calls{0};
    std::atomic_int callsStartedAfterCancel{0};
    explicit CancelProbeProvider(std::shared_ptr<std::atomic_bool> t) : token(std::move(t)) {}
    QString providerId() const override { return QStringLiteral("audit-fake"); }
    QString providerName() const override { return QStringLiteral("Audit fake; no external API"); }
    bool isAvailable() const override { return true; }
    AIProviderCapabilities capabilities() const override { return {}; }
    AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest&) override { return {}; }
    AIResponse chat(const AIRequest& request) override {
        if (token->load()) ++callsStartedAfterCancel;
        const int call = ++calls;
        const int begin = request.userPrompt.indexOf(QLatin1Char('['));
        const QJsonArray input = QJsonDocument::fromJson(request.userPrompt.mid(begin).toUtf8()).array();
        QJsonArray translated;
        for (const auto& value : input) {
            const int index = value.toObject().value(QStringLiteral("i")).toInt();
            translated.append(QJsonObject{{QStringLiteral("i"), index},
                {QStringLiteral("text"), QStringLiteral("这是第%1条正常字幕。").arg(index)}});
        }
        AIResponse response;
        response.success = true;
        response.rawText = QString::fromUtf8(QJsonDocument(translated).toJson(QJsonDocument::Compact));
        // Schedule the same legal cancellation point as a user cancel while the first API request is in flight.
        if (call == 1) token->store(true);
        return response;
    }
};

namespace cgplay {
void mergeGeneratedSubtitleCues(QVector<GeneratedSubtitleCue>*, const QVector<GeneratedSubtitleCue>&, bool);
}

static void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size())
        qFatal("Fixture write failed");
}

static QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) qFatal("Fixture read failed");
    return f.readAll();
}

static QByteArray vtt(const QString& middle)
{
    return (QStringLiteral("WEBVTT\n\n00:00:00.000 --> 00:00:01.000\n你好，欢迎回来。\n\n"
                           "00:00:01.000 --> 00:00:02.000\n") + middle +
            QStringLiteral("\n\n00:00:02.000 --> 00:00:03.000\n今天过得怎么样？\n\n")).toUtf8();
}

static TranslationEnhancementScheduleRequest requestFor(const QString& dir)
{
    TranslationEnhancementScheduleRequest r;
    r.runtime.captured = true;
    r.mediaPath = dir + QStringLiteral("/movie.mp4");
    r.playbackMode = QStringLiteral("high-quality");
    r.manualRequested = true;
    r.executeWorkers = false;
    r.ocrConfigured = true;
    r.ocrAutoEnabledForHighQualityCurrentMedia = true;
    r.currentSeconds = 1.5;
    r.quickTranslatedVttPath = dir + QStringLiteral("/quick.vtt");
    r.ocrSubtitleCachePath = dir + QStringLiteral("/ocr.vtt");
    r.ocrTranslatedCachePath = dir + QStringLiteral("/ocr-translated.vtt");
    return r;
}


int main(int argc,char**argv){QCoreApplication app(argc,argv);QString root=QString::fromUtf8(argv[1]);QJsonObject output,result;QString policyDir=QDir(root).filePath("policy");QDir().mkpath(policyDir);auto policyRequest=requestFor(policyDir);writeFile(policyRequest.mediaPath,"fixture");const QString normal=QStringLiteral("我正在制作动画。");QString error;for(const auto&path:{policyRequest.quickTranslatedVttPath,policyRequest.ocrSubtitleCachePath,policyRequest.ocrTranslatedCachePath}){writeFile(path,vtt(normal));SubtitleGenerationService::writeMediaIdentitySidecar(path,policyRequest.mediaPath,3.,&error);}TranslationEnhancementScheduler scheduler;auto evaluated=scheduler.evaluate(policyRequest);result["normalAnimationDialogueDisplayable"]=!translation_text::isLikelyVisualWatermarkText(normal)&&translation_text::isFinalDisplayableText(normal);result["schedulerShowsNormalDialogue"]=evaluated["currentMediaHighQuality"].toObject()["activeVisualCueAtTime"].toBool()&&evaluated["currentMediaHighQuality"].toObject()["finalDisplayedText"].toString()==normal;output["policy"]=evaluated;
    // Run the real public generation entry point using local sidecar subtitles and a fake provider.
    // Clear routing overrides in this probe process only, so no real credentials or network are used.
    for (const char* key : {"SUBTITLE_TRANSLATION_BASE_URL", "SUBTITLE_TRANSLATION_API_KEY",
                           "MIMO_BASE_URL", "MIMO_API_KEY", "XIAOMI_MIMO_BASE_URL", "XIAOMI_MIMO_API_KEY",
                           "QWEN_BASE_URL", "QWEN_API_KEY", "DASHSCOPE_BASE_URL", "DASHSCOPE_API_KEY"})
        qunsetenv(key);
    qputenv("SUBTITLE_TRANSLATION_CONCURRENCY", "1");
    const QString cancelDir = root + QStringLiteral("/cancellation");
    QDir().mkpath(cancelDir);
    const QString mediaPath = cancelDir + QStringLiteral("movie.fixture");
    const QString sourcePath = cancelDir + QStringLiteral("source.srt");
    writeFile(mediaPath, QByteArray("isolated non-video fixture; real explicit SRT source used"));
    QString srt;
    for (int i = 0; i < 30; ++i) {
        srt += QStringLiteral("%1\n00:00:%2,000 --> 00:00:%3,900\nThis is normal spoken dialogue number %4.\n\n")
            .arg(i + 1).arg(i * 2, 2, 10, QLatin1Char('0'))
            .arg(i * 2 + 1, 2, 10, QLatin1Char('0')).arg(i + 1);
    }
    writeFile(sourcePath, srt.toUtf8());
    SubtitleGenerationRequest cancelRequest;
    cancelRequest.runtime.captured = true;
    cancelRequest.mediaPath = mediaPath;
    cancelRequest.subtitleSourcePath = sourcePath;
    cancelRequest.outputDirectory = cancelDir;
    cancelRequest.forceRegenerate = true;
    cancelRequest.providerId = QStringLiteral("audit-fake");
    cancelRequest.translationModel = QStringLiteral("audit-fake-model");
    cancelRequest.sourceLanguageHint = QStringLiteral("en");
    cancelRequest.cancelRequested = std::make_shared<std::atomic_bool>(false);
    auto fake = std::make_shared<CancelProbeProvider>(cancelRequest.cancelRequested);
    AIProviderManager manager;
    manager.registerProvider(fake);
    SubtitleGenerationService service(&manager, nullptr);
    const SubtitleGenerationResult cancelResult = service.generate(cancelRequest);
    output.insert(QStringLiteral("cancellation"), QJsonObject{
        {QStringLiteral("cancelRequestedDuringFirstProviderCall"), cancelRequest.cancelRequested->load()},
        {QStringLiteral("totalProviderCalls"), fake->calls.load()},
        {QStringLiteral("providerCallsStartedAfterCancel"), fake->callsStartedAfterCancel.load()},
        {QStringLiteral("outputWritten"), QFileInfo::exists(cancelResult.translatedVttPath)},
        {QStringLiteral("result"), cancelResult.toJson()},
        {QStringLiteral("testBoundary"), QStringLiteral("real public generate entry; explicit local SRT; fake in-process provider; no network or real media decoding")}
    });


result["cancelNotSuccessful"]=cancelRequest.cancelRequested->load()&&!cancelResult.success;result["noNewRequestsAfterCancel"]=fake->calls==1&&fake->callsStartedAfterCancel==0;result["canceledGenerationWritesNoFinalOutput"]=!QFileInfo::exists(cancelResult.translatedVttPath);result["explicitWatermarkStillRejected"]=translation_text::isLikelyVisualWatermarkText(QStringLiteral("© Animation Partners"));output["checks"]=result;bool passed=true;for(auto i=result.begin();i!=result.end();++i)passed=passed&&i.value().toBool();output["allChecksPassed"]=passed;writeFile(QDir(root).filePath("details.json"),QJsonDocument(output).toJson());return finish(result);}
