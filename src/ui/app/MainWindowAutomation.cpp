#include "Application.h"
#include "ApplicationPrivate.h"

#include "AIAgentWorkspace.h"
#include "annotation/AnnotationItem.h"
#include "annotation/AnnotationManager.h"
#include "annotation/AnnotationOverlayProvider.h"
#include "annotation/api/IAnnotationService.h"
#include "annotation/api/IAnnotationViewBridge.h"
#include "annotation/AnnotationToolbar.h"
#include "annotation/ReviewPanel.h"
#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "plugins/PluginManager.h"
#include "plugins/annotation/AnnotationPlugin.h"
#include "playback/PlaybackController.h"
#include "playlist/PlaylistModel.h"
#include "playlist/PlaylistPanel.h"
#include "NavigationRail.h"
#include "PlaybackBar.h"
#include "SecondaryWindow.h"
#include "services/ai/SubtitleGenerationService.h"
#include "services/ai/TranslationGuardSkills.h"
#include "services/ai/TranslationPlaybackStrategy.h"
#include "services/ai/api/IAIProviderManager.h"
#include "settings/api/ISettingsService.h"
#include "viewer/CompareToolbar.h"
#include "viewer/TlViewport.h"
#include "viewer/api/IActivePlaybackView.h"
#include "viewer/api/IOverlayProvider.h"
#include "viewer/ViewerWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QSet>
#include <QSplitter>
#include <QThread>
#include <QToolButton>
#include <QTextStream>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace cgplay {

namespace automation {

bool isPluginFallbackDisabled()
{
    return qApp && qApp->property("cgplay.disablePluginFallback").toBool();
}

void sendKeyPress(QWidget* widget, int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    if (!widget) {
        return;
    }
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QApplication::sendEvent(widget, &release);
}

bool pumpUntil(const std::function<bool()>& condition, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (condition()) {
            return true;
        }
    }
    return condition();
}

QString smokeVttTime(double seconds)
{
    const qint64 totalMs = std::max<qint64>(0, qRound64(seconds * 1000.0));
    const qint64 ms = totalMs % 1000;
    const qint64 totalSeconds = totalMs / 1000;
    const qint64 s = totalSeconds % 60;
    const qint64 totalMinutes = totalSeconds / 60;
    const qint64 m = totalMinutes % 60;
    const qint64 h = totalMinutes / 60;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

bool writeSmokeSubtitleVtt(const QString& path, const QVector<GeneratedSubtitleCue>& cues)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "WEBVTT\n\n";
    for (const auto& cue : cues) {
        if (cue.translatedText.trimmed().isEmpty()) {
            continue;
        }
        stream << smokeVttTime(cue.startSeconds) << " --> "
               << smokeVttTime(cue.endSeconds) << "\n"
               << cue.translatedText.trimmed() << "\n\n";
    }
    return true;
}

bool smokeCueHasDisplayableTranslation(const GeneratedSubtitleCue& cue)
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
        static const QStringList exactFillers = {
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

bool activateViewForRuntime(Application* app, const QString& viewId)
{
    return app && !viewId.isEmpty() && app->activateViewForRuntime(viewId);
}

class LifecycleRecorder
{
public:
    explicit LifecycleRecorder(QObject* owner)
        : _owner(owner)
    {
        _timer.start();
        _eventBus = ServiceLocator::getService<IEventBus>();
        if (_eventBus) {
            _activeViewChangedSubscription = _eventBus->subscribe<ActiveViewChangedEvent>(
                [this](const ActiveViewChangedEvent& event) {
                    append(QStringLiteral("activeViewChanged"), event.viewId);
                });
            _activeViewInvalidatedSubscription = _eventBus->subscribe<ActiveViewInvalidatedEvent>(
                [this](const ActiveViewInvalidatedEvent& event) {
                    append(QStringLiteral("activeViewInvalidated"), event.viewId);
                });
            _overlayHostChangedSubscription = _eventBus->subscribe<OverlayHostChangedEvent>(
                [this](const OverlayHostChangedEvent& event) {
                    append(QStringLiteral("overlayHostChanged"), event.viewId);
                });
            _overlayHostInvalidatedSubscription = _eventBus->subscribe<OverlayHostInvalidatedEvent>(
                [this](const OverlayHostInvalidatedEvent& event) {
                    append(QStringLiteral("overlayHostInvalidated"), event.viewId);
                });
            _coordinateMapperChangedSubscription = _eventBus->subscribe<CoordinateMapperChangedEvent>(
                [this](const CoordinateMapperChangedEvent& event) {
                    append(QStringLiteral("coordinateMapperChanged"), event.viewId);
                });
            _coordinateMapperInvalidatedSubscription = _eventBus->subscribe<CoordinateMapperInvalidatedEvent>(
                [this](const CoordinateMapperInvalidatedEvent& event) {
                    append(QStringLiteral("coordinateMapperInvalidated"), event.viewId);
                });
            _transformChangedSubscription = _eventBus->subscribe<ViewTransformChangedEvent>(
                [this](const ViewTransformChangedEvent& event) {
                    append(QStringLiteral("transformChanged"), event.viewId);
                });
            _viewportResizedSubscription = _eventBus->subscribe<ViewportResizedEvent>(
                [this](const ViewportResizedEvent& event) {
                    append(QStringLiteral("viewportResized"), event.viewId);
                });
        }
    }

    ~LifecycleRecorder()
    {
        for (const auto& connection : _connections) {
            QObject::disconnect(connection);
        }
        if (_eventBus) {
            _eventBus->unsubscribe<ActiveViewChangedEvent>(_activeViewChangedSubscription);
            _eventBus->unsubscribe<ActiveViewInvalidatedEvent>(_activeViewInvalidatedSubscription);
            _eventBus->unsubscribe<OverlayHostChangedEvent>(_overlayHostChangedSubscription);
            _eventBus->unsubscribe<OverlayHostInvalidatedEvent>(_overlayHostInvalidatedSubscription);
            _eventBus->unsubscribe<CoordinateMapperChangedEvent>(_coordinateMapperChangedSubscription);
            _eventBus->unsubscribe<CoordinateMapperInvalidatedEvent>(_coordinateMapperInvalidatedSubscription);
            _eventBus->unsubscribe<ViewTransformChangedEvent>(_transformChangedSubscription);
            _eventBus->unsubscribe<ViewportResizedEvent>(_viewportResizedSubscription);
        }
    }

    void attachViewer(ViewerWidget* viewer)
    {
        if (!viewer || !_owner) {
            return;
        }
        _connections.push_back(QObject::connect(
            viewer, &ViewerWidget::viewActivated, _owner, [this](const QString& viewId) {
                append(QStringLiteral("viewActivated"), viewId);
            }));
        _connections.push_back(QObject::connect(
            viewer, &ViewerWidget::viewInvalidated, _owner, [this](const QString& viewId) {
                append(QStringLiteral("viewInvalidated"), viewId);
            }));
    }

    void append(const QString& name, const QString& viewId)
    {
        QJsonObject item;
        item.insert(QStringLiteral("index"), ++_index);
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("viewId"), viewId);
        item.insert(QStringLiteral("elapsedMs"), static_cast<double>(_timer.elapsed()));
        _events.append(item);
    }

    int findFirst(const QString& name, const QString& viewId) const
    {
        for (qsizetype i = 0; i < _events.size(); ++i) {
            const QJsonObject item = _events.at(i).toObject();
            if (item.value(QStringLiteral("name")).toString() == name &&
                item.value(QStringLiteral("viewId")).toString() == viewId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int findFirstAfter(const QString& name, const QString& viewId, int afterIndex) const
    {
        for (qsizetype i = 0; i < _events.size(); ++i) {
            if (static_cast<int>(i) <= afterIndex) {
                continue;
            }
            const QJsonObject item = _events.at(i).toObject();
            if (item.value(QStringLiteral("name")).toString() == name &&
                item.value(QStringLiteral("viewId")).toString() == viewId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    QJsonArray events() const
    {
        return _events;
    }

    qint64 elapsedMs() const
    {
        return _timer.elapsed();
    }

    qint64 eventElapsedAt(int index) const
    {
        if (index < 0 || index >= _events.size()) {
            return -1;
        }
        return static_cast<qint64>(_events.at(index).toObject().value(QStringLiteral("elapsedMs")).toDouble(-1.0));
    }

private:
    QObject* _owner = nullptr;
    IEventBus* _eventBus = nullptr;
    QElapsedTimer _timer;
    QJsonArray _events;
    int _index = 0;
    QVector<QMetaObject::Connection> _connections;
    IEventBus::SubscriptionId _activeViewChangedSubscription = 0;
    IEventBus::SubscriptionId _activeViewInvalidatedSubscription = 0;
    IEventBus::SubscriptionId _overlayHostChangedSubscription = 0;
    IEventBus::SubscriptionId _overlayHostInvalidatedSubscription = 0;
    IEventBus::SubscriptionId _coordinateMapperChangedSubscription = 0;
    IEventBus::SubscriptionId _coordinateMapperInvalidatedSubscription = 0;
    IEventBus::SubscriptionId _transformChangedSubscription = 0;
    IEventBus::SubscriptionId _viewportResizedSubscription = 0;
};

QJsonObject makeSummary(const QJsonArray& assertions)
{
    int passCount = 0;
    int failCount = 0;
    int skipCount = 0;
    for (const auto& value : assertions) {
        const QString status = value.toObject().value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("PASS")) {
            ++passCount;
        } else if (status == QStringLiteral("FAIL")) {
            ++failCount;
        } else if (status == QStringLiteral("SKIP")) {
            ++skipCount;
        }
    }
    return QJsonObject{
        { QStringLiteral("pass"), passCount },
        { QStringLiteral("fail"), failCount },
        { QStringLiteral("skip"), skipCount }
    };
}

QJsonObject overlayMetricsObject(const QJsonObject& state)
{
    return state.value(QStringLiteral("overlay_metrics")).toObject();
}

QJsonObject diffOverlayMetrics(const QJsonObject& before, const QJsonObject& after)
{
    static const QStringList keys = {
        QStringLiteral("overlayCreateCount"),
        QStringLiteral("bindAttemptCount"),
        QStringLiteral("bindSuccessCount"),
        QStringLiteral("bindReuseCount"),
        QStringLiteral("unbindCount"),
        QStringLiteral("overlayRefreshCount"),
        QStringLiteral("playbackFrameChangedCount"),
        QStringLiteral("frameSyncCount"),
        QStringLiteral("frameSyncFromPlaybackCount"),
        QStringLiteral("overlayStateSyncCount"),
        QStringLiteral("annotationChangedSyncCount"),
        QStringLiteral("transformEventCount"),
        QStringLiteral("transformRefreshCount"),
        QStringLiteral("viewportResizeEventCount"),
        QStringLiteral("viewportRefreshCount")
    };

    QJsonObject delta;
    for (const QString& key : keys) {
        delta.insert(key, after.value(key).toDouble() - before.value(key).toDouble());
    }
    return delta;
}

double jsonNumber(const QJsonObject& object, const QString& key, double fallback = 0.0)
{
    return object.value(key).toDouble(fallback);
}

int jsonInt(const QJsonObject& object, const QString& key, int fallback = 0)
{
    return object.value(key).toInt(fallback);
}

#ifdef Q_OS_WIN
double currentWorkingSetMiB()
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return 0.0;
    }
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
}
#else
double currentWorkingSetMiB()
{
    return 0.0;
}
#endif

struct Phase14LoopStats
{
    int requested = 0;
    int completed = 0;
    int pass = 0;
    int fail = 0;
    double totalLatencyMs = 0.0;
    double minLatencyMs = std::numeric_limits<double>::max();
    double maxLatencyMs = 0.0;
    double startWorkingSetMiB = 0.0;
    double endWorkingSetMiB = 0.0;
    double maxWorkingSetMiB = 0.0;
    int maxOverlayCount = 0;
    int maxSubscriptionCount = 0;

    void recordLatency(double latencyMs)
    {
        totalLatencyMs += latencyMs;
        if (latencyMs < minLatencyMs) {
            minLatencyMs = latencyMs;
        }
        if (latencyMs > maxLatencyMs) {
            maxLatencyMs = latencyMs;
        }
    }

    void sampleState(const QJsonObject& state)
    {
        const double workingSetMiB = currentWorkingSetMiB();
        if (workingSetMiB > maxWorkingSetMiB) {
            maxWorkingSetMiB = workingSetMiB;
        }
        maxOverlayCount = std::max(maxOverlayCount, state.value(QStringLiteral("overlay_count")).toInt());
        maxSubscriptionCount = std::max(maxSubscriptionCount, state.value(QStringLiteral("plugin_subscription_count")).toInt());
    }

    QJsonObject metrics() const
    {
        const double avgLatencyMs = completed > 0 ? totalLatencyMs / static_cast<double>(completed) : 0.0;
        return QJsonObject{
            { QStringLiteral("iterationsRequested"), requested },
            { QStringLiteral("iterationsCompleted"), completed },
            { QStringLiteral("passCount"), pass },
            { QStringLiteral("failCount"), fail },
            { QStringLiteral("avgLatencyMs"), avgLatencyMs },
            { QStringLiteral("minLatencyMs"), completed > 0 ? minLatencyMs : 0.0 },
            { QStringLiteral("maxLatencyMs"), maxLatencyMs },
            { QStringLiteral("startWorkingSetMiB"), startWorkingSetMiB },
            { QStringLiteral("endWorkingSetMiB"), endWorkingSetMiB },
            { QStringLiteral("maxWorkingSetMiB"), maxWorkingSetMiB },
            { QStringLiteral("workingSetGrowthMiB"), endWorkingSetMiB - startWorkingSetMiB },
            { QStringLiteral("maxOverlayCount"), maxOverlayCount },
            { QStringLiteral("maxSubscriptionCount"), maxSubscriptionCount }
        };
    }
};

SecondaryWindow* findSecondaryWindow(const QString& excludeViewId = {})
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* secondary = qobject_cast<SecondaryWindow*>(widget);
        if (!secondary || !secondary->viewerWidget()) {
            continue;
        }
        if (!excludeViewId.isEmpty() && secondary->viewerWidget()->viewId() == excludeViewId) {
            continue;
        }
        return secondary;
    }
    return nullptr;
}

} // namespace automation

using namespace automation;

