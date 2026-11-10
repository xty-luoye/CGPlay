#include "AIWorkflowService.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "services/ai/api/IAIContextBuilder.h"
#include "services/ai/api/IAIProviderManager.h"
#include "services/ai/api/IMediaFrameSnapshotService.h"
#include "features/annotation/api/IAnnotationService.h"

#include <QRegularExpression>

namespace cgplay {

AIWorkflowService::AIWorkflowService(
    IAIProviderManager* providerManager,
    IAIContextBuilder* contextBuilder,
    IMediaFrameSnapshotService* frameSnapshotService,
    IEventBus* eventBus)
    : _providerManager(providerManager)
    , _contextBuilder(contextBuilder)
    , _frameSnapshotService(frameSnapshotService)
    , _eventBus(eventBus)
{
}

QString AIWorkflowService::submit(const AIWorkflowRequest& request)
{
    auto* providerManager = _providerManager
        ? _providerManager
        : ServiceLocator::getService<IAIProviderManager>();
    auto* contextBuilder = _contextBuilder
        ? _contextBuilder
        : ServiceLocator::getService<IAIContextBuilder>();
    auto* frameSnapshotService = _frameSnapshotService
        ? _frameSnapshotService
        : ServiceLocator::getService<IMediaFrameSnapshotService>();

    if (!providerManager || !contextBuilder) {
        return {};
    }

    AIRequest aiRequest;
    aiRequest.providerId = request.providerId.trimmed();
    aiRequest.model = request.model.trimmed();
    aiRequest.systemPrompt = request.systemPrompt;
    aiRequest.userPrompt = request.userPrompt;
    aiRequest.chatHistory = request.chatHistory;
    aiRequest.options = request.options;
    aiRequest.options.remove(QStringLiteral("rangeCaptureJob"));
    aiRequest.options.insert(QStringLiteral("workflow"), static_cast<int>(request.workflow));
    aiRequest.options.insert(QStringLiteral("includeAnnotations"), request.includeAnnotations);
    aiRequest.context = contextBuilder->buildContext(request.scope, aiRequest.options);
    if (!request.includeAnnotations) {
        aiRequest.context.annotations.clear();
        aiRequest.context.selectedAnnotationId.clear();
    }

    if (request.attachFrames && frameSnapshotService) {
        AIFrameCaptureRequest captureRequest;
        captureRequest.scope = request.scope;
        captureRequest.mediaPath = aiRequest.context.mediaPath;
        captureRequest.currentFrame = aiRequest.context.currentFrame;
        captureRequest.startFrame = aiRequest.context.startFrame;
        captureRequest.endFrame = aiRequest.context.endFrame;
        captureRequest.sampleCount = request.sampleCount;
        captureRequest.targetSize = request.targetSize;
        const QString frameEncoding =
            request.options.value(QStringLiteral("frameEncoding")).toString().trimmed();
        if (!frameEncoding.isEmpty()) {
            captureRequest.encoding = frameEncoding;
        }
        captureRequest.options = request.options;

        QVector<AIMediaFrameReference> frames;
        QString captureError;
        const bool rangeRequest = request.scope == AIRequestScope::CurrentShot ||
            request.scope == AIRequestScope::CurrentSequence;
        if (rangeRequest) {
            auto* preparation = dynamic_cast<IMediaFrameRangePreparation*>(frameSnapshotService);
            if (!preparation || !preparation->prepareRangeCapture(captureRequest, &aiRequest, &captureError))
                return {};
        } else if (frameSnapshotService->capture(captureRequest, &frames, &captureError)) {
            aiRequest.frames = frames;
        } else if (!captureError.isEmpty()) {
            aiRequest.options.insert(QStringLiteral("frameCaptureError"), captureError);
        }
        if (!rangeRequest && request.requireFrames && (aiRequest.frames.isEmpty() ||
                                      !aiRequest.options.value(QStringLiteral("frameCaptureError")).toString().isEmpty())) {
            // A visual workflow with mandatory frames must fail before it can
            // reach a provider. Sending a text-only request here would make a
            // QC/scan job appear successful without the required evidence.
            return {};
        }
    } else if (request.attachFrames && !frameSnapshotService) {
        aiRequest.options.insert(QStringLiteral("frameCaptureError"),
            QStringLiteral("IMediaFrameSnapshotService 不可用"));
        if (request.requireFrames) {
            return {};
        }
    }

    const QString jobId = providerManager->submit(aiRequest);
    if (_eventBus && !jobId.isEmpty()) {
        _eventBus->publish(AIWorkflowStartedEvent{ jobId, request });
    }
    return jobId;
}

bool AIWorkflowService::cancel(const QString& jobId)
{
    auto* providerManager = _providerManager
        ? _providerManager
        : ServiceLocator::getService<IAIProviderManager>();
    return providerManager ? providerManager->cancel(jobId) : false;
}

AIJobSnapshot AIWorkflowService::jobSnapshot(const QString& jobId) const
{
    auto* providerManager = _providerManager
        ? _providerManager
        : ServiceLocator::getService<IAIProviderManager>();
    return providerManager ? providerManager->jobSnapshot(jobId) : AIJobSnapshot{};
}

QVector<AISearchResult> AIWorkflowService::searchAnnotations(const QString& query) const
{
    QVector<AISearchResult> results;
    const QString normalizedQuery = query.trimmed().toLower();
    if (normalizedQuery.isEmpty()) {
        return results;
    }

    auto* annotationService = ServiceLocator::getService<IAnnotationService>();
    if (!annotationService) {
        return results;
    }

    QStringList keywords = normalizedQuery.split(
        QRegularExpression(QStringLiteral("[\\s,，、]+")),
        Qt::SkipEmptyParts);

    bool filterOpen = false;
    for (auto it = keywords.begin(); it != keywords.end();) {
        if (*it == QStringLiteral("未解决") || *it == QStringLiteral("open") ||
            *it == QStringLiteral("待处理")) {
            filterOpen = true;
            it = keywords.erase(it);
        } else {
            ++it;
        }
    }

    const QVector<AnnotationItem> annotations = annotationService->annotations();
    for (const auto& ann : annotations) {
        bool match = keywords.isEmpty();
        QString snippet;

        const QString comment = ann.comment.toLower();
        const QString latestComment = ann.latestCommentText().toLower();

        for (const auto& kw : keywords) {
            if (comment.contains(kw) || latestComment.contains(kw)) {
                match = true;
                snippet = ann.latestCommentText();
                break;
            }
        }

        if (filterOpen && ann.status != ReviewStatus::Open) {
            continue;
        }

        if (!match) {
            continue;
        }

        AISearchResult result;
        result.frame = ann.frame;
        result.source = QStringLiteral("annotation");
        result.title = annotationTypeString(ann.type);
        result.snippet = snippet.isEmpty() ? ann.latestCommentText() : snippet;
        result.annotationId = ann.id;
        if (ann.status == ReviewStatus::Open) {
            result.severity = AISeverity::Major;
        } else if (ann.status == ReviewStatus::InProgress) {
            result.severity = AISeverity::Minor;
        } else {
            result.severity = AISeverity::Info;
        }
        results.append(result);
    }

    std::sort(results.begin(), results.end(),
        [](const AISearchResult& a, const AISearchResult& b) {
            return a.frame < b.frame;
        });

    return results;
}

} // namespace cgplay
