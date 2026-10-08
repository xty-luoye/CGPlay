#include "AIContextBuilder.h"

#include "common/core/ServiceLocator.h"
#include "core/playback/api/IPlaybackService.h"
#include "core/session/api/ISessionService.h"
#include "features/annotation/api/IAnnotationService.h"
#include "services/media/api/IMediaService.h"
#include "ui/viewer/api/IActivePlaybackView.h"

namespace cgplay {

AIRequestContext AIContextBuilder::buildContext(
    AIRequestScope scope,
    const QJsonObject& options) const
{
    AIRequestContext context;
    context.scope = scope;
    context.metadata = options;

    if (auto* activeView = ServiceLocator::getService<IActivePlaybackView>()) {
        context.activeViewId = activeView->activeViewId();
        context.metadata.insert(QStringLiteral("hasActiveView"), activeView->hasActiveView());
    }

    QString mediaPath = options.value(QStringLiteral("mediaPath")).toString().trimmed();
    if (auto* playback = ServiceLocator::getService<IPlaybackService>()) {
        if (mediaPath.isEmpty()) {
            mediaPath = playback->currentPath();
        }
        context.currentFrame = playback->currentFrame();
        context.totalFrames = playback->totalFrames();
        context.fps = playback->fps();
        switch (scope) {
        case AIRequestScope::CurrentFrame:
            context.startFrame = context.currentFrame;
            context.endFrame = context.currentFrame;
            break;
        case AIRequestScope::CurrentSequence:
            context.startFrame = 0;
            context.endFrame = context.totalFrames > 0 ? context.totalFrames - 1 : context.currentFrame;
            break;
        default:
            context.startFrame = options.value(QStringLiteral("startFrame")).toInt(context.currentFrame);
            context.endFrame = options.value(QStringLiteral("endFrame")).toInt(context.currentFrame);
            break;
        }
    }
    context.mediaPath = mediaPath;

    if (auto* mediaService = ServiceLocator::getService<IMediaService>(); mediaService && !mediaPath.isEmpty()) {
        const MediaInfo mediaInfo = mediaService->probe(mediaPath, context.fps);
        context.mediaDisplayName = mediaService->displayNameForPath(mediaPath);
        context.mediaFormat = mediaInfo.formatLabel;
        context.mediaWidth = mediaInfo.width;
        context.mediaHeight = mediaInfo.height;
        if (context.totalFrames <= 0) {
            context.totalFrames = mediaInfo.effectiveFrameCount();
        }
        if (context.fps <= 0.0) {
            context.fps = mediaInfo.fps;
        }
    }

    const bool includeAnnotations = options.value(QStringLiteral("includeAnnotations")).toBool(true);
    if (includeAnnotations) {
        if (auto* annotationService = ServiceLocator::getService<IAnnotationService>()) {
            context.annotations = annotationService->annotations();
            context.selectedAnnotationId = annotationService->selectedAnnotationId();
        }
    }

    if (auto* sessionService = ServiceLocator::getService<ISessionService>()) {
        context.sessionPath = sessionService->sessionPath();
    }

    return context;
}

} // namespace cgplay