QJsonObject MainWindow::captureRuntimeAnnotationState() const
{
    QJsonObject state;

    auto* pluginManager = ServiceLocator::getService<PluginManager>();
    auto* annotationService = _p->annotationService;
    if (!annotationService && !isPluginFallbackDisabled() && _annoMgr) {
        annotationService = static_cast<IAnnotationService*>(_annoMgr.get());
    }

    state.insert(QStringLiteral("fallback_enabled"), !isPluginFallbackDisabled());
    state.insert(QStringLiteral("annotation_service_present"), annotationService != nullptr);
    state.insert(QStringLiteral("annotation_toolbar_present"), _p->annoToolbar != nullptr);
    state.insert(QStringLiteral("annotation_toolbar_visible"), _p->annoToolbar && _p->annoToolbar->isVisible());
    state.insert(QStringLiteral("review_panel_present"), _p->reviewPanel != nullptr);
    state.insert(QStringLiteral("review_panel_visible"), _p->reviewPanel && _p->reviewPanel->isVisible());

    QJsonArray reviewPanels;
    for (QWidget* widget : qApp->allWidgets()) {
        auto* panel = qobject_cast<ReviewPanel*>(widget);
        if (!panel) {
            continue;
        }
        QJsonObject item;
        item.insert(QStringLiteral("objectName"), panel->objectName());
        item.insert(QStringLiteral("visible"), panel->isVisible());
        item.insert(QStringLiteral("width"), panel->width());
        item.insert(QStringLiteral("height"), panel->height());
        item.insert(QStringLiteral("x"), panel->x());
        item.insert(QStringLiteral("y"), panel->y());
        const QPoint globalPos = panel->mapToGlobal(QPoint(0, 0));
        item.insert(QStringLiteral("globalX"), globalPos.x());
        item.insert(QStringLiteral("globalY"), globalPos.y());
        item.insert(QStringLiteral("isPrimary"), panel == _p->reviewPanel);
        item.insert(QStringLiteral("hasToolbarAncestor"),
                    _p->annoToolbar ? panel->isAncestorOf(_p->annoToolbar) : false);
        item.insert(QStringLiteral("belongsToMainWindow"), this->isAncestorOf(panel));
        item.insert(QStringLiteral("sameWindow"), panel->window() == this);
        if (QObject* parent = panel->parent()) {
            item.insert(QStringLiteral("parentClass"), parent->metaObject()->className());
            item.insert(QStringLiteral("parentObjectName"), parent->objectName());
        }
        const QList<QTabWidget*> tabs = panel->findChildren<QTabWidget*>();
        item.insert(QStringLiteral("tabWidgetCount"), tabs.size());
        reviewPanels.append(item);
    }
    state.insert(QStringLiteral("review_panels"), reviewPanels);

    if (_p->viewer) {
        state.insert(QStringLiteral("viewer_view_id"), _p->viewer->viewId());
    } else {
        state.insert(QStringLiteral("viewer_view_id"), QString());
    }

    if (auto* overlayProvider = ServiceLocator::getService<IOverlayProvider>()) {
        state.insert(QStringLiteral("overlay_count"), overlayProvider->overlayCount());
        state.insert(QStringLiteral("overlay_instance_id"),
                     static_cast<double>(overlayProvider->overlayInstanceId()));
        if (auto* runtimeProvider = dynamic_cast<AnnotationOverlayProvider*>(overlayProvider)) {
            state.insert(QStringLiteral("overlay_metrics"), runtimeProvider->runtimeMetrics());
        } else if (QObject* overlayProviderObject = dynamic_cast<QObject*>(overlayProvider)) {
            QJsonObject overlayMetrics;
            if (QMetaObject::invokeMethod(
                    overlayProviderObject,
                    "runtimeMetrics",
                    Q_RETURN_ARG(QJsonObject, overlayMetrics))) {
                state.insert(QStringLiteral("overlay_metrics"), overlayMetrics);
            }
        }
    } else {
        state.insert(QStringLiteral("overlay_count"), 0);
        state.insert(QStringLiteral("overlay_instance_id"), 0.0);
        state.insert(QStringLiteral("overlay_metrics"), QJsonObject());
    }

    if (auto* activeView = ServiceLocator::getService<IActivePlaybackView>()) {
        state.insert(QStringLiteral("active_view_id"), activeView->activeViewId());
    } else {
        state.insert(QStringLiteral("active_view_id"), QString());
    }

    if (annotationService) {
        state.insert(QStringLiteral("annotation_count"), annotationService->count());
        state.insert(QStringLiteral("selected_annotation_id"), annotationService->selectedAnnotationId());
        state.insert(QStringLiteral("annotation_tool"), annotationService->currentTool());
        state.insert(QStringLiteral("annotation_color"), annotationService->currentToolColor().name());
    } else {
        state.insert(QStringLiteral("annotation_count"), 0);
        state.insert(QStringLiteral("selected_annotation_id"), QString());
    }

    if (_annoMgr) {
        state.insert(QStringLiteral("can_undo"), _annoMgr->canUndo());
        state.insert(QStringLiteral("can_redo"), _annoMgr->canRedo());
    } else {
        state.insert(QStringLiteral("can_undo"), false);
        state.insert(QStringLiteral("can_redo"), false);
    }

    if (auto* bridge = ServiceLocator::getService<IAnnotationViewBridge>()) {
        state.insert(QStringLiteral("bridge_present"), true);
        state.insert(QStringLiteral("bridge_current_frame"), bridge->currentFrame());
        state.insert(QStringLiteral("bridge_selected_annotation_id"), bridge->selectedAnnotationId());
    } else {
        state.insert(QStringLiteral("bridge_present"), false);
    }

    const bool pluginLoaded = pluginManager && pluginManager->hasPlugin(QStringLiteral("annotation"));
    state.insert(QStringLiteral("annotation_plugin_loaded"), pluginLoaded);
    state.insert(QStringLiteral("loaded_dynamic_plugins"),
                 pluginManager ? QJsonArray::fromStringList(pluginManager->loadedDynamicPluginIds()) : QJsonArray());

    QObject* annotationPluginObject =
        pluginManager ? pluginManager->pluginObject(QStringLiteral("annotation")) : nullptr;
    if (annotationPluginObject) {
        QString boundViewId;
        bool hasBridgeBinding = false;
        bool hasOverlayHostBinding = false;
        bool hasCoordinateMapperBinding = false;
        int subscriptionCount = 0;
        QMetaObject::invokeMethod(annotationPluginObject, "runtimeBoundViewId",
                                  Q_RETURN_ARG(QString, boundViewId));
        QMetaObject::invokeMethod(annotationPluginObject, "runtimeHasBridgeBinding",
                                  Q_RETURN_ARG(bool, hasBridgeBinding));
        QMetaObject::invokeMethod(annotationPluginObject, "runtimeHasOverlayHostBinding",
                                  Q_RETURN_ARG(bool, hasOverlayHostBinding));
        QMetaObject::invokeMethod(annotationPluginObject, "runtimeHasCoordinateMapperBinding",
                                  Q_RETURN_ARG(bool, hasCoordinateMapperBinding));
        QMetaObject::invokeMethod(annotationPluginObject, "runtimeSubscriptionCount",
                                  Q_RETURN_ARG(int, subscriptionCount));

        state.insert(QStringLiteral("plugin_bound_view_id"), boundViewId);
        state.insert(QStringLiteral("plugin_has_bridge_binding"), hasBridgeBinding);
        state.insert(QStringLiteral("plugin_has_overlay_host_binding"), hasOverlayHostBinding);
        state.insert(QStringLiteral("plugin_has_coordinate_mapper_binding"), hasCoordinateMapperBinding);
        state.insert(QStringLiteral("plugin_subscription_count"), subscriptionCount);
    } else {
        state.insert(QStringLiteral("plugin_bound_view_id"), QString());
        state.insert(QStringLiteral("plugin_has_bridge_binding"), false);
        state.insert(QStringLiteral("plugin_has_overlay_host_binding"), false);
        state.insert(QStringLiteral("plugin_has_coordinate_mapper_binding"), false);
        state.insert(QStringLiteral("plugin_subscription_count"), 0);
    }

    return state;
}

