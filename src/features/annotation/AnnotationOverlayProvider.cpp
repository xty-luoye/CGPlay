#include "AnnotationOverlayProvider.h"

#include "AnnotationManager.h"
#include "AnnotationOverlay.h"
#include "common/core/OverlayRuntimeDebug.h"
#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "core/playback/api/IPlaybackService.h"
#include "core/playback/api/PlaybackServiceSignals.h"
#include "viewer/api/IActivePlaybackView.h"
#include "viewer/api/IOverlayHost.h"
#include "viewer/api/IViewerCoordinateMapper.h"

#include <QElapsedTimer>
#include <QWidget>

#include <atomic>
#include <cmath>

namespace cgplay {

namespace {

QString activeRuntimeViewId()
{
    if (auto* activeView = ServiceLocator::getService<IActivePlaybackView>()) {
        return activeView->activeViewId();
    }
    return {};
}

quint64 nextOverlayInstanceId()
{
    static std::atomic<quint64> nextId{ 0 };
    return ++nextId;
}

double nsToMs(qint64 valueNs)
{
    return static_cast<double>(valueNs) / 1000000.0;
}

QJsonObject runtimeBindingDetails(
    const QString& viewId,
    quint64 overlayInstanceId,
    const IOverlayHost* overlayHost,
    const IViewerCoordinateMapper* coordinateMapper,
    const IPlaybackService* playbackService)
{
    return QJsonObject{
        { QStringLiteral("pluginName"), QStringLiteral("annotation") },
        { QStringLiteral("providerId"), QStringLiteral("annotation.overlay") },
        { QStringLiteral("viewId"), viewId },
        { QStringLiteral("overlayInstanceId"), static_cast<double>(overlayInstanceId) },
        { QStringLiteral("hasOverlayHost"), overlayHost != nullptr },
        { QStringLiteral("hasCoordinateMapper"), coordinateMapper != nullptr },
        { QStringLiteral("hasPlaybackService"), playbackService != nullptr }
    };
}

} // namespace

AnnotationOverlayProvider::AnnotationOverlayProvider(AnnotationManager* annotationManager, QObject* parent)
    : QObject(parent)
    , _annotationManager(annotationManager)
{
    _eventBus = ServiceLocator::getService<IEventBus>();
    _subscribeRuntime();
    bind(activeRuntimeViewId());
    _syncOverlayState();
    overlayDebugLog(
        QStringLiteral("AnnotationOverlayProvider"),
        QStringLiteral("constructed"),
        QJsonObject{
            { QStringLiteral("pluginName"), QStringLiteral("annotation") },
            { QStringLiteral("providerId"), QStringLiteral("annotation.overlay") },
            { QStringLiteral("initialViewId"), _boundViewId }
        });
}

AnnotationOverlayProvider::~AnnotationOverlayProvider()
{
    overlayDebugLog(
        QStringLiteral("AnnotationOverlayProvider"),
        QStringLiteral("destructing"),
        runtimeBindingDetails(_boundViewId, _overlayInstanceId, _overlayHost, _coordinateMapper, _playbackService));
    _unsubscribeRuntime();
    _disconnectPlaybackSignals();
    _destroyOverlay();
    _resetBindings();
}

void AnnotationOverlayProvider::bind(const QString& viewId)
{
    ++_bindAttemptCount;
    if (viewId.isEmpty()) {
        overlayDebugLog(
            QStringLiteral("AnnotationOverlayProvider"),
            QStringLiteral("bind.skipped"),
            QJsonObject{
                { QStringLiteral("pluginName"), QStringLiteral("annotation") },
                { QStringLiteral("providerId"), QStringLiteral("annotation.overlay") },
                { QStringLiteral("reason"), QStringLiteral("emptyViewId") }
            });
        unbind(_boundViewId);
        return;
    }

    QElapsedTimer bindTimer;
    bindTimer.start();

    auto* overlayHost = ServiceLocator::getService<IOverlayHost>();
    auto* coordinateMapper = ServiceLocator::getService<IViewerCoordinateMapper>();
    auto* playbackService = ServiceLocator::getService<IPlaybackService>();
    if (!overlayHost || !coordinateMapper || !playbackService || !_annotationManager) {
        overlayDebugLog(
            QStringLiteral("AnnotationOverlayProvider"),
            QStringLiteral("bind.skipped"),
            QJsonObject{
                { QStringLiteral("pluginName"), QStringLiteral("annotation") },
                { QStringLiteral("providerId"), QStringLiteral("annotation.overlay") },
                { QStringLiteral("viewId"), viewId },
                { QStringLiteral("hasOverlayHost"), overlayHost != nullptr },
                { QStringLiteral("hasCoordinateMapper"), coordinateMapper != nullptr },
                { QStringLiteral("hasPlaybackService"), playbackService != nullptr },
                { QStringLiteral("hasAnnotationManager"), _annotationManager != nullptr }
            });
        return;
    }

    const bool sameBinding =
        !_boundViewId.isEmpty() &&
        _boundViewId == viewId &&
        _overlayHost == overlayHost &&
        _coordinateMapper == coordinateMapper &&
        _playbackService == playbackService &&
        !_overlay.isNull();

    _overlayHost = overlayHost;
    _coordinateMapper = coordinateMapper;
    if (_playbackService != playbackService) {
        _disconnectPlaybackSignals();
        _playbackService = playbackService;
        _connectPlaybackSignals();
    }

    if (sameBinding) {
        ++_bindReuseCount;
        overlayDebugLog(
            QStringLiteral("AnnotationOverlayProvider"),
            QStringLiteral("bind.reuse"),
            runtimeBindingDetails(viewId, _overlayInstanceId, overlayHost, coordinateMapper, playbackService));
        _refreshOverlayGeometry();
        _syncOverlayState();
        _lastBindDurationNs = bindTimer.nsecsElapsed();
        _totalBindDurationNs += _lastBindDurationNs;
        return;
    }

    _destroyOverlay();
    _boundViewId = viewId;

    QElapsedTimer overlayCreateTimer;
    overlayCreateTimer.start();
    auto* overlay = new AnnotationOverlay(_annotationManager, nullptr);
    _overlay = overlay;
    ++_overlayCreateCount;
    _overlayInstanceId = nextOverlayInstanceId();
    overlay->setCoordinateMapper(_coordinateMapper);
    overlay->setProperty("cgplay.annotationDrawingMode", false);

    connect(overlay, &AnnotationOverlay::annotationCreated, this, [this](const AnnotationItem& annotation) {
        if (_onCreated) {
            _onCreated(annotation);
        } else if (_annotationManager) {
            _annotationManager->add(annotation);
        }
    });
    connect(overlay, &AnnotationOverlay::annotationSelected, this, [this](const QString& annotationId) {
        if (_onSelected) {
            _onSelected(annotationId);
        } else if (_annotationManager) {
            _annotationManager->selectAnnotation(annotationId);
        }
    });
    connect(overlay, &AnnotationOverlay::annotationDeleteRequested, this, [this](const QString& annotationId) {
        if (_onDeleteRequested) {
            _onDeleteRequested(annotationId);
        } else if (_annotationManager) {
            _annotationManager->removeAnnotation(annotationId);
        }
    });

    _overlayHost->attachOverlay(overlay);
    overlay->show();
    _refreshOverlayGeometry();
    _syncOverlayState();
    ++_bindSuccessCount;
    _lastOverlayCreateDurationNs = overlayCreateTimer.nsecsElapsed();
    _totalOverlayCreateDurationNs += _lastOverlayCreateDurationNs;
    _lastBindDurationNs = bindTimer.nsecsElapsed();
    _totalBindDurationNs += _lastBindDurationNs;
    overlayDebugLog(
        QStringLiteral("AnnotationOverlayProvider"),
        QStringLiteral("bind.attach"),
        runtimeBindingDetails(_boundViewId, _overlayInstanceId, _overlayHost, _coordinateMapper, _playbackService));
}

void AnnotationOverlayProvider::unbind(const QString& viewId)
{
    if (!_boundViewId.isEmpty() && !viewId.isEmpty() && viewId != _boundViewId) {
        overlayDebugLog(
            QStringLiteral("AnnotationOverlayProvider"),
            QStringLiteral("unbind.ignored"),
            QJsonObject{
                { QStringLiteral("pluginName"), QStringLiteral("annotation") },
                { QStringLiteral("providerId"), QStringLiteral("annotation.overlay") },
                { QStringLiteral("requestedViewId"), viewId },
                { QStringLiteral("boundViewId"), _boundViewId }
            });
        return;
    }

    ++_unbindCount;
    QElapsedTimer unbindTimer;
    unbindTimer.start();
    overlayDebugLog(
        QStringLiteral("AnnotationOverlayProvider"),
        QStringLiteral("unbind.detach"),
        runtimeBindingDetails(_boundViewId, _overlayInstanceId, _overlayHost, _coordinateMapper, _playbackService));
    _destroyOverlay();
    _resetBindings();
    _lastUnbindDurationNs = unbindTimer.nsecsElapsed();
    _totalUnbindDurationNs += _lastUnbindDurationNs;
}

void AnnotationOverlayProvider::handleActiveViewChanged(const QString& viewId)
{
    bind(viewId);
}

void AnnotationOverlayProvider::handleActiveViewInvalidated(const QString& viewId)
{
    unbind(viewId);
}

void AnnotationOverlayProvider::handleOverlayHostChanged(const QString& viewId)
{
    if (!viewId.isEmpty()) {
        bind(viewId);
    }
}

void AnnotationOverlayProvider::handleOverlayHostInvalidated(const QString& viewId)
{
    unbind(viewId);
}

void AnnotationOverlayProvider::handleCoordinateMapperChanged(const QString& viewId)
{
    if (!viewId.isEmpty() && (_boundViewId.isEmpty() || _boundViewId == viewId)) {
        bind(viewId);
    }
}

void AnnotationOverlayProvider::handleCoordinateMapperInvalidated(const QString& viewId)
{
    unbind(viewId);
}

QString AnnotationOverlayProvider::boundViewId() const
{
    return _boundViewId;
}

bool AnnotationOverlayProvider::hasOverlayBinding() const
{
    return !_overlay.isNull() && !_boundViewId.isEmpty();
}

bool AnnotationOverlayProvider::hasOverlayHostBinding() const
{
    return _overlayHost != nullptr;
}

bool AnnotationOverlayProvider::hasCoordinateMapperBinding() const
{
    return _coordinateMapper != nullptr;
}

int AnnotationOverlayProvider::overlayCount() const
{
    return _overlay ? 1 : 0;
}

quint64 AnnotationOverlayProvider::overlayInstanceId() const
{
    return _overlay ? _overlayInstanceId : 0;
}

int AnnotationOverlayProvider::currentFrame() const
{
    return _playbackService ? _playbackService->currentFrame() : 0;
}

int AnnotationOverlayProvider::mediaWidth() const
{
    return _mediaWidth;
}

int AnnotationOverlayProvider::mediaHeight() const
{
    return _mediaHeight;
}

QString AnnotationOverlayProvider::selectedAnnotationId() const
{
    if (_overlay) {
        return _overlay->selectedId();
    }
    return _annotationManager ? _annotationManager->selectedAnnotationId() : QString();
}

QJsonObject AnnotationOverlayProvider::runtimeMetrics() const
{
    const auto avgDurationMs = [](qint64 totalDurationNs, quint64 count) -> double {
        return count > 0 ? nsToMs(totalDurationNs / static_cast<qint64>(count)) : 0.0;
    };

    return QJsonObject{
        { QStringLiteral("pluginName"), QStringLiteral("annotation") },
        { QStringLiteral("providerId"), QStringLiteral("annotation.overlay") },
        { QStringLiteral("boundViewId"), _boundViewId },
        { QStringLiteral("overlayInstanceId"), static_cast<double>(overlayInstanceId()) },
        { QStringLiteral("overlayCount"), overlayCount() },
        { QStringLiteral("hasOverlayBinding"), hasOverlayBinding() },
        { QStringLiteral("hasOverlayHostBinding"), hasOverlayHostBinding() },
        { QStringLiteral("hasCoordinateMapperBinding"), hasCoordinateMapperBinding() },
        { QStringLiteral("overlayCreateCount"), static_cast<double>(_overlayCreateCount) },
        { QStringLiteral("bindAttemptCount"), static_cast<double>(_bindAttemptCount) },
        { QStringLiteral("bindSuccessCount"), static_cast<double>(_bindSuccessCount) },
        { QStringLiteral("bindReuseCount"), static_cast<double>(_bindReuseCount) },
        { QStringLiteral("unbindCount"), static_cast<double>(_unbindCount) },
        { QStringLiteral("overlayRefreshCount"), static_cast<double>(_overlayRefreshCount) },
        { QStringLiteral("playbackFrameChangedCount"), static_cast<double>(_playbackFrameChangedCount) },
        { QStringLiteral("frameSyncCount"), static_cast<double>(_frameSyncCount) },
        { QStringLiteral("frameSyncFromPlaybackCount"), static_cast<double>(_frameSyncFromPlaybackCount) },
        { QStringLiteral("overlayStateSyncCount"), static_cast<double>(_overlayStateSyncCount) },
        { QStringLiteral("annotationChangedSyncCount"), static_cast<double>(_annotationChangedSyncCount) },
        { QStringLiteral("transformEventCount"), static_cast<double>(_transformEventCount) },
        { QStringLiteral("transformRefreshCount"), static_cast<double>(_transformRefreshCount) },
        { QStringLiteral("viewportResizeEventCount"), static_cast<double>(_viewportResizeEventCount) },
        { QStringLiteral("viewportRefreshCount"), static_cast<double>(_viewportRefreshCount) },
        { QStringLiteral("lastBindDurationMs"), nsToMs(_lastBindDurationNs) },
        { QStringLiteral("lastUnbindDurationMs"), nsToMs(_lastUnbindDurationNs) },
        { QStringLiteral("lastOverlayCreateDurationMs"), nsToMs(_lastOverlayCreateDurationNs) },
        { QStringLiteral("avgBindDurationMs"), avgDurationMs(_totalBindDurationNs, _bindSuccessCount + _bindReuseCount) },
        { QStringLiteral("avgUnbindDurationMs"), avgDurationMs(_totalUnbindDurationNs, _unbindCount) },
        { QStringLiteral("avgOverlayCreateDurationMs"), avgDurationMs(_totalOverlayCreateDurationNs, _overlayCreateCount) },
        { QStringLiteral("totalBindDurationMs"), nsToMs(_totalBindDurationNs) },
        { QStringLiteral("totalUnbindDurationMs"), nsToMs(_totalUnbindDurationNs) },
        { QStringLiteral("totalOverlayCreateDurationMs"), nsToMs(_totalOverlayCreateDurationNs) }
    };
}

void AnnotationOverlayProvider::setInteractionHandlers(
    std::function<void(const AnnotationItem&)> onCreated,
    std::function<void(const QString&)> onSelected,
    std::function<void(const QString&)> onDeleteRequested)
{
    _onCreated = std::move(onCreated);
    _onSelected = std::move(onSelected);
    _onDeleteRequested = std::move(onDeleteRequested);
}

bool AnnotationOverlayProvider::hasBridgeBinding() const
{
    return !_boundViewId.isEmpty() &&
        _playbackService != nullptr &&
        _overlayHost != nullptr &&
        _coordinateMapper != nullptr;
}

void AnnotationOverlayProvider::_clearOverlayState()
{
    if (_overlay) {
        _overlay->setAnnotations({});
        _overlay->setSelectedId(QString());
        _overlay->setCurrentFrame(0);
    }
}

void AnnotationOverlayProvider::_subscribeRuntime()
{
    if (!_eventBus) {
        return;
    }

    _subscriptions.push_back(
        _eventBus->subscribe<ActiveViewChangedEvent>([this](const ActiveViewChangedEvent& event) {
            handleActiveViewChanged(event.viewId);
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<ActiveViewInvalidatedEvent>([this](const ActiveViewInvalidatedEvent& event) {
            handleActiveViewInvalidated(event.viewId);
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<OverlayHostChangedEvent>([this](const OverlayHostChangedEvent& event) {
            handleOverlayHostChanged(event.viewId);
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<OverlayHostInvalidatedEvent>([this](const OverlayHostInvalidatedEvent& event) {
            handleOverlayHostInvalidated(event.viewId);
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<CoordinateMapperChangedEvent>([this](const CoordinateMapperChangedEvent& event) {
            handleCoordinateMapperChanged(event.viewId);
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<CoordinateMapperInvalidatedEvent>([this](const CoordinateMapperInvalidatedEvent& event) {
            handleCoordinateMapperInvalidated(event.viewId);
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<ViewTransformChangedEvent>([this](const ViewTransformChangedEvent& event) {
            if (event.viewId == _boundViewId) {
                ++_transformEventCount;
                ++_transformRefreshCount;
                _refreshOverlayGeometry();
            }
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<ViewportResizedEvent>([this](const ViewportResizedEvent& event) {
            if (event.viewId == _boundViewId) {
                ++_viewportResizeEventCount;
                ++_viewportRefreshCount;
                _refreshOverlayGeometry();
            }
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<AnnotationChangedEvent>([this](const AnnotationChangedEvent&) {
            ++_annotationChangedSyncCount;
            _syncOverlayState();
        }));
}

void AnnotationOverlayProvider::_unsubscribeRuntime()
{
    if (!_eventBus) {
        return;
    }

    for (const auto subscriptionId : _subscriptions) {
        _eventBus->unsubscribe<ActiveViewChangedEvent>(subscriptionId);
        _eventBus->unsubscribe<ActiveViewInvalidatedEvent>(subscriptionId);
        _eventBus->unsubscribe<OverlayHostChangedEvent>(subscriptionId);
        _eventBus->unsubscribe<OverlayHostInvalidatedEvent>(subscriptionId);
        _eventBus->unsubscribe<CoordinateMapperChangedEvent>(subscriptionId);
        _eventBus->unsubscribe<CoordinateMapperInvalidatedEvent>(subscriptionId);
        _eventBus->unsubscribe<ViewTransformChangedEvent>(subscriptionId);
        _eventBus->unsubscribe<ViewportResizedEvent>(subscriptionId);
        _eventBus->unsubscribe<AnnotationChangedEvent>(subscriptionId);
    }
    _subscriptions.clear();
}

void AnnotationOverlayProvider::_connectPlaybackSignals()
{
    _disconnectPlaybackSignals();
    if (!_playbackService || !_playbackService->signalProxy()) {
        return;
    }

    auto* playbackSignals = _playbackService->signalProxy();
    _playbackConnections.push_back(connect(playbackSignals, &PlaybackServiceSignals::fileOpened, this, [this](const QString&) {
        _syncOverlayState();
    }));
    _playbackConnections.push_back(connect(playbackSignals, &PlaybackServiceSignals::fileClosed, this, [this] {
        _clearOverlayState();
    }));
    _playbackConnections.push_back(connect(playbackSignals, &PlaybackServiceSignals::currentFrameChanged, this, [this](int, int) {
        ++_playbackFrameChangedCount;
        ++_frameSyncFromPlaybackCount;
        _syncFrameState(currentFrame());
    }));
}

void AnnotationOverlayProvider::_disconnectPlaybackSignals()
{
    for (const auto& connection : _playbackConnections) {
        disconnect(connection);
    }
    _playbackConnections.clear();
}

void AnnotationOverlayProvider::_refreshOverlayGeometry()
{
    if (!_overlay || !_overlayHost || !_coordinateMapper) {
        return;
    }

    ++_overlayRefreshCount;
    _overlay->setCoordinateMapper(_coordinateMapper);
    _overlayHost->requestOverlayRefresh();
    overlayDebugLog(
        QStringLiteral("AnnotationOverlayProvider"),
        QStringLiteral("overlay.refreshGeometry"),
        runtimeBindingDetails(_boundViewId, _overlayInstanceId, _overlayHost, _coordinateMapper, _playbackService));
}

void AnnotationOverlayProvider::_syncFrameState(int frame)
{
    if (!_overlay || !_annotationManager || !_coordinateMapper) {
        return;
    }

    ++_frameSyncCount;
    _updateMediaSizeFromMapper();
    _overlay->setMediaSize(_mediaWidth, _mediaHeight);
    _overlay->setCurrentFrame(frame);
    _overlay->setAnnotations(_annotationManager->getAtFrame(frame));
}

void AnnotationOverlayProvider::_syncSelectionState()
{
    if (_overlay && _annotationManager) {
        _overlay->setSelectedId(_annotationManager->selectedAnnotationId());
    }
}

void AnnotationOverlayProvider::_syncToolState()
{
    if (!_overlay || !_annotationManager) {
        return;
    }

    const int tool = _annotationManager->currentTool();
    _overlay->setToolMode(static_cast<AnnotationOverlay::ToolMode>(tool));
    _overlay->setToolColor(_annotationManager->currentToolColor());
    const bool drawingMode = tool != static_cast<int>(AnnotationOverlay::ToolMode::Select);
    if (_overlay->property("cgplay.annotationDrawingMode").toBool() != drawingMode) {
        _overlay->setProperty("cgplay.annotationDrawingMode", drawingMode);
    }
}

void AnnotationOverlayProvider::_syncOverlayState()
{
    if (!_overlay || !_annotationManager || !_coordinateMapper) {
        return;
    }

    ++_overlayStateSyncCount;
    _syncFrameState(currentFrame());
    _syncSelectionState();
    _syncToolState();
}

void AnnotationOverlayProvider::_resetBindings()
{
    _boundViewId.clear();
    _overlayHost = nullptr;
    _coordinateMapper = nullptr;
    _playbackService = nullptr;
    _disconnectPlaybackSignals();
    _overlayInstanceId = 0;
}

void AnnotationOverlayProvider::_destroyOverlay()
{
    if (_overlay) {
        overlayDebugLog(
            QStringLiteral("AnnotationOverlayProvider"),
            QStringLiteral("overlay.destroy"),
            runtimeBindingDetails(_boundViewId, _overlayInstanceId, _overlayHost, _coordinateMapper, _playbackService));
        if (_overlayHost) {
            _overlayHost->detachOverlay(_overlay);
        } else {
            _overlay->setParent(nullptr);
        }
        delete _overlay;
        _overlay.clear();
    }
}

void AnnotationOverlayProvider::_updateMediaSizeFromMapper()
{
    if (!_coordinateMapper) {
        return;
    }

    const double zoom = _coordinateMapper->zoom();
    const QRectF visible = _coordinateMapper->visibleVideoRect();
    if (zoom <= 0.0 || visible.width() <= 0.0 || visible.height() <= 0.0) {
        return;
    }

    const int inferredWidth = static_cast<int>(std::round(visible.width() / zoom));
    const int inferredHeight = static_cast<int>(std::round(visible.height() / zoom));
    if (inferredWidth > 0) {
        _mediaWidth = inferredWidth;
    }
    if (inferredHeight > 0) {
        _mediaHeight = inferredHeight;
    }
}

} // namespace cgplay
