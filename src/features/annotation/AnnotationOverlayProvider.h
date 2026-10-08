#pragma once

#include "annotation/api/IAnnotationViewBridge.h"
#include "common/events/api/IEventBus.h"
#include "viewer/api/IOverlayProvider.h"

#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <functional>
#include <vector>

namespace cgplay {

class AnnotationManager;
class AnnotationOverlay;
class IOverlayHost;
class IPlaybackService;
class IViewerCoordinateMapper;

class AnnotationOverlayProvider : public QObject,
                                  public IOverlayProvider,
                                  public IAnnotationViewBridge
{
    Q_OBJECT
    Q_INTERFACES(cgplay::IOverlayProvider cgplay::IAnnotationViewBridge)
public:
    explicit AnnotationOverlayProvider(AnnotationManager* annotationManager, QObject* parent = nullptr);
    ~AnnotationOverlayProvider() override;

    void bind(const QString& viewId) override;
    void unbind(const QString& viewId) override;
    void handleActiveViewChanged(const QString& viewId) override;
    void handleActiveViewInvalidated(const QString& viewId) override;
    void handleOverlayHostChanged(const QString& viewId) override;
    void handleOverlayHostInvalidated(const QString& viewId) override;
    void handleCoordinateMapperChanged(const QString& viewId) override;
    void handleCoordinateMapperInvalidated(const QString& viewId) override;

    QString boundViewId() const override;
    bool hasOverlayBinding() const override;
    bool hasOverlayHostBinding() const override;
    bool hasCoordinateMapperBinding() const override;
    int overlayCount() const override;
    quint64 overlayInstanceId() const override;

    int currentFrame() const override;
    int mediaWidth() const override;
    int mediaHeight() const override;
    QString selectedAnnotationId() const override;
    Q_INVOKABLE QJsonObject runtimeMetrics() const;
    void setInteractionHandlers(
        std::function<void(const AnnotationItem&)> onCreated,
        std::function<void(const QString&)> onSelected,
        std::function<void(const QString&)> onDeleteRequested) override;

    bool hasBridgeBinding() const;

private:
    void _subscribeRuntime();
    void _unsubscribeRuntime();
    void _connectPlaybackSignals();
    void _disconnectPlaybackSignals();
    void _clearOverlayState();
    void _refreshOverlayGeometry();
    void _syncFrameState(int frame);
    void _syncSelectionState();
    void _syncToolState();
    void _syncOverlayState();
    void _resetBindings();
    void _destroyOverlay();
    void _updateMediaSizeFromMapper();

    QPointer<AnnotationManager> _annotationManager;
    IEventBus* _eventBus = nullptr;
    IPlaybackService* _playbackService = nullptr;
    IOverlayHost* _overlayHost = nullptr;
    IViewerCoordinateMapper* _coordinateMapper = nullptr;
    QPointer<AnnotationOverlay> _overlay;
    QString _boundViewId;
    quint64 _overlayInstanceId = 0;
    int _mediaWidth = 1920;
    int _mediaHeight = 1080;
    quint64 _overlayCreateCount = 0;
    quint64 _bindAttemptCount = 0;
    quint64 _bindSuccessCount = 0;
    quint64 _bindReuseCount = 0;
    quint64 _unbindCount = 0;
    quint64 _overlayRefreshCount = 0;
    quint64 _playbackFrameChangedCount = 0;
    quint64 _frameSyncCount = 0;
    quint64 _frameSyncFromPlaybackCount = 0;
    quint64 _overlayStateSyncCount = 0;
    quint64 _annotationChangedSyncCount = 0;
    quint64 _transformEventCount = 0;
    quint64 _transformRefreshCount = 0;
    quint64 _viewportResizeEventCount = 0;
    quint64 _viewportRefreshCount = 0;
    qint64 _lastBindDurationNs = 0;
    qint64 _lastUnbindDurationNs = 0;
    qint64 _lastOverlayCreateDurationNs = 0;
    qint64 _totalBindDurationNs = 0;
    qint64 _totalUnbindDurationNs = 0;
    qint64 _totalOverlayCreateDurationNs = 0;
    std::vector<IEventBus::SubscriptionId> _subscriptions;
    QVector<QMetaObject::Connection> _playbackConnections;
    std::function<void(const AnnotationItem&)> _onCreated;
    std::function<void(const QString&)> _onSelected;
    std::function<void(const QString&)> _onDeleteRequested;
};

} // namespace cgplay