QJsonObject MainWindow::runPlayerSmokeChecks(const QString& mediaPath)
{
    QJsonObject report;
    QJsonArray results;
    auto addResult = [&results](const QString& name, const QString& status, const QString& message, const QJsonObject& details = {}) {
        QJsonObject item;
        item["name"] = name;
        item["status"] = status;
        item["message"] = message;
        item["details"] = details;
        results.append(item);
    };

    auto resolveAnnotationService = [this]() -> IAnnotationService* {
        if (_p->annotationService) {
            return _p->annotationService;
        }
        if (!isPluginFallbackDisabled() && _annoMgr) {
            return static_cast<IAnnotationService*>(_annoMgr.get());
        }
        return nullptr;
    };

    QJsonArray lifecycleEvents;
    int lifecycleEventIndex = 0;
    auto appendLifecycleEvent = [&lifecycleEvents, &lifecycleEventIndex](const QString& name, const QString& viewId) {
        QJsonObject entry;
        entry["index"] = ++lifecycleEventIndex;
        entry["name"] = name;
        entry["viewId"] = viewId;
        lifecycleEvents.append(entry);
    };

    QVector<QMetaObject::Connection> lifecycleSignalConnections;
    auto attachViewerLifecycleProbe = [this, &appendLifecycleEvent, &lifecycleSignalConnections](ViewerWidget* viewer) {
        if (!viewer) {
            return;
        }
        lifecycleSignalConnections.push_back(
            QObject::connect(viewer, &ViewerWidget::viewActivated, this, [&appendLifecycleEvent](const QString& viewId) {
                appendLifecycleEvent(QStringLiteral("viewActivated"), viewId);
            }));
        lifecycleSignalConnections.push_back(
            QObject::connect(viewer, &ViewerWidget::viewInvalidated, this, [&appendLifecycleEvent](const QString& viewId) {
                appendLifecycleEvent(QStringLiteral("viewInvalidated"), viewId);
            }));
    };

    attachViewerLifecycleProbe(_p->viewer);

    auto* eventBus = ServiceLocator::getService<IEventBus>();
    IEventBus::SubscriptionId activeViewChangedSubscription = 0;
    IEventBus::SubscriptionId activeViewInvalidatedSubscription = 0;
    IEventBus::SubscriptionId overlayHostChangedSubscription = 0;
    IEventBus::SubscriptionId overlayHostInvalidatedSubscription = 0;
    IEventBus::SubscriptionId coordinateMapperChangedSubscription = 0;
    IEventBus::SubscriptionId coordinateMapperInvalidatedSubscription = 0;
    IEventBus::SubscriptionId transformChangedSubscription = 0;
    IEventBus::SubscriptionId viewportResizedSubscription = 0;
    if (eventBus) {
        activeViewChangedSubscription = eventBus->subscribe<ActiveViewChangedEvent>(
            [&appendLifecycleEvent](const ActiveViewChangedEvent& event) {
                appendLifecycleEvent(QStringLiteral("activeViewChanged"), event.viewId);
            });
        activeViewInvalidatedSubscription = eventBus->subscribe<ActiveViewInvalidatedEvent>(
            [&appendLifecycleEvent](const ActiveViewInvalidatedEvent& event) {
                appendLifecycleEvent(QStringLiteral("activeViewInvalidated"), event.viewId);
            });
        overlayHostChangedSubscription = eventBus->subscribe<OverlayHostChangedEvent>(
            [&appendLifecycleEvent](const OverlayHostChangedEvent& event) {
                appendLifecycleEvent(QStringLiteral("overlayHostChanged"), event.viewId);
            });
        overlayHostInvalidatedSubscription = eventBus->subscribe<OverlayHostInvalidatedEvent>(
            [&appendLifecycleEvent](const OverlayHostInvalidatedEvent& event) {
                appendLifecycleEvent(QStringLiteral("overlayHostInvalidated"), event.viewId);
            });
        coordinateMapperChangedSubscription = eventBus->subscribe<CoordinateMapperChangedEvent>(
            [&appendLifecycleEvent](const CoordinateMapperChangedEvent& event) {
                appendLifecycleEvent(QStringLiteral("coordinateMapperChanged"), event.viewId);
            });
        coordinateMapperInvalidatedSubscription = eventBus->subscribe<CoordinateMapperInvalidatedEvent>(
            [&appendLifecycleEvent](const CoordinateMapperInvalidatedEvent& event) {
                appendLifecycleEvent(QStringLiteral("coordinateMapperInvalidated"), event.viewId);
            });
        transformChangedSubscription = eventBus->subscribe<ViewTransformChangedEvent>(
            [&appendLifecycleEvent](const ViewTransformChangedEvent& event) {
                appendLifecycleEvent(QStringLiteral("transformChanged"), event.viewId);
            });
        viewportResizedSubscription = eventBus->subscribe<ViewportResizedEvent>(
            [&appendLifecycleEvent](const ViewportResizedEvent& event) {
                appendLifecycleEvent(QStringLiteral("viewportResized"), event.viewId);
            });
    }

    auto capturePlayer = [this, &resolveAnnotationService]() {
        QJsonObject state;
        if (_p->playbackCtrl) {
            state["valid"] = _p->playbackCtrl->isValid();
            state["frame"] = _p->playbackCtrl->currentFrame();
            state["total"] = _p->playbackCtrl->totalFrames();
            state["fps"] = _p->playbackCtrl->fps();
            state["range_first_frame"] = 0;
#if CGPLAY_HAS_TLRENDER
            if (const auto player = _p->playbackCtrl->player()) {
                state["range_first_frame"] = static_cast<int>(player->getTimeRange().start_time().value());
            }
#endif
            state["playback_state"] = _p->playbackCtrl->playbackState();
            state["muted"] = _p->playbackCtrl->isMuted();
            state["volume"] = _p->playbackCtrl->getVolume();
            state["in_point"] = _p->playbackCtrl->inPoint();
            state["out_point"] = _p->playbackCtrl->outPoint();
            state["has_compare"] = _p->playbackCtrl->hasCompare();
        }
        if (_p->viewer) {
            state["zoom"] = _p->viewer->viewport() ? _p->viewer->viewport()->zoom() : 0.0;
            state["frame_view"] = _p->viewer->viewport() ? _p->viewer->viewport()->hasFrameView() : false;
            state["timecode_visible"] = _p->viewer->isTimecodeVisible();
        }
        if (_p->playlist && _p->playlist->model()) {
            state["playlist_count"] = _p->playlist->model()->shotCount();
            state["playlist_index"] = _p->playlist->model()->currentIndex();
        }
        if (_p->compareBar) {
            state["compare_mode"] = _p->compareBar->compareMode();
            state["auto_clear_b"] = _p->compareBar->isAutoClearB();
        }
        if (auto* annotationService = resolveAnnotationService()) {
            state["annotation_service_present"] = true;
            state["anno_tool"] = annotationService->currentTool();
            state["anno_color"] = annotationService->currentToolColor().name();
            state["selected_annotation_id"] = annotationService->selectedAnnotationId();
        } else {
            state["annotation_service_present"] = false;
        }
        if (_annoMgr) {
            state["annotation_count"] = _annoMgr->count();
            state["can_undo"] = _annoMgr->canUndo();
            state["can_redo"] = _annoMgr->canRedo();
        }
        state["annotation_toolbar_present"] = _p->annoToolbar != nullptr;
        state["anno_visible"] = _p->annoToolbar && _p->annoToolbar->isVisible();
        if (auto* activeView = ServiceLocator::getService<IActivePlaybackView>()) {
            state["active_view_id"] = activeView->activeViewId();
        }
        if (auto* bridge = ServiceLocator::getService<IAnnotationViewBridge>()) {
            state["bridge_present"] = true;
            state["bridge_current_frame"] = bridge->currentFrame();
            state["bridge_selected_annotation_id"] = bridge->selectedAnnotationId();
        } else {
            state["bridge_present"] = false;
        }
        state["fullscreen"] = isFullScreen();
        return state;
    };

    if (mediaPath.isEmpty()) {
        addResult(QStringLiteral("media path"), QStringLiteral("FAIL"), QStringLiteral("empty media path"));
    } else {
        results.append(_runMediaOpenSmokeCheck(mediaPath));
    }

    const QJsonObject baseline = capturePlayer();

    if (_p->playbackCtrl) {
        _p->playbackCtrl->setMute(false);
        _p->playbackCtrl->setVolume(1.0f);
        _p->playbackCtrl->pause();
        _p->playbackCtrl->seekToFrame(0);
    }
    if (_p->viewer) {
        _p->viewer->setTimecodeVisible(false);
    }
    if (_p->compareBar) {
        _p->compareBar->setCompareMode(0);
        _p->compareBar->setAutoClearB(false);
    }
    if (auto* annotationService = resolveAnnotationService()) {
        annotationService->clearAnnotations();
        annotationService->selectAnnotation(QString());
        annotationService->setTool(AnnotationToolbar::Select);
        annotationService->setToolColor(QColor(255, 0, 0));
    } else if (_annoMgr) {
        _annoMgr->clear();
    }
    if (_p->playlist && _p->playlist->model()) {
        _p->playlist->model()->setCurrentIndex(_p->playlist->model()->shotCount() > 0 ? 0 : -1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);

    auto runKey = [&](const QString& name, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const QJsonObject before = capturePlayer();
        QElapsedTimer dispatchTimer;
        dispatchTimer.start();
        sendKeyPress(this, key, mods);
        const qint64 dispatchLatencyMs = dispatchTimer.elapsed();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        QThread::msleep(120);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        const QJsonObject after = capturePlayer();

        QJsonObject details;
        details["before"] = before;
        details["after"] = after;
        details["dispatchLatencyMs"] = dispatchLatencyMs;

        bool passed = true;
        QString message;
        if (key == Qt::Key_Space) {
            passed = before.value("playback_state").toInt() != after.value("playback_state").toInt();
            message = QStringLiteral("playback toggled");
        } else if (key == Qt::Key_M) {
            passed = before.value("muted").toBool() != after.value("muted").toBool();
            message = QStringLiteral("mute toggled");
        } else if (key == Qt::Key_F11) {
            passed = before.value("fullscreen").toBool() != after.value("fullscreen").toBool();
            message = QStringLiteral("fullscreen toggled");
        } else if (key == Qt::Key_F) {
            const double beforeZoom = before.value("zoom").toDouble();
            const double afterZoom = after.value("zoom").toDouble();
            passed = qAbs(afterZoom - beforeZoom) < 0.0001 || after.value("frame_view").toBool();
            message = QStringLiteral("fit to window");
        } else if (key == Qt::Key_1) {
            passed = after.value("zoom").toDouble() >= 0.95 && after.value("zoom").toDouble() <= 1.05;
            message = QStringLiteral("1:1 zoom");
        } else if (key == Qt::Key_Plus) {
            passed = after.value("zoom").toDouble() > before.value("zoom").toDouble() &&
                !after.value("frame_view").toBool();
            message = QStringLiteral("zoom increased and auto-fit disabled");
        } else if (key == Qt::Key_Minus) {
            passed = after.value("zoom").toDouble() < before.value("zoom").toDouble() &&
                !after.value("frame_view").toBool();
            message = QStringLiteral("zoom decreased and auto-fit disabled");
        } else if (key == Qt::Key_I) {
            if (mods & Qt::AltModifier) {
                passed = after.value("in_point").toInt() < 0;
                message = QStringLiteral("clear in point");
            } else {
                passed = after.value("in_point").toInt() >= 0;
                message = QStringLiteral("set in point");
            }
        } else if (key == Qt::Key_O) {
            if (mods & Qt::AltModifier) {
                passed = after.value("out_point").toInt() < 0;
                message = QStringLiteral("clear out point");
            } else {
                passed = after.value("out_point").toInt() >= 0;
                message = QStringLiteral("set out point");
            }
        } else if (key == Qt::Key_Left) {
            const int total = std::max(1, before.value("total").toInt());
            const int first = before.value("range_first_frame").toInt();
            const int step = (mods & Qt::ShiftModifier) ? 10 : 1;
            const int expected = first + ((before.value("frame").toInt() - first - step) % total + total) % total;
            passed = after.value("frame").toInt() == expected;
            message = (mods & Qt::ShiftModifier) ? QStringLiteral("back 10 frames") : QStringLiteral("prev frame");
        } else if (key == Qt::Key_Right) {
            const int total = std::max(1, before.value("total").toInt());
            const int first = before.value("range_first_frame").toInt();
            const int step = (mods & Qt::ShiftModifier) ? 10 : 1;
            const int expected = first + (before.value("frame").toInt() - first + step) % total;
            passed = after.value("frame").toInt() == expected;
            message = (mods & Qt::ShiftModifier) ? QStringLiteral("forward 10 frames") : QStringLiteral("next frame");
        } else if (key == Qt::Key_J) {
            passed = after.value("playback_state").toInt() == 2;
            message = QStringLiteral("reverse");
        } else if (key == Qt::Key_K) {
            passed = after.value("playback_state").toInt() == 0;
            message = QStringLiteral("stop");
        } else if (key == Qt::Key_L) {
            passed = after.value("playback_state").toInt() == 1;
            message = QStringLiteral("forward");
        } else if (key == Qt::Key_Home) {
            passed = after.value("frame").toInt() == before.value("range_first_frame").toInt();
            message = QStringLiteral("goto start");
        } else if (key == Qt::Key_End) {
            passed = after.value("frame").toInt() >= before.value("frame").toInt();
            message = QStringLiteral("goto end");
        } else if (key == Qt::Key_Delete) {
            passed = after.value("annotation_count").toInt() <= before.value("annotation_count").toInt() - 1;
            message = QStringLiteral("annotation delete");
        } else {
            message = QStringLiteral("no rule");
        }

        addResult(name, passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"), message, details);
    };

    if (_p->horzSplitter && _p->horzSplitter->count() >= 3) {
        const bool originalLeftVisible = _p->leftVisible;
        const bool originalRightVisible = _p->rightVisible;
        _p->leftVisible = true;
        _p->rightVisible = _p->reviewPanel != nullptr;
        _applyAdaptiveSidePanelLayout(true);
        const QList<int> originalSizes = _p->horzSplitter->sizes();
        QJsonObject details;
        bool allResponsive = true;
        bool restored = false;
        for (const int panelIndex : {1, _p->horzSplitter->count() - 1}) {
            if (panelIndex <= 0 || panelIndex >= originalSizes.size() || originalSizes[panelIndex] <= 0) continue;
            QList<int> collapsed = originalSizes;
            const int releasedWidth = collapsed[panelIndex];
            collapsed[panelIndex] = 0;
            collapsed[2] += releasedWidth;
            QElapsedTimer timer;
            timer.start();
            _p->sidePanelLayoutApplying = true;
            _p->horzSplitter->setSizes(collapsed);
            _p->sidePanelLayoutApplying = false;
            details[panelIndex == 1 ? QStringLiteral("leftCollapseMs") : QStringLiteral("rightCollapseMs")] = timer.elapsed();
            allResponsive = allResponsive && timer.elapsed() <= 80;
            timer.restart();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            details[panelIndex == 1 ? QStringLiteral("leftCollapseEventDrainMs") : QStringLiteral("rightCollapseEventDrainMs")] = timer.elapsed();
            timer.restart();
            _p->sidePanelLayoutApplying = true;
            _p->horzSplitter->setSizes(originalSizes);
            _p->sidePanelLayoutApplying = false;
            details[panelIndex == 1 ? QStringLiteral("leftRestoreMs") : QStringLiteral("rightRestoreMs")] = timer.elapsed();
            allResponsive = allResponsive && timer.elapsed() <= 80;
            timer.restart();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            details[panelIndex == 1 ? QStringLiteral("leftRestoreEventDrainMs") : QStringLiteral("rightRestoreEventDrainMs")] = timer.elapsed();
        }
        restored = _p->horzSplitter->sizes() == originalSizes;

        QJsonArray mouseDragSamples;
        bool mouseDragResponsive = true;
        const auto runHandleDrag = [&](int handleIndex, int panelIndex, int deltaX, const QString& side) {
            auto* handle = _p->horzSplitter->handle(handleIndex);
            if (!handle || panelIndex < 0 || panelIndex >= originalSizes.size()) {
                mouseDragResponsive = false;
                return;
            }

            _p->sidePanelLayoutApplying = true;
            _p->horzSplitter->setSizes(originalSizes);
            _p->sidePanelLayoutApplying = false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

            const QPoint startLocal = handle->rect().center();
            const QPoint startGlobal = handle->mapToGlobal(startLocal);
            const auto sendMouse = [&](QEvent::Type type, const QPoint& globalPos,
                                       Qt::MouseButton button, Qt::MouseButtons buttons) {
                const QPointF localPos(handle->mapFromGlobal(globalPos));
                QMouseEvent event(type, localPos, QPointF(globalPos), button, buttons, Qt::NoModifier);
                QElapsedTimer eventTimer;
                eventTimer.start();
                QApplication::sendEvent(handle, &event);
                return eventTimer.elapsed();
            };

            sendMouse(QEvent::MouseButtonPress, startGlobal, Qt::LeftButton, Qt::LeftButton);
            const qint64 moveMs = sendMouse(QEvent::MouseMove,
                                            startGlobal + QPoint(deltaX, 0),
                                            Qt::NoButton, Qt::LeftButton);
            const QList<int> movedSizes = _p->horzSplitter->sizes();
            const int initialWidth = originalSizes[panelIndex];
            const int movedWidth = movedSizes.value(panelIndex, initialWidth);
            const bool narrowed = movedWidth < initialWidth;

            sendMouse(QEvent::MouseMove, startGlobal, Qt::NoButton, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, startGlobal, Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            const int restoredWidth = _p->horzSplitter->sizes().value(panelIndex, -1);
            const bool returned = std::abs(restoredWidth - initialWidth) <= 2;
            // A native splitter move may repaint the viewer and both side panels;
            // keep the physical drag budget at 33ms (about 30 FPS) while the
            // synchronous layout path above remains checked at the stricter 80ms.
            mouseDragResponsive = mouseDragResponsive && narrowed && returned && moveMs <= 33;
            mouseDragSamples.append(QJsonObject{
                {QStringLiteral("side"), side},
                {QStringLiteral("moveDispatchMs"), moveMs},
                {QStringLiteral("initialWidth"), initialWidth},
                {QStringLiteral("movedWidth"), movedWidth},
                {QStringLiteral("restoredWidth"), restoredWidth},
                {QStringLiteral("narrowed"), narrowed},
                {QStringLiteral("returned"), returned}});
        };
        runHandleDrag(2, 1, -48, QStringLiteral("left"));
        if (_p->reviewPanel && _p->horzSplitter->count() > 3) {
            runHandleDrag(_p->horzSplitter->count() - 1,
                          _p->horzSplitter->count() - 1,
                          48, QStringLiteral("right"));
        }

        _p->leftVisible = originalLeftVisible;
        _p->rightVisible = originalRightVisible;
        _applyAdaptiveSidePanelLayout(true);
        details["restored"] = restored;
        details["handleWidth"] = _p->horzSplitter->handleWidth();
        addResult(QStringLiteral("Native side-panel splitter latency"),
                  allResponsive && restored ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                  QStringLiteral("collapse/restore layout path"), details);
        addResult(QStringLiteral("Native side-panel mouse drag"),
                  mouseDragResponsive ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                  QStringLiteral("QSplitterHandle press/move/release path"),
                  QJsonObject{{QStringLiteral("samples"), mouseDragSamples}});
    }

    auto runNoFocusCheck = [&](const QString& name, QWidget* target) {
        if (!target) {
            addResult(name, QStringLiteral("SKIP"), QStringLiteral("target missing"));
            return;
        }

        QJsonObject details;
        details["target_class"] = target->metaObject()->className();
        details["target_focus_policy"] = static_cast<int>(target->focusPolicy());
        details["visible"] = target->isVisible();
        details["visible_to_window"] = target->isVisibleTo(this);

        const bool passed = target->focusPolicy() == Qt::NoFocus;
        addResult(name,
                  passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                  QStringLiteral("player chrome should not capture keyboard focus"),
                  details);
    };

    if (_p->playbackCtrl && _p->playbackCtrl->isValid()) {
        QVector<QPair<int, QString>> descriptorShortcutSnapshot;
        QHash<QAction*, QKeySequence> actionShortcutSnapshot;
        for (int index = 0; index < _p->commandDescriptors.size(); ++index) {
            auto& descriptor = _p->commandDescriptors[index];
            descriptorShortcutSnapshot.push_back({index, descriptor.shortcut});
            descriptor.shortcut = _p->commandDefaultShortcuts.value(descriptor.id, descriptor.shortcut);
            for (QAction* action : findChildren<QAction*>(descriptor.id)) {
                if (!action) continue;
                actionShortcutSnapshot.insert(action, action->shortcut());
                action->setShortcut(QKeySequence(descriptor.shortcut));
            }
        }

        runKey(QStringLiteral("Space"), Qt::Key_Space);
        runKey(QStringLiteral("M"), Qt::Key_M);
        runKey(QStringLiteral("F11"), Qt::Key_F11);
        runKey(QStringLiteral("F"), Qt::Key_F);
        runKey(QStringLiteral("1"), Qt::Key_1);
        runKey(QStringLiteral("+"), Qt::Key_Plus, Qt::ShiftModifier);
        runKey(QStringLiteral("-"), Qt::Key_Minus);
        _p->playbackCtrl->pause();
        runKey(QStringLiteral("I"), Qt::Key_I);
        runKey(QStringLiteral("O"), Qt::Key_O);
        runKey(QStringLiteral("Left"), Qt::Key_Left);
        runKey(QStringLiteral("Right"), Qt::Key_Right);
        runKey(QStringLiteral("Shift+Left"), Qt::Key_Left, Qt::ShiftModifier);
        runKey(QStringLiteral("Shift+Right"), Qt::Key_Right, Qt::ShiftModifier);
        runKey(QStringLiteral("J"), Qt::Key_J);
        runKey(QStringLiteral("K"), Qt::Key_K);
        runKey(QStringLiteral("L"), Qt::Key_L);
        runKey(QStringLiteral("Home"), Qt::Key_Home);
        runKey(QStringLiteral("End"), Qt::Key_End);
        runKey(QStringLiteral("Alt+I"), Qt::Key_I, Qt::AltModifier);
        runKey(QStringLiteral("Alt+O"), Qt::Key_O, Qt::AltModifier);

        if (_p->viewer && _p->viewer->viewport() && _p->userSettings) {
            const QString wheelKey = QStringLiteral("input/mouseWheel");
            const bool hadWheelSetting = _p->userSettings->contains(wheelKey);
            const QVariant originalWheelSetting = _p->userSettings->value(wheelKey);
            _p->userSettings->setValue(wheelKey, QStringLiteral("zoom"));

            _p->playbackCtrl->pause();
            _p->playbackCtrl->seekToFrame(0);
            _p->viewer->setZoom(1.0);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            QThread::msleep(100);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            auto* viewport = _p->viewer->viewport();
            const QPointF localPos(viewport->width() / 2.0, viewport->height() / 2.0);
            const QPointF globalPos(viewport->mapToGlobal(localPos.toPoint()));

            const QJsonObject beforeWheelIn = capturePlayer();
            QWheelEvent wheelIn(
                localPos, globalPos, QPoint(), QPoint(0, 120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &wheelIn);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const QJsonObject afterWheelIn = capturePlayer();
            addResult(
                QStringLiteral("Viewer wheel zoom in"),
                afterWheelIn.value("zoom").toDouble() > beforeWheelIn.value("zoom").toDouble() &&
                        afterWheelIn.value("frame").toInt() == beforeWheelIn.value("frame").toInt() &&
                        !afterWheelIn.value("frame_view").toBool()
                    ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                QStringLiteral("wheel increases zoom without stepping frames"),
                QJsonObject{{QStringLiteral("before"), beforeWheelIn}, {QStringLiteral("after"), afterWheelIn}});

            const QJsonObject beforeWheelOut = capturePlayer();
            QWheelEvent wheelOut(
                localPos, globalPos, QPoint(), QPoint(0, -120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &wheelOut);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const QJsonObject afterWheelOut = capturePlayer();
            addResult(
                QStringLiteral("Viewer wheel zoom out"),
                afterWheelOut.value("zoom").toDouble() < beforeWheelOut.value("zoom").toDouble() &&
                        afterWheelOut.value("frame").toInt() == beforeWheelOut.value("frame").toInt()
                    ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                QStringLiteral("wheel decreases zoom without stepping frames"),
                QJsonObject{{QStringLiteral("before"), beforeWheelOut}, {QStringLiteral("after"), afterWheelOut}});

            const int originalWheelFrame = _p->playbackCtrl->currentFrame();
            const float originalWheelVolume = _p->playbackCtrl->getVolume();
            _p->userSettings->setValue(wheelKey, QStringLiteral("frames"));
            _p->playbackCtrl->pause();
            _p->playbackCtrl->seekToFrame(0);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const QJsonObject beforePixelFrames = capturePlayer();
            QWheelEvent pixelFrames(
                localPos, globalPos, QPoint(0, 120), QPoint(),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &pixelFrames);
            for (int i = 0; i < 40 &&
                 _p->playbackCtrl->currentFrame() <= beforePixelFrames.value("frame").toInt(); ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(25);
            }
            const QJsonObject afterPixelFrames = capturePlayer();
            addResult(
                QStringLiteral("Viewer pixel wheel frames"),
                afterPixelFrames.value("frame").toInt() > beforePixelFrames.value("frame").toInt() &&
                        qAbs(afterPixelFrames.value("zoom").toDouble() - beforePixelFrames.value("zoom").toDouble()) < 0.0001
                    ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                QStringLiteral("pixel-only wheel steps frames without zooming"),
                QJsonObject{{QStringLiteral("before"), beforePixelFrames}, {QStringLiteral("after"), afterPixelFrames}});

            _p->userSettings->setValue(wheelKey, QStringLiteral("volume"));
            _p->playbackCtrl->setVolume(0.5f);
            const QJsonObject beforePixelVolume = capturePlayer();
            QWheelEvent pixelVolume(
                localPos, globalPos, QPoint(0, 120), QPoint(),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, true);
            QApplication::sendEvent(viewport, &pixelVolume);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const QJsonObject afterPixelVolume = capturePlayer();
            addResult(
                QStringLiteral("Viewer pixel wheel volume"),
                afterPixelVolume.value("volume").toDouble() < beforePixelVolume.value("volume").toDouble() &&
                        qAbs(afterPixelVolume.value("zoom").toDouble() - beforePixelVolume.value("zoom").toDouble()) < 0.0001
                    ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                QStringLiteral("inverted pixel-only wheel changes volume without zooming"),
                QJsonObject{{QStringLiteral("before"), beforePixelVolume}, {QStringLiteral("after"), afterPixelVolume}});

            _p->userSettings->setValue(wheelKey, QStringLiteral("none"));
            const QJsonObject beforeWheelNone = capturePlayer();
            QWheelEvent wheelNone(
                localPos, globalPos, QPoint(), QPoint(0, 120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &wheelNone);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const QJsonObject afterWheelNone = capturePlayer();
            addResult(
                QStringLiteral("Viewer wheel none"),
                afterWheelNone.value("frame").toInt() == beforeWheelNone.value("frame").toInt() &&
                        qAbs(afterWheelNone.value("volume").toDouble() - beforeWheelNone.value("volume").toDouble()) < 0.0001 &&
                        qAbs(afterWheelNone.value("zoom").toDouble() - beforeWheelNone.value("zoom").toDouble()) < 0.0001
                    ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                QStringLiteral("disabled wheel mode consumes input without changing player state"),
                QJsonObject{{QStringLiteral("before"), beforeWheelNone}, {QStringLiteral("after"), afterWheelNone}});

            _p->playbackCtrl->setVolume(originalWheelVolume);
            _p->playbackCtrl->seekToFrame(originalWheelFrame);

            if (hadWheelSetting) _p->userSettings->setValue(wheelKey, originalWheelSetting);
            else _p->userSettings->remove(wheelKey);
            _p->userSettings->sync();
        }

        for (auto& descriptor : _p->commandDescriptors) {
            if (descriptor.id != QStringLiteral("playback.toggle")) continue;
            const QString originalShortcut = descriptor.shortcut;
            QVector<QPair<QAction*, QKeySequence>> originalActions;
            for (QAction* action : findChildren<QAction*>(descriptor.id)) {
                if (!action) continue;
                originalActions.push_back({action, action->shortcut()});
                action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+P")));
            }
            descriptor.shortcut = QStringLiteral("Ctrl+Alt+P");
            _p->playbackCtrl->pause();
            const int beforeOldKey = capturePlayer().value(QStringLiteral("playback_state")).toInt();
            sendKeyPress(this, Qt::Key_Space);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const int afterOldKey = capturePlayer().value(QStringLiteral("playback_state")).toInt();
            addResult(QStringLiteral("Shortcut remap clears old Space"),
                      beforeOldKey == afterOldKey ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                      QStringLiteral("old shortcut must stop dispatching"));
            sendKeyPress(this, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const int afterNewKey = capturePlayer().value(QStringLiteral("playback_state")).toInt();
            addResult(QStringLiteral("Shortcut remap activates new key"),
                      afterNewKey != afterOldKey ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                      QStringLiteral("new shortcut dispatches command ID"));
            descriptor.shortcut = originalShortcut;
            for (const auto& action : originalActions) action.first->setShortcut(action.second);
            _p->playbackCtrl->pause();
            break;
        }

        for (const auto& snapshot : descriptorShortcutSnapshot) {
            if (snapshot.first >= 0 && snapshot.first < _p->commandDescriptors.size()) {
                _p->commandDescriptors[snapshot.first].shortcut = snapshot.second;
            }
        }
        for (auto it = actionShortcutSnapshot.cbegin(); it != actionShortcutSnapshot.cend(); ++it) {
            if (it.key()) it.key()->setShortcut(it.value());
        }

        if (_p->playbackBar) {
            runNoFocusCheck(QStringLiteral("Playback play button focus"), _p->playbackBar->playBtn());
            if (auto* volumeSlider = _p->playbackBar->findChild<QSlider*>()) {
                runNoFocusCheck(QStringLiteral("Playback volume slider focus"), volumeSlider);
            }
        }
        if (_p->navRail && _p->navRail->btn(0)) {
            runNoFocusCheck(QStringLiteral("Navigation rail focus"), _p->navRail->btn(0));
        }
        if (_p->compareBar) {
            if (auto* compareButton = _p->compareBar->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
                runNoFocusCheck(QStringLiteral("Compare toolbar button focus"), compareButton);
            }
        }
    }

    if (_p->compareBar) {
        const auto runCompareKey = [&](const QString& name, int key, int expectedMode) {
            const QJsonObject before = capturePlayer();
            sendKeyPress(_p->compareBar, key);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            const QJsonObject after = capturePlayer();
            const bool ok = after.value("compare_mode").toInt() == expectedMode;
            addResult(name, ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                      QStringLiteral("compare mode check"),
                      QJsonObject{ { "before", before }, { "after", after }, { "expected_mode", expectedMode } });
        };
        runCompareKey(QStringLiteral("Compare A"), Qt::Key_A, 0);
        runCompareKey(QStringLiteral("Compare B"), Qt::Key_B, 1);
        runCompareKey(QStringLiteral("Compare W"), Qt::Key_W, 2);
        runCompareKey(QStringLiteral("Compare N"), Qt::Key_N, 3);
        runCompareKey(QStringLiteral("Compare D"), Qt::Key_D, 4);
        runCompareKey(QStringLiteral("Compare H"), Qt::Key_H, 5);
        runCompareKey(QStringLiteral("Compare V"), Qt::Key_V, 6);
        runCompareKey(QStringLiteral("Compare T"), Qt::Key_T, 7);

        const bool beforeAutoClear = _p->compareBar->isAutoClearB();
        sendKeyPress(this, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const bool afterAutoClear = _p->compareBar->isAutoClearB();
        addResult(QStringLiteral("Ctrl+Shift+C"), beforeAutoClear != afterAutoClear ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                  QStringLiteral("toggle auto clear B"),
                  QJsonObject{ { "before", beforeAutoClear }, { "after", afterAutoClear } });

        if (_p->playbackCtrl) {
            const QString compareMedia = QCoreApplication::applicationDirPath() + QStringLiteral("/../../../tests/media/4k_60fps.mp4");
            if (QFileInfo::exists(compareMedia)) {
                _p->playbackCtrl->setCompareFile(compareMedia);
                addResult(QStringLiteral("Set compare file"), _p->playbackCtrl->hasCompare() ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                          QStringLiteral("compare file loaded"),
                          QJsonObject{ { "compare_media", compareMedia }, { "has_compare", _p->playbackCtrl->hasCompare() } });
                _p->playbackCtrl->clearCompare();
            } else {
                addResult(QStringLiteral("Set compare file"), QStringLiteral("SKIP"), QStringLiteral("compare media missing"),
                          QJsonObject{ { "compare_media", compareMedia } });
            }
        }
    }

    if (auto* annotationService = resolveAnnotationService()) {
        if (isFullScreen()) {
            sendKeyPress(this, Qt::Key_Escape);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        }
        if (_p->annoToolbar) {
            _p->annoToolsVisible = true;
            _p->annoToolbar->show();
            _onAnnotationModeToggled(true);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
        QThread::msleep(120);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 150);

        if (!_p->annoToolbar || !_p->annoToolbar->isVisible()) {
            addResult(QStringLiteral("Annotation toolbar"), QStringLiteral("FAIL"),
                      QStringLiteral("annotation toolbar unavailable"),
                      capturePlayer());
        } else {
            annotationService->setTool(AnnotationToolbar::Select);
            annotationService->setToolColor(QColor(255, 0, 0));
            annotationService->selectAnnotation(QString());
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);

            const auto runAnnoKey = [&](const QString& name, int key, AnnotationToolbar::Tool expectedTool) {
                const QJsonObject before = capturePlayer();
                sendKeyPress(this, key, Qt::AltModifier);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
                QThread::msleep(120);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
                const QJsonObject after = capturePlayer();
                const bool ok = annotationService->currentTool() == static_cast<int>(expectedTool);
                addResult(name, ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                          QStringLiteral("annotation tool check"),
                          QJsonObject{
                              { "before", before },
                              { "after", after },
                              { "expected_tool", static_cast<int>(expectedTool) }
                          });
            };

            runAnnoKey(QStringLiteral("Alt+S"), Qt::Key_S, AnnotationToolbar::Select);
            runAnnoKey(QStringLiteral("Alt+A"), Qt::Key_A, AnnotationToolbar::Arrow);
            runAnnoKey(QStringLiteral("Alt+R"), Qt::Key_R, AnnotationToolbar::Rectangle);
            runAnnoKey(QStringLiteral("Alt+C"), Qt::Key_C, AnnotationToolbar::Circle);
            runAnnoKey(QStringLiteral("Alt+T"), Qt::Key_T, AnnotationToolbar::Text);
            runAnnoKey(QStringLiteral("Alt+D"), Qt::Key_D, AnnotationToolbar::FreeDraw);

            const QString noteId = annotationService->createNote(QStringLiteral("smoke annotation"));
            annotationService->selectAnnotation(noteId);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            QThread::msleep(120);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);

            const QJsonObject beforeDelete = capturePlayer();
            sendKeyPress(this, Qt::Key_Delete);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            QThread::msleep(120);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            const QJsonObject afterDelete = capturePlayer();
            const int countBeforeDelete = beforeDelete.value("annotation_count").toInt();
            const int countAfterDelete = afterDelete.value("annotation_count").toInt();
            addResult(QStringLiteral("Delete shortcut"),
                      countAfterDelete == countBeforeDelete - 1 ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                      QStringLiteral("annotation removed"),
                      QJsonObject{
                          { "before", beforeDelete },
                          { "after", afterDelete },
                          { "created_id", noteId }
                      });

            sendKeyPress(this, Qt::Key_Z, Qt::ControlModifier);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            QThread::msleep(120);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            const QJsonObject afterUndo = capturePlayer();
            const int countAfterUndo = afterUndo.value("annotation_count").toInt();
            addResult(QStringLiteral("Ctrl+Z"),
                      countAfterUndo == countBeforeDelete ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                      QStringLiteral("undo annotation delete"),
                      QJsonObject{
                          { "after_delete", afterDelete },
                          { "after_undo", afterUndo },
                          { "created_id", noteId }
                      });

            sendKeyPress(this, Qt::Key_Y, Qt::ControlModifier);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            QThread::msleep(120);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            const QJsonObject afterRedo = capturePlayer();
            const int countAfterRedo = afterRedo.value("annotation_count").toInt();
            addResult(QStringLiteral("Ctrl+Y"),
                      countAfterRedo == countAfterDelete ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                      QStringLiteral("redo annotation delete"),
                      QJsonObject{
                          { "after_undo", afterUndo },
                          { "after_redo", afterRedo },
                          { "created_id", noteId }
                      });
        }
    } else {
        addResult(QStringLiteral("Annotation service"), QStringLiteral("FAIL"),
                  QStringLiteral("annotation service unavailable"),
                  capturePlayer());
    }

    for (const auto& result : _runFullscreenSmokeChecks()) results.append(result);
    for (const auto& result : _runSettingsSmokeChecks()) results.append(result);

    if (_p->playlist && _p->playlist->model()) {
        const int playlistCount = _p->playlist->model()->shotCount();
        addResult(QStringLiteral("Playlist count"), playlistCount > 0 ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                  QStringLiteral("playlist populated"),
                  QJsonObject{ { "count", playlistCount } });

        const QString extraMedia = QCoreApplication::applicationDirPath() + QStringLiteral("/../../../tests/media/4k_60fps.mp4");
        if (QFileInfo::exists(extraMedia)) {
            auto* model = _p->playlist->model();
            const QString currentBefore = _p->playbackCtrl ? _p->playbackCtrl->currentPath() : QString();
            const int countBeforeAdd = model->shotCount();
            model->addPath(extraMedia);
            model->setCurrentIndex(model->shotCount() - 1);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            const int countBeforeDelete = model->shotCount();
            const int selectedIndex = model->currentIndex();
            if (selectedIndex >= 0) {
                model->removeShotAt(selectedIndex);
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            const int countAfterDelete = model->shotCount();
            const QString currentAfter = _p->playbackCtrl ? _p->playbackCtrl->currentPath() : QString();
            const bool deleted = countAfterDelete < countBeforeDelete;
            const bool keptCurrentMedia =
                currentBefore.isEmpty() ||
                QFileInfo(currentAfter).absoluteFilePath() == QFileInfo(currentBefore).absoluteFilePath();
            addResult(QStringLiteral("Playlist Delete"), deleted && keptCurrentMedia ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                      QStringLiteral("remove selected playlist item"),
                      QJsonObject{ { "count_before_add", countBeforeAdd }, { "count_before_delete", countBeforeDelete }, { "count_after_delete", countAfterDelete }, { "current_before", currentBefore }, { "current_after", currentAfter } });
        } else {
            addResult(QStringLiteral("Playlist Delete"), QStringLiteral("SKIP"), QStringLiteral("extra media missing"),
                      QJsonObject{ { "extra_media", extraMedia } });
        }
    }

    if (_p->viewer) {
        _p->viewer->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        QThread::msleep(120);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    }

    const QString mainViewId = _p->viewer ? _p->viewer->viewId() : QString();
    QString secondaryViewId;
    QPointer<SecondaryWindow> secondaryWindow;
    if (auto* app = qobject_cast<Application*>(qApp)) {
        app->openNewWindow();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QThread::msleep(150);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);

        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* candidate = qobject_cast<SecondaryWindow*>(widget);
            if (!candidate || !candidate->viewerWidget()) {
                continue;
            }
            if (candidate->viewerWidget()->viewId() == mainViewId) {
                continue;
            }
            secondaryWindow = candidate;
            secondaryViewId = candidate->viewerWidget()->viewId();
            attachViewerLifecycleProbe(candidate->viewerWidget());
            candidate->viewerWidget()->setFocus(Qt::OtherFocusReason);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            QThread::msleep(150);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            break;
        }

        if (secondaryWindow) {
            secondaryWindow->close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            QThread::msleep(150);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            if (_p->viewer) {
                _p->viewer->setFocus(Qt::OtherFocusReason);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
                QThread::msleep(150);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            }
        }
    }

    auto findLifecycleIndex = [&lifecycleEvents](const QString& name, const QString& viewId) {
        for (qsizetype i = 0; i < lifecycleEvents.size(); ++i) {
            const QJsonObject item = lifecycleEvents.at(i).toObject();
            if (item.value("name").toString() == name && item.value("viewId").toString() == viewId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    const int mainInvalidatedIndex = findLifecycleIndex(QStringLiteral("activeViewInvalidated"), mainViewId);
    const int secondaryChangedIndex = findLifecycleIndex(QStringLiteral("activeViewChanged"), secondaryViewId);
    const int secondaryInvalidatedIndex = findLifecycleIndex(QStringLiteral("activeViewInvalidated"), secondaryViewId);
    int mainChangedAfterSecondaryIndex = -1;
    for (qsizetype i = 0; i < lifecycleEvents.size(); ++i) {
        const QJsonObject item = lifecycleEvents.at(i).toObject();
        if (item.value("name").toString() == QStringLiteral("activeViewChanged") &&
            item.value("viewId").toString() == mainViewId &&
            static_cast<int>(i) > secondaryInvalidatedIndex) {
            mainChangedAfterSecondaryIndex = static_cast<int>(i);
            break;
        }
    }

    const bool lifecycleSwitchPass =
        !mainViewId.isEmpty() &&
        !secondaryViewId.isEmpty() &&
        mainInvalidatedIndex >= 0 &&
        secondaryChangedIndex > mainInvalidatedIndex &&
        secondaryInvalidatedIndex > secondaryChangedIndex &&
        mainChangedAfterSecondaryIndex > secondaryInvalidatedIndex;
    addResult(QStringLiteral("Lifecycle main-secondary-main"),
              lifecycleSwitchPass ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
              QStringLiteral("active view invalidation and rebind ordering"),
              QJsonObject{
                  { "main_view_id", mainViewId },
                  { "secondary_view_id", secondaryViewId },
                  { "main_invalidated_index", mainInvalidatedIndex },
                  { "secondary_changed_index", secondaryChangedIndex },
                  { "secondary_invalidated_index", secondaryInvalidatedIndex },
                  { "main_rebound_index", mainChangedAfterSecondaryIndex }
              });

    const QJsonObject finalState = capturePlayer();
    report["baseline"] = baseline;
    report["final_state"] = finalState;
    report["lifecycle_events"] = lifecycleEvents;
    QJsonObject manifestCommands; QString coverageManifestPath;
    const QStringList manifestCandidates = {QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../../../tests/automation_coverage_manifest.json")), QDir::current().absoluteFilePath(QStringLiteral("tests/automation_coverage_manifest.json"))};
    for (const QString& candidate : manifestCandidates) {
        QFile manifestFile(candidate);
        if (!manifestFile.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll()); if (!document.isObject()) continue;
        manifestCommands = document.object().value(QStringLiteral("commands")).toObject();
        coverageManifestPath = QFileInfo(candidate).absoluteFilePath();
        break;
    }
    QJsonArray commandInventory, uncoveredCommands, uncoveredShortcuts;
    for (const auto& descriptor : _p->commandDescriptors) {
        const QString commandId = descriptor.id.trimmed(), shortcut = _p->commandDefaultShortcuts.value(commandId, descriptor.shortcut).trimmed();
        const QJsonObject evidence = manifestCommands.value(commandId).toObject();
        commandInventory.append(QJsonObject{{QStringLiteral("id"), commandId}, {QStringLiteral("shortcut"), shortcut}, {QStringLiteral("test"),
            evidence.value(QStringLiteral("test")).toString()}, {QStringLiteral("shortcutTest"), evidence.value(QStringLiteral("shortcutTest")).toString()}});
        if (commandId.isEmpty() || evidence.value(QStringLiteral("test")).toString().trimmed().isEmpty()) uncoveredCommands.append(commandId);
        if (!shortcut.isEmpty() && evidence.value(QStringLiteral("shortcutTest")).toString().trimmed().isEmpty())
            uncoveredShortcuts.append(QJsonObject{{QStringLiteral("id"), commandId}, {QStringLiteral("shortcut"), shortcut}});
    }
    const bool coverageComplete = !coverageManifestPath.isEmpty() && uncoveredCommands.isEmpty() &&
        uncoveredShortcuts.isEmpty() && manifestCommands.size() == _p->commandDescriptors.size();
    report["coverage"] = QJsonObject{{QStringLiteral("complete"), coverageComplete}, {QStringLiteral("manifest"), coverageManifestPath},
        {QStringLiteral("registeredCommandCount"), _p->commandDescriptors.size()}, {QStringLiteral("manifestCommandCount"), manifestCommands.size()},
        {QStringLiteral("commands"), commandInventory}, {QStringLiteral("uncoveredCommands"), uncoveredCommands}, {QStringLiteral("uncoveredShortcuts"), uncoveredShortcuts}};
    addResult(QStringLiteral("Automation coverage gate"), coverageComplete ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
              QStringLiteral("every registered command and default shortcut requires automated evidence"), report.value(QStringLiteral("coverage")).toObject());

    report["results"] = results;
    int passCount = 0;
    int failCount = 0;
    int skipCount = 0;
    for (const auto& value : results) {
        const QString status = value.toObject().value("status").toString();
        if (status == QStringLiteral("PASS")) {
            ++passCount;
        } else if (status == QStringLiteral("FAIL")) {
            ++failCount;
        } else if (status == QStringLiteral("SKIP")) {
            ++skipCount;
        }
    }
    report["summary"] = QJsonObject{{"pass", passCount}, {"fail", failCount}, {"skip", skipCount}, {"manual", 0}};
    report["manual_checks"] = QJsonArray{};
    report["environment_capabilities"] = QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("physical-audio-output-device")},
        {QStringLiteral("status"), QStringLiteral("NOT_RUN")}, {QStringLiteral("excludedFromPassFail"), true}}, QJsonObject{{QStringLiteral("id"),
        QStringLiteral("system-audio-driver-routing")}, {QStringLiteral("status"), QStringLiteral("NOT_RUN")}, {QStringLiteral("excludedFromPassFail"), true}}};
    for (const auto& connection : lifecycleSignalConnections) {
        QObject::disconnect(connection);
    }
    if (eventBus) {
        eventBus->unsubscribe<ActiveViewChangedEvent>(activeViewChangedSubscription);
        eventBus->unsubscribe<ActiveViewInvalidatedEvent>(activeViewInvalidatedSubscription);
        eventBus->unsubscribe<OverlayHostChangedEvent>(overlayHostChangedSubscription);
        eventBus->unsubscribe<OverlayHostInvalidatedEvent>(overlayHostInvalidatedSubscription);
        eventBus->unsubscribe<CoordinateMapperChangedEvent>(coordinateMapperChangedSubscription);
        eventBus->unsubscribe<CoordinateMapperInvalidatedEvent>(coordinateMapperInvalidatedSubscription);
        eventBus->unsubscribe<ViewTransformChangedEvent>(transformChangedSubscription);
        eventBus->unsubscribe<ViewportResizedEvent>(viewportResizedSubscription);
    }
    return report;
}

QJsonObject MainWindow::runQwenAsrProviderSmokeChecks(const QString& screenshotPath)
{
    if (_p->aiDock) {
        _p->aiDock->show();
        _p->aiDock->raise();
    }
    QJsonObject report;
    if (!_p->aiWorkspace) {
        report.insert(QStringLiteral("success"), false);
        report.insert(QStringLiteral("error"), QStringLiteral("ai-workspace-missing"));
        return report;
    }
    report = _p->aiWorkspace->runQwenAsrProviderSmoke();
    if (!screenshotPath.trimmed().isEmpty() &&
        _p->aiWorkspace->saveSettingsDialogSmokeScreenshot(screenshotPath)) {
        report.insert(QStringLiteral("screenshot_path"), QFileInfo(screenshotPath).absoluteFilePath());
    }
    report.insert(QStringLiteral("aiDockVisible"), _p->aiDock ? _p->aiDock->isVisible() : false);
    report.insert(QStringLiteral("pass"), report.value(QStringLiteral("stubRemoved")).toBool() &&
        report.value(QStringLiteral("model")).toString() == QStringLiteral("qwen3-asr-flash") &&
        !report.value(QStringLiteral("noKeyError")).toBool() &&
        report.value(QStringLiteral("realApiAttempted")).toBool() &&
        (report.value(QStringLiteral("success")).toBool() ||
         report.value(QStringLiteral("apiReachableButRejected")).toBool()) &&
        !report.value(QStringLiteral("transientNetworkError")).toBool());
    return report;
}

QJsonObject MainWindow::runPhase915ViewerRebuildChecks(const QString& mediaPath)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](const QString& name, bool passed, const QString& message, const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        assertions.append(item);
    };

    auto resolveAnnotationService = [this]() -> IAnnotationService* {
        if (_p->annotationService) {
            return _p->annotationService;
        }
        if (!isPluginFallbackDisabled() && _annoMgr) {
            return static_cast<IAnnotationService*>(_annoMgr.get());
        }
        return nullptr;
    };

    LifecycleRecorder recorder(this);
    recorder.attachViewer(_p->viewer);

    openFile(mediaPath);
    const bool mediaOpened = pumpUntil([this] {
        return _p->playbackCtrl && _p->playbackCtrl->isValid();
    });
    addAssertion(QStringLiteral("Open media"), mediaOpened, QStringLiteral("media opened"), QJsonObject{
        { QStringLiteral("media"), mediaPath }
    });

    if (_p->viewer) {
        _p->viewer->setFocus(Qt::OtherFocusReason);
    }
    const bool viewActivated = pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return state.value(QStringLiteral("active_view_id")).toString() ==
               state.value(QStringLiteral("viewer_view_id")).toString() &&
               !state.value(QStringLiteral("active_view_id")).toString().isEmpty();
    });
    addAssertion(QStringLiteral("Activate main view"), viewActivated, QStringLiteral("main view active"), captureRuntimeAnnotationState());

    const QJsonObject before = captureRuntimeAnnotationState();
    const QString oldViewId = before.value(QStringLiteral("active_view_id")).toString();
    const int subscriptionCountBefore = before.value(QStringLiteral("plugin_subscription_count")).toInt();

    const qint64 rebuildStartMs = recorder.elapsedMs();
    QElapsedTimer rebuildTimer;
    rebuildTimer.start();
    auto* app = qobject_cast<Application*>(qApp);
    const bool rebuildInvoked = app && app->rebuildMainViewerForRuntime();
    recorder.attachViewer(_p->viewer);
    addAssertion(QStringLiteral("Invoke viewer rebuild"), rebuildInvoked, QStringLiteral("rebuild requested"));

    const bool rebuildSettled = pumpUntil([this, oldViewId] {
        const QJsonObject state = captureRuntimeAnnotationState();
        const QString activeViewId = state.value(QStringLiteral("active_view_id")).toString();
        return !activeViewId.isEmpty() &&
               activeViewId != oldViewId &&
               state.value(QStringLiteral("plugin_bound_view_id")).toString() == activeViewId &&
               state.value(QStringLiteral("overlay_count")).toInt() == 1;
    });

    const QJsonObject after = captureRuntimeAnnotationState();
    const QString newViewId = after.value(QStringLiteral("active_view_id")).toString();
    const int mainInvalidatedIndex =
        recorder.findFirst(QStringLiteral("activeViewInvalidated"), oldViewId);
    const int overlayInvalidatedIndex =
        recorder.findFirst(QStringLiteral("overlayHostInvalidated"), oldViewId);
    const int mapperInvalidatedIndex =
        recorder.findFirst(QStringLiteral("coordinateMapperInvalidated"), oldViewId);
    const int newActiveIndex =
        recorder.findFirst(QStringLiteral("activeViewChanged"), newViewId);
    const qint64 newActiveElapsedMs = recorder.eventElapsedAt(newActiveIndex);
    const qint64 invalidatedElapsedMs = recorder.eventElapsedAt(mainInvalidatedIndex);
    const qint64 viewerRebuildSettleMs = rebuildTimer.elapsed();

    addAssertion(QStringLiteral("Viewer rebuild settled"), rebuildSettled, QStringLiteral("new view rebound"),
                 QJsonObject{ { QStringLiteral("before"), before }, { QStringLiteral("after"), after } });
    addAssertion(QStringLiteral("New view id"), !oldViewId.isEmpty() && !newViewId.isEmpty() && oldViewId != newViewId,
                 QStringLiteral("new viewer has distinct viewId"),
                 QJsonObject{ { QStringLiteral("oldViewId"), oldViewId }, { QStringLiteral("newViewId"), newViewId } });
    addAssertion(QStringLiteral("Invalidation ordering"),
                 mainInvalidatedIndex >= 0 &&
                     overlayInvalidatedIndex > mainInvalidatedIndex &&
                     mapperInvalidatedIndex > overlayInvalidatedIndex &&
                     newActiveIndex > mapperInvalidatedIndex,
                 QStringLiteral("invalidate before rebind"),
                 QJsonObject{
                     { QStringLiteral("mainInvalidatedIndex"), mainInvalidatedIndex },
                     { QStringLiteral("overlayInvalidatedIndex"), overlayInvalidatedIndex },
                     { QStringLiteral("mapperInvalidatedIndex"), mapperInvalidatedIndex },
                     { QStringLiteral("newActiveIndex"), newActiveIndex }
                 });
    addAssertion(QStringLiteral("Plugin bound view"), after.value(QStringLiteral("plugin_bound_view_id")).toString() == newViewId,
                 QStringLiteral("annotation plugin rebound to new view"), after);
    addAssertion(QStringLiteral("Bridge rebound"), after.value(QStringLiteral("plugin_has_bridge_binding")).toBool() &&
                     after.value(QStringLiteral("bridge_present")).toBool(),
                 QStringLiteral("bridge available after rebuild"), after);
    addAssertion(QStringLiteral("Overlay count"), after.value(QStringLiteral("overlay_count")).toInt() == 1,
                 QStringLiteral("single overlay after rebuild"), after);
    addAssertion(QStringLiteral("Subscription stability"),
                 after.value(QStringLiteral("plugin_subscription_count")).toInt() == subscriptionCountBefore,
                 QStringLiteral("no duplicate plugin subscriptions"),
                 QJsonObject{
                     { QStringLiteral("before"), subscriptionCountBefore },
                     { QStringLiteral("after"), after.value(QStringLiteral("plugin_subscription_count")).toInt() }
                 });

    bool postRebuildAnnotationOk = false;
    QString createdId;
    if (auto* annotationService = resolveAnnotationService()) {
        const int countBefore = annotationService->count();
        createdId = annotationService->createNote(QStringLiteral("phase9.15 viewer rebuild"));
        postRebuildAnnotationOk =
            !createdId.isEmpty() &&
            pumpUntil([annotationService, countBefore] {
                return annotationService->count() == countBefore + 1;
            });
        if (!createdId.isEmpty()) {
            annotationService->removeAnnotation(createdId);
            pumpUntil([annotationService, countBefore] {
                return annotationService->count() == countBefore;
            });
        }
    }
    addAssertion(QStringLiteral("Post-rebuild annotation"), postRebuildAnnotationOk,
                 QStringLiteral("annotation service still works after rebuild"),
                 captureRuntimeAnnotationState());

    QJsonObject multiWindow;
    QJsonArray multiWindowAssertions;
    QJsonObject multiWindowMetrics;
    auto addMultiAssertion = [&multiWindowAssertions](const QString& name, bool passed, const QString& message, const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        multiWindowAssertions.append(item);
    };

    QString secondaryViewId;
    if (app) {
        app->openNewWindow();
        SecondaryWindow* secondary = nullptr;
        const bool secondaryOpened = pumpUntil([&secondary, &newViewId] {
            secondary = findSecondaryWindow(newViewId);
            return secondary != nullptr;
        });
        addMultiAssertion(QStringLiteral("Open secondary window"), secondaryOpened,
                          QStringLiteral("secondary window opened"));

        if (secondary && secondary->viewerWidget()) {
            secondaryViewId = secondary->viewerWidget()->viewId();
            recorder.attachViewer(secondary->viewerWidget());
            const double overlayInstanceBeforeSwitch =
                captureRuntimeAnnotationState().value(QStringLiteral("overlay_instance_id")).toDouble();
            QElapsedTimer mainToSecondaryTimer;
            mainToSecondaryTimer.start();
            const bool activateSecondaryOk = activateViewForRuntime(app, secondaryViewId);

            const bool switchedToSecondary = pumpUntil([this, &secondaryViewId] {
                return captureRuntimeAnnotationState().value(QStringLiteral("active_view_id")).toString() == secondaryViewId;
            });
            const double overlayInstanceAfterSecondary =
                captureRuntimeAnnotationState().value(QStringLiteral("overlay_instance_id")).toDouble();
            addMultiAssertion(QStringLiteral("Switch main->secondary"), activateSecondaryOk && switchedToSecondary,
                              QStringLiteral("secondary became active"),
                              captureRuntimeAnnotationState());
            multiWindowMetrics.insert(
                QStringLiteral("mainToSecondary"),
                QJsonObject{
                    { QStringLiteral("activeViewSwitchLatencyMs"), static_cast<double>(mainToSecondaryTimer.elapsed()) },
                    { QStringLiteral("overlayAttachLatencyMs"), static_cast<double>(mainToSecondaryTimer.elapsed()) },
                    { QStringLiteral("overlayRebuildCount"),
                      overlayInstanceAfterSecondary != overlayInstanceBeforeSwitch ? 1 : 0 },
                    { QStringLiteral("invalidationDurationMs"), QJsonValue(QJsonValue::Null) }
                });

            if (_p->viewer) {
                const double overlayInstanceBeforeReturn =
                    captureRuntimeAnnotationState().value(QStringLiteral("overlay_instance_id")).toDouble();
                QElapsedTimer secondaryToMainTimer;
                secondaryToMainTimer.start();
                const bool activateMainOk = activateViewForRuntime(app, newViewId);
                const bool switchedBackToMain = pumpUntil([this, &newViewId] {
                    return captureRuntimeAnnotationState().value(QStringLiteral("active_view_id")).toString() == newViewId;
                });
                const double overlayInstanceAfterReturn =
                    captureRuntimeAnnotationState().value(QStringLiteral("overlay_instance_id")).toDouble();
                addMultiAssertion(QStringLiteral("Switch secondary->main"), activateMainOk && switchedBackToMain,
                                  QStringLiteral("main became active again"),
                                  captureRuntimeAnnotationState());
                multiWindowMetrics.insert(
                    QStringLiteral("secondaryToMain"),
                    QJsonObject{
                        { QStringLiteral("activeViewSwitchLatencyMs"), static_cast<double>(secondaryToMainTimer.elapsed()) },
                        { QStringLiteral("overlayAttachLatencyMs"), static_cast<double>(secondaryToMainTimer.elapsed()) },
                        { QStringLiteral("overlayRebuildCount"),
                          overlayInstanceAfterReturn != overlayInstanceBeforeReturn ? 1 : 0 },
                        { QStringLiteral("invalidationDurationMs"), QJsonValue(QJsonValue::Null) }
                    });
            }

            secondary->close();
            const bool closeInactiveStable = pumpUntil([this, &newViewId] {
                return captureRuntimeAnnotationState().value(QStringLiteral("active_view_id")).toString() == newViewId &&
                       findSecondaryWindow(newViewId) == nullptr;
            });
            addMultiAssertion(QStringLiteral("Close non-active secondary"), closeInactiveStable,
                              QStringLiteral("main binding preserved"));

            app->openNewWindow();
            SecondaryWindow* activeSecondary = nullptr;
            const bool reopenedSecondary = pumpUntil([&activeSecondary, &newViewId] {
                activeSecondary = findSecondaryWindow(newViewId);
                return activeSecondary != nullptr;
            });
            addMultiAssertion(QStringLiteral("Reopen secondary window"), reopenedSecondary,
                              QStringLiteral("secondary reopened"));

            if (activeSecondary && activeSecondary->viewerWidget()) {
                const QString closingViewId = activeSecondary->viewerWidget()->viewId();
                recorder.attachViewer(activeSecondary->viewerWidget());
                const bool activateClosingSecondaryOk = activateViewForRuntime(app, closingViewId);
                const bool activatedForClose = pumpUntil([this, &closingViewId] {
                    return captureRuntimeAnnotationState().value(QStringLiteral("active_view_id")).toString() == closingViewId;
                });
                addMultiAssertion(QStringLiteral("Activate secondary before close"), activateClosingSecondaryOk && activatedForClose,
                                  QStringLiteral("secondary active before close"));

                const qint64 closeActiveStartMs = recorder.elapsedMs();
                QElapsedTimer closeActiveTimer;
                closeActiveTimer.start();
                activeSecondary->close();
                const bool closeActiveRebind = pumpUntil([this, &newViewId] {
                    return captureRuntimeAnnotationState().value(QStringLiteral("active_view_id")).toString() == newViewId;
                });
                const int secondaryInvalidatedIndex =
                    recorder.findFirst(QStringLiteral("activeViewInvalidated"), closingViewId);
                const int mainReactivatedIndex =
                    recorder.findFirstAfter(QStringLiteral("activeViewChanged"), newViewId, secondaryInvalidatedIndex);
                addMultiAssertion(QStringLiteral("Close active secondary"), closeActiveRebind,
                                  QStringLiteral("main view rebound after active secondary closed"),
                                  QJsonObject{
                                      { QStringLiteral("closingViewId"), closingViewId },
                                      { QStringLiteral("secondaryInvalidatedIndex"), secondaryInvalidatedIndex },
                                      { QStringLiteral("mainReactivatedIndex"), mainReactivatedIndex }
                                  });
                addMultiAssertion(QStringLiteral("Active close ordering"),
                                  secondaryInvalidatedIndex >= 0 && mainReactivatedIndex > secondaryInvalidatedIndex,
                                  QStringLiteral("secondary invalidated before main reactivated"),
                                  QJsonObject{
                                      { QStringLiteral("secondaryInvalidatedIndex"), secondaryInvalidatedIndex },
                                      { QStringLiteral("mainReactivatedIndex"), mainReactivatedIndex }
                                  });
                multiWindowMetrics.insert(
                    QStringLiteral("closeActiveSecondary"),
                    QJsonObject{
                        { QStringLiteral("activeViewSwitchLatencyMs"), static_cast<double>(closeActiveTimer.elapsed()) },
                        { QStringLiteral("overlayAttachLatencyMs"), static_cast<double>(closeActiveTimer.elapsed()) },
                        { QStringLiteral("overlayRebuildCount"), 1 },
                        { QStringLiteral("invalidationDurationMs"),
                          secondaryInvalidatedIndex >= 0 && mainReactivatedIndex > secondaryInvalidatedIndex
                              ? static_cast<double>(recorder.eventElapsedAt(mainReactivatedIndex) -
                                                    recorder.eventElapsedAt(secondaryInvalidatedIndex))
                              : static_cast<double>(recorder.elapsedMs() - closeActiveStartMs) }
                    });
            }
        }
    }

    multiWindow.insert(QStringLiteral("results"), multiWindowAssertions);
    multiWindow.insert(QStringLiteral("summary"), makeSummary(multiWindowAssertions));
    multiWindow.insert(QStringLiteral("metrics"), multiWindowMetrics);

    QJsonObject viewerMetrics;
    viewerMetrics.insert(QStringLiteral("overlayAttachLatencyMs"), static_cast<double>(viewerRebuildSettleMs));
    viewerMetrics.insert(
        QStringLiteral("overlayRebuildCount"),
        after.value(QStringLiteral("overlay_instance_id")).toDouble() !=
                before.value(QStringLiteral("overlay_instance_id")).toDouble()
            ? 1
            : 0);
    if (newActiveElapsedMs >= 0) {
        viewerMetrics.insert(
            QStringLiteral("activeViewSwitchLatencyMs"),
            static_cast<double>(newActiveElapsedMs - rebuildStartMs));
    } else {
        viewerMetrics.insert(QStringLiteral("activeViewSwitchLatencyMs"), QJsonValue(QJsonValue::Null));
    }
    if (invalidatedElapsedMs >= 0 && newActiveElapsedMs > invalidatedElapsedMs) {
        viewerMetrics.insert(
            QStringLiteral("invalidationDurationMs"),
            static_cast<double>(newActiveElapsedMs - invalidatedElapsedMs));
    } else {
        viewerMetrics.insert(QStringLiteral("invalidationDurationMs"), QJsonValue(QJsonValue::Null));
    }

    report.insert(QStringLiteral("before"), before);
    report.insert(QStringLiteral("after"), after);
    report.insert(QStringLiteral("lifecycle_events"), recorder.events());
    report.insert(QStringLiteral("metrics"), viewerMetrics);
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    report.insert(QStringLiteral("multi_window"), multiWindow);
    return report;
}

QJsonObject MainWindow::runPhase915PluginReloadChecks(const QString& mediaPath)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](const QString& name, bool passed, const QString& message, const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        assertions.append(item);
    };

    auto* app = qobject_cast<Application*>(qApp);
    if (app) {
        app->setPluginFallbackEnabledForRuntime(false);
    }

    LifecycleRecorder recorder(this);
    recorder.attachViewer(_p->viewer);

    openFile(mediaPath);
    const bool mediaOpened = pumpUntil([this] {
        return _p->playbackCtrl && _p->playbackCtrl->isValid();
    });
    addAssertion(QStringLiteral("Open media"), mediaOpened, QStringLiteral("media opened"));

    if (_p->viewer) {
        _p->viewer->setFocus(Qt::OtherFocusReason);
    }
    const bool viewActivated = pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return state.value(QStringLiteral("active_view_id")).toString() ==
               state.value(QStringLiteral("viewer_view_id")).toString();
    });
    addAssertion(QStringLiteral("Activate main view"), viewActivated, QStringLiteral("main view active"), captureRuntimeAnnotationState());

    const QJsonObject before = captureRuntimeAnnotationState();
    const QString activeViewId = before.value(QStringLiteral("active_view_id")).toString();
    const int subscriptionCountBefore = before.value(QStringLiteral("plugin_subscription_count")).toInt();

    QElapsedTimer unloadTimer;
    unloadTimer.start();
    const bool unloadOk = app && app->unloadPluginForRuntime(QStringLiteral("annotation"));
    const bool unloadSettled = pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return !state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
               !state.value(QStringLiteral("annotation_service_present")).toBool() &&
               state.value(QStringLiteral("overlay_count")).toInt() == 0;
    });
    const QJsonObject afterUnload = captureRuntimeAnnotationState();
    const qint64 unloadDurationMs = unloadTimer.elapsed();
    addAssertion(QStringLiteral("Unload annotation plugin"), unloadOk && unloadSettled,
                 QStringLiteral("annotation capability removed without restart"),
                 QJsonObject{ { QStringLiteral("after_unload"), afterUnload } });
    addAssertion(QStringLiteral("Unload cleared bindings"),
                 !afterUnload.value(QStringLiteral("annotation_service_present")).toBool() &&
                     !afterUnload.value(QStringLiteral("plugin_has_bridge_binding")).toBool() &&
                     afterUnload.value(QStringLiteral("overlay_count")).toInt() == 0,
                 QStringLiteral("no active annotation binding remains"),
                 afterUnload);

    QElapsedTimer reloadTimer;
    reloadTimer.start();
    const bool reloadOk = app && app->reloadPluginForRuntime(QStringLiteral("annotation"));
    const bool reloadSettled = pumpUntil([this, &activeViewId] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
               state.value(QStringLiteral("annotation_service_present")).toBool() &&
               state.value(QStringLiteral("overlay_count")).toInt() == 1 &&
               state.value(QStringLiteral("active_view_id")).toString() == activeViewId &&
               state.value(QStringLiteral("plugin_bound_view_id")).toString() == activeViewId;
    });
    const QJsonObject afterReload = captureRuntimeAnnotationState();
    const qint64 reloadDurationMs = reloadTimer.elapsed();

    addAssertion(QStringLiteral("Reload annotation plugin"), reloadOk && reloadSettled,
                 QStringLiteral("annotation plugin rebound to active view"),
                 QJsonObject{ { QStringLiteral("after_reload"), afterReload } });
    addAssertion(QStringLiteral("Reload kept active view"),
                 afterReload.value(QStringLiteral("active_view_id")).toString() == activeViewId,
                 QStringLiteral("active view id unchanged"), afterReload);
    addAssertion(QStringLiteral("Reload subscription stability"),
                 afterReload.value(QStringLiteral("plugin_subscription_count")).toInt() == subscriptionCountBefore,
                 QStringLiteral("plugin subscriptions restored without duplication"),
                 QJsonObject{
                     { QStringLiteral("before"), subscriptionCountBefore },
                     { QStringLiteral("after"), afterReload.value(QStringLiteral("plugin_subscription_count")).toInt() }
                 });
    addAssertion(QStringLiteral("Reload frame sync"),
                 afterReload.value(QStringLiteral("bridge_present")).toBool() &&
                     afterReload.value(QStringLiteral("bridge_current_frame")).toInt() == (_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0),
                 QStringLiteral("bridge current frame matches playback current frame"),
                 afterReload);

    bool undoRedoOk = false;
    if (auto* annotationService = _p->annotationService) {
        const int countBefore = annotationService->count();
        const QString id = annotationService->createNote(QStringLiteral("phase9.15 reload"));
        if (!id.isEmpty()) {
            annotationService->selectAnnotation(id);
            const bool deleted = annotationService->removeSelectedAnnotation();
            const bool undone = annotationService->undo();
            const bool redone = annotationService->redo();
            const int finalCount = annotationService->count();
            undoRedoOk = deleted && undone && redone && finalCount == countBefore;
        }
    }
    addAssertion(QStringLiteral("Undo/Redo after reload"), undoRedoOk,
                 QStringLiteral("annotation edit stack still works after reload"),
                 captureRuntimeAnnotationState());

    report.insert(QStringLiteral("before"), before);
    report.insert(QStringLiteral("after_unload"), afterUnload);
    report.insert(QStringLiteral("after_reload"), afterReload);
    report.insert(QStringLiteral("lifecycle_events"), recorder.events());
    report.insert(
        QStringLiteral("metrics"),
        QJsonObject{
            { QStringLiteral("overlayAttachLatencyMs"), static_cast<double>(reloadDurationMs) },
            { QStringLiteral("overlayRebuildCount"),
              afterReload.value(QStringLiteral("overlay_instance_id")).toDouble() !=
                      before.value(QStringLiteral("overlay_instance_id")).toDouble()
                  ? 1
                  : 0 },
            { QStringLiteral("activeViewSwitchLatencyMs"), QJsonValue(QJsonValue::Null) },
            { QStringLiteral("invalidationDurationMs"), static_cast<double>(unloadDurationMs) }
        });
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    return report;
}

QJsonObject MainWindow::runPhase915FallbackToggleChecks(const QString& mediaPath)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](const QString& name, bool passed, const QString& message, const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        assertions.append(item);
    };

    auto resolveAnnotationService = [this]() -> IAnnotationService* {
        if (_p->annotationService) {
            return _p->annotationService;
        }
        if (!isPluginFallbackDisabled() && _annoMgr) {
            return static_cast<IAnnotationService*>(_annoMgr.get());
        }
        return nullptr;
    };

    auto* app = qobject_cast<Application*>(qApp);
    LifecycleRecorder recorder(this);
    recorder.attachViewer(_p->viewer);

    openFile(mediaPath);
    const bool mediaOpened = pumpUntil([this] {
        return _p->playbackCtrl && _p->playbackCtrl->isValid();
    });
    addAssertion(QStringLiteral("Open media"), mediaOpened, QStringLiteral("media opened"));

    if (_p->viewer) {
        _p->viewer->setFocus(Qt::OtherFocusReason);
    }
    pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return state.value(QStringLiteral("active_view_id")).toString() ==
               state.value(QStringLiteral("viewer_view_id")).toString();
    });

    if (app) {
        app->setPluginFallbackEnabledForRuntime(true);
        app->unloadPluginForRuntime(QStringLiteral("annotation"));
    }
    const bool fallbackReady = pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return !state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
               state.value(QStringLiteral("annotation_service_present")).toBool() &&
               state.value(QStringLiteral("overlay_count")).toInt() == 1;
    });
    const QJsonObject before = captureRuntimeAnnotationState();
    const QString activeViewId = before.value(QStringLiteral("active_view_id")).toString();
    const double oldOverlayInstanceId = before.value(QStringLiteral("overlay_instance_id")).toDouble();
    addAssertion(QStringLiteral("Fallback baseline"), fallbackReady,
                 QStringLiteral("fallback owns annotation capability"),
                 before);

    const int eventsBeforeDisable = recorder.events().size();
    QElapsedTimer disableTimer;
    disableTimer.start();
    if (app) {
        app->setPluginFallbackEnabledForRuntime(false);
    }
    const bool disabledSettled = pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return !state.value(QStringLiteral("annotation_service_present")).toBool() &&
               state.value(QStringLiteral("overlay_count")).toInt() == 0;
    });
    const QJsonObject afterDisable = captureRuntimeAnnotationState();
    const qint64 disableDurationMs = disableTimer.elapsed();
    const int fallbackInvalidatedIndex =
        recorder.findFirstAfter(QStringLiteral("overlayHostInvalidated"), activeViewId, eventsBeforeDisable - 1);
    addAssertion(QStringLiteral("Disable fallback"), disabledSettled,
                 QStringLiteral("fallback capability invalidated"),
                 QJsonObject{
                     { QStringLiteral("state"), afterDisable },
                     { QStringLiteral("overlayHostInvalidatedIndex"), fallbackInvalidatedIndex }
                 });
    addAssertion(QStringLiteral("Disable clears capability"),
                 !afterDisable.value(QStringLiteral("annotation_service_present")).toBool() &&
                     afterDisable.value(QStringLiteral("overlay_count")).toInt() == 0 &&
                     !afterDisable.value(QStringLiteral("annotation_plugin_loaded")).toBool(),
                 QStringLiteral("fallback disabled without dual binding"),
                 afterDisable);

    QElapsedTimer enableTimer;
    enableTimer.start();
    if (app) {
        app->setPluginFallbackEnabledForRuntime(true);
    }
    const bool enabledSettled = pumpUntil([this, &activeViewId, oldOverlayInstanceId] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return state.value(QStringLiteral("annotation_service_present")).toBool() &&
               !state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
               state.value(QStringLiteral("overlay_count")).toInt() == 1 &&
               state.value(QStringLiteral("active_view_id")).toString() == activeViewId &&
               state.value(QStringLiteral("overlay_instance_id")).toDouble() != oldOverlayInstanceId;
    });
    const QJsonObject afterEnable = captureRuntimeAnnotationState();
    const qint64 enableDurationMs = enableTimer.elapsed();
    addAssertion(QStringLiteral("Enable fallback"), enabledSettled,
                 QStringLiteral("fallback capability rebound with fresh overlay"),
                 QJsonObject{
                     { QStringLiteral("before"), before },
                     { QStringLiteral("after_enable"), afterEnable }
                 });

    bool fallbackAnnotationOk = false;
    if (auto* annotationService = resolveAnnotationService()) {
        const int countBefore = annotationService->count();
        const QString id = annotationService->createNote(QStringLiteral("phase9.15 fallback"));
        fallbackAnnotationOk = !id.isEmpty() &&
            pumpUntil([annotationService, countBefore] {
                return annotationService->count() == countBefore + 1;
            });
        if (!id.isEmpty()) {
            annotationService->removeAnnotation(id);
            pumpUntil([annotationService, countBefore] {
                return annotationService->count() == countBefore;
            });
        }
    }
    addAssertion(QStringLiteral("Fallback annotation usable"), fallbackAnnotationOk,
                 QStringLiteral("annotation operations work after fallback re-enable"),
                 afterEnable);

    report.insert(QStringLiteral("before"), before);
    report.insert(QStringLiteral("after_disable"), afterDisable);
    report.insert(QStringLiteral("after_enable"), afterEnable);
    report.insert(QStringLiteral("lifecycle_events"), recorder.events());
    report.insert(
        QStringLiteral("metrics"),
        QJsonObject{
            { QStringLiteral("overlayAttachLatencyMs"), static_cast<double>(enableDurationMs) },
            { QStringLiteral("overlayRebuildCount"),
              afterEnable.value(QStringLiteral("overlay_instance_id")).toDouble() != oldOverlayInstanceId ? 1 : 0 },
            { QStringLiteral("activeViewSwitchLatencyMs"), QJsonValue(QJsonValue::Null) },
            { QStringLiteral("invalidationDurationMs"), static_cast<double>(disableDurationMs) }
        });
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    return report;
}

QJsonObject MainWindow::runPhase14PerformanceBaselineChecks(const QString& mediaPath)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](const QString& name, bool passed, const QString& message, const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        assertions.append(item);
    };

    openFile(mediaPath);
    const bool mediaOpened = pumpUntil([this] {
        return _p->playbackCtrl && _p->playbackCtrl->isValid();
    });
    addAssertion(QStringLiteral("Open media"), mediaOpened, QStringLiteral("media opened for phase14 baseline"),
                 QJsonObject{ { QStringLiteral("media"), mediaPath } });

    if (_p->viewer) {
        _p->viewer->setFocus(Qt::OtherFocusReason);
    }
    const bool viewActivated = pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return !state.value(QStringLiteral("active_view_id")).toString().isEmpty() &&
               state.value(QStringLiteral("active_view_id")).toString() ==
                   state.value(QStringLiteral("viewer_view_id")).toString();
    });
    addAssertion(QStringLiteral("Activate main view"), viewActivated, QStringLiteral("main view active for baseline"),
                 captureRuntimeAnnotationState());

    const QJsonObject initialState = captureRuntimeAnnotationState();
    const QJsonObject initialOverlayMetrics = overlayMetricsObject(initialState);
    const int subscriptionBaseline = initialState.value(QStringLiteral("plugin_subscription_count")).toInt();

    QJsonObject frameProbe;
    if (_p->playbackCtrl && _p->playbackCtrl->isValid() && _p->playbackCtrl->totalFrames() > 1) {
        int startFrame = _p->playbackCtrl->currentFrame();
        if (startFrame + 12 >= _p->playbackCtrl->totalFrames()) {
            startFrame = 0;
        }
        _p->playbackCtrl->seekToFrame(startFrame);
        pumpUntil([this, startFrame] {
            return _p->playbackCtrl && _p->playbackCtrl->currentFrame() == startFrame;
        });

        const QJsonObject beforeState = captureRuntimeAnnotationState();
        const QJsonObject beforeMetrics = overlayMetricsObject(beforeState);
        const int frameSteps = std::max(1, std::min(12, _p->playbackCtrl->totalFrames() - startFrame - 1));
        for (int i = 1; i <= frameSteps; ++i) {
            const int targetFrame = startFrame + i;
            _p->playbackCtrl->seekToFrame(targetFrame);
            pumpUntil([this, targetFrame] {
                return _p->playbackCtrl && _p->playbackCtrl->currentFrame() == targetFrame;
            }, 1500);
        }
        const QJsonObject afterState = captureRuntimeAnnotationState();
        const QJsonObject delta = diffOverlayMetrics(beforeMetrics, overlayMetricsObject(afterState));
        const double playbackEvents = jsonNumber(delta, QStringLiteral("playbackFrameChangedCount"));
        const double frameSyncs = jsonNumber(delta, QStringLiteral("frameSyncCount"));
        const double syncRatio = playbackEvents > 0.0 ? frameSyncs / playbackEvents : 0.0;

        frameProbe.insert(QStringLiteral("before"), beforeState);
        frameProbe.insert(QStringLiteral("after"), afterState);
        frameProbe.insert(QStringLiteral("delta"), delta);
        frameProbe.insert(QStringLiteral("requestedFrameSteps"), frameSteps);
        frameProbe.insert(QStringLiteral("frameSyncPerPlaybackEvent"), syncRatio);

        addAssertion(QStringLiteral("Frame sync observed"),
                     playbackEvents >= static_cast<double>(frameSteps) && frameSyncs >= playbackEvents,
                     QStringLiteral("playback frame changes reached overlay sync path"),
                     QJsonObject{
                         { QStringLiteral("requestedFrameSteps"), frameSteps },
                         { QStringLiteral("playbackFrameChangedCount"), playbackEvents },
                         { QStringLiteral("frameSyncCount"), frameSyncs },
                         { QStringLiteral("frameSyncPerPlaybackEvent"), syncRatio }
                     });
        addAssertion(QStringLiteral("Frame sync ratio bounded"),
                     playbackEvents > 0.0 && syncRatio <= 2.0,
                     QStringLiteral("overlay sync frequency stayed within expected bound"),
                     QJsonObject{
                         { QStringLiteral("playbackFrameChangedCount"), playbackEvents },
                         { QStringLiteral("frameSyncCount"), frameSyncs },
                         { QStringLiteral("frameSyncPerPlaybackEvent"), syncRatio }
                     });
    }

    QJsonObject transformProbe;
    if (_p->viewer) {
        const QJsonObject beforeState = captureRuntimeAnnotationState();
        const QJsonObject beforeMetrics = overlayMetricsObject(beforeState);
        _p->viewer->fitToWindow();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const double nextZoom = std::clamp(_p->viewer->zoom() * 1.1, 0.05, 32.0);
        _p->viewer->setZoom(nextZoom);
        const double transformCountBefore = jsonNumber(beforeMetrics, QStringLiteral("transformEventCount"));
        pumpUntil([this, transformCountBefore] {
            return jsonNumber(
                       overlayMetricsObject(captureRuntimeAnnotationState()),
                       QStringLiteral("transformEventCount")) > transformCountBefore;
        }, 1500);
        const QJsonObject afterState = captureRuntimeAnnotationState();
        const QJsonObject delta = diffOverlayMetrics(beforeMetrics, overlayMetricsObject(afterState));
        const double transformEvents = jsonNumber(delta, QStringLiteral("transformEventCount"));
        const double transformRefresh = jsonNumber(delta, QStringLiteral("transformRefreshCount"));
        const double transformRatio = transformEvents > 0.0 ? transformRefresh / transformEvents : 0.0;

        transformProbe.insert(QStringLiteral("before"), beforeState);
        transformProbe.insert(QStringLiteral("after"), afterState);
        transformProbe.insert(QStringLiteral("delta"), delta);
        transformProbe.insert(QStringLiteral("refreshPerTransformEvent"), transformRatio);

        addAssertion(QStringLiteral("Transform refresh observed"),
                     transformEvents > 0.0 && transformRefresh >= transformEvents,
                     QStringLiteral("view transform changes triggered overlay refresh"),
                     QJsonObject{
                         { QStringLiteral("transformEventCount"), transformEvents },
                         { QStringLiteral("transformRefreshCount"), transformRefresh },
                         { QStringLiteral("refreshPerTransformEvent"), transformRatio }
                     });
    }

    QJsonObject viewportProbe;
    if (_p->viewer) {
        const QJsonObject beforeState = captureRuntimeAnnotationState();
        const QJsonObject beforeMetrics = overlayMetricsObject(beforeState);
        const QSize originalSize = size();
        resize(originalSize.width() + 32, originalSize.height() + 24);
        const double resizeCountBefore = jsonNumber(beforeMetrics, QStringLiteral("viewportResizeEventCount"));
        pumpUntil([this, resizeCountBefore] {
            return jsonNumber(
                       overlayMetricsObject(captureRuntimeAnnotationState()),
                       QStringLiteral("viewportResizeEventCount")) > resizeCountBefore;
        }, 1500);
        resize(originalSize);
        pumpUntil([this, resizeCountBefore] {
            return jsonNumber(
                       overlayMetricsObject(captureRuntimeAnnotationState()),
                       QStringLiteral("viewportResizeEventCount")) > resizeCountBefore + 1.0;
        }, 1500);
        const QJsonObject afterState = captureRuntimeAnnotationState();
        const QJsonObject delta = diffOverlayMetrics(beforeMetrics, overlayMetricsObject(afterState));
        const double resizeEvents = jsonNumber(delta, QStringLiteral("viewportResizeEventCount"));
        const double resizeRefresh = jsonNumber(delta, QStringLiteral("viewportRefreshCount"));
        const double resizeRatio = resizeEvents > 0.0 ? resizeRefresh / resizeEvents : 0.0;

        viewportProbe.insert(QStringLiteral("before"), beforeState);
        viewportProbe.insert(QStringLiteral("after"), afterState);
        viewportProbe.insert(QStringLiteral("delta"), delta);
        viewportProbe.insert(QStringLiteral("refreshPerResizeEvent"), resizeRatio);

        addAssertion(QStringLiteral("Viewport resize refresh observed"),
                     resizeEvents > 0.0 && resizeRefresh >= resizeEvents,
                     QStringLiteral("viewport resize events triggered overlay refresh"),
                     QJsonObject{
                         { QStringLiteral("viewportResizeEventCount"), resizeEvents },
                         { QStringLiteral("viewportRefreshCount"), resizeRefresh },
                         { QStringLiteral("refreshPerResizeEvent"), resizeRatio }
                     });
    }

    const QJsonObject viewerReport = runPhase915ViewerRebuildChecks(mediaPath);
    const QJsonObject pluginReport = runPhase915PluginReloadChecks(mediaPath);
    const QJsonObject fallbackReport = runPhase915FallbackToggleChecks(mediaPath);
    const QJsonObject multiWindowReport = viewerReport.value(QStringLiteral("multi_window")).toObject();

    const auto compactScenario = [](const QJsonObject& scenario, const QString& metricsKey = QStringLiteral("metrics")) {
        return QJsonObject{
            { QStringLiteral("summary"), scenario.value(QStringLiteral("summary")).toObject() },
            { QStringLiteral("metrics"), scenario.value(metricsKey).toObject() }
        };
    };
    const auto scenarioWithOverlayDelta = [&compactScenario](const QJsonObject& scenario, const QString& afterKey) {
        QJsonObject out = compactScenario(scenario);
        const QJsonObject beforeMetrics = overlayMetricsObject(scenario.value(QStringLiteral("before")).toObject());
        const QJsonObject afterMetrics = overlayMetricsObject(scenario.value(afterKey).toObject());
        const bool counterResetDetected =
            afterMetrics.value(QStringLiteral("bindAttemptCount")).toDouble() <
                beforeMetrics.value(QStringLiteral("bindAttemptCount")).toDouble() ||
            afterMetrics.value(QStringLiteral("overlayCreateCount")).toDouble() <
                beforeMetrics.value(QStringLiteral("overlayCreateCount")).toDouble() ||
            afterMetrics.value(QStringLiteral("overlayStateSyncCount")).toDouble() <
                beforeMetrics.value(QStringLiteral("overlayStateSyncCount")).toDouble();
        out.insert(QStringLiteral("counterResetDetected"), counterResetDetected);
        out.insert(
            QStringLiteral("overlayMetricDelta"),
            counterResetDetected ? QJsonObject() : diffOverlayMetrics(beforeMetrics, afterMetrics));
        out.insert(QStringLiteral("beforeOverlayMetrics"), beforeMetrics);
        out.insert(QStringLiteral("afterOverlayMetrics"), afterMetrics);
        return out;
    };

    if (auto* app = qobject_cast<Application*>(qApp)) {
        app->setPluginFallbackEnabledForRuntime(false);
        app->reloadPluginForRuntime(QStringLiteral("annotation"));
        pumpUntil([this] {
            const QJsonObject state = captureRuntimeAnnotationState();
            return state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
                   state.value(QStringLiteral("annotation_service_present")).toBool() &&
                   state.value(QStringLiteral("overlay_count")).toInt() == 1;
        }, 4000);
    }
    const QJsonObject finalState = captureRuntimeAnnotationState();

    const int totalFailCount =
        makeSummary(assertions).value(QStringLiteral("fail")).toInt() +
        viewerReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() +
        multiWindowReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() +
        pluginReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() +
        fallbackReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt();

    report.insert(QStringLiteral("initial_state"), initialState);
    report.insert(QStringLiteral("initial_overlay_metrics"), initialOverlayMetrics);
    report.insert(QStringLiteral("final_state"), finalState);
    report.insert(QStringLiteral("final_overlay_metrics"), overlayMetricsObject(finalState));
    report.insert(
        QStringLiteral("frequency"),
        QJsonObject{
            { QStringLiteral("frameProbe"), frameProbe },
            { QStringLiteral("transformProbe"), transformProbe },
            { QStringLiteral("viewportProbe"), viewportProbe }
        });
    report.insert(
        QStringLiteral("scenarios"),
        QJsonObject{
            { QStringLiteral("viewerRebuild"), scenarioWithOverlayDelta(viewerReport, QStringLiteral("after")) },
            { QStringLiteral("multiWindowSwitch"), compactScenario(multiWindowReport) },
            { QStringLiteral("pluginReload"), scenarioWithOverlayDelta(pluginReport, QStringLiteral("after_reload")) },
            { QStringLiteral("fallbackToggle"), scenarioWithOverlayDelta(fallbackReport, QStringLiteral("after_enable")) }
        });
    report.insert(
        QStringLiteral("overlayProviderCosts"),
        QJsonObject{
            { QStringLiteral("subscriptionBaseline"), subscriptionBaseline },
            { QStringLiteral("current"), overlayMetricsObject(finalState) },
            { QStringLiteral("viewerRebuild"), viewerReport.value(QStringLiteral("metrics")).toObject() },
            { QStringLiteral("pluginReload"), pluginReport.value(QStringLiteral("metrics")).toObject() },
            { QStringLiteral("fallbackToggle"), fallbackReport.value(QStringLiteral("metrics")).toObject() }
        });
    report.insert(QStringLiteral("results"), assertions);
    report.insert(
        QStringLiteral("summary"),
        QJsonObject{
            { QStringLiteral("pass"), makeSummary(assertions).value(QStringLiteral("pass")).toInt() },
            { QStringLiteral("fail"), totalFailCount },
            { QStringLiteral("baselineFail"), makeSummary(assertions).value(QStringLiteral("fail")).toInt() },
            { QStringLiteral("viewerRebuildFail"),
              viewerReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() },
            { QStringLiteral("multiWindowFail"),
              multiWindowReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() },
            { QStringLiteral("pluginReloadFail"),
              pluginReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() },
            { QStringLiteral("fallbackToggleFail"),
              fallbackReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() }
        });
    return report;
}

QJsonObject MainWindow::runPhase14StressChecks(
    const QString& mediaPath,
    int viewerRebuildIterations,
    int windowSwitchIterations,
    int pluginReloadIterations,
    int fallbackToggleIterations)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](const QString& name, bool passed, const QString& message, const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        assertions.append(item);
    };

    auto sampleLoop = [](QJsonArray& samples, int iteration, double latencyMs, const QJsonObject& state) {
        QJsonObject item;
        item.insert(QStringLiteral("iteration"), iteration);
        item.insert(QStringLiteral("latencyMs"), latencyMs);
        item.insert(QStringLiteral("activeViewId"), state.value(QStringLiteral("active_view_id")).toString());
        item.insert(QStringLiteral("pluginBoundViewId"), state.value(QStringLiteral("plugin_bound_view_id")).toString());
        item.insert(QStringLiteral("overlayInstanceId"), state.value(QStringLiteral("overlay_instance_id")).toDouble());
        item.insert(QStringLiteral("overlayCount"), state.value(QStringLiteral("overlay_count")).toInt());
        item.insert(QStringLiteral("pluginSubscriptionCount"), state.value(QStringLiteral("plugin_subscription_count")).toInt());
        item.insert(QStringLiteral("workingSetMiB"), currentWorkingSetMiB());
        item.insert(QStringLiteral("overlayMetrics"), overlayMetricsObject(state));
        samples.append(item);
    };

    auto* app = qobject_cast<Application*>(qApp);
    openFile(mediaPath);
    const bool mediaOpened = pumpUntil([this] {
        return _p->playbackCtrl && _p->playbackCtrl->isValid();
    });
    addAssertion(QStringLiteral("Open media"), mediaOpened, QStringLiteral("media opened for phase14 stress"),
                 QJsonObject{ { QStringLiteral("media"), mediaPath } });

    if (_p->viewer) {
        _p->viewer->setFocus(Qt::OtherFocusReason);
    }
    const bool viewActivated = pumpUntil([this] {
        const QJsonObject state = captureRuntimeAnnotationState();
        return !state.value(QStringLiteral("active_view_id")).toString().isEmpty() &&
               state.value(QStringLiteral("active_view_id")).toString() ==
                   state.value(QStringLiteral("viewer_view_id")).toString();
    });
    addAssertion(QStringLiteral("Activate main view"), viewActivated, QStringLiteral("main view active for stress"),
                 captureRuntimeAnnotationState());

    const QJsonObject stressStartState = captureRuntimeAnnotationState();
    const int pluginSubscriptionBaseline = stressStartState.value(QStringLiteral("plugin_subscription_count")).toInt();

    QJsonObject viewerRebuildSection;
    {
        Phase14LoopStats stats;
        stats.requested = std::max(0, viewerRebuildIterations);
        stats.startWorkingSetMiB = currentWorkingSetMiB();
        stats.maxWorkingSetMiB = stats.startWorkingSetMiB;
        stats.sampleState(stressStartState);
        QJsonArray samples;

        for (int i = 0; i < stats.requested; ++i) {
            const QString oldViewId = captureRuntimeAnnotationState().value(QStringLiteral("active_view_id")).toString();
            QElapsedTimer timer;
            timer.start();
            const bool invoked = app && app->rebuildMainViewerForRuntime();
            if (_p->viewer) {
                _p->viewer->setFocus(Qt::OtherFocusReason);
            }
            const bool settled = pumpUntil([this, &oldViewId] {
                const QJsonObject state = captureRuntimeAnnotationState();
                const QString activeViewId = state.value(QStringLiteral("active_view_id")).toString();
                return !activeViewId.isEmpty() &&
                       activeViewId != oldViewId &&
                       state.value(QStringLiteral("plugin_bound_view_id")).toString() == activeViewId &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 1;
            }, 5000);
            const QJsonObject state = captureRuntimeAnnotationState();
            const bool passed = invoked && settled &&
                state.value(QStringLiteral("plugin_subscription_count")).toInt() == pluginSubscriptionBaseline;
            ++stats.completed;
            passed ? ++stats.pass : ++stats.fail;
            stats.recordLatency(static_cast<double>(timer.elapsed()));
            stats.sampleState(state);
            if (i < 5 || ((i + 1) % 100) == 0 || i + 1 == stats.requested) {
                sampleLoop(samples, i + 1, static_cast<double>(timer.elapsed()), state);
            }
        }

        stats.endWorkingSetMiB = currentWorkingSetMiB();
        viewerRebuildSection.insert(QStringLiteral("metrics"), stats.metrics());
        viewerRebuildSection.insert(QStringLiteral("samples"), samples);
        viewerRebuildSection.insert(QStringLiteral("finalState"), captureRuntimeAnnotationState());
        addAssertion(QStringLiteral("Viewer rebuild stress"), stats.fail == 0,
                     QStringLiteral("viewer rebuild loop completed without capability drift"),
                     viewerRebuildSection.value(QStringLiteral("metrics")).toObject());
    }

    QJsonObject windowSwitchSection;
    {
        Phase14LoopStats stats;
        stats.requested = std::max(0, windowSwitchIterations);
        stats.startWorkingSetMiB = currentWorkingSetMiB();
        stats.maxWorkingSetMiB = stats.startWorkingSetMiB;
        stats.sampleState(captureRuntimeAnnotationState());
        QJsonArray samples;

        SecondaryWindow* secondary = nullptr;
        if (app) {
            app->openNewWindow();
            pumpUntil([&secondary, this] {
                secondary = findSecondaryWindow(_p->viewer ? _p->viewer->viewId() : QString());
                return secondary != nullptr;
            }, 4000);
        }

        const QString mainViewId = _p->viewer ? _p->viewer->viewId() : QString();
        const QString secondaryViewId = secondary && secondary->viewerWidget() ? secondary->viewerWidget()->viewId() : QString();

        for (int i = 0; i < stats.requested && secondary && secondary->viewerWidget(); ++i) {
            const bool targetSecondary = (i % 2) == 0;
            const QString targetViewId = targetSecondary ? secondaryViewId : mainViewId;
            QElapsedTimer timer;
            timer.start();
            const bool activateOk = activateViewForRuntime(app, targetViewId);
            const bool settled = pumpUntil([this, &targetViewId] {
                const QJsonObject state = captureRuntimeAnnotationState();
                return state.value(QStringLiteral("active_view_id")).toString() == targetViewId &&
                       state.value(QStringLiteral("plugin_bound_view_id")).toString() == targetViewId &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 1;
            }, 3000);
            const QJsonObject state = captureRuntimeAnnotationState();
            const bool passed = activateOk &&
                settled &&
                state.value(QStringLiteral("plugin_subscription_count")).toInt() == pluginSubscriptionBaseline;
            ++stats.completed;
            passed ? ++stats.pass : ++stats.fail;
            stats.recordLatency(static_cast<double>(timer.elapsed()));
            stats.sampleState(state);
            if (i < 5 || ((i + 1) % 100) == 0 || i + 1 == stats.requested) {
                sampleLoop(samples, i + 1, static_cast<double>(timer.elapsed()), state);
            }
        }

        if (secondary) {
            secondary->close();
            pumpUntil([this, mainViewId] {
                return findSecondaryWindow(mainViewId) == nullptr;
            }, 3000);
        }

        stats.endWorkingSetMiB = currentWorkingSetMiB();
        windowSwitchSection.insert(QStringLiteral("metrics"), stats.metrics());
        windowSwitchSection.insert(QStringLiteral("samples"), samples);
        windowSwitchSection.insert(QStringLiteral("finalState"), captureRuntimeAnnotationState());
        addAssertion(QStringLiteral("Window switch stress"), stats.fail == 0,
                     QStringLiteral("multi-window switch loop completed without binding drift"),
                     windowSwitchSection.value(QStringLiteral("metrics")).toObject());
    }

    QJsonObject pluginReloadSection;
    {
        Phase14LoopStats stats;
        stats.requested = std::max(0, pluginReloadIterations);
        stats.startWorkingSetMiB = currentWorkingSetMiB();
        stats.maxWorkingSetMiB = stats.startWorkingSetMiB;
        stats.sampleState(captureRuntimeAnnotationState());
        QJsonArray samples;

        if (app) {
            app->setPluginFallbackEnabledForRuntime(false);
            if (!captureRuntimeAnnotationState().value(QStringLiteral("annotation_plugin_loaded")).toBool()) {
                app->reloadPluginForRuntime(QStringLiteral("annotation"));
                pumpUntil([this] {
                    const QJsonObject state = captureRuntimeAnnotationState();
                    return state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
                           state.value(QStringLiteral("annotation_service_present")).toBool() &&
                           state.value(QStringLiteral("overlay_count")).toInt() == 1;
                }, 4000);
            }
        }

        for (int i = 0; i < stats.requested; ++i) {
            const QString activeViewId = captureRuntimeAnnotationState().value(QStringLiteral("active_view_id")).toString();
            QElapsedTimer timer;
            timer.start();
            const bool unloadOk = app && app->unloadPluginForRuntime(QStringLiteral("annotation"));
            const bool unloadSettled = pumpUntil([this] {
                const QJsonObject state = captureRuntimeAnnotationState();
                return !state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
                       !state.value(QStringLiteral("annotation_service_present")).toBool() &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 0;
            }, 4000);
            const bool reloadOk = app && app->reloadPluginForRuntime(QStringLiteral("annotation"));
            const bool reloadSettled = pumpUntil([this, &activeViewId, pluginSubscriptionBaseline] {
                const QJsonObject state = captureRuntimeAnnotationState();
                return state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
                       state.value(QStringLiteral("annotation_service_present")).toBool() &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 1 &&
                       state.value(QStringLiteral("active_view_id")).toString() == activeViewId &&
                       state.value(QStringLiteral("plugin_bound_view_id")).toString() == activeViewId &&
                       state.value(QStringLiteral("plugin_subscription_count")).toInt() == pluginSubscriptionBaseline;
            }, 5000);
            const QJsonObject state = captureRuntimeAnnotationState();
            const bool passed = unloadOk && unloadSettled && reloadOk && reloadSettled;
            ++stats.completed;
            passed ? ++stats.pass : ++stats.fail;
            stats.recordLatency(static_cast<double>(timer.elapsed()));
            stats.sampleState(state);
            if (i < 5 || ((i + 1) % 20) == 0 || i + 1 == stats.requested) {
                sampleLoop(samples, i + 1, static_cast<double>(timer.elapsed()), state);
            }
        }

        stats.endWorkingSetMiB = currentWorkingSetMiB();
        pluginReloadSection.insert(QStringLiteral("metrics"), stats.metrics());
        pluginReloadSection.insert(QStringLiteral("samples"), samples);
        pluginReloadSection.insert(QStringLiteral("finalState"), captureRuntimeAnnotationState());
        addAssertion(QStringLiteral("Plugin reload stress"), stats.fail == 0,
                     QStringLiteral("plugin unload/reload loop completed without rebinding drift"),
                     pluginReloadSection.value(QStringLiteral("metrics")).toObject());
    }

    QJsonObject fallbackToggleSection;
    {
        Phase14LoopStats stats;
        stats.requested = std::max(0, fallbackToggleIterations);
        stats.startWorkingSetMiB = currentWorkingSetMiB();
        stats.maxWorkingSetMiB = stats.startWorkingSetMiB;
        stats.sampleState(captureRuntimeAnnotationState());
        QJsonArray samples;

        if (app) {
            app->setPluginFallbackEnabledForRuntime(true);
            app->unloadPluginForRuntime(QStringLiteral("annotation"));
            pumpUntil([this] {
                const QJsonObject state = captureRuntimeAnnotationState();
                return !state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
                       state.value(QStringLiteral("annotation_service_present")).toBool() &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 1;
            }, 4000);
        }

        for (int i = 0; i < stats.requested; ++i) {
            const QJsonObject beforeState = captureRuntimeAnnotationState();
            const QString activeViewId = beforeState.value(QStringLiteral("active_view_id")).toString();
            const double oldOverlayInstanceId = beforeState.value(QStringLiteral("overlay_instance_id")).toDouble();
            QElapsedTimer timer;
            timer.start();
            if (app) {
                app->setPluginFallbackEnabledForRuntime(false);
            }
            const bool disableSettled = pumpUntil([this] {
                const QJsonObject state = captureRuntimeAnnotationState();
                return !state.value(QStringLiteral("annotation_service_present")).toBool() &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 0;
            }, 4000);
            if (app) {
                app->setPluginFallbackEnabledForRuntime(true);
            }
            const bool enableSettled = pumpUntil([this, &activeViewId, oldOverlayInstanceId] {
                const QJsonObject state = captureRuntimeAnnotationState();
                return !state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
                       state.value(QStringLiteral("annotation_service_present")).toBool() &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 1 &&
                       state.value(QStringLiteral("active_view_id")).toString() == activeViewId &&
                       state.value(QStringLiteral("overlay_instance_id")).toDouble() != oldOverlayInstanceId;
            }, 5000);
            const QJsonObject state = captureRuntimeAnnotationState();
            const bool passed = disableSettled && enableSettled;
            ++stats.completed;
            passed ? ++stats.pass : ++stats.fail;
            stats.recordLatency(static_cast<double>(timer.elapsed()));
            stats.sampleState(state);
            if (i < 5 || ((i + 1) % 20) == 0 || i + 1 == stats.requested) {
                sampleLoop(samples, i + 1, static_cast<double>(timer.elapsed()), state);
            }
        }

        if (app) {
            app->setPluginFallbackEnabledForRuntime(false);
            app->reloadPluginForRuntime(QStringLiteral("annotation"));
            pumpUntil([this, pluginSubscriptionBaseline] {
                const QJsonObject state = captureRuntimeAnnotationState();
                return state.value(QStringLiteral("annotation_plugin_loaded")).toBool() &&
                       state.value(QStringLiteral("annotation_service_present")).toBool() &&
                       state.value(QStringLiteral("overlay_count")).toInt() == 1 &&
                       state.value(QStringLiteral("plugin_subscription_count")).toInt() == pluginSubscriptionBaseline;
            }, 5000);
        }

        stats.endWorkingSetMiB = currentWorkingSetMiB();
        fallbackToggleSection.insert(QStringLiteral("metrics"), stats.metrics());
        fallbackToggleSection.insert(QStringLiteral("samples"), samples);
        fallbackToggleSection.insert(QStringLiteral("finalState"), captureRuntimeAnnotationState());
        addAssertion(QStringLiteral("Fallback toggle stress"), stats.fail == 0,
                     QStringLiteral("fallback enable/disable loop completed without dual ownership"),
                     fallbackToggleSection.value(QStringLiteral("metrics")).toObject());
    }

    report.insert(QStringLiteral("initial_state"), stressStartState);
    report.insert(
        QStringLiteral("sections"),
        QJsonObject{
            { QStringLiteral("viewerRebuild"), viewerRebuildSection },
            { QStringLiteral("windowSwitch"), windowSwitchSection },
            { QStringLiteral("pluginReload"), pluginReloadSection },
            { QStringLiteral("fallbackToggle"), fallbackToggleSection }
        });
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("final_state"), captureRuntimeAnnotationState());
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    return report;
}

void MainWindow::prepareCaptureDemoState(const QString& preferredMedia)
{
    QStringList mediaPaths;
    if (!preferredMedia.isEmpty() && QFileInfo::exists(preferredMedia)) {
        mediaPaths << QFileInfo(preferredMedia).absoluteFilePath();
    }

    const QDir mediaDir(QCoreApplication::applicationDirPath() + QStringLiteral("/../../../tests/media"));
    const QStringList fixtureNames = {
        QStringLiteral("1080p_h264.mp4"),
        QStringLiteral("4k_60fps.mp4"),
        QStringLiteral("8k_60fps.mp4"),
        QStringLiteral("prores_1080p.mov")
    };
    for (const QString& name : fixtureNames) {
        const QString path = mediaDir.absoluteFilePath(name);
        if (QFileInfo::exists(path) && !mediaPaths.contains(path)) {
            mediaPaths << path;
        }
    }

    if (mediaPaths.isEmpty()) {
        return;
    }

    openFile(mediaPaths.first());

    if (_p->playlist && _p->playlist->model()) {
        auto* model = _p->playlist->model();
        for (const QString& path : mediaPaths) {
            bool exists = false;
            for (int row = 0; row < model->shotCount(); ++row) {
                if (model->shotAt(row).path == path) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                model->addPath(path);
            }
        }

        const QColor statuses[] = {
            QColor(0x34, 0x98, 0xDB),
            QColor(0x2E, 0xCC, 0x71),
            QColor(0xF1, 0xC4, 0x0F),
            QColor(0xE7, 0x4C, 0x3C)
        };
        for (int row = 0; row < model->shotCount(); ++row) {
            model->setStatusColor(row, statuses[row % 4]);
        }
        model->setCurrentIndex(0);
    }

    if (_p->playbackCtrl) {
        const int targetFrame = std::max(0, std::min(252, _p->playbackCtrl->totalFrames() - 1));
        _p->playbackCtrl->seekToFrame(targetFrame);
    }

    _annoMgr->clear();
    auto addNote = [this](int frame, const QColor& color, ReviewStatus status, const QString& author, const QString& comment) {
        AnnotationItem ann;
        ann.frame = frame;
        ann.type = AnnotationType::Point;
        ann.color = color;
        ann.status = status;
        ann.author = author;
        ann.comment = comment;
        ann.points.append(QPointF(_p->viewer ? _p->viewer->mediaW() * 0.48 : 960.0,
                                  _p->viewer ? _p->viewer->mediaH() * 0.42 : 540.0));
        _annoMgr->add(ann);
    };

    addNote(63, QColor(0xF1, 0xC4, 0x0F), ReviewStatus::Open,
            QStringLiteral("John"), QStringLiteral("Cloud highlight needs tuning."));
    addNote(252, QColor(0xE7, 0x4C, 0x3C), ReviewStatus::InProgress,
            QStringLiteral("Anna"), QStringLiteral("Rim light is too strong."));
    addNote(432, QColor(0x34, 0x98, 0xDB), ReviewStatus::Resolved,
            QStringLiteral("Mike"), QStringLiteral("Mountain detail approved."));

    const int currentFrame = _p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0;
    if (_p->reviewPanel) {
        _p->reviewPanel->refresh(_annoMgr->all(), currentFrame);
    }
}

} // namespace cgplay
